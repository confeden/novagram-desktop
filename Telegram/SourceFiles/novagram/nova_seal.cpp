/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_seal.h"

#include "base/openssl_help.h"
#include "base/random.h"

#include <openssl/evp.h>

#include <memory>

namespace NovaGram::Seal {
namespace {

constexpr auto kScalarIterations = 200000;

struct KeyDeleter {
	void operator()(EVP_PKEY *value) const {
		EVP_PKEY_free(value);
	}
};

struct ContextDeleter {
	void operator()(EVP_PKEY_CTX *value) const {
		EVP_PKEY_CTX_free(value);
	}
};

using Key = std::unique_ptr<EVP_PKEY, KeyDeleter>;
using KeyContext = std::unique_ptr<EVP_PKEY_CTX, ContextDeleter>;

// Deliberately as slow as the emergency verifier stored beside it. A pin is
// worth about twenty bits, so the derivation cost is the whole defence.
[[nodiscard]] QByteArray Scalar(
		const QString &pin,
		const QByteArray &salt) {
	if (pin.isEmpty() || salt.isEmpty()) {
		return QByteArray();
	}
	const auto passcode = pin.toUtf8();
	auto result = QByteArray(kKeySize, Qt::Uninitialized);
	const auto ok = PKCS5_PBKDF2_HMAC(
		passcode.constData(),
		passcode.size(),
		reinterpret_cast<const uchar*>(salt.constData()),
		salt.size(),
		kScalarIterations,
		EVP_sha512(),
		result.size(),
		reinterpret_cast<uchar*>(result.data()));
	return (ok == 1) ? result : QByteArray();
}

[[nodiscard]] Key PrivateKey(const QByteArray &scalar) {
	if (scalar.size() != kKeySize) {
		return nullptr;
	}
	return Key(EVP_PKEY_new_raw_private_key(
		EVP_PKEY_X25519,
		nullptr,
		reinterpret_cast<const uchar*>(scalar.constData()),
		scalar.size()));
}

[[nodiscard]] QByteArray PublicOf(const Key &key) {
	if (!key) {
		return QByteArray();
	}
	auto size = size_t(kKeySize);
	auto result = QByteArray(kKeySize, Qt::Uninitialized);
	const auto ok = EVP_PKEY_get_raw_public_key(
		key.get(),
		reinterpret_cast<uchar*>(result.data()),
		&size);
	return (ok == 1 && size == kKeySize) ? result : QByteArray();
}

[[nodiscard]] QByteArray Shared(
		const Key &mine,
		const QByteArray &theirs) {
	if (!mine || theirs.size() != kKeySize) {
		return QByteArray();
	}
	const auto peer = Key(EVP_PKEY_new_raw_public_key(
		EVP_PKEY_X25519,
		nullptr,
		reinterpret_cast<const uchar*>(theirs.constData()),
		theirs.size()));
	if (!peer) {
		return QByteArray();
	}
	const auto context = KeyContext(EVP_PKEY_CTX_new(mine.get(), nullptr));
	if (!context || EVP_PKEY_derive_init(context.get()) != 1) {
		return QByteArray();
	} else if (EVP_PKEY_derive_set_peer(context.get(), peer.get()) != 1) {
		return QByteArray();
	}
	auto size = size_t(kKeySize);
	auto result = QByteArray(kKeySize, Qt::Uninitialized);
	const auto ok = EVP_PKEY_derive(
		context.get(),
		reinterpret_cast<uchar*>(result.data()),
		&size);
	return (ok == 1 && size == kKeySize) ? result : QByteArray();
}

// No stretching here on purpose: the shared secret already carries the full
// strength of the curve, and the pin was stretched once when the private key
// was derived from it. Both public keys go into the hash so that a sealed
// value cannot be replayed against a different recipient.
[[nodiscard]] QByteArray Material(
		const QByteArray &shared,
		const QByteArray &ephemeral,
		const QByteArray &recipient) {
	if (shared.size() != kKeySize) {
		return QByteArray();
	}
	auto result = QByteArray();
	result.reserve(kMaterialSize);
	for (auto counter = char(0); result.size() < kMaterialSize; ++counter) {
		const auto block = openssl::Sha512(
			bytes::make_span(shared),
			bytes::make_span(ephemeral),
			bytes::make_span(recipient),
			bytes::make_span(&counter, 1));
		result.append(
			reinterpret_cast<const char*>(block.data()),
			int(block.size()));
	}
	result.resize(kMaterialSize);
	return result;
}

} // namespace

QByteArray PublicKey(const QString &pin, const QByteArray &salt) {
	return PublicOf(PrivateKey(Scalar(pin, salt)));
}

Envelope SealTo(const QByteArray &publicKey) {
	if (publicKey.size() != kKeySize) {
		return Envelope();
	}
	auto scalar = QByteArray(kKeySize, Qt::Uninitialized);
	base::RandomFill(scalar.data(), scalar.size());
	const auto mine = PrivateKey(scalar);
	auto result = Envelope();
	result.ephemeral = PublicOf(mine);
	if (result.ephemeral.isEmpty()) {
		return Envelope();
	}
	result.material = Material(
		Shared(mine, publicKey),
		result.ephemeral,
		publicKey);
	return result.material.isEmpty() ? Envelope() : result;
}

QByteArray Open(
		const QString &pin,
		const QByteArray &salt,
		const QByteArray &ephemeral) {
	const auto mine = PrivateKey(Scalar(pin, salt));
	const auto recipient = PublicOf(mine);
	if (recipient.isEmpty() || ephemeral.size() != kKeySize) {
		return QByteArray();
	}
	return Material(Shared(mine, ephemeral), ephemeral, recipient);
}

} // namespace NovaGram::Seal
