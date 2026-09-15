/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_autoproxy.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "core/core_settings_proxy.h"
#include "mtproto/mtproto_proxy_data.h"
#include "mtproto/facade.h"
#include "main/main_session.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "base/timer.h"

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

namespace NovaGram {
namespace {

constexpr auto kAutoProxyKey = "novagram_autoproxy"_cs;
constexpr auto kCloudProxyUrl = "https://raw.githubusercontent.com/Telegram-FZ-LLC/Telegram-Proxy/main/proxies.txt";

struct FallbackProxy {
	MTP::ProxyData::Type type;
	const char *host;
	uint32 port;
	const char *secret;
};

// Hardcoded emergency fallback proxies (verified Fake-TLS & SOCKS5)
const FallbackProxy kFallbackProxies[] = {
	{ MTP::ProxyData::Type::Mtproto, "max.kimt.click", 443, "ee1b153cf06dbd43c6085c359a6702eb936d61782e6b696d742e636c69636b" },
	{ MTP::ProxyData::Type::Mtproto, "t.meow-network.com", 443, "ee5622e11fff3e49bcc85280197a6106b5742e6d656f772d6e6574776f726b2e636f6d" },
	{ MTP::ProxyData::Type::Mtproto, "havashenasi.my-moon.co.uk", 8443, "eeNEgYdJvXrFGRMCIMJdCQ" },
	{ MTP::ProxyData::Type::Mtproto, "komi3er.fesgheli.co.uk", 8443, "eeNEgYdJvXrFGRMCIMJdCQ" },
	{ MTP::ProxyData::Type::Socks5, "195.135.255.98", 1080, "" },
	{ MTP::ProxyData::Type::Socks5, "193.233.139.106", 1080, "" },
};

class AutoProxyService final {
public:
	static AutoProxyService &Instance() {
		static AutoProxyService instance;
		return instance;
	}

	void start() {
		if (_started) {
			return;
		}
		_started = true;

		// Populate fallbacks on first launch if proxy list is empty
		initFallbackList();

		auto &proxySettings = Core::App().settings().proxy();
		if (AutoProxyEnabled() && !proxySettings.isEnabled()) {
			if (!proxySettings.list().empty()) {
				Core::App().setCurrentProxy(proxySettings.list().front(), MTP::ProxyData::Settings::Enabled);
				Core::App().saveSettingsDelayed();
			}
		}

		_watchdogTimer.setCallback([this] { checkConnectionWatchdog(); });
		_watchdogTimer.callEach(4000); // check every 4 seconds

		_refreshTimer.setCallback([this] { fetchCloudList(); });
		_refreshTimer.callEach(7200 * 1000); // refresh list every 2 hours

		// Initial cloud fetch
		fetchCloudList();
	}

	void fetchCloudList() {
		if (!AutoProxyEnabled()) {
			return;
		}
		if (_networkReply) {
			return;
		}

		auto request = QNetworkRequest(QUrl(kCloudProxyUrl));
		request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
		request.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0 NovaGram/AutoProxy");

		_networkReply = _networkManager.get(request);
		QObject::connect(_networkReply, &QNetworkReply::finished, [this] {
			if (!_networkReply) {
				return;
			}
			if (_networkReply->error() == QNetworkReply::NoError) {
				const auto data = _networkReply->readAll();
				parseAndMergeProxies(QString::fromUtf8(data));
			}
			_networkReply->deleteLater();
			_networkReply = nullptr;
		});
	}

	[[nodiscard]] QString statusText() const {
		if (!AutoProxyEnabled()) {
			return u"Отключен"_q;
		}
		const auto &proxySettings = Core::App().settings().proxy();
		if (proxySettings.isEnabled()) {
			const auto current = proxySettings.selected();
			const auto typeStr = (current.type == MTP::ProxyData::Type::Mtproto)
				? u"MTProto"_q
				: u"SOCKS5"_q;
			return u"Активен: %1 (%2:%3)\nИсточник: GitHub MTProto Repo, SOCKS5 List"_q
				.arg(typeStr)
				.arg(current.host)
				.arg(current.port);
		}
		return u"Прямое соединение (DC доступен)\nИсточник: GitHub MTProto Repo, SOCKS5 List"_q;
	}

private:
	AutoProxyService() = default;

	void initFallbackList() {
		auto &proxySettings = Core::App().settings().proxy();
		if (!proxySettings.list().empty()) {
			return;
		}

		for (const auto &entry : kFallbackProxies) {
			auto proxy = MTP::ProxyData();
			proxy.type = entry.type;
			proxy.host = QString::fromLatin1(entry.host);
			proxy.port = entry.port;
			proxy.user = QString();
			proxy.password = entry.secret ? QString::fromLatin1(entry.secret) : QString();
			proxySettings.addToList(proxy);
		}
		Core::App().saveSettingsDelayed();
	}

	void parseAndMergeProxies(const QString &text) {
		auto &proxySettings = Core::App().settings().proxy();
		const auto lines = text.split('\n', Qt::SkipEmptyParts);
		auto added = 0;

		for (const auto &rawLine : lines) {
			const auto line = rawLine.trimmed();
			if (!line.startsWith(u"tg://proxy?") && !line.startsWith(u"https://t.me/proxy?")) {
				continue;
			}
			const auto url = QUrl(line);
			const auto query = QUrlQuery(url.query());
			const auto server = query.queryItemValue(u"server"_q);
			const auto port = query.queryItemValue(u"port"_q).toUInt();
			const auto secret = query.queryItemValue(u"secret"_q);

			if (server.isEmpty() || port == 0) {
				continue;
			}

			auto proxy = MTP::ProxyData();
			proxy.type = MTP::ProxyData::Type::Mtproto;
			proxy.host = server;
			proxy.port = port;
			proxy.password = secret;

			if (proxySettings.indexInList(proxy) < 0) {
				proxySettings.addToList(proxy);
				++added;
			}
			if (added >= 15) {
				break;
			}
		}

		if (added > 0) {
			Core::App().saveSettingsDelayed();
		}
	}

	void checkConnectionWatchdog() {
		if (!AutoProxyEnabled()) {
			return;
		}

		// Check if any active account is disconnected or connecting
		auto anyConnectingTooLong = false;
		for (const auto &entry : Core::App().domain().accounts()) {
			const auto account = entry.account.get();
			if (account && account->sessionExists() && !account->mtp().isTestMode()) {
				if (account->mtp().dcstate() != MTP::ConnectedState) {
					anyConnectingTooLong = true;
					break;
				}
			}
		}

		if (!anyConnectingTooLong) {
			_failCount = 0;
			return;
		}

		++_failCount;
		// If hanging for >= 2 ticks (approx 8 seconds), rotate / enable proxy
		if (_failCount >= 2) {
			_failCount = 0;
			activateNextAvailableProxy();
		}
	}

	void activateNextAvailableProxy() {
		auto &proxySettings = Core::App().settings().proxy();
		if (proxySettings.list().empty()) {
			initFallbackList();
		}

		const auto current = proxySettings.selected();
		auto nextIndex = 0;
		for (auto i = 0; i < int(proxySettings.list().size()); ++i) {
			if (proxySettings.list()[i].host == current.host && proxySettings.list()[i].port == current.port) {
				nextIndex = (i + 1) % proxySettings.list().size();
				break;
			}
		}

		if (proxySettings.list().size() > 1) {
			proxySettings.removeFromList(current);
			if (nextIndex > 0) {
				nextIndex--;
			}
		} else {
			fetchCloudList();
		}

		if (proxySettings.list().empty()) {
			initFallbackList();
			nextIndex = 0;
		}

		const auto &list = proxySettings.list();
		if (!list.empty()) {
			nextIndex = nextIndex % list.size();
			Core::App().setCurrentProxy(list[nextIndex], MTP::ProxyData::Settings::Enabled);
			Core::App().saveSettingsDelayed();
		}
	}

	bool _started = false;
	int _failCount = 0;
	base::Timer _watchdogTimer;
	base::Timer _refreshTimer;
	QNetworkAccessManager _networkManager;
	QNetworkReply *_networkReply = nullptr;
};

} // namespace

bool AutoProxyEnabled() {
	return Core::App().settings().readPref<bool>(kAutoProxyKey, true);
}

void SetAutoProxyEnabled(bool enabled) {
	Core::App().settings().writePref<bool>(kAutoProxyKey, enabled);
	Core::App().saveSettingsDelayed();
	if (!enabled) {
		// Revert to system/direct proxy mode
		Core::App().setCurrentProxy(MTP::ProxyData(), MTP::ProxyData::Settings::System);
	} else {
		AutoProxyService::Instance().fetchCloudList();
	}
}

void StartAutoProxy() {
	AutoProxyService::Instance().start();
}

void RefreshCloudProxies() {
	AutoProxyService::Instance().fetchCloudList();
}

QString AutoProxyStatusText() {
	return AutoProxyService::Instance().statusText();
}

} // namespace NovaGram
