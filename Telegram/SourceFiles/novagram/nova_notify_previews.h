/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace NovaGram {

// Keeping the text of a message out of the push queue.
//
// The desktop client has no push of its own: it holds its own connection and
// receives every message over it, so nothing here is drawn from a push payload
// and no switch on this machine can change what a push carries. What this does
// is not about this machine at all. `show_previews` is an account setting, and
// while it is on the Telegram servers compose the text of a notification
// themselves, put it into a push payload and hand it to Google or Apple for
// delivery to the phone. Turning it off is the only lever Telegram offers for
// "do not put the text into push", and the promise it serves is an account
// promise, so the desktop is as obliged to send it as the phone is.
//
// The Android fork already does this. Both halves have to agree: an account
// where one client keeps saying "previews on" and the other keeps saying
// "previews off" ends up with whichever spoke last.
[[nodiscard]] bool NotifyPreviewsWithheld(not_null<Main::Session*> session);
void SetNotifyPreviewsWithheld(
	not_null<Main::Session*> session,
	bool withheld);

// Starts the per-session watcher, safe to call more than once.
void StartNotifyPreviews(not_null<Main::Session*> session);

// Forces `show_previews` to false in settings the client is about to send.
// Called from the three places in ApiWrap that put an inputPeerNotifySettings
// on the wire.
//
// A value with no flags at all is passed through untouched: that is not a
// change of anything, it is how the client says "this peer has no settings of
// its own". Adding a field to it would turn a reset into an exception and make
// every peer the user ever muted and unmuted into a permanent server record.
[[nodiscard]] MTPinputPeerNotifySettings WithheldNotifyPreviews(
	not_null<Main::Session*> session,
	const MTPinputPeerNotifySettings &settings);

// The value of `show_previews` to put into account.setReactionsNotifySettings.
// Kept apart from the local one on purpose - see the comment on
// AcceptReactionsPreviewsEcho.
[[nodiscard]] bool ReactionsPreviewsToSend(
	not_null<Main::Session*> session,
	bool local);

// What the local `show_previews` of the reactions settings should become,
// given what the server just said.
//
// The local value is not write-only here: notifications_manager reads it to
// decide whether the name of whoever reacted is shown, and the stock reactions
// settings show it as a switch. Storing a "false" the promise itself put on
// the server would blank that name on this screen too, quietly turning an
// account-level promise about push into the stock "do not show the sender" -
// which is a different setting, not asked for, and visibly a downgrade.
//
// While the promise is on:
// - a "true" means the server has not been told yet, so it is taken as the
//   local answer too and a push is asked for;
// - a "false" says nothing about this screen, because it is there to keep the
//   promise and may have been written by the Android fork rather than by this
//   client. The local answer is then the one the user last chose here, kept by
//   NoteReactionsSenderShown - the server cannot be asked for it, so it has to
//   be remembered.
[[nodiscard]] bool LocalReactionsPreviews(
	not_null<Main::Session*> session,
	bool serverValue);

// Remembers the user's own answer to "show the name of whoever reacted",
// which while the promise is on cannot be read back from the server.
void NoteReactionsSenderShown(
	not_null<Main::Session*> session,
	bool shown);

[[nodiscard]] QString NotifyPreviewsTitle();

} // namespace NovaGram
