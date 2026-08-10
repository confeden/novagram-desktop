/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_marquee_button.h"

#include "ui/painter.h"

namespace NovaGram {
namespace {

// Slow enough to read, and the same speed for every row: a duration computed
// from the overflow means a barely clipped label crawls while a badly clipped
// one races, which reads as two different animations.
constexpr auto kPixelsPerSecond = 48;

// Nothing starts while the cursor is only passing through. Without this every
// row twitches as the mouse travels down the list.
constexpr auto kStartDelay = crl::time(350);

} // namespace

MarqueeButton::MarqueeButton(
	QWidget *parent,
	const QString &text,
	const style::SettingsButton &st)
: Ui::SettingsButton(parent, rpl::single(text), st) {
	_label.setText(st.style, text);
	_delay.setCallback([=] { startScrolling(); });
}

int MarqueeButton::availableWidth() const {
	auto result = width() - st().padding.left() - st().padding.right();
	const auto toggle = maybeToggleRect();
	if (!toggle.isEmpty()) {
		result -= (width() - toggle.x());
	}
	return result;
}

int MarqueeButton::overflow() const {
	return std::max(_label.maxWidth() - availableWidth(), 0);
}

void MarqueeButton::startScrolling() {
	const auto distance = overflow();
	if (!_over || !distance) {
		return;
	}
	const auto duration = crl::time(
		(distance * crl::time(1000)) / kPixelsPerSecond);
	_scroll.start([=] { update(); }, 0., 1., duration, anim::linear);
}

void MarqueeButton::onStateChanged(State was, StateChangeSource source) {
	Ui::SettingsButton::onStateChanged(was, source);

	const auto over = isOver() && !isDisabled();
	if (_over == over) {
		return;
	}
	_over = over;
	if (over) {
		if (overflow()) {
			_delay.callOnce(kStartDelay);
		}
	} else {
		// Instantly, on purpose: scrolling back would take as long as
		// scrolling forward did, and the row would keep moving after the
		// cursor has already left it.
		_delay.cancel();
		_scroll.stop();
		update();
	}
}

void MarqueeButton::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);

	const auto over = (isOver() || isDown()) && !isDisabled();
	paintBg(p, e->rect(), over);
	paintRipple(p, 0, 0);

	const auto available = availableWidth();
	if (available > 0) {
		p.setPen(over ? st().textFgOver : st().textFg);
		const auto left = st().padding.left();
		const auto top = st().padding.top();
		const auto shift = int(base::SafeRound(
			overflow() * _scroll.value(_over ? 1. : 0.)));
		if (!shift) {
			_label.drawLeftElided(p, left, top, available, width());
		} else {
			p.setClipRect(left, 0, available, height());
			_label.drawLeft(
				p,
				left - shift,
				top,
				_label.maxWidth(),
				width());
			p.setClipping(false);
		}
	}

	paintToggle(p, width());
}

} // namespace NovaGram
