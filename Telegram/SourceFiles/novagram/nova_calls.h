/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class QString;

namespace NovaGram::Calls {

// Never let the media of a call go straight to the other side.
//
// Telegram already has a privacy setting for this, but the client cannot keep
// it: whether a call may go peer to peer is decided by the server, which sends
// the answer as phoneCall.p2p_allowed, and both stock clients hand that flag
// straight to tgcalls. So the setting is a request, not a guarantee - the one
// party who is not asked is the client of the person it protects.
//
// This is the guarantee. It is ANDed with the server's flag at the single place
// where the call instance is built, so a "yes" from the server cannot bring
// direct connections back. In tgcalls enableP2P = false means the port
// allocator gets PORTALLOCATOR_DISABLE_UDP and PORTALLOCATOR_DISABLE_STUN and
// loses CF_REFLEXIVE, so only relay candidates are ever gathered - the same
// thing WebRTC calls iceTransportPolicy: relay. Nothing has to be agreed with
// the other side: ICE simply settles on a relay pair.
//
// The price is honest and small: every call goes through a Telegram reflector,
// which costs some latency and gives Telegram the traffic it would have had
// anyway for every call between people who are not in each other's contacts.
[[nodiscard]] bool RelayOnly();
void SetRelayOnly(bool value);

// Whether an address the server handed to the call stack may be used at all.
//
// Reflector addresses arrive as numeric IPs and there is no DNS anywhere in a
// normal call. But nothing checks that: tgcalls takes the string as it is and
// wraps it in rtc::SocketAddress, and a name would be left unresolved and
// handed to getaddrinfo - the system resolver, in the clear, past every
// promise this fork makes about DNS. One address the server chooses is enough
// to learn that this account is in a call and where from.
//
// So a non-numeric address is dropped rather than resolved. Doing it the other
// way round - resolving it over DoH - would keep the leak of the fact and only
// hide the name.
[[nodiscard]] bool AcceptableCallAddress(const QString &address);

[[nodiscard]] QString RelayOnlyTitle();
[[nodiscard]] QString RelayOnlyAbout();

} // namespace NovaGram::Calls
