/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace NovaGram {

// What the application calls itself on screen: the fork name normally, the
// stock one while the decoy is armed. Display only — every data path and
// OS-level identifier keeps the base name, so wearing the disguise costs the
// decoy no access to its own installed copy, and renaming costs no profile.
[[nodiscard]] QString AppName();

// The fork name, whatever the decoy is doing. Needed exactly where the decoy
// has to undo something an ordinary run wrote under the fork name — it cannot
// ask AppName() for that, because while the decoy is armed AppName() answers
// with the disguise and the undo would miss its target.
[[nodiscard]] QString ForkAppName();

// Upstream language packs spell the application name inside whole phrases, so
// a fork cannot rename itself by changing a constant. Rewriting the finished
// phrase keeps every translation working without touching lang.strings.
[[nodiscard]] QString WithAppName(QString text);

// The version of the Telegram Desktop this is built on, which is also the
// version NovaGram releases under on this platform. One release covers both
// platforms and carries both base versions in its tag, but each client only
// ever compares its own: an Android-only release leaves this number where it
// was, and the desktop correctly reports that there is nothing to install.
[[nodiscard]] QString AppVersion();

// Tag of the release this build belongs to, both halves of it:
// "v<desktop base>/<android base>". Has to be updated by hand for every
// release, because a desktop build cannot know the Android version.
[[nodiscard]] QString ReleaseTag();

[[nodiscard]] QString ProjectUrl();

// Page of the release this build came from, with the list of what changed.
[[nodiscard]] QString ReleaseUrl();

// Negative when `a` is older than `b`, zero when they are the same release,
// positive when `a` is newer. Missing components count as zero, so "1.1" and
// "1.1.0" are one version, and a value that is not a version at all sorts as
// the oldest possible one rather than throwing.
[[nodiscard]] int CompareVersions(const QString &a, const QString &b);

} // namespace NovaGram
