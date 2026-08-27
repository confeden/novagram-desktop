/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace NovaGram::Decoy {

// Whether the permanent decoy entered after the emergency PIN is in effect.
// Cheap: the marker is read - and unsealed - once per process, and this is
// asked on the path of every network request.
//
// Driven by the marker's presence, never by its contents: a marker this
// machine cannot open still means a wipe happened here. The opposite would
// take the disguise down in front of whoever was just shown an account.
[[nodiscard]] bool Active();

// Turns the decoy on. Called before the destruction starts and again after it,
// because the sweep removes the marker together with everything else. The
// identity comes from the account being destroyed and may be empty, in which
// case the decoy invents its own.
void Arm(
	const QString &firstName,
	const QString &lastName,
	const QString &phone);

// Writes the marker back in whatever format applies now, so that it follows
// the device binding between sealed and unsealed. Called by the device lock
// while the machine secret is still there (N15). Does nothing when the decoy
// is not armed, and never replaces a marker holding an identity with one that
// does not.
void Rewrite();

// Seed and time anchor of the invented account. Both are fixed when the decoy
// is armed, so the conversations are the same on every later launch and are as
// old as the wipe.
[[nodiscard]] quint64 Seed();
[[nodiscard]] TimeId Anchor();

[[nodiscard]] QString SelfFirstName();
[[nodiscard]] QString SelfLastName();
[[nodiscard]] QString SelfPhone();

} // namespace NovaGram::Decoy
