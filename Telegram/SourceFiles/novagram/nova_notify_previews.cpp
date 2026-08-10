/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_notify_previews.h"

#include "api/api_reactions_notify_settings.h"
#include "apiwrap.h"
#include "base/weak_ptr.h"
#include "data/data_changes.h"
#include "data/data_folder.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/notify/data_notify_settings.h"
#include "dialogs/dialogs_entry.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "history/history.h"
#include "logs.h"
#include "main/main_session.h"
#include "novagram/nova_decoy.h"
#include "novagram/nova_pin.h"
#include "storage/storage_account.h"

#include <QtCore/QDataStream>

#include <array>

namespace NovaGram {
namespace {

constexpr auto kMagic = quint32(0x4E56504E);
// Version 2 remembers whether the name of whoever reacted is shown here. While
// the promise is on the server holds false for everyone, so that answer cannot
// be read back from it.
constexpr auto kVersion = qint32(2);
constexpr auto kMinVersion = qint32(1);
constexpr auto kStateKey = "novagram_notify_previews"_cs;

// Data::DefaultNotify: User, Group, Broadcast. The account has no other
// scopes, and every dialog with no settings of its own is answered for by one
// of these three.
constexpr auto kDefaultNotifyTypes = 3;

struct State {
	bool withheld = true;

	// Written down only once the server has answered, and separately for the
	// two requests that carry the value. A flag set at the moment of sending
	// would record an intention: a request dies unsent when the process is
	// killed or the connection never comes up, and the next start would find
	// the account already done and never try again.
	bool scopesConfirmed = false;
	bool reactionsConfirmed = false;

	// The user's own answer to "show the name of whoever reacted". Upstream
	// keeps it in the account setting this module forces to false, so with the
	// promise on there is nowhere else it could survive a restart. True is
	// both the Telegram default and what the fork's own description promises.
	bool reactionsSenderShown = true;
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
	auto withheld = qint32(0);
	auto scopes = qint32(0);
	auto reactions = qint32(0);
	auto ownerId = quint64(0);
	stream >> magic >> version >> withheld >> scopes >> reactions >> ownerId;
	if (stream.status() != QDataStream::Ok
		|| magic != kMagic
		|| version < kMinVersion
		|| version > kVersion
		|| ownerId != quint64(session->userId().bare)) {
		// The account key value store keeps its map in memory across a
		// logout, so the previous account's blob can still be there when the
		// next one starts. Its confirmations say nothing about this account,
		// and the safe answer to "has the server been told" is no.
		return State();
	}
	result.withheld = (withheld != 0);
	result.scopesConfirmed = (scopes != 0);
	result.reactionsConfirmed = (reactions != 0);
	if (version >= 2) {
		auto senderShown = qint32(0);
		stream >> senderShown;
		if (stream.status() != QDataStream::Ok) {
			return State();
		}
		result.reactionsSenderShown = (senderShown != 0);
	}
	// A version 1 blob leaves it at true, which is what the account had before
	// this was remembered: the local answer was whatever the server echoed,
	// and while the promise is on that echo was refused, so true is what was
	// on screen.
	return result;
}

void WriteState(not_null<Main::Session*> session, const State &state) {
	auto blob = QByteArray();
	auto stream = QDataStream(&blob, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_15);
	stream << kMagic
		<< kVersion
		<< qint32(state.withheld ? 1 : 0)
		<< qint32(state.scopesConfirmed ? 1 : 0)
		<< qint32(state.reactionsConfirmed ? 1 : 0)
		<< quint64(session->userId().bare)
		<< qint32(state.reactionsSenderShown ? 1 : 0);
	session->local().writePref<QByteArray>(kStateKey, blob);
}

class Watcher final : public base::has_weak_ptr {
public:
	explicit Watcher(not_null<Main::Session*> session);

	[[nodiscard]] const State &state() const {
		return _state;
	}

	void start();
	void setWithheld(bool withheld);

	// Asks for one push of the reactions settings, at most once per run of the
	// process. One correction is a correction; repeating it on every echo
	// would be a loop, because every answer of the server is itself an echo.
	void pushReactions();

	void noteReactionsConfirmed();
	void setReactionsSenderShown(bool shown);

	// Called for every reactions settings answer the client applies, from
	// either the reload or a save. It is the only signal this module has that
	// the in-memory reactions settings hold what the server holds - see
	// pushReactions for why that matters.
	void noteReactionsKnown() {
		_reactionsKnown = true;
	}

private:
	void pushScopes();
	void pushScope(
		Data::DefaultNotify type,
		const MTPinputPeerNotifySettings &settings);
	void scopeAnswered(int index, bool told);
	void pushPeer(not_null<PeerData*> peer);
	void sweepPeers();
	void save();

	const not_null<Main::Session*> _session;
	State _state;

	// Peers already told during this run. The client applies its own change
	// locally before sending and this fork only rewrites what goes on the
	// wire, so the local value stays as the server last echoed it and the same
	// peer would otherwise be pushed again on its every notification update.
	base::flat_set<PeerId> _pushedPeers;

	// One mark per scope rather than a count of outstanding requests. A count
	// says how many answers arrived, not which, so an answer left over from an
	// abandoned attempt could complete a later one and the account would be
	// written down as told while a scope was still untold.
	bool _scopeTold[kDefaultNotifyTypes] = {};

	// How many requests of the current attempt are still unanswered. Only a
	// re-entry guard: what decides confirmation is the marks above.
	int _scopesInFlight = 0;

	bool _reactionsPushed = false;
	bool _reactionsKnown = false;
	bool _reactionsReloadAsked = false;

	rpl::lifetime _lifetime;

};

Watcher::Watcher(not_null<Main::Session*> session)
: _session(session)
, _state(ReadState(session)) {
}

void Watcher::start() {
	if (Decoy::Active()) {
		// The decoy is a client that never heard of the fork, and rewriting
		// the notification settings of the account it pretends to be is the
		// one thing it must never do. It has no network at all, so this is a
		// second lock on a closed door - kept because the first one is not
		// this file's to guarantee.
		return;
	}
	auto &notify = _session->data().notifySettings();
	for (auto i = 0; i != kDefaultNotifyTypes; ++i) {
		notify.defaultUpdates(
			Data::DefaultNotify(i)
		) | rpl::on_next([=] {
			pushScopes();
		}, _lifetime);
	}
	_session->changes().peerUpdates(
		Data::PeerUpdate::Flag::Notifications
	) | rpl::on_next([=](const Data::PeerUpdate &update) {
		pushPeer(update.peer);
	}, _lifetime);

	pushScopes();
	if (!_state.reactionsConfirmed) {
		// The reactions half is otherwise driven only by an answer arriving,
		// and an answer only arrives because something asked. ApiWrap does ask
		// once at startup, but if that request fails nothing would ever ask
		// again in this run, and the account would keep composing push text
		// for reactions while the switch reads as on. Asking here costs one
		// request per start and only until the server has confirmed once.
		pushReactions();
	}
}

void Watcher::setWithheld(bool withheld) {
	if (_state.withheld == withheld) {
		return;
	}
	_state.withheld = withheld;
	// Both confirmations describe a value that is no longer the one to send.
	_state.scopesConfirmed = false;
	_state.reactionsConfirmed = false;
	_pushedPeers.clear();
	// A new promise has to be told to all three scopes again: the marks say
	// the server acknowledged the previous value, not this one.
	for (auto &mark : _scopeTold) {
		mark = false;
	}
	_reactionsPushed = false;
	// Not _reactionsKnown: whether an answer has been applied is a fact about
	// this run and does not change with the switch.
	_reactionsReloadAsked = false;
	save();

	if (!withheld) {
		// Nothing is sent when the switch goes off, and that is not an
		// oversight. The client knows only what the server currently holds,
		// which by then is what this client put there; there is no earlier
		// value to restore. What stops is the forcing: the stock notification
		// settings decide again, from whichever client the user changes them.
		return;
	}
	pushScopes();
	sweepPeers();
	pushReactions();
}

void Watcher::pushScopes() {
	if (!_state.withheld || _scopesInFlight > 0) {
		return;
	}
	auto &notify = _session->data().notifySettings();
	if (_state.scopesConfirmed) {
		// Confirmed once is not confirmed for ever. The setting belongs to the
		// account, so any other client - an official one, the web one - can put
		// previews back on, and the update saying so arrives here. A promise
		// that stops enforcing the moment it has been kept once is not a
		// promise. Nothing is re-sent while every scope still reads false, so
		// the ordinary start costs nothing.
		auto reopened = false;
		for (auto i = 0; i != kDefaultNotifyTypes; ++i) {
			const auto previews = notify.defaultSettings(
				Data::DefaultNotify(i)).showPreviews();
			if (previews && *previews) {
				_scopeTold[i] = false;
				reopened = true;
			}
		}
		if (!reopened) {
			return;
		}
		_state.scopesConfirmed = false;
		save();
	}
	auto values = std::array<MTPinputPeerNotifySettings, kDefaultNotifyTypes>();
	auto sendable = std::array<bool, kDefaultNotifyTypes>();
	auto count = 0;
	for (auto i = 0; i != kDefaultNotifyTypes; ++i) {
		if (_scopeTold[i]) {
			continue;
		}
		const auto type = Data::DefaultNotify(i);
		auto value = WithheldNotifyPreviews(
			_session,
			notify.defaultSettings(type).serialize());
		using Flag = MTPDinputPeerNotifySettings::Flag;
		if (!(value.data().vflags().v & Flag::f_show_previews)) {
			// The question asked here is not "does the client know this
			// scope" but "is there anything safe to send". Asking
			// settingsUnknown() was not enough: a scope whose description
			// never arrived is written down as an empty one - that is what
			// ApiWrap does when the request for it fails - and an empty one
			// counts as known, so that guard let it through.
			//
			// What serialising an empty scope gives is a value with no flags
			// at all, and that is not a request to change one field. It is
			// how this client says "no settings of my own", the very thing
			// resetToDefault sends. For a global scope that would hand the
			// account's mute, sound and story settings back to the Telegram
			// defaults - and put previews back on - to say one word about
			// previews.
			//
			// Skipped, not fatal: a scope that cannot be told safely must not
			// silence the two that can. Nothing is written down for it, so it
			// is tried again on the next description of any scope and at every
			// later start, and the promise is never recorded as kept while it
			// is outstanding.
			//
			// Said out loud, because otherwise a run in which this feature did
			// nothing at all looks exactly like a run in which it worked.
			LOG(("NovaGram: notify previews, scope %1 has no settings to "
				"carry the flag, skipped.").arg(i));
			continue;
		}
		values[i] = value;
		sendable[i] = true;
		++count;
	}
	if (!count) {
		return;
	}
	_scopesInFlight = count;
	for (auto i = 0; i != kDefaultNotifyTypes; ++i) {
		if (sendable[i]) {
			pushScope(Data::DefaultNotify(i), values[i]);
		}
	}
}

void Watcher::pushScope(
		Data::DefaultNotify type,
		const MTPinputPeerNotifySettings &settings) {
	const auto api = &_session->api();
	const auto weak = base::make_weak(this);
	const auto index = static_cast<int>(type);
	api->request(MTPaccount_UpdateNotifySettings(
		Data::DefaultNotifyToMTP(type),
		settings
	)).done([=] {
		if (const auto strong = weak.get()) {
			strong->scopeAnswered(index, true);
		}
	}).fail([=] {
		if (const auto strong = weak.get()) {
			strong->scopeAnswered(index, false);
		}
	}).send();
}

void Watcher::scopeAnswered(int index, bool told) {
	if (told && index >= 0 && index < kDefaultNotifyTypes) {
		_scopeTold[index] = true;
	}
	if (_scopesInFlight > 0) {
		--_scopesInFlight;
	}
	if (_scopesInFlight > 0) {
		return;
	}
	for (const auto mark : _scopeTold) {
		if (!mark) {
			// One of the three is still untold. Nothing is written down, so
			// the whole thing is attempted again later; the marks are what
			// keeps that attempt from re-sending what the server has already
			// acknowledged, and what keeps an answer left over from an
			// abandoned attempt from completing a later one.
			return;
		}
	}
	_state.scopesConfirmed = true;
	save();
	sweepPeers();
}

void Watcher::pushPeer(not_null<PeerData*> peer) {
	if (!_state.withheld) {
		return;
	}
	const auto previews = peer->notify().showPreviews();
	if (!previews || !*previews) {
		// No value means the peer has no say of its own and is answered for
		// by its scope, which has been told. A stored false is already what
		// the promise asks for.
		return;
	} else if (!_pushedPeers.emplace(peer->id).second) {
		return;
	}
	_session->api().updateNotifySettingsDelayed(peer);
}

void Watcher::sweepPeers() {
	if (!_state.withheld) {
		return;
	}
	const auto owner = &_session->data();
	const auto sweep = [&](not_null<Dialogs::MainList*> list) {
		for (const auto &row : list->indexed()->all()) {
			if (const auto history = row->entry()->asHistory()) {
				pushPeer(history->peer);
			}
		}
	};
	sweep(owner->chatsList());
	if (const auto folder = owner->folderLoaded(Data::Folder::kId)) {
		sweep(folder->chatsList());
	}
}

void Watcher::pushReactions() {
	if (!_state.withheld || _reactionsPushed) {
		return;
	}
	auto &reactions = _session->api().reactionsNotifySettings();
	if (!_reactionsKnown) {
		// Nothing may be sent before an answer has been applied. Saving the
		// reactions settings rebuilds the whole value out of what is in
		// memory, and what is in memory before the first answer is the class's
		// own initialisers - "notify about everything, default sound". Pushing
		// then would turn reaction notifications back on account-wide, and
		// reset the reaction sound, for a user who had turned them off. The
		// reload is asked for once; its answer comes back through
		// LocalReactionsPreviews, which calls this again.
		if (!_reactionsReloadAsked) {
			_reactionsReloadAsked = true;
			reactions.reload();
		}
		return;
	}
	_reactionsPushed = true;
	// Passing the current value back changes nothing locally and sends the
	// whole settings again, which is the only way this client has to put a
	// show_previews on the wire. What goes out is decided by
	// ReactionsPreviewsToSend, not by what is passed here.
	reactions.updateShowPreviews(reactions.showPreviewsCurrent());
}

void Watcher::setReactionsSenderShown(bool shown) {
	if (_state.reactionsSenderShown == shown) {
		return;
	}
	_state.reactionsSenderShown = shown;
	save();
}

void Watcher::noteReactionsConfirmed() {
	if (_state.reactionsConfirmed) {
		return;
	}
	_state.reactionsConfirmed = true;
	save();
}

void Watcher::save() {
	WriteState(_session, _state);
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
	StartNotifyPreviews(session);
	return *Find(session);
}

} // namespace

bool NotifyPreviewsWithheld(not_null<Main::Session*> session) {
	if (Decoy::Active()) {
		// The decoy has to behave like a plain client that never heard of the
		// feature. Its requests never leave the process, so this changes
		// nothing that can be observed - it keeps every path here inert
		// rather than relying on the interception one layer down.
		return false;
	}
	return Get(session).state().withheld;
}

void SetNotifyPreviewsWithheld(
		not_null<Main::Session*> session,
		bool withheld) {
	Get(session).setWithheld(withheld);
}

void StartNotifyPreviews(not_null<Main::Session*> session) {
	if (Find(session)) {
		return;
	}
	Map().emplace(session.get(), std::make_unique<Watcher>(session));
	session->lifetime().add([raw = session.get()] {
		Map().remove(raw);
	});
	// Started after it is in the map: the first push asks the map about
	// itself, through the very getters this module exports.
	Find(session)->start();
}

MTPinputPeerNotifySettings WithheldNotifyPreviews(
		not_null<Main::Session*> session,
		const MTPinputPeerNotifySettings &settings) {
	if (!NotifyPreviewsWithheld(session)) {
		return settings;
	}
	const auto &data = settings.data();
	const auto flags = data.vflags().v;
	if (!flags) {
		// Not a change of anything: this is how the client says "this peer
		// has no settings of its own". Putting a field into it would turn a
		// reset into an exception and leave a permanent server record for
		// every chat the user ever muted and unmuted.
		return settings;
	}
	using Flag = MTPDinputPeerNotifySettings::Flag;
	const auto silent = data.vsilent();
	const auto muteUntil = data.vmute_until();
	const auto sound = data.vsound();
	const auto storiesMuted = data.vstories_muted();
	const auto storiesHideSender = data.vstories_hide_sender();
	const auto storiesSound = data.vstories_sound();
	return MTP_inputPeerNotifySettings(
		MTP_flags(flags | Flag::f_show_previews),
		MTP_bool(false),
		silent ? *silent : MTPBool(),
		muteUntil ? *muteUntil : MTPint(),
		sound ? *sound : MTPNotificationSound(),
		storiesMuted ? *storiesMuted : MTPBool(),
		storiesHideSender ? *storiesHideSender : MTPBool(),
		storiesSound ? *storiesSound : MTPNotificationSound());
}

bool ReactionsPreviewsToSend(not_null<Main::Session*> session, bool local) {
	return local && !NotifyPreviewsWithheld(session);
}

bool LocalReactionsPreviews(
		not_null<Main::Session*> session,
		bool serverValue) {
	if (!NotifyPreviewsWithheld(session)) {
		return serverValue;
	}
	auto &watcher = Get(session);
	// Reaching here at all means an answer from the server is being applied,
	// so the in-memory reactions settings now hold what the server holds.
	watcher.noteReactionsKnown();
	if (serverValue) {
		// The server does not know yet - a new account, or a request that
		// never arrived. It is told again.
		watcher.pushReactions();
	} else {
		watcher.noteReactionsConfirmed();
	}
	// Whatever the server said, while the promise is on the account setting is
	// forced and therefore carries no answer about this screen: a false is
	// there to keep the promise, and it is there whether this client put it or
	// the Android one did. Asking "did I send it" was the first answer written
	// here and it was wrong for the ordinary case - both forks keep the same
	// promise, so on a desktop installed second the false is already on the
	// server, and taking it for the user's own choice blanked the name of
	// whoever reacted and made the stock switch flip itself back off a round
	// trip after being turned on.
	//
	// The honest boundary: a user who had turned the sender off in another
	// client before this one was installed gets it shown here until they turn
	// it off again. That false is indistinguishable from the promise's own,
	// and defaulting to showing is what the fork's own description promises.
	return watcher.state().reactionsSenderShown;
}

void NoteReactionsSenderShown(
		not_null<Main::Session*> session,
		bool shown) {
	if (Decoy::Active()) {
		return;
	}
	Get(session).setReactionsSenderShown(shown);
}

QString NotifyPreviewsTitle() {
	return UseRussianTexts()
		? u"Не отдавать текст уведомлений серверам"_q
		: u"Keep notification text off the servers"_q;
}

} // namespace NovaGram
