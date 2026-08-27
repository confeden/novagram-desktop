/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_autodelete.h"

#include "apiwrap.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_histories.h"
#include "data/data_peer.h"
#include "data/data_msg_id.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"
#include "novagram/nova_pin.h"
#include "storage/storage_account.h"
#include "ui/layers/show.h"
#include "window/window_controller.h"

#include <QtCore/QDataStream>

#include <algorithm>

namespace NovaGram {
namespace {

constexpr auto kMagic = quint32(0x4E564144);
// Version 2 added the per message erase flag and the Erase evidence reports.
constexpr auto kVersion = qint32(2);
constexpr auto kMinVersion = qint32(1);
constexpr auto kMaxReports = 8;
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

// A flood error that does not spell out the number of seconds still has to
// stop the queue for a while, otherwise the next tick walks into the same wall.
constexpr auto kUnknownFloodWait = TimeId(60);

enum class Stage : qint32 {
	Replace,
	Delete,
};

struct Entry {
	PeerId peerId = 0;
	MsgId msgId = 0;
	TimeId dueAt = 0;
	Stage stage = Stage::Replace;
	// Queued by Erase evidence, so the outcome belongs in that chat's report.
	bool erase = false;
};

// What one Erase evidence run did in one chat. It outlives the box that
// started it: the queue keeps working after the window is closed and after a
// restart, so the result has to be remembered until it can be shown.
struct Report {
	PeerId peerId = 0;
	qint32 queued = 0;
	qint32 replaced = 0;
	qint32 deleted = 0;
	qint32 skipped = 0;
	bool finished = false;
	bool shown = false;
};

struct State {
	bool enabled = true;
	qint32 periodHours = kAutoDeleteDefaultHours;
	QString replacement = u"."_q;
	TimeId activatedAt = 0;
	base::flat_map<PeerId, PeerRule> rules;
	std::vector<Entry> queue;
	std::vector<Report> reports;
};

// The state rides in the account key-value store, which already lives in an
// encrypted blob with a randomized file name. A separate file of our own would
// have announced the feature by its name alone.
constexpr auto kStateKey = "novagram_autodelete"_cs;

// FLOOD_WAIT_x names the number of seconds the server wants us to stay quiet.
// Without this the wait would be read as "this message cannot be edited" and
// the queue would delete it on the spot, which is the opposite of what the
// server asked for.
[[nodiscard]] TimeId FloodWaitSeconds(const QString &error) {
	if (!MTP::IsFloodError(error)) {
		return 0;
	}
	auto ok = false;
	const auto tail = error.mid(error.lastIndexOf(QChar('_')) + 1);
	const auto seconds = tail.toInt(&ok);
	return (ok && seconds > 0) ? TimeId(seconds) : kUnknownFloodWait;
}

// A refusal that says something about the message itself and will read the
// same way tomorrow. Everything outside this list is treated as temporary and
// the entry is kept, because a message left on the server is the one failure
// this queue must not produce quietly.
[[nodiscard]] bool FinalDeleteError(const QString &error) {
	return (error == u"MESSAGE_ID_INVALID"_q)
		|| (error == u"MESSAGE_DELETE_FORBIDDEN"_q)
		|| (error == u"MESSAGE_AUTHOR_REQUIRED"_q)
		|| (error == u"CHAT_ADMIN_REQUIRED"_q)
		|| (error == u"CHANNEL_INVALID"_q)
		|| (error == u"CHANNEL_PRIVATE"_q)
		|| (error == u"PEER_ID_INVALID"_q)
		|| (error == u"USER_BANNED_IN_CHANNEL"_q);
}

[[nodiscard]] QString ReportText(int deleted, int replaced, int skipped) {
	return UseRussianTexts()
		? u"Erase evidence: удалено %1, из них с заменой на точку %2, "
			"пропущено %3."_q.arg(deleted).arg(replaced).arg(skipped)
		: u"Erase evidence: deleted %1, of them replaced with a dot first %2, "
			"skipped %3."_q.arg(deleted).arg(replaced).arg(skipped);
}

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
		|| version < kMinVersion
		|| version > kVersion) {
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
		auto erase = qint32(0);
		stream >> peerId >> msgId >> dueAt >> stage;
		if (version >= 2) {
			stream >> erase;
		}
		if (stream.status() != QDataStream::Ok) {
			return State();
		}
		result.queue.push_back({
			.peerId = PeerId(peerId),
			.msgId = MsgId(msgId),
			.dueAt = TimeId(dueAt),
			.stage = Stage(stage),
			.erase = (erase != 0),
		});
	}
	if (version >= 2) {
		auto reportCount = qint32(0);
		stream >> reportCount;
		if (stream.status() != QDataStream::Ok
			|| reportCount < 0
			|| reportCount > kMaxReports) {
			return State();
		}
		for (auto i = 0; i != reportCount; ++i) {
			auto peerId = quint64(0);
			auto report = Report();
			auto finished = qint32(0);
			auto shown = qint32(0);
			stream >> peerId
				>> report.queued
				>> report.replaced
				>> report.deleted
				>> report.skipped
				>> finished
				>> shown;
			if (stream.status() != QDataStream::Ok
				|| report.queued < 0
				|| report.replaced < 0
				|| report.deleted < 0
				|| report.skipped < 0) {
				return State();
			}
			report.peerId = PeerId(peerId);
			report.finished = (finished != 0);
			report.shown = (shown != 0);
			result.reports.push_back(report);
		}
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
			<< qint32(entry.stage)
			<< qint32(entry.erase ? 1 : 0);
	}
	stream << qint32(state.reports.size());
	for (const auto &report : state.reports) {
		stream << quint64(report.peerId.value)
			<< report.queued
			<< report.replaced
			<< report.deleted
			<< report.skipped
			<< qint32(report.finished ? 1 : 0)
			<< qint32(report.shown ? 1 : 0);
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
	enum class Outcome {
		Replaced,
		Deleted,
		Skipped,
	};

	void schedule();
	void tick();
	void process(const Entry &entry);
	void replace(not_null<HistoryItem*> item, const Entry &entry);
	void replaceFailed(const Entry &entry, const QString &error);
	void erase(const Entry &entry);
	void erased(const Entry &entry);
	void eraseFailed(const Entry &entry, const QString &error);
	void floodWait(Stage stage, TimeId seconds);
	[[nodiscard]] TimeId floodUntil(Stage stage) const;
	void advance(const Entry &entry, Stage stage, TimeId dueAt);
	void drop(const Entry &entry);
	void count(const Entry &entry, Outcome outcome);
	void checkReportFinished(PeerId peerId);
	void showPendingReports();

	const not_null<Main::Session*> _session;
	State _state;
	base::Timer _timer;
	base::flat_set<FullMsgId> _busy;
	// One deadline per step, because messages.editMessage is limited far more
	// tightly than messages.deleteMessages: a wait earned by the cosmetic
	// replacement must not hold back the deletions, which are the part the
	// user actually asked for. Not stored with the rest of the state: a wait
	// the server named is only meaningful while this run lasts, and a restart
	// takes longer than most.
	TimeId _floodUntilReplace = 0;
	TimeId _floodUntilDelete = 0;

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
		const auto waiting = ranges::any_of(_state.reports, [](const Report &r) {
			return r.finished && !r.shown;
		});
		if (waiting) {
			// Keep ticking until the report has somewhere to be shown.
			_timer.callOnce(kTickInterval);
		} else {
			_timer.cancel();
		}
		return;
	}
	const auto now = base::unixtime::now();
	// With a backlog the queue is polled far more often: Erase evidence can
	// hand over hundreds of messages at once, and a minute between batches
	// would stretch a manual command over hours.
	const auto ready = [&](const Entry &entry) {
		return (entry.dueAt <= now) && (floodUntil(entry.stage) <= now);
	};
	if (ranges::any_of(_state.queue, ready)) {
		_timer.callOnce(kBusyInterval);
		return;
	}
	// Nothing can go right now. If the only thing in the way is a flood wait,
	// wake up when the server says it ends; the extra interval keeps the tick
	// from firing a moment too early.
	auto wakeAt = TimeId(0);
	for (const auto &entry : _state.queue) {
		if (entry.dueAt > now) {
			continue;
		}
		const auto until = floodUntil(entry.stage);
		if (!wakeAt || until < wakeAt) {
			wakeAt = until;
		}
	}
	const auto delay = wakeAt
		? (crl::time(wakeAt - now) * 1000 + kBusyInterval)
		: kTickInterval;
	_timer.callOnce(std::min(delay, kTickInterval));
}

void Runner::enqueueNow(const std::vector<FullMsgId> &ids) {
	if (ids.empty()) {
		return;
	}
	const auto now = base::unixtime::now();
	const auto peerId = ids.front().peer;

	// One report per chat, replacing whatever an earlier run left there.
	_state.reports.erase(
		ranges::remove(_state.reports, peerId, &Report::peerId),
		end(_state.reports));
	while (_state.reports.size() >= kMaxReports) {
		_state.reports.erase(begin(_state.reports));
	}
	_state.reports.push_back({ .peerId = peerId });
	auto &report = _state.reports.back();

	for (const auto &id : ids) {
		++report.queued;
		const auto i = ranges::find_if(_state.queue, [&](const Entry &e) {
			return (e.peerId == id.peer) && (e.msgId == id.msg);
		});
		if (i != end(_state.queue)) {
			// An entry already waiting for its ordinary period is moved to now
			// and joins this run: Erase evidence is an order, not a policy.
			i->dueAt = now;
			i->erase = true;
			continue;
		}
		_state.queue.push_back({
			.peerId = id.peer,
			.msgId = id.msg,
			.dueAt = now,
			.erase = true,
		});
	}
	WriteState(_session, _state);
	schedule();
}

void Runner::count(const Entry &entry, Outcome outcome) {
	if (!entry.erase) {
		return;
	}
	const auto i = ranges::find(_state.reports, entry.peerId, &Report::peerId);
	if (i == end(_state.reports)) {
		return;
	}
	switch (outcome) {
	case Outcome::Replaced: ++i->replaced; break;
	case Outcome::Deleted: ++i->deleted; break;
	case Outcome::Skipped: ++i->skipped; break;
	}
}

void Runner::checkReportFinished(PeerId peerId) {
	const auto i = ranges::find(_state.reports, peerId, &Report::peerId);
	if (i == end(_state.reports) || i->finished) {
		return;
	}
	const auto left = ranges::any_of(_state.queue, [&](const Entry &entry) {
		return entry.erase && (entry.peerId == peerId);
	});
	if (left) {
		return;
	}
	i->finished = true;
	WriteState(_session, _state);
	showPendingReports();
}

void Runner::showPendingReports() {
	const auto window = Core::App().activePrimaryWindow();
	if (!window
		|| !window->widget()->isVisible()
		|| window->widget()->isMinimized()
		|| window->locked()) {
		// The last active window is remembered until it is destroyed, so a
		// pointer proves nothing about anyone seeing a toast: minimized to the
		// tray it goes to a hidden widget, and behind the lock screen it would
		// name the feature to whoever is holding the machine. The report keeps
		// waiting and the next tick tries again, so it is not lost.
		return;
	}
	const auto show = window->uiShow();
	auto changed = false;
	for (auto &report : _state.reports) {
		if (!report.finished || report.shown) {
			continue;
		}
		report.shown = true;
		changed = true;
		show->showToast(ReportText(report.deleted, report.replaced, report.skipped));
	}
	if (changed) {
		WriteState(_session, _state);
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
	// A run that finished while the window was closed shows its result here,
	// on the first tick that finds a window again.
	showPendingReports();

	const auto now = base::unixtime::now();
	auto handled = 0;
	auto due = std::vector<Entry>();
	for (const auto &entry : _state.queue) {
		if (entry.dueAt > now) {
			continue;
		} else if (floodUntil(entry.stage) > now) {
			// Only the step that ran into the wall waits it out; the other
			// one keeps working.
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
	if (!entry.erase && entry.stage != Stage::Delete) {
		// The rules are read again here, not only when the message was sent.
		// Switching auto delete off, choosing "do not delete here", or being
		// made an administrator of the group has to spare what is already
		// waiting in the queue, otherwise the menu says one thing and the
		// queue does another. An Erase evidence entry is a direct order and
		// is not subject to any of this.
		//
		// An entry that already reached Stage::Delete is not spared either:
		// its text has been replaced by the placeholder, so the content is
		// gone whatever happens now, and the choice is between removing the
		// message and leaving a lone dot in the conversation for ever. The
		// dot is the worse trace of the two. Android decides this the same
		// way.
		const auto peer = _session->data().peerLoaded(entry.peerId);
		if (!_state.enabled || (peer && !AppliesTo(peer))) {
			drop(entry);
			return;
		}
	}
	if (!IsServerMsgId(entry.msgId)) {
		// The send never completed, so there is nothing on the server to
		// remove and no identifier the server would understand.
		count(entry, Outcome::Skipped);
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
		count(entry, Outcome::Skipped);
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
	const auto api = &_session->api();

	// The request is built here instead of going through Api::EditTextMessage
	// only because of handleFloodErrors(): without it mtproto swallows
	// FLOOD_WAIT_x, silently re-sends the edit on its own schedule and never
	// tells the queue to stop, so a long Erase evidence run keeps piling up
	// deferred edits that all fire at once when the wait ends. Everything the
	// shared helper would add here is inapplicable: process() has already
	// established that the message is a plain outgoing text one, so there is
	// no media, no entities in the replacement, and no scheduled or quick
	// reply identifier to look up.
	using Flag = MTPmessages_EditMessage::Flag;
	api->request(MTPmessages_EditMessage(
		MTP_flags(Flag::f_message | Flag::f_no_webpage),
		item->history()->peer->input(),
		MTP_int(entry.msgId),
		MTP_string(_state.replacement),
		MTPInputMedia(),
		MTPReplyMarkup(),
		MTPVector<MTPMessageEntity>(),
		MTP_int(0), // schedule_date
		MTP_int(0), // schedule_repeat_period
		MTP_int(0), // quick_reply_shortcut_id
		MTPInputRichMessage()
	)).done([=](const MTPUpdates &result) {
		api->applyUpdates(result);
		if (const auto strong = weak.get()) {
			strong->_busy.remove(id);
			strong->count(entry, Outcome::Replaced);
			strong->advance(
				entry,
				Stage::Delete,
				base::unixtime::now() + kReplaceToDeleteDelay);
		}
	}).fail([=](const MTP::Error &error) {
		if (const auto strong = weak.get()) {
			strong->_busy.remove(id);
			strong->replaceFailed(entry, error.type());
		}
	}).handleFloodErrors().send();
}

void Runner::replaceFailed(const Entry &entry, const QString &error) {
	if (const auto seconds = FloodWaitSeconds(error)) {
		// Deleting right now would walk into the same wait, and the point of
		// the step is that the dot is seen before the message goes, so the
		// message keeps its place in the queue until the server allows it.
		floodWait(Stage::Replace, seconds);
		advance(entry, Stage::Replace, _floodUntilReplace);
		return;
	}
	// Every other edit failure ends in a deletion anyway: the user chose
	// removal over masking when the message can no longer be edited.
	advance(entry, Stage::Delete, base::unixtime::now());
}

void Runner::floodWait(Stage stage, TimeId seconds) {
	const auto until = base::unixtime::now() + seconds;
	auto &field = (stage == Stage::Replace)
		? _floodUntilReplace
		: _floodUntilDelete;
	if (field < until) {
		field = until;
	}
}

TimeId Runner::floodUntil(Stage stage) const {
	return (stage == Stage::Replace) ? _floodUntilReplace : _floodUntilDelete;
}

void Runner::erase(const Entry &entry) {
	const auto peer = _session->data().peerLoaded(entry.peerId);
	if (!peer) {
		// tdesktop keeps no peers on disk, so right after a start - and for a
		// chat that has not come down with the dialog list yet - there is
		// nothing here to build the request from. Dropping the entry would
		// leave the message on the server forever, which is the one outcome
		// this queue exists to prevent, so it simply waits for the peer.
		advance(entry, entry.stage, base::unixtime::now() + kRetryDelay);
		return;
	}
	const auto id = FullMsgId(entry.peerId, entry.msgId);
	const auto history = _session->data().history(entry.peerId);
	const auto channel = peer->asChannel();
	const auto api = &_session->api();
	const auto weak = base::make_weak(this);
	const auto ids = QVector<MTPint>{ MTP_int(entry.msgId) };

	// Histories::deleteMessages() over a MessageIdsList only touches messages
	// that are currently loaded: it looks every identifier up in the session
	// data and quietly drops the ones it cannot find. Most of the queue is
	// older than the loaded part of its history, so that path removed the
	// entry and left the message on the server forever. The request is sent
	// by identifier instead, which needs nothing loaded, and the entry stays
	// in the queue until the server has confirmed the deletion.
	_busy.emplace(id);
	_session->data().histories().sendRequest(
		history,
		Data::Histories::RequestType::Delete,
		[=](Fn<void()> finish) {
			const auto done = [=](
					const MTPmessages_AffectedMessages &result) {
				api->applyAffectedMessages(history->peer, result);
				finish();
				if (const auto strong = weak.get()) {
					strong->_busy.remove(id);
					strong->erased(entry);
				}
			};
			const auto fail = [=](const MTP::Error &error) {
				finish();
				if (const auto strong = weak.get()) {
					strong->_busy.remove(id);
					strong->eraseFailed(entry, error.type());
				}
			};
			if (channel) {
				return api->request(MTPchannels_DeleteMessages(
					channel->inputChannel(),
					MTP_vector<MTPint>(ids)
				)).done(done).fail(fail).handleFloodErrors().send();
			}
			return api->request(MTPmessages_DeleteMessages(
				MTP_flags(MTPmessages_DeleteMessages::Flag::f_revoke),
				MTP_vector<MTPint>(ids)
			)).done(done).fail(fail).handleFloodErrors().send();
		});
}

void Runner::erased(const Entry &entry) {
	const auto id = FullMsgId(entry.peerId, entry.msgId);
	if (const auto item = _session->data().message(id)) {
		// The server sends no update about a message the account deleted
		// itself, so a copy that happens to be loaded is dropped by hand, the
		// way Histories::deleteMessages() does it for the loaded case.
		const auto history = item->history();
		const auto wasLast = (history->lastMessage() == item);
		const auto wasInChats = (history->chatListMessage() == item);
		auto destroyed = std::vector<not_null<HistoryItem*>>();
		destroyed.push_back(item);
		_session->data().notifyItemsAboutToBeDestroyed(destroyed);
		item->destroy();
		if (wasLast || wasInChats) {
			history->requestChatListMessage();
		}
	}
	count(entry, Outcome::Deleted);
	drop(entry);
}

void Runner::eraseFailed(const Entry &entry, const QString &error) {
	if (const auto seconds = FloodWaitSeconds(error)) {
		floodWait(Stage::Delete, seconds);
		advance(entry, Stage::Delete, _floodUntilDelete);
		return;
	} else if (!FinalDeleteError(error)) {
		// mtproto handles timeouts and 5xx on its own, but the failures it
		// invents itself - a broken or empty answer, a dropped connection -
		// carry code 0, miss IsDefaultHandledError() and land right here.
		// They say nothing about the message, so it keeps its place.
		advance(entry, Stage::Delete, base::unixtime::now() + kRetryDelay);
		return;
	}
	count(entry, Outcome::Skipped);
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
	const auto wasErase = i->erase;
	const auto peerId = entry.peerId;
	_state.queue.erase(i);
	WriteState(_session, _state);
	if (wasErase) {
		checkReportFinished(peerId);
	}
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

QString PeerMenuText(not_null<PeerData*> peer, bool wholeGroup) {
	const auto russian = UseRussianTexts();
	// A rule is stored per chat, and a room is not a chat of its own: it shares
	// the identifier of the group it lives in. So inside a room the entry does
	// exactly what it does in the group, and has to say so - "here" would read
	// as "in this room" and would be false for every other room next to it.
	if (wholeGroup) {
		return AppliesTo(peer)
			? (russian
				? u"Не удалять мои сообщения в этой группе"_q
				: u"Keep my messages in this group"_q)
			: (russian
				? u"Удалять мои сообщения в этой группе"_q
				: u"Auto-delete my messages in this group"_q);
	}
	return AppliesTo(peer)
		? (russian
			? u"Не удалять мои сообщения здесь"_q
			: u"Keep my messages here"_q)
		: (russian
			? u"Удалять мои сообщения здесь"_q
			: u"Auto-delete my messages here"_q);
}

} // namespace NovaGram
