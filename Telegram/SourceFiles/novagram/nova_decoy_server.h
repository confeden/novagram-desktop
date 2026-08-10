/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/core_types.h"

namespace NovaGram::Decoy {

// Stands in for Telegram while the decoy is on.
//
// MTP::Instance hands every request here instead of the network. Answering a
// handful of them is what lets the real client fill itself with the invented
// account through its own code: it asks for dialogs, stores them, asks for a
// history, stores that too.
//
// An empty result means the request is dropped without a callback, exactly as
// a request that never came back. That is deliberate: replying with an error
// makes parts of the client retry in a loop, while silence is a state it is
// built to sit in.
[[nodiscard]] mtpBuffer Respond(const mtpPrime *from, const mtpPrime *end);

} // namespace NovaGram::Decoy
