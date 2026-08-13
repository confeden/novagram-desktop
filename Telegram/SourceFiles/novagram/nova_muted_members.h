/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class PeerData;
class UserData;
class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace NovaGram {

// Muting one person inside one chat.
//
// Blocking is a decision about every chat at once, leaving is a decision about
// everyone in the chat. This is the missing middle: in a group of more than two
// people the messages of one member stop demanding attention - no notification,
// and in the conversation itself the message collapses to a single dimmed line
// that unfolds when clicked. Nothing is hidden from the user, and nothing is
// told to the server: the rule lives on this device only.
//
// Private dialogs are excluded on purpose: there the stock mute already says
// the same thing, and collapsing the only person in the conversation would
// leave an empty screen.

void StartMutedMembers(not_null<Main::Session*> session);

// Whether the chat is one where a member can be muted at all.
[[nodiscard]] bool MutableChat(not_null<PeerData*> chat);

[[nodiscard]] bool MemberMuted(
	not_null<PeerData*> chat,
	not_null<UserData*> user);

void SetMemberMuted(
	not_null<PeerData*> chat,
	not_null<UserData*> user,
	bool muted);

// The one question the drawing and the notifications both ask, so that the two
// halves of the feature can never disagree about a message.
[[nodiscard]] bool ItemFromMutedMember(not_null<HistoryItem*> item);

// The second question the notifications have to ask. For a reaction or a poll
// vote the item is the user's own message and the person who acted is passed
// separately, so the item alone never names them.
[[nodiscard]] bool MutedNotificationSender(
	not_null<HistoryItem*> item,
	UserData *sender);

// Whether this message is currently shown as the collapsed line. False once the
// user has unfolded it by clicking.
[[nodiscard]] bool ItemCollapsed(not_null<HistoryItem*> item);
void ExpandItem(not_null<HistoryItem*> item);

// The handler that unfolds one collapsed row. Cached per message, because the
// click machinery compares handlers by pointer: a fresh object on every hover
// means the one pressed is never the one released, and the click is lost.
[[nodiscard]] ClickHandlerPtr ExpandLink(not_null<HistoryItem*> item);

// The rule lost the chat it was made about.
void ForgetMutedMembers(not_null<PeerData*> chat);

// Two hairlines over the userpic of a muted member. Drawn by whoever paints
// that userpic, because the chat list paints userpics after the messages and a
// mark drawn inside the message would end up underneath it.
void PaintMutedMark(
	QPainter &p,
	int left,
	int top,
	int size,
	float64 baseOpacity);

// What a collapsed row says in place of the message. Deliberately not the
// sender's name: the point of muting someone is that their name stops catching
// the eye, and a row that spells it out every time defeats that.
[[nodiscard]] QString MutedMessageText();

[[nodiscard]] QString MuteMemberMenuText(bool muted);
void MuteMemberBox(
	not_null<Ui::GenericBox*> box,
	not_null<PeerData*> chat,
	not_null<UserData*> user);

} // namespace NovaGram
