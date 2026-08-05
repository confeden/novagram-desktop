/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_read_status.h"

#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "novagram/nova_pin.h"
#include "storage/storage_account.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDataStream>

namespace NovaGram {
namespace {

constexpr auto kMagic = quint32(0x4E565253);
constexpr auto kVersion = qint32(1);
constexpr auto kStateKey = "novagram_read_status"_cs;

enum class Rule : qint32 {
	Hidden,
	Revealed,
};

struct State {
	bool enabled = true;
	base::flat_map<PeerId, Rule> rules;
};

[[nodiscard]] State ReadState(not_null<Main::Session*> session) {
	auto result = State();
	const auto blob = session->local().readPref<QByteArray>(kStateKey);
	if (blob.isEmpty()) {
		return result;
	}
	auto stream = QDataStream(blob);
	stream.setVersion(QDataStream::Qt_5_15);
	auto magic = quint32(0);
	auto version = qint32(0);
	auto enabled = qint32(0);
	auto count = qint32(0);
	stream >> magic >> version >> enabled >> count;
	if (stream.status() != QDataStream::Ok
		|| magic != kMagic
		|| version != kVersion
		|| count < 0) {
		return State();
	}
	for (auto i = 0; i != count; ++i) {
		auto peerId = quint64(0);
		auto rule = qint32(0);
		stream >> peerId >> rule;
		if (stream.status() != QDataStream::Ok) {
			return State();
		}
		result.rules.emplace(PeerId(peerId), Rule(rule));
	}
	result.enabled = (enabled != 0);
	return result;
}

void WriteState(not_null<Main::Session*> session, const State &state) {
	auto blob = QByteArray();
	auto stream = QDataStream(&blob, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_15);
	stream << kMagic
		<< kVersion
		<< qint32(state.enabled ? 1 : 0)
		<< qint32(state.rules.size());
	for (const auto &[peerId, rule] : state.rules) {
		stream << quint64(peerId.value) << qint32(rule);
	}
	session->local().writePref<QByteArray>(kStateKey, blob);
}

class Watcher final {
public:
	explicit Watcher(not_null<Main::Session*> session);

	[[nodiscard]] const State &state() const {
		return _state;
	}
	void change(Fn<void(State&)> mutation);

private:
	void note(not_null<HistoryItem*> item);

	const not_null<Main::Session*> _session;
	State _state;

};

Watcher::Watcher(not_null<Main::Session*> session)
: _session(session)
, _state(ReadState(session)) {
	_session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		note(item);
	}, _session->lifetime());
}

void Watcher::change(Fn<void(State&)> mutation) {
	mutation(_state);
	WriteState(_session, _state);
}

void Watcher::note(not_null<HistoryItem*> item) {
	const auto peer = item->history()->peer;
	if (!peer->isUser() || peer->isSelf() || item->isService()) {
		return;
	} else if (_state.rules.contains(peer->id)) {
		return;
	}
	// The very first message decides, once and for good. Whether the dialog
	// was started by the other side cannot be recomputed later: by then the
	// user has replied, and the beginning may not even be loaded.
	if (item->out()) {
		return;
	}
	change([&](State &state) {
		state.rules.emplace(peer->id, Rule::Hidden);
	});
}

[[nodiscard]] base::flat_map<Main::Session*, std::unique_ptr<Watcher>> &Map() {
	static auto result
		= base::flat_map<Main::Session*, std::unique_ptr<Watcher>>();
	return result;
}

[[nodiscard]] Watcher *Find(not_null<Main::Session*> session) {
	const auto i = Map().find(session.get());
	return (i != end(Map())) ? i->second.get() : nullptr;
}

[[nodiscard]] Watcher &Get(not_null<Main::Session*> session) {
	if (const auto found = Find(session)) {
		return *found;
	}
	StartReadStatus(session);
	return *Find(session);
}

} // namespace

bool ReadStatusEnabled(not_null<Main::Session*> session) {
	return Get(session).state().enabled;
}

void SetReadStatusEnabled(not_null<Main::Session*> session, bool enabled) {
	Get(session).change([&](State &state) {
		state.enabled = enabled;
	});
}

void StartReadStatus(not_null<Main::Session*> session) {
	if (Find(session)) {
		return;
	}
	Map().emplace(session.get(), std::make_unique<Watcher>(session));
	session->lifetime().add([raw = session.get()] {
		Map().remove(raw);
	});
}

bool ReadStatusHiddenFor(not_null<PeerData*> peer) {
	const auto session = &peer->session();
	if (!ReadStatusEnabled(session)) {
		return false;
	}
	const auto &rules = Get(session).state().rules;
	const auto i = rules.find(peer->id);
	return (i != end(rules)) && (i->second == Rule::Hidden);
}

bool ReadStatusHidden(not_null<History*> history) {
	return ReadStatusHiddenFor(history->peer);
}

void RevealReadStatus(not_null<PeerData*> peer) {
	const auto id = peer->id;
	Get(&peer->session()).change([&](State &state) {
		state.rules[id] = Rule::Revealed;
	});
}

QString ReadStatusTitle() {
	return UseRussianTexts()
		? u"Скрывать статус прочтения"_q
		: u"Hide the read status"_q;
}

QString ReadStatusSettingsLabel(not_null<Main::Session*> session) {
	const auto russian = UseRussianTexts();
	if (!ReadStatusEnabled(session)) {
		return russian ? u"выключено"_q : u"off"_q;
	}
	return russian ? u"в новых диалогах"_q : u"in new dialogs"_q;
}

void ReadStatusBox(
		not_null<Ui::GenericBox*> box,
		not_null<PeerData*> peer) {
	const auto russian = UseRussianTexts();
	const auto hidden = ReadStatusHiddenFor(peer);
	box->setTitle(rpl::single(ReadStatusTitle()));
	box->setWidth(st::boxWideWidth);

	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(hidden
			? (russian
				? u"Этот диалог начали не вы, поэтому NovaGram не сообщает "
					"собеседнику, что вы прочитали его сообщения: галочки "
					"прочтения у него не появляются.\n\nПобочный эффект: для "
					"Telegram сообщения остаются непрочитанными, поэтому "
					"счётчик непрочитанного в этом диалоге может возвращаться "
					"после перезапуска и на других устройствах."_q
				: u"You did not start this dialog, so NovaGram does not tell "
					"the other side that you read their messages: the read "
					"marks never appear for them.\n\nSide effect: for Telegram "
					"the messages stay unread, so the unread counter in this "
					"dialog can come back after a restart and on other "
					"devices."_q)
			: (russian
				? u"В этом диалоге статус прочтения не скрывается."_q
				: u"The read status is not hidden in this dialog."_q)),
		st::boxLabel));

	if (hidden) {
		Ui::AddSkip(box->verticalLayout());
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(russian
				? u"Отключение необратимо: как только подтверждение уйдёт на "
					"сервер, собеседник увидит, что сообщения прочитаны, и "
					"вернуть скрытие в этом диалоге будет нельзя."_q
				: u"Turning it off cannot be undone: once the receipt reaches "
					"the server the other side sees the messages as read, and "
					"hiding cannot be restored in this dialog."_q),
			st::boxDividerLabel));

		box->addButton(
			rpl::single(russian
				? u"Отключить навсегда"_q
				: u"Turn off permanently"_q),
			[=] {
				RevealReadStatus(peer);
				box->closeBox();
			},
			st::attentionBoxButton);
	}

	box->addButton(
		rpl::single(russian ? u"Закрыть"_q : u"Close"_q),
		[=] { box->closeBox(); });
}

} // namespace NovaGram
