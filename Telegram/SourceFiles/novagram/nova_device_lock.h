/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace NovaGram::DeviceLock {

// How the device secret is protected on this machine. Reported in settings,
// because the guarantee must never be stated stronger than what the machine
// actually gave us.
enum class Backend : uchar {
	None,
	// Machine identifiers only: no operating system secret store took part.
	// The binding file alone is useless, but a full disk image carries the
	// identifiers with it.
	Fingerprint,
	// Windows DPAPI, current user scope, machine fingerprint as the extra
	// entropy. Opening the blob needs both this Windows account and this
	// machine.
	Dpapi,
};

enum class State : uchar {
	// Switched off by the owner: tdata stays portable, as upstream.
	Off,
	// Nothing bound yet - the next accounts write creates the binding.
	Fresh,
	// The secret was recovered on this machine.
	Bound,
	// A binding exists and this machine cannot open it. The data came from
	// somewhere else, or this Windows account is no longer the one that
	// created it.
	Foreign,
};

// Reads the binding file. Must run before the storage domain is started and
// before anything asks for Wrap()/Unwrap().
void Start();

[[nodiscard]] State Current();
[[nodiscard]] Backend CurrentBackend();

// True when the accounts file on disk was bound elsewhere. The client must
// not start the domain in that case: nothing under tdata can be read, and
// overwriting it would destroy data that the rightful machine still opens.
[[nodiscard]] bool Blocked();

[[nodiscard]] bool Enabled();

// Rewrites the binding file and re-writes the accounts file through it. Does
// nothing while Blocked().
void SetEnabled(bool enabled);

// Wraps / unwraps the passcode-encrypted local key, which is the one value
// every other file under tdata hangs from. Wrap() returns its argument
// unchanged while the binding is off; Unwrap() returns an empty array and
// flips Blocked() when the value belongs to another machine.
[[nodiscard]] QByteArray Wrap(const QByteArray &keyEncrypted);
[[nodiscard]] QByteArray Unwrap(const QByteArray &stored);

// True when what was read from disk is not what would be written now: an
// unbound blob from an older build, or a binding switched on or off since the
// last write. The caller answers by rewriting the accounts file.
[[nodiscard]] bool NeedsRewrite();

// Drops the unreadable data together with the stale binding, so that the next
// start is an ordinary first start. Only the blocked screen calls this: it is
// the moment the owner chooses to give up on data this machine cannot read.
void ResetForNewDevice();

// Forgets the secret held in memory, without touching anything on disk. Has to
// be called by whoever removes the binding file behind this module's back - the
// emergency wipe does - or the next accounts write would seal to a secret that
// the next start cannot recover, and the wipe would end at the blocked screen.
void Forget();

[[nodiscard]] QString BlockedTitle();
[[nodiscard]] QString BlockedText();
[[nodiscard]] QString BlockedResetButton();
[[nodiscard]] QString BackendName(Backend backend);

} // namespace NovaGram::DeviceLock
