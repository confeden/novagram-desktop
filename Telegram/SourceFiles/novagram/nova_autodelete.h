/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class PeerData;
class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace NovaGram {

inline constexpr auto kAutoDeleteMinHours = 24;
inline constexpr auto kAutoDeleteMaxHours = 168;
inline constexpr auto kAutoDeleteDefaultHours = 168;

enum class PeerRule : uchar {
	Default,
	Always,
	Never,
};

// The whole state lives in a per-account file encrypted with the account key,
// because the queue and the per-chat rules are a list of peer and message
// identifiers, and that is exactly the metadata the fork promises to protect.
// The settings key-value store would not do: it is readable without the pin.

[[nodiscard]] bool Enabled(not_null<Main::Session*> session);
void SetEnabled(not_null<Main::Session*> session, bool enabled);

[[nodiscard]] int PeriodHours(not_null<Main::Session*> session);
void SetPeriodHours(not_null<Main::Session*> session, int hours);

[[nodiscard]] QString Replacement(not_null<Main::Session*> session);
void SetReplacement(not_null<Main::Session*> session, const QString &text);

// True where the account can moderate, so the default is not to delete.
[[nodiscard]] bool HasModerationRights(not_null<PeerData*> peer);
[[nodiscard]] bool AppliesByDefault(not_null<PeerData*> peer);

[[nodiscard]] PeerRule RuleFor(not_null<PeerData*> peer);
void SetRuleFor(not_null<PeerData*> peer, PeerRule rule);
[[nodiscard]] bool AppliesTo(not_null<PeerData*> peer);

// Seconds left before the message is processed, 0 when it is not tracked.
[[nodiscard]] TimeId DueIn(not_null<HistoryItem*> item);

// "Удалится через: 3 д 23 ч 16 м", empty when the message is not tracked.
[[nodiscard]] QString CountdownText(not_null<HistoryItem*> item);

// Starts the per-session scheduler, safe to call more than once.
void Start(not_null<Main::Session*> session);

// Queues the given messages for immediate destruction, ignoring the enabled
// flag and the per-chat rules: this serves the manual Erase evidence command,
// which is an explicit order rather than a policy.
void EnqueueNow(
	not_null<Main::Session*> session,
	const std::vector<FullMsgId> &ids);

[[nodiscard]] QString SettingsTitle();
[[nodiscard]] QString SettingsLabel(not_null<Main::Session*> session);
[[nodiscard]] QString FormatPeriod(int hours);
[[nodiscard]] QString PeerMenuText(not_null<PeerData*> peer);

} // namespace NovaGram
