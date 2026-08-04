/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_night_silent.h"

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

bool NightSilentEnabled() {
	return ReadFlag(kEnabledKey, false);
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

bool NightSilentActive(not_null<PeerData*> peer) {
	if (!NightSilentEnabled() || !NightSilentHourNow()) {
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
