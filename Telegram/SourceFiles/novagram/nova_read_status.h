/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_msg_id.h"

class PeerData;
class HistoryItem;
class History;
class UserData;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class GenericBox;
class RpWidget;
} // namespace Ui

namespace ChatHelpers {
class Show;
} // namespace ChatHelpers

namespace HistoryView {
class SlidingBar;
} // namespace HistoryView

namespace NovaGram {

[[nodiscard]] bool ReadStatusEnabled(not_null<Main::Session*> session);
void SetReadStatusEnabled(not_null<Main::Session*> session, bool enabled);

// Starts the per-session watcher, safe to call more than once.
void StartReadStatus(not_null<Main::Session*> session);

// The single gate asked before a read receipt would be sent to the server.
[[nodiscard]] bool ReadStatusHidden(not_null<History*> history);

[[nodiscard]] bool ReadStatusHiddenFor(not_null<PeerData*> peer);

// True while the answer that decides this dialog is not there yet: a message
// has arrived in it before the list of the dialogs that were already there was
// loaded. Refusing to send a receipt can be taken back, a sent one cannot, so
// a caller that would talk to the server has to wait instead of guessing.
[[nodiscard]] bool ReadStatusPending(not_null<History*> history);

[[nodiscard]] bool ReadStatusPendingFor(not_null<PeerData*> peer);

// Writes down how far the user has read in a dialog whose receipts are being
// withheld. The desktop keeps no local copy of that position: at every start
// History::applyDialogFields takes it from the server, and the server is by
// design not told about anything read while the rule holds. Without this the
// receipt sent when the rule is lifted would repeat the position the server
// gave and leave everything read in earlier runs still unread for the other
// side. Only ever moves forward, and only for server messages.
void NoteHeldRead(not_null<History*> history, MsgId tillId);

// The furthest position written down by NoteHeldRead, or zero.
[[nodiscard]] MsgId HeldReadTill(not_null<History*> history);

// Stops hiding in this dialog and sends out the receipt that was held
// back. There is no way back: the receipt reaches the server and the
// other side sees the change. Any non-service message the user sends to
// the dialog, from this device or from another one, does the same thing:
// an answer tells the other side that the messages were read anyway.
void RevealReadStatus(not_null<PeerData*> peer);

// Drops what was decided about this dialog, because the conversation it was
// decided about is being deleted. Whoever writes first in the next one decides
// it again - starting over after deleting a conversation is the case this
// feature exists for, and a rule that outlives the conversation would answer
// about the old one.
void ForgetReadStatusRule(not_null<PeerData*> peer);

// Fires whenever the stored rules or the master switch change, so a chat that
// shows the local note can drop it the moment hiding is turned off.
[[nodiscard]] rpl::producer<> ReadStatusUpdates(
	not_null<Main::Session*> session);

[[nodiscard]] QString ReadStatusTitle();
[[nodiscard]] QString ReadStatusSettingsLabel(
	not_null<Main::Session*> session);

void ReadStatusBox(not_null<Ui::GenericBox*> box, not_null<PeerData*> peer);

// The local note pinned above the message list of a dialog where the read
// receipts are withheld. Nothing about it reaches the server; without it the
// feature would be invisible and the only way to turn it off would be a menu
// entry nobody has a reason to open.
class ReadStatusBar final {
public:
	ReadStatusBar(
		std::shared_ptr<ChatHelpers::Show> show,
		not_null<Ui::RpWidget*> parent,
		not_null<UserData*> user);
	~ReadStatusBar();

	void show();
	void hide();

	[[nodiscard]] HistoryView::SlidingBar &bar();

private:
	class Bar;

	void setupState();
	void setupHandlers();

	const std::shared_ptr<ChatHelpers::Show> _show;
	const not_null<UserData*> _user;
	QPointer<Bar> _inner;
	const std::unique_ptr<HistoryView::SlidingBar> _bar;
	bool _hidden = false;
	bool _shown = false;

};

} // namespace NovaGram
