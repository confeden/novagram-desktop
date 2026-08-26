/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_branding.h"

#include "core/version.h"
#include "novagram/nova_decoy.h"

namespace NovaGram {
namespace {

// Both halves of the joint release tag. Bump together with the bases.
constexpr auto kReleaseTag = "v7.1.2/12.10.1";
constexpr auto kProjectUrl = "https://github.com/confeden/Novagram";

// The base application's own name, i.e. the value ::AppName held before the
// fork renamed it. The decoy wears it so that every label built from the name
// reads as a stock Telegram Desktop install instead of exposing the fork.
constexpr auto kDecoyAppName = "Telegram Desktop";

} // namespace

QString AppName() {
	// In the decoy the fork has to be invisible, so every visible label built
	// from the name is spelled with the stock one. Data paths and OS-level
	// identifiers do not pass through here and keep the real name, so wearing
	// the disguise costs the decoy no access to its own installed copy.
	return Decoy::Active()
		? QString::fromUtf8(kDecoyAppName)
		: QString::fromUtf8(::AppName.utf8());
}

QString WithAppName(QString text) {
	// Outside the decoy the fork name takes the place of "Telegram" inside an
	// already-translated phrase. Inside it the phrase is left exactly as the
	// language pack wrote it ("Open Telegram", "Quit Telegram") — that is the
	// stock wording already — so no substitution happens.
	return Decoy::Active()
		? text
		: text.replace(u"Telegram"_q, AppName());
}

QString AppVersion() {
	return QString::fromLatin1(AppVersionStr);
}

QString ReleaseTag() {
	return QString::fromLatin1(kReleaseTag);
}

QString ProjectUrl() {
	return QString::fromLatin1(kProjectUrl);
}

QString ReleaseUrl() {
	// The separator is escaped: a tag with a slash in it is a valid tag, but a
	// slash left raw in the path would be read as another path segment.
	const auto tag = ReleaseTag().replace(u"/"_q, u"%2F"_q);
	return ProjectUrl() + u"/releases/tag/"_q + tag;
}

int CompareVersions(const QString &a, const QString &b) {
	const auto left = a.split('.');
	const auto right = b.split('.');
	const auto count = std::max(left.size(), right.size());
	for (auto i = 0; i != count; ++i) {
		const auto part = [](const QStringList &list, int index) {
			return (index < list.size()) ? list[index].toInt() : 0;
		};
		const auto one = part(left, i);
		const auto two = part(right, i);
		if (one != two) {
			return (one < two) ? -1 : 1;
		}
	}
	return 0;
}

} // namespace NovaGram
