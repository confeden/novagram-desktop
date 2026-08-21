/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/details/mtproto_domain_resolver.h"

#include "base/random.h"
#include "base/invoke_queued.h"
#include "base/call_delayed.h"
#include "base/weak_qptr.h"
#include "novagram/nova_doh.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

namespace MTP::details {
namespace {

constexpr auto kMinTimeToLive = 10 * crl::time(1000);
constexpr auto kMaxTimeToLive = 300 * crl::time(1000);

} // namespace

const std::vector<QString> &DnsDomains() {
	static const auto kResult = std::vector<QString>{
		"google.com",
		"www.google.com",
		"google.ru",
		"www.google.ru",
	};
	return kResult;
}

QString GenerateDnsRandomPadding() {
	constexpr char kValid[] = "abcdefghijklmnopqrstuvwxyz"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

	auto result = QString();
	const auto count = [&] {
		constexpr auto kMinPadding = 13;
		constexpr auto kMaxPadding = 128;
		while (true) {
			const auto result = 1 + (base::RandomValue<uchar>() / 2);
			Assert(result <= kMaxPadding);
			if (result >= kMinPadding) {
				return result;
			}
		}
	}();
	result.resize(count);
	for (auto &ch : result) {
		ch = kValid[base::RandomValue<uchar>() % (sizeof(kValid) - 1)];
	}
	return result;
}

QByteArray DnsUserAgent() {
	static const auto kResult = QByteArray(
		"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
		"AppleWebKit/537.36 (KHTML, like Gecko) "
		"Chrome/150.0.0.0 Safari/537.36");
	return kResult;
}

std::vector<DnsEntry> ParseDnsResponse(
		const QByteArray &bytes,
		std::optional<int> typeRestriction) {
	if (bytes.isEmpty()) {
		return {};
	}

	// Read and store to "result" all the data bytes from the response:
	// { ..,
	//   "Answer": [
	//     { .., "data": "bytes1", "TTL": int, .. },
	//     { .., "data": "bytes2", "TTL": int, .. }
	//   ],
	// .. }
	auto error = QJsonParseError{ 0, QJsonParseError::NoError };
	const auto document = QJsonDocument::fromJson(bytes, &error);
	if (error.error != QJsonParseError::NoError) {
		LOG(("Config Error: Failed to parse dns response JSON, error: %1"
			).arg(error.errorString()));
		return {};
	} else if (!document.isObject()) {
		LOG(("Config Error: Not an object received in dns response JSON."));
		return {};
	}
	const auto response = document.object();
	const auto answerIt = response.find("Answer");
	if (answerIt == response.constEnd()) {
		LOG(("Config Error: Could not find Answer in dns response JSON."));
		return {};
	} else if (!(*answerIt).isArray()) {
		LOG(("Config Error: Not an array received "
			"in Answer in dns response JSON."));
		return {};
	}

	const auto array = (*answerIt).toArray();
	auto result = std::vector<DnsEntry>();
	for (const auto elem : array) {
		if (!elem.isObject()) {
			LOG(("Config Error: Not an object found "
				"in Answer array in dns response JSON."));
			continue;
		}
		const auto object = elem.toObject();
		if (typeRestriction) {
			const auto typeIt = object.find("type");
			const auto type = int(base::SafeRound((*typeIt).toDouble()));
			if (!(*typeIt).isDouble()) {
				LOG(("Config Error: Not a number in type field "
					"in Answer array in dns response JSON."));
				continue;
			} else if (type != *typeRestriction) {
				continue;
			}
		}
		const auto dataIt = object.find("data");
		if (dataIt == object.constEnd()) {
			LOG(("Config Error: Could not find data "
				"in Answer array entry in dns response JSON."));
			continue;
		} else if (!(*dataIt).isString()) {
			LOG(("Config Error: Not a string data found "
				"in Answer array entry in dns response JSON."));
			continue;
		}

		const auto ttlIt = object.find("TTL");
		const auto ttl = (ttlIt != object.constEnd())
			? crl::time(base::SafeRound((*ttlIt).toDouble()))
			: crl::time(0);
		result.push_back({ (*dataIt).toString(), ttl });
	}
	return result;
}

ServiceWebRequest::ServiceWebRequest(not_null<QNetworkReply*> reply)
: reply(reply.get()) {
}

ServiceWebRequest::ServiceWebRequest(ServiceWebRequest &&other)
: reply(base::take(other.reply)) {
}

ServiceWebRequest &ServiceWebRequest::operator=(ServiceWebRequest &&other) {
	if (reply != other.reply) {
		destroy();
		reply = base::take(other.reply);
	}
	return *this;
}

void ServiceWebRequest::destroy() {
	if (const auto value = base::take(reply)) {
		value->disconnect(
			value,
			&QNetworkReply::finished,
			nullptr,
			nullptr);
		value->abort();
		value->deleteLater();
	}
}

ServiceWebRequest::~ServiceWebRequest() {
	if (reply) {
		reply->deleteLater();
	}
}

DomainResolver::DomainResolver(Fn<void(
	const QString &host,
	const QStringList &ips,
	crl::time expireAt)> callback)
: _callback(std::move(callback)) {
}

void DomainResolver::resolve(const QString &domain) {
	resolve({ domain, false });
	resolve({ domain, true });
}

void DomainResolver::resolve(const AttemptKey &key) {
	if (_requested.contains(key)) {
		return;
	}
	const auto i = _cache.find(key);
	_lastTimestamp = crl::now();
	if (i != end(_cache) && i->second.expireAt > _lastTimestamp) {
		checkExpireAndPushResult(key.domain);
		return;
	}
	_requested.emplace(key);

	const auto weak = base::make_weak(this);
	NovaGram::Doh::Resolve(
		key.domain,
		(key.ipv6
			? NovaGram::Doh::RecordType::AAAA
			: NovaGram::Doh::RecordType::A),
		[=](NovaGram::Doh::Answer answer) {
			if (!weak) {
				return;
			}
			applyAnswer(key, answer.values, answer.ttl);
		});
}

void DomainResolver::applyAnswer(
		const AttemptKey &key,
		const std::vector<QString> &ips,
		crl::time ttl) {
	_requested.erase(key);
	if (ips.empty()) {
		// Nothing is cached, so the next attempt asks again rather than
		// remembering a failure - and nothing is pushed to the callback,
		// because the caller must not read "the resolver said no" as
		// permission to reach the name some other way.
		DEBUG_LOG(("NovaGram DoH: no %1 record for %2.").arg(
			key.ipv6 ? u"AAAA"_q : u"A"_q,
			key.domain));
		return;
	}

	auto entry = CacheEntry();
	for (const auto &ip : ips) {
		entry.ips.push_back(ip);
	}
	_lastTimestamp = crl::now();
	entry.expireAt = _lastTimestamp
		+ std::clamp(ttl, kMinTimeToLive, kMaxTimeToLive);
	_cache[key] = std::move(entry);

	checkExpireAndPushResult(key.domain);
}

void DomainResolver::checkExpireAndPushResult(const QString &domain) {
	const auto ipv4 = _cache.find({ domain, false });
	if (ipv4 == end(_cache) || ipv4->second.expireAt <= _lastTimestamp) {
		return;
	}
	auto result = ipv4->second;
	const auto ipv6 = _cache.find({ domain, true });
	if (ipv6 != end(_cache) && ipv6->second.expireAt > _lastTimestamp) {
		result.ips.append(ipv6->second.ips);
		accumulate_min(result.expireAt, ipv6->second.expireAt);
	}
	InvokeQueued(this, [=] {
		_callback(domain, result.ips, result.expireAt);
	});
}

} // namespace MTP::details
