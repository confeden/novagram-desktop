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

// Name and phone number of the account, kept so that the decoy entered after
// the emergency pin can show the real ones. Sealed to a public key whose
// private half exists only while the emergency pin is being typed, so the
// plaintext never reaches the disk before the wipe and the running
// application can replace the snapshot without knowing the pin. Empty when
// nothing could be captured.
struct EmergencyIdentity {
	QString firstName;
	QString lastName;
	QString phone;
};

// An empty pin removes the emergency pin. Fixes the key pair the snapshot is
// sealed to and takes the first snapshot of the signed in account.
void SetEmergencyPin(const QString &pin);
[[nodiscard]] bool CheckEmergencyPin(const QString &pin);
[[nodiscard]] EmergencyIdentity ReadEmergencyIdentity(const QString &pin);

// Replaces the snapshot with the account as it is now. Needs no pin and does
// nothing when there is no emergency pin or no signed in account.
void RefreshEmergencyIdentity();

[[nodiscard]] crl::time LockoutRemaining();
void RecordFailedAttempt();
void ResetFailedAttempts();

[[nodiscard]] QString FormatLockoutLeft(crl::time remaining);
[[nodiscard]] QString LockoutMessage(crl::time remaining);

// Expects the accounts to be not started yet, so no data file is open. The pin
// is the one just entered: it unlocks the stored identity for the decoy.
void RunEmergencyWipe(const QString &pin);

[[nodiscard]] bool UseRussianTexts();

[[nodiscard]] QString UnlockTitle();
[[nodiscard]] QString HiddenInputHint();
[[nodiscard]] QString SubmitButton();
[[nodiscard]] QString WrongPin();
[[nodiscard]] QString InvalidLength();
[[nodiscard]] QString PinPlaceholder();

} // namespace NovaGram
