/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_filenames.h"

#include "base/random.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "novagram/nova_pin.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFileInfo>

namespace NovaGram {
namespace {

constexpr auto kEnabledKey = "novagram_masked_filenames"_cs;
constexpr auto kSaltKey = "novagram_filename_salt"_cs;
constexpr auto kSaltSize = 32;

// Eight bytes, sixteen characters. Long enough that two different names cannot
// realistically land on the same token, short enough that the result still
// looks like a file name and not like a hash dump.
constexpr auto kTokenBytes = 8;

[[nodiscard]] QByteArray Salt() {
	auto stored = Core::App().settings().readPref<QByteArray>(
		kSaltKey,
		QByteArray());
	if (stored.size() == kSaltSize) {
		return stored;
	}
	// Made once, on the first masked name, and kept: a salt regenerated on
	// every start would give the same file a new name every time it is saved,
	// and the folder would fill up with copies.
	stored = QByteArray(kSaltSize, Qt::Uninitialized);
	base::RandomFill(stored.data(), stored.size());
	Core::App().settings().writePref<QByteArray>(kSaltKey, stored);
	Core::App().saveSettingsDelayed();
	return stored;
}

} // namespace

bool MaskedFileNamesEnabled() {
	return Core::App().settings().readPref<bool>(kEnabledKey, true);
}

void SetMaskedFileNamesEnabled(bool enabled) {
	Core::App().settings().writePref<bool>(kEnabledKey, enabled);
	Core::App().saveSettingsDelayed();
}

QString MaskedFileNamesTitle() {
	return UseRussianTexts()
		? u"Обезличивать имена сохранённых файлов"_q
		: u"Mask the names of saved files"_q;
}

QString MaskLocalFileName(const QString &name) {
	if (!MaskedFileNamesEnabled() || name.isEmpty()) {
		return name;
	}
	const auto info = QFileInfo(name);
	const auto base = info.completeBaseName();
	if (base.isEmpty()) {
		// Nothing but an extension: there is no sender's name here to hide,
		// and upstream has already replaced it with one of its own.
		return name;
	}
	const auto suffix = info.suffix();

	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	hash.addData(Salt());
	hash.addData(name.toUtf8());
	const auto token = QString::fromLatin1(
		hash.result().left(kTokenBytes).toHex());

	return suffix.isEmpty() ? token : (token + '.' + suffix);
}

} // namespace NovaGram
