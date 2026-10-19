/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_forbidden.h"

#include "base/timer.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_peer.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "logs.h"
#include "main/main_session.h"
#include "novagram/nova_decoy.h"
#include "novagram/nova_doh.h"
#include "novagram/nova_pin.h"
#include "settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "styles/style_chat.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace NovaGram::Forbidden {
namespace {

constexpr auto kEnabledKey = "novagram_forbidden_check"_cs;
constexpr auto kListUrl = "https://raw.githubusercontent.com"
	"/confeden/nova_updates/refs/heads/main/forbidden_list"
	"/forbidden_list.json";
constexpr auto kPeriod = crl::time(24 * 60 * 60 * 1000);
constexpr auto kFirstDelay = crl::time(20 * 1000);
constexpr auto kTimeout = 60 * 1000;
constexpr auto kMaxBody = 32 * 1024 * 1024;
constexpr auto kIndexHeader = "NVFL2";
constexpr auto kMinTitleLength = 12;
constexpr auto kScanLimit = 8192;

// The list keys in the published file, in the order of `List`.
const char *const kListKeys[] = {
	"иноагенты",
	"террористы",
	"экстремисты",
	"нежелательные",
};

enum class Kind : char {
	Id = 'i',
	Username = 'u',
	Url = 'l',
	Name = 'n',
};

// One probe per kind, all O(1). Built once per load, then only read.
struct Index {
	QHash<quint64, uchar> ids;
	QHash<QString, uchar> usernames;
	QHash<QString, uchar> urls;
	QHash<QString, uchar> names;
	// Normalised title of an extremist material -> the title as listed.
	QHash<QString, QString> titles;

	[[nodiscard]] bool empty() const {
		return ids.isEmpty()
			&& usernames.isEmpty()
			&& urls.isEmpty()
			&& names.isEmpty()
			&& titles.isEmpty();
	}
};

struct Meta {
	QString etag;
	QString sha256;
	qint64 checked = 0;
};

std::optional<bool> GlobalEnabled;
std::shared_ptr<const Index> Current;
uint32 GlobalGeneration = 1;
Meta GlobalMeta;
bool Started = false;
bool Loading = false;
bool Fetching = false;
bool Pass = false;
base::Timer *SyncTimer = nullptr;
QNetworkAccessManager *Manager = nullptr;

[[nodiscard]] QString FolderPath() {
	return cWorkingDir() + u"novagram_forbidden/"_q;
}

[[nodiscard]] QString IndexPath() {
	return FolderPath() + u"index.txt"_q;
}

[[nodiscard]] QString MetaPath() {
	return FolderPath() + u"meta.json"_q;
}

[[nodiscard]] QString BundledPath() {
	return QCoreApplication::applicationDirPath()
		+ u"/novagram_forbidden.json"_q;
}

[[nodiscard]] QString NormalizeName(const QString &text) {
	auto result = QString();
	result.reserve(text.size());
	auto space = false;
	for (const auto ch : text.toLower()) {
		if (ch.isLetterOrNumber()) {
			result.append((ch == QChar(0x0451)) ? QChar(0x0435) : ch);
			space = false;
		} else if (!space && !result.isEmpty()) {
			result.append(' ');
			space = true;
		}
	}
	return result.trimmed();
}

// The title rule, identical on Android: lower case, "ё" as "е", quotes of
// every kind as a space, runs of whitespace folded into one, trimmed.
[[nodiscard]] QString NormalizeTitle(const QString &text) {
	static const auto quotes = QString::fromUtf16(
		u"\u00AB\u00BB\"\u201C\u201D\u201E'`\u2018\u2019");
	auto result = QString();
	result.reserve(text.size());
	auto space = false;
	for (auto ch : text.toLower()) {
		if (ch == QChar(0x0451)) {
			ch = QChar(0x0435);
		}
		if (ch.isSpace() || quotes.contains(ch)) {
			if (!space && !result.isEmpty()) {
				result.append(' ');
				space = true;
			}
		} else {
			result.append(ch);
			space = false;
		}
	}
	return result.trimmed();
}

struct ParsedLink {
	QString username;
	QString key;
};

[[nodiscard]] ParsedLink ParseLink(const QString &raw) {
	auto s = raw.trimmed();
	static const auto trailing = QString::fromUtf16(
		u".,;:!?)]>\"'»");
	while (!s.isEmpty() && trailing.contains(s.back())) {
		s.chop(1);
	}
	s = s.toLower();
	if (s.startsWith(u"tg://"_q) || s.startsWith(u"tg:"_q)) {
		const auto at = s.indexOf(u"domain="_q);
		if (at >= 0) {
			auto name = s.mid(at + 7);
			const auto end = name.indexOf('&');
			if (end >= 0) {
				name.truncate(end);
			}
			return { .username = name };
		}
		return {};
	}
	for (const auto &scheme : { u"https://"_q, u"http://"_q, u"//"_q }) {
		if (s.startsWith(scheme)) {
			s = s.mid(scheme.size());
			break;
		}
	}
	const auto cut = s.indexOf(QRegularExpression(u"[/?#]"_q));
	auto host = (cut < 0) ? s : s.left(cut);
	auto rest = (cut < 0) ? QString() : s.mid(cut);
	if (const auto at = host.lastIndexOf('@'); at >= 0) {
		host = host.mid(at + 1);
	}
	if (const auto colon = host.indexOf(':'); colon >= 0) {
		host.truncate(colon);
	}
	for (const auto &prefix : { u"www."_q, u"m."_q, u"mobile."_q }) {
		if (host.startsWith(prefix)) {
			host = host.mid(prefix.size());
		}
	}
	if (host == u"twitter.com"_q) {
		host = u"x.com"_q;
	} else if (host == u"telegram.me"_q || host == u"telegram.dog"_q) {
		host = u"t.me"_q;
	}
	if (!host.contains('.') || host.endsWith('.')) {
		return {};
	}
	if (const auto hash = rest.indexOf('#'); hash >= 0) {
		rest.truncate(hash);
	}
	auto path = rest;
	auto query = QString();
	if (const auto q = rest.indexOf('?'); q >= 0) {
		path = rest.left(q);
		query = rest.mid(q + 1);
	}
	while (path.endsWith('/')) {
		path.chop(1);
	}
	if (host == u"t.me"_q) {
		const auto segments = path.split('/', Qt::SkipEmptyParts);
		auto i = 0;
		if (!segments.isEmpty() && segments[0] == u"s"_q) {
			i = 1;
		}
		if (i >= segments.size()) {
			return {};
		}
		const auto &first = segments[i];
		if (first.startsWith('+') || first == u"joinchat"_q) {
			return { .key = host + path };
		}
		static const auto valid = QRegularExpression(
			u"^[a-z0-9_]{3,}$"_q);
		if (valid.match(first).hasMatch()) {
			return { .username = first };
		}
		return {};
	}
	auto key = host + path;
	if (path.endsWith(u"/profile.php"_q)) {
		for (const auto &pair : query.split('&')) {
			if (pair.startsWith(u"id="_q)) {
				key += u"?"_q + pair;
			}
		}
	}
	return { .key = key };
}

void AddLink(Index &index, uchar list, const QString &link) {
	auto rest = link;
	static const auto idPattern = QRegularExpression(
		u"\\bID\\s*:?\\s*([-–—−]?)\\s*(\\d{5,})"_q,
		QRegularExpression::CaseInsensitiveOption);
	const auto match = idPattern.match(rest);
	if (match.hasMatch()) {
		const auto negative = !match.captured(1).isEmpty();
		const auto digits = match.captured(2);
		if (negative && digits.startsWith(u"100"_q) && digits.size() >= 12) {
			index.ids.insert(
				peerFromChannel(ChannelId(digits.mid(3).toULongLong())).value,
				list);
		} else if (negative) {
			index.ids.insert(
				peerFromChat(ChatId(digits.toULongLong())).value,
				list);
		} else {
			index.ids.insert(
				peerFromUser(UserId(digits.toULongLong())).value,
				list);
		}
		rest.remove(match.capturedStart(), match.capturedLength());
	}
	for (const auto &token : rest.split(
			QRegularExpression(u"[\\s,;]+"_q),
			Qt::SkipEmptyParts)) {
		const auto parsed = ParseLink(token);
		if (!parsed.username.isEmpty()) {
			index.usernames.insert(parsed.username, list);
		} else if (!parsed.key.isEmpty()) {
			index.urls.insert(parsed.key, list);
		}
	}
}

// The quoted name of an organisation: «...» or "...". A plain person's name
// is never taken - it would collide with ordinary accounts.
void AddNames(Index &index, uchar list, const QString &name) {
	if (name.startsWith(u"Дата принятия"_q)) {
		return;
	}
	static const auto quoted = QRegularExpression(
		u"[«\"]([^»\"]{6,})[»\"]"_q);
	auto it = quoted.globalMatch(name);
	while (it.hasNext()) {
		const auto normalized = NormalizeName(it.next().captured(1));
		if (normalized.size() >= 10) {
			index.names.insert(normalized, list);
		}
	}
}

void AddTitle(Index &index, const QString &name) {
	const auto normalized = NormalizeTitle(name);
	if (normalized.size() < kMinTitleLength) {
		return;
	}
	index.titles.insert(normalized, name.simplified());
}

[[nodiscard]] std::shared_ptr<Index> BuildFromJson(const QByteArray &body) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(body, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		return nullptr;
	}
	const auto lists = document.object().value(u"lists"_q).toObject();
	if (lists.isEmpty()) {
		return nullptr;
	}
	auto result = std::make_shared<Index>();
	for (auto i = 0; i != 4; ++i) {
		const auto items = lists.value(QString::fromUtf8(kListKeys[i]))
			.toObject().value(u"items"_q).toArray();
		for (const auto &item : items) {
			const auto object = item.toObject();
			if (i == int(List::Agent)) {
				for (const auto &link : object.value(u"links"_q).toArray()) {
					AddLink(*result, uchar(i), link.toString());
				}
			} else {
				AddNames(*result, uchar(i), object.value(u"name"_q).toString());
				const auto shortName = NormalizeName(
					object.value(u"short_name"_q).toString());
				if (shortName.size() >= 6) {
					result->names.insert(shortName, uchar(i));
				}
			}
		}
	}
	const auto materials = lists.value(u"материалы"_q)
		.toObject().value(u"items"_q).toArray();
	for (const auto &item : materials) {
		AddTitle(*result, item.toObject().value(u"name"_q).toString());
	}
	return result->empty() ? nullptr : result;
}

[[nodiscard]] std::shared_ptr<Index> LoadIndexFile() {
	auto file = QFile(IndexPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return nullptr;
	}
	if (file.readLine().trimmed() != QByteArray(kIndexHeader)) {
		return nullptr;
	}
	auto result = std::make_shared<Index>();
	while (!file.atEnd()) {
		const auto line = QString::fromUtf8(file.readLine()).trimmed();
		const auto parts = line.split('\t');
		if (parts.size() < 3 || parts.size() > 4
			|| parts[0].size() != 1
			|| parts[1].size() != 1) {
			continue;
		}
		const auto list = uchar(parts[0][0].unicode() - '0');
		if (list > uchar(List::Materials)) {
			continue;
		}
		if (parts[1][0] == QChar('t')) {
			if (parts.size() == 4) {
				result->titles.insert(parts[2], parts[3]);
			}
			continue;
		} else if (parts.size() != 3) {
			continue;
		}
		switch (parts[1][0].toLatin1()) {
		case 'i': result->ids.insert(parts[2].toULongLong(), list); break;
		case 'u': result->usernames.insert(parts[2], list); break;
		case 'l': result->urls.insert(parts[2], list); break;
		case 'n': result->names.insert(parts[2], list); break;
		}
	}
	return result->empty() ? nullptr : result;
}

// False when the index did not reach the disk: the caller must not record the
// sync as done, or the next start would trust a file that is not there.
[[nodiscard]] bool SaveIndexFile(const Index &index) {
	QDir().mkpath(FolderPath());
	auto file = QSaveFile(IndexPath());
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	file.write(kIndexHeader);
	file.write("\n");
	const auto put = [&](uchar list, char kind, const QString &key) {
		file.write(QByteArray::number(int(list)));
		file.write("\t");
		file.write(QByteArray(1, kind));
		file.write("\t");
		file.write(key.toUtf8());
		file.write("\n");
	};
	for (auto i = index.ids.cbegin(); i != index.ids.cend(); ++i) {
		put(i.value(), 'i', QString::number(i.key()));
	}
	for (auto i = index.usernames.cbegin(); i != index.usernames.cend(); ++i) {
		put(i.value(), 'u', i.key());
	}
	for (auto i = index.urls.cbegin(); i != index.urls.cend(); ++i) {
		put(i.value(), 'l', i.key());
	}
	for (auto i = index.names.cbegin(); i != index.names.cend(); ++i) {
		put(i.value(), 'n', i.key());
	}
	for (auto i = index.titles.cbegin(); i != index.titles.cend(); ++i) {
		auto original = i.value();
		original.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');
		file.write(QByteArray::number(int(List::Materials)));
		file.write("\t");
		file.write("t");
		file.write("\t");
		file.write(i.key().toUtf8());
		file.write("\t");
		file.write(original.toUtf8());
		file.write("\n");
	}
	return file.commit();
}

void LoadMeta() {
	GlobalMeta = Meta();
	auto file = QFile(MetaPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	GlobalMeta.etag = object.value(u"etag"_q).toString();
	GlobalMeta.sha256 = object.value(u"sha256"_q).toString();
	GlobalMeta.checked = object.value(u"checked"_q).toString().toLongLong();
}

void SaveMeta() {
	QDir().mkpath(FolderPath());
	auto file = QSaveFile(MetaPath());
	if (!file.open(QIODevice::WriteOnly)) {
		return;
	}
	file.write(QJsonDocument(QJsonObject{
		{ u"etag"_q, GlobalMeta.etag },
		{ u"sha256"_q, GlobalMeta.sha256 },
		{ u"checked"_q, QString::number(GlobalMeta.checked) },
	}).toJson(QJsonDocument::Compact));
	file.commit();
}

void Adopt(std::shared_ptr<const Index> index) {
	Current = std::move(index);
	++GlobalGeneration;
}

[[nodiscard]] QString ListName(List list) {
	const auto russian = UseRussianTexts();
	switch (list) {
	case List::Agent:
		return russian ? u"иноагент"_q : u"foreign agent"_q;
	case List::Terrorist:
		return russian
			? u"террористическая организация"_q
			: u"terrorist organisation"_q;
	case List::Extremist:
		return russian
			? u"экстремистская организация"_q
			: u"extremist organisation"_q;
	case List::Undesirable:
		return russian
			? u"нежелательная организация"_q
			: u"undesirable organisation"_q;
	case List::Materials:
		return russian
			? u"экстремистский материал"_q
			: u"extremist material"_q;
	}
	Unexpected("List in NovaGram::Forbidden::ListName.");
}

[[nodiscard]] std::optional<List> Probe(
		const QHash<QString, uchar> &hash,
		const QString &key) {
	if (key.isEmpty()) {
		return std::nullopt;
	}
	const auto i = hash.constFind(key);
	return (i != hash.cend())
		? std::make_optional(List(i.value()))
		: std::nullopt;
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
		if (hop == 0 && !GlobalMeta.etag.isEmpty() && Current) {
			request.setRawHeader("If-None-Match", GlobalMeta.etag.toLatin1());
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
		GlobalMeta.checked = QDateTime::currentMSecsSinceEpoch();
		ScheduleSync(kPeriod);
		if (status == 304) {
			SaveMeta();
			return;
		} else if (status != 200 || body.isEmpty() || body.size() > kMaxBody) {
			// Silent by design; the next attempt is tomorrow.
			DEBUG_LOG(("NovaGram forbidden list: sync failed, %1.").arg(status));
			SaveMeta();
			return;
		}
		const auto sha = QString::fromLatin1(QCryptographicHash::hash(
			body,
			QCryptographicHash::Sha256).toHex());
		if (sha == GlobalMeta.sha256) {
			GlobalMeta.etag = QString::fromLatin1(etag);
			SaveMeta();
			return;
		}
		crl::async([=, body = std::move(body)] {
			auto index = BuildFromJson(body);
			const auto saved = index ? SaveIndexFile(*index) : false;
			crl::on_main([=, index = std::move(index)] {
				if (!index) {
					SaveMeta();
					return;
				}
				if (saved) {
					GlobalMeta.etag = QString::fromLatin1(etag);
					GlobalMeta.sha256 = sha;
					SaveMeta();
				}
				Adopt(index);
			});
		});
	});
}

void ScheduleSync(crl::time delay) {
	if (!SyncTimer) {
		SyncTimer = new base::Timer([] { Sync(); });
	}
	SyncTimer->callOnce(delay);
}

void BeginLoad() {
	if (Started || Loading || Decoy::Active() || !Enabled()) {
		return;
	}
	Loading = true;
	crl::async([] {
		auto index = LoadIndexFile();
		auto seeded = QString();
		if (!index) {
			auto bundled = QFile(BundledPath());
			if (bundled.open(QIODevice::ReadOnly)) {
				const auto body = bundled.readAll();
				index = BuildFromJson(body);
				if (index && SaveIndexFile(*index)) {
					seeded = QString::fromLatin1(QCryptographicHash::hash(
						body,
						QCryptographicHash::Sha256).toHex());
				}
			}
		}
		crl::on_main([=, index = std::move(index)] {
			Loading = false;
			Started = true;
			LoadMeta();
			if (!seeded.isEmpty()) {
				// The snapshot's own hash, so a first sync that finds the same
				// bytes upstream skips the parse and the rewrite.
				GlobalMeta.sha256 = seeded;
				GlobalMeta.etag = QString();
				SaveMeta();
			}
			if (!index && seeded.isEmpty()) {
				// Nothing could be loaded (a cache of an older format, no
				// snapshot): the next sync must not skip the same bytes.
				GlobalMeta.sha256 = QString();
				GlobalMeta.etag = QString();
			}
			if (index) {
				Adopt(index);
			}
			const auto elapsed = GlobalMeta.checked
				? (QDateTime::currentMSecsSinceEpoch() - GlobalMeta.checked)
				: kPeriod;
			ScheduleSync((elapsed >= kPeriod || elapsed < 0)
				? kFirstDelay
				: std::max(kFirstDelay, kPeriod - crl::time(elapsed)));
		});
	});
}

[[nodiscard]] QString WarnTitle() {
	return UseRussianTexts()
		? u"Запрещённый контент"_q
		: u"Prohibited content"_q;
}

void Warn(
		std::shared_ptr<Ui::Show> show,
		const QString &text,
		Fn<void()> proceed) {
	const auto russian = UseRussianTexts();
	show->show(Ui::MakeConfirmBox({
		.text = text,
		.confirmed = [=](Fn<void()> close) {
			close();
			proceed();
		},
		.confirmText = russian ? u"Продолжить"_q : u"Continue"_q,
		.cancelText = russian ? u"Отмена"_q : u"Cancel"_q,
		.title = WarnTitle(),
	}));
}

[[nodiscard]] QString AuthorText(List list) {
	return UseRussianTexts()
		? (u"Это сообщение написано лицом или организацией из перечня: "_q
			+ ListName(list)
			+ u". Цитирование или пересылка его содержимого может повлечь "
			u"наказание."_q)
		: (u"This message was written by a person or organisation on the "
			u"list: "_q + ListName(list) + u". Quoting or forwarding its "
			u"content may be punishable."_q);
}

// What a text hit says. `forwarded` is the wording of a message that is passed
// on, not one that is typed.
[[nodiscard]] QString TextWarning(const TextHit &hit, bool forwarded) {
	const auto russian = UseRussianTexts();
	if (hit.list == List::Materials) {
		return russian
			? ((forwarded
				? u"В пересылаемом сообщении есть полное название материала "
					u"из федерального списка экстремистских материалов: "_q
				: u"В сообщении есть полное название материала из "
					u"федерального списка экстремистских материалов: "_q)
				+ u"«"_q + hit.link + u"». Распространение такого материала "
					u"может повлечь наказание."_q)
			: ((forwarded
				? u"The forwarded message contains the full title of a "
					u"material from the federal list of extremist materials: "_q
				: u"The message contains the full title of a material from "
					u"the federal list of extremist materials: "_q)
				+ u"\""_q + hit.link + u"\". Sharing it may be punishable."_q);
	}
	return russian
		? ((forwarded
			? u"В пересылаемом сообщении есть ссылка на ресурс из перечня: "_q
			: u"В сообщении есть ссылка на ресурс из перечня: "_q)
			+ ListName(hit.list) + u" ("_q + hit.link
			+ u"). Распространение ссылок на такие ресурсы может "
				u"повлечь наказание."_q)
		: ((forwarded
			? u"The forwarded message contains a link to a resource on the "
				u"list: "_q
			: u"The message contains a link to a resource on the list: "_q)
			+ ListName(hit.list) + u" ("_q + hit.link
			+ u"). Sharing links to such resources may be punishable."_q);
}

} // namespace

bool Enabled() {
	if (Decoy::Active()) {
		return false;
	}
	if (!GlobalEnabled) {
		GlobalEnabled = Core::App().settings().readPref<bool>(
			kEnabledKey,
			true);
	}
	return *GlobalEnabled;
}

void SetEnabled(bool enabled) {
	if (GlobalEnabled && (*GlobalEnabled == enabled)) {
		return;
	}
	GlobalEnabled = enabled;
	Core::App().settings().writePref<bool>(kEnabledKey, enabled);
	Core::App().saveSettingsDelayed();
	++GlobalGeneration;
	if (enabled) {
		BeginLoad();
	}
}

QString Title() {
	return UseRussianTexts()
		? u"Проверка на запрещённый контент"_q
		: u"Prohibited content check"_q;
}

QString About() {
	return UseRussianTexts()
		? u"Помечает кирпичным цветом сообщения авторов из перечней "
			"иноагентов, террористов, экстремистов и нежелательных "
			"организаций и предупреждает перед цитированием, пересылкой и "
			"отправкой ссылки на их ресурс. Перечень лежит на устройстве, "
			"обновляется раз в сутки, наружу ничего не уходит. Ссылки есть "
			"только у иноагентов; у остальных сверяется название канала или "
			"аккаунта."_q
		: u"Tints messages whose author is on the lists of foreign agents, "
			"terrorists, extremists and undesirable organisations, and warns "
			"before quoting, forwarding or sending a link to their resource. "
			"The list is stored on the device and refreshed daily; nothing "
			"is sent out. Only foreign agents have links; for the rest the "
			"channel or account title is compared."_q;
}

void Start() {
	if (Decoy::Active()) {
		return;
	}
	BeginLoad();
}

uint32 Generation() {
	return GlobalGeneration;
}

std::optional<List> ForPeer(not_null<PeerData*> peer) {
	if (!Current || !Enabled()) {
		return std::nullopt;
	}
	const auto &index = *Current;
	if (const auto i = index.ids.constFind(peer->id.value);
		i != index.ids.cend()) {
		return List(i.value());
	}
	if (!index.usernames.isEmpty()) {
		if (const auto hit = Probe(index.usernames, peer->username().toLower())) {
			return hit;
		}
		for (const auto &name : peer->usernames()) {
			if (const auto hit = Probe(index.usernames, name.toLower())) {
				return hit;
			}
		}
	}
	if (!index.names.isEmpty()) {
		return Probe(index.names, NormalizeName(peer->name()));
	}
	return std::nullopt;
}

std::optional<List> ForItem(not_null<HistoryItem*> item) {
	if (!Current || !Enabled()) {
		return std::nullopt;
	}
	if (const auto hit = ForPeer(item->from())) {
		return hit;
	}
	if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
		if (forwarded->originalSender) {
			return ForPeer(not_null(forwarded->originalSender));
		}
	}
	return std::nullopt;
}

std::optional<TextHit> ForText(const QString &text) {
	if (!Current || !Enabled() || text.size() < 6) {
		return std::nullopt;
	}
	const auto &index = *Current;
	if (!index.urls.isEmpty() || !index.usernames.isEmpty()) {
		static const auto pattern = QRegularExpression(
			u"(?:(?:https?|tg)://)?(?:[a-z0-9][a-z0-9\\-]*\\.)+[a-z]{2,}"
			u"(?::\\d+)?(?:[/?#][^\\s<>«»\"]*)?|tg:[^\\s<>\"]+"_q,
			QRegularExpression::CaseInsensitiveOption);
		auto it = pattern.globalMatch(text);
		while (it.hasNext()) {
			const auto link = it.next().captured();
			const auto parsed = ParseLink(link);
			if (const auto hit = Probe(index.usernames, parsed.username)) {
				return TextHit{ *hit, link };
			} else if (const auto hit = Probe(index.urls, parsed.key)) {
				return TextHit{ *hit, link };
			}
		}
	}
	if (!index.titles.isEmpty()) {
		// A title is a substring of the normalised text, not a probe, so only
		// the head of the text is scanned.
		const auto normalized = NormalizeTitle(text.left(kScanLimit));
		if (normalized.size() >= kMinTitleLength) {
			for (auto i = index.titles.cbegin();
				i != index.titles.cend();
				++i) {
				if (normalized.contains(i.key())) {
					return TextHit{ List::Materials, i.value() };
				}
			}
		}
	}
	return std::nullopt;
}

void PaintMark(QPainter &p, const QRect &bubble) {
	// A brick, not a red: 170/52/40 at 45%. Over a light bubble it comes out as
	// a dusty rose that dark text still reads on (about 10:1), over a dark one
	// as a deep maroon under light text (about 11:1). Pure red would shout and
	// would clash with the unread and error colours of the themes.
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(170, 52, 40, 115));
	const auto radius = st::bubbleRadiusLarge;
	p.drawRoundedRect(bubble, radius, radius);
}

void ConfirmQuote(
		std::shared_ptr<Ui::Show> show,
		not_null<HistoryItem*> item,
		Fn<void()> proceed) {
	const auto hit = ForItem(item);
	if (!hit || !show) {
		proceed();
		return;
	}
	Warn(std::move(show), AuthorText(*hit), std::move(proceed));
}

bool ConfirmForward(
		std::shared_ptr<Ui::Show> show,
		not_null<Main::Session*> session,
		const std::vector<FullMsgId> &ids,
		Fn<void()> proceed) {
	if (Pass) {
		Pass = false;
		return false;
	} else if (!show || !Current || !Enabled()) {
		return false;
	}
	for (const auto &id : ids) {
		if (const auto item = session->data().message(id)) {
			if (const auto hit = ForItem(item)) {
				Warn(std::move(show), AuthorText(*hit), std::move(proceed));
				return true;
			} else if (const auto text = ForText(item->originalText().text)) {
				Warn(
					std::move(show),
					TextWarning(*text, true),
					std::move(proceed));
				return true;
			}
		}
	}
	return false;
}

bool ConfirmSend(
		std::shared_ptr<Ui::Show> show,
		not_null<PeerData*> peer,
		const QString &text,
		Fn<void()> proceed) {
	if (Pass) {
		Pass = false;
		return false;
	} else if (!show || peer->isSelf()) {
		return false;
	}
	const auto hit = ForText(text);
	if (!hit) {
		return false;
	}
	Warn(std::move(show), TextWarning(*hit, false), std::move(proceed));
	return true;
}

void PassOnce() {
	Pass = true;
}

} // namespace NovaGram::Forbidden
