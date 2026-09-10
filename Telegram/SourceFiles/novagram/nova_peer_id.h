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

namespace Window {
class SessionController;
} // namespace Window

namespace NovaGram::PeerIds {

// Opening a profile by the bare number the fork shows in it.
//
// The hard part is not the interface. MTProto has no "give me a peer by
// number": an id carries no access key and not even the kind of peer, and the
// user, chat and channel spaces are independent and overlap - so 668079634
// could be a person, a group or a channel, and by itself it is none of them.
// What is possible is opening what this authorization already knows or may
// legitimately ask about:
//
// - always: this account itself, the five service ids the documentation gives
//   a zero access hash to, and plain groups, which have no access hash by
//   design;
// - whenever the peer is already in this session with an access hash - which
//   is most of the people the user has ever met;
// - through a message: inputUserFromMessage lets the server confirm a peer
//   that really appears in a named message of a named chat. tdesktop builds
//   such inputs by itself, and Data::Session::messageWithPeer is the registry
//   it keeps for it;
// - never: an arbitrary id this authorization has not met. For a user account
//   there is no way round that, and pretending otherwise is what makes the
//   same feature in other clients feel broken.
//
// Hence the rule this follows: a number in a message becomes a link only when
// something can actually be opened for it. No dead links over prices, order
// numbers and years - and nothing that answers a tap with silence.
[[nodiscard]] bool Resolvable(
	not_null<Main::Session*> session,
	uint64 bare);

// Opens the profile, or says which of the two things went wrong: this client
// has no key for that number, or the server would not confirm it.
void Open(
	not_null<Window::SessionController*> controller,
	uint64 bare);

// Turns the resolvable numbers of a message into links. Called from
// HistoryItem::setText, which is the single door every message text passes.
void Linkify(
	TextWithEntities &text,
	not_null<Main::Session*> session);

// The prefix of the internal link the entities carry.
[[nodiscard]] QString LinkPrefix();

} // namespace NovaGram::PeerIds
