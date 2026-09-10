/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_peer_id.h"

#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "novagram/nova_decoy.h"
#include "novagram/nova_pin.h"
#include "ui/boxes/confirm_box.h"
#include "ui/text/text_utilities.h"
#include "window/window_session_controller.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>

namespace NovaGram::PeerIds {
namespace {

// Shorter than any identifier Telegram hands out and longer than any it will,
// so a year, a price and a four-digit code are never candidates.
constexpr auto kMinDigits = 6;
constexpr auto kMaxDigits = 13;

// The five the documentation allows a zero access hash for: the service
// account, and the four bots every client is expected to be able to reach.
constexpr uint64 kServiceIds[] = {
	777000,
	1271266957,
	1087968824,
	136817688,
	5434988373,
};

[[nodiscard]] bool IsServiceId(uint64 bare) {
	return ranges::contains(kServiceIds, bare);
}

// Whether this client holds what the server needs to be asked about the peer.
// peerLoaded() alone is not that: it answers for deleted accounts whose hash
// has been zeroed, and hides min-peers, which are exactly the ones a context
// message can resolve. So the test is the hash, or a message that names them.
[[nodiscard]] PeerData *ResolvablePeer(
		not_null<Main::Session*> session,
		uint64 bare) {
	if (!bare) {
		return nullptr;
	}
	const auto owner = &session->data();
	if (session->userId().bare == bare) {
		return session->user();
	}
	const auto userId = peerFromUser(UserId(bare));
	if (const auto user = owner->userLoaded(UserId(bare))) {
		if (user->accessHash() || IsServiceId(bare)) {
			return user;
		}
	}
	if (owner->messageWithPeer(userId)) {
		return owner->user(UserId(bare));
	}
	// A plain group has no access hash by design, so its identifier is enough.
	const auto chatId = peerFromChat(ChatId(bare));
	if (const auto chat = owner->chatLoaded(ChatId(bare))) {
		return chat;
	}
	const auto channelId = peerFromChannel(ChannelId(bare));
	if (const auto channel = owner->channelLoaded(ChannelId(bare))) {
		if (channel->accessHash()) {
			return channel;
		}
	}
	if (owner->messageWithPeer(channelId)) {
		return owner->channel(ChannelId(bare));
	}
	return nullptr;
}

[[nodiscard]] bool InsideEntity(
		const EntitiesInText &entities,
		int from,
		int till) {
	for (const auto &entity : entities) {
		const auto start = entity.offset();
		const auto end = start + entity.length();
		if (from < end && start < till) {
			return true;
		}
	}
	return false;
}

} // namespace

bool Resolvable(not_null<Main::Session*> session, uint64 bare) {
	return ResolvablePeer(session, bare) != nullptr;
}

void Open(not_null<Window::SessionController*> controller, uint64 bare) {
	const auto session = &controller->session();
	const auto russian = UseRussianTexts();
	if (const auto peer = ResolvablePeer(session, bare)) {
		controller->showPeerInfo(peer);
		return;
	}
	// Deliberately not "no such peer": the server does not distinguish "does
	// not exist" from "not for you", and neither may this. What is being said
	// is only what is true - this client has no key for that number.
	const auto id = QString::number(bare);
	controller->show(Ui::MakeConfirmBox({
		.text = (russian
			? u"У этого клиента нет ключа доступа к %1.\n\nTelegram не "
				"позволяет открыть собеседника по одному номеру: его нужно "
				"встретить в чате, по @имени, по номеру телефона или по "
				"ссылке. Номер сам по себе не говорит даже, человек это, "
				"группа или канал."_q
			: u"This client has no access key for %1.\n\nTelegram does not "
				"allow opening a peer by a bare number: it has to be met in "
				"a chat, by @name, by phone number or by link. The number "
				"alone does not even say whether it is a person, a group or "
				"a channel."_q).arg(id),
		.confirmed = [=](Fn<void()> &&close) {
			QGuiApplication::clipboard()->setText(id);
			close();
		},
		.confirmText = (russian ? u"Скопировать ID"_q : u"Copy the ID"_q),
	}));
}

void Linkify(TextWithEntities &text, not_null<Main::Session*> session) {
	if (text.text.isEmpty() || Decoy::Active()) {
		// No fork-only link may appear in the decoy: a number that turns blue
		// where plain Telegram leaves it black is a sign of the fork (I4).
		return;
	}
	auto added = EntitiesInText();
	const auto length = int(text.text.size());
	auto i = 0;
	while (i < length) {
		if (!text.text[i].isDigit()) {
			++i;
			continue;
		}
		auto from = i;
		while (i < length && text.text[i].isDigit()) {
			++i;
		}
		const auto count = i - from;
		if (count < kMinDigits || count > kMaxDigits) {
			continue;
		}
		// Part of something longer - a code, an address, a serial - is left
		// alone: only a number standing on its own is a candidate.
		const auto glued = [](QChar ch) {
			return ch.isLetterOrNumber() || (ch == '.') || (ch == '/');
		};
		if ((from > 0 && glued(text.text.at(from - 1)))
			|| (i < length && glued(text.text.at(i)))) {
			continue;
		}
		if (InsideEntity(text.entities, from, i)) {
			continue;
		}
		const auto bare = text.text.mid(from, count).toULongLong();
		if (!bare || !Resolvable(session, bare)) {
			// The rule the whole feature rests on: no link where nothing can
			// be opened. A dead link on a price would be worse than no link.
			continue;
		}
		added.push_back(EntityInText(
			EntityType::CustomUrl,
			from,
			count,
			LinkPrefix() + QString::number(bare)));
	}
	if (added.empty()) {
		return;
	}
	// EntitiesInText is a QVector here, which takes no iterator range.
	for (const auto &entity : added) {
		text.entities.push_back(entity);
	}
	ranges::sort(text.entities, ranges::less(), &EntityInText::offset);
}

QString LinkPrefix() {
	return u"internal:nova-peer:"_q;
}

} // namespace NovaGram::PeerIds
