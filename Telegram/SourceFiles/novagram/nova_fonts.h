/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace NovaGram {

// The family behind a font setting, the empty "Default" one included. On
// Windows the built-in Open Sans looks noticeably lighter than the system UI
// font, so "Default" means the system semibold face. The application hands the
// result to style::SetCustomFont (and not to the Open Sans fallback inside
// style::internal::ResolveFont), which keeps the usual x-height normalization,
// and the font box previews "Default" through the same answer, or it would show
// Open Sans for an entry that never renders with it.
//
// Asking QFontDatabase whether the family exists is unreliable here: Qt 6
// often reports Segoe UI Semibold as a style of Segoe UI rather than as a
// family of its own, and the check silently dropped the default. Naming the
// family directly is safe, because an unknown family falls back to the same
// system default an empty one would give.
[[nodiscard]] inline QString ResolveFontFamily(QString chosen) {
#ifdef Q_OS_WIN
	if (chosen.isEmpty()) {
		return u"Segoe UI Semibold"_q;
	}
#endif // Q_OS_WIN
	return chosen;
}

} // namespace NovaGram
