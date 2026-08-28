/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_stories.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "novagram/nova_pin.h"

namespace NovaGram {
namespace {

// Prefixed like the fork's other nineteen keys rather than named `stories_hidden`
// after the Android half: on Android that name sits in its own privacy file, here
// it would share one blob with upstream's settings, where an unprefixed word is
// the one that can collide. The default and the behaviour are what have to match
// across the two platforms, and they do.
constexpr auto kHiddenKey = "novagram_stories_hidden"_cs;

// The answer is wanted once per avatar per frame while the chat list scrolls,
// and reading a preference builds a QByteArray out of the key every time. Held
// here instead; SetStoriesHidden is the only thing that can change it while the
// process lives, and it writes both.
std::optional<bool> GlobalHidden;

rpl::event_stream<bool> GlobalChanges;

} // namespace

bool StoriesHidden() {
	if (!GlobalHidden) {
		GlobalHidden = Core::App().settings().readPref<bool>(
			kHiddenKey,
			false);
	}
	return *GlobalHidden;
}

void SetStoriesHidden(bool hidden) {
	if (GlobalHidden && (*GlobalHidden == hidden)) {
		return;
	}
	GlobalHidden = hidden;
	Core::App().settings().writePref<bool>(kHiddenKey, hidden);
	Core::App().saveSettingsDelayed();
	GlobalChanges.fire_copy(hidden);
}

rpl::producer<bool> StoriesHiddenChanges() {
	return GlobalChanges.events();
}

QString StoriesHiddenTitle() {
	return UseRussianTexts()
		? u"Скрыть истории"_q
		: u"Hide stories"_q;
}

QString StoriesHiddenAbout() {
	// Two sentences, per the house rule for these subtitles: what goes off the
	// screen, and the one limit that would otherwise be discovered as a bug.
	return UseRussianTexts()
		? u"Убирает строку историй над списком чатов, кольца вокруг аватарок и "
			"вход в истории. Истории продолжают приходить — прячется только "
			"показ, ссылка на историю по-прежнему открывается."_q
		: u"Removes the stories strip above the chat list, the rings around "
			"userpics and the entry points. Stories still arrive - only the "
			"display is hidden, and a story link still opens."_q;
}

} // namespace NovaGram
