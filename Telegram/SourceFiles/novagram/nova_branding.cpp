/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_branding.h"

#include "core/version.h"

namespace NovaGram {

QString AppName() {
	return QString::fromUtf8(::AppName.utf8());
}

QString WithAppName(QString text) {
	return text.replace(u"Telegram"_q, AppName());
}

} // namespace NovaGram
