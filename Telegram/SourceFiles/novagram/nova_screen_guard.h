/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace NovaGram {

[[nodiscard]] bool ScreenGuardSupported();
[[nodiscard]] bool ScreenGuardEnabled();
void SetScreenGuardEnabled(bool enabled);

// Installs an application wide watcher that excludes every window NovaGram
// creates from screen capture. Safe to call once at startup.
void StartScreenGuard();

} // namespace NovaGram
