/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <optional>

namespace NovaGram::RelayToken {

// The per-install token the own Cloudflare Worker route offers in the
// Sec-WebSocket-Protocol of the upgrade ("binary, nova2.<payload>.<sig>").
// Telegram never sees it. It is had from nova-app.eu/relay: a one-off
// enrollment guarded by a proof of work, then an HMAC-signed request for the
// short-lived token.
//
// Everything here is best effort and silent: with no token the route is simply
// not on the ladder, and no other route ever waits for it.

// The cached token while it has not expired, else empty. Any thread.
[[nodiscard]] QString Current();

// Starts, if needed and allowed, enrolling or refreshing the token. Cheap and
// safe to call on every connection attempt, from any thread: it is throttled,
// works on the main thread and a worker thread, and backs off by itself.
void Ensure();

// Pieces of the protocol, exposed so that they can be checked against fixed
// vectors.

// Lowercase hex of HMAC-SHA256(key = secret as text, "token|<id>|<ts>").
[[nodiscard]] QString TokenMac(
	const QString &id,
	const QString &secret,
	qint64 timestamp);

[[nodiscard]] int LeadingZeroBits(const QByteArray &digest);

// The smallest n with SHA256("<challenge>|<n>") having `bits` leading zero
// bits; nullopt when `timeoutMs` ran out first. Blocking: call off the main
// thread.
[[nodiscard]] std::optional<qint64> SolveWork(
	const QByteArray &challenge,
	int bits,
	int timeoutMs);

} // namespace NovaGram::RelayToken
