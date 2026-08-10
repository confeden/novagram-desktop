/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class PeerData;

namespace NovaGram {

inline constexpr auto kNightSilentFromHour = 22;
inline constexpr auto kNightSilentTillHour = 7;

[[nodiscard]] bool NightSilentEnabled();
void SetNightSilentEnabled(bool enabled);

[[nodiscard]] bool NightSilentForUsers();
void SetNightSilentForUsers(bool enabled);

[[nodiscard]] bool NightSilentForGroups();
void SetNightSilentForGroups(bool enabled);

[[nodiscard]] bool NightSilentForChannels();
void SetNightSilentForChannels(bool enabled);

[[nodiscard]] bool NightSilentHourNow();

// A scheduled message is judged by the hour it is due: the flag reaches the
// server together with the schedule and is never recomputed afterwards.
[[nodiscard]] bool NightSilentActive(
	not_null<PeerData*> peer,
	TimeId scheduled = 0);

[[nodiscard]] QString NightSilentTitle();

} // namespace NovaGram
