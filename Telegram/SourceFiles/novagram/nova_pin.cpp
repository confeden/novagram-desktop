/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_pin.h"

#include "base/openssl_help.h"
#include "base/random.h"
#include "core/application.h"
#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "novagram/nova_decoy.h"
#include "novagram/nova_device_lock.h"
#include "novagram/nova_update.h"
#include "novagram/nova_seal.h"
#include "platform/platform_integration.h"
#include "settings.h"
#include "storage/details/storage_file_utilities.h"

#include <QtCore/QBuffer>
#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QLocale>
#include <QtCore/QSaveFile>

#include <algorithm>
#include <limits>

namespace NovaGram {
namespace {

constexpr auto kMagic = quint32(0x4E56504E);
// Version 5 moved the whole body inside the device seal. Up to version 4 the
// salt, the verifier and the sealed identity sat in the open next to each
// other, and a pin is worth about twenty bits: a stolen tdata folder gave up
// the account's real name and number for a few GPU-hours. Older versions are
// still read, once, and rewritten in the new format.
constexpr auto kVersion = qint32(5);
constexpr auto kSaltSize = 32;
// Android's floor is 600k (NovaPinKdf.ITERATIONS) and this is the platform
// where the verifier was the exposed one, so the weaker parameter was on the
// wrong side. Files written earlier keep their own count - it is stored beside
// the verifier - and are not re-derived, because that needs the pin.
constexpr auto kIterations = 600000;
constexpr auto kLegacyIterations = 200000;
constexpr auto kFailuresBeforeDelay = 3;
constexpr auto kDelayStep = crl::time(5 * 60 * 1000);
constexpr auto kMaxDelay = crl::time(24 * 60 * 60 * 1000);

struct State {
	bool pinModeEnabled = false;
	bool shuffledKeypad = true;
	QByteArray emergencySalt;
	QByteArray emergencyVerifier;
	qint32 emergencyIterations = 0;
	qint32 failedAttempts = 0;
	qint64 lockoutStartedMs = 0;
	qint64 lockoutDeadlineMs = 0;
	QByteArray identitySalt;
	QByteArray identityEncrypted;
	QByteArray identityPublicKey;
	QByteArray identityEphemeral;
	// The file was there and this machine could not open it. Never written
	// over: the rightful machine still opens it, and nothing here wipes
	// anything by itself (D13).
	bool unreadable = false;
	// The file was not there while the device store says a pin is armed on
	// this installation. Someone removed it.
	bool tampered = false;
};

// Detected once and remembered for the run. The state file is rebuilt the
// moment the deletion is found, so a second read would see an ordinary file
// again and the person in front of the unlock screen would never be told.
bool GlobalTampered/* = false*/;

[[nodiscard]] QString BasePath() {
	return cWorkingDir() + u"tdata/"_q;
}

[[nodiscard]] QString StatePath() {
	return BasePath() + u"novagram_pin"_q;
}

void WriteState(const State &state);
[[nodiscard]] crl::time DelayForAttempts(int failedAttempts);

// The derivation cost that produced this file's verifier, and with it the
// public half of the identity key pair: SetEmergencyPin() fixes both in the
// same call, so one stored number governs both. A raised constant must never
// be applied to an older file - the verifier would stop matching and the
// snapshot would stop opening, with nothing to say why.
[[nodiscard]] int StoredIterations(const State &state) {
	return (state.emergencyIterations > 0)
		? state.emergencyIterations
		: kLegacyIterations;
}

// Everything except the magic and the version number. Up to version 4 these
// fields sat directly in the file; from version 5 they are one blob that goes
// through the device seal.
[[nodiscard]] bool ReadBody(QDataStream &stream, qint32 version, State &to) {
	auto enabled = qint32(0);
	stream >> enabled
		>> to.emergencySalt
		>> to.emergencyVerifier
		>> to.emergencyIterations
		>> to.failedAttempts
		>> to.lockoutStartedMs
		>> to.lockoutDeadlineMs;
	if (stream.status() != QDataStream::Ok) {
		return false;
	}
	if (version >= 2) {
		auto keypad = qint32(0);
		stream >> keypad;
		if (stream.status() != QDataStream::Ok) {
			return false;
		}
		to.shuffledKeypad = (keypad != 0);
	}
	if (version >= 3) {
		stream >> to.identitySalt >> to.identityEncrypted;
		if (stream.status() != QDataStream::Ok) {
			return false;
		}
	}
	if (version >= 4) {
		stream >> to.identityPublicKey >> to.identityEphemeral;
		if (stream.status() != QDataStream::Ok) {
			return false;
		}
	}
	to.pinModeEnabled = (enabled != 0);
	if (to.failedAttempts < 0) {
		to.failedAttempts = 0;
	}
	if (to.lockoutDeadlineMs < to.lockoutStartedMs) {
		to.lockoutDeadlineMs = to.lockoutStartedMs;
	}
	return true;
}

[[nodiscard]] QByteArray WriteBody(const State &state) {
	auto result = QByteArray();
	auto buffer = QBuffer(&result);
	if (!buffer.open(QIODevice::WriteOnly)) {
		return QByteArray();
	}
	auto stream = QDataStream(&buffer);
	stream.setVersion(QDataStream::Qt_5_15);
	stream << qint32(state.pinModeEnabled ? 1 : 0)
		<< state.emergencySalt
		<< state.emergencyVerifier
		<< state.emergencyIterations
		<< state.failedAttempts
		<< state.lockoutStartedMs
		<< state.lockoutDeadlineMs
		<< qint32(state.shuffledKeypad ? 1 : 0)
		<< state.identitySalt
		<< state.identityEncrypted
		<< state.identityPublicKey
		<< state.identityEphemeral;
	return (stream.status() == QDataStream::Ok) ? result : QByteArray();
}

// The state file existed and was understood: bring the device-bound flag up to
// date, and carry a tampering already found in this run into the answer - the
// file was put back the moment it was found missing, so nothing on the disk
// records it any more. This is also the migration path for installations whose
// binding file predates the flag: one small write, once, then it agrees.
void FinishRead(State &state) {
	DeviceLock::SetPinArmed(state.pinModeEnabled);
	state.tampered = GlobalTampered;
}

[[nodiscard]] State ReadState() {
	auto result = State();
	auto file = QFile(StatePath());
	const auto exists = file.exists();
	if (!file.open(QIODevice::ReadOnly)) {
		if (exists) {
			// There, and this process cannot read it: a lock, or permissions.
			// A different thing from a deletion, and nothing may be written
			// over it - so it fails closed and stays untouched.
			return State{ .pinModeEnabled = true, .unreadable = true };
		}
		// Deleting this one file used to remove the emergency pin, the
		// persistent lockout counter and the pin mode in a single gesture, and
		// left no trace at all: a missing file simply read as "no pin here".
		// The device store remembers instead, so the two cases are told apart
		// explicitly - a first run has never armed anything, a deletion has.
		if (DeviceLock::PinArmed()) {
			if (!GlobalTampered) {
				GlobalTampered = true;
				LOG(("NovaGram pin: the state file is gone while the device "
					"store says a pin is armed here - tampering"));
			}
			// The attempts that were deleted count as spent. Not a full
			// lockout: an honest user whose file was lost to a disk fault must
			// not be shut out, and one delay step is already enough to make
			// deleting the file over and over cost something.
			const auto now = QDateTime::currentMSecsSinceEpoch();
			result.pinModeEnabled = true;
			result.failedAttempts = kFailuresBeforeDelay;
			result.lockoutStartedMs = now;
			result.lockoutDeadlineMs = now
				+ DelayForAttempts(result.failedAttempts);
			result.tampered = true;
			// Put back on the disk at once, so the delay runs down instead of
			// being recomputed - and slid forward - by every later read.
			static auto restored = false;
			if (!restored) {
				restored = true;
				WriteState(result);
			}
			return result;
		}
		return result;
	}
	auto stream = QDataStream(&file);
	stream.setVersion(QDataStream::Qt_5_15);
	auto magic = quint32(0);
	auto version = qint32(0);
	stream >> magic >> version;
	if (stream.status() != QDataStream::Ok
		|| magic != kMagic
		|| version < 1
		|| version > kVersion) {
		// A file that exists but cannot be understood must not silently
		// downgrade NovaGram to the plain upstream passcode behaviour, so
		// the pin mode stays on while the emergency verifier is dropped.
		// The local data itself is still protected by the passcode key.
		return State{ .pinModeEnabled = true, .unreadable = true };
	}
	if (version < 5) {
		if (!ReadBody(stream, version, result)) {
			return State{ .pinModeEnabled = true, .unreadable = true };
		}
		FinishRead(result);
		// Written in the open by an older build. Rewriting it sealed is what
		// takes the verifier and the sealed identity out of reach of an
		// offline attack on the pin, and nothing else here is guaranteed to
		// write at all - a profile whose pin is never touched again would keep
		// the old file for ever. Once per process, because a seal that is
		// being refused would otherwise be retried on every single read.
		static auto migrated = false;
		if (!migrated) {
			migrated = true;
			WriteState(result);
		}
		return result;
	}
	auto stored = QByteArray();
	stream >> stored;
	if (stream.status() != QDataStream::Ok) {
		return State{ .pinModeEnabled = true, .unreadable = true };
	}
	auto body = QByteArray();
	const auto opened = DeviceLock::OpenPayload(
		DeviceLock::SealedFile::Pin,
		stored,
		body);
	if (opened == DeviceLock::OpenResult::Foreign) {
		// Sealed to another machine. Fails closed - the pin mode stays on -
		// and unreadable stops anything from writing over a file the rightful
		// machine can still open.
		LOG(("NovaGram pin: the state file belongs to another machine"));
		return State{ .pinModeEnabled = true, .unreadable = true };
	}
	auto inner = QBuffer();
	inner.setData(body);
	if (!inner.open(QIODevice::ReadOnly)) {
		return State{ .pinModeEnabled = true, .unreadable = true };
	}
	auto bodyStream = QDataStream(&inner);
	bodyStream.setVersion(QDataStream::Qt_5_15);
	if (!ReadBody(bodyStream, version, result)) {
		return State{ .pinModeEnabled = true, .unreadable = true };
	}
	FinishRead(result);
	return result;
}

void WriteState(const State &state) {
	if (state.unreadable) {
		// Refusing rather than replacing: what is on the disk is somebody's
		// working pin file, and this machine only failed to open it.
		return;
	}
	const auto body = WriteBody(state);
	if (body.isEmpty()) {
		return;
	}
	const auto stored = DeviceLock::SealPayload(
		DeviceLock::SealedFile::Pin,
		body);
	if (stored.isEmpty()) {
		// A binding exists and sealing failed. Writing the body in the clear
		// instead would hand out the verifier and the sealed identity - the
		// very thing the seal was added for - so the previous file stays.
		LOG(("NovaGram pin: the state file was not written, "
			"the device seal was refused"));
		return;
	}
	auto file = QSaveFile(StatePath());
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	auto stream = QDataStream(&file);
	stream.setVersion(QDataStream::Qt_5_15);
	stream << kMagic << kVersion << stored;
	if (stream.status() != QDataStream::Ok) {
		file.cancelWriting();
		return;
	}
	if (file.commit()) {
		// The device-bound flag follows every write, so the emergency wipe -
		// which writes an empty state and then removes the file - leaves the
		// decoy with nothing to report as tampering.
		DeviceLock::SetPinArmed(state.pinModeEnabled);
	}
}

[[nodiscard]] QByteArray ComputeVerifier(
		const QString &pin,
		const QByteArray &salt,
		int iterations) {
	const auto utf8 = pin.toUtf8();
	const auto computed = openssl::Pbkdf2Sha512(
		bytes::make_span(utf8),
		bytes::make_span(salt),
		iterations);
	return QByteArray(
		reinterpret_cast<const char*>(computed.data()),
		int(computed.size()));
}

[[nodiscard]] MTP::AuthKeyPtr KeyFromMaterial(const QByteArray &material) {
	if (material.size() != Seal::kMaterialSize) {
		return nullptr;
	}
	auto data = MTP::AuthKey::Data();
	static_assert(sizeof(data) == Seal::kMaterialSize);
	memcpy(data.data(), material.constData(), sizeof(data));
	return std::make_shared<MTP::AuthKey>(data);
}

// Files written before the sealed scheme keyed the snapshot with the pin
// itself. They are still read, so that setting an emergency pin once does not
// have to be done again, but they are never written any more: a snapshot only
// the pin can rewrite cannot follow a changed name or number.
[[nodiscard]] MTP::AuthKeyPtr LegacyIdentityKey(
		const QString &pin,
		const QByteArray &salt) {
	return Storage::details::CreateLocalKey(pin.toUtf8(), salt);
}

[[nodiscard]] QByteArray EncryptIdentity(
		const MTP::AuthKeyPtr &key,
		const EmergencyIdentity &identity) {
	if (!key) {
		return QByteArray();
	}
	// The size matters: the default constructed descriptor has no device
	// behind its stream at all, so everything written into it is silently
	// dropped and the result decrypts to nothing.
	const auto size = uint32(3 * sizeof(quint32)
		+ identity.firstName.size() * sizeof(ushort)
		+ identity.lastName.size() * sizeof(ushort)
		+ identity.phone.size() * sizeof(ushort));
	auto data = Storage::details::EncryptedDescriptor(size);
	data.stream
		<< identity.firstName
		<< identity.lastName
		<< identity.phone;
	return Storage::details::PrepareEncrypted(data, key);
}

// Seals the snapshot to the public key the emergency pin defines. No secret is
// needed here, which is the whole point: the running application can keep the
// snapshot current without the emergency pin ever being typed again.
bool SealIdentity(
		State &state,
		const EmergencyIdentity &identity) {
	if (state.identityPublicKey.isEmpty()) {
		return false;
	}
	const auto envelope = Seal::SealTo(state.identityPublicKey);
	const auto encrypted = EncryptIdentity(
		KeyFromMaterial(envelope.material),
		identity);
	if (encrypted.isEmpty()) {
		return false;
	}
	state.identityEphemeral = envelope.ephemeral;
	state.identityEncrypted = encrypted;
	return true;
}

[[nodiscard]] EmergencyIdentity CurrentIdentity() {
	auto result = EmergencyIdentity();
	if (!Core::App().domain().started()) {
		return result;
	}
	const auto session = Core::App().domain().active().maybeSession();
	if (!session) {
		return result;
	}
	const auto self = session->user();
	result.firstName = self->firstName;
	result.lastName = self->lastName;
	result.phone = self->phone();
	return result;
}

[[nodiscard]] bool ConstantTimeEquals(
		const QByteArray &a,
		const QByteArray &b) {
	if (a.size() != b.size() || a.isEmpty()) {
		return false;
	}
	auto diff = 0;
	for (auto i = 0; i != a.size(); ++i) {
		diff |= (uchar(a[i]) ^ uchar(b[i]));
	}
	return (diff == 0);
}

// Left alone. Always empty by construction: tdata/tdummy exists so that the
// Windows file dialog can be opened against a folder with no files in it
// (platform/win/file_utilities_win.cpp), nothing is ever written into it, and
// its path is cached in memory for the run - so removing it would take away
// nothing and break the next file dialog.
[[nodiscard]] bool KeptOnWipe(const QString &name) {
	return (name == u"tdummy"_q);
}

// Emptied, but the directory itself stays. tdata/temp is not the scratch
// space the old rule took it for: notification avatars of the user's contacts
// are written there as plain PNG and are only removed on a timeout or on a
// graceful exit (window/notifications_utilities.cpp), and a calendar event
// built from a message is written there as .ics text. Both are exactly what
// the wipe exists to destroy.
//
// The directory has to survive because there are writers that do not create
// it - Tray::QuitJumpListIconPath(), which the wipe itself reaches through
// refreshCustomJumpList(), simply fails to write its icon if the folder is
// gone. There is no race to speak of: the wipe runs on the main thread on a
// locked cold start, no session exists to write an avatar, and a file some
// other process is holding open is refused here exactly as anywhere else in
// this sweep.
[[nodiscard]] bool EmptiedOnWipe(const QString &name) {
	return (name == u"temp"_q);
}

[[nodiscard]] QDir::Filters WipeFilters() {
	return QDir::Files
		| QDir::Dirs
		| QDir::NoDotAndDotDot
		| QDir::Hidden
		| QDir::System;
}

void WipeInside(const QDir &dir) {
	for (const auto &entry : dir.entryInfoList(WipeFilters())) {
		if (entry.isDir()) {
			QDir(entry.absoluteFilePath()).removeRecursively();
		} else {
			QFile::remove(entry.absoluteFilePath());
		}
	}
}

// The logs sit beside tdata, not inside it, so the sweep does not reach them -
// and log.txt is written by every build, not only with debug logging on. What
// accumulated in it before the wipe is the destroyed account's own activity.
//
// Truncated when it cannot be removed: this process holds log.txt open and
// Windows refuses to delete an open file, while a second handle may still
// shorten it to nothing. The still-open append handle keeps its old offset, so
// what the decoy writes afterwards lands past a run of zero bytes - ugly, and
// still the whole point, because the text that was there is gone.
void WipeLogs() {
	const auto drop = [](const QString &path) {
		auto file = QFile(path);
		if (!file.exists() || file.remove()) {
			return;
		} else if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			file.close();
		}
	};
	const auto working = QDir(cWorkingDir());
	for (const auto &entry : working.entryInfoList(
			QStringList() << u"log.txt"_q << u"log_start*.txt"_q,
			QDir::Files | QDir::Hidden | QDir::System)) {
		drop(entry.absoluteFilePath());
	}
	const auto debug = QDir(cWorkingDir() + u"DebugLogs"_q);
	for (const auto &entry : debug.entryInfoList(
			QDir::Files | QDir::Hidden | QDir::System)) {
		drop(entry.absoluteFilePath());
	}
}

[[nodiscard]] crl::time DelayForAttempts(int failedAttempts) {
	if (failedAttempts < kFailuresBeforeDelay) {
		return 0;
	}
	const auto steps = crl::time(failedAttempts - (kFailuresBeforeDelay - 1));
	return std::min(steps * kDelayStep, kMaxDelay);
}

[[nodiscard]] crl::time EvaluateLockout(State &state, bool &changed) {
	changed = false;
	if (!state.lockoutDeadlineMs) {
		return 0;
	}
	const auto now = QDateTime::currentMSecsSinceEpoch();
	if (!DelayForAttempts(state.failedAttempts)) {
		state.lockoutStartedMs = 0;
		state.lockoutDeadlineMs = 0;
		changed = true;
		return 0;
	} else if (now < state.lockoutStartedMs) {
		// The system clock moved backwards, which never shortens a delay:
		// the whole current delay is restarted from the observed moment.
		const auto delay = DelayForAttempts(state.failedAttempts);
		state.lockoutStartedMs = now;
		state.lockoutDeadlineMs = now + delay;
		changed = true;
		return delay;
	} else if (now >= state.lockoutDeadlineMs) {
		state.lockoutStartedMs = 0;
		state.lockoutDeadlineMs = 0;
		changed = true;
		return 0;
	}
	return crl::time(state.lockoutDeadlineMs - now);
}

} // namespace

void WipeLocalData() {
	const auto dir = QDir(BasePath());
	const auto entries = dir.entryInfoList(WipeFilters());
	for (const auto &entry : entries) {
		const auto name = entry.fileName();
		if (KeptOnWipe(name)) {
			continue;
		} else if (entry.isDir() && EmptiedOnWipe(name)) {
			WipeInside(QDir(entry.absoluteFilePath()));
		} else if (entry.isDir()) {
			QDir(entry.absoluteFilePath()).removeRecursively();
		} else {
			QFile::remove(entry.absoluteFilePath());
		}
	}
}

bool ValidPin(const QString &pin) {
	if (pin.size() < kMinPinLength || pin.size() > kMaxPinLength) {
		return false;
	}
	for (const auto &ch : pin) {
		if (ch < QChar('0') || ch > QChar('9')) {
			return false;
		}
	}
	return true;
}

bool PinModeEnabled() {
	return ReadState().pinModeEnabled;
}

bool PinStateTampered() {
	return ReadState().tampered;
}

void RewriteState() {
	if (!QFile::exists(StatePath())) {
		// Nothing to move between formats, and creating a state file here
		// would invent a pin configuration that the owner never asked for.
		return;
	}
	const auto state = ReadState();
	if (state.unreadable) {
		return;
	}
	WriteState(state);
}

bool HasEmergencyPin() {
	const auto state = ReadState();
	return !state.emergencyVerifier.isEmpty()
		&& !state.emergencySalt.isEmpty();
}

void SetPinModeEnabled(bool enabled) {
	auto state = ReadState();
	state.pinModeEnabled = enabled;
	if (!enabled) {
		state.emergencySalt = QByteArray();
		state.emergencyVerifier = QByteArray();
		state.emergencyIterations = 0;
		state.failedAttempts = 0;
		state.lockoutStartedMs = 0;
		state.lockoutDeadlineMs = 0;
	}
	WriteState(state);
}

bool ShuffledKeypadEnabled() {
	return ReadState().shuffledKeypad;
}

void SetShuffledKeypadEnabled(bool enabled) {
	auto state = ReadState();
	if (state.shuffledKeypad == enabled) {
		return;
	}
	state.shuffledKeypad = enabled;
	WriteState(state);
}

void SetEmergencyPin(const QString &pin) {
	auto state = ReadState();
	if (pin.isEmpty()) {
		state.emergencySalt = QByteArray();
		state.emergencyVerifier = QByteArray();
		state.emergencyIterations = 0;
		// The snapshot goes with it. Nothing could open it any more anyway,
		// and leaving the ciphertext of a real name and number behind for no
		// reason is not a thing this file should do.
		state.identitySalt = QByteArray();
		state.identityEncrypted = QByteArray();
		state.identityPublicKey = QByteArray();
		state.identityEphemeral = QByteArray();
		WriteState(state);
		return;
	}
	auto salt = QByteArray(kSaltSize, Qt::Uninitialized);
	base::RandomFill(salt.data(), salt.size());
	state.emergencySalt = salt;
	state.emergencyIterations = kIterations;
	state.emergencyVerifier = ComputeVerifier(pin, salt, kIterations);

	// The key pair is fixed here and only here, because its public half has to
	// stay the same for as long as the emergency pin does: everything sealed
	// with an older one would stop opening.
	auto identitySalt = QByteArray(kSaltSize, Qt::Uninitialized);
	base::RandomFill(identitySalt.data(), identitySalt.size());
	state.identitySalt = identitySalt;
	state.identityPublicKey = Seal::PublicKey(pin, identitySalt, kIterations);
	state.identityEphemeral = QByteArray();
	state.identityEncrypted = QByteArray();
	SealIdentity(state, CurrentIdentity());
	WriteState(state);
}

void RefreshEmergencyIdentity() {
	auto state = ReadState();
	if (state.emergencyVerifier.isEmpty()
		|| state.identityPublicKey.isEmpty()) {
		return;
	}
	const auto identity = CurrentIdentity();
	if (identity.phone.isEmpty() && identity.firstName.isEmpty()) {
		return;
	}
	// Rewritten whether or not anything changed. There is no way to tell from
	// here — the snapshot cannot be read back without the emergency pin — and
	// a fingerprint kept in the clear to compare against would hand out the
	// very name and number the sealing exists to hide.
	if (SealIdentity(state, identity)) {
		WriteState(state);
	}
}

EmergencyIdentity ReadEmergencyIdentity(const QString &pin) {
	const auto state = ReadState();
	if (pin.isEmpty()
		|| state.identitySalt.isEmpty()
		|| state.identityEncrypted.isEmpty()) {
		return EmergencyIdentity();
	}
	const auto key = state.identityPublicKey.isEmpty()
		? LegacyIdentityKey(pin, state.identitySalt)
		: KeyFromMaterial(Seal::Open(
			pin,
			state.identitySalt,
			state.identityEphemeral,
			StoredIterations(state)));
	auto data = Storage::details::EncryptedDescriptor();
	if (!key
		|| !Storage::details::DecryptLocal(
			data,
			state.identityEncrypted,
			key)) {
		return EmergencyIdentity();
	}
	auto result = EmergencyIdentity();
	data.stream >> result.firstName >> result.lastName >> result.phone;
	if (data.stream.status() != QDataStream::Ok) {
		return EmergencyIdentity();
	}
	return result;
}

bool CheckEmergencyPin(const QString &pin) {
	if (pin.isEmpty()) {
		return false;
	}
	const auto state = ReadState();
	if (state.emergencySalt.isEmpty()
		|| state.emergencyVerifier.isEmpty()
		|| state.emergencyIterations <= 0) {
		return false;
	}
	const auto verifier = ComputeVerifier(
		pin,
		state.emergencySalt,
		state.emergencyIterations);
	return ConstantTimeEquals(verifier, state.emergencyVerifier);
}

crl::time LockoutRemaining() {
	auto state = ReadState();
	if (!state.pinModeEnabled) {
		return 0;
	}
	auto changed = false;
	const auto remaining = EvaluateLockout(state, changed);
	if (changed) {
		WriteState(state);
	}
	return remaining;
}

void RecordFailedAttempt() {
	auto state = ReadState();
	if (!state.pinModeEnabled) {
		return;
	}
	if (state.failedAttempts < std::numeric_limits<qint32>::max()) {
		++state.failedAttempts;
	}
	const auto delay = DelayForAttempts(state.failedAttempts);
	const auto now = QDateTime::currentMSecsSinceEpoch();
	state.lockoutStartedMs = delay ? now : 0;
	state.lockoutDeadlineMs = delay ? (now + delay) : 0;
	WriteState(state);
}

void ResetFailedAttempts() {
	auto state = ReadState();
	if (!state.failedAttempts
		&& !state.lockoutStartedMs
		&& !state.lockoutDeadlineMs) {
		return;
	}
	state.failedAttempts = 0;
	state.lockoutStartedMs = 0;
	state.lockoutDeadlineMs = 0;
	WriteState(state);
}

QString FormatLockoutLeft(crl::time remaining) {
	const auto total = int((remaining + 999) / 1000);
	const auto hours = total / 3600;
	const auto minutes = (total % 3600) / 60;
	const auto seconds = total % 60;
	return QString("%1:%2:%3"
	).arg(hours
	).arg(minutes, 2, 10, QChar('0')
	).arg(seconds, 2, 10, QChar('0'));
}

QString LockoutMessage(crl::time remaining) {
	return (UseRussianTexts()
		? u"Повторите через "_q
		: u"Try again in "_q) + FormatLockoutLeft(remaining) + u"."_q;
}

void RunEmergencyWipe(const QString &pin) {
	// Read before the state file goes: on a cold start this is the only source
	// of the real name and number, because nothing else is decrypted yet.
	auto identity = ReadEmergencyIdentity(pin);
	if (identity.phone.isEmpty() && identity.firstName.isEmpty()) {
		identity = CurrentIdentity();
	}

	auto state = State();
	WriteState(state);
	QFile::remove(StatePath());
	// Explicitly, and not only through the write above, which can be refused:
	// the device store must not go on saying a pin is armed here. It would
	// make the next launch report the missing file as tampering, and it would
	// do it on the unlock screen of the decoy.
	DeviceLock::SetPinArmed(false);

	Decoy::Arm(identity.firstName, identity.lastName, identity.phone);

	// On a locked cold start no account has been created yet, so every local
	// file is closed and the ciphertext can be removed for real instead of
	// being only orphaned by the new key. When the lock appeared over a
	// running session the cache databases are open and Windows refuses to
	// remove them, so only the upstream logout cleanup applies there.
	if (!Core::App().domain().started()) {
		WipeLocalData();
		// The sweep took tdata/novagram_device with everything else. Without
		// this the decoy's first accounts write would seal to a secret no next
		// start can recover, and the wipe would end at the "another device"
		// screen instead of the disguise.
		DeviceLock::Forget();
	}
	// Re-armed last, because the sweep above removes the marker together with
	// everything else: the decoy must survive its own wipe.
	Decoy::Arm(identity.firstName, identity.lastName, identity.phone);

	// The update checker outlives the wipe - it is a process-wide object with
	// its own timer - and the chat list the decoy builds next reads its phase.
	// A release found before the emergency PIN would otherwise put a bar
	// saying "Update NovaGram" across the bottom of the disguise, and a
	// download in flight would go on running inside it.
	Update::Stop();
	QDir(cWorkingDir() + u"novagram_update/"_q).removeRecursively();

	// The jump list was built at startup under the fork name and is not touched
	// by arming; rebuild it now so a right-click on the taskbar button does not
	// still offer "Quit NovaGram" after the disguise is up.
	Core::App().platformIntegration().refreshCustomJumpList();

	// Last of the destruction, so that as little as possible of the disguise's
	// own start is appended behind it. Not part of WipeLocalData(): the device
	// lock's "start over" shares that sweep, and there the log is the only
	// thing that can explain why this machine was blocked.
	WipeLogs();

	Core::App().logoutWithChecks(nullptr);
}

bool UseRussianTexts() {
	const auto id = Lang::Id();
	if (id.startsWith(u"ru"_q)) {
		return true;
	} else if (!id.isEmpty()) {
		return false;
	}
	return (QLocale::system().language() == QLocale::Russian);
}

QString UnlockTitle() {
	return UseRussianTexts()
		? u"Разблокируйте NovaGram"_q
		: u"Unlock NovaGram"_q;
}

QString TamperedNotice() {
	return UseRussianTexts()
		? u"Файл настроек PIN удалён. Аварийный PIN и счётчик неудачных "
			"попыток пропали вместе с ним; сам PIN по-прежнему нужен."_q
		: u"The PIN settings file has been deleted. The emergency PIN and the "
			"failed attempt counter went with it; the PIN itself is still "
			"required."_q;
}

QString HiddenInputHint() {
	// The unlock screen is where the fork already speaks to the person in
	// front of it, and it is the one screen a deletion of tdata/novagram_pin
	// cannot take away, so the warning is put in front of the usual hint
	// instead of getting a widget of its own.
	const auto hint = UseRussianTexts()
		? u"Введите PIN и нажмите «Продолжить». "
			"Введённые цифры и их количество намеренно скрыты."_q
		: u"Enter the PIN and press \"Continue\". "
			"The entered digits and their count are intentionally hidden."_q;
	return PinStateTampered()
		? (TamperedNotice() + u"\n\n"_q + hint)
		: hint;
}

QString SubmitButton() {
	return UseRussianTexts() ? u"Продолжить"_q : u"Continue"_q;
}

QString WrongPin() {
	return UseRussianTexts() ? u"Неверный PIN."_q : u"Incorrect PIN."_q;
}

QString InvalidLength() {
	return UseRussianTexts()
		? u"PIN должен содержать 4–6 цифр."_q
		: u"The PIN must contain 4-6 digits."_q;
}

QString PinPlaceholder() {
	return UseRussianTexts() ? u"PIN"_q : u"PIN"_q;
}

} // namespace NovaGram
