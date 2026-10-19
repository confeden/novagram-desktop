/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

#include <optional>

namespace NovaGram::Terminator {

// Qt cannot shape a TLS ClientHello: the order and the set of extensions, GREASE
// and the key shares are whatever OpenSSL writes. The WSS transport can instead
// hand the TLS leg to nova-tls-terminator.exe, a small BoringSSL program of the
// Nova PC project that writes the ClientHello of Yandex Browser, and speak
// WebSocket over the plain loopback leg it gives back.
//
// What this changes: how the connection looks to someone watching the wire.
// What it does not change: the address the connection comes from, and what the
// relay sees. The program listens on 127.0.0.1 only, behind a token that is
// made anew on every start and handed over in the environment, and it checks
// the certificate against the roots in nova_roots.pem (Mozilla's bundle) beside
// the executable, because BoringSSL has no Windows certificate store.

// Both files lie next to the executable.
[[nodiscard]] bool Available();

// The setting: on by default when Available(). Off while the files are absent.
[[nodiscard]] bool Enabled();
void SetEnabled(bool enabled);

// After the settings are read, on the main thread. Starts the program when the
// WSS transport and this setting are both on and does nothing otherwise.
void Start();
// Called when WSS or this setting is switched, to start or stop it.
void Refresh();
// Kills the child. Also done by the job object if the client dies.
void Stop();

struct Endpoint {
	int port = 0;
	QString token;
	QString roots; // Absolute path of the PEM bundle.
};

// Readable from the connection thread: set only while the program has said
// that it listens. Empty means "use the Qt socket".
[[nodiscard]] std::optional<Endpoint> Current();

// One line in the log per run, the first time a connection had to fall back.
void NoteFellBack(const QString &why);

[[nodiscard]] QString Title();
[[nodiscard]] QString About();

} // namespace NovaGram::Terminator
