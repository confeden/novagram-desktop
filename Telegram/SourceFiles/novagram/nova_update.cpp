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
#include "core/core_settings_proxy.h"
#include "logs.h"
#include "novagram/nova_authenticode.h"
#include "novagram/nova_branding.h"
#include "novagram/nova_decoy.h"
#include "novagram/nova_doh.h"
#include "novagram/nova_pin.h"
#include "settings.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QSaveFile>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace NovaGram::Update {
namespace {

// The release itself, not a file describing it. Everything the check needs is
// already there: the tag carries both bases, and every asset carries its name,
// its size and a sha256 digest computed by GitHub. A separate manifest was one
// more thing that could go stale - and did, once, at the cost of a release.
//
// /releases/latest skips drafts and prereleases, which is exactly the wanted
// behaviour and removes the step where a freshly published release had to be
// followed by a workflow run before any client could see it.
constexpr auto kReleasesUrl = "https://api.github.com"
	"/repos/confeden/Novagram/releases/latest";
constexpr auto kEnabledKey = "novagram_update_check"_cs;
constexpr auto kPeriod = crl::time(8 * 60 * 60 * 1000);

// When the release host was last asked, and until when it asked not to be
// asked again. Both are wall-clock milliseconds and both survive a restart -
// which is the whole point of them, see Checker::start().
constexpr auto kLastAttemptKey = "novagram_update_attempt"_cs;
constexpr auto kRateLimitKey = "novagram_update_rate_limit"_cs;

// Not zero: the first seconds after start are the busiest ones, and an update
// check is the least urgent thing happening then.
constexpr auto kFirstDelay = crl::time(30 * 1000);

// The floor under "check now" in settings. That button had no cooldown at all,
// so holding it down was a way of spending an hourly quota of sixty anonymous
// requests in about a minute - after which the client could not check for
// updates for the rest of the hour, which is the opposite of what pressing it
// was meant to achieve.
constexpr auto kMinManualInterval = crl::time(60 * 1000);

// What to wait when the host refuses but says nothing useful about when it
// will stop, and the most it may ask for however loudly it asks. The upper
// bound matters: without it one broken or hostile header could switch update
// checking off for years.
constexpr auto kRateLimitFallback = crl::time(60 * 60 * 1000);
constexpr auto kRateLimitMax = crl::time(24 * 60 * 60 * 1000);

// A manifest is a few hundred bytes and an installer is tens of megabytes.
// Both limits exist so that a wrong or hostile answer cannot fill the disk.
constexpr auto kMaxManifestSize = 64 * 1024;
constexpr auto kMaxInstallerSize = 512 * 1024 * 1024;

// Without one a request that gets no answer at all never finishes, and the
// only thing the user ever sees is "Checking…" for the rest of the session.
// That is not a theoretical case: the check is the one request that leaves the
// Telegram network, and it goes to a host that is blocked outright in some of
// the places this fork is used.
constexpr auto kManifestTimeout = 20 * 1000;

// The same twenty seconds as the manifest. A long timeout was tried first and
// is the wrong tool: it makes a broken transfer sit there looking alive. What
// makes the download survive a connection that breaks every few megabytes is
// keeping the bytes and continuing from them, which is what happens below.
constexpr auto kDownloadTimeout = 20 * 1000;

// How many times a download may continue by itself after breaking. Only
// attempts that actually moved forward count, so this is a bound on pieces,
// not on retries of nothing.
constexpr auto kMaxDownloadAttempts = 40;

// Between two of those pieces. There used to be nothing here at all, and a
// connection that delivered a kilobyte and dropped turned into forty requests
// in as many milliseconds - which is how a client talks itself into being
// refused by the host it is downloading from. It grows with the number of
// consecutive breaks and stops growing at half a minute.
constexpr auto kDownloadRetryStep = crl::time(1000);
constexpr auto kDownloadRetryMax = crl::time(30 * 1000);

[[nodiscard]] QString FolderPath() {
	return cWorkingDir() + u"novagram_update/"_q;
}

// Both requests this module makes go out with the roots of nova_doh_roots.h
// and nothing else, exactly as the resolver's own queries do.
//
// This is not about which host is being talked to - it is about which
// authorities may vouch for it. A root that a corporate image or a piece of
// malware added to the Windows store is not among them, so it cannot sit in
// the middle of the one exchange in this client that ends with an executable
// being run. The release host chains to an authority that is already on the
// list; a provider that moves off it stops working and says so, which is the
// intended failure and not a reason to widen the list quietly.
void PinRoots(QNetworkRequest &request) {
	request.setSslConfiguration(Doh::PinnedConfiguration());
}

// Qt may follow redirects here: the name was resolved by a proxy rather than
// by us, so there is no system resolver to keep out of the way.
void Prepare(QNetworkRequest &request, int timeout) {
	PinRoots(request);
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setMaximumRedirectsAllowed(5);
	request.setTransferTimeout(timeout);
}

// The address was resolved here, so the name only lives in SNI, in the
// certificate check and in the Host header - and every redirect has to come
// back through this module to be resolved again.
void PrepareManual(
		QNetworkRequest &request,
		const QString &verifyName,
		int timeout) {
	PinRoots(request);
	request.setPeerVerifyName(verifyName);
	request.setRawHeader("Host", verifyName.toLatin1());
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::ManualRedirectPolicy);
	request.setTransferTimeout(timeout);
}

[[nodiscard]] qint64 ReadStamp(std::string_view key) {
	const auto stored = Core::App().settings().readPref<QByteArray>(
		key,
		QByteArray());
	if (stored.isEmpty()) {
		return 0;
	}
	auto ok = false;
	const auto value = stored.toLongLong(&ok);
	return (ok && value > 0) ? value : 0;
}

void WriteStamp(std::string_view key, qint64 value) {
	Core::App().settings().writePref<QByteArray>(
		key,
		QByteArray::number(value));
}

// How long ago the release host was last asked, across restarts. Negative
// means "never, as far as this machine knows" - which also covers a clock that
// has moved backwards since, because a stamp in the future says nothing about
// how long ago anything happened.
[[nodiscard]] crl::time SinceLastAttempt() {
	const auto stamp = ReadStamp(kLastAttemptKey);
	if (!stamp) {
		return -1;
	}
	const auto now = QDateTime::currentMSecsSinceEpoch();
	return (stamp > now) ? -1 : crl::time(now - stamp);
}

// Milliseconds still to wait before the host may be asked at all, zero when it
// may. Unlike the interval above this one also stops a check asked for by
// hand: the host did not say "not so often", it said "not until then".
[[nodiscard]] crl::time RateLimitLeft() {
	const auto until = ReadStamp(kRateLimitKey);
	if (!until) {
		return 0;
	}
	const auto now = QDateTime::currentMSecsSinceEpoch();
	if (until <= now) {
		return 0;
	} else if (until - now > kRateLimitMax) {
		// The clock moved backwards, or the value is nonsense. Either way the
		// answer is not "wait a year".
		return 0;
	}
	return crl::time(until - now);
}

// GitHub answers 403 or 429 when too much has been asked from one address, and
// says in the headers when it will answer again. Believing that is both
// politer and cheaper than guessing: guessing wrong means being refused for the
// rest of the hour and finding out one request at a time.
void RememberRateLimit(QNetworkReply *reply) {
	const auto now = QDateTime::currentMSecsSinceEpoch();
	auto until = qint64(0);

	// An absolute unix time in seconds, and the one GitHub actually sets.
	const auto reset = reply->rawHeader("x-ratelimit-reset").trimmed();
	if (!reset.isEmpty()) {
		auto ok = false;
		const auto seconds = reset.toLongLong(&ok);
		if (ok && seconds > 0) {
			until = seconds * 1000;
		}
	}
	// A number of seconds from now, which the secondary rate limit uses.
	const auto after = reply->rawHeader("Retry-After").trimmed();
	if (!after.isEmpty()) {
		auto ok = false;
		const auto seconds = after.toLongLong(&ok);
		if (ok && seconds > 0) {
			until = std::max(until, now + seconds * 1000);
		}
	}
	if (until <= now) {
		// Neither header made sense. A refusal without a time on it is still a
		// refusal, so it costs the same as the usual one.
		until = now + kRateLimitFallback;
	}
	until = std::min(until, now + kRateLimitMax);
	LOG(("NovaGram update: not asking again for %1 s.").arg(
		QString::number((until - now) / 1000)));
	WriteStamp(kRateLimitKey, until);
}

class Checker final {
public:
	Checker();

	void start();
	void stop();
	void checkNow();
	void download();
	void cancel();
	void installAndRestart();

	[[nodiscard]] Status current() const;
	[[nodiscard]] rpl::producer<Status> statusValue() const;

private:
	void set(Phase phase);
	void applyProxy(const QUrl &url);
	void startDownload(QUrl target, QString verifyName, int hop);
	void getResolved(
		QUrl url,
		int timeout,
		int hop,
		Fn<void(QNetworkReply*)> done);
	void applyManifest(const QByteArray &body);
	void finishDownload(const QByteArray &body);
	[[nodiscard]] bool verifyInstaller();
	void dropInstaller();
	void fail(Failure reason);
	void failCheck(Failure reason);
	void runInstaller();

	rpl::variable<Status> _status;
	QNetworkAccessManager _manager;
	QPointer<QNetworkReply> _reply;
	base::Timer _timer;

	// The pause between two pieces of a download that keeps breaking. Separate
	// from _timer on purpose: that one carries the eight hour cycle, and a
	// download borrowing it would silently cancel the next check.
	base::Timer _retry;

	QString _installer;

	// What earlier attempts of the current release managed to fetch. Kept so a
	// download that broke near the end continues instead of starting over: on
	// the kind of connection this fork is used over, fifty megabytes in one
	// unbroken run is the exception, not the rule.
	QByteArray _partial;
	QString _partialVersion;

	// How much was already held when the running request was made, and whether
	// the server's answer to the Range has been looked at yet.
	int _resumeBase = 0;
	bool _resumeDecided = false;

	// Consecutive automatic continuations. Bounded so that a connection which
	// delivers one byte at a time cannot spin here for ever.
	int _attempts = 0;

	// Set while a download is being stopped on purpose, so that the aborted
	// reply is not reported as a failure: the release is still there and the
	// button has to offer it again, not say that checking went wrong.
	bool _cancelling = false;

	// The installer is started from aboutToQuit rather than right away. Quit()
	// is allowed to refuse - an export, an upload or a download in progress
	// asks the user first - and a silent installer started next to a client
	// that went on working would replace the files under it.
	bool _installOnQuit = false;
	bool _quitHooked = false;

};

Checker::Checker() {
	_timer.setCallback([=] { checkNow(); });
	_retry.setCallback([=] { download(); });
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
	// Any phase at all is news, and news replaces the reason the last attempt
	// gave. A reason that outlived it would label the wrong thing.
	status.failure = Failure::None;
	_status = status;
}

void Checker::start() {
	if (Decoy::Active() || !CheckEnabled()) {
		return;
	}
	// Nothing about the eight hour cycle used to survive the process, so
	// restarting the client was a way of asking GitHub again thirty seconds
	// later, however recently it had answered - and a machine that is switched
	// on and off a few times an hour spent the whole anonymous quota on
	// nothing. What is left of the interval is waited out here instead.
	const auto since = SinceLastAttempt();
	const auto rest = (since < 0 || since >= kPeriod)
		? crl::time(0)
		: (kPeriod - since);
	_timer.callOnce(std::max(rest, kFirstDelay));
}

// The proxy the user chose for Telegram, whenever it is one a plain HTTPS
// request can use. Upstream applies such a proxy to the whole application only
// when it is SOCKS5 or HTTP, and explicitly clears the application proxy when
// an MTProto one is selected - so without this the update check quietly went
// out directly while everything else in the client went through the proxy.
//
// Honest boundary: an MTProto proxy cannot carry this request at all. It
// speaks Telegram's protocol and nothing else. Where the manifest host is
// blocked and the only way out is an MTProto proxy, the check fails - visibly,
// thanks to the timeout, which is the whole of what can be promised here.
void Checker::applyProxy(const QUrl &url) {
	// One policy for everything that leaves the Telegram network, resolver
	// included - see NovaGram::Doh::ProxyFor.
	_manager.setProxy(Doh::ProxyFor(url));
}

// Issues a GET through the fork's own resolver and nowhere else. The name is
// turned into an address here; the request then goes to that address with the
// name carried in SNI, in the certificate check and in the Host header.
//
// Redirects are followed by hand on purpose. GitHub sends the installer
// download on to another host, and letting Qt follow a Location by name would
// put the system resolver back into the middle of the one request this fork
// promises to keep out of it.
void Checker::getResolved(
		QUrl url,
		int timeout,
		int hop,
		Fn<void(QNetworkReply*)> done) {
	if (hop > 5) {
		LOG(("NovaGram update: too many redirects for %1.").arg(url.toString()));
		done(nullptr);
		return;
	} else if (url.scheme() != u"https"_q) {
		LOG(("NovaGram update: refusing a non-https hop to %1.").arg(
			url.toString()));
		done(nullptr);
		return;
	}
	const auto host = url.host();
	const auto proxy = Doh::ProxyFor(url);
	if (proxy.capabilities() & QNetworkProxy::HostNameLookupCapability) {
		// The proxy resolves for us, so no name is looked up on this machine
		// at all - which is what the promise is about, and is stricter than
		// doing it ourselves rather than weaker. Resolving here anyway would
		// also be actively worse: it would pin the connection to one address
		// we chose and throw away the routing the proxy would have picked.
		//
		// This is not a fall-back to the system resolver. It is the case where
		// there is nothing for a resolver to do.
		DEBUG_LOG(("NovaGram update: %1 is resolved by the proxy.").arg(host));
		auto request = QNetworkRequest(url);
		Prepare(request, timeout);
		_manager.setProxy(proxy);
		const auto reply = _manager.get(request);
		_reply = reply;
		QObject::connect(reply, &QNetworkReply::finished, [=] {
			_reply = nullptr;
			reply->deleteLater();
			done(reply);
		});
		return;
	}
	Doh::Resolve(host, Doh::RecordType::A, [=](Doh::Answer answer) {
		if (answer.values.empty()) {
			// No fall-back to the system resolver, by design: a name that
			// quietly resolved through the system is the leak this replaces.
			LOG(("NovaGram update: could not resolve %1 over secure DNS.").arg(
				host));
			done(nullptr);
			return;
		}
		auto target = url;
		target.setHost(answer.values.front());
		applyProxy(url);
		auto request = QNetworkRequest(target);
		PrepareManual(request, host, timeout);
		request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
		const auto reply = _manager.get(request);
		_reply = reply;
		QObject::connect(reply, &QNetworkReply::finished, [=] {
			_reply = nullptr;
			reply->deleteLater();
			const auto redirect = reply->attribute(
				QNetworkRequest::RedirectionTargetAttribute).toUrl();
			if (!redirect.isEmpty()) {
				getResolved(url.resolved(redirect), timeout, hop + 1, done);
				return;
			}
			done(reply);
		});
	});
}

void Checker::stop() {
	_timer.cancel();
	_retry.cancel();
	_attempts = 0;
	// The flag is raised only when there is a reply to abort, and it is the
	// handler of that reply that lowers it again. Raising it unconditionally
	// latched it for the rest of the session: with nothing in flight nobody
	// ever cleared it, and the next answer was thrown away by the very branch
	// that exists to recognise a deliberate abort - so switching the check off
	// and on again silently ended automatic checking until the next start.
	if (_reply) {
		_cancelling = true;
		_reply->abort();
	}
	// Back to Idle rather than left on the last answer: the update bar in the
	// chat list is driven by the phase, and a release found before the switch
	// was turned off would go on offering itself from a client that was told
	// not to look. Half a downloaded installer goes with it - the user asked
	// this to stop, not to be remembered.
	_partial.clear();
	_partialVersion = QString();
	_status = Status();
}

// A check that failed must not leave the release found by an earlier one
// behind: the bar and the state line tell a download failure from a check
// failure by whether a release is known, and a stale one would label the wrong
// thing and offer a button that has nothing to fetch.
void Checker::failCheck(Failure reason) {
	_status = Status();
	fail(reason);
}

void Checker::fail(Failure reason) {
	auto status = _status.current();
	status.phase = Phase::Failed;
	status.failure = reason;
	_status = status;
	if (!Decoy::Active() && CheckEnabled()) {
		_timer.callOnce(kPeriod);
	}
}

void Checker::checkNow() {
	if (Decoy::Active() || !CheckEnabled() || _reply) {
		return;
	}
	if (const auto left = RateLimitLeft()) {
		// Not "later, maybe": the host named a time, and asking before it only
		// makes the refusal last longer.
		LOG(("NovaGram update: refused until %1 s from now, not asking."
			).arg(QString::number(left / 1000)));
		failCheck(Failure::RateLimited);
		return;
	}
	const auto since = SinceLastAttempt();
	if (since >= 0 && since < kMinManualInterval) {
		// Nothing is reported here and that is deliberate: the last attempt
		// finished less than a minute ago, so the line under the switch is
		// already showing its answer, and replacing it with a complaint about
		// pressing the button twice would hide the thing the user asked for.
		LOG(("NovaGram update: asked again after %1 ms, too soon."
			).arg(QString::number(since)));
		// Whoever asked, the cycle has to survive being refused: without this a
		// call that returns here and finds nothing armed would end automatic
		// checking until the next launch.
		if (!_timer.isActive() && !Decoy::Active() && CheckEnabled()) {
			_timer.callOnce(kPeriod - since);
		}
		return;
	}
	// Written down before the answer and not after it, so that a check which
	// fails, times out or never comes back still costs the same eight hours as
	// one that succeeds. Otherwise a host that is blocked - which is the
	// normal case in some of the places this fork is used - would be asked
	// again on every launch for ever.
	WriteStamp(kLastAttemptKey, QDateTime::currentMSecsSinceEpoch());

	set(Phase::Checking);
	LOG(("NovaGram update: asking %1.").arg(QString::fromLatin1(kReleasesUrl)));

	const auto url = QUrl(QString::fromLatin1(kReleasesUrl));
	getResolved(url, kManifestTimeout, 0, [=](QNetworkReply *reply) {
		if (_cancelling) {
			// The switch was turned off while the manifest was on its way.
			_cancelling = false;
			return;
		} else if (!reply) {
			failCheck(Failure::Network);
			return;
		}
		const auto code = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (code == 403 || code == 429) {
			// Looked at before the error, because Qt reports both of these as
			// errors and "check your connection" is the wrong thing to say
			// about a host that answered perfectly well.
			LOG(("NovaGram update: the release host answered %1.").arg(code));
			RememberRateLimit(reply);
			failCheck(Failure::RateLimited);
			return;
		} else if (reply->error() != QNetworkReply::NoError) {
			LOG(("NovaGram update: check failed, %1 (%2).").arg(
				reply->errorString(),
				QString::number(int(reply->error()))));
			failCheck(Failure::Network);
			return;
		}
		applyManifest(reply->read(kMaxManifestSize));
	});
}

// The half of the tag that belongs to this platform. A tag names both bases -
// "v7.0.9.2/12.9.2.2" - because one release covers two clients, and each of
// them compares only its own half.
[[nodiscard]] QString VersionFromTag(const QString &tag) {
	auto trimmed = tag;
	if (trimmed.startsWith('v')) {
		trimmed = trimmed.mid(1);
	}
	const auto slash = trimmed.indexOf('/');
	return (slash < 0) ? trimmed : trimmed.left(slash);
}

void Checker::applyManifest(const QByteArray &body) {
	const auto document = QJsonDocument::fromJson(body);
	if (!document.isObject()) {
		LOG(("NovaGram update: the answer is not a release object."));
		failCheck(Failure::Network);
		return;
	}
	const auto object = document.object();
	auto release = Release{
		.version = VersionFromTag(object.value(u"tag_name"_q).toString()),
		.releaseUrl = object.value(u"html_url"_q).toString(),
	};
	// The asset is found by what it is, not by its exact name: a rename in the
	// packaging script must not silently stop every client from updating.
	for (const auto value : object.value(u"assets"_q).toArray()) {
		const auto asset = value.toObject();
		const auto name = asset.value(u"name"_q).toString();
		if (!name.endsWith(u".exe"_q, Qt::CaseInsensitive)) {
			continue;
		}
		release.url = asset.value(u"browser_download_url"_q).toString();
		const auto digest = asset.value(u"digest"_q).toString().toLower();
		// GitHub gives it as "sha256:<hex>". Anything else is not a digest we
		// know how to check, and a check that silently passes is worse than
		// none - so an unknown algorithm leaves the field empty and the
		// download refuses to install.
		if (digest.startsWith(u"sha256:"_q)) {
			release.sha256 = digest.mid(7);
		}
		break;
	}
	if (release.version.isEmpty()) {
		LOG(("NovaGram update: the answer names no version."));
		failCheck(Failure::Network);
		return;
	} else if (!release.releaseUrl.isEmpty()
		&& !release.releaseUrl.startsWith(ProjectUrl())) {
		// The answer may only ever point back into the project it belongs to.
		LOG(("NovaGram update: the answer points at %1, refusing."
			).arg(release.releaseUrl));
		failCheck(Failure::Network);
		return;
	} else if (!release.url.isEmpty()
		&& !release.url.startsWith(ProjectUrl() + u"/releases/download/"_q)) {
		LOG(("NovaGram update: the asset is at %1, refusing."
			).arg(release.url));
		failCheck(Failure::Network);
		return;
	}

	auto status = Status();
	if (CompareVersions(release.version, AppVersion()) <= 0) {
		status.phase = Phase::UpToDate;
	} else {
		status.phase = Phase::Found;
		status.release = release;
	}
	LOG(("NovaGram update: release says %1, installed %2, %3.").arg(
		release.version,
		AppVersion(),
		(status.phase == Phase::Found) ? u"newer"_q : u"not newer"_q));
	_status = status;

	_timer.callOnce(kPeriod);
}

void Checker::download() {
	const auto release = _status.current().release;
	// The decoy promises the process opens no sockets, and this is the one
	// request that would break that promise in the most traceable way there
	// is. The check is repeated here rather than trusted from the caller.
	if (Decoy::Active() || _reply || release.url.isEmpty()) {
		return;
	} else if (release.sha256.size() != 64) {
		// An installer that cannot be checked is not installed. Publishing a
		// release without the digest is a mistake worth failing loudly on.
		LOG(("NovaGram update: the release carries no usable digest."));
		fail(Failure::Rejected);
		return;
	}
	_retry.cancel();
	_cancelling = false;
	if (_partialVersion != release.version) {
		_partial.clear();
		_partialVersion = release.version;
	}
	LOG(("NovaGram update: downloading %1%2.").arg(
		release.url,
		(_partial.isEmpty()
			? QString()
			: u" from byte "_q + QString::number(_partial.size()))));
	auto status = _status.current();
	status.phase = Phase::Downloading;
	status.failure = Failure::None;
	if (_partial.isEmpty()) {
		// A continuation keeps the percentage it had reached. With a pause
		// before it the bar would otherwise sit at zero for half a minute and
		// then jump back to seventy, which reads as a restart and is not one.
		status.progress = 0;
	}
	_status = status;

	const auto url = QUrl(release.url);
	const auto proxy = Doh::ProxyFor(url);
	if (proxy.capabilities() & QNetworkProxy::HostNameLookupCapability) {
		// The proxy resolves for us, so nothing is looked up here at all and
		// the request goes out exactly as it did before this module existed -
		// including Qt following the redirect GitHub sends to the asset host.
		startDownload(url, QString(), 0);
		return;
	}
	const auto host = url.host();
	Doh::Resolve(host, Doh::RecordType::A, [=](Doh::Answer answer) {
		if (answer.values.empty()) {
			LOG(("NovaGram update: could not resolve %1 over secure DNS.").arg(
				host));
			fail(Failure::Network);
			return;
		}
		auto target = url;
		target.setHost(answer.values.front());
		startDownload(target, host, 0);
	});
}

// Issues the request itself. `verifyName` empty means the proxy resolved and
// Qt may follow redirects on its own; otherwise the address was resolved here
// and every hop has to be resolved here too - letting Qt follow a Location by
// name would put the system resolver back into the middle of the one download
// this fork promises to keep out of it.
void Checker::startDownload(QUrl target, QString verifyName, int hop) {
	if (hop > 5) {
		LOG(("NovaGram update: too many redirects while downloading."));
		fail(Failure::Network);
		return;
	}
	applyProxy(target);
	auto request = QNetworkRequest(target);
	if (verifyName.isEmpty()) {
		Prepare(request, kDownloadTimeout);
	} else {
		PrepareManual(request, verifyName, kDownloadTimeout);
	}
	// Ask for the rest of what an earlier attempt managed to fetch. The asset
	// host answers 206 and sends only the tail; a host that cannot do that
	// answers 200 with the whole file and the partial is simply replaced, which
	// is exactly the old behaviour. Either way the digest at the end is what
	// decides whether the result is trustworthy.
	if (!_partial.isEmpty()) {
		request.setRawHeader(
			"Range",
			"bytes=" + QByteArray::number(qint64(_partial.size())) + "-");
	}
	_resumeBase = _partial.size();
	_resumeDecided = false;
	const auto reply = _manager.get(request);
	_reply = reply;

	// Read as it arrives instead of once at the end. This is the whole reason
	// resuming did not work: a transfer timeout makes Qt call abort(), abort()
	// closes the reply, and closing a QIODevice throws its read buffer away -
	// so the bytes that the progress bar had just counted to seventy-odd per
	// cent were gone by the time anyone asked for them, and every attempt
	// started from nothing.
	QObject::connect(reply, &QNetworkReply::readyRead, [=] {
		if (!_resumeDecided) {
			_resumeDecided = true;
			const auto code = reply->attribute(
				QNetworkRequest::HttpStatusCodeAttribute).toInt();
			if (code != 206) {
				// The Range was ignored and this is the whole file again, so
				// what was held is not a prefix of what is coming.
				_partial.clear();
				_resumeBase = 0;
			}
		}
		_partial.append(reply->readAll());
		if (_partial.size() > kMaxInstallerSize) {
			_partial.clear();
			_cancelling = false;
			reply->abort();
		}
	});
	QObject::connect(reply, &QNetworkReply::downloadProgress, [=](
			qint64 received,
			qint64 total) {
		// `total` is only the tail when the server honoured the Range, so the
		// whole is what is already held plus what is still coming.
		const auto full = (total > 0) ? (_resumeBase + total) : 0;
		if (full <= 0) {
			return;
		}
		auto status = _status.current();
		status.progress = int((qint64(_partial.size()) * 100) / full);
		_status = status;
	});
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		_reply = nullptr;
		reply->deleteLater();
		const auto redirect = reply->attribute(
			QNetworkRequest::RedirectionTargetAttribute).toUrl();
		if (!verifyName.isEmpty() && !redirect.isEmpty()) {
			// Nothing of the body was the file, so the partial is untouched
			// and the next hop simply asks for the same range again.
			const auto next = target.resolved(redirect);
			if (next.scheme() != u"https"_q) {
				LOG(("NovaGram update: refusing a non-https hop."));
				fail(Failure::Network);
				return;
			}
			const auto nextHost = next.host();
			Doh::Resolve(nextHost, Doh::RecordType::A, [=](
					Doh::Answer answer) {
				if (answer.values.empty()) {
					LOG(("NovaGram update: could not resolve %1.").arg(nextHost));
					fail(Failure::Network);
					return;
				}
				auto resolved = next;
				resolved.setHost(answer.values.front());
				startDownload(resolved, nextHost, hop + 1);
			});
			return;
		}
		_partial.append(reply->readAll());

		if (_cancelling) {
			// Asked for, not broken. The release stays known, so the bar and
			// the settings row offer to download it again - and what was
			// already fetched is kept, so pressing it asks for the rest.
			// Only Downloading is restored: stop() aborts the same way and has
			// already put the phase back to Idle, and undoing that here would
			// bring back a bar the user has just switched off.
			_cancelling = false;
			if (_status.current().phase == Phase::Downloading) {
				auto status = _status.current();
				status.phase = Phase::Found;
				status.progress = 0;
				_status = status;
			}
			return;
		}
		if (reply->error() != QNetworkReply::NoError) {
			const auto code = reply->attribute(
				QNetworkRequest::HttpStatusCodeAttribute).toInt();
			if (code == 403 || code == 429) {
				// The asset host refusing outright is not a broken transfer,
				// and continuing forty times would only convince it further.
				LOG(("NovaGram update: the download was refused with %1."
					).arg(code));
				_attempts = 0;
				fail(Failure::RateLimited);
				return;
			}
			const auto gained = (_partial.size() > _resumeBase);
			LOG(("NovaGram update: download stopped at %1 bytes, %2 (%3)%4.").arg(
				QString::number(_partial.size()),
				reply->errorString(),
				QString::number(int(reply->error())),
				(gained ? u", continuing"_q : QString())));
			// An attempt that moved forward is worth repeating: on a connection
			// that breaks every few megabytes the file only ever arrives in
			// pieces, and asking the user to press the button twenty times is
			// not offering the feature at all. An attempt that gained nothing
			// is a real failure and is reported as one.
			if (gained && (++_attempts < kMaxDownloadAttempts)) {
				_retry.callOnce(std::min(
					kDownloadRetryStep * _attempts,
					kDownloadRetryMax));
			} else {
				_attempts = 0;
				fail(Failure::Network);
			}
			return;
		}
		_attempts = 0;
		finishDownload(_partial);
	});
}

void Checker::cancel() {
	if (_status.current().phase != Phase::Downloading) {
		return;
	}
	// A continuation may be waiting on the backoff rather than on the wire, and
	// pressing "stop" while nothing is in flight has to stop that too -
	// otherwise the button appeared to work and the download resumed by itself
	// a few seconds later.
	_retry.cancel();
	if (_reply) {
		_cancelling = true;
		_reply->abort();
		return;
	}
	_attempts = 0;
	auto status = _status.current();
	status.phase = Phase::Found;
	status.progress = 0;
	_status = status;
}

void Checker::finishDownload(const QByteArray &body) {
	const auto release = _status.current().release;
	const auto digest = QCryptographicHash::hash(
		body,
		QCryptographicHash::Sha256).toHex();
	if (body.isEmpty() || QString::fromLatin1(digest) != release.sha256) {
		// Whatever is held is not the release it claims to be - a truncated
		// tail, a mixed-up resume, a changed file. Keeping it would make every
		// later attempt continue from the same wrong bytes and fail the same
		// way for ever, so the next one starts clean.
		LOG(("NovaGram update: digest mismatch on %1 bytes, starting over."
			).arg(QString::number(body.size())));
		_partial.clear();
		fail(Failure::Rejected);
		return;
	}
	QDir().mkpath(FolderPath());
	const auto path = FolderPath()
		+ u"NovaGramSetup-"_q
		+ release.version
		+ u".exe"_q;
	// Installers of releases that were downloaded and never installed, or
	// installed long ago, are tens of megabytes each and nothing else ever
	// removed them - not even the emergency wipe, which only sweeps tdata.
	// Cleared here rather than after installing: at that moment the file is
	// the running process.
	for (const auto &entry : QDir(FolderPath()).entryInfoList(QDir::Files)) {
		if (entry.absoluteFilePath() != QFileInfo(path).absoluteFilePath()) {
			QFile::remove(entry.absoluteFilePath());
		}
	}
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)
		|| file.write(body) != body.size()
		|| !file.commit()) {
		fail(Failure::Network);
		return;
	}
	_installer = path;
	if (!verifyInstaller()) {
		return;
	}
	_partial.clear();
	_partialVersion = QString();
	set(Phase::Ready);
}

// Everything the staged file is worth, in one place.
//
// The digest checked above proves only that the bytes are the bytes the same
// JSON answer described - one attacker writes both, so it is a check against
// a broken transfer and against nothing else. This is the check that has to
// hold when the release host, the connection and the answer are all somebody
// else's: the file must carry an Authenticode signature made by one specific
// certificate, and no other file may be executed, whatever it claims to be.
bool Checker::verifyInstaller() {
	const auto verdict = Authenticode::Verify(_installer);
	if (verdict == Authenticode::Verdict::Ok) {
		return true;
	}
	LOG(("NovaGram update: refusing to run %1 - %2.").arg(
		_installer,
		Authenticode::VerdictName(verdict)));
	dropInstaller();
	fail(Failure::Rejected);
	return false;
}

// Nothing of a refused update is kept: not the file, not the half of it that
// an earlier attempt held, and not the promise that something is ready. A
// staged installer left behind would be offered again by the next press of the
// button without ever being fetched.
void Checker::dropInstaller() {
	_installOnQuit = false;
	if (!_installer.isEmpty()) {
		QFile::remove(_installer);
		_installer = QString();
	}
	_partial.clear();
	_partialVersion = QString();
}

void Checker::runInstaller() {
	if (!_installOnQuit) {
		return;
	}
	_installOnQuit = false;
	// Checked once more, here, a moment before the file is executed - and not
	// only when it was downloaded. Between those two moments it sits in a
	// folder that every process running as this user may write to, and Quit()
	// is allowed to take its time: an export or an upload can hold the client
	// open for minutes after the button was pressed. That window is the point
	// of this call.
	if (!verifyInstaller()) {
		return;
	}
	// Silent, because the user already agreed in the application, and with the
	// finish task left enabled so that the installer starts the new build.
	QProcess::startDetached(_installer, {
		u"/VERYSILENT"_q,
		u"/SUPPRESSMSGBOXES"_q,
		u"/NORESTART"_q,
	});
}

void Checker::installAndRestart() {
	if (_installer.isEmpty() || !QFile::exists(_installer)) {
		return;
	} else if (!verifyInstaller()) {
		// The one refusal the user can still be shown: after this the client
		// is on its way out and there is no window left to say it in.
		return;
	}
	// Order matters and it used to be the other way round. Core::Quit() is
	// allowed to refuse: an export, an upload or a download in progress asks
	// the user for confirmation and answers "not now", and the confirmed quit
	// arrives later through a different call. The installer used to be started
	// before that answer was known, so a client that stayed open kept working
	// while its own files were being replaced under it.
	if (!_quitHooked) {
		_quitHooked = true;
		QObject::connect(
			qApp,
			&QCoreApplication::aboutToQuit,
			[=] { runInstaller(); });
	}
	_installOnQuit = true;
	Core::Quit();
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
		// start() first, and then the check. checkNow() is allowed to refuse -
		// the host was asked a minute ago, or asked us not to ask again yet -
		// and something has to arm the cycle, or switching the setting off and
		// on again would quietly end automatic checking until the next launch.
		Instance().start();
		Instance().checkNow();
	} else {
		Instance().stop();
	}
}

void Start() {
	Instance().start();
}

void Stop() {
	Instance().stop();
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

void Cancel() {
	Instance().cancel();
}

void InstallAndRestart() {
	Instance().installAndRestart();
}

bool BarVisible(const Status &status) {
	// The decoy is armed inside the running process, and the chat list it
	// builds afterwards subscribes to this. Without the check here a release
	// found before the emergency PIN was entered would put a bar reading
	// "Update NovaGram" across the bottom of the disguise.
	if (Decoy::Active()) {
		return false;
	}
	switch (status.phase) {
	case Phase::Found:
	case Phase::Downloading:
	case Phase::Ready:
		return true;
	case Phase::Failed:
		// A download that failed leaves the release known, and the bar has to
		// go on offering it. Hiding it was the first behaviour and it was
		// wrong twice over: the user saw the button vanish the moment they
		// pressed it, and the only way back was to find the settings and ask
		// for a check by hand.
		return !status.release.url.isEmpty();
	}
	return false;
}

QString BarText(const Status &status) {
	const auto russian = UseRussianTexts();
	switch (status.phase) {
	case Phase::Downloading:
		return (russian ? u"Загрузка… "_q : u"Downloading… "_q)
			+ QString::number(status.progress)
			+ u"%"_q;
	case Phase::Ready:
		return russian
			? u"Установить и перезапустить"_q
			: u"Install and restart"_q;
	case Phase::Failed:
		// A refused installer is not a hiccup to try again, and the bar is the
		// only place some users ever look. It says what happened; the settings
		// section says why.
		return (status.failure == Failure::Rejected)
			? (russian ? u"Обновление отклонено"_q : u"Update rejected"_q)
			: (russian ? u"Попробовать снова"_q : u"Try again"_q);
	}
	return russian ? u"Обновить NovaGram"_q : u"Update NovaGram"_q;
}

QString ActionText(const Status &status) {
	const auto russian = UseRussianTexts();
	switch (status.phase) {
	case Update::Phase::Failed:
		return russian ? u"Попробовать снова"_q : u"Try again"_q;
	case Update::Phase::Downloading:
		return russian ? u"Остановить загрузку"_q : u"Stop the download"_q;
	case Update::Phase::Ready:
		return russian
			? u"Установить и перезапустить"_q
			: u"Install and restart"_q;
	}
	return russian ? u"Скачать обновление"_q : u"Download the update"_q;
}

void ActOnBar() {
	const auto status = Current();
	switch (status.phase) {
	case Phase::Found: Download(); return;
	case Phase::Downloading: Cancel(); return;
	case Phase::Ready: InstallAndRestart(); return;
	case Phase::Failed:
		// Only reachable while the bar is shown, which is only while there is
		// a release to try again.
		if (!status.release.url.isEmpty()) {
			Download();
		}
		return;
	}
}

} // namespace NovaGram::Update
