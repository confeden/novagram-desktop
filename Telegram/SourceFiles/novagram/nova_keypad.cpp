/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_keypad.h"

#include "base/random.h"
#include "novagram/nova_pin.h"
#include "styles/style_layers.h"
#include "styles/style_passcode_box.h"
#include "styles/style_window_lock_widgets.h"

#include <QtGui/QPainter>
#include <QtGui/QMouseEvent>

#include <utility>

namespace NovaGram {
namespace {

constexpr auto kColumns = 3;
constexpr auto kRows = 4;
constexpr auto kCells = kColumns * kRows;
constexpr auto kDigits = 10;
constexpr auto kBackspaceCell = 9;
constexpr auto kLastDigitCell = 10;
constexpr auto kSubmitCell = 11;

[[nodiscard]] int Spacing() {
	return st::passcodeLittleSkip;
}

[[nodiscard]] int CellHeight() {
	return st::passcodeSubmit.height;
}

// Nine digits fill the first three rows and the tenth one sits in the middle
// of the last row, between the erase and the confirm cells, so the layout
// matches the Android keypad even after the digits are shuffled.
[[nodiscard]] int DigitSlot(int index) {
	return (index < kBackspaceCell)
		? index
		: (index == kLastDigitCell)
		? (kDigits - 1)
		: -1;
}

[[nodiscard]] QString BackspaceLabel() {
	return UseRussianTexts() ? u"Стереть"_q : u"Erase"_q;
}

[[nodiscard]] QString SubmitLabel() {
	return UseRussianTexts() ? u"Ввод"_q : u"Enter"_q;
}

} // namespace

PinKeypad::PinKeypad(QWidget *parent, Descriptor descriptor)
: RpWidget(parent)
, _descriptor(std::move(descriptor)) {
	// The keypad must never take the focus away from the field it types into:
	// otherwise the field stops being the focused one and the code that
	// decides where a digit goes picks the wrong target.
	setFocusPolicy(Qt::NoFocus);
	setMouseTracking(true);
	shuffle();
}

void PinKeypad::shuffle() {
	for (auto i = 0; i != int(_digits.size()); ++i) {
		_digits[i] = QChar('0' + i);
	}
	for (auto i = int(_digits.size()) - 1; i > 0; --i) {
		std::swap(_digits[i], _digits[base::RandomIndex(i + 1)]);
	}
	update();
}

int PinKeypad::resizeGetHeight(int newWidth) {
	return kRows * CellHeight() + (kRows - 1) * Spacing();
}

QRect PinKeypad::cellRect(int index) const {
	const auto column = index % kColumns;
	const auto row = index / kColumns;
	const auto spacing = Spacing();
	const auto full = width() + spacing;
	const auto left = column * full / kColumns;
	const auto right = (column + 1) * full / kColumns - spacing;
	const auto top = row * (CellHeight() + spacing);
	return QRect(left, top, right - left, CellHeight());
}

int PinKeypad::cellAt(QPoint position) const {
	for (auto i = 0; i != kCells; ++i) {
		if (cellRect(i).contains(position)) {
			return i;
		}
	}
	return -1;
}

void PinKeypad::setSelected(int index) {
	if (_selected == index) {
		return;
	}
	_selected = index;
	update();
}

void PinKeypad::activate(int index) {
	const auto slot = DigitSlot(index);
	if (slot >= 0) {
		if (_descriptor.digit) {
			_descriptor.digit(_digits[slot]);
		}
	} else if (index == kBackspaceCell) {
		if (_descriptor.backspace) {
			_descriptor.backspace();
		}
	} else if (index == kSubmitCell) {
		if (_descriptor.submit) {
			_descriptor.submit();
		}
	}
}

void PinKeypad::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);

	for (auto i = 0; i != kCells; ++i) {
		const auto rect = cellRect(i);
		if (!rect.intersects(e->rect())) {
			continue;
		}
		const auto active = (_pressed == i)
			|| ((_pressed < 0) && (_selected == i));
		p.setBrush(active ? st::windowBgRipple : st::windowBgOver);
		p.drawRoundedRect(rect, st::boxRadius, st::boxRadius);

		const auto slot = DigitSlot(i);
		p.setPen((i == kSubmitCell) ? st::windowActiveTextFg : st::windowFg);
		if (slot >= 0) {
			p.setFont(st::passcodeHeaderFont);
			p.drawText(rect, QString(_digits[slot]), style::al_center);
		} else {
			p.setFont(st::boxTextFont);
			p.drawText(
				rect,
				(i == kBackspaceCell) ? BackspaceLabel() : SubmitLabel(),
				style::al_center);
		}
		p.setPen(Qt::NoPen);
	}
}

void PinKeypad::mouseMoveEvent(QMouseEvent *e) {
	setSelected(cellAt(e->pos()));
}

void PinKeypad::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_pressed = cellAt(e->pos());
	update();
}

void PinKeypad::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = _pressed;
	_pressed = -1;
	update();
	if (pressed >= 0 && pressed == cellAt(e->pos())) {
		activate(pressed);
	}
}

void PinKeypad::leaveEventHook(QEvent *e) {
	setSelected(-1);
}

} // namespace NovaGram
