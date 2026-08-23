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
constexpr auto kVersion = qint32(4);
constexpr auto kSaltSize = 32;
constexpr auto kIterations = 200000;
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
};

[[nodiscard]] QString BasePath() {
	return cWorkingDir() + u"tdata/"_q;
}

[[nodiscard]] QString StatePath() {
	return BasePath() + u"novagram_pin"_q;
}

[[nodiscard]] State ReadState() {
	auto result = State();
	auto file = QFile(StatePath());
	if (!file.open(QIODevice::ReadOnly)) {
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
		return State{ .pinModeEnabled = true };
	}
	auto enabled = qint32(0);
	stream >> enabled
		>> result.emergencySalt
		>> result.emergencyVerifier
		>> result.emergencyIterations
		>> result.failedAttempts
		>> result.lockoutStartedMs
		>> result.lockoutDeadlineMs;
	if (stream.status() != QDataStream::Ok) {
		return State{ .pinModeEnabled = true };
	}
	if (version >= 2) {
		auto keypad = qint32(0);
		stream >> keypad;
		if (stream.status() != QDataStream::Ok) {
			return State{ .pinModeEnabled = true };
		}
		result.shuffledKeypad = (keypad != 0);
	}
	if (version >= 3) {
		stream >> result.identitySalt >> result.identityEncrypted;
		if (stream.status() != QDataStream::Ok) {
			return State{ .pinModeEnabled = true };
		}
	}
	if (version >= 4) {
		stream >> result.identityPublicKey >> result.identityEphemeral;
		if (stream.status() != QDataStream::Ok) {
			return State{ .pinModeEnabled = true };
		}
	}
	result.pinModeEnabled = (enabled != 0);
	if (result.failedAttempts < 0) {
		result.failedAttempts = 0;
	}
	if (result.lockoutDeadlineMs < result.lockoutStartedMs) {
		result.lockoutDeadlineMs = result.lockoutStartedMs;
	}
	return result;
}

void WriteState(const State &state) {
	auto file = QSaveFile(StatePath());
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	auto stream = QDataStream(&file);
	stream.setVersion(QDataStream::Qt_5_15);
	stream << kMagic
		<< kVersion
		<< qint32(state.pinModeEnabled ? 1 : 0)
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
	if (stream.status() != QDataStream::Ok) {
		file.cancelWriting();
		return;
	}
	file.commit();
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

[[nodiscard]] bool KeptOnWipe(const QString &name) {
	// Both are scratch directories that the running application recreates on
	// demand and that never hold account data, so removing them would only
	// race with the writes that happen right after the wipe.
	return (name == u"temp"_q) || (name == u"tdummy"_q);
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
	const auto entries = dir.entryInfoList(
		QDir::Files
		| QDir::Dirs
		| QDir::NoDotAndDotDot
		| QDir::Hidden
		| QDir::System);
	for (const auto &entry : entries) {
		if (KeptOnWipe(entry.fileName())) {
			continue;
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
	state.identityPublicKey = Seal::PublicKey(pin, identitySalt);
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
			state.identityEphemeral));
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

QString HiddenInputHint() {
	return UseRussianTexts()
		? u"Введите PIN и нажмите «Продолжить». "
			"Введённые цифры и их количество намеренно скрыты."_q
		: u"Enter the PIN and press \"Continue\". "
			"The entered digits and their count are intentionally hidden."_q;
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
