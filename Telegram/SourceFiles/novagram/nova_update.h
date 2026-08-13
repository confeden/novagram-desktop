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

// Aborts whatever is in flight and forgets the release that was found. Called
// when the decoy is armed inside a running process: the phase drives the bar
// in the chat list, and nothing found before the emergency PIN may survive
// into the disguise.
void Stop();

[[nodiscard]] Status Current();
[[nodiscard]] rpl::producer<Status> StatusValue();

void CheckNow();
void Download();

// Stops a download in progress and goes back to Found, so the same release can
// be fetched again. The bar in the chat list offers this while downloading, the
// way the official client does.
void Cancel();

// Hands the downloaded installer over and quits. The installer puts the new
// executable in place and starts it again; nothing it does touches the data
// directory.
void InstallAndRestart();

// The three functions below exist so that the update bar in the chat list is
// one call into this module and does not have to know either the phases or the
// interface language. Upstream drives its own bar from Core::UpdateChecker,
// which this build has switched off for good.

// True from the moment a newer release is known until it is installed.
[[nodiscard]] bool BarVisible(const Status &status);

// Short enough for a 46 pixel high button: "Update NovaGram", "Downloading
// 42%", "Install and restart". The bar in the chat list is the progress
// indicator as well, which is why the middle phase reads as a state.
[[nodiscard]] QString BarText(const Status &status);

// The same three phases named as actions, for a button that sits under a line
// already showing the state. "Downloading 42%" on both would name no action.
[[nodiscard]] QString ActionText(const Status &status);

// What pressing the bar does, which depends on the phase: start the download,
// stop it, or install.
void ActOnBar();

} // namespace NovaGram::Update
