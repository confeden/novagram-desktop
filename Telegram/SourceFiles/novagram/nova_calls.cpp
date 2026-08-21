/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_calls.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "novagram/nova_pin.h"

namespace NovaGram::Calls {
namespace {

constexpr auto kRelayOnlyKey = "novagram_calls_relay_only"_cs;

} // namespace

// On by default, and that is the whole point of the fork having this at all:
// a promise that has to be switched on protects only the people who already
// knew to look for it. A pref that was never touched has no record, so the
// default applies to everyone; an explicit "no" is stored and survives.
bool RelayOnly() {
	return Core::App().settings().readPref<bool>(kRelayOnlyKey, true);
}

void SetRelayOnly(bool value) {
	Core::App().settings().writePref<bool>(kRelayOnlyKey, value);
	Core::App().saveSettingsDelayed();
}

// Deliberately not QHostAddress. The question is not "can Qt read this", it is
// "will the call stack read this, and if it cannot, will it hand the string to
// the resolver". The call stack is tgcalls, whose rtc::SocketAddress goes
// through inet_pton - and inet_pton is stricter than Qt, which still accepts
// shortened and non-decimal forms like 127.1 or 0x7f.0.0.1. A string that Qt
// reads and inet_pton refuses would be passed here and then resolved there,
// which is the one outcome this function exists to prevent. So the check is
// written to inet_pton's rules, and to the same rules on both platforms - the
// twin of this is NovaCallPolicy.acceptableCallAddress on Android.
[[nodiscard]] bool LooksLikeIpv4(const QString &address) {
	auto groups = 0;
	auto digits = 0;
	auto value = 0;
	for (const auto ch : address) {
		if (ch == '.') {
			if (!digits || ++groups > 3) {
				return false;
			}
			digits = 0;
			value = 0;
		} else if (ch >= '0' && ch <= '9') {
			if (++digits > 3) {
				return false;
			}
			value = value * 10 + (ch.unicode() - '0');
			if (value > 255) {
				return false;
			}
		} else {
			return false;
		}
	}
	return (groups == 3) && (digits > 0);
}

[[nodiscard]] bool LooksLikeIpv6(const QString &address) {
	// Permissive about the shape and strict about the alphabet: what has to be
	// impossible is a name reaching the resolver, and a malformed literal is
	// refused by the socket layer anyway. A zone index is refused too - it
	// names an interface of this machine and has no business arriving from a
	// server.
	auto colons = 0;
	for (const auto ch : address) {
		if (ch == ':') {
			++colons;
		} else if (ch == '.'
			|| (ch >= '0' && ch <= '9')
			|| (ch >= 'a' && ch <= 'f')
			|| (ch >= 'A' && ch <= 'F')) {
			continue;
		} else {
			return false;
		}
	}
	return (colons >= 2) && (colons <= 7);
}

bool AcceptableCallAddress(const QString &address) {
	if (address.isEmpty()) {
		// Not an address at all. The stock code skips empty strings in some of
		// the places this is asked from and not in others, so it is answered
		// here once.
		return false;
	}
	return address.contains(':')
		? LooksLikeIpv6(address)
		: LooksLikeIpv4(address);
}

QString RelayOnlyTitle() {
	return UseRussianTexts()
		? u"Звонки только через серверы Telegram"_q
		: u"Route calls through Telegram servers only"_q;
}

QString RelayOnlyAbout() {
	return UseRussianTexts()
		? u"Собеседник не увидит ваш IP-адрес: голос и видео идут через "
			"ретранслятор, а не напрямую. Связь при этом чуть медленнее, а "
			"трафик звонка проходит через Telegram."_q
		: u"The other side never sees your IP address: voice and video go "
			"through a relay instead of straight to them. Calls are a little "
			"slower, and their traffic passes through Telegram."_q;
}

} // namespace NovaGram::Calls
