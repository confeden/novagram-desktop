/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace NovaGram {

// Metadata carried inside a file the user sends.
//
// A photograph out of a phone's camera roll carries the camera model, the
// serial number of the lens on some bodies, the exact second it was taken and,
// unless the owner turned it off, the coordinates of where. None of that is
// visible in the picture, all of it travels with the file, and the caption the
// user wrote is the only part they meant to send.
//
// The stripping is done by walking the container and dropping the blocks that
// hold metadata, not by re-encoding: re-encoding costs a generation of quality
// on a file the user may have chosen to send uncompressed, and leaves an
// encoder fingerprint of its own instead of the one it removed.
[[nodiscard]] bool StripMetadataEnabled();

// The same answer, readable from the worker thread that prepares an upload.
// The settings are a main-thread structure, so the value is mirrored into an
// atomic when it is read or written.
[[nodiscard]] bool StripMetadataEnabledForTask();

void SetStripMetadataEnabled(bool enabled);
[[nodiscard]] QString StripMetadataTitle();

// Formats this build can actually strip. Deliberately narrow: claiming a file
// was cleaned when nothing was done to it is worse than saying it was not.
[[nodiscard]] bool CanStripMetadata(const QString &mime, const QString &name);

// Returns the bytes unchanged when there is nothing to remove or the format is
// not one of the two, so the caller can always assign the result.
[[nodiscard]] QByteArray StripImageMetadata(const QByteArray &bytes);

// True when the JPEG says it has to be rotated before it is looked at.
//
// The orientation lives in the Exif block, which is exactly what stripping
// removes, so the two cannot both happen to the same bytes: a picture whose
// pixels are sideways and whose "turn me" note has been cut off is sent
// sideways. The photo path answers this question first and re-encodes instead
// when it matters, because the image it re-encodes from has already been
// turned and needs no note.
[[nodiscard]] bool JpegNeedsRotation(const QByteArray &bytes);

} // namespace NovaGram
