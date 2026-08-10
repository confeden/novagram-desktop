/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_map.h"

namespace NovaGram::Decoy {

// The account the decoy pretends to be.
//
// Everything here is ordinary Telegram data. The decoy does not draw an
// imitation of the client, it hands the client something to draw: a hand made
// window can only get close to the original, and "almost right" is worse than
// plainly different, because that is exactly what a Telegram user notices.
//
// Generation is deterministic, so the same seed produces the same people, the
// same conversations and the same timestamps on every launch. The name and the
// phone number are the real ones of the destroyed account when they could be
// captured; only the content is invented.
struct Persona {
	MTPUser self = MTPUser();
	QVector<MTPUser> users;
	QVector<MTPChat> chats;
	QVector<MTPContact> contacts;

	// Newest conversation first, the order the dialog list shows.
	QVector<MTPDialog> dialogs;
	// Last message of every dialog, what messages.getDialogs carries.
	QVector<MTPMessage> topMessages;
	// Positive keys are users, negative ones chats and channels, exactly as
	// the peer identifiers the requests carry. Newest message first.
	base::flat_map<int64, QVector<MTPMessage>> history;

	// Profile descriptions, keyed the same way as the histories. Most people
	// are absent from it on purpose: a decoy where everyone wrote a bio is
	// itself the odd thing.
	base::flat_map<int64, QString> about;

	// Members of the ordinary groups, the owner first. Broadcast channels are
	// absent: they do not show a member list.
	base::flat_map<int64, QVector<int64>> members;

	int64 selfId = 0;
	int lastMessageId = 0;
};

// A freshly built copy for one answer. Building is deterministic, so this is
// equal to every other copy down to the identifiers and timestamps.
[[nodiscard]] Persona Build();

// A cached copy for reading identifiers only.
[[nodiscard]] const Persona &Snapshot();

// Everything the decoy hands out is unmuted with default sounds, which is what
// a peer nobody touched the notification settings of looks like.
[[nodiscard]] MTPPeerNotifySettings DefaultNotifySettings();

} // namespace NovaGram::Decoy
