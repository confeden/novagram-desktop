/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_sync_deauth.h"

#include "apiwrap.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "novagram/nova_decoy.h"
#include "novagram/nova_pin.h"
#include "storage/storage_account.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>

namespace NovaGram::SyncDeauth {
namespace {

constexpr auto kEnabledKey = "novagram_sync_deauth"_cs;
constexpr auto kStateKey = "novagram_sync_deauth"_cs;
constexpr auto kMagic = quint32(0x4E565344);
constexpr auto kVersion = qint32(1);

// The line. Three characters that say what it is, sixteen that no two devices
// and no two wipes ever repeat, five that check the other nineteen against the
// account they belong to. Twenty-four in total, because the promise made in
// the settings names that number and because a line that fits on one row is
// one the owner can recognise in their own Saved Messages afterwards.
constexpr auto kLength = 24;
constexpr auto kNonceChars = 16;
constexpr auto kTagChars = 5;

// RFC 4648 base32: no character that another one can be mistaken for, and
// nothing outside it in the line, so "does this match" is answered by a
// character range and not by a parser.
const auto kAlphabet = QByteArrayLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567");

// How far back the one look at Saved Messages goes. The line is the newest
// message there when it matters - a client that is not running adds nothing to
// Saved Messages - so a dozen covers a device that was switched off for a
// week.
constexpr auto kScanLimit = 12;

// The safety net, and the only thing here that repeats. Everything normally
// arrives as an update; this covers the one hole updates have - a client that
// was offline long enough for the server to refuse to tell it what it missed.
constexpr auto kRescanInterval = 30 * 60 * crl::time(1000);

// How long the device under duress waits for the server to acknowledge the
// line before it stops waiting. Long enough for a slow network to answer,
// short enough that whoever is standing over the device does not see a client
// that has visibly hung. What happens when it passes is a decision and not an
// oversight: see the note over Broadcast().
constexpr auto kSendTimeout = crl::time(12000);

// A line is stamped by the server at the moment it is sent, so a date in the
// future is not one. Scheduled messages are the reason this is checked: they
// pass through the same funnel, they are outgoing, and a scheduled line would
// be a wipe planned in advance rather than one happening now.
constexpr auto kFutureSkew = TimeId(300);

constexpr auto kAuthorizationRetries = 3;
constexpr auto kAuthorizationRetryDelay = crl::time(4000);

struct State {
	// When this feature was switched on here. A line older than this was
	// written while nothing was listening, and firing on it now would be a
	// wipe nobody asked for.
	TimeId enabledSince = 0;

	// The newest line already acted on. Belt to the braces: the destruction
	// arms the decoy first and the decoy never listens, so this is only ever
	// read by a run that was interrupted before it got that far.
	TimeId lastActed = 0;
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
	auto ownerId = quint64(0);
	auto enabledSince = qint32(0);
	auto lastActed = qint32(0);
	stream >> magic >> version >> ownerId >> enabledSince >> lastActed;
	if (stream.status() != QDataStream::Ok
		|| magic != kMagic
		|| version != kVersion
		|| enabledSince < 0
		|| lastActed < 0) {
		return State();
	} else if (ownerId != quint64(session->userId().bare)) {
		// The account key value store keeps its map in memory across a logout,
		// so the previous account's watermark can still be there when the next
		// one starts. Answering with it would measure this account's lines
		// against another account's clock.
		return State();
	}
	result.enabledSince = TimeId(enabledSince);
	result.lastActed = TimeId(lastActed);
	return result;
}

void WriteState(not_null<Main::Session*> session, const State &state) {
	auto blob = QByteArray();
	auto stream = QDataStream(&blob, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_15);
	stream << kMagic
		<< kVersion
		<< quint64(session->userId().bare)
		<< qint32(state.enabledSince)
		<< qint32(state.lastActed);
	session->local().writePref<QByteArray>(kStateKey, blob);
}

[[nodiscard]] QString Prefix() {
	return u"NG1"_q;
}

[[nodiscard]] QString MakeNonce() {
	auto bytes = QByteArray(kNonceChars, Qt::Uninitialized);
	base::RandomFill(bytes.data(), bytes.size());
	auto result = QString();
	result.reserve(kNonceChars);
	for (auto i = 0; i != kNonceChars; ++i) {
		result.append(QChar::fromLatin1(kAlphabet[uchar(bytes[i]) & 31]));
	}
	return result;
}

// Not a signature, and not pretending to be one: the key is the account's own
// id, which is not a secret. What it buys is worth the five characters - a
// line out of a screenshot of somebody else's Saved Messages, or one left over
// from another account on a shared machine, is not a line for this account and
// is refused long before the wipe.
[[nodiscard]] QString Tag(uint64 userId, const QString &nonce) {
	auto data = QByteArray("NovaGram/sync-deauth/v1|");
	data += QByteArray::number(qulonglong(userId));
	data += '|';
	data += nonce.toLatin1();
	const auto hash = QCryptographicHash::hash(
		data,
		QCryptographicHash::Sha256);
	auto result = QString();
	result.reserve(kTagChars);
	for (auto i = 0; i != kTagChars; ++i) {
		result.append(QChar::fromLatin1(kAlphabet[uchar(hash[i]) & 31]));
	}
	return result;
}

[[nodiscard]] QString MakeSignal(uint64 userId) {
	const auto nonce = MakeNonce();
	return Prefix() + nonce + Tag(userId, nonce);
}

[[nodiscard]] bool IsSignal(const QString &text, uint64 userId) {
	// Ordered so that the cheapest question is asked first: every message that
	// is not this one is answered by its length alone.
	const auto prefix = Prefix();
	if (text.size() != kLength || !text.startsWith(prefix)) {
		return false;
	}
	const auto body = text.mid(prefix.size());
	for (const auto &ch : body) {
		const auto latin = ch.toLatin1();
		if (!latin || kAlphabet.indexOf(latin) < 0) {
			return false;
		}
	}
	return (body.mid(kNonceChars) == Tag(userId, body.left(kNonceChars)));
}

class Watcher final {
public:
	explicit Watcher(not_null<Main::Session*> session);
	~Watcher();

	void start();

	// The switch was turned on again: everything already in Saved Messages
	// belongs to the time when nothing here was listening.
	void resetEnabledSince();

private:
	[[nodiscard]] TimeId threshold() const;
	void notice(not_null<HistoryItem*> item);
	void consider(TimeId date, const QString &text);
	void requestAuthorized();
	void scan();
	void maybeAct();

	const not_null<Main::Session*> _session;
	State _state;

	// When this authorization was created, straight from the server. The one
	// answer that makes "never fire again after the next login" true without
	// depending on anything this device stores - a device that has just wiped
	// itself keeps nothing at all.
	TimeId _authorized = 0;

	// When this watcher began listening, on the monotonic clock. The anchor
	// below is that moment expressed in server time, which is the only clock
	// worth writing down: until the first exchange with the server, ours is
	// whatever the machine says, and a machine a day slow would anchor a day
	// in the past and obey a line from before the feature existed.
	crl::time _startedAt = 0;

	int _authorizationTries = 0;
	mtpRequestId _authorizationRequest = 0;
	mtpRequestId _scanRequest = 0;

	// A line recognised while the answer above was not there yet. Held, never
	// dropped: the request is on its way, and the wipe waits for it rather
	// than guessing in either direction.
	TimeId _held = 0;
	bool _acted = false;

	base::Timer _retry;
	base::Timer _rescan;
	rpl::lifetime _lifetime;

};

[[nodiscard]] base::flat_map<Main::Session*, std::unique_ptr<Watcher>> &Map() {
	static auto result
		= base::flat_map<Main::Session*, std::unique_ptr<Watcher>>();
	return result;
}

[[nodiscard]] Watcher *Find(not_null<Main::Session*> session) {
	const auto i = Map().find(session.get());
	return (i != end(Map())) ? i->second.get() : nullptr;
}

Watcher::Watcher(not_null<Main::Session*> session)
: _session(session)
, _state(ReadState(session))
, _retry([=] { requestAuthorized(); })
, _rescan([=] { scan(); }) {
	// The live half. Every message that becomes an item passes here, including
	// the ones a reconnection catches up on, so a client that was merely
	// asleep needs nothing else. The whole cost of it while nothing is
	// happening is a comparison of one string length.
	_session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		notice(item);
	}, _lifetime);
}

Watcher::~Watcher() {
	// The watcher goes when the session does, and a request still in flight
	// would answer into a destroyed object. ApiWrap outlives the session
	// lifetime this is torn down from, so cancelling here is safe.
	_session->api().request(base::take(_authorizationRequest)).cancel();
	_session->api().request(base::take(_scanRequest)).cancel();
}

void Watcher::start() {
	if (Decoy::Active()) {
		// The disguise has no account of its own and must never behave like a
		// client that has heard of this at all.
		return;
	}
	// Deliberately not writing the anchor here. It is set once the server has
	// said what time it is - see requestAuthorized() - and until then nothing
	// is obeyed: a line is held, never dropped, so a wipe that arrives during
	// these first seconds is still carried out.
	_startedAt = crl::now();
	requestAuthorized();
	scan();
	_rescan.callEach(kRescanInterval);
}

void Watcher::resetEnabledSince() {
	_state.enabledSince = base::unixtime::now();
	_held = 0;
	WriteState(_session, _state);
}

TimeId Watcher::threshold() const {
	return std::max({ _state.enabledSince, _state.lastActed, _authorized });
}

void Watcher::notice(not_null<HistoryItem*> item) {
	if (_acted || Decoy::Active() || !Enabled()) {
		return;
	} else if (!item->out()
		|| item->isService()
		|| !item->history()->peer->isSelf()) {
		// This account's own outgoing message in Saved Messages, and nothing
		// else. Nobody but a session of this account can write there, which is
		// what makes the line worth obeying at all.
		return;
	} else if (item->Has<HistoryMessageForwarded>()) {
		// A forward is a line carried in by hand - out of an old conversation,
		// out of a screenshot. The line means "it is happening now", and a
		// forward says nothing about when.
		return;
	}
	consider(item->date(), item->originalText().text);
}

void Watcher::consider(TimeId date, const QString &text) {
	if (_acted
		|| date <= 0
		|| date > base::unixtime::now() + kFutureSkew
		|| date <= std::max(_state.enabledSince, _state.lastActed)) {
		return;
	} else if (!IsSignal(text, _session->userId().bare)) {
		return;
	} else if (date > _held) {
		_held = date;
	}
	maybeAct();
}

void Watcher::maybeAct() {
	if (_acted || !_held) {
		return;
	} else if (!_authorized || !_state.enabledSince) {
		// Nothing is decided without the server's two answers. Refusing would
		// leave the promise unkept; acting would risk a wipe on a line from
		// before this login, or from before the feature was switched on. So
		// the line waits.
		//
		// A line is waiting, so the attempt budget is refilled: it exists to
		// stop a client with no network asking forever, not to give up on a
		// wipe somebody has actually asked for. The retry timer still paces
		// what follows.
		_authorizationTries = 0;
		requestAuthorized();
		return;
	} else if (_held <= threshold()) {
		_held = 0;
		return;
	}
	_acted = true;
	_state.lastActed = _held;
	_held = 0;
	WriteState(_session, _state);
	// The same destruction the emergency PIN runs on the device it was typed
	// on, and deliberately without its first half: this client sends nothing,
	// or a room full of NovaGram clients would answer each other in a circle.
	//
	// Queued, not called: this is reached from inside the handling of an
	// arriving message or of a network answer, and the destruction ends by
	// logging the account out - taking apart, from the middle of its own
	// stack, the machinery that is still walking it.
	crl::on_main([] { RunSyncedWipe(); });
}

void Watcher::requestAuthorized() {
	if (_authorized
		|| _authorizationRequest
		|| _authorizationTries >= kAuthorizationRetries) {
		return;
	}
	++_authorizationTries;
	_authorizationRequest = _session->api().request(
		MTPaccount_GetAuthorizations()
	).done([=](const MTPaccount_Authorizations &result) {
		_authorizationRequest = 0;
		for (const auto &entry : result.data().vauthorizations().v) {
			const auto &data = entry.data();
			if (data.is_current()) {
				_authorized = data.vdate_created().v;
				break;
			}
		}
		// The clock is the server's from here on, so this is where the anchor
		// is written. It is not "now" but the moment this watcher started,
		// carried across on the monotonic clock: a line that arrived while the
		// answer was travelling belongs to the time when we were already
		// listening, and anchoring at "now" would throw it away.
		//
		// Written when there is none - the first run of a build that has this,
		// and everything already lying in Saved Messages is older than the
		// promise - and when the stored one is in the future, which is what a
		// machine with a fast clock wrote before this rule existed. Never the
		// other way, or switching the feature off and on again would be undone
		// by the next start.
		const auto elapsed = TimeId(
			(crl::now() - _startedAt) / crl::time(1000));
		const auto anchor = base::unixtime::now() - elapsed;
		if (!_state.enabledSince || _state.enabledSince > anchor) {
			_state.enabledSince = anchor;
			WriteState(_session, _state);
		}
		maybeAct();
	}).fail([=] {
		_authorizationRequest = 0;
		if (_authorizationTries < kAuthorizationRetries) {
			_retry.callOnce(kAuthorizationRetryDelay);
		}
	}).send();
}

void Watcher::scan() {
	if (_acted || _scanRequest || Decoy::Active() || !Enabled()) {
		return;
	}
	_scanRequest = _session->api().request(MTPmessages_GetHistory(
		MTP_inputPeerSelf(),
		MTP_int(0), // offset_id
		MTP_int(0), // offset_date
		MTP_int(0), // add_offset
		MTP_int(kScanLimit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		_scanRequest = 0;
		result.match([](const MTPDmessages_messagesNotModified &) {
		}, [&](const auto &data) {
			for (const auto &message : data.vmessages().v) {
				message.match([&](const MTPDmessage &fields) {
					if (!fields.is_out() || fields.vfwd_from()) {
						return;
					}
					consider(fields.vdate().v, qs(fields.vmessage()));
				}, [](const auto &) {
				});
			}
		});
	}).fail([=] {
		_scanRequest = 0;
	}).send();
}

// The one send, per account. Deliberately a bare request and not the ordinary
// send path: no local message is created, so nothing of this shows on the
// screen of the device being taken away, and none of the fork's own outgoing
// hooks - the auto-deletion queue, the night-silent flag, the metadata scrub -
// is given a chance to rewrite the one line whose bytes have to arrive exactly
// as they were built.
class Sender final {
public:
	explicit Sender(Fn<void()> done);

private:
	void finishOne();
	void finish();

	Fn<void()> _done;
	int _outstanding = 0;
	bool _finished = false;
	base::Timer _deadline;

};

Sender::Sender(Fn<void()> done)
: _done(std::move(done))
, _deadline([=] { finish(); }) {
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		const auto session = account->maybeSession();
		if (!session) {
			continue;
		}
		const auto text = MakeSignal(session->userId().bare);
		using Flag = MTPmessages_SendMessage::Flag;
		++_outstanding;
		session->api().request(MTPmessages_SendMessage(
			MTP_flags(Flag::f_silent | Flag::f_no_webpage),
			MTP_inputPeerSelf(),
			MTPInputReplyTo(),
			MTP_string(text),
			MTP_long(base::RandomValue<uint64>()),
			MTPReplyMarkup(),
			MTPVector<MTPMessageEntity>(),
			MTP_int(0), // schedule_date
			MTP_int(0), // schedule_repeat_period
			MTP_inputPeerEmpty(), // send_as
			MTPInputQuickReplyShortcut(),
			MTP_long(0), // effect
			MTP_long(0), // allow_paid_stars
			MTPSuggestedPost(),
			MTPInputRichMessage()
		)).done([=] {
			finishOne();
		}).fail([=] {
			// A refusal is an answer too. Retrying inside the deadline would
			// only spend it, and the deadline is the whole retry budget.
			finishOne();
		}).send();
	}
	if (!_outstanding) {
		finish();
	} else {
		_deadline.callOnce(kSendTimeout);
	}
}

void Sender::finishOne() {
	if (_outstanding > 0 && !--_outstanding) {
		finish();
	}
}

void Sender::finish() {
	if (_finished) {
		return;
	}
	_finished = true;
	_deadline.cancel();
	if (const auto done = base::take(_done)) {
		done();
	}
}

[[nodiscard]] std::unique_ptr<Sender> &Sending() {
	static auto result = std::unique_ptr<Sender>();
	return result;
}

} // namespace

bool Enabled() {
	// On by default: the promise the emergency PIN makes is about the account
	// as the person holding the device thinks of it, and one client of it
	// staying signed in with everything on it keeps none of that promise.
	return Core::App().settings().readPref<bool>(kEnabledKey, true);
}

void SetEnabled(bool enabled) {
	if (Enabled() == enabled) {
		return;
	}
	Core::App().settings().writePref<bool>(kEnabledKey, enabled);
	Core::App().saveSettingsDelayed();
	if (!enabled) {
		return;
	}
	// Switching it back on must not obey a line written while it was off: the
	// device it came from was destroyed hours ago and this one was told, by
	// its owner, not to follow.
	for (const auto &[session, watcher] : Map()) {
		watcher->resetEnabledSince();
	}
}

QString Title() {
	return UseRussianTexts()
		? u"Синхронная деавторизация"_q
		: u"Synchronous deauthorization"_q;
}

QString About() {
	return UseRussianTexts()
		? u"Аварийный PIN уничтожает только то устройство, где его ввели. С "
			"этой настройкой оно сначала пишет в «Избранное» строку из 24 "
			"символов и дожидается сервера, и лишь потом стирает себя, а "
			"остальные клиенты NovaGram этого аккаунта делают то же самое — "
			"даже без PIN. Строка старше последнего входа не принимается; без "
			"сети ожидание обрывается через 12 секунд."_q
		: u"The emergency PIN destroys only the device it was typed on. With "
			"this on, that device first writes a 24-character line into Saved "
			"Messages and waits for the server, and only then wipes itself, "
			"while every other NovaGram client of the account does the same - "
			"even one with no PIN. A line older than the last login is "
			"refused; with no network the wait ends after 12 seconds."_q;
}

void Start(not_null<Main::Session*> session) {
	if (Find(session)) {
		return;
	}
	Map().emplace(session.get(), std::make_unique<Watcher>(session));
	session->lifetime().add([raw = session.get()] {
		Map().remove(raw);
	});
	Find(session)->start();
}

void Broadcast(Fn<void()> done) {
	if (!Enabled() || Decoy::Active()) {
		done();
		return;
	}
	// Asked before anything is built, so that "there is nothing to send" ends
	// in a plain call and not inside the constructor of an object that would
	// then be assigned to after the whole wipe has already run. This is the
	// locked cold start: the accounts were never started, no key is in memory,
	// and the destruction goes ahead exactly as it did before this existed.
	auto reachable = false;
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (account->maybeSession()) {
			reachable = true;
			break;
		}
	}
	if (!reachable) {
		done();
		return;
	}
	if (Sending()) {
		// The wipe this belongs to is already on its way and owns the ending.
		// Replacing the sender would destroy one whose requests are still in
		// flight and are holding a pointer to it.
		return;
	}
	Sending() = std::make_unique<Sender>(std::move(done));
}

} // namespace NovaGram::SyncDeauth
