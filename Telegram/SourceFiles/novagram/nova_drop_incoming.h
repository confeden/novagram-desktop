/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_msg_id.h"

class PeerData;

namespace Ui {
class GenericBox;
} // namespace Ui

namespace NovaGram {

// Drops everything the other side sends in one dialog, on this device only.
//
// The case it exists for: somebody writes to you whom you do not want to block
// - blocking tells them, and it also tells them that everything they sent up
// to that point was read - but do not want to hear from either. With this on,
// what they write from that moment never reaches this computer's storage, its
// notifications or its screen. From their side the conversation looks exactly
// like one being ignored: delivered, never read.
//
// Where it can be turned on, and what turns it off. Only inside a dialog that
// already hides its read receipts - one the other side started and the user
// has never answered in. Two reasons, and both matter: the promise "they get
// no read receipt" is the read-status rule's, not this switch's, and a dialog
// the user did answer in is a conversation, where quietly dropping half of it
// is not ignoring somebody but losing mail. It goes off the moment the user
// sends anything there - a message, a reaction, from this device or another -
// because that is the same door the hiding is lifted by (RevealReadStatus).
//
// What it deliberately does not do: nothing is deleted for the other side, no
// request about it leaves this device, and there is no way to read what was
// dropped - it is not a folder, not an archive and not a filter.
[[nodiscard]] bool DropIncomingOffered(not_null<PeerData*> peer);
[[nodiscard]] bool DropIncomingActive(not_null<PeerData*> peer);

// The id from which incoming messages are dropped, or zero.
[[nodiscard]] MsgId DropIncomingFrom(not_null<PeerData*> peer);

void SetDropIncoming(not_null<PeerData*> peer, bool active);

// The one decision, asked wherever a message would become an item. Takes the
// peer rather than the history: a message can arrive before its conversation
// record does, and building one from here to answer this would be the wrong
// way round.
[[nodiscard]] bool DropsIncoming(
	not_null<PeerData*> peer,
	MsgId id,
	bool out);

[[nodiscard]] QString DropIncomingTitle();
[[nodiscard]] QString DropIncomingStopTitle();
[[nodiscard]] QString DropIncomingAbout();

void DropIncomingBox(
	not_null<Ui::GenericBox*> box,
	not_null<PeerData*> peer);

} // namespace NovaGram
