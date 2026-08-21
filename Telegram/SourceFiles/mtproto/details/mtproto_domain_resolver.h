/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"

#include <QtCore/QPointer>
#include <QtNetwork/QNetworkReply>
#include <optional>
#include <set>

namespace MTP::details {

[[nodiscard]] const std::vector<QString> &DnsDomains();
[[nodiscard]] QString GenerateDnsRandomPadding();
[[nodiscard]] QByteArray DnsUserAgent();

struct DnsEntry {
	QString data;
	crl::time TTL = 0;
};

[[nodiscard]] std::vector<DnsEntry> ParseDnsResponse(
	const QByteArray &bytes,
	std::optional<int> typeRestriction = std::nullopt);

struct ServiceWebRequest {
	ServiceWebRequest(not_null<QNetworkReply*> reply);
	ServiceWebRequest(ServiceWebRequest &&other);
	ServiceWebRequest &operator=(ServiceWebRequest &&other);
	~ServiceWebRequest();

	void destroy();

	QPointer<QNetworkReply> reply;
};

// NovaGram: the three Google fronts and the Mozilla one are gone, and with
// them the shuffling, the fake user agent and the random padding that made a
// by-name request to a public resolver look like browsing. Names are asked of
// the fork's own list of endpoints and of nothing else, so there is one attempt
// per record type and no order to hide.
//
// The manager went with them: this class no longer owns a socket. What it still
// owns is the cache that decides when a proxy address has to be looked up
// again, which is why it stays a class instead of becoming a free function.
class DomainResolver : public QObject {
public:
	DomainResolver(Fn<void(
		const QString &domain,
		const QStringList &ips,
		crl::time expireAt)> callback);

	void resolve(const QString &domain);

private:
	struct AttemptKey {
		QString domain;
		bool ipv6 = false;

		inline bool operator<(const AttemptKey &other) const {
			return (domain < other.domain)
				|| (domain == other.domain && !ipv6 && other.ipv6);
		}
		inline bool operator==(const AttemptKey &other) const {
			return (domain == other.domain) && (ipv6 == other.ipv6);
		}
	};
	struct CacheEntry {
		QStringList ips;
		crl::time expireAt = 0;
	};

	void resolve(const AttemptKey &key);
	void applyAnswer(
		const AttemptKey &key,
		const std::vector<QString> &ips,
		crl::time ttl);
	void checkExpireAndPushResult(const QString &domain);

	Fn<void(
		const QString &domain,
		const QStringList &ips,
		crl::time expireAt)> _callback;

	std::set<AttemptKey> _requested;
	std::map<AttemptKey, CacheEntry> _cache;
	crl::time _lastTimestamp = 0;

};

} // namespace MTP::details
