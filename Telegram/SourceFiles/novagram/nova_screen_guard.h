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

// True while this guard insists that every window of the client stays out of
// screen capture. Platform::SetWindowScreenshotProtection asks this before it
// clears a window's display affinity and refuses the request while the answer
// is true; a request to switch protection *on* is never refused, so anything
// that wants a window hidden may still have it.
//
// It exists because protection is not the fork's alone any more. Upstream
// keeps a registry of its own reasons (Core::ScreenshotProtection) and, the
// moment its last one is released, walks over every top level window switching
// protection off - which on Windows clears WDA_EXCLUDEFROMCAPTURE from the
// main window as well. One view once photo, opened and closed, used to leave
// the whole client visible to capture until it was restarted.
//
// The answer is read from the live setting, not from a flag latched at start,
// so turning the guard off in the settings takes the refusal down with it.
[[nodiscard]] bool ScreenGuardHoldsWindows();

// Installs an application wide watcher that excludes every window NovaGram
// creates from screen capture. Safe to call once at startup.
void StartScreenGuard();

} // namespace NovaGram
