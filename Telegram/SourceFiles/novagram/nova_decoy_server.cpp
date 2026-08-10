/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_decoy_server.h"

#include "base/platform/base_platform_info.h"
#include "base/unixtime.h"
#include "core/version.h"
#include "novagram/nova_decoy.h"
#include "novagram/nova_decoy_persona.h"

#include <QtCore/QLocale>

#include <atomic>
#include <optional>

namespace NovaGram::Decoy {
namespace {

// The decoy keeps its own update sequence instead of reading the session's.
// Requests are sent from more than one thread, and a counter that answers
// updates.getState and every later confirmation consistently is both simpler
// and safer than reaching into the session from wherever the call came from.
std::atomic<int> GlobalPts = 1;
std::atomic<int> GlobalSentId = 0;

// Sequential reader over a request body. The scheme is fixed, so reading the
// fields in order is enough; the generated request classes keep their
// arguments private and cannot be read back.
class Reader final {
public:
	Reader(const mtpPrime *from, const mtpPrime *end)
	: _from(from)
	, _end(end) {
	}

	[[nodiscard]] int32 readInt() {
		return (_from < _end) ? *_from++ : 0;
	}

	template <typename Type>
	[[nodiscard]] Type read() {
		auto result = Type();
		if (!result.read(_from, _end)) {
			_from = _end;
		}
		return result;
	}

private:
	const mtpPrime *_from = nullptr;
	const mtpPrime *_end = nullptr;

};

[[nodiscard]] mtpBuffer Serialize(const auto &boxed) {
	auto result = mtpBuffer();
	boxed.write(result);
	return result;
}

// Positive for users, negative for chats and channels, matching the keys the
// persona stores its conversations under.
[[nodiscard]] int64 DialogIdOf(const MTPInputPeer &peer, int64 selfId) {
	return peer.match([&](const MTPDinputPeerSelf &) {
		return selfId;
	}, [](const MTPDinputPeerUser &data) {
		return int64(data.vuser_id().v);
	}, [](const MTPDinputPeerUserFromMessage &data) {
		return int64(data.vuser_id().v);
	}, [](const MTPDinputPeerChat &data) {
		return -int64(data.vchat_id().v);
	}, [](const MTPDinputPeerChannel &data) {
		return -int64(data.vchannel_id().v);
	}, [](const MTPDinputPeerChannelFromMessage &data) {
		return -int64(data.vchannel_id().v);
	}, [](const MTPDinputPeerEmpty &) {
		return int64(0);
	});
}

[[nodiscard]] mtpBuffer Dialogs(Reader &reader) {
	const auto persona = Build();
	const auto flags = reader.readInt();
	if (flags & (1 << 1)) {
		const auto folderId = reader.readInt();
		if (folderId != 0) {
			// The archive is empty. An ordinary answer, not an error.
			return Serialize(MTPmessages_Dialogs(MTP_messages_dialogs(
				MTP_vector<MTPDialog>(),
				MTP_vector<MTPMessage>(),
				MTP_vector<MTPChat>(),
				MTP_vector<MTPUser>())));
		}
	}
	reader.readInt(); // offset_date
	const auto offsetId = reader.readInt();
	if (offsetId != 0) {
		// Anything past the first page is the end of the list.
		return Serialize(MTPmessages_Dialogs(MTP_messages_dialogs(
			MTP_vector<MTPDialog>(),
			MTP_vector<MTPMessage>(),
			MTP_vector<MTPChat>(),
			MTP_vector<MTPUser>())));
	}
	return Serialize(MTPmessages_Dialogs(MTP_messages_dialogs(
		MTP_vector<MTPDialog>(persona.dialogs),
		MTP_vector<MTPMessage>(persona.topMessages),
		MTP_vector<MTPChat>(persona.chats),
		MTP_vector<MTPUser>(persona.users))));
}

[[nodiscard]] mtpBuffer History(Reader &reader) {
	const auto persona = Build();
	const auto peer = reader.read<MTPInputPeer>();
	const auto offsetId = reader.readInt();
	reader.readInt(); // offset_date
	const auto addOffset = reader.readInt();
	const auto limit = reader.readInt();
	const auto maxId = reader.readInt();
	const auto minId = reader.readInt();

	auto messages = QVector<MTPMessage>();
	const auto dialogId = DialogIdOf(peer, persona.selfId);
	const auto i = persona.history.find(dialogId);
	if (i != persona.history.end()) {
		// Newest first, which is the order the client expects back.
		for (const auto &message : i->second) {
			const auto id = message.match([](const MTPDmessage &data) {
				return data.vid().v;
			}, [](const auto &data) {
				return data.vid().v;
			});
			if (offsetId && addOffset >= 0 && id >= offsetId) {
				continue;
			} else if (minId && id <= minId) {
				continue;
			} else if (maxId && id >= maxId) {
				continue;
			}
			messages.push_back(message);
			if (limit > 0 && messages.size() >= limit) {
				break;
			}
		}
	}
	return Serialize(MTPmessages_Messages(MTP_messages_messages(
		MTP_vector<MTPMessage>(messages),
		MTP_vector<MTPForumTopic>(),
		MTP_vector<MTPChat>(persona.chats),
		MTP_vector<MTPUser>(persona.users))));
}

[[nodiscard]] int64 DialogIdOf(const MTPPeer &peer) {
	return peer.match([](const MTPDpeerUser &data) {
		return int64(data.vuser_id().v);
	}, [](const MTPDpeerChat &data) {
		return -int64(data.vchat_id().v);
	}, [](const MTPDpeerChannel &data) {
		return -int64(data.vchannel_id().v);
	});
}

// What the dialog list already says about a conversation. The full chat answer
// has to repeat those counters, and taking them from the same place keeps the
// profile of a channel from contradicting its row in the list.
struct Counters {
	int readInbox = 0;
	int readOutbox = 0;
	int unread = 0;
	int pts = 0;
};

[[nodiscard]] Counters CountersOf(const Persona &persona, int64 dialogId) {
	auto result = Counters();
	for (const auto &dialog : persona.dialogs) {
		dialog.match([&](const MTPDdialog &data) {
			if (DialogIdOf(data.vpeer()) != dialogId) {
				return;
			}
			result.readInbox = data.vread_inbox_max_id().v;
			result.readOutbox = data.vread_outbox_max_id().v;
			result.unread = data.vunread_count().v;
			result.pts = data.vpts().value_or_empty();
		}, [](const auto &) {
		});
	}
	return result;
}

[[nodiscard]] QString AboutOf(const Persona &persona, int64 dialogId) {
	const auto i = persona.about.find(dialogId);
	return (i != persona.about.end()) ? i->second : QString();
}

[[nodiscard]] MTPPeerSettings EmptyPeerSettings() {
	return MTP_peerSettings(
		MTP_flags(MTPDpeerSettings::Flags()),
		MTPint(),
		MTPstring(),
		MTPint(),
		MTPlong(),
		MTPstring(),
		MTPlong(),
		MTPstring(),
		MTPstring(),
		MTPint(),
		MTPint());
}

// Everyone in the decoy is left without the call flags. That is an ordinary
// state on the real network — it is what a user who turned calls off looks
// like — and it keeps the buttons away from a path that has no server behind
// it: a call placed here would sit in "connecting" for ever, which is far more
// visible than the absence of a button.
[[nodiscard]] mtpBuffer FullUser(Reader &reader) {
	const auto persona = Build();
	const auto input = reader.read<MTPInputUser>();
	const auto id = input.match([&](const MTPDinputUserSelf &) {
		return persona.selfId;
	}, [](const MTPDinputUser &data) {
		return int64(data.vuser_id().v);
	}, [](const MTPDinputUserFromMessage &data) {
		return int64(data.vuser_id().v);
	}, [](const MTPDinputUserEmpty &) {
		return int64(0);
	});
	auto found = std::optional<MTPUser>();
	for (const auto &user : persona.users) {
		const auto matches = user.match([&](const MTPDuser &data) {
			return (int64(data.vid().v) == id);
		}, [&](const MTPDuserEmpty &data) {
			return (int64(data.vid().v) == id);
		});
		if (matches) {
			found = user;
			break;
		}
	}
	if (!id || !found) {
		return mtpBuffer();
	}
	const auto about = AboutOf(persona, id);
	using Flag = MTPDuserFull::Flag;
	auto flags = MTPDuserFull::Flags();
	if (!about.isEmpty()) {
		flags |= Flag::f_about;
	}
	return Serialize(MTPusers_UserFull(MTP_users_userFull(
		MTP_userFull(
			MTP_flags(flags),
			MTP_long(id),
			MTP_string(about),
			EmptyPeerSettings(),
			MTPPhoto(),
			MTPPhoto(),
			MTPPhoto(),
			DefaultNotifySettings(),
			MTPBotInfo(),
			MTPint(),
			MTP_int(0),
			MTPint(),
			MTPint(),
			MTPChatTheme(),
			MTPstring(),
			MTPChatAdminRights(),
			MTPChatAdminRights(),
			MTPWallPaper(),
			MTPPeerStories(),
			MTPBusinessWorkHours(),
			MTPBusinessLocation(),
			MTPBusinessGreetingMessage(),
			MTPBusinessAwayMessage(),
			MTPBusinessIntro(),
			MTPBirthday(),
			MTPlong(),
			MTPint(),
			MTPint(),
			MTPStarRefProgram(),
			MTPBotVerification(),
			MTPlong(),
			MTPDisallowedGiftsSettings(),
			MTPStarsRating(),
			MTPStarsRating(),
			MTPint(),
			MTPProfileTab(),
			MTPDocument(),
			MTPTextWithEntities(),
			MTPlong()),
		MTP_vector<MTPChat>(),
		MTP_vector<MTPUser>(QVector<MTPUser>{ *found }))));
}

[[nodiscard]] mtpBuffer FullChat(Reader &reader) {
	const auto persona = Build();
	const auto chatId = int64(reader.read<MTPlong>().v);
	const auto dialogId = -chatId;
	const auto i = persona.members.find(dialogId);
	if (!chatId || i == persona.members.end()) {
		return mtpBuffer();
	}
	const auto anchor = Anchor();
	auto participants = QVector<MTPChatParticipant>();
	participants.reserve(i->second.size());
	for (const auto id : i->second) {
		const auto creator = (id == i->second.front());
		participants.push_back(creator
			? MTP_chatParticipantCreator(
				MTP_flags(MTPDchatParticipantCreator::Flags()),
				MTP_long(id),
				MTPstring())
			: MTP_chatParticipant(
				MTP_flags(MTPDchatParticipant::Flags()),
				MTP_long(id),
				MTP_long(i->second.front()),
				MTP_int(anchor - 150 * 24 * 60 * 60),
				MTPstring()));
	}
	const auto about = AboutOf(persona, dialogId);
	return Serialize(MTPmessages_ChatFull(MTP_messages_chatFull(
		MTP_chatFull(
			MTP_flags(MTPDchatFull::Flags()),
			MTP_long(chatId),
			MTP_string(about),
			MTP_chatParticipants(
				MTP_long(chatId),
				MTP_vector<MTPChatParticipant>(participants),
				MTP_int(1)),
			MTPPhoto(),
			DefaultNotifySettings(),
			MTPExportedChatInvite(),
			MTP_vector<MTPBotInfo>(),
			MTPint(),
			MTPint(),
			MTPInputGroupCall(),
			MTPint(),
			MTPPeer(),
			MTPstring(),
			MTPint(),
			MTPVector<MTPlong>(),
			MTPChatReactions(),
			MTPint()),
		MTP_vector<MTPChat>(persona.chats),
		MTP_vector<MTPUser>(persona.users))));
}

[[nodiscard]] mtpBuffer FullChannel(Reader &reader) {
	const auto persona = Build();
	const auto input = reader.read<MTPInputChannel>();
	const auto channelId = input.match([](const MTPDinputChannel &data) {
		return int64(data.vchannel_id().v);
	}, [](const MTPDinputChannelFromMessage &data) {
		return int64(data.vchannel_id().v);
	}, [](const MTPDinputChannelEmpty &) {
		return int64(0);
	});
	const auto dialogId = -channelId;
	if (!channelId || !persona.history.contains(dialogId)) {
		return mtpBuffer();
	}
	auto participantsCount = 0;
	for (const auto &chat : persona.chats) {
		chat.match([&](const MTPDchannel &data) {
			if (data.vid().v == channelId) {
				participantsCount = data.vparticipants_count().value_or_empty();
			}
		}, [](const auto &) {
		});
	}
	const auto counters = CountersOf(persona, dialogId);
	const auto about = AboutOf(persona, dialogId);
	using Flag = MTPDchannelFull::Flag;
	auto flags = Flag::f_participants_count;
	return Serialize(MTPmessages_ChatFull(MTP_messages_chatFull(
		MTP_channelFull(
			MTP_flags(flags),
			MTP_long(channelId),
			MTP_string(about),
			MTP_int(participantsCount),
			MTPint(),
			MTPint(),
			MTPint(),
			MTPint(),
			MTP_int(counters.readInbox),
			MTP_int(counters.readOutbox),
			MTP_int(counters.unread),
			MTP_photoEmpty(MTP_long(0)),
			DefaultNotifySettings(),
			MTPExportedChatInvite(),
			MTP_vector<MTPBotInfo>(),
			MTPlong(),
			MTPint(),
			MTPint(),
			MTPStickerSet(),
			MTPint(),
			MTPint(),
			MTPlong(),
			MTPChannelLocation(),
			MTPint(),
			MTPint(),
			MTPint(),
			MTP_int(std::max(counters.pts, 1)),
			MTPInputGroupCall(),
			MTPint(),
			MTPVector<MTPstring>(),
			MTPPeer(),
			MTPstring(),
			MTPint(),
			MTPVector<MTPlong>(),
			MTPPeer(),
			MTPChatReactions(),
			MTPint(),
			MTPPeerStories(),
			MTPWallPaper(),
			MTPint(),
			MTPint(),
			MTPStickerSet(),
			MTPBotVerification(),
			MTPint(),
			MTPlong(),
			MTPProfileTab(),
			MTPlong()),
		MTP_vector<MTPChat>(persona.chats),
		MTP_vector<MTPUser>(persona.users))));
}

[[nodiscard]] mtpBuffer Contacts() {
	const auto persona = Build();
	return Serialize(MTPcontacts_Contacts(MTP_contacts_contacts(
		MTP_vector<MTPContact>(persona.contacts),
		MTP_int(int(persona.contacts.size())),
		MTP_vector<MTPUser>(persona.users))));
}

[[nodiscard]] int UnreadCount() {
	auto result = 0;
	for (const auto &dialog : Snapshot().dialogs) {
		result += dialog.match([](const MTPDdialog &data) {
			return data.vunread_count().v;
		}, [](const auto &) {
			return 0;
		});
	}
	return result;
}

[[nodiscard]] mtpBuffer State() {
	return Serialize(MTPupdates_State(MTP_updates_state(
		MTP_int(GlobalPts.load()),
		MTP_int(1),
		MTP_int(base::unixtime::now()),
		MTP_int(1),
		MTP_int(UnreadCount()))));
}

// Nothing happened since the client last asked, which is true: there is no
// server. Answering matters because an unanswered difference leaves the client
// stuck showing "Updating…" over the chat list.
[[nodiscard]] mtpBuffer NoDifference() {
	return Serialize(MTPupdates_Difference(MTP_updates_differenceEmpty(
		MTP_int(base::unixtime::now()),
		MTP_int(1))));
}

// Same for a channel.
[[nodiscard]] mtpBuffer NoChannelDifference(Reader &reader) {
	reader.readInt(); // flags
	reader.read<MTPInputChannel>();
	reader.read<MTPChannelMessagesFilter>();
	const auto pts = reader.readInt();
	using Flag = MTPDupdates_channelDifferenceEmpty::Flag;
	return Serialize(MTPupdates_ChannelDifference(
		MTP_updates_channelDifferenceEmpty(
			MTP_flags(Flag::f_final),
			MTP_int(std::max(pts, 1)),
			MTPint())));
}

[[nodiscard]] mtpBuffer ReadAcknowledged() {
	return Serialize(MTPmessages_AffectedMessages(
		MTP_messages_affectedMessages(
			MTP_int(GlobalPts.load()),
			MTP_int(0))));
}

// Confirms a message the user typed into the decoy. It is stored locally by
// the client and goes nowhere. Without this the message would keep the sending
// clock forever, which contradicts a chat list that says there is a connection.
[[nodiscard]] mtpBuffer Sent() {
	auto id = GlobalSentId.load();
	if (!id) {
		id = Snapshot().lastMessageId;
	}
	GlobalSentId = ++id;
	using Flag = MTPDupdateShortSentMessage::Flag;
	return Serialize(MTPUpdates(MTP_updateShortSentMessage(
		MTP_flags(Flag::f_out),
		MTP_int(id),
		MTP_int(++GlobalPts),
		MTP_int(1),
		MTP_int(base::unixtime::now()),
		MTPMessageMedia(),
		MTPVector<MTPMessageEntity>(),
		MTPint())));
}

// One session, this computer, signed in long ago. Without an answer the
// "Devices" page in settings spins for ever, which is the most visible place
// where an unanswered request shows through.
[[nodiscard]] mtpBuffer Authorizations() {
	using Flag = MTPDauthorization::Flag;
	const auto anchor = Anchor();
	auto sessions = QVector<MTPAuthorization>();
	sessions.push_back(MTP_authorization(
		MTP_flags(Flag::f_current | Flag::f_official_app),
		MTP_long(0),
		MTP_string(Platform::DeviceModelPretty()),
		MTP_string(QString()), // platform, derived by the server in real life
		MTP_string(Platform::SystemVersionPretty()),
		MTP_int(0),
		MTP_string(u"Telegram Desktop"_q),
		MTP_string(QString::fromLatin1(AppVersionStr)),
		MTP_int(anchor - 130 * 24 * 60 * 60),
		MTP_int(base::unixtime::now()),
		// The address is left empty on purpose: an invented one is a claim
		// that can be checked and found wrong, while the country taken from
		// the system locale is the country the machine really is in.
		MTP_string(QString()),
		MTP_string(QLocale::system().nativeCountryName()),
		MTP_string(QString())));
	return Serialize(MTPaccount_Authorizations(MTP_account_authorizations(
		MTP_int(180),
		MTP_vector<MTPAuthorization>(sessions))));
}

// The client waits for this one before it finishes signing out, and an
// unanswered logout leaves it half way: the session is neither alive nor gone.
// Nothing leaves the machine either way, there is no server to tell.
[[nodiscard]] mtpBuffer LoggedOut() {
	return Serialize(MTPauth_LoggedOut(MTP_auth_loggedOut(
		MTP_flags(MTPDauth_loggedOut::Flags()),
		MTPbytes())));
}

// Every privacy key answers the same: visible to contacts. Without an answer
// the rows in "Privacy and Security" stay blank, where a real client always
// shows a value.
[[nodiscard]] mtpBuffer PrivacyRules() {
	auto rules = QVector<MTPPrivacyRule>();
	rules.push_back(MTP_privacyValueAllowContacts());
	return Serialize(MTPaccount_PrivacyRules(MTP_account_privacyRules(
		MTP_vector<MTPPrivacyRule>(rules),
		MTP_vector<MTPChat>(),
		MTP_vector<MTPUser>())));
}

} // namespace

mtpBuffer Respond(const mtpPrime *from, const mtpPrime *end) {
	if (from >= end) {
		return mtpBuffer();
	}
	auto reader = Reader(from + 1, end);
	switch (mtpTypeId(*from)) {
	case mtpc_messages_getDialogs: return Dialogs(reader);
	case mtpc_messages_getHistory: return History(reader);
	case mtpc_users_getFullUser: return FullUser(reader);
	case mtpc_messages_getFullChat: return FullChat(reader);
	case mtpc_channels_getFullChannel: return FullChannel(reader);
	case mtpc_contacts_getContacts: return Contacts();
	case mtpc_updates_getState: return State();
	case mtpc_updates_getDifference: return NoDifference();
	case mtpc_updates_getChannelDifference:
		return NoChannelDifference(reader);
	case mtpc_messages_readHistory: return ReadAcknowledged();
	case mtpc_messages_sendMessage: return Sent();
	case mtpc_account_getAuthorizations: return Authorizations();
	case mtpc_auth_logOut: return LoggedOut();
	case mtpc_account_getPrivacy: return PrivacyRules();
	}
	return mtpBuffer();
}

} // namespace NovaGram::Decoy
