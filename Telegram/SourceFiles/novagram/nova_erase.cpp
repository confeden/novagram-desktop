/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_erase.h"

#include "apiwrap.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "main/main_session.h"
#include "novagram/nova_autodelete.h"
#include "novagram/nova_pin.h"
#include "novagram/nova_read_status.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

namespace NovaGram {
namespace {

constexpr auto kPageSize = 100;

// How long the finished window stays before it closes itself. Long enough to
// read three numbers, short enough not to be in the way.
constexpr auto kLingerAfterFinished = crl::time(10 * 1000);

// Telegram answers a history page request with the newest messages first, so
// walking backwards by offset identifier reaches the oldest message of the
// requested window without asking the server for anything else.
class Collector final : public std::enable_shared_from_this<Collector> {
public:
	Collector(not_null<History*> history, TimeId since);

	void start(Fn<void(int found)> done);

private:
	void requestPage();
	[[nodiscard]] bool consume(const QVector<MTPMessage> &list);
	void finish();

	const not_null<History*> _history;
	const TimeId _since = 0;
	std::vector<FullMsgId> _found;
	std::shared_ptr<Collector> _hold;
	Fn<void(int found)> _done;
	MsgId _offsetId = 0;

};

Collector::Collector(not_null<History*> history, TimeId since)
: _history(history)
, _since(since) {
}

void Collector::start(Fn<void(int found)> done) {
	_done = std::move(done);
	_hold = shared_from_this();
	requestPage();
}

void Collector::requestPage() {
	const auto session = &_history->session();
	session->api().request(MTPmessages_GetHistory(
		_history->peer->input(),
		MTP_int(_offsetId),
		MTP_int(0),
		MTP_int(0),
		MTP_int(kPageSize),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0)
	)).done([=](const MTPmessages_Messages &result) {
		const auto more = result.match([&](
				const MTPDmessages_messagesNotModified &) {
			return false;
		}, [&](const auto &data) {
			_history->owner().processUsers(data.vusers());
			_history->owner().processChats(data.vchats());
			return consume(data.vmessages().v);
		});
		if (more) {
			requestPage();
		} else {
			finish();
		}
	}).fail([=] {
		finish();
	}).send();
}

bool Collector::consume(const QVector<MTPMessage> &list) {
	if (list.isEmpty()) {
		return false;
	}
	// The messages are turned into real items on purpose: the replacement step
	// of the destruction needs an item to edit, and without this the whole
	// batch would fall back to a plain deletion.
	_history->owner().processMessages(list, NewMessageType::Existing);

	const auto peerId = _history->peer->id;
	auto reachedEnd = false;
	for (const auto &message : list) {
		const auto id = message.match([](const auto &data) {
			return MsgId(data.vid().v);
		});
		_offsetId = id;
		const auto item = _history->owner().message(peerId, id);
		if (!item) {
			continue;
		} else if (_since && item->date() < _since) {
			reachedEnd = true;
			break;
		} else if (item->out() && !item->isService()) {
			_found.push_back(item->fullId());
		}
	}
	return !reachedEnd && (list.size() >= kPageSize);
}

void Collector::finish() {
	// Reactions can only be dropped from what is loaded: Telegram has no way to
	// ask the server where this account has reacted, so nothing but the local
	// history can be walked here.
	auto reactions = 0;
	// Removing a reaction goes out the same door as putting one on, and that
	// door lifts the hiding of the read status. Erasing traces must not tell
	// the other side that their messages were read - see the class comment.
	const auto suppress = ReactionRevealSuppressor();
	for (const auto &block : _history->blocks) {
		for (const auto &view : block->messages) {
			const auto item = view->data();
			if (_since && item->date() < _since) {
				continue;
			}
			for (const auto &id : item->chosenReactions()) {
				item->toggleReaction(id, HistoryReactionSource::Existing);
				++reactions;
			}
		}
	}

	EnqueueNow(&_history->session(), _found);
	const auto found = int(_found.size());
	if (const auto done = _done) {
		done(found);
	}
	_hold = nullptr;
}

[[nodiscard]] TimeId SinceForPeriod(int index) {
	const auto now = base::unixtime::now();
	switch (index) {
	case 0: return now - 24 * 3600;
	case 1: return now - 7 * 24 * 3600;
	case 2: return now - 30 * 24 * 3600;
	}
	return 0;
}

[[nodiscard]] QString PeriodName(int index) {
	const auto russian = UseRussianTexts();
	switch (index) {
	case 0: return russian ? u"За последние 24 часа"_q : u"Last 24 hours"_q;
	case 1: return russian ? u"За последнюю неделю"_q : u"Last week"_q;
	case 2: return russian ? u"За последний месяц"_q : u"Last month"_q;
	}
	return russian ? u"За всё время"_q : u"All time"_q;
}

[[nodiscard]] QString About() {
	return UseRussianTexts()
		? u"Будут уничтожены только ваши следы в этом чате: исходящие "
			"сообщения по схеме «заменить на точку, затем удалить», а также "
			"отправленные вами файлы, голосовые и видеосообщения. Сообщения "
			"собеседника не трогаются.\n\nОперация необратима."_q
		: u"Only your own traces in this chat are destroyed: outgoing messages "
			"through the replace-then-delete steps, together with the files, "
			"voice and video messages you sent. The other side's messages are "
			"left alone.\n\nThe operation cannot be undone."_q;
}

[[nodiscard]] QString ReactionsLimit() {
	return UseRussianTexts()
		? u"Реакции снимаются только с тех сообщений, которые загружены в "
			"этом чате: у Telegram нет способа спросить сервер, где вы "
			"ставили реакцию. Пролистайте чат выше, чтобы захватить больше."_q
		: u"Reactions are removed only from the messages loaded in this chat: "
			"Telegram offers no way to ask the server where this account "
			"reacted. Scroll the chat up to cover more."_q;
}

} // namespace

// The window that stays up while the queue works.
//
// Erase evidence used to answer with one toast - "queued: 47" - and then went
// quiet for as long as the queue took, which for a long correspondence was
// minutes. Nothing on the screen said whether it was working, finished or
// stuck, and the toast that eventually reported the outcome could easily be
// missed. So the work is now watched: the numbers move while it runs, the
// window cannot be dismissed until it is through, and afterwards it waits ten
// seconds - or until it is closed - so the result is actually read.
void ProgressBox(
		not_null<Ui::GenericBox*> box,
		not_null<PeerData*> peer,
		int found) {
	const auto russian = UseRussianTexts();
	box->setTitle(rpl::single(EraseMenuText()));
	box->setWidth(st::boxWideWidth);
	box->setCloseByEscape(false);
	box->setCloseByOutsideClick(false);

	struct State {
		base::Timer close;
		bool finished = false;
	};
	const auto state = box->lifetime().make_state<State>();
	state->close.setCallback([=] { box->closeBox(); });

	const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(QString()),
		st::boxLabel));

	const auto update = [=] {
		const auto progress = EraseProgressFor(peer);
		const auto queued = progress.queued ? progress.queued : found;
		const auto done = progress.deleted + progress.skipped;
		auto text = QStringList();
		text.push_back(russian
			? u"Найдено ваших сообщений: %1"_q.arg(queued)
			: u"Messages of yours found: %1"_q.arg(queued));
		text.push_back(russian
			? u"Уничтожено: %1 · заменено на точку: %2 · пропущено: %3"_q
				.arg(progress.deleted)
				.arg(progress.replaced)
				.arg(progress.skipped)
			: u"Destroyed: %1 · replaced with a dot: %2 · skipped: %3"_q
				.arg(progress.deleted)
				.arg(progress.replaced)
				.arg(progress.skipped));
		if (!progress.finished) {
			text.push_back(russian
				? u"Осталось в очереди: %1. Окно закроется само, когда очередь "
					"дойдёт до конца — не закрывайте его, чтобы видеть ход."_q
					.arg(progress.left)
				: u"Left in the queue: %1. This window closes itself once the "
					"queue is through - leave it up to watch."_q
					.arg(progress.left));
		} else {
			text.push_back(russian
				? u"Готово. Окно закроется через несколько секунд."_q
				: u"Done. This window closes in a few seconds."_q);
		}
		label->setText(text.join(u"\n\n"_q));
		if (progress.finished && !state->finished) {
			state->finished = true;
			// Only now may it be dismissed, and only now does it carry a
			// button: while the queue runs there is nothing to press that
			// would stop it, and a button that does nothing is a lie.
			box->setCloseByEscape(true);
			box->setCloseByOutsideClick(true);
			box->addButton(
				rpl::single(russian ? u"Закрыть"_q : u"Close"_q),
				[=] { box->closeBox(); });
			state->close.callOnce(kLingerAfterFinished);
		}
	};
	update();
	EraseProgressChanges(
		&peer->session()
	) | rpl::on_next(update, box->lifetime());
}

QString EraseMenuText() {
	return UseRussianTexts() ? u"Erase evidence"_q : u"Erase evidence"_q;
}

void EraseEvidenceBox(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show,
		not_null<PeerData*> peer) {
	const auto russian = UseRussianTexts();
	box->setTitle(rpl::single(EraseMenuText()));
	box->setWidth(st::boxWideWidth);

	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(About()),
		st::boxLabel));
	Ui::AddSkip(box->verticalLayout());

	const auto history = peer->owner().history(peer);
	const auto run = [=](int index) {
		const auto since = SinceForPeriod(index);
		show->showBox(Ui::MakeConfirmBox({
			.text = (russian
				? u"Уничтожить ваши сообщения и реакции в этом чате: %1?"_q
				: u"Destroy your messages and reactions in this chat: %1?"_q
				).arg(PeriodName(index).toLower()),
			.confirmed = [=](Fn<void()> &&close) {
				close();
				const auto collector = std::make_shared<Collector>(
					history,
					since);
				collector->start([=](int found) {
					if (!found) {
						show->showToast(russian
							? u"Ваших сообщений за этот период не найдено"_q
							: u"No messages of yours in this period"_q);
						return;
					}
					// The queue starts working the moment it is handed the list,
					// and this window is what makes that visible while it does.
					show->showBox(Box([=](not_null<Ui::GenericBox*> progress) {
						ProgressBox(progress, peer, found);
					}));
				});
			},
			.confirmText = (russian ? u"Уничтожить"_q : u"Destroy"_q),
			.confirmStyle = &st::attentionBoxButton,
		}));
		box->closeBox();
	};

	for (auto i = 0; i != 4; ++i) {
		const auto button = box->addRow(
			object_ptr<Ui::SettingsButton>(
				box,
				rpl::single(PeriodName(i)),
				st::settingsButtonNoIcon),
			style::margins());
		button->setClickedCallback([=] { run(i); });
	}

	Ui::AddSkip(box->verticalLayout());
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(ReactionsLimit()),
		st::boxDividerLabel));

	box->addButton(
		rpl::single(russian ? u"Отмена"_q : u"Cancel"_q),
		[=] { box->closeBox(); });
}

} // namespace NovaGram
