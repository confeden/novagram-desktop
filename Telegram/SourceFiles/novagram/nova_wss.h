/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>
#include <vector>

namespace NovaGram::Wss {

// MTProto carried inside a WebSocket over TLS, for networks where the plain
// TCP to Telegram's data centres is dropped but an ordinary HTTPS site is not.
// Telegram runs the relay itself (kwsN.web.telegram.org/apiws, what web.
// telegram.org talks to); the same path is served by third-party fronts
// behind Cloudflare, which is the only way to DC1, DC3 and DC5 on a line that
// blocks the official ingress there.
//
// What a relay or a front can see: your address, which data centre you asked
// for and how much you send. The traffic itself is the same MTProto encrypted
// under the account's own keys as over TCP; none of them holds a key.
//
// On by default. The list of relays is carried inside the client and checked
// once a day against the published NovaGram/relay.json of the update
// repository, downloaded only when its version or its ETag moved.
[[nodiscard]] bool Enabled();
void SetEnabled(bool enabled);

// Reads the list kept on the disk and starts the daily check. After the
// settings are read; does nothing while the decoy is on.
void Start();

// The same answer, readable from the connection thread. Enabled() has to be
// called once on the main thread after the settings are read.
[[nodiscard]] bool EnabledForSocket();

[[nodiscard]] QString Title();
[[nodiscard]] QString About();

struct Route {
	QString host;   // name sent as SNI, checked against the certificate, and Host:
	QString ip;     // empty: resolved through NovaGram DoH, never the system
	QString path;
	bool front = false;
	int index = 0;  // place in the ladder: 0 is the official relay
};

// Whether data centre `dcId` (the protocol id: negative for media, test mode
// shifted by 10000) can be carried this way at all.
[[nodiscard]] bool Supports(int protocolDcId);

// Routes to try for one connection attempt, best first: the one that worked
// last time, then the next. Exhausting them is a failed attempt, and the next
// attempt starts further along.
[[nodiscard]] std::vector<Route> Attempt(int protocolDcId, int count);
void NoteWorked(int protocolDcId, const Route &route);
void NoteFailed(int protocolDcId, const Route &route);

} // namespace NovaGram::Wss
