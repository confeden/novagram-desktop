/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_crash_stickers.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_location.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/stickers/data_stickers.h"
#include "novagram/nova_pin.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtGui/QImageReader>

namespace NovaGram::CrashStickers {
namespace {

constexpr auto kEnabledKey = "novagram_crash_stickers"_cs;

// Telegram publishes stickers at 512x512, 64 KB for an animated one, 256 KB
// for a video one and three seconds of play. Every limit here is the published
// one loosened until it cannot be met by accident: a file past any of them was
// never going to be drawn as a sticker, so refusing it takes nothing away.
constexpr auto kMaxSide = 1024;
constexpr auto kMaxArea = 1024 * 1024;
constexpr auto kMaxFileBytes = 4 * 1024 * 1024;
constexpr auto kMaxUnpackedBytes = 4 * 1024 * 1024;
constexpr auto kMaxDurationMs = crl::time(30 * 1000);

// The Lottie file itself. Depth and the object count answer the two shapes a
// crash file takes when it is small enough to pass every size limit: nesting
// that overflows the parser's stack, and a scene that is cheap to write and
// impossible to rasterise. A repeater is the third: twenty bytes of JSON ask
// for its shape to be drawn as many times as the number says.
constexpr auto kMaxDepth = 128;
constexpr auto kMaxObjects = 200 * 1000;
constexpr auto kMaxItems = 1000 * 1000;
constexpr auto kMaxRepeaterCopies = 100;
constexpr auto kMaxFps = 120.;
constexpr auto kMaxFrames = 1200.;

// One entry per sticker seen, so a pack of a hundred costs a hundred. Cleared
// wholesale rather than by age: the answers are cheap to compute again, and a
// map that grows for the life of the process is not.
constexpr auto kVerdictsLimit = 8192;

enum class Verdict : uchar {
	Safe,
	Dangerous,
};

std::optional<bool> GlobalEnabled;
rpl::event_stream<bool> GlobalChanges;
base::flat_map<DocumentId, Verdict> GlobalVerdicts;
base::flat_map<uint64, QImage> GlobalPlaceholders;

void Remember(DocumentId id, Verdict verdict) {
	if (GlobalVerdicts.size() >= kVerdictsLimit) {
		GlobalVerdicts.clear();
	}
	GlobalVerdicts.emplace(id, verdict);
}

struct JsonScan {
	int maxDepth = 0;
	int objects = 0;
	bool balanced = false;
};

// How many leading bytes the Lottie reader throws away before it parses. This
// repeats Lottie::ReadUtf8 (lib_lottie/lottie/lottie_common.cpp) case for case
// on purpose, and must keep repeating it: the guard has to judge exactly the
// bytes the decoder will judge. With a byte order mark in front, the reader
// skipped it and parsed the file happily while the guard, looking for '{' at
// the front, decided the content was not JSON at all and said "not judged" -
// so three bytes turned the whole guard off, and the verdict was remembered.
// lib_lottie is vendored upstream and is not patched for this.
[[nodiscard]] int BomSkip(const QByteArray &data) {
	if (data.size() < 4) {
		return 0;
	}
	const auto bom = uint32(uint8(data[0]))
		| (uint32(uint8(data[1])) << 8)
		| (uint32(uint8(data[2])) << 16)
		| (uint32(uint8(data[3])) << 24);
	return ((bom == 0xFFFE0000U) || (bom == 0x0000FEFFU))
		? 4
		: (((bom & 0xFFFFU) == 0xFFFEU) || ((bom & 0xFFFFU) == 0xFEFFU))
		? 2
		: (((bom & 0xFFFFFFU) == 0xBFBBEFU)
			? 3
			: 0);
}

// A pass over the bytes, before any parser sees them. It answers how deep the
// file goes without recursing itself, which is the whole point: the parser
// that would answer the same question is the one being protected.
[[nodiscard]] JsonScan ScanJson(const QByteArray &json) {
	auto result = JsonScan();
	auto depth = 0;
	auto inString = false;
	auto escaped = false;
	for (const char ch : json) {
		if (inString) {
			if (escaped) {
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
			} else if (ch == '"') {
				inString = false;
			}
			continue;
		}
		switch (ch) {
		case '"':
			inString = true;
			break;
		case '{':
			++result.objects;
			[[fallthrough]];
		case '[':
			if (++depth > result.maxDepth) {
				result.maxDepth = depth;
				if (depth > kMaxDepth) {
					return result;
				}
			}
			break;
		case '}':
		case ']':
			if (--depth < 0) {
				return result;
			}
			break;
		}
		if (result.objects > kMaxObjects) {
			return result;
		}
	}
	result.balanced = !depth && !inString;
	return result;
}

// Recursion is safe here and only here: ScanJson has already refused anything
// deeper than kMaxDepth, so the tree this walks is at most that tall.
[[nodiscard]] bool WalkDangerous(const QJsonValue &value, int &items) {
	if (++items > kMaxItems) {
		return true;
	}
	if (value.isObject()) {
		const auto object = value.toObject();
		if (object.value(u"ty"_q).toString() == u"rp"_q) {
			// A repeater with a fixed count says it in "c": { "a": 0, "k": N }.
			// An animated count leaves k an array of keyframes and reads as
			// zero here - exotic enough that the size limits are left to catch
			// it rather than teaching this to interpolate.
			const auto copies = object.value(u"c"_q).toObject().value(u"k"_q);
			if (copies.toDouble() > kMaxRepeaterCopies) {
				return true;
			}
		}
		for (auto i = object.begin(); i != object.end(); ++i) {
			if (WalkDangerous(QJsonValue(i.value()), items)) {
				return true;
			}
		}
	} else if (value.isArray()) {
		const auto array = value.toArray();
		for (auto i = array.begin(); i != array.end(); ++i) {
			if (WalkDangerous(QJsonValue(*i), items)) {
				return true;
			}
		}
	}
	return false;
}

[[nodiscard]] bool LottieDangerous(const QByteArray &json) {
	const auto scan = ScanJson(json);
	if (!scan.balanced
		|| (scan.maxDepth > kMaxDepth)
		|| (scan.objects > kMaxObjects)) {
		return true;
	}
	auto error = QJsonParseError();
	const auto parsed = QJsonDocument::fromJson(json, &error);
	if ((error.error != QJsonParseError::NoError) || !parsed.isObject()) {
		return true;
	}
	const auto root = parsed.object();
	const auto width = root.value(u"w"_q).toInt();
	const auto height = root.value(u"h"_q).toInt();
	if ((width <= 0)
		|| (height <= 0)
		|| (width > kMaxSide)
		|| (height > kMaxSide)
		|| (int64(width) * height > kMaxArea)) {
		return true;
	}
	const auto fps = root.value(u"fr"_q).toDouble();
	if ((fps <= 0.) || (fps > kMaxFps)) {
		return true;
	}
	const auto frames = root.value(u"op"_q).toDouble()
		- root.value(u"ip"_q).toDouble();
	if ((frames <= 0.) || (frames > kMaxFrames)) {
		return true;
	}
	auto items = 0;
	return WalkDangerous(QJsonValue(root), items);
}

// Only the header is read, never the pixels: QImageReader::size stops at it,
// and the whole point is to know how big the picture claims to be before
// something allocates that much.
[[nodiscard]] bool ImageDangerous(const QByteArray &content) {
	auto buffer = QBuffer();
	buffer.setData(content);
	if (!buffer.open(QIODevice::ReadOnly)) {
		return false;
	}
	auto reader = QImageReader(&buffer);
	reader.setAutoDetectImageFormat(true);
	const auto size = reader.size();
	if (!size.isValid() || size.isEmpty()) {
		// Not a picture this build can read at all, so nothing will decode it.
		return false;
	}
	return (size.width() > kMaxSide)
		|| (size.height() > kMaxSide)
		|| (int64(size.width()) * size.height() > kMaxArea);
}

// What the server said about the file, which is known before a byte of it is
// downloaded. A lie here is caught by the content check later; the truth here
// is caught before anything is fetched.
[[nodiscard]] bool DeclaredDangerous(not_null<DocumentData*> document) {
	if (document->size > kMaxFileBytes) {
		return true;
	}
	const auto dimensions = document->dimensions;
	if ((dimensions.width() > kMaxSide)
		|| (dimensions.height() > kMaxSide)
		|| (int64(dimensions.width()) * dimensions.height() > kMaxArea)) {
		return true;
	}
	return (document->duration() > kMaxDurationMs);
}

[[nodiscard]] bool ContentDangerous(
		not_null<DocumentData*> document,
		const QByteArray &content) {
	const auto sticker = document->sticker();
	if (!sticker || content.isEmpty()) {
		return false;
	} else if (content.size() > kMaxFileBytes) {
		return true;
	} else if (sticker->isLottie()) {
		// A .tgs on the wire is gzipped JSON - but a client is free to keep it
		// already inflated, and the Android half caches exactly that. Assuming
		// the gzip there refused every real sticker in a pack, so both shapes
		// are read here too, and this half is not left holding an assumption
		// that has already been wrong once.
		const auto gzipped = (content.size() >= 2)
			&& (uchar(content[0]) == 0x1F)
			&& (uchar(content[1]) == 0x8B);
		const auto json = gzipped ? Images::UnpackGzip(content) : content;
		if (gzipped && (json.size() > kMaxUnpackedBytes)) {
			return true;
		}
		// From the same offset the decoder starts at, never from zero - and
		// the same slice is what gets judged below, because LottieDangerous
		// calls a parse error dangerous, and Qt's parser refuses a byte order
		// mark that the Lottie reader would have stepped over. Judging the
		// unsliced bytes would refuse ordinary stickers that carry one.
		const auto skip = BomSkip(json);
		auto i = skip;
		while ((i != json.size()) && QChar::isSpace(uchar(json[i]))) {
			++i;
		}
		if ((i == json.size()) || (json[i] != '{')) {
			// Neither a gzip this build can inflate nor a JSON object -
			// including what UnpackGzip hands back unchanged when the stream
			// is broken. Not judged rather than accused: rlottie refuses
			// garbage by itself, and it is a file that *parses* into something
			// ruinous which this exists to stop.
			return false;
		}
		return LottieDangerous(skip ? json.mid(skip) : json);
	} else if (sticker->isWebm()) {
		// The container is left to FFmpeg, which is streamed rather than
		// decoded into one buffer. What can be answered here is the size and
		// the dimensions, and those are answered above.
		return false;
	}
	return ImageDangerous(content);
}

[[nodiscard]] QByteArray ContentFor(not_null<Data::DocumentMedia*> media) {
	auto content = media->bytes();
	if (!content.isEmpty()) {
		return content;
	}
	const auto document = media->owner();
	const auto &location = document->location(true);
	if (location.isEmpty() || !location.accessEnable()) {
		return QByteArray();
	}
	const auto guard = gsl::finally([&] { location.accessDisable(); });
	auto file = QFile(location.name());
	if (!file.open(QIODevice::ReadOnly)) {
		return QByteArray();
	}
	return file.read(kMaxFileBytes + 1);
}

[[nodiscard]] bool BlockedById(
		not_null<DocumentData*> document,
		not_null<bool*> known) {
	const auto i = GlobalVerdicts.find(document->id);
	if (i != end(GlobalVerdicts)) {
		*known = true;
		return (i->second == Verdict::Dangerous);
	}
	*known = false;
	if (DeclaredDangerous(document)) {
		Remember(document->id, Verdict::Dangerous);
		*known = true;
		return true;
	}
	return false;
}

// A sticker, and not merely something the sticker decoder is asked to draw.
//
// Emoji are outside this on purpose. A reaction, an animated emoji, a custom
// emoji, a status, a topic icon, a dice roll and a gift animation are all .tgs
// files and all used to arrive here, which is how an ordinary reaction came to
// be labelled "Краш-стикер (обезврежен)" - a false accusation about a file
// Telegram itself supplies.
//
// The line is who chose the file: this guard exists against a document another
// user sends. A custom emoji is marked as one by its set type; everything else
// the server hands out from its own configuration names one of its own sets,
// which is what StickerData::serviceSet records.
//
// It used to read "has a set identifier" instead, and that was wrong in both
// directions. UpdateStickerSetIdentifier keeps an identifier only for
// inputStickerSetID, so a sticker from a set named by its short name had none
// either and went unjudged - and so did anything a sender simply labelled
// inputStickerSetEmpty, which is three words in a modified client and turned
// the guard off completely. Asking about the service sets by name instead
// keeps the narrowing this line was added for (a reaction must never be
// accused, G53) without leaving that door.
[[nodiscard]] bool IsSticker(not_null<DocumentData*> document) {
	const auto sticker = document->sticker();
	return sticker
		&& (sticker->setType != Data::StickersType::Emoji)
		&& !sticker->serviceSet;
}

} // namespace

bool Enabled() {
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
	GlobalChanges.fire_copy(enabled);
}

rpl::producer<bool> EnabledChanges() {
	return GlobalChanges.events();
}

QString Title() {
	return UseRussianTexts()
		? u"Защита от краш-стикеров"_q
		: u"Crash sticker guard"_q;
}

QString About() {
	return UseRussianTexts()
		? u"Файл, который выдаёт себя за стикер, а собран как бомба — "
			"огромный после распаковки, с вложенностью в сотни уровней или с "
			"повторителем на сотни копий, — не доходит до декодера и не "
			"отправляется дальше; вместо него остаётся подпись «Краш-стикер "
			"(обезврежен)». Настоящий стикер до этих границ не дотягивает и "
			"близко. Обычный по устройству файл, который роняет чужой клиент "
			"через ошибку в его декодере, так не распознаётся."_q
		: u"A file that poses as a sticker but is built as a bomb - enormous "
			"once unpacked, nested hundreds of levels deep, or repeating one "
			"shape hundreds of times - never reaches the decoder, and is not "
			"passed on either. A \"Crash sticker (defused)\" caption takes its "
			"place. A real sticker is nowhere near those limits. This does not "
			"recognise a file that is ordinary in shape and crashes some other "
			"client through a bug in its decoder."_q;
}

QString PlaceholderText() {
	return UseRussianTexts()
		? u"Краш-стикер (обезврежен)"_q
		: u"Crash sticker (defused)"_q;
}

bool Blocked(not_null<DocumentData*> document) {
	if (!Enabled() || !IsSticker(document)) {
		return false;
	}
	auto known = false;
	return BlockedById(document, &known);
}

bool Blocked(
		not_null<DocumentData*> document,
		const QByteArray &content) {
	if (!Enabled() || !IsSticker(document)) {
		return false;
	}
	auto known = false;
	if (const auto blocked = BlockedById(document, &known); known) {
		return blocked;
	} else if (content.isEmpty()) {
		return false;
	}
	const auto dangerous = ContentDangerous(document, content);
	Remember(document->id, dangerous ? Verdict::Dangerous : Verdict::Safe);
	return dangerous;
}

bool Blocked(not_null<Data::DocumentMedia*> media) {
	const auto document = media->owner();
	if (!Enabled() || !IsSticker(document)) {
		return false;
	}
	// Nothing read yet means nothing to decode either, and ContentFor says so
	// with an empty answer rather than with a verdict of its own.
	return Blocked(document, ContentFor(media));
}

void PaintPlaceholder(QPainter &p, QRect box) {
	if (box.isEmpty()) {
		return;
	}
	const auto side = std::min(box.width(), box.height());
	const auto radius = std::max(side / 12, 2);
	const auto inner = box.marginsRemoved(
		QMargins(side / 16, side / 16, side / 16, side / 16));
	const auto hints = p.renderHints();
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0, 0, 0, 140));
	p.drawRoundedRect(inner, radius, radius);

	auto font = st::normalFont->f;
	font.setBold(true);
	font.setPixelSize(std::max(side / 9, 8));
	p.setFont(font);
	p.setPen(QColor(255, 255, 255, 230));
	p.drawText(
		inner.marginsRemoved(QMargins(side / 12, 0, side / 12, 0)),
		Qt::AlignCenter | Qt::TextWordWrap,
		PlaceholderText());
	p.setRenderHints(hints);
}

QImage PlaceholderImage(QSize size) {
	if (size.isEmpty()) {
		size = QSize(kMaxSide / 2, kMaxSide / 2);
	}
	size = QSize(
		std::clamp(size.width(), 32, kMaxSide),
		std::clamp(size.height(), 32, kMaxSide));
	const auto key = (uint64(uint32(size.width())) << 32)
		| uint64(uint32(size.height()));
	if (const auto i = GlobalPlaceholders.find(key)
		; i != end(GlobalPlaceholders)) {
		return i->second;
	}
	auto result = QImage(size, QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	{
		auto p = QPainter(&result);
		PaintPlaceholder(p, QRect(QPoint(), size));
	}
	if (GlobalPlaceholders.size() >= 32) {
		GlobalPlaceholders.clear();
	}
	GlobalPlaceholders.emplace(key, result);
	return result;
}

} // namespace NovaGram::CrashStickers
