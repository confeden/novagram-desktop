/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_doh.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "mtproto/facade.h"
#include "mtproto/mtproto_proxy_data.h"
#include "base/unixtime.h"
#include "logs.h"
#include "novagram/nova_pin.h" // NovaGram::UseRussianTexts.
#include "novagram/nova_doh_roots.h"

#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkInterface>
#include <QtNetwork/QNetworkProxyFactory>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslConfiguration>

namespace NovaGram::Doh {
namespace {

constexpr auto kListKey = "novagram_doh_endpoints"_cs;
constexpr auto kBlobVersion = qint32(1);

// A name that does not answer in this long is treated as not answering at all
// and the next endpoint is tried. Deliberately short: four endpoints in a row
// have to fit inside the patience of whoever is waiting for a connection.
constexpr auto kTimeout = 5 * 1000;

// The cache is not an optimisation, it is what keeps a working client from
// asking four strangers about the same name over and over. Bounded because a
// hostile answer could otherwise name a million records.
constexpr auto kMaxCacheEntries = 256;
constexpr auto kMinTtl = crl::time(30 * 1000);
constexpr auto kMaxTtl = crl::time(6 * 60 * 60 * 1000);

// How often the same name may actually be asked over the wire, however loudly
// a caller insists on a live reply.
//
// Found by running it: a client that cannot connect asks for the time on every
// connection attempt, and Cache::Skip turned that into thirty-nine queries to
// Cloudflare in one minute for one name. The caller was right to want a fresh
// reply and wrong about how often - so the floor lives here, where it cannot be
// forgotten by the next caller that needs one.
constexpr auto kMinLiveInterval = crl::time(60 * 1000);

// Not the A-records of the endpoint names. cloudflare-dns.com resolves to
// Cloudflare's CDN range, which the provider promises only as a range, while
// 1.1.1.1 is the address of the service itself and is covered by the
// certificate. Every address below was checked against the provider's own
// documentation and by a live request.
[[nodiscard]] std::vector<Endpoint> MakeBuiltin() {
	const auto make = [](
			const char *host,
			std::vector<QString> addresses) {
		return Endpoint{
			.host = QString::fromLatin1(host),
			.path = u"/dns-query"_q,
			.addresses = std::move(addresses),
			.builtin = true,
			.enabled = true,
		};
	};
	return {
		make("cloudflare-dns.com", {
			u"1.1.1.1"_q,
			u"1.0.0.1"_q,
			u"2606:4700:4700::1111"_q,
			u"2606:4700:4700::1001"_q }),
		make("dns.google", {
			u"8.8.8.8"_q,
			u"8.8.4.4"_q,
			u"2001:4860:4860::8888"_q,
			u"2001:4860:4860::8844"_q }),
		make("dns.adguard-dns.com", {
			u"94.140.14.14"_q,
			u"94.140.15.15"_q,
			u"2a10:50c0::ad1:ff"_q,
			u"2a10:50c0::ad2:ff"_q }),
		make("dns.quad9.net", {
			u"9.9.9.9"_q,
			u"149.112.112.112"_q,
			u"2620:fe::fe"_q,
			u"2620:fe::9"_q }),
		// The fifth is not one of the big four, and that is the point: the
		// other four are the resolvers everyone uses, so a network that wants
		// to know who is asking already knows where to look. Its certificate
		// is Let's Encrypt, whose roots are pinned here already (M10), and all
		// four addresses were checked by a live TLS handshake.
		//
		// It answers over HTTP/2 only - an HTTP/1.1 request gets 505 - which
		// this half handles, because Qt is told the request may use h2. The
		// Android half writes HTTP/1.1 by hand over a TLS socket, so for this
		// one endpoint it speaks DNS-over-TLS on 853 instead: the same socket,
		// the same certificate check, a two-byte length and the same DNS
		// message, and no HTTP at all. Less code there than the HTTP path, not
		// more - see NovaDoh.askOverTls.
		make("dns.dns-ai.ru", {
			u"192.144.59.14"_q,
			u"186.246.49.127"_q,
			u"2a0a:2b41:0:500d::53"_q,
			u"2a0d:8480:0:67c::14"_q }),
	};
}

[[nodiscard]] bool LooksLikeAddress(const QString &value) {
	// Same rules as the call stack uses, and for the same reason: what must be
	// impossible is a name reaching a resolver by accident.
	if (value.isEmpty()) {
		return false;
	} else if (value.contains(':')) {
		auto colons = 0;
		for (const auto ch : value) {
			if (ch == ':') {
				++colons;
			} else if (ch != '.'
				&& !(ch >= '0' && ch <= '9')
				&& !(ch >= 'a' && ch <= 'f')
				&& !(ch >= 'A' && ch <= 'F')) {
				return false;
			}
		}
		return (colons >= 2) && (colons <= 7);
	}
	auto groups = 0;
	auto digits = 0;
	auto number = 0;
	for (const auto ch : value) {
		if (ch == '.') {
			if (!digits || ++groups > 3) {
				return false;
			}
			digits = 0;
			number = 0;
		} else if (ch >= '0' && ch <= '9') {
			if (++digits > 3) {
				return false;
			}
			number = number * 10 + (ch.unicode() - '0');
			if (number > 255) {
				return false;
			}
		} else {
			return false;
		}
	}
	return (groups == 3) && (digits > 0);
}

// Whether this machine has a route to the IPv6 internet at all. Without the
// check every IPv6 address in the list costs a full timeout before the next
// address is tried - on a machine with no IPv6 that is the whole list spent on
// nothing, which is exactly what the first run showed.
[[nodiscard]] bool HasIpv6() {
	static const auto result = [] {
		for (const auto &address : QNetworkInterface::allAddresses()) {
			if (address.protocol() == QAbstractSocket::IPv6Protocol
				&& !address.isLoopback()
				&& !address.isLinkLocal()
				&& !address.isUniqueLocalUnicast()) {
				return true;
			}
		}
		return false;
	}();
	return result;
}

[[nodiscard]] QString UrlFor(const Endpoint &endpoint, const QString &address) {
	const auto host = address.contains(':')
		? ('[' + address + ']')
		: address;
	return u"https://"_q + host + endpoint.path;
}

struct CacheKey {
	QString host;
	RecordType type = RecordType::A;

	[[nodiscard]] bool operator==(const CacheKey &) const = default;
	[[nodiscard]] bool operator<(const CacheKey &other) const {
		return (host != other.host)
			? (host < other.host)
			: (int(type) < int(other.type));
	}
};

struct CacheEntry {
	std::vector<QString> values;
	crl::time until = 0;
};

// The Date header of an endpoint reply, in seconds since the epoch, or zero if
// there is nothing believable there.
//
// The sanity window matters more than the parse: this value goes on to shift
// the clock of the whole client, and a header that is decades off - from a
// broken cache, a captive portal or a deliberately lying middlebox - would
// take message ordering and key expiry with it. Anything outside the window is
// treated as no answer at all rather than as a correction.
[[nodiscard]] TimeId ParseHttpDate(const QByteArray &value) {
	if (value.isEmpty()) {
		return 0;
	}
	const auto parsed = QDateTime::fromString(
		QString::fromLatin1(value),
		Qt::RFC2822Date);
	if (!parsed.isValid()) {
		return 0;
	}
	const auto seconds = parsed.toSecsSinceEpoch();
	constexpr auto kNotBefore = TimeId(1735689600); // 2025-01-01.
	constexpr auto kNotAfter = TimeId(2524608000); // 2050-01-01.
	return (seconds >= kNotBefore && seconds <= kNotAfter)
		? TimeId(seconds)
		: TimeId(0);
}

// --- RFC 8484 wire format -------------------------------------------------
//
// A minimal codec on purpose. The JSON APIs are not an option: only two of the
// four endpoints have one, and Google's lives on a different path - the single
// thing all four accept is a binary POST of a DNS message.

[[nodiscard]] quint16 TypeCode(RecordType type) {
	switch (type) {
	case RecordType::A: return 1;
	case RecordType::AAAA: return 28;
	case RecordType::TXT: return 16;
	}
	Unexpected("RecordType in NovaGram::Doh.");
}

[[nodiscard]] QString TypeName(RecordType type) {
	switch (type) {
	case RecordType::A: return u"A"_q;
	case RecordType::AAAA: return u"AAAA"_q;
	case RecordType::TXT: return u"TXT"_q;
	}
	Unexpected("RecordType in NovaGram::Doh.");
}

[[nodiscard]] QByteArray BuildQuery(const QString &host, RecordType type) {
	auto result = QByteArray();
	const auto push16 = [&](quint16 value) {
		result.append(char(value >> 8));
		result.append(char(value & 0xFF));
	};
	push16(0); // Id. Zero on purpose: over HTTPS it buys nothing and a
	           // constant one keeps identical queries byte-identical, which
	           // is what lets a proxy cache them.
	push16(0x0100); // Recursion desired.
	push16(1); // One question.
	push16(0);
	push16(0);
	push16(0);
	for (const auto &label : host.split('.', Qt::SkipEmptyParts)) {
		const auto bytes = label.toLatin1();
		if (bytes.isEmpty() || bytes.size() > 63) {
			return QByteArray();
		}
		result.append(char(bytes.size()));
		result.append(bytes);
	}
	result.append(char(0));
	push16(TypeCode(type));
	push16(1); // IN.
	return result;
}

// Walks a name at `offset`, following compression pointers, and leaves
// `offset` after the name as it appears here. Returns false on anything
// malformed - a hostile answer must end the parse, not steer it.
[[nodiscard]] bool SkipName(const QByteArray &data, int &offset) {
	// A pointer sends the reader backwards, so the position to continue from
	// is the one right after the FIRST pointer, not wherever the chain ends.
	// The guard is what keeps a name that points at itself from hanging the
	// parse - the data here comes from a stranger.
	auto guard = 0;
	auto position = offset;
	auto after = -1;
	while (position >= 0 && position < data.size()) {
		const auto length = uchar(data[position]);
		if (!length) {
			offset = (after >= 0) ? after : (position + 1);
			return true;
		} else if ((length & 0xC0) == 0xC0) {
			if (position + 1 >= data.size() || ++guard > 32) {
				return false;
			} else if (after < 0) {
				after = position + 2;
			}
			position = ((length & 0x3F) << 8) | uchar(data[position + 1]);
		} else if (length > 63) {
			return false;
		} else {
			position += 1 + length;
		}
	}
	return false;
}

[[nodiscard]] QString AddressFromRecord(
		const QByteArray &data,
		int offset,
		int length,
		quint16 type) {
	if (type == 1 && length == 4) {
		return u"%1.%2.%3.%4"_q
			.arg(uchar(data[offset]))
			.arg(uchar(data[offset + 1]))
			.arg(uchar(data[offset + 2]))
			.arg(uchar(data[offset + 3]));
	} else if (type == 28 && length == 16) {
		auto parts = QStringList();
		for (auto i = 0; i != 8; ++i) {
			const auto value = (uchar(data[offset + i * 2]) << 8)
				| uchar(data[offset + i * 2 + 1]);
			parts.push_back(QString::number(value, 16));
		}
		return parts.join(':');
	}
	return QString();
}

[[nodiscard]] Answer ParseResponse(
		const QByteArray &data,
		RecordType type) {
	auto result = Answer();
	if (data.size() < 12) {
		return result;
	}
	const auto read16 = [&](int at) {
		return quint16((uchar(data[at]) << 8) | uchar(data[at + 1]));
	};
	const auto flags = read16(2);
	if ((flags & 0x000F) != 0) {
		// Anything but NOERROR: an answer that says "no" is still an answer,
		// and the caller must not read a failure as "try the system instead".
		return result;
	}
	const auto questions = read16(4);
	const auto answers = read16(6);
	auto offset = 12;
	for (auto i = 0; i != questions; ++i) {
		if (!SkipName(data, offset) || offset + 4 > data.size()) {
			return result;
		}
		offset += 4;
	}
	const auto wanted = TypeCode(type);
	auto ttl = crl::time(0);
	for (auto i = 0; i != answers; ++i) {
		if (!SkipName(data, offset) || offset + 10 > data.size()) {
			break;
		}
		const auto recordType = read16(offset);
		const auto recordTtl = (uint(uchar(data[offset + 4])) << 24)
			| (uint(uchar(data[offset + 5])) << 16)
			| (uint(uchar(data[offset + 6])) << 8)
			| uint(uchar(data[offset + 7]));
		const auto length = read16(offset + 8);
		offset += 10;
		if (offset + length > data.size()) {
			break;
		}
		if (recordType == wanted) {
			if (type == RecordType::TXT) {
				auto text = QByteArray();
				auto at = offset;
				while (at < offset + length) {
					const auto piece = uchar(data[at]);
					if (at + 1 + piece > offset + length) {
						break;
					}
					text.append(data.mid(at + 1, piece));
					at += 1 + piece;
				}
				if (!text.isEmpty()) {
					result.values.push_back(QString::fromLatin1(text));
				}
			} else {
				auto address = AddressFromRecord(
					data,
					offset,
					length,
					recordType);
				if (!address.isEmpty()) {
					result.values.push_back(std::move(address));
				}
			}
			if (!ttl || crl::time(recordTtl) * 1000 < ttl) {
				ttl = crl::time(recordTtl) * 1000;
			}
		}
		offset += length;
	}
	result.ttl = std::clamp(ttl ? ttl : kMinTtl, kMinTtl, kMaxTtl);
	return result;
}

// --- the resolver ---------------------------------------------------------

class Resolver final {
public:
	[[nodiscard]] static Resolver &Instance();

	void resolve(
		const QString &host,
		RecordType type,
		Fn<void(Answer)> done,
		Cache cache);
	[[nodiscard]] Answer cached(
		const QString &host,
		RecordType type) const;
	void clear();

private:
	// One address of one endpoint, with the name that address answers under.
	//
	// The name has to travel with the address rather than sit beside the list:
	// it drives both SNI and the certificate check, so a single field shared
	// by every target means every attempt but one is made against the wrong
	// name and dies in the handshake. That is what the first run did - three
	// endpoints failing invisibly and the fourth, whose name happened to be
	// the one left in the field, carrying the whole feature.
	struct Target {
		QString url;
		QString verifyName;

		// Whether this is one of the four compiled-in endpoints, and so
		// whether its roots are pinned. A server the owner added is their
		// choice and may legitimately use a private authority; pinning ours to
		// it would only make the feature unusable.
		bool pinned = false;
	};
	struct Request {
		QString host;
		RecordType type = RecordType::A;
		std::vector<Fn<void(Answer)>> waiting;
		std::vector<Target> targets;
		int next = 0;
		TimeId httpUnixtime = 0;
	};

	void step(std::shared_ptr<Request> request);
	void finish(const std::shared_ptr<Request> &request, Answer answer);
	[[nodiscard]] std::vector<std::pair<Endpoint, QString>> plan() const;

	QNetworkAccessManager _manager;
	base::flat_map<CacheKey, CacheEntry> _cache;

	// When each name was last actually asked over the wire, as opposed to
	// answered from the cache. See kMinLiveInterval.
	base::flat_map<CacheKey, crl::time> _live;

};

Resolver &Resolver::Instance() {
	static auto result = Resolver();
	return result;
}

Answer Resolver::cached(const QString &host, RecordType type) const {
	const auto i = _cache.find(CacheKey{ host.toLower(), type });
	const auto now = crl::now();
	if (i == end(_cache) || i->second.until < now) {
		return {};
	}
	return Answer{
		.values = i->second.values,
		.ttl = (i->second.until - now),
	};
}

void Resolver::clear() {
	_cache.clear();
	_live.clear();
}

// Every endpoint paired with every address it answers on, in the configured
// order. An endpoint the owner added by name only has no addresses of its own
// and is therefore resolved through the built-in ones first - that is the
// decision of 2026-08-18, and it keeps the invariant intact: nothing here ever
// reaches the system resolver.
std::vector<std::pair<Endpoint, QString>> Resolver::plan() const {
	// Every IPv4 address of every endpoint first, then IPv6. Interleaving them
	// per endpoint - which is how they are written down - means a machine
	// without IPv6 spends two timeouts inside each endpoint before reaching
	// the next one, and the list runs out before anything is asked.
	auto result = std::vector<std::pair<Endpoint, QString>>();
	const auto collect = [&](bool ipv6) {
		for (const auto &endpoint : Endpoints()) {
			if (!endpoint.enabled) {
				continue;
			}
			for (const auto &address : endpoint.addresses) {
				if (LooksLikeAddress(address)
					&& (address.contains(':') == ipv6)) {
					result.push_back({ endpoint, address });
				}
			}
		}
	};
	collect(false);
	if (HasIpv6()) {
		collect(true);
	}
	return result;
}

void Resolver::resolve(
		const QString &host,
		RecordType type,
		Fn<void(Answer)> done,
		Cache cache) {
	const auto lowered = host.toLower();
	if (LooksLikeAddress(lowered)) {
		// Already an address. Asking about it would be a question with a known
		// answer and a needless request to a stranger. True whatever the cache
		// policy says: it is a property of what was asked, not of what is
		// remembered.
		done(Answer{ .values = { lowered }, .ttl = kMaxTtl });
		return;
	}
	const auto key = CacheKey{ lowered, type };
	const auto now = crl::now();
	const auto asked = _live.find(key);
	const auto tooSoon = (cache == Cache::Skip)
		&& (asked != end(_live))
		&& (now - asked->second < kMinLiveInterval);
	if (cache == Cache::Use || tooSoon) {
		if (auto ready = cached(lowered, type); !ready.values.empty()) {
			DEBUG_LOG(("NovaGram DoH: cache hit %1 ttl %2%3.").arg(
				lowered,
				QString::number(ready.ttl / 1000),
				(tooSoon ? u", asked live too recently"_q : QString())));
			done(std::move(ready));
			return;
		}
	}
	_live[key] = now;
	auto request = std::make_shared<Request>();
	request->host = lowered;
	request->type = type;
	request->waiting.push_back(std::move(done));
	for (const auto &[endpoint, address] : plan()) {
		if (endpoint.host.compare(lowered, Qt::CaseInsensitive) == 0) {
			// Never ask an endpoint to resolve itself.
			continue;
		}
		request->targets.push_back({
			.url = UrlFor(endpoint, address),
			.verifyName = endpoint.host,
			.pinned = endpoint.builtin,
		});
	}
	if (request->targets.empty()) {
		LOG(("NovaGram DoH: no usable endpoint for %1.").arg(lowered));
		finish(request, {});
		return;
	}
	step(std::move(request));
}

void Resolver::step(std::shared_ptr<Request> request) {
	if (request->next >= int(request->targets.size())) {
		LOG(("NovaGram DoH: all endpoints failed for %1.").arg(request->host));
		finish(request, {});
		return;
	}
	const auto target = request->targets[request->next++];
	const auto url = target.url;
	const auto verifyName = target.verifyName;
	const auto query = BuildQuery(request->host, request->type);
	if (query.isEmpty()) {
		finish(request, {});
		return;
	}
	// The name is carried by peerVerifyName, which in Qt 6 drives both SNI and
	// the certificate check, and by an explicit Host header. Redirects have to
	// be manual: following one by name would be a request through the system
	// resolver, which is the whole thing this avoids.
	auto request2 = QNetworkRequest(QUrl(url));
	if (target.pinned) {
		request2.setSslConfiguration(PinnedConfiguration());
	}
	request2.setPeerVerifyName(verifyName);
	request2.setRawHeader("Host", verifyName.toLatin1());
	request2.setRawHeader("Accept", "application/dns-message");
	request2.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/dns-message"_q);
	// HTTP/2 stays ON, and the Host header above is a courtesy rather than the
	// mechanism. RFC 8484 requires HTTP/2, and Quad9 enforces it: with h2 off
	// it answers 505 "HTTP Version Not Supported" - which is what the first
	// run showed. In h2 the header is dropped and :authority becomes the
	// literal address, and that is fine: all four endpoints document or answer
	// requests addressed by IP, and what actually protects the connection is
	// the certificate checked against peerVerifyName, not the authority.
	//
	// Asked for rather than left to the default: the default differs between
	// the Qt versions this fork has been built against, and one endpoint out
	// of four refusing the request is not something to leave to a toolkit
	// setting that changes underneath.
	request2.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
	request2.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::ManualRedirectPolicy);
	request2.setTransferTimeout(kTimeout);

	// The endpoint is as unreachable as anything else in a network that needs
	// a proxy, so it goes out the same way the rest of the fork does.
	_manager.setProxy(ProxyFor(QUrl(url)));

	DEBUG_LOG(("NovaGram DoH: query %1/%2 via %3 [%4].").arg(
		request->host,
		TypeName(request->type),
		verifyName,
		url));
	const auto reply = _manager.post(request2, query);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto code = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (reply->error() != QNetworkReply::NoError || code != 200) {
			LOG(("NovaGram DoH: endpoint %1 failed: %2 (%3).").arg(
				url,
				reply->errorString(),
				QString::number(code)));
			step(request);
			return;
		}
		// Read before the body is looked at, and applied even if the body
		// turns out to be useless: an endpoint that answered at all has told
		// us the time over a checked certificate, and that is the only place
		// the fork gets it from now. It is also kept on the request, so that a
		// caller who asked for a live reply still learns the time when every
		// endpoint answers "no such name" - it wanted the reply, not the
		// records.
		if (const auto stamp = ParseHttpDate(reply->rawHeader("Date"))) {
			base::unixtime::http_update(stamp);
			request->httpUnixtime = stamp;
		}
		auto parsed = ParseResponse(reply->readAll(), request->type);
		if (parsed.values.empty()) {
			step(request);
			return;
		}
		LOG(("NovaGram DoH: answer %1 -> %2 ttl %3.").arg(
			request->host,
			parsed.values.front(),
			QString::number(parsed.ttl / 1000)));
		finish(request, std::move(parsed));
	});
}

void Resolver::finish(
		const std::shared_ptr<Request> &request,
		Answer answer) {
	answer.httpUnixtime = request->httpUnixtime;
	if (_live.size() >= kMaxCacheEntries) {
		_live.clear();
	}
	if (!answer.values.empty() && answer.ttl > 0) {
		if (_cache.size() >= kMaxCacheEntries) {
			_cache.clear();
		}
		_cache[CacheKey{ request->host, request->type }] = CacheEntry{
			.values = answer.values,
			.until = crl::now() + answer.ttl,
		};
	}
	for (const auto &callback : request->waiting) {
		callback(answer);
	}
}

[[nodiscard]] QByteArray Serialize(const std::vector<Endpoint> &list) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_15);
	stream << kBlobVersion << qint32(list.size());
	for (const auto &endpoint : list) {
		stream << endpoint.host
			<< endpoint.path
			<< qint32(endpoint.builtin ? 1 : 0)
			<< qint32(endpoint.enabled ? 1 : 0)
			<< qint32(endpoint.addresses.size());
		for (const auto &address : endpoint.addresses) {
			stream << address;
		}
	}
	return result;
}

[[nodiscard]] std::vector<Endpoint> Deserialize(const QByteArray &blob) {
	if (blob.isEmpty()) {
		return {};
	}
	auto stream = QDataStream(blob);
	stream.setVersion(QDataStream::Qt_5_15);
	auto version = qint32(0);
	auto count = qint32(0);
	stream >> version >> count;
	if (stream.status() != QDataStream::Ok
		|| version != kBlobVersion
		|| count < 0
		|| count > 64) {
		return {};
	}
	auto result = std::vector<Endpoint>();
	for (auto i = 0; i != count; ++i) {
		auto endpoint = Endpoint();
		auto builtin = qint32(0);
		auto enabled = qint32(0);
		auto addresses = qint32(0);
		stream >> endpoint.host
			>> endpoint.path
			>> builtin
			>> enabled
			>> addresses;
		if (stream.status() != QDataStream::Ok
			|| addresses < 0
			|| addresses > 16) {
			return {};
		}
		endpoint.builtin = (builtin != 0);
		endpoint.enabled = (enabled != 0);
		for (auto j = 0; j != addresses; ++j) {
			auto address = QString();
			stream >> address;
			if (stream.status() != QDataStream::Ok) {
				return {};
			}
			endpoint.addresses.push_back(address);
		}
		result.push_back(std::move(endpoint));
	}
	return result;
}

} // namespace

const std::vector<Endpoint> &Builtin() {
	static const auto result = MakeBuiltin();
	return result;
}

std::vector<Endpoint> Endpoints() {
	const auto stored = Deserialize(
		Core::App().settings().readPref<QByteArray>(kListKey, QByteArray()));
	if (stored.empty()) {
		return Builtin();
	}
	// The built-in four are always present in the list, whatever a stored blob
	// says: a release that adds or renames one must not be overruled by a file
	// written by an older build. What the blob decides about them is only
	// whether they are on.
	auto result = Builtin();
	for (auto &endpoint : result) {
		for (const auto &saved : stored) {
			if (saved.builtin && saved.host == endpoint.host) {
				endpoint.enabled = saved.enabled;
			}
		}
	}
	for (const auto &saved : stored) {
		if (!saved.builtin && !saved.host.isEmpty()) {
			result.push_back(saved);
		}
	}
	return result;
}

void SetEndpoints(const std::vector<Endpoint> &list) {
	Core::App().settings().writePref<QByteArray>(kListKey, Serialize(list));
	Core::App().saveSettingsDelayed();
	Resolver::Instance().clear();
}

QString ValidateEndpoints(const std::vector<Endpoint> &list) {
	const auto russian = UseRussianTexts();
	auto enabledBuiltin = 0;
	auto enabledAny = 0;
	auto namedWithoutAddress = 0;
	for (const auto &endpoint : list) {
		if (!endpoint.enabled) {
			continue;
		}
		++enabledAny;
		if (endpoint.builtin) {
			++enabledBuiltin;
			continue;
		}
		const auto hasAddress = ranges::any_of(
			endpoint.addresses,
			LooksLikeAddress);
		if (!hasAddress) {
			++namedWithoutAddress;
		}
	}
	if (!enabledAny) {
		return russian
			? u"Нужен хотя бы один включённый сервер: иначе имена не будет "
				"кому разрешать, а к системному DNS NovaGram не обращается."_q
			: u"At least one server has to stay on: otherwise there is nobody "
				"to resolve names, and NovaGram never asks the system."_q;
	} else if (namedWithoutAddress > 0 && !enabledBuiltin) {
		return russian
			? u"Ваш сервер задан именем, а разрешить это имя нечем: оставьте "
				"включённым хотя бы один встроенный сервер или укажите "
				"IP-адрес своего."_q
			: u"Your server is given by name and there is nothing to resolve "
				"that name with: keep at least one built-in server on, or "
				"give your server an IP address."_q;
	}
	return QString();
}

QNetworkProxy ProxyFor(const QUrl &url) {
	const auto &settings = Core::App().settings().proxy();
	if (settings.isEnabled()) {
		const auto proxy = settings.selected();
		if (proxy.type == MTP::ProxyData::Type::Socks5
			|| proxy.type == MTP::ProxyData::Type::Http) {
			if (proxy.tryCustomResolve() && proxy.resolvedIPs.empty()) {
				// The proxy is named and its name is not known yet. Handing it
				// to Qt like this would have Qt look the name up - through the
				// system, before any code of this fork runs - and that is the
				// one thing the whole module exists to prevent.
				//
				// So this request goes without it. That is not a fall-back and
				// not a weakening: what it does is break the circle in which
				// the resolver needs the proxy and the proxy needs the
				// resolver. If the way out of this network really is that
				// proxy and only that proxy, this request fails, the name
				// stays unresolved and the client says so - the answer to
				// which is to give the proxy by address, and then nothing
				// needs resolving at all.
				DEBUG_LOG(("NovaGram DoH: proxy %1 is not resolved yet, "
					"going without it.").arg(proxy.host));
			} else {
				return MTP::ToNetworkProxy(MTP::ToDirectIpProxy(proxy));
			}
		}
	}
	// Nothing of Telegram's own to use, so ask the system - and ask it
	// explicitly rather than letting the application-wide setting decide. See
	// the header for why that distinction is the difference between a request
	// that goes out and one that waits for ever.
	const auto system = QNetworkProxyFactory::systemProxyForQuery(
		QNetworkProxyQuery(url));
	return system.isEmpty()
		? QNetworkProxy(QNetworkProxy::NoProxy)
		: system.front();
}

// The TLS configuration used for the four built-in endpoints and for the
// update check: the roots in nova_doh_roots.h and nothing else.
//
// setCaCertificates() is doing two things at once here, and the second is the
// one that matters. Besides replacing the list, it turns off Qt's on-demand
// loading of roots from the Windows store - and it is that loading which would
// otherwise accept a certificate authority someone added to the machine. With
// it off, a Windows-supplied root that is not in our list is refused, so an
// intercepting proxy ends the connection with a verification error instead of
// quietly answering in the resolver's place.
//
// If the compiled-in list somehow fails to parse there is nothing to fall back
// on that is both safe and useful, so it falls back to the default
// configuration and says so in the log. That trade is deliberate: an empty list
// would take the resolver down completely, and with it the proxy by name, the
// emergency config and updates - while the condition can only ever be a defect
// in this file, never anything an attacker arranges.
const QSslConfiguration &PinnedConfiguration() {
	static const auto result = [] {
		auto config = QSslConfiguration::defaultConfiguration();
		const auto parsed = QSslCertificate::fromData(
			QByteArray(kPinnedRoots),
			QSsl::Pem);
		auto roots = QList<QSslCertificate>();
		for (const auto &certificate : parsed) {
			if (!certificate.isNull()) {
				roots.push_back(certificate);
			}
		}
		if (roots.isEmpty()) {
			LOG(("NovaGram DoH Error: pinned roots did not parse, "
				"falling back to the system store."));
			return config;
		}
		LOG(("NovaGram DoH: pinned %1 roots for the built-in endpoints "
			"and for the update check.").arg(roots.size()));
		config.setCaCertificates(roots);
		return config;
	}();
	return result;
}

void Resolve(
		const QString &host,
		RecordType type,
		Fn<void(Answer)> done,
		Cache cache) {
	Resolver::Instance().resolve(host, type, std::move(done), cache);
}

Answer Cached(const QString &host, RecordType type) {
	return Resolver::Instance().cached(host, type);
}

void ClearCache() {
	Resolver::Instance().clear();
}

} // namespace NovaGram::Doh
