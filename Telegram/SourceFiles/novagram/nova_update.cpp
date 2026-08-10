/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_update.h"

#include "base/timer.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "novagram/nova_branding.h"
#include "novagram/nova_decoy.h"
#include "settings.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QSaveFile>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace NovaGram::Update {
namespace {

constexpr auto kManifestUrl = "https://raw.githubusercontent.com"
	"/confeden/nova_updates/main/Novagram_PC.json";
constexpr auto kEnabledKey = "novagram_update_check"_cs;
constexpr auto kPeriod = crl::time(8 * 60 * 60 * 1000);

// Not zero: the first seconds after start are the busiest ones, and an update
// check is the least urgent thing happening then.
constexpr auto kFirstDelay = crl::time(30 * 1000);

// A manifest is a few hundred bytes and an installer is tens of megabytes.
// Both limits exist so that a wrong or hostile answer cannot fill the disk.
constexpr auto kMaxManifestSize = 64 * 1024;
constexpr auto kMaxInstallerSize = 512 * 1024 * 1024;

[[nodiscard]] QString FolderPath() {
	return cWorkingDir() + u"novagram_update/"_q;
}

class Checker final {
public:
	Checker();

	void start();
	void checkNow();
	void download();
	void installAndRestart();

	[[nodiscard]] Status current() const;
	[[nodiscard]] rpl::producer<Status> statusValue() const;

private:
	void set(Phase phase);
	void applyManifest(const QByteArray &body);
	void finishDownload(const QByteArray &body);
	void fail();

	rpl::variable<Status> _status;
	QNetworkAccessManager _manager;
	QPointer<QNetworkReply> _reply;
	base::Timer _timer;
	QString _installer;

};

Checker::Checker() {
	_timer.setCallback([=] { checkNow(); });
}

Status Checker::current() const {
	return _status.current();
}

rpl::producer<Status> Checker::statusValue() const {
	return _status.value();
}

void Checker::set(Phase phase) {
	auto status = _status.current();
	status.phase = phase;
	_status = status;
}

void Checker::start() {
	if (Decoy::Active() || !CheckEnabled()) {
		return;
	}
	_timer.callOnce(kFirstDelay);
}

void Checker::fail() {
	set(Phase::Failed);
	if (!Decoy::Active() && CheckEnabled()) {
		_timer.callOnce(kPeriod);
	}
}

void Checker::checkNow() {
	if (Decoy::Active() || !CheckEnabled() || _reply) {
		return;
	}
	set(Phase::Checking);

	auto request = QNetworkRequest(QUrl(QString::fromLatin1(kManifestUrl)));
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setMaximumRedirectsAllowed(5);
	const auto reply = _manager.get(request);
	_reply = reply;
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		_reply = nullptr;
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			fail();
			return;
		}
		applyManifest(reply->read(kMaxManifestSize));
	});
}

void Checker::applyManifest(const QByteArray &body) {
	const auto document = QJsonDocument::fromJson(body);
	if (!document.isObject()) {
		fail();
		return;
	}
	const auto object = document.object();
	auto release = Release{
		.version = object.value(u"version"_q).toString(),
		.url = object.value(u"url"_q).toString(),
		.sha256 = object.value(u"sha256"_q).toString().toLower(),
		.releaseUrl = object.value(u"release_url"_q).toString(),
	};
	if (release.version.isEmpty()) {
		fail();
		return;
	} else if (!release.releaseUrl.isEmpty()
		&& !release.releaseUrl.startsWith(ProjectUrl())) {
		// The manifest may only ever point back into the project it belongs
		// to. It is a file in a repository, and a repository can be edited by
		// more people than the one who signs the releases.
		fail();
		return;
	} else if (!release.url.isEmpty()
		&& !release.url.startsWith(ProjectUrl() + u"/releases/download/"_q)) {
		fail();
		return;
	}

	auto status = Status();
	if (CompareVersions(release.version, AppVersion()) <= 0) {
		status.phase = Phase::UpToDate;
	} else {
		status.phase = Phase::Found;
		status.release = release;
	}
	_status = status;

	_timer.callOnce(kPeriod);
}

void Checker::download() {
	const auto release = _status.current().release;
	if (_reply || release.url.isEmpty()) {
		return;
	} else if (release.sha256.size() != 64) {
		// An installer that cannot be checked is not installed. Publishing a
		// release without the digest is a mistake worth failing loudly on.
		fail();
		return;
	}
	auto status = _status.current();
	status.phase = Phase::Downloading;
	status.progress = 0;
	_status = status;

	auto request = QNetworkRequest(QUrl(release.url));
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setMaximumRedirectsAllowed(5);
	const auto reply = _manager.get(request);
	_reply = reply;
	QObject::connect(reply, &QNetworkReply::downloadProgress, [=](
			qint64 received,
			qint64 total) {
		if (total <= 0 || received > kMaxInstallerSize) {
			return;
		}
		auto status = _status.current();
		status.progress = int((received * 100) / total);
		_status = status;
	});
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		_reply = nullptr;
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			fail();
			return;
		}
		finishDownload(reply->read(kMaxInstallerSize));
	});
}

void Checker::finishDownload(const QByteArray &body) {
	const auto release = _status.current().release;
	const auto digest = QCryptographicHash::hash(
		body,
		QCryptographicHash::Sha256).toHex();
	if (body.isEmpty() || QString::fromLatin1(digest) != release.sha256) {
		fail();
		return;
	}
	QDir().mkpath(FolderPath());
	const auto path = FolderPath()
		+ u"NovaGramSetup-"_q
		+ release.version
		+ u".exe"_q;
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)
		|| file.write(body) != body.size()
		|| !file.commit()) {
		fail();
		return;
	}
	_installer = path;
	set(Phase::Ready);
}

void Checker::installAndRestart() {
	if (_installer.isEmpty() || !QFile::exists(_installer)) {
		return;
	}
	// Silent, because the user already agreed in the application, and with the
	// finish task left enabled so that the installer starts the new build.
	const auto started = QProcess::startDetached(_installer, {
		u"/VERYSILENT"_q,
		u"/SUPPRESSMSGBOXES"_q,
		u"/NORESTART"_q,
	});
	if (started) {
		Core::Quit();
	} else {
		fail();
	}
}

[[nodiscard]] Checker &Instance() {
	static auto result = Checker();
	return result;
}

} // namespace

bool CheckEnabled() {
	return !Decoy::Active()
		&& Core::App().settings().readPref<bool>(kEnabledKey, true);
}

void SetCheckEnabled(bool enabled) {
	Core::App().settings().writePref<bool>(kEnabledKey, enabled);
	Core::App().saveSettingsDelayed();
	if (enabled) {
		Instance().checkNow();
	}
}

void Start() {
	Instance().start();
}

Status Current() {
	return Instance().current();
}

rpl::producer<Status> StatusValue() {
	return Instance().statusValue();
}

void CheckNow() {
	Instance().checkNow();
}

void Download() {
	Instance().download();
}

void InstallAndRestart() {
	Instance().installAndRestart();
}

} // namespace NovaGram::Update
