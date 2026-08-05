/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_autodelete.h"

#include "api/api_common.h"
#include "api/api_editing.h"
#include "apiwrap.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_drafts.h"
#include "data/data_histories.h"
#include "data/data_peer.h"
#include "data/data_msg_id.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "novagram/nova_pin.h"
#include "storage/storage_account.h"

#include <QtCore/QDataStream>

#include <algorithm>

namespace NovaGram {
namespace {

constexpr auto kMagic = quint32(0x4E564144);
constexpr auto kVersion = qint32(1);
constexpr auto kTickInterval = crl::time(60 * 1000);
constexpr auto kBusyInterval = crl::time(5 * 1000);
constexpr auto kStartupDelay = crl::time(15 * 1000);
constexpr auto kRetryDelay = TimeId(5 * 60);
constexpr auto kPerTick = 5;

// The gap between replacing the text and deleting the message. The promise
// spells it out as "изменить на точку, а через минуту стереть": the edited
// version has to reach the other side before the message disappears,
// otherwise the replacement never gets seen and the step is pointless.
constexpr auto kReplaceToDeleteDelay = TimeId(60);

enum class Stage : qint32 {
	Replace,
	Delete,
};

struct Entry {
	PeerId peerId = 0;
	MsgId msgId = 0;
	TimeId dueAt = 0;
	Stage stage = Stage::Replace;
};

struct State {
	bool enabled = true;
	qint32 periodHours = kAutoDeleteDefaultHours;
	QString replacement = u"."_q;
	TimeId activatedAt = 0;
	base::flat_map<PeerId, PeerRule> rules;
	std::vector<Entry> queue;
};

// The state rides in the account key-value store, which already lives in an
// encrypted blob with a randomized file name. A separate file of our own would
// have announced the feature by its name alone.
constexpr auto kStateKey = "novagram_autodelete"_cs;

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
	stream >> magic >> version;
	if (stream.status() != QDataStream::Ok
		|| magic != kMagic
		|| version != kVersion) {
		return result;
	}
	auto enabled = qint32(0);
	auto activatedAt = qint32(0);
	auto rulesCount = qint32(0);
	auto queueCount = qint32(0);
	stream >> enabled
		>> result.periodHours
		>> result.replacement
		>> activatedAt
		>> rulesCount;
	if (stream.status() != QDataStream::Ok
		|| rulesCount < 0
		|| result.periodHours < kAutoDeleteMinHours
		|| result.periodHours > kAutoDeleteMaxHours) {
		return State();
	}
	for (auto i = 0; i != rulesCount; ++i) {
		auto peerId = quint64(0);
		auto rule = qint32(0);
		stream >> peerId >> rule;
		if (stream.status() != QDataStream::Ok) {
			return State();
		}
		result.rules.emplace(PeerId(peerId), PeerRule(rule));
	}
	stream >> queueCount;
	if (stream.status() != QDataStream::Ok || queueCount < 0) {
		return State();
	}
	for (auto i = 0; i != queueCount; ++i) {
		auto peerId = quint64(0);
		auto msgId = qint64(0);
		auto dueAt = qint32(0);
		auto stage = qint32(0);
		stream >> peerId >> msgId >> dueAt >> stage;
		if (stream.status() != QDataStream::Ok) {
			return State();
		}
		result.queue.push_back({
			.peerId = PeerId(peerId),
			.msgId = MsgId(msgId),
			.dueAt = TimeId(dueAt),
			.stage = Stage(stage),
		});
	}
	result.enabled = (enabled != 0);
	result.activatedAt = TimeId(activatedAt);
	return result;
}

void WriteState(not_null<Main::Session*> session, const State &state) {
	auto blob = QByteArray();
	auto stream = QDataStream(&blob, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_15);
	stream << kMagic
		<< kVersion
		<< qint32(state.enabled ? 1 : 0)
		<< state.periodHours
		<< state.replacement
		<< qint32(state.activatedAt)
		<< qint32(state.rules.size());
	for (const auto &[peerId, rule] : state.rules) {
		stream << quint64(peerId.value) << qint32(rule);
	}
	stream << qint32(state.queue.size());
	for (const auto &entry : state.queue) {
		stream << quint64(entry.peerId.value)
			<< qint64(entry.msgId.bare)
			<< qint32(entry.dueAt)
			<< qint32(entry.stage);
	}
	session->local().writePref<QByteArray>(kStateKey, blob);
}

class Runner final : public base::has_weak_ptr {
public:
	explicit Runner(not_null<Main::Session*> session);

	[[nodiscard]] const State &state() const {
		return _state;
	}
	void change(Fn<void(State&)> mutation);

	void track(not_null<HistoryItem*> item);
	void enqueueNow(const std::vector<FullMsgId> &ids);
	[[nodiscard]] TimeId dueAt(FullMsgId id) const;

private:
	void schedule();
	void tick();
	void process(const Entry &entry);
	void replace(not_null<HistoryItem*> item, const Entry &entry);
	void erase(const Entry &entry);
	void advance(const Entry &entry, Stage stage, TimeId dueAt);
	void drop(const Entry &entry);

	const not_null<Main::Session*> _session;
	State _state;
	base::Timer _timer;
	base::flat_set<FullMsgId> _busy;

};

Runner::Runner(not_null<Main::Session*> session)
: _session(session)
, _state(ReadState(session))
, _timer([=] { tick(); }) {
	if (!_state.activatedAt) {
		// Messages that already existed when the feature was switched on are
		// counted from that moment, not from when they were sent, so enabling
		// it never wipes a history at once.
		_state.activatedAt = base::unixtime::now();
		WriteState(_session, _state);
	}

	_session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		track(item);
	}, _session->lifetime());

	_session->data().itemIdChanged(
	) | rpl::on_next([=](const Data::Session::IdChange &change) {
		const auto i = ranges::find_if(_state.queue, [&](const Entry &e) {
			return (e.peerId == change.newId.peer)
				&& (e.msgId == change.oldId);
		});
		if (i != end(_state.queue)) {
			i->msgId = change.newId.msg;
			WriteState(_session, _state);
		}
	}, _session->lifetime());

	_timer.callOnce(kStartupDelay);
}

TimeId Runner::dueAt(FullMsgId id) const {
	const auto i = ranges::find_if(_state.queue, [&](const Entry &e) {
		return (e.peerId == id.peer) && (e.msgId == id.msg);
	});
	return (i != end(_state.queue)) ? i->dueAt : 0;
}

void Runner::change(Fn<void(State&)> mutation) {
	mutation(_state);
	WriteState(_session, _state);
	schedule();
}

void Runner::schedule() {
	if (_state.queue.empty()) {
		_timer.cancel();
		return;
	}
	// With a backlog the queue is polled far more often: Erase evidence can
	// hand over hundreds of messages at once, and a minute between batches
	// would stretch a manual command over hours.
	const auto now = base::unixtime::now();
	const auto due = ranges::any_of(_state.queue, [&](const Entry &entry) {
		return (entry.dueAt <= now);
	});
	_timer.callOnce(due ? kBusyInterval : kTickInterval);
}

void Runner::enqueueNow(const std::vector<FullMsgId> &ids) {
	const auto now = base::unixtime::now();
	auto added = false;
	for (const auto &id : ids) {
		const auto already = ranges::any_of(_state.queue, [&](const Entry &e) {
			return (e.peerId == id.peer) && (e.msgId == id.msg);
		});
		if (already) {
			continue;
		}
		_state.queue.push_back({
			.peerId = id.peer,
			.msgId = id.msg,
			.dueAt = now,
		});
		added = true;
	}
	if (added) {
		WriteState(_session, _state);
		schedule();
	}
}

void Runner::track(not_null<HistoryItem*> item) {
	// A freshly sent message still carries a client side identifier here, so
	// requiring isRegular() would silently skip every message the user sends
	// in this run. The entry is stored with whatever identifier exists and
	// itemIdChanged() replaces it once the server answers.
	if (!_state.enabled
		|| !item->out()
		|| item->isService()
		|| item->history()->peer->isSelf()) {
		return;
	}
	const auto peer = item->history()->peer;
	if (!AppliesTo(peer)) {
		return;
	}
	const auto id = item->fullId();
	const auto already = ranges::any_of(_state.queue, [&](const Entry &e) {
		return (e.peerId == id.peer) && (e.msgId == id.msg);
	});
	if (already) {
		return;
	}
	_state.queue.push_back({
		.peerId = id.peer,
		.msgId = id.msg,
		.dueAt = item->date() + _state.periodHours * 3600,
	});
	WriteState(_session, _state);
	schedule();
}

void Runner::tick() {
	const auto now = base::unixtime::now();
	auto handled = 0;
	auto due = std::vector<Entry>();
	for (const auto &entry : _state.queue) {
		if (entry.dueAt > now) {
			continue;
		} else if (_busy.contains(FullMsgId(entry.peerId, entry.msgId))) {
			continue;
		}
		due.push_back(entry);
		if (++handled == kPerTick) {
			break;
		}
	}
	for (const auto &entry : due) {
		process(entry);
	}
	schedule();
}

void Runner::process(const Entry &entry) {
	if (!IsServerMsgId(entry.msgId)) {
		// The send never completed, so there is nothing on the server to
		// remove and no identifier the server would understand.
		drop(entry);
		return;
	}
	const auto id = FullMsgId(entry.peerId, entry.msgId);
	const auto item = _session->data().message(id);
	if (entry.stage == Stage::Delete || !item) {
		// Without the item loaded there is nothing to edit, and deleting by
		// identifier still works, so the replacement step is simply skipped.
		erase(entry);
		return;
	} else if (!item->out() || !item->isRegular()) {
		drop(entry);
		return;
	} else if (item->media()
		|| item->emptyText()
		|| !item->allowsEdit(base::unixtime::now())) {
		// Only a plain text message that is still editable can be turned into
		// a dot. Editing a caption would leave the media itself in place, and
		// past the server edit window the request would just fail.
		erase(entry);
		return;
	}
	replace(item, entry);
}

void Runner::replace(not_null<HistoryItem*> item, const Entry &entry) {
	const auto id = FullMsgId(entry.peerId, entry.msgId);
	_busy.emplace(id);
	const auto weak = base::make_weak(this);
	const auto finish = [=](Stage stage, TimeId dueAt) {
		if (const auto strong = weak.get()) {
			strong->_busy.remove(id);
			strong->advance(entry, stage, dueAt);
		}
	};
	Api::EditTextMessage(
		item,
		TextWithEntities{ _state.replacement },
		Data::WebPageDraft{ .removed = true },
		Api::SendOptions(),
		[=](mtpRequestId) {
			finish(Stage::Delete, base::unixtime::now() + kReplaceToDeleteDelay);
		},
		[=](const QString &error, mtpRequestId) {
			// Every edit failure ends in a deletion anyway: the user chose
			// removal over masking when the message can no longer be edited.
			finish(Stage::Delete, base::unixtime::now());
		},
		false);
}

void Runner::erase(const Entry &entry) {
	const auto peer = _session->data().peerLoaded(entry.peerId);
	if (!peer) {
		drop(entry);
		return;
	}
	_session->data().histories().deleteMessages(
		{ FullMsgId(entry.peerId, entry.msgId) },
		true);
	drop(entry);
}

void Runner::advance(const Entry &entry, Stage stage, TimeId dueAt) {
	const auto i = ranges::find_if(_state.queue, [&](const Entry &e) {
		return (e.peerId == entry.peerId) && (e.msgId == entry.msgId);
	});
	if (i == end(_state.queue)) {
		return;
	}
	i->stage = stage;
	i->dueAt = dueAt;
	WriteState(_session, _state);
	schedule();
}

void Runner::drop(const Entry &entry) {
	const auto i = ranges::find_if(_state.queue, [&](const Entry &e) {
		return (e.peerId == entry.peerId) && (e.msgId == entry.msgId);
	});
	if (i == end(_state.queue)) {
		return;
	}
	_state.queue.erase(i);
	WriteState(_session, _state);
}

[[nodiscard]] base::flat_map<Main::Session*, std::unique_ptr<Runner>> &Map() {
	static auto result
		= base::flat_map<Main::Session*, std::unique_ptr<Runner>>();
	return result;
}

[[nodiscard]] Runner *Find(not_null<Main::Session*> session) {
	const auto i = Map().find(session.get());
	return (i != end(Map())) ? i->second.get() : nullptr;
}

[[nodiscard]] Runner &Get(not_null<Main::Session*> session) {
	if (const auto found = Find(session)) {
		return *found;
	}
	Start(session);
	return *Find(session);
}

} // namespace

bool Enabled(not_null<Main::Session*> session) {
	return Get(session).state().enabled;
}

void SetEnabled(not_null<Main::Session*> session, bool enabled) {
	Get(session).change([&](State &state) {
		state.enabled = enabled;
		if (enabled) {
			state.activatedAt = base::unixtime::now();
		}
	});
}

int PeriodHours(not_null<Main::Session*> session) {
	return Get(session).state().periodHours;
}

void SetPeriodHours(not_null<Main::Session*> session, int hours) {
	const auto clamped = std::clamp(
		hours,
		kAutoDeleteMinHours,
		kAutoDeleteMaxHours);
	Get(session).change([&](State &state) {
		state.periodHours = clamped;
	});
}

QString Replacement(not_null<Main::Session*> session) {
	return Get(session).state().replacement;
}

void SetReplacement(not_null<Main::Session*> session, const QString &text) {
	Get(session).change([&](State &state) {
		state.replacement = text.isEmpty() ? u"."_q : text;
	});
}

bool HasModerationRights(not_null<PeerData*> peer) {
	if (const auto channel = peer->asChannel()) {
		return channel->amCreator() || channel->hasAdminRights();
	} else if (const auto chat = peer->asChat()) {
		return chat->amCreator() || chat->hasAdminRights();
	}
	return false;
}

bool AppliesByDefault(not_null<PeerData*> peer) {
	// Where the account moderates, messages are more often part of running the
	// community than private correspondence, so the default is to keep them.
	return !HasModerationRights(peer);
}

PeerRule RuleFor(not_null<PeerData*> peer) {
	const auto &rules = Get(&peer->session()).state().rules;
	const auto i = rules.find(peer->id);
	return (i != end(rules)) ? i->second : PeerRule::Default;
}

void SetRuleFor(not_null<PeerData*> peer, PeerRule rule) {
	const auto id = peer->id;
	Get(&peer->session()).change([&](State &state) {
		if (rule == PeerRule::Default) {
			state.rules.remove(id);
		} else {
			state.rules[id] = rule;
		}
	});
}

bool AppliesTo(not_null<PeerData*> peer) {
	if (!Enabled(&peer->session())) {
		return false;
	}
	switch (RuleFor(peer)) {
	case PeerRule::Always: return true;
	case PeerRule::Never: return false;
	case PeerRule::Default: return AppliesByDefault(peer);
	}
	return false;
}

TimeId DueIn(not_null<HistoryItem*> item) {
	const auto session = &item->history()->session();
	const auto runner = Find(session);
	if (!runner) {
		return 0;
	}
	const auto dueAt = runner->dueAt(item->fullId());
	if (!dueAt) {
		return 0;
	}
	const auto left = dueAt - base::unixtime::now();
	return (left > 0) ? left : TimeId(1);
}

QString CountdownText(not_null<HistoryItem*> item) {
	const auto left = DueIn(item);
	if (!left) {
		return QString();
	}
	const auto russian = UseRussianTexts();
	const auto days = left / 86400;
	const auto hours = (left % 86400) / 3600;
	const auto minutes = (left % 3600) / 60;
	auto parts = QStringList();
	if (days > 0) {
		parts.push_back(QString::number(days) + (russian ? u" д"_q : u"d"_q));
	}
	if (days > 0 || hours > 0) {
		parts.push_back(QString::number(hours) + (russian ? u" ч"_q : u"h"_q));
	}
	parts.push_back(QString::number(minutes) + (russian ? u" м"_q : u"m"_q));
	return (russian ? u"Удалится через: "_q : u"Deletes in: "_q)
		+ parts.join(QChar(' '));
}

void EnqueueNow(
		not_null<Main::Session*> session,
		const std::vector<FullMsgId> &ids) {
	if (!ids.empty()) {
		Get(session).enqueueNow(ids);
	}
}

void Start(not_null<Main::Session*> session) {
	if (Find(session)) {
		return;
	}
	Map().emplace(session.get(), std::make_unique<Runner>(session));
	session->lifetime().add([raw = session.get()] {
		Map().remove(raw);
	});
}

QString SettingsTitle() {
	return UseRussianTexts()
		? u"Автоудаление своих сообщений"_q
		: u"Auto-delete own messages"_q;
}

QString FormatPeriod(int hours) {
	const auto russian = UseRussianTexts();
	if (hours % 24 == 0) {
		const auto days = hours / 24;
		if (days == 7) {
			return russian ? u"неделя"_q : u"1 week"_q;
		}
		return russian
			? (QString::number(days) + u" сут."_q)
			: (QString::number(days) + u" days"_q);
	}
	return russian
		? (QString::number(hours) + u" ч."_q)
		: (QString::number(hours) + u" h"_q);
}

QString SettingsLabel(not_null<Main::Session*> session) {
	if (!Enabled(session)) {
		return UseRussianTexts() ? u"выключено"_q : u"off"_q;
	}
	return FormatPeriod(PeriodHours(session));
}

QString PeerMenuText(not_null<PeerData*> peer) {
	const auto russian = UseRussianTexts();
	return AppliesTo(peer)
		? (russian
			? u"Не удалять мои сообщения здесь"_q
			: u"Keep my messages here"_q)
		: (russian
			? u"Удалять мои сообщения здесь"_q
			: u"Auto-delete my messages here"_q);
}

} // namespace NovaGram
