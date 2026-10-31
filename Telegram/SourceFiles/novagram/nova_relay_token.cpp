/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_relay_token.h"

#include "crl/crl_on_main.h"
#include "logs.h"
#include "novagram/nova_decoy.h"
#include "novagram/nova_doh.h"
#include "novagram/nova_wss.h"
#include "settings.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMessageAuthenticationCode>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace NovaGram::RelayToken {
namespace {

// Tried in order; the next one only when the connection fails or times out,
// never on an HTTP answer. The first is on an address that is reachable where
// the apex is not.
constexpr const char *kBases[] = {
	"https://relay-tg.nova-app.eu/relay/",
	"https://nova-app.eu/relay/",
};
constexpr auto kBasesCount = int(sizeof(kBases) / sizeof(kBases[0]));
std::atomic<int> LastBase = 0;
constexpr auto kTimeout = 30 * 1000;
constexpr auto kMaxBody = 16 * 1024;
constexpr auto kWorkTimeoutMs = 60 * 1000;
constexpr auto kMaxBits = 26; // The issuer asks 20, a little more under load.
constexpr auto kRefreshBefore = qint64(10 * 60);
constexpr auto kEnrollBackoff = qint64(6 * 60 * 60);
constexpr auto kReenrollEvery = qint64(24 * 60 * 60);
constexpr auto kTokenRetry = qint64(10 * 60);
constexpr auto kTokenRetryBusy = qint64(60 * 60);
constexpr auto kKickThrottle = crl::time(5000);

// ---- Main thread only. ------------------------------------------------------

struct Credential {
	QString id;
	QString secret;
	qint64 enrolledAt = 0;
};

Credential Cred;
qint64 EnrollAfter = 0;
qint64 TokenAfter = 0;
bool Loaded = false;
bool Busy = false;
bool LoggedEnroll = false;
bool LoggedToken = false;
QNetworkAccessManager *Manager = nullptr;

// ---- Any thread. ------------------------------------------------------------

std::mutex TokenMutex;
QString TokenValue;
qint64 TokenExp = 0;
std::atomic<crl::time> LastKick = 0;

[[nodiscard]] qint64 Now() {
	return QDateTime::currentSecsSinceEpoch();
}

[[nodiscard]] bool GoodToken(const QString &token) {
	static const auto pattern = QRegularExpression(
		u"^nova2\\.[A-Za-z0-9_=-]{1,2048}\\.[A-Za-z0-9_=-]{1,512}$"_q);
	return pattern.match(token).hasMatch();
}

[[nodiscard]] bool GoodText(const QString &text) {
	static const auto pattern = QRegularExpression(
		u"^[A-Za-z0-9_=.-]{1,256}$"_q);
	return pattern.match(text).hasMatch();
}

[[nodiscard]] qint64 Remaining() {
	const auto lock = std::lock_guard(TokenMutex);
	return TokenValue.isEmpty() ? 0 : (TokenExp - Now());
}

void StoreToken(const QString &token, qint64 exp) {
	const auto lock = std::lock_guard(TokenMutex);
	TokenValue = token;
	TokenExp = exp;
}

[[nodiscard]] QString FilePath() {
	return cWorkingDir() + u"novagram_wss/relay_token.json"_q;
}

void Save() {
	QDir().mkpath(cWorkingDir() + u"novagram_wss/"_q);
	auto file = QSaveFile(FilePath());
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	file.write(QJsonDocument(QJsonObject{
		{ u"id"_q, Cred.id },
		{ u"secret"_q, Cred.secret },
		{ u"enrolled_at"_q, double(Cred.enrolledAt) },
		{ u"enroll_after"_q, double(EnrollAfter) },
	}).toJson(QJsonDocument::Compact));
	file.commit();
}

void Load() {
	if (Loaded) {
		return;
	}
	Loaded = true;
	auto file = QFile(FilePath());
	if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxBody) {
		return;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	const auto id = object.value(u"id"_q).toString();
	const auto secret = object.value(u"secret"_q).toString();
	if (GoodText(id) && GoodText(secret)) {
		Cred.id = id;
		Cred.secret = secret;
	}
	Cred.enrolledAt = qint64(object.value(u"enrolled_at"_q).toDouble());
	EnrollAfter = qint64(object.value(u"enroll_after"_q).toDouble());
}

// One request to the issuer, answered with the status and the JSON object (empty
// when the body is not one). The name is resolved through NovaGram DoH and the
// certificate is checked against the pinned roots, as for relay.json.
void RequestAt(
		int base,
		const QString &path,
		const QJsonObject *body,
		Fn<void(int, QJsonObject)> done) {
	if (!Manager) {
		Manager = new QNetworkAccessManager();
	}
	const auto url = QUrl(QString::fromLatin1(kBases[base]) + path);
	const auto host = url.host();
	const auto post = (body != nullptr);
	const auto payload = post
		? QJsonDocument(*body).toJson(QJsonDocument::Compact)
		: QByteArray();
	const auto proxy = Doh::ProxyFor(url);
	Manager->setProxy(proxy);
	const auto issue = [=](QUrl target, bool resolved) {
		auto request = QNetworkRequest(target);
		request.setSslConfiguration(Doh::PinnedConfiguration());
		request.setTransferTimeout(kTimeout);
		request.setMaximumRedirectsAllowed(0);
		request.setAttribute(
			QNetworkRequest::RedirectPolicyAttribute,
			QNetworkRequest::ManualRedirectPolicy);
		if (resolved) {
			request.setPeerVerifyName(host);
			request.setRawHeader("Host", host.toLatin1());
			request.setAttribute(
				QNetworkRequest::Http2AllowedAttribute,
				false);
		}
		if (post) {
			request.setHeader(
				QNetworkRequest::ContentTypeHeader,
				u"application/json"_q);
		}
		const auto reply = post
			? Manager->post(request, payload)
			: Manager->get(request);
		QObject::connect(reply, &QNetworkReply::finished, [=] {
			reply->deleteLater();
			const auto status = reply->attribute(
				QNetworkRequest::HttpStatusCodeAttribute).toInt();
			if (reply->error() != QNetworkReply::NoError || status != 200) {
				done(status, QJsonObject());
				return;
			}
			const auto bytes = reply->read(kMaxBody + 1);
			if (bytes.size() > kMaxBody) {
				done(0, QJsonObject());
				return;
			}
			done(status, QJsonDocument::fromJson(bytes).object());
		});
	};
	if (proxy.capabilities() & QNetworkProxy::HostNameLookupCapability) {
		issue(url, false);
		return;
	}
	Doh::Resolve(host, Doh::RecordType::A, [=](Doh::Answer answer) {
		if (answer.values.empty()) {
			done(0, QJsonObject());
			return;
		}
		auto target = url;
		target.setHost(answer.values.front());
		issue(target, true);
	});
}

// Walks the bases from the one that worked last; a status of zero is "no
// connection / no answer", the only thing that moves on to the next base.
void Request(
		const QString &path,
		const QJsonObject *body,
		Fn<void(int, QJsonObject)> done) {
	const auto first = LastBase.load();
	const auto payload = body ? *body : QJsonObject();
	const auto post = (body != nullptr);
	const auto step = std::make_shared<Fn<void(int)>>();
	// The pending request holds the step; the step holds itself only weakly.
	*step = [=, weak = std::weak_ptr<Fn<void(int)>>(step)](int tried) {
		const auto base = (first + tried) % kBasesCount;
		const auto strong = weak.lock();
		if (!strong) {
			return;
		}
		RequestAt(base, path, post ? &payload : nullptr, [=](
				int status,
				QJsonObject answer) {
			if (status != 0) {
				LastBase.store(base);
			} else if (tried + 1 < kBasesCount) {
				(*strong)(tried + 1);
				return;
			}
			done(status, std::move(answer));
		});
	};
	(*step)(0);
}

void RequestToken();

void EnrollFailed(const QString &stage, int status) {
	Busy = false;
	EnrollAfter = Now() + kEnrollBackoff;
	Save();
	if (!LoggedEnroll) {
		LoggedEnroll = true;
		LOG(("WSS: own relay enrollment failed at %1 (%2), not before 6 h."
			).arg(stage, QString::number(status)));
	}
}

void Enroll(const QJsonObject &challenge, qint64 n) {
	auto body = challenge;
	body.insert(u"n"_q, double(n));
	Request(u"enroll"_q, &body, [](int status, QJsonObject answer) {
		const auto id = answer.value(u"id"_q).toString();
		const auto secret = answer.value(u"secret"_q).toString();
		if (status != 200 || !GoodText(id) || !GoodText(secret)) {
			EnrollFailed(u"enroll"_q, status);
			return;
		}
		Cred = Credential{ id, secret, Now() };
		EnrollAfter = 0;
		Save();
		RequestToken();
	});
}

void BeginEnroll() {
	Request(u"challenge"_q, nullptr, [](int status, QJsonObject answer) {
		const auto c = answer.value(u"c"_q).toString();
		const auto m = answer.value(u"m"_q).toString();
		const auto d = answer.value(u"d"_q).toInt();
		if (status != 200
			|| c.isEmpty()
			|| m.isEmpty()
			|| d < 1
			|| d > kMaxBits
			|| !answer.value(u"e"_q).isDouble()) {
			EnrollFailed(u"challenge"_q, status);
			return;
		}
		// The challenge goes back as it came, with the answer added.
		const auto echo = QJsonObject{
			{ u"c"_q, c },
			{ u"e"_q, answer.value(u"e"_q) },
			{ u"d"_q, d },
			{ u"m"_q, m },
		};
		std::thread([echo, c, d] {
			const auto n = SolveWork(c.toUtf8(), d, kWorkTimeoutMs);
			crl::on_main([echo, n] {
				if (!n) {
					EnrollFailed(u"work"_q, 0);
				} else {
					Enroll(echo, *n);
				}
			});
		}).detach();
	});
}

void RequestToken() {
	const auto id = Cred.id;
	const auto ts = Now();
	const auto body = QJsonObject{
		{ u"id"_q, id },
		{ u"ts"_q, double(ts) },
		{ u"mac"_q, TokenMac(id, Cred.secret, ts) },
	};
	Request(u"token"_q, &body, [=](int status, QJsonObject answer) {
		Busy = false;
		const auto token = answer.value(u"token"_q).toString();
		const auto exp = qint64(answer.value(u"exp"_q).toDouble());
		if (status == 200 && GoodToken(token) && exp > Now() + 60) {
			StoreToken(token, exp);
			TokenAfter = 0;
			return;
		}
		if (status == 403) {
			// Unknown or revoked here: the stored credential is no good. A new
			// one is enrolled, but not more often than once a day.
			Cred = Credential{ QString(), QString(), Cred.enrolledAt };
			StoreToken(QString(), 0);
			EnrollAfter = std::max(
				EnrollAfter,
				Cred.enrolledAt + kReenrollEvery);
			Save();
		}
		TokenAfter = Now() + ((status == 429)
			? kTokenRetryBusy
			: kTokenRetry);
		if (!LoggedToken) {
			LoggedToken = true;
			LOG(("WSS: own relay token not had (%1).").arg(status));
		}
	});
}

void Kick() {
	if (Busy || Decoy::Active() || !Wss::Enabled()) {
		return;
	}
	Load();
	const auto now = Now();
	if (!Cred.id.isEmpty()) {
		if (Remaining() > kRefreshBefore || now < TokenAfter) {
			return;
		}
		Busy = true;
		RequestToken();
	} else if (now >= EnrollAfter) {
		Busy = true;
		BeginEnroll();
	}
}

} // namespace

QString Current() {
	const auto lock = std::lock_guard(TokenMutex);
	return (TokenExp > Now()) ? TokenValue : QString();
}

void Ensure() {
	if (Remaining() > kRefreshBefore) {
		return;
	}
	const auto now = crl::now();
	const auto last = LastKick.load();
	if (last && (now - last) < kKickThrottle) {
		return;
	}
	LastKick.store(now);
	crl::on_main([] { Kick(); });
}

QString TokenMac(
		const QString &id,
		const QString &secret,
		qint64 timestamp) {
	const auto message = QString(u"token|%1|%2"_q).arg(
		id,
		QString::number(timestamp)).toUtf8();
	return QString::fromLatin1(QMessageAuthenticationCode::hash(
		message,
		secret.toUtf8(),
		QCryptographicHash::Sha256).toHex());
}

int LeadingZeroBits(const QByteArray &digest) {
	auto result = 0;
	for (const auto byte : digest) {
		const auto value = uchar(byte);
		if (!value) {
			result += 8;
			continue;
		}
		for (auto mask = 0x80; mask && !(value & mask); mask >>= 1) {
			++result;
		}
		break;
	}
	return result;
}

std::optional<qint64> SolveWork(
		const QByteArray &challenge,
		int bits,
		int timeoutMs) {
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeoutMs);
	auto message = challenge + '|';
	const auto prefix = message.size();
	for (auto n = qint64(0);; ++n) {
		message.truncate(prefix);
		message.append(QByteArray::number(n));
		if (LeadingZeroBits(QCryptographicHash::hash(
				message,
				QCryptographicHash::Sha256)) >= bits) {
			return n;
		}
		if (!(n & 0x3FFF)
			&& std::chrono::steady_clock::now() > deadline) {
			return std::nullopt;
		}
	}
}

} // namespace NovaGram::RelayToken
