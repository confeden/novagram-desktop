/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_drop_incoming.h"

#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "novagram/nova_pin.h"
#include "novagram/nova_read_status.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"

namespace NovaGram {

bool DropIncomingOffered(not_null<PeerData*> peer) {
	// The same condition the read-status entry uses. This one lives under it
	// and means nothing without it: the silence it promises rests on the
	// receipts being withheld, which is that rule's promise and not this
	// switch's.
	return peer->isUser() && ReadStatusHiddenFor(peer);
}

MsgId DropIncomingFrom(not_null<PeerData*> peer) {
	const auto ranges = ReadStatusDropRanges(peer);
	return (!ranges.empty() && ranges.back().open)
		? ranges.back().from
		: MsgId(0);
}

bool DropIncomingActive(not_null<PeerData*> peer) {
	return (DropIncomingFrom(peer) > 0);
}

void SetDropIncoming(not_null<PeerData*> peer, bool active) {
	if (!active) {
		// The range is closed, not forgotten: what it dropped has to stay
		// dropped, or the next time the history is opened the server hands
		// back the whole pile the user turned this on to avoid.
		CloseReadStatusDropRange(peer);
		return;
	} else if (!DropIncomingOffered(peer) || DropIncomingActive(peer)) {
		return;
	}
	// Everything already in the dialog stays readable; only what comes after
	// the newest message it holds right now is dropped.
	const auto history = peer->owner().history(peer);
	const auto last = history->lastMessage();
	const auto from = (last && IsServerMsgId(last->id))
		? (last->id + 1)
		: MsgId(1);
	auto ranges = ReadStatusDropRanges(peer);
	ranges.push_back({ from, MsgId(0), true });
	SetReadStatusDropRanges(peer, std::move(ranges));
}

bool DropsIncoming(not_null<PeerData*> peer, MsgId id, bool out) {
	if (out || !IsServerMsgId(id)) {
		// A negative id is this client's own unsent message.
		return false;
	} else if (!peer->isUser()) {
		// Only a private dialog can be ignored this way: a group has members
		// who are not the one person being ignored.
		return false;
	}
	const auto ranges = ReadStatusDropRanges(peer);
	for (const auto &range : ranges) {
		if (id < range.from) {
			continue;
		} else if (range.open) {
			// The open range goes on into whatever arrives next, and the id it
			// is now dropping is written down so that closing it later leaves
			// behind exactly what was thrown away.
			NoteReadStatusDropped(peer, id);
			return true;
		} else if (id <= range.till) {
			return true;
		}
	}
	return false;
}

QString DropIncomingTitle() {
	return UseRussianTexts()
		? u"Удалять будущие сообщения"_q
		: u"Delete future messages"_q;
}

QString DropIncomingStopTitle() {
	return UseRussianTexts()
		? u"Не удалять будущие сообщения"_q
		: u"Stop deleting future messages"_q;
}

QString DropIncomingAbout() {
	return UseRussianTexts()
		? u"Всё, что собеседник пришлёт дальше, удаляется на этом "
			"устройстве до того, как будет сохранено, показано или "
			"объявлено: ни уведомления, ни счётчика, ни возможности "
			"прочитать позже. У собеседника не удаляется ничего, и его "
			"сообщения так и висят непрочитанными.\n\nВыключается само, как "
			"только вы что-нибудь отправите в этот чат — сообщение или "
			"реакцию, с любого устройства."_q
		: u"Everything this person sends from now on is deleted on this "
			"device before it is stored, shown or announced: no "
			"notification, no unread badge, no way to read it later. Nothing "
			"is deleted for them, and their messages stay "
			"unread.\n\nIt switches itself off as soon as you send anything "
			"here - a message or a reaction, from any device."_q;
}

void DropIncomingBox(
		not_null<Ui::GenericBox*> box,
		not_null<PeerData*> peer) {
	const auto russian = UseRussianTexts();
	const auto active = DropIncomingActive(peer);
	box->setTitle(rpl::single(DropIncomingTitle()));
	box->setWidth(st::boxWideWidth);

	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(DropIncomingAbout()),
		st::boxLabel));

	if (active) {
		Ui::AddSkip(box->verticalLayout());
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(russian
				? u"Сейчас включено. Выключение вернёт только те сообщения, "
					"которые придут после него, — удалённое не "
					"восстанавливается."_q
				: u"It is on. Turning it off brings back only what arrives "
					"afterwards - what was dropped is not kept anywhere."_q),
			st::boxDividerLabel));
		box->addButton(
			rpl::single(DropIncomingStopTitle()),
			[=] {
				SetDropIncoming(peer, false);
				box->closeBox();
			});
	} else {
		box->addButton(
			rpl::single(russian ? u"Начать удалять"_q : u"Start deleting"_q),
			[=] {
				SetDropIncoming(peer, true);
				box->closeBox();
			},
			st::attentionBoxButton);
	}

	box->addButton(
		rpl::single(russian ? u"Закрыть"_q : u"Close"_q),
		[=] { box->closeBox(); });
}

} // namespace NovaGram
