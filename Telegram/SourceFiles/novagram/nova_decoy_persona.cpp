/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_decoy_persona.h"

#include "novagram/nova_decoy.h"
#include "novagram/nova_pin.h"

#include <algorithm>
#include <random>

namespace NovaGram::Decoy {
namespace {

constexpr auto kMinute = 60;
constexpr auto kHour = 60 * kMinute;
constexpr auto kDay = 24 * kHour;

// One line of a conversation. Speaker 0 is the account owner, 1 and above are
// the other participants of that conversation in order.
struct Line {
	int speaker = 0;
	const char *en = nullptr;
	const char *ru = nullptr;
};

// The conversations are deliberately dull: errands, shifts, a parcel, a broken
// washing machine. A decoy full of interesting content invites the very
// question it exists to avoid. Nothing here names NovaGram, security, privacy
// or anything a reader could recognise as staged. The same conversations run
// on Android, so the two platforms tell one story.

const Line kColleague[] = {
	{ 1, "are you coming in tomorrow?", "завтра будешь в офисе?" },
	{ 0, "yes, in the morning", "да, с утра" },
	{ 1, "good, the report is on the shared drive", "хорошо, отчёт лежит на общем диске" },
	{ 0, "got it, I will look at it before the meeting", "принял, посмотрю до совещания" },
	{ 1, "the meeting moved to 11", "совещание перенесли на 11" },
	{ 0, "ok", "ок" },
	{ 1, "and Pavel asked for last month numbers", "и Павел просил цифры за прошлый месяц" },
	{ 0, "I will send them today", "сегодня отправлю" },
	{ 1, "thanks", "спасибо" },
	{ 0, "no problem", "не за что" },
	{ 1, "see you tomorrow", "до завтра" },
};

const Line kFamily[] = {
	{ 1, "who is picking up the parcel?", "кто заберёт посылку?" },
	{ 0, "I can, after work", "я смогу, после работы" },
	{ 2, "do not forget the bread", "хлеб не забудь" },
	{ 0, "ok", "хорошо" },
	{ 1, "dinner at seven", "ужин в семь" },
	{ 2, "I will be late, start without me", "я задержусь, начинайте без меня" },
	{ 0, "fine", "ладно" },
	{ 1, "there is soup in the fridge", "суп в холодильнике" },
	{ 0, "thanks", "спасибо" },
	{ 2, "the car is in the shop until Friday", "машина в сервисе до пятницы" },
	{ 1, "then we go by bus", "значит на автобусе" },
	{ 0, "I will manage", "разберусь" },
};

const Line kDelivery[] = {
	{ 1, "Your order has been shipped. Expected delivery Thursday.", "Заказ отправлен. Ожидаемая доставка в четверг." },
	{ 1, "Your parcel has arrived at the pickup point. Code 4471.", "Ваша посылка прибыла в пункт выдачи. Код 4471." },
	{ 1, "The pickup point works until 21:00.", "Пункт выдачи работает до 21:00." },
	{ 1, "Your parcel has been handed over. Thank you for your order.", "Посылка выдана. Спасибо за заказ." },
};

const Line kFriend[] = {
	{ 1, "gym tonight?", "сегодня в зал?" },
	{ 0, "cannot, working late", "не могу, работаю допоздна" },
	{ 1, "tomorrow then", "тогда завтра" },
	{ 0, "tomorrow works", "завтра нормально" },
	{ 1, "same time", "в то же время" },
	{ 0, "yes", "да" },
	{ 1, "bring the ball, the one at the club is flat", "мяч возьми, клубный спущен" },
	{ 0, "ok", "ок" },
	{ 1, "see you", "давай" },
};

const Line kBuilding[] = {
	{ 1, "hot water off on Tuesday from 9 to 18", "во вторник отключат горячую воду с 9 до 18" },
	{ 2, "again?", "опять?" },
	{ 1, "that is what the notice says", "так в объявлении написано" },
	{ 0, "thanks for the warning", "спасибо, что предупредили" },
	{ 2, "someone is parked across two spaces again", "снова кто-то встал на два места" },
	{ 1, "the silver one?", "серая?" },
	{ 2, "yes", "да" },
	{ 0, "I will tell the manager", "скажу управляющему" },
	{ 1, "the lift is fixed", "лифт починили" },
	{ 2, "finally", "наконец-то" },
};

const Line kChannel[] = {
	{ 1, "Coffee beans 1 kg, 30 percent off until Sunday.", "Кофе в зёрнах 1 кг, минус 30 процентов до воскресенья." },
	{ 1, "Winter tyres, second set at half price.", "Зимние шины, второй комплект за полцены." },
	{ 1, "New bakery on the corner, free tea before 10.", "Новая пекарня на углу, чай бесплатно до 10." },
	{ 1, "Household chemicals, three for the price of two.", "Бытовая химия, три по цене двух." },
	{ 1, "Grocery delivery is free this week from 2000.", "Доставка продуктов на этой неделе бесплатно от 2000." },
};

const Line kRepairs[] = {
	{ 1, "Good afternoon. I can come on Saturday morning.", "Добрый день. Могу приехать в субботу утром." },
	{ 0, "Saturday morning works", "суббота утром подходит" },
	{ 1, "Between 10 and 12, is that ok?", "С 10 до 12, устроит?" },
	{ 0, "yes", "да" },
	{ 1, "Is it the same washing machine?", "Машинка та же?" },
	{ 0, "yes, it stopped draining", "да, перестала сливать" },
	{ 1, "Understood, I will bring the pump.", "Понял, привезу помпу." },
	{ 0, "thank you", "спасибо" },
};

const Line kSaved[] = {
	{ 0, "Wi-Fi guest: 8462 0194", "Wi-Fi гостевой: 8462 0194" },
	{ 0, "milk, bread, coffee, batteries", "молоко, хлеб, кофе, батарейки" },
	{ 0, "meter readings before the 25th", "передать показания счётчика до 25-го" },
};

struct Name {
	const char *firstEn = nullptr;
	const char *firstRu = nullptr;
	const char *lastEn = nullptr;
	const char *lastRu = nullptr;
};

// The other members of the building group. They are not in the address book
// and have no conversation: in a real house chat most participants are exactly
// that, a name and nothing else.
const Name kNeighbours[] = {
	{ "Anna", "Анна", "Sokolova", "Соколова" },
	{ "Boris", "Борис", "", "" },
	{ "Galina", "Галина", "", "" },
	{ "Igor", "Игорь", "Panov", "Панов" },
	{ "Nina", "Нина", "", "" },
	{ "Pavel", "Павел", "", "" },
	{ "Raisa", "Раиса", "", "" },
	{ "Roman", "Роман", "", "" },
	{ "Sofia", "София", "", "" },
	{ "Tamara", "Тамара", "", "" },
	{ "Vadim", "Вадим", "", "" },
	{ "Vera", "Вера", "Titova", "Титова" },
	{ "Yuri", "Юрий", "", "" },
	{ "Zoya", "Зоя", "", "" },
	{ "Maxim", "Максим", "", "" },
};

// The first seven also have conversations; the rest are contacts without one,
// as in any real address book.
const Name kContacts[] = {
	{ "Marina", "Марина", "Kovaleva", "Ковалёва" },
	{ "Ilya", "Илья", "", "" },
	{ "Mom", "Мама", "", "" },
	{ "Dad", "Папа", "", "" },
	{ "Sergey", "Сергей", "repairs", "мастер" },
	{ "Olga", "Ольга", "neighbour", "соседка" },
	{ "Victor", "Виктор", "neighbour", "сосед" },
	{ "Kate", "Катя", "Miller", "Мельникова" },
	{ "Dmitry", "Дмитрий", "Orlov", "Орлов" },
	{ "Lena", "Лена", "", "" },
};

[[nodiscard]] QString Pick(const char *en, const char *ru) {
	return QString::fromUtf8(UseRussianTexts() ? ru : en);
}

// A message before it gets an identifier. Building the whole conversation
// first and numbering it afterwards keeps the identifiers growing with time
// across the account, the way Telegram numbers messages outside channels.
struct Draft {
	int id = 0;
	bool out = false;
	bool post = false;
	int64 from = 0;
	int64 peer = 0; // Positive for users, negative for chats and channels.
	TimeId date = 0;
	QString text;
	int views = 0;
	int forwards = 0;
};

[[nodiscard]] MTPPeer PeerOf(int64 id, bool channel) {
	return (id > 0)
		? MTP_peerUser(MTP_long(id))
		: channel
		? MTP_peerChannel(MTP_long(-id))
		: MTP_peerChat(MTP_long(-id));
}

[[nodiscard]] MTPMessage Compose(const Draft &draft, bool channel) {
	using Flag = MTPDmessage::Flag;
	auto flags = MTPDmessage::Flags();
	if (draft.out) {
		flags |= Flag::f_out;
	}
	if (draft.post) {
		flags |= Flag::f_post | Flag::f_views | Flag::f_forwards;
	}
	if (draft.from) {
		flags |= Flag::f_from_id;
	}
	return MTP_message(
		MTP_flags(flags),
		MTP_int(draft.id),
		draft.from ? MTP_peerUser(MTP_long(draft.from)) : MTPPeer(),
		MTPint(),
		MTPstring(),
		PeerOf(draft.peer, channel),
		MTPPeer(),
		MTPMessageFwdHeader(),
		MTPlong(),
		MTPlong(),
		MTPPeer(),
		MTPMessageReplyHeader(),
		MTP_int(draft.date),
		MTP_string(draft.text),
		MTPMessageMedia(),
		MTPReplyMarkup(),
		MTPVector<MTPMessageEntity>(),
		MTP_int(draft.views),
		MTP_int(draft.forwards),
		MTPMessageReplies(),
		MTPint(),
		MTPstring(),
		MTPlong(),
		MTPMessageReactions(),
		MTPVector<MTPRestrictionReason>(),
		MTPint(),
		MTPint(),
		MTPlong(),
		MTPFactCheck(),
		MTPint(),
		MTPlong(),
		MTPSuggestedPost(),
		MTPint(),
		MTPstring(),
		MTPRichMessage());
}

class Builder final {
public:
	Builder();

	[[nodiscard]] Persona take();

private:
	[[nodiscard]] uint64 next(uint64 bound);
	[[nodiscard]] QString neighbouringPhone();
	[[nodiscard]] int step(int spacing);

	void buildSelf();
	[[nodiscard]] int64 addUser(
		const QString &first,
		const QString &last,
		bool contact);
	[[nodiscard]] int64 addGroup(
		const QString &title,
		int participants,
		int age);
	[[nodiscard]] int64 addChannel(const QString &title, int age);

	void conversation(
		int64 dialogPeer,
		bool channel,
		const int64 *participants,
		const Line *lines,
		int count,
		int lastAgo,
		int spacing,
		int unread,
		int pts);

	Persona _result;
	std::mt19937_64 _random;
	TimeId _anchor = 0;
	int _nextMessageId = 0;

};

Builder::Builder()
: _random(Seed())
, _anchor(Anchor()) {
	_nextMessageId = 4000 + int(next(20000));

	buildSelf();

	auto people = QVector<int64>();
	for (const auto &name : kContacts) {
		people.push_back(addUser(
			Pick(name.firstEn, name.firstRu),
			Pick(name.lastEn, name.lastRu),
			true));
	}
	const auto colleague = people[0];
	const auto friendly = people[1];
	const auto mother = people[2];
	const auto father = people[3];
	const auto repairman = people[4];
	const auto neighbourOne = people[5];
	const auto neighbourTwo = people[6];

	// Not a contact: a shop notification account nobody adds to the address
	// book, and every real Telegram has one or two of them.
	const auto delivery = addUser(
		Pick("Delivery", "Доставка"),
		QString(),
		false);

	const auto family = addGroup(Pick("Family", "Семья"), 3, 40 * kDay);
	const auto building = addGroup(
		Pick("Building 14, entrance 2", "Дом 14, подъезд 2"),
		18,
		200 * kDay);
	const auto deals = addChannel(
		Pick("Deals nearby", "Скидки рядом"),
		130 * kDay);

	const int64 familyMembers[] = { mother, father };
	const int64 neighbours[] = { neighbourOne, neighbourTwo };

	// Ordered newest first, which is also the order the dialog list shows.
	conversation(colleague, false, nullptr, kColleague,
		int(std::size(kColleague)), 42 * kMinute, 25 * kMinute, 0, 0);
	conversation(-family, false, familyMembers, kFamily,
		int(std::size(kFamily)), 3 * kHour + 20 * kMinute, 20 * kMinute, 0, 0);
	conversation(delivery, false, nullptr, kDelivery,
		int(std::size(kDelivery)), 7 * kHour, 9 * kHour, 1, 0);
	conversation(friendly, false, nullptr, kFriend,
		int(std::size(kFriend)), 21 * kHour, 35 * kMinute, 0, 0);
	conversation(-building, false, neighbours, kBuilding,
		int(std::size(kBuilding)), 2 * kDay + 4 * kHour, 50 * kMinute, 0, 0);
	conversation(-deals, true, nullptr, kChannel,
		int(std::size(kChannel)), 3 * kDay + 6 * kHour, 22 * kHour, 2,
		40 + int(next(400)));
	conversation(repairman, false, nullptr, kRepairs,
		int(std::size(kRepairs)), 6 * kDay + 3 * kHour, 40 * kMinute, 0, 0);
	conversation(_result.selfId, false, nullptr, kSaved,
		int(std::size(kSaved)), 9 * kDay, 2 * kDay, 0, 0);

	_result.about.emplace(repairman, Pick(
		"Washing machines and fridges. Call-outs across the city.",
		"Стиральные машины и холодильники. Выезд по городу."));
	_result.about.emplace(delivery, Pick(
		"Order and delivery notifications.",
		"Уведомления о заказах и доставке."));
	_result.about.emplace(people[7], Pick("rarely here", "тут редко"));
	_result.about.emplace(-building, Pick(
		"Entrance notices. Adverts are removed.",
		"Объявления по подъезду. Реклама удаляется."));
	_result.about.emplace(-deals, Pick(
		"Discounts and offers nearby. No spam.",
		"Скидки и акции рядом. Без спама."));

	_result.members.emplace(
		-family,
		QVector<int64>{ _result.selfId, mother, father });

	// Drawn last on purpose. Everything above keeps the identifiers it had
	// before these were added, so a decoy armed by an earlier build still
	// generates the same people and the same conversations.
	auto residents = QVector<int64>{ _result.selfId, neighbourOne, neighbourTwo };
	for (const auto &name : kNeighbours) {
		residents.push_back(addUser(
			Pick(name.firstEn, name.firstRu),
			Pick(name.lastEn, name.lastRu),
			false));
	}
	_result.members.emplace(-building, residents);
}

uint64 Builder::next(uint64 bound) {
	return bound ? (_random() % bound) : 0;
}

int Builder::step(int spacing) {
	return kMinute + int(next(uint64(std::max(2 * kMinute, spacing))));
}

void Builder::buildSelf() {
	auto first = SelfFirstName();
	auto last = SelfLastName();
	if (first.isEmpty()) {
		first = Pick("Alex", "Алексей");
		last = QString();
	}
	auto phone = SelfPhone();
	if (phone.isEmpty()) {
		// Only when the wipe could not capture the real one, which on desktop
		// is the usual case: the emergency PIN is entered before anything is
		// decrypted, so there is no account to read a number from. An account
		// with no number at all in the settings is more obviously wrong than a
		// made up one.
		phone = Pick("44", "79");
		while (phone.size() < 11) {
			phone += QString::number(next(10));
		}
	}
	_result.selfId = int64(100'000'000 + next(800'000'000));

	using Flag = MTPDuser::Flag;
	auto flags = Flag::f_self
		| Flag::f_access_hash
		| Flag::f_first_name
		| Flag::f_phone;
	if (!last.isEmpty()) {
		flags |= Flag::f_last_name;
	}
	_result.self = MTP_user(
		MTP_flags(flags),
		MTP_long(_result.selfId),
		MTP_long(int64(_random())),
		MTP_string(first),
		MTP_string(last),
		MTPstring(),
		MTP_string(phone),
		MTPUserProfilePhoto(),
		MTPUserStatus(),
		MTPint(),
		MTPVector<MTPRestrictionReason>(),
		MTPstring(),
		MTPstring(),
		MTPEmojiStatus(),
		MTPVector<MTPUsername>(),
		MTPRecentStory(),
		MTPPeerColor(),
		MTPPeerColor(),
		MTPint(),
		MTPlong(),
		MTPlong(),
		MTPlong());
	_result.users.push_back(_result.self);
}

QString Builder::neighbouringPhone() {
	// A number shaped like the owner's own: same country and operator part,
	// different subscriber digits. Contacts from another country would be the
	// kind of detail that makes a decoy fall apart under a glance.
	const auto own = SelfPhone();
	auto result = (own.size() > 7)
		? own.left(own.size() - 7)
		: Pick("44", "79");
	for (auto i = 0; i != 7; ++i) {
		result += QString::number(next(10));
	}
	return result;
}

int64 Builder::addUser(
		const QString &first,
		const QString &last,
		bool contact) {
	using Flag = MTPDuser::Flag;
	auto flags = Flag::f_access_hash | Flag::f_first_name | Flag::f_status;
	if (!last.isEmpty()) {
		flags |= Flag::f_last_name;
	}
	auto phone = QString();
	if (contact) {
		flags |= Flag::f_contact | Flag::f_mutual_contact | Flag::f_phone;
		phone = neighbouringPhone();
	}
	const auto id = int64(100'000'000 + next(800'000'000));
	const auto wasOnline = _anchor - (5 * kMinute + int(next(3 * kDay)));
	_result.users.push_back(MTP_user(
		MTP_flags(flags),
		MTP_long(id),
		MTP_long(int64(_random())),
		MTP_string(first),
		MTP_string(last),
		MTPstring(),
		MTP_string(phone),
		MTPUserProfilePhoto(),
		MTP_userStatusOffline(MTP_int(wasOnline)),
		MTPint(),
		MTPVector<MTPRestrictionReason>(),
		MTPstring(),
		MTPstring(),
		MTPEmojiStatus(),
		MTPVector<MTPUsername>(),
		MTPRecentStory(),
		MTPPeerColor(),
		MTPPeerColor(),
		MTPint(),
		MTPlong(),
		MTPlong(),
		MTPlong()));
	if (contact) {
		_result.contacts.push_back(MTP_contact(MTP_long(id), MTP_boolTrue()));
	}
	return id;
}

int64 Builder::addGroup(
		const QString &title,
		int participants,
		int age) {
	const auto id = int64(1'000'000 + next(900'000'000));
	_result.chats.push_back(MTP_chat(
		MTP_flags(MTPDchat::Flags()),
		MTP_long(id),
		MTP_string(title),
		MTP_chatPhotoEmpty(),
		MTP_int(participants),
		MTP_int(_anchor - age),
		MTP_int(1),
		MTPInputChannel(),
		MTPChatAdminRights(),
		MTPChatBannedRights()));
	return id;
}

int64 Builder::addChannel(const QString &title, int age) {
	using Flag = MTPDchannel::Flag;
	const auto id = int64(1'000'000'000 + next(900'000'000));
	_result.chats.push_back(MTP_channel(
		MTP_flags(Flag::f_access_hash
			| Flag::f_broadcast
			| Flag::f_participants_count),
		MTP_long(id),
		MTP_long(int64(_random())),
		MTP_string(title),
		MTPstring(),
		MTP_chatPhotoEmpty(),
		MTP_int(_anchor - age),
		MTPVector<MTPRestrictionReason>(),
		MTPChatAdminRights(),
		MTPChatBannedRights(),
		MTPChatBannedRights(),
		MTP_int(400 + int(next(9000))),
		MTPVector<MTPUsername>(),
		MTPRecentStory(),
		MTPPeerColor(),
		MTPPeerColor(),
		MTPEmojiStatus(),
		MTPint(),
		MTPint(),
		MTPlong(),
		MTPlong(),
		MTPlong(),
		MTPlong()));
	return id;
}

void Builder::conversation(
		int64 dialogPeer,
		bool channel,
		const int64 *participants,
		const Line *lines,
		int count,
		int lastAgo,
		int spacing,
		int unread,
		int pts) {
	auto drafts = QVector<Draft>();
	auto date = _anchor - lastAgo;
	for (auto i = count - 1; i >= 0; --i) {
		const auto &line = lines[i];
		const auto out = (line.speaker == 0);
		auto draft = Draft();
		draft.date = date;
		draft.text = Pick(line.en, line.ru);
		draft.out = out;
		if (channel) {
			draft.post = true;
			draft.peer = dialogPeer;
			draft.out = false;
			draft.views = 300 + int(next(5000));
			draft.forwards = int(next(20));
		} else if (dialogPeer < 0) {
			draft.from = out
				? _result.selfId
				: participants[line.speaker - 1];
			draft.peer = dialogPeer;
		} else {
			// Incoming private messages carry the owner as the peer and the
			// other side as the sender: that is how the client works the
			// conversation out.
			draft.from = out ? _result.selfId : dialogPeer;
			draft.peer = out ? dialogPeer : _result.selfId;
		}
		drafts.push_back(draft);
		date -= step(spacing);
	}
	std::reverse(drafts.begin(), drafts.end());

	auto incoming = QVector<int>();
	auto readOutbox = 0;
	for (auto &draft : drafts) {
		draft.id = _nextMessageId++;
		if (draft.out) {
			readOutbox = draft.id;
		} else {
			incoming.push_back(draft.id);
		}
	}
	_result.lastMessageId = std::max(_result.lastMessageId, _nextMessageId - 1);
	// A gap before the next conversation: real numbering also skips the
	// identifiers of everything that happened elsewhere in between.
	_nextMessageId += 1 + int(next(40));

	const auto unreadCount = std::min(unread, int(incoming.size()));
	const auto topId = drafts.back().id;
	const auto readInbox = unreadCount
		? (incoming[incoming.size() - unreadCount] - 1)
		: topId;

	auto messages = QVector<MTPMessage>();
	messages.reserve(drafts.size());
	for (const auto &draft : drafts) {
		messages.push_back(Compose(draft, channel));
	}

	using Flag = MTPDdialog::Flag;
	auto flags = MTPDdialog::Flags();
	if (pts) {
		flags |= Flag::f_pts;
	}
	_result.dialogs.push_back(MTP_dialog(
		MTP_flags(flags),
		PeerOf(dialogPeer, channel),
		MTP_int(topId),
		MTP_int(readInbox),
		MTP_int(readOutbox),
		MTP_int(unreadCount),
		MTP_int(0),
		MTP_int(0),
		MTP_int(0),
		DefaultNotifySettings(),
		MTP_int(pts),
		MTPDraftMessage(),
		MTPint(),
		MTPint()));
	_result.topMessages.push_back(messages.back());

	std::reverse(messages.begin(), messages.end());
	_result.history.emplace(dialogPeer, std::move(messages));
}

Persona Builder::take() {
	return std::move(_result);
}

} // namespace

Persona Build() {
	auto builder = Builder();
	return builder.take();
}

const Persona &Snapshot() {
	static const auto result = Build();
	return result;
}

MTPPeerNotifySettings DefaultNotifySettings() {
	return MTP_peerNotifySettings(
		MTP_flags(MTPDpeerNotifySettings::Flags()),
		MTPBool(),
		MTPBool(),
		MTPint(),
		MTPNotificationSound(),
		MTPNotificationSound(),
		MTPNotificationSound(),
		MTPBool(),
		MTPBool(),
		MTPNotificationSound(),
		MTPNotificationSound(),
		MTPNotificationSound());
}

} // namespace NovaGram::Decoy
