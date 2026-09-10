/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_read_status.h"

#include "apiwrap.h"
#include "chat_helpers/compose/compose_show.h"
#include "data/data_changes.h"
#include "data/data_folder.h"
#include "data/data_histories.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_entry.h"
#include "dialogs/dialogs_main_list.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_contact_status.h"
#include "main/main_session.h"
#include "novagram/nova_decoy.h"
#include "novagram/nova_drop_incoming.h"
#include "novagram/nova_pin.h"
#include "storage/storage_account.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDataStream>

namespace NovaGram {
namespace {

constexpr auto kMagic = quint32(0x4E565253);
// Version 2 names the account the rules belong to, version 3 remembers that
// the dialogs which were already there have been written down, version 4 also
// remembers how old they were, version 5 keeps the read position held back in
// each hidden dialog, version 6 the ranges in which a dialog drops what the
// other side sends, version 7 tells a Revealed the client was told from one it
// only assumed. Blobs written by the older versions are still read as they
// are: dropping them would send exactly the receipts the user was promised
// would not be sent, and a rule made by the older, too eager logic can be
// removed per dialog from the note above the chat.
constexpr auto kVersion = qint32(7);
constexpr auto kMinVersion = qint32(1);
constexpr auto kStateKey = "novagram_read_status"_cs;

// Spells of dropping remembered per dialog. Four is already generous - the
// switch goes off by itself as soon as the user answers - and when there are
// more the two oldest are merged into one span rather than forgotten, because
// forgetting a range hands back what was thrown away.
constexpr auto kDropRangesLimit = 4;

// The date written down when the baseline was taken over a chat list that held
// nothing datable at all. Nothing is older than it, so no dialog is taken for
// an old one by its date alone; that is safe only because the list it was taken
// over was empty.
constexpr auto kBaselineDateUnknown = TimeId(1);

enum class Rule : qint32 {
	Hidden,
	Revealed,
	// Written down by the sweep over the dialogs that were already there,
	// while the one thing that could have told them apart - the bar the server
	// offers over a stranger who wrote first - had not been asked for. It
	// behaves like a Revealed that is not final: the gate withholds, and the
	// first real answer about that peer turns it into one of the two above.
	//
	// Its whole reason: the sweep runs when the chat list arrives, and at that
	// moment barSettings() is unknown for every peer, because the desktop only
	// asks for it when a conversation is opened. So the sweep used to write
	// Revealed over every dialog the account already had - including the ones
	// a stranger had started and the user never answered, which are exactly
	// the ones the feature is for.
	Assumed,
};

struct State {
	bool enabled = true;

	// The date of the oldest dialog that was there when the dialogs the
	// account already had were written down. Zero until that is done, and
	// until then a personal dialog without a rule says nothing about who
	// started it. Afterwards a dialog whose last message is not newer than
	// this one was there before, whichever page of the chat list finally
	// brings it.
	TimeId baselineDate = 0;

	base::flat_map<PeerId, Rule> rules;

	// How far the user has read in a dialog that is hiding its receipts. Kept
	// here and not in History, because History takes its read position from
	// the server at every start and the server was never told this one.
	base::flat_map<PeerId, MsgId> heldReadTill;

	// The spells of dropping everything the other side sends, per dialog.
	// Only ever written for a dialog whose rule is Hidden, and thrown away
	// with it.
	base::flat_map<PeerId, std::vector<DropRange>> dropRanges;

	[[nodiscard]] bool baselineTaken() const {
		return (baselineDate != 0);
	}
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
		|| version < kMinVersion
		|| version > kVersion
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
	if (version < 7) {
		// Every Revealed in an older blob may be an assumption: the sweep
		// wrote them without the answer, and a real reveal - an answer sent,
		// a reaction, the button of the note - wrote the same value. Asking
		// again costs one messages.getPeerSettings for each dialog the user
		// touches and settles both cases correctly, because a dialog the user
		// has written in shows no stranger bar. A Hidden is never re-asked:
		// taking one back would send the receipts it held.
		for (auto &[peerId, rule] : result.rules) {
			if (rule == Rule::Revealed) {
				rule = Rule::Assumed;
			}
		}
	}
	if (version >= 2) {
		auto ownerId = quint64(0);
		stream >> ownerId;
		if (stream.status() != QDataStream::Ok
			|| ownerId != quint64(session->userId().bare)) {
			// The account key value store keeps its map in memory across a
			// logout, so the previous account's blob can still be there when
			// the next one starts. A rule names a peer of one account only.
			return State();
		}
	}
	if (version >= 3) {
		auto baselineDate = qint32(0);
		stream >> baselineDate;
		if (stream.status() != QDataStream::Ok || baselineDate < 0) {
			return State();
		}
		// Version 3 wrote a flag in this place rather than a date, and a flag
		// cannot say which dialogs were old enough to be written down, so its
		// baseline is taken again. The rules it made are kept as they are:
		// taking the baseline only ever adds rules where there is none.
		result.baselineDate = (version == 3) ? TimeId(0) : TimeId(baselineDate);
	}
	if (version >= 5) {
		auto held = qint32(0);
		stream >> held;
		if (stream.status() != QDataStream::Ok || held < 0) {
			return State();
		}
		for (auto i = 0; i != held; ++i) {
			auto peerId = quint64(0);
			auto tillId = qint64(0);
			stream >> peerId >> tillId;
			if (stream.status() != QDataStream::Ok || tillId <= 0) {
				return State();
			}
			result.heldReadTill.emplace(PeerId(peerId), MsgId(tillId));
		}
	}
	if (version >= 6) {
		auto dialogs = qint32(0);
		stream >> dialogs;
		if (stream.status() != QDataStream::Ok || dialogs < 0) {
			return State();
		}
		for (auto i = 0; i != dialogs; ++i) {
			auto peerId = quint64(0);
			auto count = qint32(0);
			stream >> peerId >> count;
			if (stream.status() != QDataStream::Ok
				|| count <= 0
				|| count > kDropRangesLimit) {
				return State();
			}
			auto ranges = std::vector<DropRange>();
			ranges.reserve(count);
			for (auto j = 0; j != count; ++j) {
				auto from = qint64(0);
				auto till = qint64(0);
				auto open = qint32(0);
				stream >> from >> till >> open;
				if (stream.status() != QDataStream::Ok || from <= 0) {
					return State();
				}
				ranges.push_back({ MsgId(from), MsgId(till), (open != 0) });
			}
			result.dropRanges.emplace(PeerId(peerId), std::move(ranges));
		}
	}
	// A blob of version 1 or 2 keeps its rules and leaves the baseline
	// untaken, so the dialogs that are there when the chat list next arrives
	// are written down as older ones, and the rules made before stay as they
	// are.
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
	stream << quint64(session->userId().bare)
		<< qint32(state.baselineDate)
		<< qint32(state.heldReadTill.size());
	for (const auto &[peerId, tillId] : state.heldReadTill) {
		stream << quint64(peerId.value) << qint64(tillId.bare);
	}
	stream << qint32(state.dropRanges.size());
	for (const auto &[peerId, ranges] : state.dropRanges) {
		stream << quint64(peerId.value) << qint32(ranges.size());
		for (const auto &range : ranges) {
			stream << qint64(range.from.bare)
				<< qint64(range.till.bare)
				<< qint32(range.open ? 1 : 0);
		}
	}
	session->local().writePref<QByteArray>(kStateKey, blob);
}

// Dialogs a rule could never apply to, whatever happens in them later. Bots
// and service accounts never show read marks of their own, so a rule here
// would stay forever and hide nothing, and Saved Messages is the user talking
// to themselves.
[[nodiscard]] bool NeverHiddenPeer(not_null<PeerData*> peer) {
	const auto user = peer->asUser();
	return !user
		|| user->isSelf()
		|| user->isBot()
		|| user->isSupport()
		|| user->isServiceUser();
}

// Telegram has no field for "who started this", but while a stranger has
// written and the user has not answered the server offers the bar that reports
// or blocks them. It is not asked for any more - a dialog that was not there
// before answers the question by itself - yet when the answer is already at
// hand it still recognises a dialog that arrived with a synchronisation
// instead of with a live message.
[[nodiscard]] bool StrangerBarShown(not_null<PeerData*> peer) {
	if (NeverHiddenPeer(peer)) {
		return false;
	}
	using Flag = PeerBarSetting;
	const auto settings = peer->barSettings();
	return settings
		&& !!((*settings)
			& (Flag::ReportSpam | Flag::BlockContact | Flag::AddContact));
}

// What to write down about a dialog that was there before this client started
// counting. Assumed while the bar has not been asked for, which on the desktop
// is the normal state of every peer until its conversation is opened - the
// answer is fetched later, once, for the dialogs the user actually touches.
[[nodiscard]] Rule RuleFromBar(not_null<PeerData*> peer) {
	return !peer->barSettings()
		? Rule::Assumed
		: StrangerBarShown(peer)
		? Rule::Hidden
		: Rule::Revealed;
}

class Watcher final {
public:
	explicit Watcher(not_null<Main::Session*> session);

	[[nodiscard]] const State &state() const {
		return _state;
	}
	void change(Fn<void(State&)> mutation);

	// Not done in the constructor: writing the first rules fires events, and
	// whoever hears them looks the watcher up, so it has to be findable first.
	void start();

	// Assumed counts as undecided: the receipt is held while the answer is
	// fetched, which is the direction that can be taken back (I2). Held, not
	// lost - resolveAssumed() fires the update stream, and whoever postponed
	// the receipt sends it then.
	[[nodiscard]] bool undecided(PeerId peerId) const {
		const auto i = _state.rules.find(peerId);
		return _pending.contains(peerId)
			|| (i != end(_state.rules) && i->second == Rule::Assumed);
	}

	[[nodiscard]] rpl::producer<> updates() const {
		return _updates.events();
	}

	// The conversation is being deleted, so what was decided about it goes
	// with it and the next first message decides again.
	void forget(not_null<PeerData*> peer);

	// Stops hiding in this dialog for good and sends out the receipt that was
	// held back. Both ways in end here - the button of the note above the chat
	// and an answer written in the dialog - so that the two behave alike.
	void reveal(not_null<PeerData*> peer);
	[[nodiscard]] std::vector<DropRange> dropRanges(PeerId peerId) const;
	void setDropRanges(PeerId peerId, std::vector<DropRange> ranges);
	void closeDropRange(PeerId peerId);
	void noteDropped(PeerId peerId, MsgId id);

	void noteHeldRead(PeerId peerId, MsgId tillId);

	[[nodiscard]] MsgId heldReadTill(PeerId peerId) const;

	// Settles a rule the sweep only assumed, if the answer is at hand, and
	// asks for it once if it is not. Public because the gate calls it: the
	// dialogs the user opens are exactly the ones worth a request, and
	// asking for all of them when the chat list arrives would be a burst of
	// requests about conversations nobody looked at.
	void resolveAssumed(not_null<PeerData*> peer);

private:
	void takeBaseline();
	void revealOlderThanBaseline();
	void flushPending();
	void note(not_null<HistoryItem*> item);
	void decide(not_null<PeerData*> peer);
	void askBar(not_null<PeerData*> peer);

	const not_null<Main::Session*> _session;
	State _state;

	// Dialogs waiting for an answer: either the dialogs that were already
	// there have not been written down yet, or the dialog record of this one
	// has not arrived. Nothing is known about them, and a receipt cannot be
	// recalled, so the gate stays closed while they are in here.
	base::flat_set<PeerId> _pending;

	// Peers whose stranger bar has already been asked for in this run, so that
	// a gate consulted on every repaint asks the server once and not once per
	// frame. ApiWrap keeps its own guard as well; this one also keeps the
	// subscription below from being made twice.
	base::flat_set<PeerId> _barAsked;

	// Size of the chat list the last catching up saw. The list changes on
	// every little thing, and walking all of it each time would cost for
	// nothing: only a longer list can hold dialogs that were not seen yet.
	int _sweptCount = -1;

	rpl::event_stream<> _updates;
	rpl::lifetime _lifetime;

};

Watcher::Watcher(not_null<Main::Session*> session)
: _session(session)
, _state(ReadState(session)) {
	_session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		note(item);
	}, _lifetime);

	// A folder keeps a list of its own and is loaded separately, later, so
	// every arrival is another chance to write down dialogs that were there
	// before this client started counting.
	_session->data().chatsListLoadedEvents(
	) | rpl::on_next([=] {
		takeBaseline();
	}, _lifetime);

	// The chat list arrives a page at a time, and the baseline is taken only
	// when the last page is there, so every page is another chance for it.
	// Receipts are held while it loads, which is the cost of not guessing.
	_session->data().chatsListChanges(
	) | rpl::on_next([=] {
		takeBaseline();
	}, _lifetime);

	// The answer a dialog waiting for its record was waiting for: the folder
	// it belongs to and how far the other side has read the user arrive
	// together, with the record itself.
	_session->changes().historyUpdates(
		Data::HistoryUpdate::Flag::Folder
		| Data::HistoryUpdate::Flag::OutboxRead
	) | rpl::on_next([=](const Data::HistoryUpdate &update) {
		const auto history = update.history;
		if (!_state.baselineTaken()
			|| !history->folderKnown()
			|| !_pending.contains(history->peer->id)) {
			return;
		}
		_pending.remove(history->peer->id);
		decide(history->peer);
		// The gate was held closed while this one waited, so whoever
		// postponed a receipt has to learn that it may go now.
		_updates.fire({});
	}, _lifetime);
}

void Watcher::change(Fn<void(State&)> mutation) {
	mutation(_state);
	WriteState(_session, _state);
	_updates.fire({});
}

void Watcher::start() {
	// The chat list can already be loaded when the watcher starts, and then no
	// event about it is coming.
	takeBaseline();
}

// Writes down, once in the life of the account, every personal dialog that is
// already there, so that from this moment on a personal dialog without a rule
// is one that the other side has started. The list of chats is the only place
// on the desktop where this can be asked: History::setLastMessage puts a dialog
// into the list before newItemAdded reports the message, so by the time a
// message is seen its dialog is already indistinguishable from an old one.
// It waits for the whole list - the main one and the archive, which is loaded
// on its own - because a dialog that is missing from it gets no rule here and
// there is nothing else on the desktop to ask afterwards: the local copy of a
// conversation holds no messages until it is opened, and the read markers the
// server keeps survive deleting the conversation, which is the very case this
// feature exists for.
void Watcher::takeBaseline() {
	if (Decoy::Active()) {
		// The decoy has to behave like a plain client that never heard of the
		// feature, so it writes nothing down.
		return;
	}
	const auto owner = &_session->data();
	if (_state.baselineTaken()) {
		// Written down once and for good. A second sweep would happen on every
		// start of the application, and every dialog that the other side has
		// started since would be written down by it as one that was already
		// there - including the ones that arrived with a synchronisation
		// instead of with a live message, which never pass through note().
		revealOlderThanBaseline();
		flushPending();
		return;
	}
	if (!owner->chatsListLoaded(nullptr)) {
		// The first page is not enough. The list is sorted by date, but a
		// pinned dialog can be older than everything below it, so the date of
		// the oldest row of a page says nothing about the pages that follow,
		// and every dialog they hold would be left without a rule and taken
		// for a new one later. Whatever waits for the answer waits in
		// _pending, and its receipts are held meanwhile.
		return;
	}
	const auto folder = owner->folderLoaded(Data::Folder::kId);
	if (folder && !owner->chatsListLoaded(folder)) {
		// The archive keeps a list of its own and is asked for only when the
		// user opens it - one page of twenty arrives with the account and
		// nothing more. Without asking for the rest, every archived dialog
		// from the twenty-first down would have no rule, and the first message
		// to arrive in one would silence it for good.
		_session->api().requestDialogs(folder);
		return;
	}
	auto older = std::vector<std::pair<PeerId, Rule>>();
	auto oldest = TimeId(0);
	const auto collect = [&](not_null<Dialogs::MainList*> list) {
		for (const auto &row : list->indexed()->all()) {
			const auto history = row->entry()->asHistory();
			if (!history
				|| !history->folderKnown()
				|| _pending.contains(history->peer->id)) {
				// Only the dialogs the server has told about say what the
				// account had before. A dialog that is here because a message
				// has just created it says the opposite, and its own record
				// has not even arrived yet.
				continue;
			}
			const auto date = history->chatListTimeId();
			if (date > 0 && (!oldest || date < oldest)) {
				oldest = date;
			}
			const auto peer = history->peer;
			if (NeverHiddenPeer(peer) || _state.rules.contains(peer->id)) {
				// A rule that is already there is never rewritten: taking a
				// Hidden back would send exactly the receipts that were held.
				continue;
			}
			older.push_back({ peer->id, RuleFromBar(peer) });
		}
	};
	collect(owner->chatsList());
	if (folder) {
		collect(folder->chatsList());
	}
	// The date of the oldest dialog of the whole list, not the clock: it is
	// what a row arriving later - a dialog moved out of the archive, a page of
	// a list that was reloaded - is measured against. A clock can be a day
	// fast, and then a dialog the other side starts today would be taken for
	// an old one.
	const auto date = oldest ? oldest : kBaselineDateUnknown;
	change([&](State &state) {
		for (const auto &[peerId, rule] : older) {
			state.rules.emplace(peerId, rule);
		}
		state.baselineDate = date;
	});
	revealOlderThanBaseline();
	flushPending();
}

// The chat list arrives a page at a time and the archive on its own, so dialogs
// older than the baseline keep turning up long after it was taken. They were
// there before it and are written down the moment they appear.
void Watcher::revealOlderThanBaseline() {
	if (!_state.baselineTaken()) {
		return;
	}
	const auto owner = &_session->data();
	const auto folder = owner->folderLoaded(Data::Folder::kId);
	const auto count = owner->chatsList()->indexed()->size()
		+ (folder ? folder->chatsList()->indexed()->size() : 0);
	if (count <= _sweptCount) {
		return;
	}
	_sweptCount = count;
	auto older = std::vector<std::pair<PeerId, Rule>>();
	const auto collect = [&](not_null<Dialogs::MainList*> list) {
		for (const auto &row : list->indexed()->all()) {
			const auto history = row->entry()->asHistory();
			if (!history) {
				continue;
			}
			const auto peer = history->peer;
			if (NeverHiddenPeer(peer)
				|| _state.rules.contains(peer->id)
				|| _pending.contains(peer->id)) {
				continue;
			}
			const auto date = history->chatListTimeId();
			if (date > 0 && date <= _state.baselineDate) {
				older.push_back({ peer->id, RuleFromBar(peer) });
			}
		}
	};
	collect(owner->chatsList());
	if (folder) {
		collect(folder->chatsList());
	}
	if (older.empty()) {
		return;
	}
	change([&](State &state) {
		for (const auto &[peerId, rule] : older) {
			state.rules.emplace(peerId, rule);
		}
	});
}

void Watcher::flushPending() {
	if (!_state.baselineTaken() || _pending.empty()) {
		return;
	}
	for (const auto peerId : base::take(_pending)) {
		if (_state.rules.contains(peerId)) {
			// The dialog has just been written down as one that was already
			// there, so its message was not the beginning of anything.
			continue;
		} else if (const auto peer = _session->data().peerLoaded(peerId)) {
			decide(peer);
		}
	}
	// The gate was held closed while these waited, so whoever postponed a
	// receipt has to learn that it may go now.
	_updates.fire({});
}

void Watcher::forget(not_null<PeerData*> peer) {
	// A rule outliving the conversation it was made about is the same mistake
	// as the read markers the server keeps: it would answer about the deleted
	// dialog while the question is about the new one. A Revealed left behind
	// sends the receipt the other side was not supposed to get when they write
	// first; a Hidden left behind silences a dialog the user has started.
	const auto pending = _pending.remove(peer->id);
	if (_state.rules.contains(peer->id)
		|| _state.heldReadTill.contains(peer->id)) {
		change([&](State &state) {
			state.rules.remove(peer->id);
			// The held position names messages of the conversation being
			// deleted, and sending it about the next one would report as read
			// what has not even arrived yet.
			state.heldReadTill.remove(peer->id);
		});
	} else if (pending) {
		_updates.fire({});
	}
}

void Watcher::note(not_null<HistoryItem*> item) {
	const auto peer = item->history()->peer;
	if (NeverHiddenPeer(peer) || item->isService()) {
		return;
	} else if (Decoy::Active()) {
		// The decoy has to behave like a plain client that never heard of the
		// feature, so it makes no rules of its own.
		return;
	} else if (item->out()) {
		// Answering is reading: once the user has written here, the other side
		// learns from the answer itself that the messages were seen, and
		// withholding the receipt only makes the dialog look broken. So this is
		// checked before the stored rule and takes a Hidden back - the one
		// thing that does, apart from the button of the note above the chat,
		// and just as permanently.
		//
		// The same message written from another device arrives here too, over
		// an update, and is meant to count: it is the user who answered. A
		// draft never arrives here at all - it creates no message - and a
		// service message was already dropped above.
		//
		// The rule falls at the moment the message is created locally, before
		// the server confirms it. A send that fails afterwards leaves the
		// receipt sent and the message unsent; waiting for the confirmation
		// instead would need an event the desktop client does not have.
		reveal(peer);
		return;
	} else if (const auto i = _state.rules.find(peer->id);
		i != end(_state.rules)) {
		// The first message decides, once and for good - except an assumption,
		// which is not a decision. A stranger writing again is a good moment
		// to settle it, because their bar is exactly what is being asked
		// about.
		if (i->second == Rule::Assumed) {
			resolveAssumed(peer);
		}
		return;
	} else if (!_state.baselineTaken()) {
		// Which dialogs were there before is not known yet, so the message
		// waits for that answer instead of being taken for the beginning of a
		// new dialog. Until then the gate stays closed for this dialog.
		_pending.emplace(peer->id);
		return;
	}
	decide(peer);
}

void Watcher::decide(not_null<PeerData*> peer) {
	if (NeverHiddenPeer(peer)) {
		return;
	}
	const auto owner = &peer->owner();
	const auto history = owner->history(peer);
	if (!history->folderKnown()) {
		// The dialog record has not been applied yet, so where this dialog
		// belongs is not known. The request for the record is already on its
		// way from History::newItemAdded, asking again only joins it, and its
		// answer arrives as a history update, which is where this dialog is
		// decided.
		_pending.emplace(peer->id);
		owner->histories().requestDialogEntry(history);
		return;
	} else if (!owner->chatsListLoaded(nullptr)
		|| (history->folder() && !owner->chatsListLoaded(history->folder()))) {
		// "This dialog was not here before" cannot be told from "its page of
		// the list has not arrived yet" while the list is still coming. The
		// wait costs a held receipt, which can be taken back; guessing costs
		// a sent one, which cannot. An archived dialog is measured against the
		// archive, which is a list of its own and is asked for here, because
		// nothing else asks for it until the user opens it.
		_pending.emplace(peer->id);
		if (const auto folder = history->folder()) {
			_session->api().requestDialogs(folder);
		}
		return;
	}
	// Deliberately not asking outboxReadTillId() here, though it looks like
	// the exact question. Found by running the two-account check 2026-08-08:
	// the server keeps read_inbox_max_id and read_outbox_max_id of a deleted
	// conversation and hands them back with the record of the new one. A
	// dialog created ten seconds ago arrived with read_outbox_max_id 137361,
	// so "the user has written here" was answered yes about messages that no
	// longer exist, and the receipt went out. That marker says the two have
	// talked at some point, not that this dialog is the user's - and starting
	// over after deleting the conversation is exactly what this feature is
	// for. Whether the dialog was there before is answered by the baseline,
	// which is written down from the list of chats and knows nothing of
	// deleted history.
	// Every dialog that was there before is written down, so one without a
	// rule is one the other side has just started - a contact just as much as
	// a stranger, which is the whole point: the bar the server offers is shown
	// for strangers only, and the promise was about any dialog the user did
	// not start.
	change([&](State &state) {
		state.rules.emplace(peer->id, Rule::Hidden);
	});
}

void Watcher::resolveAssumed(not_null<PeerData*> peer) {
	const auto i = _state.rules.find(peer->id);
	if (i == end(_state.rules) || i->second != Rule::Assumed) {
		return;
	} else if (!peer->barSettings()) {
		askBar(peer);
		return;
	}
	// The bar is Telegram's own answer to "they wrote first and you have not
	// answered", which is the question this rule was always about. It goes
	// away as soon as the user writes there, so a dialog they have answered
	// in settles as Revealed by itself and never comes back here.
	const auto rule = StrangerBarShown(peer) ? Rule::Hidden : Rule::Revealed;
	change([&](State &state) {
		state.rules[peer->id] = rule;
	});
}

void Watcher::askBar(not_null<PeerData*> peer) {
	if (Decoy::Active() || !_barAsked.emplace(peer->id).second) {
		return;
	}
	// Subscribed before the request, because a failed one answers too:
	// ApiWrap writes empty settings on failure, so the assumption is always
	// settled and a dialog can never stay withholding its receipts for good.
	peer->barSettingsValue(
	) | rpl::on_next([=] {
		resolveAssumed(peer);
	}, _lifetime);
	_session->api().requestPeerSettings(peer);
}

void Watcher::reveal(not_null<PeerData*> peer) {
	const auto id = peer->id;
	const auto i = _state.rules.find(id);
	if (i != end(_state.rules) && i->second == Rule::Revealed) {
		// Already open, and the receipt below has gone out once already. Every
		// further message of the user would repeat that request for nothing.
		return;
	}
	// A dialog left in _pending keeps ReadStatusPending answering yes, and
	// sendReadRequests puts such a receipt off by another timeout every time it
	// looks at it. The answer this dialog was waiting for has just arrived by
	// other means, so the wait is over.
	const auto wasPending = _pending.remove(id);
	const auto wasHidden = (i != end(_state.rules));
	// Dropping what the other side sends is allowed only while this dialog is
	// one the user never answered in, and answering is exactly what brings the
	// code here - from a message, from a reaction, or from the box. So the
	// open range is closed here, through the one door both switches are lifted
	// by. The closed ranges stay: what they dropped must not come back.
	closeDropRange(id);
	change([&](State &state) {
		state.rules[id] = Rule::Revealed;
	});
	if (!wasHidden && !wasPending) {
		// Nothing was ever held back here: the dialog is only being written
		// down now, and the ordinary path has already sent whatever was due.
		return;
	}
	const auto owner = &peer->owner();
	// Not history(peer): a message from another device can arrive before the
	// dialog record does, and creating a conversation from here to send a
	// receipt about it would be the wrong way round.
	if (const auto history = owner->historyLoaded(peer)) {
		owner->histories().sendReadInboxAfterReveal(history);
	}
	// Read after the call above, which is the one place that needs it. A
	// dialog that hides nothing any more has no position to hold back, and
	// keeping it would grow the blob for every dialog the user ever answered.
	if (_state.heldReadTill.contains(id)) {
		change([&](State &state) {
			state.heldReadTill.remove(id);
		});
	}
}

std::vector<DropRange> Watcher::dropRanges(PeerId peerId) const {
	const auto i = _state.dropRanges.find(peerId);
	return (i != end(_state.dropRanges))
		? i->second
		: std::vector<DropRange>();
}

void Watcher::setDropRanges(PeerId peerId, std::vector<DropRange> ranges) {
	while (ranges.size() > kDropRangesLimit) {
		const auto second = ranges.begin() + 1;
		second->from = std::min(second->from, ranges.front().from);
		second->till = std::max(second->till, ranges.front().till);
		ranges.erase(ranges.begin());
	}
	change([&](State &state) {
		if (ranges.empty()) {
			state.dropRanges.remove(peerId);
		} else {
			state.dropRanges[peerId] = std::move(ranges);
		}
	});
}

void Watcher::closeDropRange(PeerId peerId) {
	auto ranges = dropRanges(peerId);
	if (ranges.empty() || !ranges.back().open) {
		return;
	}
	ranges.back().open = false;
	if (ranges.back().till < ranges.back().from) {
		// Nothing ever arrived while it was on, so there is nothing to
		// remember about it.
		ranges.pop_back();
	}
	setDropRanges(peerId, std::move(ranges));
}

void Watcher::noteDropped(PeerId peerId, MsgId id) {
	auto ranges = dropRanges(peerId);
	if (ranges.empty() || !ranges.back().open || ranges.back().till >= id) {
		return;
	}
	ranges.back().till = id;
	setDropRanges(peerId, std::move(ranges));
}

void Watcher::noteHeldRead(PeerId peerId, MsgId tillId) {
	if (!IsServerMsgId(tillId)) {
		return;
	}
	const auto i = _state.heldReadTill.find(peerId);
	if (i != end(_state.heldReadTill) && i->second >= tillId) {
		// Reading never moves backwards, and a lower value would report as
		// unread what the user has already seen.
		return;
	}
	_state.heldReadTill[peerId] = tillId;
	// Deliberately not change(): nothing on screen shows this position, and
	// firing the update stream on every read would rebuild the note above the
	// chat each time the user scrolls through unread messages.
	WriteState(_session, _state);
}

MsgId Watcher::heldReadTill(PeerId peerId) const {
	const auto i = _state.heldReadTill.find(peerId);
	return (i != end(_state.heldReadTill)) ? i->second : MsgId(0);
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
	Find(session)->start();
}

bool ReadStatusHiddenFor(not_null<PeerData*> peer) {
	const auto session = &peer->session();
	if (!ReadStatusEnabled(session)) {
		return false;
	}
	auto &watcher = Get(session);
	watcher.resolveAssumed(peer);
	const auto &rules = watcher.state().rules;
	const auto i = rules.find(peer->id);
	return (i != end(rules)) && (i->second == Rule::Hidden);
}

bool ReadStatusHidden(not_null<History*> history) {
	return ReadStatusHiddenFor(history->peer);
}

bool ReadStatusPendingFor(not_null<PeerData*> peer) {
	const auto session = &peer->session();
	if (!ReadStatusEnabled(session)) {
		return false;
	}
	auto &watcher = Get(session);
	watcher.resolveAssumed(peer);
	return watcher.undecided(peer->id);
}

bool ReadStatusPending(not_null<History*> history) {
	return ReadStatusPendingFor(history->peer);
}

bool ReadStatusWithheldFor(not_null<PeerData*> peer) {
	return ReadStatusHiddenFor(peer) || ReadStatusPendingFor(peer);
}

void NoteHeldRead(not_null<History*> history, MsgId tillId) {
	const auto peer = history->peer;
	Get(&peer->session()).noteHeldRead(peer->id, tillId);
}

MsgId HeldReadTill(not_null<History*> history) {
	const auto peer = history->peer;
	// Deliberately not Get(): a watcher that was never started holds nothing
	// back, and starting one only to be asked this question would read the
	// rules of the account from a place that is not deciding anything.
	if (const auto watcher = Find(&peer->session())) {
		return watcher->heldReadTill(peer->id);
	}
	return MsgId(0);
}

void ForgetReadStatusRule(not_null<PeerData*> peer) {
	// Deliberately not Get(): a watcher that was never started has nothing to
	// forget, and starting one here would read the rules of the account in the
	// middle of deleting a conversation.
	if (const auto watcher = Find(&peer->session())) {
		watcher->forget(peer);
	}
}

void RevealReadStatus(not_null<PeerData*> peer) {
	Get(&peer->session()).reveal(peer);
}

std::vector<DropRange> ReadStatusDropRanges(not_null<PeerData*> peer) {
	// Deliberately not Get(): this is asked for every message that arrives,
	// and a watcher that was never started drops nothing.
	if (const auto watcher = Find(&peer->session())) {
		return watcher->dropRanges(peer->id);
	}
	return {};
}

void SetReadStatusDropRanges(
		not_null<PeerData*> peer,
		std::vector<DropRange> ranges) {
	Get(&peer->session()).setDropRanges(peer->id, std::move(ranges));
}

void CloseReadStatusDropRange(not_null<PeerData*> peer) {
	if (const auto watcher = Find(&peer->session())) {
		watcher->closeDropRange(peer->id);
	}
}

void NoteReadStatusDropped(not_null<PeerData*> peer, MsgId id) {
	if (const auto watcher = Find(&peer->session())) {
		watcher->noteDropped(peer->id, id);
	}
}

namespace {

// Not a bool: Erase evidence walks a history inside one scope, and a nested
// one must not open the gate when it ends.
int SuppressReactionReveal/* = 0*/;

} // namespace

ReactionRevealSuppressor::ReactionRevealSuppressor() {
	++SuppressReactionReveal;
}

ReactionRevealSuppressor::~ReactionRevealSuppressor() {
	--SuppressReactionReveal;
}

void NoteReactionSent(not_null<HistoryItem*> item) {
	if (SuppressReactionReveal > 0) {
		return;
	} else if (item->out() || item->isService()) {
		// Reacting to what the user wrote themselves says nothing about the
		// other side's messages having been read, so it changes nothing.
		return;
	} else if (Decoy::Active()) {
		// The decoy has to behave like a plain client that never heard of the
		// feature, so it makes no rules of its own.
		return;
	}
	const auto peer = item->history()->peer;
	if (NeverHiddenPeer(peer)) {
		return;
	}
	// Called for taking a reaction back as well as for putting one on, and
	// deliberately so: taking one back means it had been there, and the other
	// side has already been told. Nothing is lost either way - the dialog is
	// already open by then and reveal() returns at once.
	Get(&peer->session()).reveal(peer);
}

rpl::producer<> ReadStatusUpdates(not_null<Main::Session*> session) {
	return Get(session).updates();
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
					"прочтения у него не появляются, а в списке "
					"просмотревших его истории вас нет.\n\nСкрытие "
					"снимется само, как только вы отправите сюда "
					"любое сообщение — с этого устройства или с "
					"другого: ответ и так говорит собеседнику, "
					"что вы прочитали.\n\nПобочный эффект: для "
					"Telegram сообщения остаются непрочитанными, поэтому "
					"счётчик непрочитанного в этом диалоге может возвращаться "
					"после перезапуска и на других устройствах."_q
				: u"You did not start this dialog, so NovaGram does not tell "
					"the other side that you read their messages: the read "
					"marks never appear for them, and the viewer list of "
					"their stories does not name you.\n\nHiding stops by itself as "
					"soon as you send anything here, from this device or from "
					"another one: an answer tells the other side that you read "
					"it anyway.\n\nSide effect: for Telegram "
					"the messages stay unread, so the unread counter in this "
					"dialog can come back after a restart and on other "
					"devices."_q)
			: (russian
				? u"В этом диалоге статус прочтения не скрывается."_q
				: u"The read status is not hidden in this dialog."_q)),
		st::boxLabel));

	if (hidden) {
		// The switch that belongs with this one, offered where the rule it
		// depends on is explained. It also has an entry in the chat menu, and
		// that is deliberate: this box is opened from the note above the
		// messages, which is where most people meet the hiding at all.
		Ui::AddSkip(box->verticalLayout());
		struct DropState {
			rpl::variable<QString> title;
		};
		const auto drop = box->lifetime().make_state<DropState>();
		drop->title = DropIncomingActive(peer)
			? DropIncomingStopTitle()
			: DropIncomingTitle();
		const auto button = box->addRow(
			object_ptr<Ui::SettingsButton>(
				box,
				drop->title.value(),
				st::settingsButtonNoIcon),
			style::margins());
		button->setClickedCallback([=] {
			const auto now = !DropIncomingActive(peer);
			SetDropIncoming(peer, now);
			drop->title = now
				? DropIncomingStopTitle()
				: DropIncomingTitle();
		});
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(DropIncomingAbout()),
			st::boxDividerLabel));

		Ui::AddSkip(box->verticalLayout());
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(russian
				? u"Отключение необратимо: как только подтверждение уйдёт на "
					"сервер, собеседник увидит, что сообщения прочитаны — "
					"и что вы смотрели его истории, — и "
					"вернуть скрытие в этом диалоге будет нельзя. Тот же "
					"необратимый шаг сделает и ваш ответ в этом диалоге — "
					"с любого устройства."_q
				: u"Turning it off cannot be undone: once the receipt reaches "
					"the server the other side sees the messages as read and "
					"finds you in the viewer list of their stories, and "
					"hiding cannot be restored in this dialog. Answering here "
					"takes the same irreversible step, from any device."_q),
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

namespace {

[[nodiscard]] QString BarTitle() {
	return UseRussianTexts()
		? u"Статус прочтения скрыт"_q
		: u"Read status hidden"_q;
}

[[nodiscard]] QString BarText() {
	// Same promise as the Android panel; only the way out differs, because
	// here the whole note is a button and the chat menu is a second door.
	// The answer is named as a third one: it lifts the hiding without any
	// click at all, so a note that kept quiet about it would be a lie.
	return UseRussianTexts()
		? u"Собеседник не видит, что вы прочитали. Ваш ответ снимет "
			"скрытие. Нажмите, чтобы отключить сейчас."_q
		: u"The other side cannot see that you read this. Your answer "
			"lifts the hiding. Click to turn it off now."_q;
}

} // namespace

class ReadStatusBar::Bar final : public Ui::RippleButton {
public:
	explicit Bar(QWidget *parent);

private:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;
	int resizeGetHeight(int newWidth) override;

	object_ptr<Ui::FlatLabel> _title;
	object_ptr<Ui::FlatLabel> _text;

};

ReadStatusBar::Bar::Bar(QWidget *parent)
: RippleButton(parent, st::historyContactStatusButton.ripple)
, _title(this, BarTitle(), st::historyBusinessBotName)
, _text(this, BarText(), st::historyBusinessBotStatus) {
	// The note is clicked as a whole, the labels must not eat the press.
	_title->setAttribute(Qt::WA_TransparentForMouseEvents);
	_text->setAttribute(Qt::WA_TransparentForMouseEvents);
}

void ReadStatusBar::Bar::paintEvent(QPaintEvent *e) {
	QPainter p(this);

	p.fillRect(
		e->rect(),
		(isOver()
			? st::historyContactStatusButton.overBgColor
			: st::historyContactStatusButton.bgColor));
	paintRipple(p, 0, 0);
}

void ReadStatusBar::Bar::onStateChanged(
		State was,
		StateChangeSource source) {
	RippleButton::onStateChanged(was, source);
	update();
}

int ReadStatusBar::Bar::resizeGetHeight(int newWidth) {
	const auto skip = st::historyContactStatusMinSkip / 2;
	const auto available = newWidth - 2 * skip;
	_title->resizeToWidth(available);
	_title->moveToLeft(skip, skip, newWidth);
	_text->resizeToWidth(available);
	_text->moveToLeft(skip, _title->y() + _title->height(), newWidth);
	return _text->y() + _text->height() + skip;
}

ReadStatusBar::ReadStatusBar(
	std::shared_ptr<ChatHelpers::Show> show,
	not_null<Ui::RpWidget*> parent,
	not_null<UserData*> user)
: _show(std::move(show))
, _user(user)
, _inner(Ui::CreateChild<Bar>(parent.get()))
, _bar(std::make_unique<HistoryView::SlidingBar>(
	parent,
	object_ptr<Bar>::fromRaw(_inner))) {
	setupState();
	setupHandlers();
}

ReadStatusBar::~ReadStatusBar() = default;

HistoryView::SlidingBar &ReadStatusBar::bar() {
	return *_bar;
}

void ReadStatusBar::setupState() {
	rpl::single(rpl::empty) | rpl::then(
		ReadStatusUpdates(&_user->session())
	) | rpl::on_next([=] {
		// The decoy shows nothing about the feature even if its own storage
		// somehow holds rules, the same way it hides the rest of NovaGram.
		_hidden = !Decoy::Active() && ReadStatusHiddenFor(_user);
		_bar->toggleContent(_hidden);
	}, _bar->lifetime());
}

void ReadStatusBar::setupHandlers() {
	const auto user = _user;
	_inner->clicks(
	) | rpl::to_empty | rpl::on_next([=] {
		_show->showBox(Box([=](not_null<Ui::GenericBox*> box) {
			ReadStatusBox(box, user);
		}));
	}, _bar->lifetime());
}

void ReadStatusBar::show() {
	if (!_shown) {
		_shown = true;
		if (_hidden) {
			_bar->toggleContent(true);
		}
	}
	_bar->show();
}

void ReadStatusBar::hide() {
	_bar->hide();
}

} // namespace NovaGram
