/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace NovaGram {

// When the passcode is asked for again after it has been entered once.
//
// The same five choices exist on the Android fork, spelled with the same
// storage keys, so one line in the roadmap describes both platforms. Upstream
// offers only the third and fourth of them - a stretch of doing nothing - and
// nothing at all for "I walked away from an unlocked machine", which is the
// case a passcode is most often wanted for.
//
// Every option answers one question: when does an unlock stop being valid. An
// answer is made of three parts - an event, a stretch of inactivity and a
// ceiling on the age of the unlock - and an option is any combination of them.
enum class PinLockPolicy : uchar {
	OnMinimize,
	OnScreenLock,
	Inactivity10m,
	Inactivity60m,
	OnStart,
};

[[nodiscard]] PinLockPolicy PinPolicy();
void SetPinPolicy(PinLockPolicy policy);

// In the order they are offered, strictest first.
[[nodiscard]] std::vector<PinLockPolicy> PinPolicyOptions();

[[nodiscard]] QString PinPolicyTitle();
[[nodiscard]] QString PinPolicyName(PinLockPolicy policy);
[[nodiscard]] QString PinPolicyAbout();

// Seconds of doing nothing after which the passcode locks, or zero when the
// chosen option does not measure inactivity at all. Read by
// Application::checkAutoLock in place of the stock setting.
[[nodiscard]] int PinPolicyAutoLockSeconds();

// The passcode has just been entered.
void NotePinUnlocked();

// The main window went out of sight - minimised, or closed to the tray.
void NotePinWindowHidden();

// The Windows session was locked.
void NotePinScreenLocked();

// Milliseconds left before the unlock stops being valid by age alone, a
// negative or zero value when it already has, and std::nullopt when the chosen
// option puts no ceiling on it. Only "ask at start" has one, and only because
// a client that is never closed would otherwise never ask again.
[[nodiscard]] std::optional<crl::time> PinSessionRemaining();

} // namespace NovaGram
