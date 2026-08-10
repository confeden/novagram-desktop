/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/sender.h"

class ApiWrap;

namespace Main {
class Session;
} // namespace Main

namespace Api {

enum class ReactionsNotifyFrom : uchar {
	None,
	Contacts,
	All,
};

class ReactionsNotifySettings final {
public:
	explicit ReactionsNotifySettings(not_null<ApiWrap*> api);

	void reload(Fn<void()> callback = nullptr);

	void updateMessagesFrom(ReactionsNotifyFrom value);
	void updatePollVotesFrom(ReactionsNotifyFrom value);
	void setAllFrom(ReactionsNotifyFrom value);
	void updateShowPreviews(bool value);

	[[nodiscard]] ReactionsNotifyFrom messagesFromCurrent() const;
	[[nodiscard]] rpl::producer<ReactionsNotifyFrom> messagesFrom() const;
	[[nodiscard]] ReactionsNotifyFrom pollVotesFromCurrent() const;
	[[nodiscard]] rpl::producer<ReactionsNotifyFrom> pollVotesFrom() const;
	[[nodiscard]] bool showPreviewsCurrent() const;
	[[nodiscard]] rpl::producer<bool> showPreviews() const;

	[[nodiscard]] bool enabledCurrent() const;
	[[nodiscard]] rpl::producer<bool> enabled() const;

private:
	void apply(const MTPReactionsNotifySettings &settings);
	void save();

	const not_null<Main::Session*> _session;
	MTP::Sender _api;
	mtpRequestId _requestId = 0;
	rpl::variable<ReactionsNotifyFrom> _messagesFrom
		= ReactionsNotifyFrom::All;
	rpl::variable<ReactionsNotifyFrom> _storiesFrom
		= ReactionsNotifyFrom::All;
	rpl::variable<ReactionsNotifyFrom> _pollVotesFrom
		= ReactionsNotifyFrom::All;
	rpl::variable<bool> _showPreviews = true;
	// NovaGram: `sound` is a required field of reactionsNotifySettings, so
	// every save writes one. Upstream parses no sound and always writes the
	// default, which only ever cost the user their choice when they changed a
	// reactions setting by hand; novagram/nova_notify_previews now saves by
	// itself, which would have made that silent and automatic.
	MTPNotificationSound _sound = MTP_notificationSoundDefault();
	std::vector<Fn<void()>> _callbacks;

};

} // namespace Api
