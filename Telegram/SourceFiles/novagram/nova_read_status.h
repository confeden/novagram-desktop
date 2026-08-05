/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class PeerData;
class HistoryItem;
class History;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class GenericBox;
} // namespace Ui

namespace NovaGram {

[[nodiscard]] bool ReadStatusEnabled(not_null<Main::Session*> session);
void SetReadStatusEnabled(not_null<Main::Session*> session, bool enabled);

// Starts the per-session watcher, safe to call more than once.
void StartReadStatus(not_null<Main::Session*> session);

// The single gate asked before a read receipt would be sent to the server.
[[nodiscard]] bool ReadStatusHidden(not_null<History*> history);

[[nodiscard]] bool ReadStatusHiddenFor(not_null<PeerData*> peer);

// Stops hiding in this dialog. There is no way back: the receipt is already
// on its way to the server and the other side has already seen the change.
void RevealReadStatus(not_null<PeerData*> peer);

[[nodiscard]] QString ReadStatusTitle();
[[nodiscard]] QString ReadStatusSettingsLabel(
	not_null<Main::Session*> session);

void ReadStatusBox(not_null<Ui::GenericBox*> box, not_null<PeerData*> peer);

} // namespace NovaGram
