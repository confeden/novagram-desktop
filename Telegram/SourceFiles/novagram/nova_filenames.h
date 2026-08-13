/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace NovaGram {

// Names of files the application writes to disk by itself.
//
// The local database and the media cache are already opaque on this platform:
// upstream draws their names from a random key. What is not opaque is the
// download folder - a file that arrives with automatic downloading on is
// written under the name its sender chose, so a directory listing is a list of
// what was sent to this computer, readable without opening anything.
//
// Only the automatic path is masked. An explicit "Save as" is the user asking
// for a file under a name of their choosing, and answering that with a random
// one would be a different feature, and a worse one.
[[nodiscard]] bool MaskedFileNamesEnabled();
void SetMaskedFileNamesEnabled(bool enabled);

[[nodiscard]] QString MaskedFileNamesTitle();

// Returns `name` unchanged while the setting is off. Otherwise keeps the
// extension - it decides which program opens the file, and a file the user
// cannot open is not saved, it is lost - and replaces the rest with a token.
//
// The token is derived, not drawn: the same original name always gives the same
// masked one. Saving the same file twice therefore reuses it instead of leaving
// a second copy, which is what a fresh random value on every call would do. It
// cannot be read back either way, because the derivation is keyed by a random
// value made once per installation and kept with the settings.
[[nodiscard]] QString MaskLocalFileName(const QString &name);

} // namespace NovaGram
