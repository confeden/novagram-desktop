/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace NovaGram::Update {

// One entry of the manifest published next to the releases.
struct Release {
	QString version;
	QString url;
	QString sha256;
	QString releaseUrl;
};

enum class Phase : uchar {
	Idle,
	Checking,
	UpToDate,
	Found,
	Downloading,
	Ready,
	Failed,
};

struct Status {
	Phase phase = Phase::Idle;
	Release release;
	int progress = 0;
};

// The only network request NovaGram makes outside Telegram, so it is a
// setting and not a fact of life. On by default: a privacy client that misses
// its own security fixes is worse off than one that asks a static file whether
// there are any.
[[nodiscard]] bool CheckEnabled();
void SetCheckEnabled(bool enabled);

// Begins the eight hour cycle, with the first check shortly after start.
// Does nothing at all while the decoy is on: the decoy promises that the
// process opens no sockets, and an update check would break that promise in
// the most traceable way possible.
void Start();

[[nodiscard]] Status Current();
[[nodiscard]] rpl::producer<Status> StatusValue();

void CheckNow();
void Download();

// Hands the downloaded installer over and quits. The installer puts the new
// executable in place and starts it again; nothing it does touches the data
// directory.
void InstallAndRestart();

} // namespace NovaGram::Update
