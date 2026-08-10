/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_reactions_notify_settings.h"

#include "apiwrap.h"
#include "main/main_session.h"
#include "novagram/nova_notify_previews.h"

namespace Api {
namespace {

[[nodiscard]] ReactionsNotifyFrom ParseFrom(
		const MTPReactionNotificationsFrom &from) {
	return from.match([](const MTPDreactionNotificationsFromContacts &) {
		return ReactionsNotifyFrom::Contacts;
	}, [](const MTPDreactionNotificationsFromAll &) {
		return ReactionsNotifyFrom::All;
	});
}

[[nodiscard]] MTPReactionNotificationsFrom SerializeFrom(
		ReactionsNotifyFrom from) {
	switch (from) {
	case ReactionsNotifyFrom::Contacts:
		return MTP_reactionNotificationsFromContacts();
	case ReactionsNotifyFrom::All:
		return MTP_reactionNotificationsFromAll();
	}
	Unexpected("Value in SerializeFrom.");
}

} // namespace

ReactionsNotifySettings::ReactionsNotifySettings(not_null<ApiWrap*> api)
: _session(&api->session())
, _api(&api->instance()) {
}

void ReactionsNotifySettings::reload(Fn<void()> callback) {
	if (callback) {
		_callbacks.push_back(std::move(callback));
	}
	if (_requestId) {
		return;
	}
	_requestId = _api.request(MTPaccount_GetReactionsNotifySettings(
	)).done([=](const MTPReactionsNotifySettings &result) {
		_requestId = 0;
		apply(result);
		for (const auto &callback : base::take(_callbacks)) {
			callback();
		}
	}).fail([=] {
		_requestId = 0;
		for (const auto &callback : base::take(_callbacks)) {
			callback();
		}
	}).send();
}

void ReactionsNotifySettings::updateMessagesFrom(ReactionsNotifyFrom value) {
	_messagesFrom = value;
	save();
}

void ReactionsNotifySettings::updatePollVotesFrom(
		ReactionsNotifyFrom value) {
	_pollVotesFrom = value;
	save();
}

void ReactionsNotifySettings::setAllFrom(ReactionsNotifyFrom value) {
	_messagesFrom = value;
	_pollVotesFrom = value;
	save();
}

void ReactionsNotifySettings::updateShowPreviews(bool value) {
	_showPreviews = value;
	// NovaGram: this is the user saying what they want to see on this screen,
	// and it is the only place that says it. While the promise is on the
	// server holds false whatever the user chose, so the answer cannot be read
	// back from it at the next start - it is remembered here instead.
	NovaGram::NoteReactionsSenderShown(_session, value);
	save();
}

ReactionsNotifyFrom ReactionsNotifySettings::messagesFromCurrent() const {
	return _messagesFrom.current();
}

rpl::producer<ReactionsNotifyFrom> ReactionsNotifySettings::messagesFrom() const {
	return _messagesFrom.value();
}

ReactionsNotifyFrom ReactionsNotifySettings::pollVotesFromCurrent() const {
	return _pollVotesFrom.current();
}

rpl::producer<ReactionsNotifyFrom> ReactionsNotifySettings::pollVotesFrom() const {
	return _pollVotesFrom.value();
}

bool ReactionsNotifySettings::showPreviewsCurrent() const {
	return _showPreviews.current();
}

rpl::producer<bool> ReactionsNotifySettings::showPreviews() const {
	return _showPreviews.value();
}

bool ReactionsNotifySettings::enabledCurrent() const {
	return (_messagesFrom.current() != ReactionsNotifyFrom::None)
		|| (_pollVotesFrom.current() != ReactionsNotifyFrom::None);
}

rpl::producer<bool> ReactionsNotifySettings::enabled() const {
	return rpl::combine(
		_messagesFrom.value(),
		_pollVotesFrom.value()
	) | rpl::map([](
			ReactionsNotifyFrom messages,
			ReactionsNotifyFrom pollVotes) {
		return (messages != ReactionsNotifyFrom::None)
			|| (pollVotes != ReactionsNotifyFrom::None);
	}) | rpl::distinct_until_changed();
}

void ReactionsNotifySettings::apply(
		const MTPReactionsNotifySettings &settings) {
	const auto &data = settings.data();
	const auto messages = data.vmessages_notify_from();
	const auto stories = data.vstories_notify_from();
	const auto pollVotes = data.vpoll_votes_notify_from();
	_messagesFrom = messages
		? ParseFrom(*messages)
		: ReactionsNotifyFrom::None;
	_storiesFrom = stories
		? ParseFrom(*stories)
		: ReactionsNotifyFrom::None;
	_pollVotesFrom = pollVotes
		? ParseFrom(*pollVotes)
		: ReactionsNotifyFrom::None;
	// NovaGram: read out of `data` before anything else, and before the call
	// below. That call reaches into novagram/nova_notify_previews, which can
	// answer by asking for a save() there and then - and a save() sends this
	// field. Assigning it afterwards would repair the member only after the
	// wrong value had already gone to the server.
	_sound = data.vsound();
	// NovaGram: the local value is not a copy of the server one while the
	// promise is on, because the false the server holds was put there to keep
	// that promise - by this client or by the Android one. Storing it back
	// would blank the name of whoever reacted on this screen too, which is a
	// different setting and one the user did not ask for.
	_showPreviews = NovaGram::LocalReactionsPreviews(
		_session,
		mtpIsTrue(data.vshow_previews()));
}

void ReactionsNotifySettings::save() {
	using Flag = MTPDreactionsNotifySettings::Flag;
	const auto messages = _messagesFrom.current();
	const auto stories = _storiesFrom.current();
	const auto pollVotes = _pollVotesFrom.current();
	const auto previews = _showPreviews.current();
	const auto flags = Flag()
		| ((messages != ReactionsNotifyFrom::None)
			? Flag::f_messages_notify_from
			: Flag())
		| ((stories != ReactionsNotifyFrom::None)
			? Flag::f_stories_notify_from
			: Flag())
		| ((pollVotes != ReactionsNotifyFrom::None)
			? Flag::f_poll_votes_notify_from
			: Flag());
	_api.request(base::take(_requestId)).cancel();
	_requestId = _api.request(MTPaccount_SetReactionsNotifySettings(
		MTP_reactionsNotifySettings(
			MTP_flags(flags),
			((messages != ReactionsNotifyFrom::None)
				? SerializeFrom(messages)
				: MTPReactionNotificationsFrom()),
			((stories != ReactionsNotifyFrom::None)
				? SerializeFrom(stories)
				: MTPReactionNotificationsFrom()),
			((pollVotes != ReactionsNotifyFrom::None)
				? SerializeFrom(pollVotes)
				: MTPReactionNotificationsFrom()),
			// NovaGram: the sound the server last reported, not the default.
			_sound,
			// NovaGram: see the comment in apply(). The account is told to
			// keep the text out of push whatever the local answer is.
			MTP_bool(NovaGram::ReactionsPreviewsToSend(_session, previews)))
	)).done([=](const MTPReactionsNotifySettings &result) {
		_requestId = 0;
		apply(result);
	}).fail([=] {
		_requestId = 0;
	}).send();
}

} // namespace Api
