/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_authenticode.h"

#include "logs.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>

#ifdef Q_OS_WIN

// No OpenSSL in this file, on purpose. wincrypt.h turns X509_NAME and a dozen
// neighbouring names into macros, so anything that needs both has to include
// OpenSSL first and remember why for ever after (see nova_device_lock.cpp).
// Everything used below - SHA-256 included, from Qt - is available without it,
// so the question never comes up.
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>

#endif // Q_OS_WIN

namespace NovaGram::Authenticode {
namespace {

#ifdef Q_OS_WIN

// SHA-256 over the DER bytes of the certificate that signs NovaGram releases:
//
//   subject and issuer  CN=Brent NovaGram
//   serial              7EC30BDDB0D228A844BF7DBC7C3EA659
//   sha256RSA, valid until 2036-07-15
//
// That certificate is self-signed, which is the whole reason this constant
// exists. Nothing on the machine vouches for it, so "does the chain check
// out" is the wrong question: it is answered yes by every authority the
// machine carries, including one an employer's image or an installer the user
// was talked into put there. The signer is compared to this instead.
//
// If the signing certificate is ever rotated this line has to change, and a
// build carrying the new value has to be in the field before the first
// installer signed by the new certificate is published - otherwise every
// client already installed refuses the update it is being offered.
constexpr auto kSigningCertificateSha256 =
	"C94EFC2682E519BF938EFA91E4CC9A238E7A0714C29054055BFD9A4C105DACB5";

[[nodiscard]] bool EqualConstantTime(
		const QByteArray &a,
		const QByteArray &b) {
	if (a.isEmpty() || a.size() != b.size()) {
		return false;
	}
	auto diff = 0;
	for (auto i = 0, count = int(a.size()); i != count; ++i) {
		// No early exit: how long the comparison takes must not depend on how
		// much of the digest was right.
		diff |= (uchar(a[i]) ^ uchar(b[i]));
	}
	return (diff == 0);
}

// The first half: is the file Authenticode signed at all, and do its bytes
// still match what was signed? Nothing here decides whether the signer is
// ours - that is the second half, and it is the half that matters.
//
// An untrusted root is the expected answer and not a failure. The release
// certificate is self-signed, so a machine on which the chain did check out
// would be a machine where somebody had installed that certificate as a root -
// which is not something this build is willing to depend on in either
// direction.
[[nodiscard]] Verdict VerifyTrust(const std::wstring &path) {
	auto file = WINTRUST_FILE_INFO();
	file.cbStruct = sizeof(file);
	file.pcwszFilePath = path.c_str();

	// Written the way the SDK expects it: the macro is a braced list, so this
	// is aggregate initialisation and not a cast.
	GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
	auto data = WINTRUST_DATA();
	data.cbStruct = sizeof(data);
	data.dwUIChoice = WTD_UI_NONE;
	data.fdwRevocationChecks = WTD_REVOKE_NONE;
	data.dwUnionChoice = WTD_CHOICE_FILE;
	data.dwStateAction = WTD_STATEACTION_VERIFY;
	// Nothing about this check may reach the network. A fork that promises one
	// request outside Telegram does not get to make a second one here, and a
	// revocation list that cannot be fetched must not turn into a stall in
	// front of a button the user has just pressed.
	data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
	data.pFile = &file;

	const auto window = static_cast<HWND>(INVALID_HANDLE_VALUE);
	const auto status = ::WinVerifyTrust(window, &action, &data);

	// Always, whatever the verdict: the provider keeps everything it parsed on
	// the WINTRUST_DATA and hands none of it back without this.
	data.dwStateAction = WTD_STATEACTION_CLOSE;
	::WinVerifyTrust(window, &action, &data);

	switch (status) {
	case ERROR_SUCCESS:
	case LONG(CERT_E_UNTRUSTEDROOT):
	case LONG(CERT_E_UNTRUSTEDTESTROOT):
	case LONG(CERT_E_CHAINING):
	case LONG(TRUST_E_SUBJECT_NOT_TRUSTED):
		return Verdict::Ok;
	case LONG(TRUST_E_NOSIGNATURE):
	case LONG(TRUST_E_SUBJECT_FORM_UNKNOWN):
	case LONG(TRUST_E_PROVIDER_UNKNOWN):
		return Verdict::Unsigned;
	case LONG(TRUST_E_BAD_DIGEST):
		return Verdict::Tampered;
	}
	// Everything else - an expired certificate, a broken provider, a signature
	// Windows dislikes for a reason of its own - is refused rather than
	// guessed about, and the number is written down so it can be looked up.
	LOG(("NovaGram update: WinVerifyTrust answered 0x%1."
		).arg(uint(status), 8, 16, QChar('0')));
	return Verdict::Unknown;
}

// The second half: the certificate that actually made the signature, as bytes.
// CryptQueryObject opens the embedded PKCS#7, the message names its signer and
// the store holds the certificate that name belongs to.
[[nodiscard]] QByteArray SignerCertificateDigest(const std::wstring &path) {
	auto encoding = DWORD(0);
	auto contentType = DWORD(0);
	auto formatType = DWORD(0);
	auto store = HCERTSTORE(nullptr);
	auto message = HCRYPTMSG(nullptr);
	if (!::CryptQueryObject(
			CERT_QUERY_OBJECT_FILE,
			path.c_str(),
			CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
			CERT_QUERY_FORMAT_FLAG_BINARY,
			0,
			&encoding,
			&contentType,
			&formatType,
			&store,
			&message,
			nullptr)) {
		LOG(("NovaGram update: could not open the installer's signature."));
		return QByteArray();
	}
	const auto guard = gsl::finally([&] {
		if (message) {
			::CryptMsgClose(message);
		}
		if (store) {
			::CertCloseStore(store, 0);
		}
	});
	auto size = DWORD(0);
	if (!::CryptMsgGetParam(
			message,
			CMSG_SIGNER_CERT_INFO_PARAM,
			0,
			nullptr,
			&size) || !size) {
		LOG(("NovaGram update: the signature names no signer."));
		return QByteArray();
	}
	auto buffer = std::vector<BYTE>(size);
	const auto info = reinterpret_cast<CERT_INFO*>(buffer.data());
	if (!::CryptMsgGetParam(
			message,
			CMSG_SIGNER_CERT_INFO_PARAM,
			0,
			info,
			&size)) {
		LOG(("NovaGram update: could not read the signer."));
		return QByteArray();
	}
	const auto context = ::CertFindCertificateInStore(
		store,
		encoding,
		0,
		CERT_FIND_SUBJECT_CERT,
		info,
		nullptr);
	if (!context) {
		LOG(("NovaGram update: the signer's certificate is not in the file."));
		return QByteArray();
	}
	const auto result = QCryptographicHash::hash(
		QByteArray(
			reinterpret_cast<const char*>(context->pbCertEncoded),
			int(context->cbCertEncoded)),
		QCryptographicHash::Sha256);
	::CertFreeCertificateContext(context);
	return result;
}

#endif // Q_OS_WIN

} // namespace

Verdict Verify(const QString &path) {
	if (path.isEmpty() || !QFile::exists(path)) {
		return Verdict::Unknown;
	}
#ifdef Q_OS_WIN
	const auto native = QDir::toNativeSeparators(path).toStdWString();
	const auto trusted = VerifyTrust(native);
	if (trusted != Verdict::Ok) {
		return trusted;
	}
	const auto actual = SignerCertificateDigest(native);
	if (actual.isEmpty()) {
		return Verdict::Unknown;
	} else if (!EqualConstantTime(
			actual,
			QByteArray::fromHex(kSigningCertificateSha256))) {
		LOG(("NovaGram update: the installer is signed by %1, not by us."
			).arg(QString::fromLatin1(actual.toHex().toUpper())));
		return Verdict::ForeignSigner;
	}
	return Verdict::Ok;
#else // Q_OS_WIN
	// The installer is a Windows one and there is nothing here to check it
	// with. Refused rather than waved through: on this path a verifier with no
	// opinion has to count as a no.
	return Verdict::Unknown;
#endif // Q_OS_WIN
}

QString VerdictName(Verdict verdict) {
	switch (verdict) {
	case Verdict::Ok: return u"signed by us"_q;
	case Verdict::Unsigned: return u"not signed at all"_q;
	case Verdict::Tampered: return u"signed, but the bytes changed"_q;
	case Verdict::ForeignSigner: return u"signed by somebody else"_q;
	case Verdict::Unknown: return u"could not be checked"_q;
	}
	return u"unknown"_q;
}

} // namespace NovaGram::Authenticode
