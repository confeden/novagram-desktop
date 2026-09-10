/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace NovaGram::IconDesign {

// The application's own mark, drawn rather than shipped as a picture.
//
// Three axes, and the whole point of drawing it is that their product - 3840
// looks - could never be shipped as files. The mark itself is the same in all
// of them: a paper plane in two facets, laid out in a unit box and scaled into
// whatever size is asked for, so the 16 px tray icon and the 256 px window
// icon are the same drawing and not two pictures that drift apart.
//
// Style says what the plate under the plane is - its outline and how the
// accent is used on it. Texture is a pattern painted over that plate, clipped
// to it and kept faint enough to leave the plane readable at 16 px. Accent is
// one of 32 fixed colours: sixteen hues in a deep and a bright tone, each
// picked so that whichever of white or ink the plane is drawn in contrasts
// with the plate by at least 5.7:1 - the colour is chosen by the user, the
// readability is not.
inline constexpr auto kStyles = 10;
inline constexpr auto kTextures = 12;
inline constexpr auto kAccents = 32;

// Style 0, texture 0 and this accent are the stock look: what the fork shipped
// before the picker existed, and what a client that never opens it keeps.
inline constexpr auto kDefaultAccent = 18;

struct Design {
	int style = 0;
	int texture = 0;
	int accent = kDefaultAccent;

	[[nodiscard]] friend inline bool operator==(
		const Design &a,
		const Design &b) = default;
};

[[nodiscard]] Design Current();
void SetCurrent(Design design);

// Fired on every change, so an open settings page and the window icon follow
// without a restart.
[[nodiscard]] rpl::producer<Design> Changes();

// Counts changes. The tray keeps its own cache of the icon, keyed by size
// only, so a design change would otherwise leave the old mark down there until
// a restart; the cache compares this number instead of the whole design.
[[nodiscard]] int Generation();

// True while the design is the stock one, which is the only case where the
// shipped picture is used instead of a drawing.
[[nodiscard]] bool IsOriginal(Design design);

[[nodiscard]] QString SectionTitle();
[[nodiscard]] QString SectionAbout();
[[nodiscard]] QString StyleLabel();
[[nodiscard]] QString TextureLabel();
[[nodiscard]] QString AccentLabel();
[[nodiscard]] QString StyleName(int style);
[[nodiscard]] QString TextureName(int texture);
[[nodiscard]] QString AccentName(int accent);
[[nodiscard]] QColor AccentColor(int accent);

// The mark at any size. `margin` leaves the transparent border the shipped
// icon has, which is what Windows expects of a taskbar icon; the tray and the
// preview ask for it without.
[[nodiscard]] QImage Render(Design design, int size, bool margin);

// The 256 px image the application uses as its own logo, or null while the
// design is the stock one - then the shipped picture is the answer. Cached
// per design, so the callers that ask for it on every notification do not
// redraw it.
[[nodiscard]] const QImage *LogoImage(bool margin);

} // namespace NovaGram::IconDesign
