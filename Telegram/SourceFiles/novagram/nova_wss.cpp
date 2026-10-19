/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_wss.h"

#include "base/random.h"
#include "base/timer.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "logs.h"
#include "novagram/nova_decoy.h"
#include "novagram/nova_doh.h"
#include "novagram/nova_pin.h"
#include "novagram/nova_terminator.h"
#include "settings.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>

namespace NovaGram::Wss {
namespace {

constexpr auto kEnabledKey = "novagram_wss"_cs;
constexpr auto kListUrl = "https://raw.githubusercontent.com"
	"/confeden/nova_updates/refs/heads/main/NovaGram/relay.json";
constexpr auto kPeriod = crl::time(24 * 60 * 60 * 1000);
constexpr auto kFirstDelay = crl::time(45 * 1000);
constexpr auto kTimeout = 30 * 1000;
constexpr auto kMaxBody = 128 * 1024;
constexpr auto kMaxEntries = 96;

// On by default: it is what makes Telegram reachable at all on a line that
// drops the plain TCP, and where plain TCP works the relay is Telegram's own.
// The mirrors are only tried after it, and a failed relay is remembered.
std::atomic<int> Mirrored = -1;

// One way in. `host` and `path` may carry {dc} and {media}: the number of the
// data centre, and "-1" for the media variant of the official relay.
struct Entry {
	QString host;
	QString path = u"/apiws"_q;
	std::array<QString, 6> ips; // By data centre, 1..5; empty: resolved by DoH.
};

struct RouteSet {
	int version = 0;
	Entry official;
	std::vector<Entry> own;     // Tried in order, before the mirrors.
	std::vector<Entry> fronts;  // Rotated, so that not everybody starts at the top.
};

[[nodiscard]] Entry Front(const char *host) {
	auto result = Entry();
	result.host = u"kws{dc}."_q + QLatin1String(host);
	return result;
}

// What the client carries inside, the same as the first published
// NovaGram/relay.json. Used until a newer list has been read.
[[nodiscard]] std::shared_ptr<const RouteSet> BuiltIn() {
	auto result = std::make_shared<RouteSet>();
	result->version = 1;
	result->official.host = u"kws{dc}{media}.web.telegram.org"_q;
	// The name alone resolves to an address that is not served the same way;
	// a wrong one answers with a redirect or with silence.
	result->official.ips[1] = u"149.154.174.100"_q;
	result->official.ips[2] = u"149.154.167.220"_q;
	result->official.ips[3] = u"149.154.174.100"_q;
	result->official.ips[4] = u"149.154.167.220"_q;
	result->official.ips[5] = u"149.154.170.100"_q;
	for (const auto host : {
		"pclead.co.uk",
		"offshor.co.uk",
		"cakeisalie.co.uk",
		"noskomnadzor.co.uk",
		"lovetrue.co.uk",
		"sorokdva.co.uk",
		"pyatdesyatdva.co.uk",
		"kartoshka.co.uk",
		"sorokodin.co.uk",
		"pyatdesyatodin.co.uk",
		"notelega.co.uk",
		"ebally.co.uk",
		"nebally.co.uk",
		"havegreatday.co.uk",
		"pomogite.co.uk",
		"fixtelega.co.uk",
		"sadnews.co.uk",
		"onedaychamp.co.uk",
		"stopblocking.co.uk",
		"nothingthere.co.uk",
	}) {
		result->fronts.push_back(Front(host));
	}
	return result;
}

std::mutex SetMutex;
std::shared_ptr<const RouteSet> Current;

[[nodiscard]] std::shared_ptr<const RouteSet> CurrentSet() {
	const auto lock = std::lock_guard(SetMutex);
	if (!Current) {
		Current = BuiltIn();
	}
	return Current;
}

void Adopt(std::shared_ptr<const RouteSet> set) {
	const auto lock = std::lock_guard(SetMutex);
	Current = std::move(set);
}

[[nodiscard]] bool GoodHost(const QString &host) {
	static const auto pattern = QRegularExpression(
		u"^[a-z0-9{}][a-z0-9.{}-]{0,120}$"_q);
	return pattern.match(host).hasMatch();
}

[[nodiscard]] bool GoodPath(const QString &path) {
	static const auto pattern = QRegularExpression(
		u"^/[A-Za-z0-9/_.~?=&%{}-]{0,120}$"_q);
	return pattern.match(path).hasMatch();
}

[[nodiscard]] bool GoodIp(const QString &ip) {
	static const auto pattern = QRegularExpression(
		u"^\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}$"_q);
	return ip.isEmpty() || pattern.match(ip).hasMatch();
}

[[nodiscard]] std::optional<Entry> ParseEntry(const QJsonValue &value) {
	if (!value.isObject()) {
		return std::nullopt;
	}
	const auto object = value.toObject();
	auto result = Entry();
	result.host = object.value(u"host"_q).toString().toLower();
	const auto path = object.value(u"path"_q).toString();
	if (!path.isEmpty()) {
		result.path = path;
	}
	if (!GoodHost(result.host) || !GoodPath(result.path)) {
		return std::nullopt;
	}
	const auto ips = object.value(u"ips"_q).toObject();
	for (auto dc = 1; dc != 6; ++dc) {
		const auto ip = ips.value(QString::number(dc)).toString();
		if (!GoodIp(ip)) {
			return std::nullopt;
		}
		result.ips[dc] = ip;
	}
	return result;
}

[[nodiscard]] std::shared_ptr<const RouteSet> Parse(const QByteArray &bytes) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(bytes, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return nullptr;
	}
	const auto root = document.object();
	auto result = std::make_shared<RouteSet>();
	result->version = root.value(u"version"_q).toInt();
	if (result->version <= 0) {
		return nullptr;
	}
	const auto official = ParseEntry(root.value(u"official"_q));
	if (!official) {
		return nullptr;
	}
	result->official = *official;
	for (const auto &item : root.value(u"own"_q).toArray()) {
		if (const auto entry = ParseEntry(item)) {
			result->own.push_back(*entry);
		}
	}
	for (const auto &item : root.value(u"fronts"_q).toArray()) {
		if (const auto entry = ParseEntry(item)) {
			result->fronts.push_back(*entry);
		}
	}
	if ((result->own.size() + result->fronts.size()) > kMaxEntries) {
		result->fronts.resize(std::max<int>(
			0,
			kMaxEntries - int(result->own.size())));
	}
	return result;
}

// Where the walk along the own relays and the mirrors starts, per data centre
// and kind. Static storage: every slot starts at zero, which Attempt reads as
// the first one.
constexpr auto kSlots = 12;
std::atomic<int> WalkStart[kSlots];

// Until when the official relay is skipped, per ingress address.
constexpr auto kRelayBlockedFor = crl::time(10 * 60 * 1000);
std::atomic<crl::time> RelayBlockedUntil[3];

[[nodiscard]] int AbsDc(int protocolDcId) {
	return (protocolDcId < 0) ? -protocolDcId : protocolDcId;
}

[[nodiscard]] int SlotFor(int protocolDcId) {
	return (AbsDc(protocolDcId) - 1) + ((protocolDcId < 0) ? 6 : 0);
}

[[nodiscard]] int RelayGroup(int protocolDcId) {
	switch (AbsDc(protocolDcId)) {
	case 1: case 3: return 0;
	case 2: case 4: return 1;
	}
	return 2;
}

[[nodiscard]] bool RelayAvailable(int protocolDcId) {
	return crl::now() >= RelayBlockedUntil[RelayGroup(protocolDcId)].load();
}

// Spreads the mirrors across users: everyone walking the list from its top
// would put the whole load on the first few.
[[nodiscard]] int FrontOffset(int count) {
	static const auto random = base::RandomValue<uint32>();
	return count ? int(random % uint32(count)) : 0;
}

[[nodiscard]] QString Fill(QString text, int protocolDcId) {
	return text
		.replace(u"{dc}"_q, QString::number(AbsDc(protocolDcId)))
		.replace(u"{media}"_q, (protocolDcId < 0) ? u"-1"_q : QString());
}

[[nodiscard]] Route MakeRoute(
		const Entry &entry,
		int protocolDcId,
		int index,
		bool front) {
	auto result = Route{ .front = front, .index = index };
	result.host = Fill(entry.host, protocolDcId);
	result.path = Fill(entry.path, protocolDcId);
	result.ip = entry.ips[AbsDc(protocolDcId)];
	return result;
}

// Index 0 is the official relay, then the own relays, then the mirrors; the
// number is what the ladder remembers, so a new list only has to keep the
// order meaningful, not the numbers.
[[nodiscard]] Route RouteAt(
		const RouteSet &set,
		int protocolDcId,
		int index) {
	if (index <= 0) {
		return MakeRoute(set.official, protocolDcId, 0, false);
	}
	const auto own = int(set.own.size());
	if (index <= own) {
		return MakeRoute(set.own[index - 1], protocolDcId, index, false);
	}
	const auto count = int(set.fronts.size());
	const auto front = (index - 1 - own + FrontOffset(count)) % count;
	return MakeRoute(set.fronts[front], protocolDcId, index, true);
}

// ---- The published list: once a day, only when it changed. -----------------

struct Meta {
	QString etag;
	QString sha256;
	int version = 0;
};

Meta CurrentMeta;
QNetworkAccessManager *Manager = nullptr;
base::Timer *SyncTimer = nullptr;
bool Fetching = false;

[[nodiscard]] QString FolderPath() {
	return cWorkingDir() + u"novagram_wss/"_q;
}

[[nodiscard]] QString ListPath() {
	return FolderPath() + u"relay.json"_q;
}

[[nodiscard]] QString MetaPath() {
	return FolderPath() + u"relay.meta"_q;
}

void SaveMeta() {
	QDir().mkpath(FolderPath());
	auto file = QSaveFile(MetaPath());
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	file.write(QJsonDocument(QJsonObject{
		{ u"etag"_q, CurrentMeta.etag },
		{ u"sha256"_q, CurrentMeta.sha256 },
		{ u"version"_q, CurrentMeta.version },
	}).toJson(QJsonDocument::Compact));
	file.commit();
}

void LoadCached() {
	auto meta = QFile(MetaPath());
	if (meta.open(QIODevice::ReadOnly)) {
		const auto object = QJsonDocument::fromJson(meta.readAll()).object();
		CurrentMeta.etag = object.value(u"etag"_q).toString();
		CurrentMeta.sha256 = object.value(u"sha256"_q).toString();
		CurrentMeta.version = object.value(u"version"_q).toInt();
	}
	auto list = QFile(ListPath());
	if (!list.open(QIODevice::ReadOnly) || list.size() > kMaxBody) {
		return;
	}
	if (auto set = Parse(list.readAll())) {
		DEBUG_LOG(("WSS: relay list v%1 read from the disk.").arg(
			set->version));
		Adopt(std::move(set));
	}
}

void ScheduleSync(crl::time delay);

void Fetch(QUrl url, int hop, Fn<void(int, QByteArray, QByteArray)> done) {
	if (hop > 3 || url.scheme() != u"https"_q) {
		done(0, QByteArray(), QByteArray());
		return;
	}
	if (!Manager) {
		Manager = new QNetworkAccessManager();
	}
	const auto host = url.host();
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
		if (hop == 0 && !CurrentMeta.etag.isEmpty()) {
			request.setRawHeader("If-None-Match", CurrentMeta.etag.toLatin1());
		}
		const auto reply = Manager->get(request);
		QObject::connect(reply, &QNetworkReply::finished, [=] {
			reply->deleteLater();
			const auto redirect = reply->attribute(
				QNetworkRequest::RedirectionTargetAttribute).toUrl();
			if (!redirect.isEmpty()) {
				Fetch(url.resolved(redirect), hop + 1, done);
				return;
			}
			const auto status = reply->attribute(
				QNetworkRequest::HttpStatusCodeAttribute).toInt();
			if (reply->error() != QNetworkReply::NoError && status != 304) {
				done(status, QByteArray(), QByteArray());
				return;
			}
			done(
				status,
				(status == 200) ? reply->read(kMaxBody + 1) : QByteArray(),
				reply->rawHeader("ETag"));
		});
	};
	if (proxy.capabilities() & QNetworkProxy::HostNameLookupCapability) {
		issue(url, false);
		return;
	}
	Doh::Resolve(host, Doh::RecordType::A, [=](Doh::Answer answer) {
		if (answer.values.empty()) {
			done(0, QByteArray(), QByteArray());
			return;
		}
		auto target = url;
		target.setHost(answer.values.front());
		issue(target, true);
	});
}

void Sync() {
	if (Decoy::Active() || !Enabled() || Fetching) {
		ScheduleSync(kPeriod);
		return;
	}
	Fetching = true;
	Fetch(QUrl(QString::fromLatin1(kListUrl)), 0, [=](
			int status,
			QByteArray body,
			QByteArray etag) {
		Fetching = false;
		ScheduleSync(kPeriod);
		if (status == 304) {
			return;
		} else if (status != 200 || body.isEmpty() || body.size() > kMaxBody) {
			// Silent by design; the next attempt is tomorrow.
			DEBUG_LOG(("WSS: relay list sync failed, %1.").arg(status));
			return;
		}
		const auto sha = QString::fromLatin1(QCryptographicHash::hash(
			body,
			QCryptographicHash::Sha256).toHex());
		if (sha == CurrentMeta.sha256) {
			CurrentMeta.etag = QString::fromLatin1(etag);
			SaveMeta();
			return;
		}
		auto set = Parse(body);
		if (!set) {
			LOG(("WSS: the published relay list did not parse, kept the old."));
			return;
		}
		// A list that went back in version is somebody's mistake or a replay
		// of an old file; the one in use stays.
		if (set->version < CurrentMeta.version) {
			return;
		}
		QDir().mkpath(FolderPath());
		auto file = QSaveFile(ListPath());
		if (file.open(QIODevice::WriteOnly)) {
			file.write(body);
			file.commit();
		}
		CurrentMeta.etag = QString::fromLatin1(etag);
		CurrentMeta.sha256 = sha;
		CurrentMeta.version = set->version;
		SaveMeta();
		LOG(("WSS: relay list v%1: %2 own, %3 mirrors.").arg(
			QString::number(set->version),
			QString::number(set->own.size()),
			QString::number(set->fronts.size())));
		Adopt(std::move(set));
	});
}

void ScheduleSync(crl::time delay) {
	if (!SyncTimer) {
		SyncTimer = new base::Timer([] { Sync(); });
	}
	SyncTimer->callOnce(delay);
}

} // namespace

bool Enabled() {
	const auto result = Core::App().settings().readPref<bool>(
		kEnabledKey,
		true);
	Mirrored.store(result ? 1 : 0, std::memory_order_relaxed);
	return result;
}

void SetEnabled(bool enabled) {
	Core::App().settings().writePref<bool>(kEnabledKey, enabled);
	Core::App().saveSettingsDelayed();
	Mirrored.store(enabled ? 1 : 0, std::memory_order_relaxed);
	Terminator::Refresh();
}

bool EnabledForSocket() {
	return Mirrored.load(std::memory_order_relaxed) == 1;
}

void Start() {
	if (Decoy::Active()) {
		return;
	}
	LoadCached();
	ScheduleSync(kFirstDelay);
	Terminator::Start();
}

QString Title() {
	return UseRussianTexts()
		? u"Подключаться через WebSocket (WSS)"_q
		: u"Connect over WebSocket (WSS)"_q;
}

QString About() {
	return UseRussianTexts()
		? u"Telegram идёт внутри обычного HTTPS-соединения к реле "
			"kwsN.web.telegram.org — тому, что использует web.telegram.org. "
			"Если реле недоступно, пробуются запасные ретрансляторы. Это "
			"помогает там, где прямое TCP-подключение к Telegram блокируется. "
			"Содержимое защищено ключами вашего аккаунта, как и по TCP; "
			"реле и ретрансляторы видят ваш адрес, номер дата-центра и объём "
			"трафика. Список ретрансляторов раз в сутки сверяется с "
			"опубликованным и скачивается только если изменился. Действует на "
			"новые соединения."_q
		: u"Telegram travels inside an ordinary HTTPS connection to the "
			"kwsN.web.telegram.org relay - the one web.telegram.org uses. If "
			"the relay is unreachable, fallback relays are tried. It helps "
			"where the direct TCP connection to Telegram is blocked. The "
			"content is protected by your account's own keys, as over TCP; "
			"the relays see your address, the data centre number and how much "
			"you send. The relay list is checked against the published one "
			"once a day and downloaded only when it changed. Applies to new "
			"connections."_q;
}

bool Supports(int protocolDcId) {
	const auto dc = AbsDc(protocolDcId);
	return (dc >= 1) && (dc <= 5);
}

std::vector<Route> Attempt(int protocolDcId, int count) {
	auto result = std::vector<Route>();
	if (!Supports(protocolDcId)) {
		return result;
	}
	const auto set = CurrentSet();
	// The official relay first, unless it was found blocked a moment ago.
	// Several data centres share one ingress address, so one failure is
	// known to all of them, and a connection does not spend its first 3.5
	// seconds on a door that is known to be shut.
	if (RelayAvailable(protocolDcId)) {
		result.push_back(RouteAt(*set, protocolDcId, 0));
	}
	const auto others = int(set->own.size() + set->fronts.size());
	const auto start = std::max(WalkStart[SlotFor(protocolDcId)].load(), 1);
	for (auto i = 0; int(result.size()) < count && i != others; ++i) {
		result.push_back(RouteAt(
			*set,
			protocolDcId,
			1 + (start - 1 + i) % others));
	}
	return result;
}

void NoteWorked(int protocolDcId, const Route &route) {
	if (!Supports(protocolDcId)) {
		return;
	} else if (route.index == 0) {
		RelayBlockedUntil[RelayGroup(protocolDcId)].store(0);
	} else {
		WalkStart[SlotFor(protocolDcId)].store(route.index);
	}
}

void NoteFailed(int protocolDcId, const Route &route) {
	if (!Supports(protocolDcId)) {
		return;
	} else if (route.index == 0) {
		RelayBlockedUntil[RelayGroup(protocolDcId)].store(
			crl::now() + kRelayBlockedFor);
		return;
	}
	// The next one, wrapping and never onto the official relay.
	const auto set = CurrentSet();
	const auto others = int(set->own.size() + set->fronts.size());
	if (!others) {
		return;
	}
	auto &slot = WalkStart[SlotFor(protocolDcId)];
	auto expected = route.index;
	slot.compare_exchange_strong(
		expected,
		1 + (route.index % others));
}

} // namespace NovaGram::Wss
