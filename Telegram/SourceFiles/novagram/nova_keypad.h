/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rp_widget.h"

#include <array>

namespace NovaGram {

class PinKeypad final : public Ui::RpWidget {
public:
	struct Descriptor {
		Fn<void(QChar digit)> digit;
		Fn<void()> backspace;
		Fn<void()> submit;
	};

	PinKeypad(QWidget *parent, Descriptor descriptor);

	void shuffle();

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	[[nodiscard]] QRect cellRect(int index) const;
	[[nodiscard]] int cellAt(QPoint position) const;
	void setSelected(int index);
	void activate(int index);

	const Descriptor _descriptor;
	std::array<QChar, 10> _digits;
	int _selected = -1;
	int _pressed = -1;

};

} // namespace NovaGram
