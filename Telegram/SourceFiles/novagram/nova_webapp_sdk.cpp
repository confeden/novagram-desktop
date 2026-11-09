/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_webapp_sdk.h"

#include "logs.h"
#include "settings.h"
#include "webview/webview_data_stream_memory.h"
#include "webview/webview_interface.h"

#include <QtCore/QFile>
#include <QtCore/QPointer>
#include <QtCore/QSaveFile>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace NovaGram::WebAppSdk {
namespace {

constexpr auto kPrefix = "https://telegram.org/js/telegram-web-app.js";
constexpr auto kLiveTimeout = 3000;
constexpr auto kBlockedFor = crl::time(10 * 60 * 1000);
constexpr auto kMaxBytes = 1024 * 1024;
constexpr auto kMime = "application/javascript";

crl::time BlockedUntil = 0;

[[nodiscard]] QString CachePath() {
	return cWorkingDir() + u"tdata/novagram_telegram-web-app.js"_q;
}

[[nodiscard]] bool LooksLikeSdk(const QByteArray &body) {
	return (body.size() >= 10'000)
		&& (body.size() <= kMaxBytes)
		&& body.contains("TelegramWebviewProxy");
}

[[nodiscard]] QByteArray ReadIfSdk(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxBytes) {
		return {};
	}
	const auto body = file.readAll();
	return LooksLikeSdk(body) ? body : QByteArray();
}

void Store(const QByteArray &body) {
	auto file = QSaveFile(CachePath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(body);
		file.commit();
	}
}

[[nodiscard]] QByteArray Local() {
	auto result = ReadIfSdk(CachePath());
	if (result.isEmpty()) {
		result = ReadIfSdk(u":/novagram/telegram-web-app.js"_q);
	}
	return result;
}

void Answer(const Webview::DataRequest &request, const QByteArray &body) {
	request.done({
		.stream = std::make_unique<Webview::DataStreamFromMemory>(
			body,
			kMime),
	});
}

QNetworkAccessManager *Manager() {
	static auto manager = (QNetworkAccessManager*)nullptr;
	if (!manager) {
		manager = new QNetworkAccessManager();
	}
	return manager;
}

Webview::DataResult Handle(Webview::DataRequest request) {
	if (crl::now() < BlockedUntil) {
		const auto local = Local();
		if (local.isEmpty()) {
			return Webview::DataResult::Failed;
		}
		Answer(request, local);
		return Webview::DataResult::Done;
	}
	auto live = QNetworkRequest(QUrl(QString::fromStdString(request.id)));
	live.setTransferTimeout(kLiveTimeout);
	live.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::ManualRedirectPolicy);
	const auto reply = Manager()->get(live);
	// The transfer timeout is reset by every byte; a connect that never
	// completes is what this guards, so a plain deadline stands beside it.
	const auto guard = QPointer<QNetworkReply>(reply);
	QTimer::singleShot(kLiveTimeout, reply, [=] {
		if (guard && guard->isRunning() && !guard->bytesAvailable()) {
			guard->abort();
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		reply->deleteLater();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto body = (reply->error() == QNetworkReply::NoError
			&& status == 200)
			? reply->read(kMaxBytes + 1)
			: QByteArray();
		if (LooksLikeSdk(body)) {
			Store(body);
			Answer(request, body);
			return;
		}
		BlockedUntil = crl::now() + kBlockedFor;
		LOG(("NovaGram WebAppSdk: telegram.org not reached (%1), "
			"serving the kept copy").arg(reply->errorString()));
		const auto local = Local();
		if (!local.isEmpty()) {
			Answer(request, local);
		} else {
			request.done({});
		}
	});
	return Webview::DataResult::Pending;
}

} // namespace

void Register() {
	Webview::SetInterceptedUrls({ kPrefix }, Handle);
}

} // namespace NovaGram::WebAppSdk
