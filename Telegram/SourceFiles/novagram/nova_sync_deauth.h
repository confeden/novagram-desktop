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

namespace NovaGram::SyncDeauth {

// One emergency PIN, every device.
//
// The emergency PIN destroys the client it was typed on. Every other client of
// the same account keeps the messages, the media and the cache that the PIN
// exists to destroy, and stays signed in - which is the whole account, on the
// tablet in the next room. This carries the one bit "a wipe happened" to them
// over the only channel every client of an account already shares: Saved
// Messages.
//
// The channel. The device under duress writes one 24-character line into Saved
// Messages and waits for the server to acknowledge it before anything else
// happens - a wipe that ran first would take the authorization key with it and
// there would be nothing left to send with. Every other NovaGram client sees
// that line arrive, recognises it and runs the same destruction locally,
// sending nothing itself. Nothing else is needed and nothing else is used: no
// server of the fork's own, no push, no extra authorization.
//
// Why writing into Saved Messages is enough of an authorisation. Only a
// session that is signed in to this account can write there. Anyone able to do
// that can already end every other session from the account's own device list;
// they do not need this to do harm. So the line is not a secret and is not
// signed with one - it is a message from the account to itself. What it does
// carry is a check digit over the account's own id, so that a line copied out
// of someone else's screenshot means nothing here.
//
// What keeps it from firing by accident, and from firing again at the next
// login - the three questions this module exists to answer:
//  - the line must be the account's own outgoing message in Saved Messages,
//    not a forward of one, and must match the grammar and the check exactly.
//    Twenty-four characters of which eighty bits are random are not typed by
//    accident;
//  - it must be newer than this authorization. The server says when this
//    session was created, so a line that was already lying in Saved Messages
//    when the user signed in here can never fire - which is exactly the state
//    a device is in after it has wiped itself and been signed in again;
//  - it must be newer than the moment this feature was switched on here, and
//    newer than the last line this client acted on.
//
// What it costs while nothing happens: one comparison of a string length on
// each arriving message, one small request when the session starts, and one
// every half hour. No polling of anything else, no timer that wakes the CPU.
[[nodiscard]] bool Enabled();
void SetEnabled(bool enabled);

[[nodiscard]] QString Title();
[[nodiscard]] QString About();

// Per-session watcher: reads the local watermark, asks the server when this
// authorization was created, looks at Saved Messages once, and listens to
// every message that becomes an item from there on. Safe to call more than
// once.
void Start(not_null<Main::Session*> session);

// Writes the line into Saved Messages of every signed in account and calls
// `done` when the server has acknowledged all of them - or when the deadline
// passes, because a device seized with no network still has to destroy what is
// on it. Calls `done` at once, from inside the call, when there is nothing to
// send: the feature is off, no session is running (the desktop lock at a cold
// start has no key in memory to send with), or the decoy is up.
void Broadcast(Fn<void()> done);

} // namespace NovaGram::SyncDeauth
