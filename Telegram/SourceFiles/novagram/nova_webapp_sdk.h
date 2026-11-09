/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace NovaGram::WebAppSdk {

// Telegram's mini-app SDK when telegram.org cannot be reached.
//
// Almost every mini app starts with
// <script src="https://telegram.org/js/telegram-web-app.js"> in its head. On a
// line that drops Telegram's addresses that script never arrives: the page
// stays blank until the connect gives up, and Telegram.WebApp is missing, so
// the app is dead even when its own host is reachable. The WSS ladder carries
// only MTProto.
//
// So every webview hands that one URL here. It is fetched live first (3 s),
// as the webview would; a live copy is kept in tdata. When telegram.org does
// not answer, the kept copy or the one built in (:/novagram/
// telegram-web-app.js, refreshed at release time) is served, and telegram.org
// is not tried again for ten minutes. Android twin: NovaWebAppSdk.java.
void Register();

} // namespace NovaGram::WebAppSdk
