/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_muted_members.h"

#include "base/random.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "novagram/nova_decoy.h"
#include "novagram/nova_pin.h"
#include "storage/storage_account.h"
#include "ui/painter.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/labels.h"
#include "styles/style_layers.h"

#include <QtCore/QDataStream>

namespace NovaGram {
namespace {

constexpr auto kMagic = quint32(0x4E564D4D); // NVMM
constexpr auto kVersion = qint32(1);
constexpr auto kMinVersion = qint32(1);
constexpr auto kStateKey = "novagram_muted_members"_cs;

struct State {
	// Chat to the members muted in it. A chat with none is not kept, so an
	// empty map is the ordinary state and costs one comparison to check.
	base::flat_map<PeerId, base::flat_set<PeerId>> rules;
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
	auto chats = qint32(0);
	stream >> magic >> version >> ownerId >> chats;
	if (stream.status() != QDataStream::Ok
		|| magic != kMagic
		|| version < kMinVersion
		|| version > kVersion
		|| chats < 0
		// The account key value store keeps its map in memory across a logout,
		// so the previous account's blob can still be there when the next one
		// starts. A rule names peers of one account only.
		|| ownerId != quint64(session->userId().bare)) {
		return State();
	}
	for (auto i = 0; i != chats; ++i) {
		auto chatId = quint64(0);
		auto members = qint32(0);
		stream >> chatId >> members;
		if (stream.status() != QDataStream::Ok || members < 0) {
			return State();
		}
		auto users = base::flat_set<PeerId>();
		for (auto j = 0; j != members; ++j) {
			auto userId = quint64(0);
			stream >> userId;
			if (stream.status() != QDataStream::Ok || !userId) {
				return State();
			}
			users.emplace(PeerId(userId));
		}
		if (!users.empty()) {
			result.rules.emplace(PeerId(chatId), std::move(users));
		}
	}
	return result;
}

void WriteState(not_null<Main::Session*> session, const State &state) {
	auto blob = QByteArray();
	auto stream = QDataStream(&blob, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_15);
	stream << kMagic
		<< kVersion
		<< quint64(session->userId().bare)
		<< qint32(state.rules.size());
	for (const auto &[chatId, users] : state.rules) {
		stream << quint64(chatId.value) << qint32(users.size());
		for (const auto &userId : users) {
			stream << quint64(userId.value);
		}
	}
	session->local().writePref<QByteArray>(kStateKey, blob);
}

class Holder final {
public:
	explicit Holder(not_null<Main::Session*> session);

	[[nodiscard]] bool muted(PeerId chatId, PeerId userId) const;
	[[nodiscard]] bool anyMuted(PeerId chatId) const;
	void setMuted(PeerId chatId, PeerId userId, bool muted);
	void forget(PeerId chatId);

	[[nodiscard]] bool expanded(FullMsgId id) const;
	void expand(FullMsgId id);
	[[nodiscard]] ClickHandlerPtr expandLink(FullMsgId id);

private:
	void refresh(PeerId chatId, PeerId userId);

	const not_null<Main::Session*> _session;
	State _state;

	// Unfolded by hand. Kept in memory only: it is a choice about a moment,
	// not a rule, and leaving the chat should fold it again.
	base::flat_set<FullMsgId> _expanded;

	// One handler per collapsed message, kept alive for as long as the row is.
	base::flat_map<FullMsgId, ClickHandlerPtr> _expandLinks;

};

Holder::Holder(not_null<Main::Session*> session)
: _session(session)
, _state(ReadState(session)) {
}

bool Holder::muted(PeerId chatId, PeerId userId) const {
	const auto i = _state.rules.find(chatId);
	return (i != end(_state.rules)) && i->second.contains(userId);
}

bool Holder::anyMuted(PeerId chatId) const {
	const auto i = _state.rules.find(chatId);
	return (i != end(_state.rules)) && !i->second.empty();
}

void Holder::setMuted(PeerId chatId, PeerId userId, bool muted) {
	if (this->muted(chatId, userId) == muted) {
		return;
	}
	if (muted) {
		_state.rules[chatId].emplace(userId);
	} else {
		const auto i = _state.rules.find(chatId);
		if (i != end(_state.rules)) {
			i->second.remove(userId);
			if (i->second.empty()) {
				_state.rules.erase(i);
			}
		}
		// What was unfolded by hand is forgotten with the rule, or a message
		// would come back already unfolded the next time this person is muted.
		for (auto i = begin(_expanded); i != end(_expanded);) {
			i = (i->peer == chatId) ? _expanded.erase(i) : (i + 1);
		}
	}
	WriteState(_session, _state);
	refresh(chatId, userId);
}

void Holder::forget(PeerId chatId) {
	const auto i = _state.rules.find(chatId);
	if (i == end(_state.rules)) {
		return;
	}
	_state.rules.erase(i);
	WriteState(_session, _state);
}

bool Holder::expanded(FullMsgId id) const {
	return _expanded.contains(id);
}

void Holder::expand(FullMsgId id) {
	_expanded.emplace(id);
	_expandLinks.remove(id);
}

ClickHandlerPtr Holder::expandLink(FullMsgId id) {
	const auto i = _expandLinks.find(id);
	if (i != end(_expandLinks)) {
		return i->second;
	}
	const auto session = _session;
	auto handler = std::make_shared<LambdaClickHandler>([=] {
		if (const auto item = session->data().message(id)) {
			ExpandItem(item);
		}
	});
	_expandLinks.emplace(id, handler);
	return handler;
}

void Holder::refresh(PeerId chatId, PeerId userId) {
	// Every message of this member changes height, so the views have to be
	// rebuilt rather than repainted. Same shape as the upstream reaction to a
	// change in the restriction settings, Data::Session::requestItemViewRefresh.
	const auto history = _session->data().historyLoaded(chatId);
	if (!history) {
		return;
	}
	auto &owner = _session->data();
	for (const auto &block : history->blocks) {
		for (const auto &view : block->messages) {
			const auto item = view->data();
			const auto from = item->displayFrom();
			if (from && from->id == userId) {
				owner.requestItemViewRefresh(item);
				owner.requestItemResize(item);
			}
		}
	}
}

[[nodiscard]] base::flat_map<Main::Session*, std::unique_ptr<Holder>> &Map() {
	static auto result
		= base::flat_map<Main::Session*, std::unique_ptr<Holder>>();
	return result;
}

[[nodiscard]] Holder *Find(not_null<Main::Session*> session) {
	const auto i = Map().find(session.get());
	return (i != end(Map())) ? i->second.get() : nullptr;
}

[[nodiscard]] Holder &Get(not_null<Main::Session*> session) {
	if (const auto found = Find(session)) {
		return *found;
	}
	StartMutedMembers(session);
	return *Find(session);
}

} // namespace

void StartMutedMembers(not_null<Main::Session*> session) {
	if (Find(session)) {
		return;
	}
	Map().emplace(session.get(), std::make_unique<Holder>(session));
	session->lifetime().add([raw = session.get()] {
		Map().remove(raw);
	});
}

bool MutableChat(not_null<PeerData*> chat) {
	if (Decoy::Active()) {
		// The gate lives here because this is the one question every caller
		// asks, the menu entry included: a row no Telegram build has is exactly
		// what the decoy must not show.
		return false;
	}
	// A monoforum is a channel's direct messages - a set of one-to-one
	// conversations - so it carries the megagroup flag while being the very
	// case the feature excludes. Android already excludes it by name.
	if (const auto channel = chat->asChannel()) {
		if (channel->isMonoforum()) {
			return false;
		}
	}
	// More than two people, and everyone in it speaks for themselves: a channel
	// posts as itself, so there is no member there to mute.
	return chat->isChat() || chat->isMegagroup();
}

bool MemberMuted(
		not_null<PeerData*> chat,
		not_null<UserData*> user) {
	if (Decoy::Active() || !MutableChat(chat)) {
		// The decoy has to read as a client that never heard of the fork.
		return false;
	}
	return Get(&chat->session()).muted(chat->id, user->id);
}

void SetMemberMuted(
		not_null<PeerData*> chat,
		not_null<UserData*> user,
		bool muted) {
	if (Decoy::Active() || !MutableChat(chat)) {
		return;
	}
	Get(&chat->session()).setMuted(chat->id, user->id, muted);
}

bool ItemFromMutedMember(not_null<HistoryItem*> item) {
	if (Decoy::Active() || item->out() || item->isService()) {
		return false;
	}
	const auto chat = item->history()->peer;
	if (!MutableChat(chat)) {
		return false;
	}
	const auto holder = Find(&chat->session());
	if (!holder || !holder->anyMuted(chat->id)) {
		// The common case, and it has to be cheap: this is asked for every
		// message that is drawn.
		return false;
	}
	const auto from = item->displayFrom();
	const auto user = from ? from->asUser() : nullptr;
	// An anonymous admin speaks as the chat itself; there is no member to mute.
	return user && holder->muted(chat->id, user->id);
}

bool MutedNotificationSender(
		not_null<HistoryItem*> item,
		UserData *sender) {
	if (!sender || Decoy::Active()) {
		return false;
	}
	const auto chat = item->history()->peer;
	return MutableChat(chat) && MemberMuted(chat, sender);
}

bool ItemCollapsed(not_null<HistoryItem*> item) {
	if (!ItemFromMutedMember(item)) {
		return false;
	}
	const auto holder = Find(&item->history()->session());
	return holder && !holder->expanded(item->fullId());
}

void ExpandItem(not_null<HistoryItem*> item) {
	const auto session = &item->history()->session();
	if (const auto holder = Find(session)) {
		holder->expand(item->fullId());
		session->data().requestItemViewRefresh(item);
		session->data().requestItemResize(item);
	}
}

ClickHandlerPtr ExpandLink(not_null<HistoryItem*> item) {
	return Get(&item->history()->session()).expandLink(item->fullId());
}

void ForgetMutedMembers(not_null<PeerData*> chat) {
	if (const auto holder = Find(&chat->session())) {
		// A rule that outlives its conversation would mute a stranger in
		// whatever chat takes that identifier next.
		holder->forget(chat->id);
	}
}

void PaintMutedMark(
		QPainter &p,
		int left,
		int top,
		int size,
		float64 baseOpacity) {
	if (size <= 0) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	// Red is what "off" is drawn in everywhere else in the client; thin and
	// half transparent so the mark answers "why is this one line" without
	// becoming the thing the eye lands on.
	auto pen = QPen(st::attentionButtonFg->c);
	pen.setWidthF(style::ConvertScaleExact(1.5));
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.setOpacity(baseOpacity * 0.55);
	const auto inset = size / 6;
	p.drawLine(left + inset, top + inset, left + size - inset, top + size - inset);
	p.drawLine(left + size - inset, top + inset, left + inset, top + size - inset);
	p.setOpacity(baseOpacity);
}

QString MutedMessageText() {
	return UseRussianTexts()
		? u"Скрытое сообщение"_q
		: u"Hidden message"_q;
}

QString MuteMemberMenuText(bool muted) {
	return UseRussianTexts()
		? (muted ? u"Не заглушать в чате"_q : u"Заглушить в чате"_q)
		: (muted ? u"Unmute in this chat"_q : u"Mute in this chat"_q);
}

void MuteMemberBox(
		not_null<Ui::GenericBox*> box,
		not_null<PeerData*> chat,
		not_null<UserData*> user) {
	const auto russian = UseRussianTexts();
	const auto muted = MemberMuted(chat, user);
	const auto name = user->shortName();
	box->setTitle(rpl::single(MuteMemberMenuText(muted)));

	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(muted
			? (russian
				? (u"Сообщения "_q + name + u" в этом чате снова будут "
					"вызывать уведомления и показываться целиком."_q)
				: (u"Messages from "_q + name + u" in this chat will notify "
					"you and be shown in full again."_q))
			: (russian
				? (u"Сообщения "_q + name + u" в этом чате не будут вызывать "
					"уведомлений и свернутся в приглушённую строку. Нажмите на "
					"строку, чтобы прочитать. Никуда ничего не отправляется: "
					"собеседник этого не увидит."_q)
				: (u"Messages from "_q + name + u" in this chat will never "
					"notify you and will collapse into a dimmed line. Click "
					"the line to read one. Nothing is sent anywhere: the other "
					"side cannot tell."_q))),
		st::boxLabel));

	box->addButton(
		rpl::single(muted
			? (russian ? u"Отключить"_q : u"Turn off"_q)
			: (russian ? u"Заглушить"_q : u"Mute"_q)),
		[=] {
			SetMemberMuted(chat, user, !muted);
			box->closeBox();
		});
	box->addButton(
		rpl::single(russian ? u"Назад"_q : u"Back"_q),
		[=] { box->closeBox(); });
}

} // namespace NovaGram
