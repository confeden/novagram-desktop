/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace NovaGram {

// Whether this client draws anything about stories.
//
// Off by default, unlike every other switch in the fork. The others take away
// something the user never sees - a request, a header, a piece of metadata -
// while this one removes a working part of Telegram from the screen, so it has
// to be chosen rather than inherited. The way back is the same row that
// switched it on, which is what I8 asks of anything that hides.
//
// Nothing here touches the network. Stories are still delivered and still
// counted, a t.me story link handed to the client still opens, and the separate
// promise that a hidden dialog does not have its story views reported is
// decided by NovaGram::ReadStatus and is unaffected either way.
[[nodiscard]] bool StoriesHidden();
void SetStoriesHidden(bool hidden);

// Fired on every change, so the chat list can drop the strip without a restart.
[[nodiscard]] rpl::producer<bool> StoriesHiddenChanges();

[[nodiscard]] QString StoriesHiddenTitle();
[[nodiscard]] QString StoriesHiddenAbout();

} // namespace NovaGram
