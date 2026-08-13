/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_metadata.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "novagram/nova_pin.h"

#include <atomic>
#include <cstring>

namespace NovaGram {
namespace {

constexpr auto kEnabledKey = "novagram_strip_metadata"_cs;

// -1 while nothing has read the setting yet. The default is on, so an unread
// value answers the same way the stored one would.
std::atomic<int> Mirrored = -1;

[[nodiscard]] bool IsJpeg(const QByteArray &bytes) {
	return (bytes.size() > 3)
		&& (uchar(bytes[0]) == 0xFF)
		&& (uchar(bytes[1]) == 0xD8);
}

[[nodiscard]] bool IsPng(const QByteArray &bytes) {
	static const char kSignature[] = "\x89PNG\r\n\x1A\n";
	return (bytes.size() > 8)
		&& !memcmp(bytes.constData(), kSignature, 8);
}

// Everything an application, a camera or an editor writes about itself lives in
// an APPn or a comment segment. The three that are kept are the ones that
// change how the picture looks: JFIF density, an embedded colour profile and
// the Adobe marker that says how the colour channels were transformed.
[[nodiscard]] bool KeepJpegSegment(uchar marker, const char *payload, int size) {
	switch (marker) {
	case 0xE0: // APP0, JFIF.
		return true;
	case 0xE2: // APP2 - a colour profile, or a multi-picture index.
		return (size >= 12) && !memcmp(payload, "ICC_PROFILE", 12);
	case 0xEE: // APP14, Adobe: drops the colour transform if removed.
		return true;
	case 0xE1: // APP1: Exif and XMP, the two that carry everything.
	case 0xEC: // APP12, Ducky.
	case 0xED: // APP13, Photoshop and IPTC.
	case 0xFE: // COM.
		return false;
	default:
		// The remaining APPn are vendor blocks: Samsung, Apple, Canon and the
		// rest write their own. Nothing reads them here.
		return (marker < 0xE0) || (marker > 0xEF);
	}
}

[[nodiscard]] QByteArray StripJpeg(const QByteArray &bytes) {
	auto result = QByteArray();
	result.reserve(bytes.size());
	result.append(bytes.constData(), 2); // SOI.

	auto position = 2;
	const auto size = int(bytes.size());
	while (position + 1 < size) {
		if (uchar(bytes[position]) != 0xFF) {
			// Not a marker where one must be: the file is not what it claims
			// to be, and guessing at it would be worse than leaving it alone.
			return bytes;
		}
		auto marker = uchar(bytes[position + 1]);
		// A run of fill bytes is allowed before a marker.
		auto markerAt = position + 1;
		while ((marker == 0xFF) && (markerAt + 1 < size)) {
			++markerAt;
			marker = uchar(bytes[markerAt]);
		}
		if (marker == 0xD8 || marker == 0x01
			|| (marker >= 0xD0 && marker <= 0xD7)) {
			// Standalone, no length of its own.
			result.append(bytes.constData() + position, markerAt + 1 - position);
			position = markerAt + 1;
			continue;
		} else if (marker == 0xDA || marker == 0xD9) {
			// Start of scan: the compressed data follows and is not segmented.
			// Everything from here to the end is copied as it is.
			result.append(bytes.constData() + position, size - position);
			return result;
		}
		if (markerAt + 2 >= size) {
			return bytes;
		}
		const auto length = (int(uchar(bytes[markerAt + 1])) << 8)
			| int(uchar(bytes[markerAt + 2]));
		if (length < 2 || markerAt + 1 + length > size) {
			return bytes;
		}
		const auto payload = bytes.constData() + markerAt + 3;
		const auto payloadSize = length - 2;
		if (KeepJpegSegment(marker, payload, payloadSize)) {
			result.append(
				bytes.constData() + position,
				markerAt + 1 + length - position);
		}
		position = markerAt + 1 + length;
	}
	// Fell off the end without ever reaching the scan. A whole JPEG always
	// leaves through the branch above, so getting here means the file is
	// truncated - and what has been collected so far is a header with no
	// picture behind it. Sending that instead of what the user picked would be
	// worse than sending an unstripped file.
	return bytes;
}

[[nodiscard]] bool KeepPngChunk(const char *type) {
	// Text in any of its three spellings, the modification time and the Exif
	// block PNG gained later. Everything else describes the image itself.
	static const char *kDropped[] = { "tEXt", "zTXt", "iTXt", "tIME", "eXIf" };
	for (const auto dropped : kDropped) {
		if (!memcmp(type, dropped, 4)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] QByteArray StripPng(const QByteArray &bytes) {
	auto result = QByteArray();
	result.reserve(bytes.size());
	result.append(bytes.constData(), 8); // Signature.

	auto position = 8;
	auto ended = false;
	const auto size = int(bytes.size());
	while (position + 8 <= size) {
		const auto data = bytes.constData() + position;
		const auto length = (int64(uchar(data[0])) << 24)
			| (int64(uchar(data[1])) << 16)
			| (int64(uchar(data[2])) << 8)
			| int64(uchar(data[3]));
		const auto whole = length + 12; // Length, type, data, checksum.
		if (length < 0 || position + whole > size) {
			return bytes;
		}
		const auto type = data + 4;
		if (KeepPngChunk(type)) {
			result.append(data, int(whole));
		}
		position += int(whole);
		if (!memcmp(type, "IEND", 4)) {
			ended = true;
			break;
		}
	}
	// No IEND: the file is truncated, and the part collected so far is a
	// header without a picture. Same rule as for JPEG - leave it alone.
	return ended ? result : bytes;
}

// Reads Exif tag 0x0112 out of an APP1 block. Everything is bounds checked
// against the segment, because this runs on files picked by the user and a
// malformed Exif block is not an unusual thing to meet.
[[nodiscard]] int JpegOrientation(const char *payload, int size) {
	if (size < 14 || memcmp(payload, "Exif\0\0", 6)) {
		return 0;
	}
	const auto tiff = payload + 6;
	const auto tiffSize = size - 6;
	const auto big = !memcmp(tiff, "MM", 2);
	if (!big && memcmp(tiff, "II", 2)) {
		return 0;
	}
	const auto u16 = [&](int at) {
		const auto a = uchar(tiff[at]);
		const auto b = uchar(tiff[at + 1]);
		return big ? ((a << 8) | b) : ((b << 8) | a);
	};
	const auto u32 = [&](int at) {
		const auto a = uint32(uchar(tiff[at]));
		const auto b = uint32(uchar(tiff[at + 1]));
		const auto c = uint32(uchar(tiff[at + 2]));
		const auto d = uint32(uchar(tiff[at + 3]));
		return big
			? ((a << 24) | (b << 16) | (c << 8) | d)
			: ((d << 24) | (c << 16) | (b << 8) | a);
	};
	const auto offset = int(u32(4));
	if (offset < 8 || offset + 2 > tiffSize) {
		return 0;
	}
	const auto count = u16(offset);
	for (auto i = 0; i != count; ++i) {
		const auto entry = offset + 2 + (i * 12);
		if (entry + 12 > tiffSize) {
			return 0;
		} else if (u16(entry) == 0x0112) {
			return u16(entry + 8);
		}
	}
	return 0;
}

} // namespace

bool JpegNeedsRotation(const QByteArray &bytes) {
	if (!IsJpeg(bytes)) {
		return false;
	}
	auto position = 2;
	const auto size = int(bytes.size());
	while (position + 3 < size) {
		if (uchar(bytes[position]) != 0xFF) {
			return false;
		}
		auto markerAt = position + 1;
		auto marker = uchar(bytes[markerAt]);
		while ((marker == 0xFF) && (markerAt + 1 < size)) {
			++markerAt;
			marker = uchar(bytes[markerAt]);
		}
		if (marker == 0xDA || marker == 0xD9) {
			return false;
		} else if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
			position = markerAt + 1;
			continue;
		}
		if (markerAt + 2 >= size) {
			return false;
		}
		const auto length = (int(uchar(bytes[markerAt + 1])) << 8)
			| int(uchar(bytes[markerAt + 2]));
		if (length < 2 || markerAt + 1 + length > size) {
			return false;
		}
		if (marker == 0xE1) {
			const auto orientation = JpegOrientation(
				bytes.constData() + markerAt + 3,
				length - 2);
			// 1 is "as it lies", 0 is "not said at all".
			if (orientation > 1) {
				return true;
			}
		}
		position = markerAt + 1 + length;
	}
	return false;
}

bool StripMetadataEnabled() {
	const auto result = Core::App().settings().readPref<bool>(
		kEnabledKey,
		true);
	Mirrored.store(result ? 1 : 0, std::memory_order_relaxed);
	return result;
}

bool StripMetadataEnabledForTask() {
	const auto mirrored = Mirrored.load(std::memory_order_relaxed);
	return (mirrored < 0) || (mirrored == 1);
}

void SetStripMetadataEnabled(bool enabled) {
	Core::App().settings().writePref<bool>(kEnabledKey, enabled);
	Core::App().saveSettingsDelayed();
	Mirrored.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

QString StripMetadataTitle() {
	return UseRussianTexts()
		? u"Очищать метаданные отправляемых файлов"_q
		: u"Strip metadata from files you send"_q;
}

bool CanStripMetadata(const QString &mime, const QString &name) {
	const auto lowerMime = mime.toLower();
	if (lowerMime == u"image/jpeg"_q
		|| lowerMime == u"image/jpg"_q
		|| lowerMime == u"image/png"_q) {
		return true;
	}
	const auto lowerName = name.toLower();
	return lowerName.endsWith(u".jpg"_q)
		|| lowerName.endsWith(u".jpeg"_q)
		|| lowerName.endsWith(u".png"_q);
}

QByteArray StripImageMetadata(const QByteArray &bytes) {
	if (!StripMetadataEnabledForTask() || bytes.isEmpty()) {
		return bytes;
	} else if (IsJpeg(bytes)) {
		return StripJpeg(bytes);
	} else if (IsPng(bytes)) {
		return StripPng(bytes);
	}
	return bytes;
}

} // namespace NovaGram
