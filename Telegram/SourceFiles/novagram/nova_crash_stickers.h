/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class DocumentData;

namespace Data {
class DocumentMedia;
} // namespace Data

namespace NovaGram::CrashStickers {

// Refuses to decode a sticker that is not a sticker but a bomb.
//
// A sticker set is content the sender chooses and the receiver opens without
// being asked, so a file that kills the decoder is a remote crash: the pack
// twomassiveee takes this client down on the preview alone, while the same
// pack is harmless on Android, where every frame is drawn into a bitmap the
// application sized itself. On by default, unlike the switches that hide
// something - this one takes nothing away from a sticker that is real.
//
// What "not a sticker" means is Telegram's own publishing rules, loosened by
// a wide margin: 512x512 becomes 1024, 64 KB becomes 4 MB, three seconds
// become thirty. Anything past that was never going to render as a sticker,
// so refusing it costs nobody a sticker they could have seen.
[[nodiscard]] bool Enabled();
void SetEnabled(bool enabled);

// Fired on every change, so the open chats and panels can redraw without a
// restart. A sticker already replaced stays replaced until its view is rebuilt.
[[nodiscard]] rpl::producer<bool> EnabledChanges();

[[nodiscard]] QString Title();
[[nodiscard]] QString About();

// The words drawn in place of the sticker.
[[nodiscard]] QString PlaceholderText();

// The verdict, remembered per document. The overload that takes the media
// looks at the bytes when they are already in memory; the one that takes the
// document alone answers from what the server declared and from an earlier
// look at the content, and never reads a file itself.
//
// Both answer false with the switch off, so a caller may ask before painting
// without checking the setting first.
[[nodiscard]] bool Blocked(not_null<DocumentData*> document);
[[nodiscard]] bool Blocked(not_null<Data::DocumentMedia*> media);

// For the decoders that are handed the bytes directly instead of the media -
// the custom emoji renderer, the hover preview, the good-thumbnail worker.
[[nodiscard]] bool Blocked(
	not_null<DocumentData*> document,
	const QByteArray &content);

// The image put in the place of a blocked sticker: the words above on a dark
// rounded plate, drawn once per size. Deliberately theme-independent - it is
// cached inside Data::DocumentMedia, which outlives a theme change.
[[nodiscard]] QImage PlaceholderImage(QSize size);

// Drawn over the sticker box by the views that paint stickers themselves
// rather than through Data::DocumentMedia.
void PaintPlaceholder(QPainter &p, QRect box);

} // namespace NovaGram::CrashStickers
