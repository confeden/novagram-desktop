/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"

#include <QtCore/QString>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QSslConfiguration>
#include <vector>

namespace NovaGram::Doh {

// One place a name may be asked. The four built-in ones carry the addresses
// they answer on, so they can be reached without asking anybody first - that
// is the whole point of a fixed list. An endpoint the owner added carries a
// name and, optionally, addresses of its own.
struct Endpoint {
	QString host;
	QString path;
	std::vector<QString> addresses;
	bool builtin = false;
	bool enabled = true;

	[[nodiscard]] bool operator==(const Endpoint &other) const = default;
};

// The four compiled-in endpoints, in the order they are tried. Untouched by
// settings: this is what the list falls back to and what an added endpoint is
// resolved through.
[[nodiscard]] const std::vector<Endpoint> &Builtin();

// The list as configured: the built-in four with their on/off state, followed
// by whatever the owner added.
[[nodiscard]] std::vector<Endpoint> Endpoints();
void SetEndpoints(const std::vector<Endpoint> &list);

// Whether a list may be saved. An empty list, or one whose only usable members
// are named endpoints with nowhere to resolve them, would leave the client
// without a resolver at all - and it must say so rather than quietly fall back
// to the system, which is the one thing this module exists to prevent.
[[nodiscard]] QString ValidateEndpoints(const std::vector<Endpoint> &list);

// The proxy a request to `url` has to go through, decided the same way for the
// resolver and for everything in the fork that leaves the Telegram network.
//
// It exists here rather than at each caller because getting it wrong is silent:
// with no Telegram proxy selected, Sandbox::refreshGlobalProxy() calls
// setApplicationProxy(NoProxy), and that overrides the system configuration for
// every QNetworkAccessManager in the process. On a machine whose way out is a
// PAC file, a request that trusts the application-wide setting goes out
// directly, into nothing, and waits there for ever.
[[nodiscard]] QNetworkProxy ProxyFor(const QUrl &url);

// The TLS configuration that allows the roots of nova_doh_roots.h and nothing
// else - see the definition for what handing Qt an explicit list actually
// does.
//
// Shared with the update checker rather than kept private here, and not by
// accident: the update check ends with an executable being run, so it is the
// one exchange in the fork where a certificate authority someone added to this
// machine would be worth the most. The list is not a list of hosts, it is a
// list of authorities, and it happens to cover the release host as well.
[[nodiscard]] const QSslConfiguration &PinnedConfiguration();

enum class RecordType {
	A,
	AAAA,
	TXT,
};

// What came back. `ttl` is how long the answer stays good for, already clamped
// to the bounds the module keeps; `httpUnixtime` is the Date header of the
// reply that carried it and is zero unless this answer came off the wire.
//
// The clock is a by-product of asking, not a path of its own: an endpoint that
// answers a query has just proved, over a checked certificate, what time it
// thinks it is. That is the same four hosts the promise names, so there is no
// reason left for the client to ask anybody else what time it is.
struct Answer {
	std::vector<QString> values;
	crl::time ttl = 0;
	TimeId httpUnixtime = 0;
};

enum class Cache {
	Use,

	// The caller wants the reply itself, not only what it says. A cached
	// answer carries no Date header, so anything reading the clock out of one
	// has to ask for a live request or it would wait for a header that is
	// never going to arrive.
	Skip,
};

// Resolves a name over the configured endpoints and nowhere else.
//
// An empty answer is an answer: there is no fall-back to getaddrinfo, to the
// hosts file or to whatever the network handed out over DHCP. A caller that
// cannot work without an address has to fail visibly, because a name that
// silently resolved through the system is exactly the leak this replaces.
void Resolve(
	const QString &host,
	RecordType type,
	Fn<void(Answer)> done,
	Cache cache = Cache::Use);

// Cached answers only, no network. Returns empty if nothing is known, and
// never carries a Date header - see Cache::Skip.
[[nodiscard]] Answer Cached(const QString &host, RecordType type);

void ClearCache();

} // namespace NovaGram::Doh
