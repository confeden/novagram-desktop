/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_decoy.h"

#include "base/random.h"
#include "base/unixtime.h"
#include "settings.h"

#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QSaveFile>

namespace NovaGram::Decoy {
namespace {

constexpr auto kMagic = quint32(0x4E564443); // "NVDC"
constexpr auto kVersion = qint32(1);

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

[[nodiscard]] bool ReadMarker(Marker &to) {
	auto file = QFile(MarkerPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return false;
	}
	auto stream = QDataStream(&file);
	stream.setVersion(QDataStream::Qt_5_1);
	auto magic = quint32(0);
	auto version = qint32(0);
	stream >> magic >> version;
	if (stream.status() != QDataStream::Ok
		|| magic != kMagic
		|| version != kVersion) {
		return false;
	}
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

bool WriteMarker(const Marker &marker) {
	auto file = QSaveFile(MarkerPath());
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	auto stream = QDataStream(&file);
	stream.setVersion(QDataStream::Qt_5_1);
	stream
		<< kMagic
		<< kVersion
		<< marker.seed
		<< marker.anchor
		<< marker.firstName
		<< marker.lastName
		<< marker.phone;
	return (stream.status() == QDataStream::Ok) && file.commit();
}

[[nodiscard]] quint64 NewSeed() {
	const auto value = base::RandomValue<quint64>();
	return value ? value : quint64(1);
}

// The decoy is decided once and never changes inside a run. Reading the marker
// on every request would put a file check on the path of every network call.
struct Cache {
	bool loaded = false;
	bool active = false;
	Marker marker;
};

[[nodiscard]] Cache &Data() {
	static auto result = Cache();
	if (!result.loaded) {
		result.loaded = true;
		result.active = QFile::exists(MarkerPath());
		if (result.active && !ReadMarker(result.marker)) {
			// Damaged or written by an older build. Generating now and writing
			// back keeps the decoy identical on every later launch, which is
			// the whole point of storing the seed at all.
			result.marker.seed = NewSeed();
			result.marker.anchor = base::unixtime::now();
			WriteMarker(result.marker);
		}
	}
	return result;
}

} // namespace

bool Active() {
	return Data().active;
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
