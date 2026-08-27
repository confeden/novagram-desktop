/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace NovaGram::Authenticode {

// What a check of a downloaded installer came to. Anything but Ok means the
// file must not be executed; the separate values exist so that the log names
// which of four very different things happened, because they call for four
// very different reactions from whoever reads it.
enum class Verdict : uchar {
	Ok,

	// No Authenticode signature at all, or one Windows cannot even parse.
	Unsigned,

	// Signed, but the bytes no longer match what was signed. This is the shape
	// a modified installer has.
	Tampered,

	// A whole, valid signature - made by somebody else's certificate.
	ForeignSigner,

	// The check itself could not be made. Refused all the same: a verifier
	// that has no answer is not a verifier that said yes.
	Unknown,
};

// Whether `path` is an installer this build is allowed to run: Authenticode
// signed, undamaged, and signed by the one certificate whose SHA-256 is
// compiled in.
//
// Deliberately not "does Windows trust this file". The release certificate is
// self-signed, so the ordinary chain check says no - and where it says yes it
// says yes to anything signed by any of the several hundred authorities the
// machine happens to trust, an employer's and a piece of malware's included.
// What decides here is the signer certificate itself, compared byte for byte.
[[nodiscard]] Verdict Verify(const QString &path);

// A few words for the log. Not shown to anybody: the update UI has its own
// wording, in both languages.
[[nodiscard]] QString VerdictName(Verdict verdict);

} // namespace NovaGram::Authenticode
