/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace NovaGram::Decoy {

// Whether the permanent decoy entered after the emergency PIN is in effect.
// Cheap: the marker is read once per process.
[[nodiscard]] bool Active();

// Turns the decoy on. Called before the destruction starts and again after it,
// because the sweep removes the marker together with everything else. The
// identity comes from the account being destroyed and may be empty, in which
// case the decoy invents its own.
void Arm(
	const QString &firstName,
	const QString &lastName,
	const QString &phone);

// Seed and time anchor of the invented account. Both are fixed when the decoy
// is armed, so the conversations are the same on every later launch and are as
// old as the wipe.
[[nodiscard]] quint64 Seed();
[[nodiscard]] TimeId Anchor();

[[nodiscard]] QString SelfFirstName();
[[nodiscard]] QString SelfLastName();
[[nodiscard]] QString SelfPhone();

} // namespace NovaGram::Decoy
