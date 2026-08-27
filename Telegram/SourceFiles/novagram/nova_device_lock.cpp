/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_device_lock.h"

#include "base/openssl_help.h"
#include "base/random.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "logs.h"
#include "main/main_domain.h"
#include "novagram/nova_decoy.h"
#include "novagram/nova_pin.h"
#include "settings.h"
#include "storage/localstorage.h"
#include "storage/storage_domain.h"

#include <QtCore/QByteArrayList>
#include <QtCore/QDataStream>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSaveFile>
#include <QtCore/QSysInfo>

#include <array>
#include <cstring>
#include <optional>

// windows.h drags wincrypt.h in, and wincrypt.h turns X509_NAME and friends
// into macros. OpenSSL declares types by those names, so its headers have to
// be seen first: after that the macros only shadow names this file never uses.
#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#endif // Q_OS_WIN

namespace NovaGram::DeviceLock {
namespace {

constexpr auto kEnabledKey = "novagram_device_lock"_cs;

constexpr auto kFileMagic = quint32(0x4E56444C); // NVDL
// Version 2 appended the "a pin is armed here" flag. Version 1 files are still
// read - rejecting them would put every existing installation on the "another
// device" screen - and are lifted to 2 by the first write.
constexpr auto kFileVersion = qint32(2);
constexpr auto kFileVersionMin = qint32(1);

// Prefix of a wrapped local key. It sits in front of a value that used to be
// raw ciphertext, whose first bytes are an AES-IGE key fingerprint, so a
// collision is a 2^-32 event: it would cost one unnecessary "another device"
// screen, never a silent bypass.
constexpr auto kWrapMagic = "NVD1";
constexpr auto kWrapMagicSize = 4;
constexpr auto kSecretSize = 32;
constexpr auto kNonceSize = 12;
constexpr auto kTagSize = 16;

struct Binding {
	State state = State::Fresh;
	Backend backend = Backend::None;
	QByteArray secret;
	// What the binding file holds in front of the secret: the DPAPI blob, or
	// the public salt of the fingerprint fallback. Kept so that the file can
	// be rewritten - to carry the pin flag - without recreating the binding.
	QByteArray material;
	bool enabled = false;
	// True when nothing on this machine identifies it, so no binding can be
	// created here at all. Told apart from "creating the binding failed",
	// which is a fault and makes writes refuse.
	bool machineless = false;
};

std::optional<Binding> GlobalBinding;
bool GlobalBlocked/* = false*/;
bool GlobalDiskKnown/* = false*/;
bool GlobalDiskWrapped/* = false*/;

// Survives the Binding it was read from: SetEnabled() drops the binding file
// and lets the next write recreate it, and the flag has to ride through that.
std::optional<bool> GlobalPinArmed;

[[nodiscard]] QByteArray FingerprintInfo() {
	return QByteArray("NovaGram device binding v1");
}

[[nodiscard]] QString BasePath() {
	return cWorkingDir() + u"tdata/"_q;
}

[[nodiscard]] QString BindingPath() {
	return BasePath() + u"novagram_device"_q;
}

[[nodiscard]] QByteArray FromBytes(const bytes::vector &value) {
	return QByteArray(
		reinterpret_cast<const char*>(value.data()),
		int(value.size()));
}

#ifdef Q_OS_WIN

[[nodiscard]] QByteArray SystemVolumeSerial() {
	auto windows = std::array<wchar_t, MAX_PATH + 1>{ };
	const auto length = GetSystemWindowsDirectoryW(windows.data(), MAX_PATH);
	if (length < 3 || windows[1] != L':') {
		return QByteArray();
	}
	auto root = std::array<wchar_t, 4>{ windows[0], L':', L'\\', L'\0' };
	auto serial = DWORD(0);
	if (!GetVolumeInformationW(
			root.data(),
			nullptr,
			0,
			&serial,
			nullptr,
			nullptr,
			nullptr,
			0)) {
		return QByteArray();
	}
	return QByteArray::number(quint32(serial));
}

[[nodiscard]] QByteArray DpapiProtect(
		const QByteArray &data,
		const QByteArray &entropy) {
	auto in = DATA_BLOB{
		DWORD(data.size()),
		reinterpret_cast<BYTE*>(const_cast<char*>(data.constData())),
	};
	auto salt = DATA_BLOB{
		DWORD(entropy.size()),
		reinterpret_cast<BYTE*>(const_cast<char*>(entropy.constData())),
	};
	auto out = DATA_BLOB{ 0, nullptr };
	if (!CryptProtectData(
			&in,
			L"NovaGram device binding",
			&salt,
			nullptr,
			nullptr,
			CRYPTPROTECT_UI_FORBIDDEN,
			&out)) {
		return QByteArray();
	}
	auto result = QByteArray(
		reinterpret_cast<const char*>(out.pbData),
		int(out.cbData));
	LocalFree(out.pbData);
	return result;
}

[[nodiscard]] QByteArray DpapiUnprotect(
		const QByteArray &data,
		const QByteArray &entropy) {
	auto in = DATA_BLOB{
		DWORD(data.size()),
		reinterpret_cast<BYTE*>(const_cast<char*>(data.constData())),
	};
	auto salt = DATA_BLOB{
		DWORD(entropy.size()),
		reinterpret_cast<BYTE*>(const_cast<char*>(entropy.constData())),
	};
	auto out = DATA_BLOB{ 0, nullptr };
	if (!CryptUnprotectData(
			&in,
			nullptr,
			&salt,
			nullptr,
			nullptr,
			CRYPTPROTECT_UI_FORBIDDEN,
			&out)) {
		return QByteArray();
	}
	auto result = QByteArray(
		reinterpret_cast<const char*>(out.pbData),
		int(out.cbData));
	SecureZeroMemory(out.pbData, out.cbData);
	LocalFree(out.pbData);
	return result;
}

#endif // Q_OS_WIN

// Identifiers of the machine itself, never of the folder the data sits in:
// moving tdata to another disk of the same computer must keep working, and
// moving it to another computer must not.
//
// Deliberately excluded: the computer name, which people rename, and anything
// a driver update can renumber. What is left changes when the system is
// reinstalled - and then the local data is unreachable anyway.
[[nodiscard]] QByteArray MachineFingerprint() {
	auto parts = QByteArrayList();

	// On Windows this is HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid,
	// written once when the system is installed; on Linux /etc/machine-id;
	// on macOS the IOPlatformUUID.
	const auto unique = QSysInfo::machineUniqueId();
	if (!unique.isEmpty()) {
		parts.push_back(unique);
	}
#ifdef Q_OS_WIN
	const auto volume = SystemVolumeSerial();
	if (!volume.isEmpty()) {
		parts.push_back(volume);
	}
#endif // Q_OS_WIN

	if (parts.isEmpty()) {
		// Nothing identified this machine. Binding to a constant would be a
		// promise with nothing behind it, so the caller is told there is none.
		return QByteArray();
	}
	auto material = QByteArray();
	for (const auto &part : parts) {
		material += QByteArray::number(part.size()) + ':' + part + ';';
	}
	return FromBytes(openssl::Sha256(bytes::make_span(material)));
}

[[nodiscard]] QByteArray AesGcmSeal(
		const QByteArray &key,
		const QByteArray &nonce,
		const QByteArray &plaintext,
		const QByteArray &additional) {
	Expects(key.size() == kSecretSize);
	Expects(nonce.size() == kNonceSize);

	auto result = QByteArray(plaintext.size() + kTagSize, Qt::Uninitialized);
	const auto context = EVP_CIPHER_CTX_new();
	if (!context) {
		return QByteArray();
	}
	auto outLength = 0;
	auto fullLength = 0;
	auto ok = (EVP_EncryptInit_ex(
		context,
		EVP_aes_256_gcm(),
		nullptr,
		nullptr,
		nullptr) == 1)
		&& (EVP_CIPHER_CTX_ctrl(
			context,
			EVP_CTRL_GCM_SET_IVLEN,
			kNonceSize,
			nullptr) == 1)
		&& (EVP_EncryptInit_ex(
			context,
			nullptr,
			nullptr,
			reinterpret_cast<const uchar*>(key.constData()),
			reinterpret_cast<const uchar*>(nonce.constData())) == 1);
	if (ok && !additional.isEmpty()) {
		ok = (EVP_EncryptUpdate(
			context,
			nullptr,
			&outLength,
			reinterpret_cast<const uchar*>(additional.constData()),
			int(additional.size())) == 1);
	}
	if (ok && !plaintext.isEmpty()) {
		ok = (EVP_EncryptUpdate(
			context,
			reinterpret_cast<uchar*>(result.data()),
			&outLength,
			reinterpret_cast<const uchar*>(plaintext.constData()),
			int(plaintext.size())) == 1);
		fullLength = outLength;
	}
	ok = ok
		&& (fullLength == int(plaintext.size()))
		&& (EVP_EncryptFinal_ex(
			context,
			reinterpret_cast<uchar*>(result.data()) + fullLength,
			&outLength) == 1)
		&& (outLength == 0)
		&& (EVP_CIPHER_CTX_ctrl(
			context,
			EVP_CTRL_GCM_GET_TAG,
			kTagSize,
			result.data() + plaintext.size()) == 1);
	EVP_CIPHER_CTX_free(context);
	return ok ? result : QByteArray();
}

[[nodiscard]] QByteArray AesGcmOpen(
		const QByteArray &key,
		const QByteArray &nonce,
		const QByteArray &ciphertext,
		const QByteArray &additional) {
	Expects(key.size() == kSecretSize);
	Expects(nonce.size() == kNonceSize);

	if (ciphertext.size() <= kTagSize) {
		return QByteArray();
	}
	const auto plainLength = int(ciphertext.size()) - kTagSize;
	auto result = QByteArray(plainLength, Qt::Uninitialized);
	auto tag = ciphertext.mid(plainLength);
	const auto context = EVP_CIPHER_CTX_new();
	if (!context) {
		return QByteArray();
	}
	auto outLength = 0;
	auto fullLength = 0;
	auto ok = (EVP_DecryptInit_ex(
		context,
		EVP_aes_256_gcm(),
		nullptr,
		nullptr,
		nullptr) == 1)
		&& (EVP_CIPHER_CTX_ctrl(
			context,
			EVP_CTRL_GCM_SET_IVLEN,
			kNonceSize,
			nullptr) == 1)
		&& (EVP_DecryptInit_ex(
			context,
			nullptr,
			nullptr,
			reinterpret_cast<const uchar*>(key.constData()),
			reinterpret_cast<const uchar*>(nonce.constData())) == 1);
	if (ok && !additional.isEmpty()) {
		ok = (EVP_DecryptUpdate(
			context,
			nullptr,
			&outLength,
			reinterpret_cast<const uchar*>(additional.constData()),
			int(additional.size())) == 1);
	}
	if (ok) {
		ok = (EVP_DecryptUpdate(
			context,
			reinterpret_cast<uchar*>(result.data()),
			&outLength,
			reinterpret_cast<const uchar*>(ciphertext.constData()),
			plainLength) == 1);
		fullLength = outLength;
	}
	ok = ok
		&& (fullLength == plainLength)
		&& (EVP_CIPHER_CTX_ctrl(
			context,
			EVP_CTRL_GCM_SET_TAG,
			kTagSize,
			tag.data()) == 1)
		&& (EVP_DecryptFinal_ex(
			context,
			reinterpret_cast<uchar*>(result.data()) + fullLength,
			&outLength) == 1);
	EVP_CIPHER_CTX_free(context);
	return ok ? result : QByteArray();
}

// False means the secret was not persisted. The caller must then not seal
// anything with it: a value sealed to a secret that no next start can recover
// is a self-inflicted lockout.
[[nodiscard]] bool WriteBinding(
		Backend backend,
		const QByteArray &material,
		bool enabled) {
	const auto path = BasePath();
	if (!QDir().exists(path)) {
		QDir().mkpath(path);
	}
	auto file = QSaveFile(BindingPath());
	if (!file.open(QIODevice::WriteOnly)) {
		LOG(("NovaGram device lock: cannot open the binding file for writing"));
		return false;
	}
	auto stream = QDataStream(&file);
	stream.setVersion(QDataStream::Qt_5_15);
	stream << kFileMagic
		<< kFileVersion
		<< qint32(enabled ? 1 : 0)
		<< qint32(backend)
		<< material
		<< qint32(GlobalPinArmed.value_or(false) ? 1 : 0);
	if (stream.status() != QDataStream::Ok || !file.commit()) {
		LOG(("NovaGram device lock: failed to write the binding file"));
		return false;
	}
	return true;
}

// Creates the machine secret and stores whatever is needed to recover it.
// Returns an unbound state when nothing on this machine can hold a secret:
// binding must never be claimed without a mechanism behind it.
[[nodiscard]] Binding CreateBinding() {
	const auto fingerprint = MachineFingerprint();
	if (fingerprint.isEmpty()) {
		LOG(("NovaGram device lock: no stable identifier on this machine, "
			"the data stays portable"));
		// Recorded on disk, otherwise every start would try again and rewrite
		// the accounts file for nothing. Nothing depends on that write landing.
		[[maybe_unused]] const auto recorded = WriteBinding(
			Backend::None,
			QByteArray(),
			false);
		return Binding{
			.state = State::Off,
			.backend = Backend::None,
			.machineless = true,
		};
	}
	auto secret = QByteArray(kSecretSize, Qt::Uninitialized);
	base::RandomFill(secret.data(), secret.size());

#ifdef Q_OS_WIN
	// Protect and immediately open again: a mechanism that cannot read back
	// what it just wrote would lock the owner out on the next start.
	const auto guarded = DpapiProtect(secret, fingerprint);
	if (!guarded.isEmpty()
		&& DpapiUnprotect(guarded, fingerprint) == secret
		&& WriteBinding(Backend::Dpapi, guarded, true)) {
		LOG(("NovaGram device lock: bound through DPAPI"));
		return Binding{
			.state = State::Bound,
			.backend = Backend::Dpapi,
			.secret = secret,
			.material = guarded,
			.enabled = true,
		};
	}
	LOG(("NovaGram device lock: DPAPI is unavailable, "
		"falling back to the machine fingerprint"));
#endif // Q_OS_WIN

	// No operating system secret store took part: the file holds a public
	// salt and the secret is recomputed from the machine identifiers. Weaker
	// than DPAPI - a full disk image carries the identifiers along - but a
	// copied tdata folder on its own still opens nothing.
	auto salt = QByteArray(kSecretSize, Qt::Uninitialized);
	base::RandomFill(salt.data(), salt.size());
	const auto info = FingerprintInfo() + salt;
	secret = FromBytes(openssl::HmacSha256(
		bytes::make_span(fingerprint),
		bytes::make_span(info)));
	if (!WriteBinding(Backend::Fingerprint, salt, true)) {
		// A fault, not "this machine cannot be bound": machineless stays false
		// so that every write refuses instead of quietly going out unsealed.
		return Binding{ .state = State::Off, .backend = Backend::None };
	}
	LOG(("NovaGram device lock: bound through the machine fingerprint"));
	return Binding{
		.state = State::Bound,
		.backend = Backend::Fingerprint,
		.secret = secret,
		.material = salt,
		.enabled = true,
	};
}

[[nodiscard]] Binding ReadBinding() {
	auto file = QFile(BindingPath());
	if (!file.exists()) {
		return Binding{ .state = State::Fresh };
	} else if (!file.open(QIODevice::ReadOnly)) {
		LOG(("NovaGram device lock: the binding file cannot be opened"));
		return Binding{ .state = State::Foreign };
	}
	auto stream = QDataStream(&file);
	stream.setVersion(QDataStream::Qt_5_15);
	auto magic = quint32(0);
	auto version = qint32(0);
	auto enabled = qint32(0);
	auto backend = qint32(0);
	auto material = QByteArray();
	stream >> magic >> version >> enabled >> backend >> material;
	if (stream.status() != QDataStream::Ok
		|| magic != kFileMagic
		|| version < kFileVersionMin
		|| version > kFileVersion) {
		// A binding file that cannot be parsed counts as a foreign one. The
		// opposite - quietly rebinding - would hand a thief a way to skip the
		// check by corrupting one file.
		LOG(("NovaGram device lock: the binding file is not readable"));
		return Binding{ .state = State::Foreign };
	}
	if (version >= 2) {
		auto armed = qint32(0);
		stream >> armed;
		if (stream.status() != QDataStream::Ok) {
			LOG(("NovaGram device lock: the binding file is truncated"));
			return Binding{ .state = State::Foreign };
		}
		GlobalPinArmed = (armed != 0);
	} else {
		// Written before the flag existed. Unknown, not "no pin was ever set":
		// the first read of tdata/novagram_pin lifts it to the truth.
		GlobalPinArmed = std::nullopt;
	}
	const auto fingerprint = MachineFingerprint();
	if (!enabled) {
		return Binding{
			.state = State::Off,
			.backend = Backend::None,
			.material = material,
			.machineless = fingerprint.isEmpty(),
		};
	}
	if (fingerprint.isEmpty()) {
		return Binding{ .state = State::Foreign };
	}
	if (backend == qint32(Backend::Fingerprint)) {
		const auto info = FingerprintInfo() + material;
		return Binding{
			.state = State::Bound,
			.backend = Backend::Fingerprint,
			.secret = FromBytes(openssl::HmacSha256(
				bytes::make_span(fingerprint),
				bytes::make_span(info))),
			.material = material,
			.enabled = true,
		};
	}
#ifdef Q_OS_WIN
	if (backend == qint32(Backend::Dpapi)) {
		const auto secret = DpapiUnprotect(material, fingerprint);
		if (secret.size() != kSecretSize) {
			LOG(("NovaGram device lock: DPAPI refused the stored secret - "
				"another Windows account, or another machine"));
			return Binding{
				.state = State::Foreign,
				.backend = Backend::Dpapi,
			};
		}
		return Binding{
			.state = State::Bound,
			.backend = Backend::Dpapi,
			.secret = secret,
			.material = material,
			.enabled = true,
		};
	}
#endif // Q_OS_WIN
	LOG(("NovaGram device lock: the binding uses a mechanism "
		"this build cannot open"));
	return Binding{ .state = State::Foreign };
}

[[nodiscard]] const Binding &Ensure() {
	if (!GlobalBinding) {
		GlobalBinding = ReadBinding();
	}
	return *GlobalBinding;
}

// True when a value written right now would be wrapped. Not the same question
// as Enabled(): the owner may want binding while this machine cannot provide
// one.
[[nodiscard]] bool WouldWrap() {
	if (!Enabled()) {
		return false;
	}
	const auto state = Ensure().state;
	return (state == State::Bound) || (state == State::Fresh);
}

// Returns the machine secret, creating the binding when there is none yet.
// Empty means the value cannot be wrapped.
[[nodiscard]] QByteArray SecretForWriting() {
	if (Ensure().state == State::Fresh) {
		GlobalBinding = CreateBinding();
	}
	const auto &binding = *GlobalBinding;
	return (binding.state == State::Bound) ? binding.secret : QByteArray();
}

[[nodiscard]] bool IsWrapped(const QByteArray &stored) {
	return (stored.size() > kWrapMagicSize + kNonceSize + kTagSize)
		&& (memcmp(stored.constData(), kWrapMagic, kWrapMagicSize) == 0);
}

// The magic alone is what goes in front of key_data, so that value keeps the
// construction it already has on disk. Every other file adds its own name, so
// that a blob sealed for one of them does not open as another.
[[nodiscard]] QByteArray SealContext(SealedFile file) {
	const auto magic = QByteArray(kWrapMagic, kWrapMagicSize);
	switch (file) {
	case SealedFile::Pin: return magic + "novagram_pin";
	case SealedFile::Decoy: return magic + "novagram_decoy";
	}
	Unexpected("File in NovaGram::DeviceLock::SealContext.");
}

// Shared by Wrap() and SealPayload(). Empty additional data is never passed
// in, so an empty result always means "sealing failed, refuse the write".
[[nodiscard]] QByteArray SealWith(
		const QByteArray &secret,
		const QByteArray &additional,
		const QByteArray &plain) {
	auto nonce = QByteArray(kNonceSize, Qt::Uninitialized);
	base::RandomFill(nonce.data(), nonce.size());
	const auto sealed = AesGcmSeal(secret, nonce, plain, additional);
	return sealed.isEmpty()
		? QByteArray()
		: (QByteArray(kWrapMagic, kWrapMagicSize) + nonce + sealed);
}

void RewriteAccounts() {
	auto &domain = Core::App().domain();
	if (domain.started()) {
		domain.local().writeAccounts();
	}
}

// Every fork file that goes through SealPayload(). key_data is not here - it
// belongs to the storage domain and is written by RewriteAccounts().
void RewriteSealedFiles() {
	RewriteState();
	Decoy::Rewrite();
}

} // namespace

void Start() {
	const auto &binding = Ensure();
	const auto state = (binding.state == State::Off)
		? u"off"_q
		: (binding.state == State::Fresh)
		? u"not created yet"_q
		: (binding.state == State::Bound)
		? u"bound"_q
		: u"foreign"_q;
	LOG(("NovaGram device lock: %1, mechanism: %2"
		).arg(state
		).arg(BackendName(binding.backend)));
}

State Current() {
	return GlobalBlocked ? State::Foreign : Ensure().state;
}

Backend CurrentBackend() {
	return Ensure().backend;
}

bool Blocked() {
	return GlobalBlocked;
}

bool Enabled() {
	// The decoy marker is read by the very first branding call, which can come
	// before the application object exists. The preference defaults to on, so
	// answering from the default there gives the same answer settings would.
	return !Core::IsAppLaunched()
		|| Core::App().settings().readPref<bool>(kEnabledKey, true);
}

bool DataSealed() {
	return GlobalDiskKnown && GlobalDiskWrapped;
}

void SetEnabled(bool enabled) {
	if (GlobalBlocked || !Core::App().domain().started()) {
		return;
	} else if ((Enabled() == enabled) && (DataSealed() == enabled)) {
		return;
	}
	// Falling through with Enabled() == enabled means the preference and the
	// disk disagree. The settings toggle draws the disk, so what the owner just
	// asked for is a retry, not a no-op.
	Core::App().settings().writePref<bool>(kEnabledKey, enabled);
	Local::writeSettings();
	if (!enabled) {
		// Order matters (N15): every file that carries the seal is written
		// unbound first, and only then the secret goes. A key dropped ahead of
		// them would leave sealed files that nothing can ever open again - the
		// pin configuration and the decoy marker included, not only key_data.
		RewriteAccounts();
		RewriteSealedFiles();
		QFile::remove(BindingPath());
		GlobalBinding = Binding{ .state = State::Off };
	} else {
		// Reachable from the off state, where what is on disk is unbound and
		// dropping the stale marker orphans nothing, and from a state where
		// the preference says bound and the disk is not - and there too the
		// disk holds nothing sealed to the marker being removed.
		QFile::remove(BindingPath());
		GlobalBinding = std::nullopt;
		RewriteAccounts();
		// After the accounts write, because that is what creates the binding:
		// these have no way of their own to conjure a secret worth sealing to.
		RewriteSealedFiles();
	}
	// RewriteAccounts() went through Wrap(), which left GlobalDiskWrapped
	// describing what actually landed on the disk.
}

QByteArray Wrap(const QByteArray &keyEncrypted) {
	if (keyEncrypted.isEmpty()) {
		// There is nothing to seal and nothing worth writing either, so this
		// is a refusal like every other empty answer from here.
		return QByteArray();
	} else if (!Enabled()) {
		GlobalDiskKnown = true;
		GlobalDiskWrapped = false;
		return keyEncrypted;
	}
	const auto secret = SecretForWriting();
	if (secret.size() != kSecretSize) {
		if (Ensure().machineless) {
			// Nothing on this machine identifies it, so there is no binding to
			// be had here and none was ever claimed - CreateBinding() said so
			// in the log and recorded it. Refusing the write instead would
			// leave such a machine unable to keep an account at all.
			GlobalDiskKnown = true;
			GlobalDiskWrapped = false;
			return keyEncrypted;
		}
		// A mechanism exists and could not be used. Writing the key unbound
		// here is a silent unbind: the owner asked for binding, the file would
		// stop being bound, and nothing would say so. Android refuses the same
		// write (jni/tgnet/Config.cpp), and the previous file stays valid.
		LOG(("NovaGram device lock: no machine secret for the local key, "
			"refusing to write the accounts file unbound"));
		return QByteArray();
	}
	const auto wrapped = SealWith(
		secret,
		QByteArray(kWrapMagic, kWrapMagicSize),
		keyEncrypted);
	if (wrapped.isEmpty()) {
		LOG(("NovaGram device lock: sealing the local key failed, "
			"refusing to write the accounts file unbound"));
		return QByteArray();
	}
	GlobalDiskKnown = true;
	GlobalDiskWrapped = true;
	return wrapped;
}

QByteArray Unwrap(const QByteArray &stored) {
	GlobalDiskKnown = true;
	if (!IsWrapped(stored)) {
		// Written by a build without device binding, or with it switched off.
		GlobalDiskWrapped = false;
		return stored;
	}
	GlobalDiskWrapped = true;
	const auto &binding = Ensure();
	if (binding.state != State::Bound) {
		GlobalBlocked = true;
		return QByteArray();
	}
	const auto additional = QByteArray(kWrapMagic, kWrapMagicSize);
	const auto opened = AesGcmOpen(
		binding.secret,
		stored.mid(kWrapMagicSize, kNonceSize),
		stored.mid(kWrapMagicSize + kNonceSize),
		additional);
	if (opened.isEmpty()) {
		LOG(("NovaGram device lock: the local key does not belong "
			"to this machine"));
		GlobalBlocked = true;
		return QByteArray();
	}
	return opened;
}

QByteArray SealPayload(SealedFile file, const QByteArray &plain) {
	if (plain.isEmpty() || !Enabled()) {
		return plain;
	}
	const auto secret = SecretForWriting();
	if (secret.size() != kSecretSize) {
		// Same split as in Wrap(): no mechanism here is an honest unsealed
		// write, a mechanism that failed is a refusal.
		return Ensure().machineless ? plain : QByteArray();
	}
	return SealWith(secret, SealContext(file), plain);
}

OpenResult OpenPayload(
		SealedFile file,
		const QByteArray &stored,
		QByteArray &plain) {
	if (!IsWrapped(stored)) {
		plain = stored;
		return OpenResult::Plain;
	}
	const auto &binding = Ensure();
	if (binding.state != State::Bound) {
		return OpenResult::Foreign;
	}
	const auto opened = AesGcmOpen(
		binding.secret,
		stored.mid(kWrapMagicSize, kNonceSize),
		stored.mid(kWrapMagicSize + kNonceSize),
		SealContext(file));
	if (opened.isEmpty()) {
		// Deliberately not GlobalBlocked: a side file this machine cannot open
		// is handled by whoever owns it, and never by taking the whole
		// installation away from the owner.
		return OpenResult::Foreign;
	}
	plain = opened;
	return OpenResult::Opened;
}

bool PinArmed() {
	// The flag is read out of the binding file, so make sure it was read.
	[[maybe_unused]] const auto &binding = Ensure();
	return GlobalPinArmed.value_or(false);
}

void SetPinArmed(bool armed) {
	if (GlobalPinArmed && (*GlobalPinArmed == armed)) {
		return;
	} else if (GlobalBlocked || (Ensure().state == State::Foreign)) {
		// The binding file holds the only copy of the machine secret's
		// material. Rewriting one we could not read would destroy it.
		return;
	}
	GlobalPinArmed = armed;
	if ((Ensure().state == State::Fresh) && Enabled()) {
		// Create the binding rather than record "there is none" beside the
		// flag: a file saying enabled=0 would freeze this installation as
		// portable while the preference still promises the opposite.
		[[maybe_unused]] const auto created = SecretForWriting();
	}
	const auto &binding = Ensure();
	if (!WriteBinding(binding.backend, binding.material, binding.enabled)) {
		LOG(("NovaGram device lock: could not record the pin flag"));
	}
}

bool NeedsRewrite() {
	if (!GlobalDiskKnown || GlobalBlocked) {
		return false;
	}
	return (GlobalDiskWrapped != WouldWrap());
}

void ResetForNewDevice() {
	QFile::remove(BindingPath());
	WipeLocalData();
	Forget();
}

void Forget() {
	GlobalBinding = std::nullopt;
	GlobalBlocked = false;
	GlobalDiskKnown = false;
	GlobalDiskWrapped = false;
	// The pin flag goes with it. Whoever calls this has just turned the
	// installation back into a first start, and a flag left saying "a pin is
	// armed here" would make the next launch report the missing pin file as
	// tampering - inside the decoy, of all places.
	GlobalPinArmed = std::nullopt;
}

QString BlockedTitle() {
	return UseRussianTexts()
		? u"Данные принадлежат другому устройству"_q
		: u"This data belongs to another device"_q;
}

QString BlockedText() {
	return UseRussianTexts()
		? u"Локальные файлы NovaGram зашифрованы ключом, который хранится в "
			"этом компьютере и в этой учётной записи Windows. Здесь такого "
			"ключа нет, поэтому переписка не откроется и войти в аккаунт по "
			"этим файлам нельзя.\n\n"
			"Если это ваш компьютер и вы переустановили Windows или сменили "
			"учётную запись — начните заново и войдите в аккаунт так же, как "
			"с нового устройства. На серверах Telegram ничего не изменится."_q
		: u"The local NovaGram files are encrypted with a key kept inside "
			"this computer and this Windows account. That key is not here, "
			"so the history will not open and these files cannot sign anyone "
			"in.\n\n"
			"If this is your own computer and you reinstalled Windows or "
			"changed the account, start over and sign in the way a new device "
			"does. Nothing on the Telegram servers is affected."_q;
}

QString BlockedResetButton() {
	return UseRussianTexts() ? u"Начать заново"_q : u"Start over"_q;
}

QString BackendName(Backend backend) {
	switch (backend) {
	case Backend::Dpapi:
		return UseRussianTexts()
			? u"Windows DPAPI + отпечаток компьютера"_q
			: u"Windows DPAPI + machine fingerprint"_q;
	case Backend::Fingerprint:
		return UseRussianTexts()
			? u"отпечаток компьютера"_q
			: u"machine fingerprint"_q;
	case Backend::None:
		break;
	}
	return UseRussianTexts() ? u"нет"_q : u"none"_q;
}

} // namespace NovaGram::DeviceLock
