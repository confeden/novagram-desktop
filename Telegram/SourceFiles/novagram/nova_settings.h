/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "settings/settings_type.h"

namespace NovaGram {

[[nodiscard]] Settings::Type SettingsSectionId();
[[nodiscard]] QString SettingsSectionTitle();

// Keeps the name, the avatar, the text and the reply and mark as read
// buttons out of every notification, whatever the stock notification
// preview setting says. Read by Notifications::Manager, in the single
// place where both the built-in windows and the system toasts ask what
// they are allowed to show.
[[nodiscard]] bool HideNotificationContentEnabled();
void SetHideNotificationContentEnabled(bool enabled);

[[nodiscard]] QString HideNotificationContentTitle();

} // namespace NovaGram
