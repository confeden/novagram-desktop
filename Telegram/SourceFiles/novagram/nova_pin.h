/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace NovaGram {

inline constexpr auto kMinPinLength = 4;
inline constexpr auto kMaxPinLength = 6;

[[nodiscard]] bool ValidPin(const QString &pin);

[[nodiscard]] bool PinModeEnabled();
[[nodiscard]] bool HasEmergencyPin();

void SetPinModeEnabled(bool enabled);

[[nodiscard]] bool ShuffledKeypadEnabled();
void SetShuffledKeypadEnabled(bool enabled);

// An empty pin removes the emergency pin.
void SetEmergencyPin(const QString &pin);
[[nodiscard]] bool CheckEmergencyPin(const QString &pin);

[[nodiscard]] crl::time LockoutRemaining();
void RecordFailedAttempt();
void ResetFailedAttempts();

[[nodiscard]] QString FormatLockoutLeft(crl::time remaining);
[[nodiscard]] QString LockoutMessage(crl::time remaining);

// Expects the accounts to be not started yet, so no data file is open.
void RunEmergencyWipe();

[[nodiscard]] bool UseRussianTexts();

[[nodiscard]] QString UnlockTitle();
[[nodiscard]] QString HiddenInputHint();
[[nodiscard]] QString SubmitButton();
[[nodiscard]] QString WrongPin();
[[nodiscard]] QString InvalidLength();
[[nodiscard]] QString PinPlaceholder();

} // namespace NovaGram
