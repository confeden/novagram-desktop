/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_night_silent.h"

#include "api/api_common.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_peer.h"
#include "data/data_channel.h"
#include "novagram/nova_pin.h"

#include <QtCore/QTime>

namespace NovaGram {
namespace {

constexpr auto kEnabledKey = "novagram_night_silent"_cs;
constexpr auto kUsersKey = "novagram_night_silent_users"_cs;
constexpr auto kGroupsKey = "novagram_night_silent_groups"_cs;
constexpr auto kChannelsKey = "novagram_night_silent_channels"_cs;

[[nodiscard]] bool ReadFlag(std::string_view key, bool fallback) {
	return Core::App().settings().readPref<bool>(key, fallback);
}

void WriteFlag(std::string_view key, bool value) {
	Core::App().settings().writePref<bool>(key, value);
	Core::App().saveSettingsDelayed();
}

} // namespace

// On by default. A pref that was never touched has no record at all, so this
// turns the mode on for everyone who never opened the section, while an
// explicit "off" is a stored false and stays off.
bool NightSilentEnabled() {
	return ReadFlag(kEnabledKey, true);
}

void SetNightSilentEnabled(bool enabled) {
	WriteFlag(kEnabledKey, enabled);
}

bool NightSilentForUsers() {
	return ReadFlag(kUsersKey, true);
}

void SetNightSilentForUsers(bool enabled) {
	WriteFlag(kUsersKey, enabled);
}

bool NightSilentForGroups() {
	return ReadFlag(kGroupsKey, true);
}

void SetNightSilentForGroups(bool enabled) {
	WriteFlag(kGroupsKey, enabled);
}

bool NightSilentForChannels() {
	return ReadFlag(kChannelsKey, true);
}

void SetNightSilentForChannels(bool enabled) {
	WriteFlag(kChannelsKey, enabled);
}

bool NightSilentHourNow() {
	const auto hour = QTime::currentTime().hour();
	return (hour >= kNightSilentFromHour) || (hour < kNightSilentTillHour);
}

// A scheduled message is judged by the hour it is due, not by the hour it was
// written: the flag travels to the server with the schedule and is never
// recomputed, so the current hour would put a silent stamp on a message due at
// nine in the morning and a loud one on a message due at three at night.
// kScheduledUntilOnlineTimestamp means "when the person comes online", and
// nobody knows when that is, so there the moment of sending decides.
[[nodiscard]] bool NightSilentHourFor(TimeId scheduled) {
	if (!scheduled
		|| scheduled == Api::kScheduledUntilOnlineTimestamp) {
		return NightSilentHourNow();
	}
	const auto hour = base::unixtime::parse(scheduled).time().hour();
	return (hour >= kNightSilentFromHour) || (hour < kNightSilentTillHour);
}

bool NightSilentActive(not_null<PeerData*> peer, TimeId scheduled) {
	if (!NightSilentEnabled() || !NightSilentHourFor(scheduled)) {
		return false;
	} else if (peer->isBroadcast()) {
		return NightSilentForChannels();
	} else if (peer->isChat() || peer->isMegagroup()) {
		return NightSilentForGroups();
	} else if (peer->isUser()) {
		return NightSilentForUsers();
	}
	return false;
}

QString NightSilentTitle() {
	return UseRussianTexts()
		? u"Беззвучные сообщения ночью"_q
		: u"Silent messages at night"_q;
}

} // namespace NovaGram
