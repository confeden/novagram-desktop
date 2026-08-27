/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace NovaGram::Seal {

inline constexpr auto kKeySize = 32;
inline constexpr auto kMaterialSize = 256;

// One half of a key pair defined by a pin.
//
// The private half is never stored: it is derived from the pin whenever it is
// needed, so the disk holds only the public half. That is what lets the
// running application overwrite a sealed value it cannot read — the pin is
// required to open, not to seal.
//
// The public key does not weaken the pin beyond what is already on the disk:
// it can be attacked by guessing pins exactly the way the emergency verifier
// stored next to it can, with the same derivation cost.
//
// The derivation cost is passed in and not compiled in, because the value that
// produced an existing public key is the only one that can reproduce it: a
// raised constant would silently stop opening every snapshot already on a
// disk. The caller keeps the number next to the key it derived.
[[nodiscard]] QByteArray PublicKey(
	const QString &pin,
	const QByteArray &salt,
	int iterations);

// One sealing. The ephemeral public key is stored next to the ciphertext in
// the clear, the material is used once and forgotten.
struct Envelope {
	QByteArray ephemeral;
	QByteArray material;
};

[[nodiscard]] Envelope SealTo(const QByteArray &publicKey);

// The same material, recovered from the pin and the stored ephemeral key.
// Empty when the pin is wrong or anything else does not add up. The iteration
// count must be the one PublicKey() was called with.
[[nodiscard]] QByteArray Open(
	const QString &pin,
	const QByteArray &salt,
	const QByteArray &ephemeral,
	int iterations);

} // namespace NovaGram::Seal
