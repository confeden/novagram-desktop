/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

namespace NovaGram {

[[nodiscard]] bool AutoProxyEnabled();
void SetAutoProxyEnabled(bool enabled);

// Starts background monitoring and proxy updater.
void StartAutoProxy();

// Forces an immediate reload of the cloud proxy list.
void RefreshCloudProxies();

// Returns status text (e.g. "Прямое соединение" or "Через прокси: ... (XX ms)")
[[nodiscard]] QString AutoProxyStatusText();

} // namespace NovaGram
