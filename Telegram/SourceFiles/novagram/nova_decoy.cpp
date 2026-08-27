/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_decoy.h"

#include "base/random.h"
#include "base/unixtime.h"
#include "novagram/nova_device_lock.h"
#include "settings.h"

#include <QtCore/QBuffer>
#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QSaveFile>

namespace NovaGram::Decoy {
namespace {

constexpr auto kMagic = quint32(0x4E564443); // "NVDC"
// Version 1 held the identity of the wiped account as plain text: one file
// told a forensic examiner both that the wipe had been run and whose account
// was hidden. Version 2 keeps the same fields inside the device seal that
// tdata/key_data uses. Version 1 files are still read, once, and rewritten.
constexpr auto kVersion = qint32(2);
constexpr auto kLegacyVersion = qint32(1);

struct Marker {
	quint64 seed = 0;
	qint32 anchor = 0;
	QString firstName;
	QString lastName;
	QString phone;
};

[[nodiscard]] QString MarkerPath() {
	return cWorkingDir() + u"tdata/novagram_decoy"_q;
}

[[nodiscard]] QByteArray Payload(const Marker &marker) {
	auto result = QByteArray();
	auto buffer = QBuffer(&result);
	if (!buffer.open(QIODevice::WriteOnly)) {
		return QByteArray();
	}
	auto stream = QDataStream(&buffer);
	stream.setVersion(QDataStream::Qt_5_1);
	stream
		<< marker.seed
		<< marker.anchor
		<< marker.firstName
		<< marker.lastName
		<< marker.phone;
	return (stream.status() == QDataStream::Ok) ? result : QByteArray();
}

[[nodiscard]] bool ReadPayload(const QByteArray &payload, Marker &to) {
	auto buffer = QBuffer();
	buffer.setData(payload);
	if (!buffer.open(QIODevice::ReadOnly)) {
		return false;
	}
	auto stream = QDataStream(&buffer);
	stream.setVersion(QDataStream::Qt_5_1);
	auto result = Marker();
	stream
		>> result.seed
		>> result.anchor
		>> result.firstName
		>> result.lastName
		>> result.phone;
	if (stream.status() != QDataStream::Ok || !result.seed || !result.anchor) {
		return false;
	}
	to = result;
	return true;
}

// legacy is set when the marker was found in the cleartext format, so that the
// caller rewrites it sealed - that is the only moment the plain identity
// leaves the disk.
[[nodiscard]] bool ReadMarker(Marker &to, bool &legacy) {
	legacy = false;
	auto file = QFile(MarkerPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	auto stream = QDataStream(&file);
	stream.setVersion(QDataStream::Qt_5_1);
	auto magic = quint32(0);
	auto version = qint32(0);
	stream >> magic >> version;
	if (stream.status() != QDataStream::Ok || magic != kMagic) {
		return false;
	}
	if (version == kLegacyVersion) {
		auto result = Marker();
		stream
			>> result.seed
			>> result.anchor
			>> result.firstName
			>> result.lastName
			>> result.phone;
		if (stream.status() != QDataStream::Ok
			|| !result.seed
			|| !result.anchor) {
			return false;
		}
		legacy = true;
		to = result;
		return true;
	} else if (version != kVersion) {
		return false;
	}
	auto stored = QByteArray();
	stream >> stored;
	if (stream.status() != QDataStream::Ok) {
		return false;
	}
	auto payload = QByteArray();
	const auto opened = DeviceLock::OpenPayload(
		DeviceLock::SealedFile::Decoy,
		stored,
		payload);
	if (opened == DeviceLock::OpenResult::Foreign) {
		// The machine changed, or the DPAPI blob is gone. Nothing is destroyed
		// here and nothing is wiped (D13): the caller keeps the decoy armed -
		// the file exists, so a wipe did happen here - and lets the disguise
		// invent an identity of its own. Reading this as "no decoy" would be
		// the dangerous direction: the client would come up as an ordinary
		// signed-out NovaGram in front of whoever was just shown an account.
		return false;
	}
	return ReadPayload(payload, to);
}

// stripOnFailure says what to do when a binding exists and sealing is refused.
// The wipe needs the marker on the disk whatever happens - without it the
// disguise does not come up at all - so it writes one without the identity
// rather than none. A migration of an older file has the opposite priority:
// the file it would replace still holds a usable identity, so it waits for a
// launch on which sealing works instead of destroying it.
bool WriteMarker(const Marker &marker, bool stripOnFailure = true) {
	auto payload = Payload(marker);
	if (payload.isEmpty()) {
		return false;
	}
	auto stored = DeviceLock::SealPayload(
		DeviceLock::SealedFile::Decoy,
		payload);
	if (stored.isEmpty()) {
		if (!stripOnFailure) {
			return false;
		}
		// Never in the clear: the one thing this file must not carry is whose
		// account was hidden, so the identity is dropped and the disguise
		// invents its own.
		stored = Payload(Marker{
			.seed = marker.seed,
			.anchor = marker.anchor,
		});
		if (stored.isEmpty()) {
			return false;
		}
	}
	auto file = QSaveFile(MarkerPath());
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	auto stream = QDataStream(&file);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << kMagic << kVersion << stored;
	return (stream.status() == QDataStream::Ok) && file.commit();
}

[[nodiscard]] quint64 NewSeed() {
	const auto value = base::RandomValue<quint64>();
	return value ? value : quint64(1);
}

// The decoy is decided once and never changes inside a run. Reading the marker
// on every request would put a file check - and now a decryption - on the path
// of every network call, and Active() is asked once per request.
struct Cache {
	bool loaded = false;
	bool active = false;
	Marker marker;
};

[[nodiscard]] Cache &Data() {
	static auto result = Cache();
	if (!result.loaded) {
		result.loaded = true;
		// Existence alone decides, exactly as before the payload was sealed:
		// the marker is written by the wipe and by nothing else, so a file
		// that is there means a wipe happened here whether or not this machine
		// can read what is inside it.
		result.active = QFile::exists(MarkerPath());
		if (!result.active) {
			return result;
		}
		auto legacy = false;
		if (!ReadMarker(result.marker, legacy)) {
			// Damaged, written by an older build, or sealed to a machine this
			// is not. Generating now and writing back keeps the decoy
			// identical on every later launch, which is the whole point of
			// storing the seed at all.
			result.marker = Marker{
				.seed = NewSeed(),
				.anchor = base::unixtime::now(),
			};
			WriteMarker(result.marker);
		} else if (legacy) {
			// Read from the cleartext format. Rewriting it sealed is the only
			// moment the real name and number leave the disk.
			WriteMarker(result.marker, false);
		}
	}
	return result;
}

} // namespace

bool Active() {
	return Data().active;
}

void Rewrite() {
	auto &data = Data();
	if (!data.active || !data.marker.seed) {
		return;
	}
	// stripOnFailure stays off: this is a format change, not the wipe, and the
	// marker it would replace still holds a usable identity.
	WriteMarker(data.marker, false);
}

void Arm(
		const QString &firstName,
		const QString &lastName,
		const QString &phone) {
	auto &data = Data();
	if (!data.marker.seed) {
		data.marker.seed = NewSeed();
		data.marker.anchor = base::unixtime::now();
	}
	if (!firstName.isEmpty()) {
		data.marker.firstName = firstName;
	}
	if (!lastName.isNull()) {
		data.marker.lastName = lastName;
	}
	if (!phone.isEmpty()) {
		data.marker.phone = phone;
	}
	data.active = WriteMarker(data.marker) || data.active;
}

quint64 Seed() {
	return Data().marker.seed;
}

TimeId Anchor() {
	return Data().marker.anchor;
}

QString SelfFirstName() {
	return Data().marker.firstName;
}

QString SelfLastName() {
	return Data().marker.lastName;
}

QString SelfPhone() {
	return Data().marker.phone;
}

} // namespace NovaGram::Decoy
