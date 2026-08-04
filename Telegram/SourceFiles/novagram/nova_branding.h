/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace NovaGram {

[[nodiscard]] QString AppName();

// Upstream language packs spell the application name inside whole phrases, so
// a fork cannot rename itself by changing a constant. Rewriting the finished
// phrase keeps every translation working without touching lang.strings.
[[nodiscard]] QString WithAppName(QString text);

} // namespace NovaGram
