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

// Fork files other than key_data that carry the same seal. The value only
// picks the context that goes into the AAD, so a sealed blob cannot be moved
// from one of these files into another.
enum class SealedFile : uchar {
	Pin,
	Decoy,
};

enum class OpenResult : uchar {
	// No seal in front of the value: written by an older build, or while the
	// binding was off. The bytes are handed back unchanged.
	Plain,
	// Sealed, and opened on this machine.
	Opened,
	// Sealed, and this machine cannot open it.
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

// The owner's preference. Not the same question as DataSealed(): a preference
// can outlive the mechanism that was supposed to keep it.
[[nodiscard]] bool Enabled();

// What the disk actually holds after the last read or write of the accounts
// file. The settings toggle draws this one: a switch that says "bound" over a
// tdata folder that is portable again is a promise the file does not keep.
[[nodiscard]] bool DataSealed();

// Rewrites the binding file and re-writes the accounts file through it. Does
// nothing while Blocked(). Switching on while the preference is already on but
// nothing is sealed is a retry, not a no-op.
void SetEnabled(bool enabled);

// Wraps / unwraps the passcode-encrypted local key, which is the one value
// every other file under tdata hangs from. Wrap() returns its argument
// unchanged while the binding is off or while this machine offers no mechanism
// at all, and an **empty** array when a secret exists and sealing failed - the
// caller must then refuse the write, never fall back to the plaintext, because
// that is a silent unbind of data the owner asked to be bound. Unwrap()
// returns an empty array and flips Blocked() when the value belongs to another
// machine.
[[nodiscard]] QByteArray Wrap(const QByteArray &keyEncrypted);
[[nodiscard]] QByteArray Unwrap(const QByteArray &stored);

// The same seal for the fork's own side files. Unlike Wrap()/Unwrap() these
// never touch Blocked(): one unreadable side file must not send the whole
// installation to the "another device" screen, and must never destroy
// anything (D13).
//
// SealPayload() follows Wrap()'s contract - the argument unchanged when there
// is nothing to seal to, an empty array when sealing failed and the write has
// to be refused.
[[nodiscard]] QByteArray SealPayload(
	SealedFile file,
	const QByteArray &plain);
[[nodiscard]] OpenResult OpenPayload(
	SealedFile file,
	const QByteArray &stored,
	QByteArray &plain);

// True when what was read from disk is not what would be written now: an
// unbound blob from an older build, or a binding switched on or off since the
// last write. The caller answers by rewriting the accounts file.
[[nodiscard]] bool NeedsRewrite();

// Device-bound record of "a NovaGram pin is set on this installation". It
// lives next to the machine secret and not in tdata/novagram_pin, because
// deleting that one file is the cheapest way to take the emergency pin, the
// lockout counter and the pin mode away in a single gesture. The mismatch -
// this flag on, the file gone - is what makes the deletion detectable.
[[nodiscard]] bool PinArmed();
void SetPinArmed(bool armed);

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

// Whether "this belongs to another device" is an answer or only the safe reply
// to a question that could not be asked. A DPAPI call that never happened, a
// file version this build does not know and a machine that would not identify
// itself all look exactly like a foreign data folder from the outside - and
// none of them is a reason to offer to destroy an account.
[[nodiscard]] bool ForeignCertain();
[[nodiscard]] QString UnsureTitle();
[[nodiscard]] QString UnsureText();
[[nodiscard]] QString UnsureRetryButton();
[[nodiscard]] QString BackendName(Backend backend);

} // namespace NovaGram::DeviceLock
