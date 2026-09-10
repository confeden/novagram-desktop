/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_icon_design.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "novagram/nova_decoy.h"
#include "novagram/nova_pin.h"

#include <QtGui/QLinearGradient>
#include <QtGui/QPainterPath>
#include <QtGui/QRadialGradient>

namespace NovaGram::IconDesign {
namespace {

// One key, four bytes: a version and the three axes. Core::Settings only
// instantiates its preference template for bool and QByteArray, and three
// separate blobs for three small numbers would be worse than one.
constexpr auto kDesignKey = "novagram_icon_design"_cs;
constexpr auto kDesignVersion = char(1);

// Sixteen hues, each in a deep and a bright tone. The deep ones carry a white
// plane, the bright ones an ink one, and which of the two it is never depends
// on taste: PlaneColor picks whichever contrasts more, and the worst pair in
// the table sits at 5.78:1. They are laid out deep-bright-deep-bright so that
// walking the row with one arrow alternates dark and light rather than
// crawling through the hue circle twice.
constexpr uint32 kAccentColors[kAccents] = {
	0xC22525, 0xF39E9E, 0x9F4F1E, 0xEEA377, 0x7A6217, 0xDFAE1B, 0x606B14,
	0xABC018, 0x437015, 0x72CB19, 0x227516, 0x31D01A, 0x16752E, 0x1AD047,
	0x167350, 0x19CD8A, 0x157171, 0x19C8C9, 0x1D6A99, 0x6EBEED, 0x315BD8,
	0x9EB3F3, 0x5F4EDD, 0xB6ACF5, 0x8838D9, 0xCCA5F4, 0xA823BA, 0xE798F2,
	0xB52390, 0xF297DB, 0xBE245E, 0xF39ABC,
};

constexpr auto kInk = 0x12141AU;

std::optional<Design> GlobalCurrent;
int GlobalGeneration/* = 0*/;
rpl::event_stream<Design> GlobalChanges;
base::flat_map<uint64, QImage> GlobalLogos;

[[nodiscard]] QColor Rgb(uint32 value, int alpha = 255) {
	return QColor(
		int((value >> 16) & 0xFF),
		int((value >> 8) & 0xFF),
		int(value & 0xFF),
		alpha);
}

// Toward white for k > 0, toward black for k < 0.
[[nodiscard]] uint32 Shade(uint32 value, float64 k) {
	const auto target = (k > 0.) ? 255. : 0.;
	const auto amount = std::abs(k);
	const auto mix = [&](int shift) {
		const auto channel = float64((value >> shift) & 0xFF);
		return uint32(base::SafeRound(channel + (target - channel) * amount));
	};
	return (mix(16) << 16) | (mix(8) << 8) | mix(0);
}

[[nodiscard]] float64 Luminance(uint32 value) {
	const auto channel = [&](int shift) {
		const auto c = float64((value >> shift) & 0xFF) / 255.;
		return (c <= 0.03928)
			? (c / 12.92)
			: std::pow((c + 0.055) / 1.055, 2.4);
	};
	return 0.2126 * channel(16) + 0.7152 * channel(8) + 0.0722 * channel(0);
}

[[nodiscard]] float64 Contrast(float64 a, float64 b) {
	return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

// White or ink, whichever the plate lets be read. Never a matter of taste.
[[nodiscard]] uint32 PlaneColor(uint32 plate) {
	const auto lum = Luminance(plate);
	return (Contrast(lum, 1.) >= Contrast(lum, Luminance(kInk)))
		? 0xFFFFFFU
		: kInk;
}

[[nodiscard]] uint32 Mix(uint32 a, uint32 b, float64 k) {
	const auto mix = [&](int shift) {
		const auto from = float64((a >> shift) & 0xFF);
		const auto to = float64((b >> shift) & 0xFF);
		return uint32(base::SafeRound(from + (to - from) * k));
	};
	return (mix(16) << 16) | (mix(8) << 8) | mix(0);
}

[[nodiscard]] QPainterPath PlatePath(int style, QRectF box) {
	auto result = QPainterPath();
	const auto side = std::min(box.width(), box.height());
	if (style == 1 || style == 8) {              // Neon, Terminal
		result.addRoundedRect(box, side * 0.28, side * 0.28);
	} else if (style == 5) {                     // Outline
		const auto inset = side * 0.04;
		result.addEllipse(box.adjusted(inset, inset, -inset, -inset));
	} else if (style == 9) {                     // Origami
		const auto center = box.center();
		const auto radius = side / 2.;
		for (auto i = 0; i != 6; ++i) {
			const auto angle = M_PI / 6. + i * M_PI / 3.;
			const auto point = QPointF(
				center.x() + radius * std::cos(angle),
				center.y() + radius * std::sin(angle));
			if (i) {
				result.lineTo(point);
			} else {
				result.moveTo(point);
			}
		}
		result.closeSubpath();
	} else if (style == 2 || style == 7) {       // Retro, Metal
		result.addRoundedRect(box, side * 0.16, side * 0.16);
	} else {
		result.addEllipse(box);
	}
	return result;
}

void PaintPlate(
		QPainter &p,
		int style,
		uint32 accent,
		uint32 second,
		QRectF box,
		const QPainterPath &plate) {
	const auto side = std::min(box.width(), box.height());
	p.save();
	if (style == 0) {                            // Original
		auto gradient = QLinearGradient(box.topLeft(), box.bottomLeft());
		gradient.setColorAt(0., Rgb(Shade(accent, 0.18)));
		gradient.setColorAt(1., Rgb(Shade(accent, -0.14)));
		p.fillPath(plate, gradient);
	} else if (style == 1) {                     // Neon
		p.fillPath(plate, Rgb(0x0B0D12));
		auto pen = QPen(Rgb(accent));
		pen.setWidthF(side * 0.045);
		p.setPen(pen);
		p.drawPath(plate);
		pen.setColor(Rgb(accent, 90));
		pen.setWidthF(side * 0.10);
		p.setPen(pen);
		p.drawPath(plate);
	} else if (style == 2) {                     // Retro
		p.fillPath(plate, Rgb(Shade(accent, 0.62)));
		p.setClipPath(plate);
		for (auto i = 0; i != 4; ++i) {
			p.fillRect(
				QRectF(
					box.left(),
					box.top() + side * (0.58 + i * 0.11),
					box.width(),
					side * 0.08),
				Rgb(Shade(accent, -0.10 - i * 0.06), 140));
		}
		p.setClipping(false);
	} else if (style == 3) {                     // Glass
		auto gradient = QLinearGradient(box.topLeft(), box.bottomRight());
		gradient.setColorAt(0., Rgb(Shade(accent, 0.30)));
		gradient.setColorAt(1., Rgb(Shade(accent, -0.32)));
		p.fillPath(plate, gradient);
		p.setClipPath(plate);
		auto glare = QRadialGradient(
			QPointF(box.left() + side * 0.30, box.top() + side * 0.22),
			side * 0.75);
		glare.setColorAt(0., QColor(255, 255, 255, 150));
		glare.setColorAt(0.45, QColor(255, 255, 255, 26));
		glare.setColorAt(1., QColor(255, 255, 255, 0));
		p.fillRect(box, glare);
		p.setClipping(false);
		auto pen = QPen(QColor(255, 255, 255, 140));
		pen.setWidthF(side * 0.016);
		p.setPen(pen);
		p.drawPath(plate);
	} else if (style == 4) {                     // Mono
		p.fillPath(plate, Rgb(accent));
	} else if (style == 5) {                     // Outline
		auto pen = QPen(Rgb(accent));
		pen.setWidthF(side * 0.075);
		p.setPen(pen);
		p.drawPath(plate);
	} else if (style == 6) {                     // Aurora
		auto gradient = QLinearGradient(box.bottomLeft(), box.topRight());
		gradient.setColorAt(0., Rgb(Shade(accent, -0.35)));
		gradient.setColorAt(0.5, Rgb(accent));
		gradient.setColorAt(1., Rgb(Shade(second, 0.10)));
		p.fillPath(plate, gradient);
	} else if (style == 7) {                     // Metal
		auto gradient = QLinearGradient(box.topLeft(), box.bottomRight());
		gradient.setColorAt(0., Rgb(Shade(accent, 0.55)));
		gradient.setColorAt(0.38, Rgb(Shade(accent, -0.05)));
		gradient.setColorAt(0.5, Rgb(Shade(accent, 0.35)));
		gradient.setColorAt(0.62, Rgb(Shade(accent, -0.32)));
		gradient.setColorAt(1., Rgb(Shade(accent, 0.25)));
		p.fillPath(plate, gradient);
	} else if (style == 8) {                     // Terminal
		p.fillPath(plate, Rgb(0x06070A));
		auto pen = QPen(Rgb(accent, 200));
		pen.setWidthF(side * 0.02);
		p.setPen(pen);
		p.drawPath(plate);
	} else if (style == 9) {                     // Origami
		p.fillPath(plate, Rgb(accent));
		p.setClipPath(plate);
		auto fold = QPainterPath();
		fold.moveTo(box.bottomLeft());
		fold.lineTo(QPointF(box.right(), box.top() + side * 0.25));
		fold.lineTo(box.bottomRight());
		fold.closeSubpath();
		p.fillPath(fold, QColor(0, 0, 0, 51));
		p.setClipping(false);
	}
	p.restore();
}

// Painted twice: over the plate, and again inside the mark. A pattern that
// only ever touched the background changed the picture without changing the
// thing in the middle of it, which is not what picking a texture is for.
void PaintTexture(
		QPainter &p,
		int texture,
		QColor ink,
		QColor ink2,
		QRectF box,
		const QPainterPath &plate) {
	if (!texture) {
		return;
	}
	const auto side = std::min(box.width(), box.height());
	const auto u = side / 16.;
	const auto x0 = box.left();
	const auto y0 = box.top();
	const auto at = [&](float64 x, float64 y) {
		return QPointF(x0 + x * u, y0 + y * u);
	};
	p.save();
	p.setClipPath(plate);
	auto pen = QPen(ink);
	pen.setWidthF(std::max(side * 0.012, 1.));
	if (texture == 1) {                          // Bubbles
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		const auto seed = std::array<std::array<float64, 3>, 8>{ {
			{ 3, 4, 2.4 }, { 9, 3, 1.5 }, { 12, 7, 2.8 }, { 5, 9, 1.9 },
			{ 10, 12, 2.2 }, { 2, 12, 1.3 }, { 14, 11, 1.2 }, { 7, 6, 1.0 },
		} };
		for (const auto &circle : seed) {
			p.drawEllipse(
				at(circle[0], circle[1]),
				circle[2] * u,
				circle[2] * u);
		}
	} else if (texture == 2) {                   // Fabric
		pen.setWidthF(std::max(side * 0.02, 1.));
		p.setPen(pen);
		for (auto i = -16; i < 32; i += 2) {
			p.drawLine(at(i, 0), at(i + 16, 16));
		}
		pen.setColor(ink2);
		p.setPen(pen);
		for (auto i = -16; i < 32; i += 2) {
			p.drawLine(at(i, 16), at(i + 16, 0));
		}
	} else if (texture == 3) {                   // Grain
		p.setPen(Qt::NoPen);
		p.setBrush(ink);
		for (auto i = 0; i != 220; ++i) {
			const auto fract = [](float64 value) {
				return value - std::floor(value);
			};
			const auto x = fract(std::sin(i * 12.9898) * 43758.5453);
			const auto y = fract(std::sin(i * 78.233) * 12345.6789);
			p.drawRect(QRectF(
				x0 + x * side,
				y0 + y * side,
				side * 0.014,
				side * 0.014));
		}
	} else if (texture == 4) {                   // Carbon
		p.setPen(Qt::NoPen);
		p.setBrush(ink2);
		for (auto y = 0; y < 16; y += 2) {
			for (auto x = 0; x < 16; x += 2) {
				const auto shift = (y % 4) ? 1 : 0;
				p.drawRect(QRectF(at(x + shift, y), QSizeF(u, u)));
			}
		}
	} else if (texture == 5) {                   // Stripes
		pen.setWidthF(side * 0.05);
		p.setPen(pen);
		for (auto i = -16; i < 32; i += 3) {
			p.drawLine(at(i, 0), at(i + 16, 16));
		}
	} else if (texture == 6) {                   // Halftone
		p.setPen(Qt::NoPen);
		p.setBrush(ink);
		for (auto y = 1; y < 16; y += 2) {
			for (auto x = 1; x < 16; x += 2) {
				const auto r = (0.18 + 0.34 * (x / 16.)) * u * 1.6;
				p.drawEllipse(at(x, y), r, r);
			}
		}
	} else if (texture == 7) {                   // Grid
		p.setPen(pen);
		for (auto i = 2; i < 16; i += 3) {
			p.drawLine(at(i, 0), at(i, 16));
			p.drawLine(at(0, i), at(16, i));
		}
	} else if (texture == 8) {                   // Waves
		pen.setWidthF(std::max(side * 0.016, 1.));
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		for (auto y = 1; y < 17; y += 3) {
			auto path = QPainterPath();
			for (auto x = 0; x <= 16; ++x) {
				const auto point = at(x, y + std::sin(x * 0.9) * 0.8);
				if (x) {
					path.lineTo(point);
				} else {
					path.moveTo(point);
				}
			}
			p.drawPath(path);
		}
	} else if (texture == 9) {                   // Hex
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		const auto r = u * 1.7;
		for (auto row = -1; row != 7; ++row) {
			for (auto col = -1; col != 7; ++col) {
				const auto cx = col * r * 1.73 + ((row % 2) ? r * 0.87 : 0.);
				const auto cy = row * r * 1.5;
				auto path = QPainterPath();
				for (auto i = 0; i != 6; ++i) {
					const auto angle = M_PI / 6. + i * M_PI / 3.;
					const auto point = QPointF(
						x0 + cx + r * std::cos(angle),
						y0 + cy + r * std::sin(angle));
					if (i) {
						path.lineTo(point);
					} else {
						path.moveTo(point);
					}
				}
				path.closeSubpath();
				p.drawPath(path);
			}
		}
	} else if (texture == 10) {                  // Circuit
		pen.setWidthF(std::max(side * 0.016, 1.));
		p.setPen(pen);
		auto path = QPainterPath();
		path.moveTo(at(2, 3));
		path.lineTo(at(7, 3));
		path.lineTo(at(7, 8));
		path.lineTo(at(13, 8));
		path.moveTo(at(4, 12));
		path.lineTo(at(11, 12));
		path.lineTo(at(11, 5));
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
		p.setPen(Qt::NoPen);
		p.setBrush(ink);
		const auto pads = std::array<QPointF, 7>{ {
			at(2, 3), at(7, 3), at(7, 8), at(13, 8),
			at(4, 12), at(11, 12), at(11, 5),
		} };
		for (const auto &pad : pads) {
			p.drawEllipse(pad, u * 0.32, u * 0.32);
		}
	} else if (texture == 11) {                  // Scales
		pen.setWidthF(std::max(side * 0.014, 1.));
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		for (auto row = 0; row != 8; ++row) {
			for (auto col = -1; col != 8; ++col) {
				const auto cx = (col * 2 + ((row % 2) ? 1 : 0)) * u;
				const auto cy = row * 1.6 * u;
				const auto r = u * 1.15;
				p.drawArc(
					QRectF(x0 + cx - r, y0 + cy - r, r * 2, r * 2),
					180 * 16,
					180 * 16);
			}
		}
	}
	p.restore();
}

struct PlanePaths {
	QPainterPath wing;
	QPainterPath fold;
	QPainterPath whole;
};

// The colour of the mark for a style, and what sits under it: the texture pass
// inside the mark paints the plate showing through it, so it needs both.
struct PlaneColors {
	uint32 main = 0;
	uint32 plate = 0;
};

[[nodiscard]] PlanePaths MakePlanePaths(QRectF box) {
	const auto side = std::min(box.width(), box.height());
	const auto inner = side * 0.58;
	const auto left = box.left() + (box.width() - inner) / 2.;
	const auto top = box.top() + (box.height() - inner) / 2.;
	const auto at = [&](float64 x, float64 y) {
		return QPointF(left + x * inner, top + y * inner);
	};
	auto result = PlanePaths();
	result.wing.moveTo(at(0.02, 0.52));
	result.wing.lineTo(at(0.98, 0.04));
	result.wing.lineTo(at(0.42, 0.66));
	result.wing.closeSubpath();
	result.fold.moveTo(at(0.42, 0.66));
	result.fold.lineTo(at(0.98, 0.04));
	result.fold.lineTo(at(0.58, 0.98));
	result.fold.closeSubpath();
	result.whole = result.wing.united(result.fold);
	return result;
}

[[nodiscard]] PlaneColors MakePlaneColors(int style, uint32 accent) {
	auto plate = accent;
	auto main = PlaneColor(accent);
	if (style == 1 || style == 8) {              // Neon, Terminal
		main = accent;
		plate = 0x0B0D12;
	} else if (style == 5) {                     // Outline
		main = accent;
		plate = 0x1B1D21;
	} else if (style == 2) {                     // Retro
		main = 0x2A2622;
		plate = Shade(accent, 0.62);
	}
	return { main, plate };
}

void PaintPlane(
		QPainter &p,
		int style,
		PlaneColors colors,
		const PlanePaths &paths) {
	p.save();
	p.setPen(Qt::NoPen);
	if (style == 4) {
		// Cutout takes the mark out of the plate instead of drawing it on top.
		p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
		p.setBrush(QColor(0, 0, 0, 255));
		p.drawPath(paths.wing);
		p.setBrush(QColor(0, 0, 0, 140));
		p.drawPath(paths.fold);
	} else {
		p.setBrush(Rgb(colors.main));
		p.drawPath(paths.wing);
		p.setBrush(Rgb(Mix(colors.main, colors.plate, 0.42)));
		p.drawPath(paths.fold);
	}
	p.restore();
}

[[nodiscard]] Design Clamped(Design design) {
	return {
		std::clamp(design.style, 0, kStyles - 1),
		std::clamp(design.texture, 0, kTextures - 1),
		std::clamp(design.accent, 0, kAccents - 1),
	};
}

} // namespace

Design Current() {
	if (!GlobalCurrent) {
		const auto blob = Core::App().settings().readPref<QByteArray>(
			kDesignKey,
			QByteArray());
		GlobalCurrent = ((blob.size() == 4) && (blob[0] == kDesignVersion))
			? Clamped({ int(blob[1]), int(blob[2]), int(blob[3]) })
			: Design();
	}
	return *GlobalCurrent;
}

void SetCurrent(Design design) {
	design = Clamped(design);
	if (GlobalCurrent && (*GlobalCurrent == design)) {
		return;
	}
	GlobalCurrent = design;
	GlobalLogos.clear();
	++GlobalGeneration;
	auto blob = QByteArray(4, char(0));
	blob[0] = kDesignVersion;
	blob[1] = char(design.style);
	blob[2] = char(design.texture);
	blob[3] = char(design.accent);
	Core::App().settings().writePref<QByteArray>(kDesignKey, blob);
	Core::App().saveSettingsDelayed();
	GlobalChanges.fire_copy(design);
}

rpl::producer<Design> Changes() {
	return GlobalChanges.events();
}

int Generation() {
	return GlobalGeneration;
}

bool IsOriginal(Design design) {
	return (design == Design());
}

QColor AccentColor(int accent) {
	return Rgb(kAccentColors[std::clamp(accent, 0, kAccents - 1)]);
}

QImage Render(Design design, int size, bool margin) {
	design = Clamped(design);
	if (size < 1) {
		size = 1;
	}
	auto result = QImage(size, size, QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);
	const auto inset = margin ? (size * 0.055) : (size * 0.01);
	const auto box = QRectF(
		inset,
		inset,
		size - 2 * inset,
		size - 2 * inset);
	const auto accent = kAccentColors[design.accent];
	// Aurora needs a second colour, and it is taken from the palette itself
	// rather than computed: nine steps along keeps the two hues apart without
	// ever landing outside the readable set.
	const auto second = kAccentColors[(design.accent + 9) % kAccents];
	auto p = QPainter(&result);
	p.setRenderHint(QPainter::Antialiasing, true);
	const auto plate = PlatePath(design.style, box);
	const auto dark = (design.style == 1 || design.style == 8);
	PaintPlate(p, design.style, accent, second, box, plate);
	// Over the plate, and strong enough to be seen at a glance: at the alpha
	// this started with, the whole row of textures looked alike.
	PaintTexture(
		p,
		design.texture,
		dark ? Rgb(accent, 105) : QColor(255, 255, 255, 92),
		dark ? Rgb(accent, 64) : QColor(0, 0, 0, 64),
		box,
		plate);
	const auto paths = MakePlanePaths(box);
	const auto colors = MakePlaneColors(design.style, accent);
	PaintPlane(p, design.style, colors, paths);
	if (design.style != 4) {
		// And inside the mark: the plate colour painted through the plane, so
		// the texture reads as the mark being made of it rather than as
		// wallpaper behind it. Cutout is skipped - its mark is a hole, and
		// painting into a hole fills it.
		PaintTexture(
			p,
			design.texture,
			Rgb(colors.plate, 190),
			Rgb(colors.plate, 130),
			box,
			paths.whole);
	}
	p.end();
	return result;
}

const QImage *LogoImage(bool margin) {
	if (Decoy::Active()) {
		// The decoy has to look like plain Telegram, and an icon nobody else
		// has is a sign of the fork - I4. The chosen design is kept and comes
		// back with the real session; the decoy simply never asks for it.
		return nullptr;
	}
	const auto design = Current();
	if (IsOriginal(design)) {
		return nullptr;
	}
	const auto key = uint64(margin ? 1 : 0);
	const auto i = GlobalLogos.find(key);
	if (i != end(GlobalLogos)) {
		return &i->second;
	}
	return &GlobalLogos.emplace(key, Render(design, 256, margin)).first->second;
}

QString SectionTitle() {
	return UseRussianTexts() ? u"Значок"_q : u"Icon"_q;
}

QString SectionAbout() {
	return UseRussianTexts()
		? u"Значок рисуется, а не берётся из файла, поэтому сочетаний "
			"3840. Меняется значок окна, панели задач и трея; значок в самом "
			".exe и ярлык, созданный установщиком, остаются прежними — их "
			"Windows берёт из файла, а не у работающей программы."_q
		: u"The icon is drawn rather than taken from a file, which is why "
			"there are 3840 combinations. It changes the window, the taskbar "
			"and the tray; the icon inside the .exe and the shortcut the "
			"installer made stay as they were - Windows takes those from the "
			"file, not from the running program."_q;
}

QString StyleLabel() {
	return UseRussianTexts() ? u"Стиль"_q : u"Style"_q;
}

QString TextureLabel() {
	return UseRussianTexts() ? u"Текстура"_q : u"Texture"_q;
}

QString AccentLabel() {
	return UseRussianTexts() ? u"Оттенок"_q : u"Accent"_q;
}

QString StyleName(int style) {
	const auto russian = UseRussianTexts();
	switch (std::clamp(style, 0, kStyles - 1)) {
	case 0: return russian ? u"Оригинал"_q : u"Original"_q;
	case 1: return russian ? u"Неон"_q : u"Neon"_q;
	case 2: return russian ? u"Ретро"_q : u"Retro"_q;
	case 3: return russian ? u"Стекло"_q : u"Glass"_q;
	case 4: return russian ? u"Вырез"_q : u"Cutout"_q;
	case 5: return russian ? u"Контур"_q : u"Outline"_q;
	case 6: return russian ? u"Сияние"_q : u"Aurora"_q;
	case 7: return russian ? u"Металл"_q : u"Metal"_q;
	case 8: return russian ? u"Терминал"_q : u"Terminal"_q;
	}
	return russian ? u"Оригами"_q : u"Origami"_q;
}

QString TextureName(int texture) {
	const auto russian = UseRussianTexts();
	switch (std::clamp(texture, 0, kTextures - 1)) {
	case 0: return russian ? u"Без текстуры"_q : u"Plain"_q;
	case 1: return russian ? u"Пузыри"_q : u"Bubbles"_q;
	case 2: return russian ? u"Ткань"_q : u"Fabric"_q;
	case 3: return russian ? u"Зерно"_q : u"Grain"_q;
	case 4: return russian ? u"Карбон"_q : u"Carbon"_q;
	case 5: return russian ? u"Полосы"_q : u"Stripes"_q;
	case 6: return russian ? u"Растр"_q : u"Halftone"_q;
	case 7: return russian ? u"Сетка"_q : u"Grid"_q;
	case 8: return russian ? u"Волны"_q : u"Waves"_q;
	case 9: return russian ? u"Соты"_q : u"Hex"_q;
	case 10: return russian ? u"Плата"_q : u"Circuit"_q;
	}
	return russian ? u"Чешуя"_q : u"Scales"_q;
}

QString AccentName(int accent) {
	return QString::number(std::clamp(accent, 0, kAccents - 1) + 1)
		+ u" / "_q
		+ QString::number(kAccents);
}

} // namespace NovaGram::IconDesign
