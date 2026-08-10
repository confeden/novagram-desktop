/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "ui/effects/animations.h"
#include "ui/text/text.h"
#include "ui/widgets/buttons.h"

namespace NovaGram {

// A settings row whose label scrolls while the cursor is over it, so a title
// too long for the row can be read to the end without opening anything.
//
// Upstream elides such a label and leaves no way to see the rest of it. The
// natural place for this would be Ui::SettingsButton itself, but that class
// lives in the lib_ui submodule, which upstream moves on every release, so the
// behaviour is added here as a subclass instead of as a patch that would have
// to be carried forward for ever.
class MarqueeButton final : public Ui::SettingsButton {
public:
	MarqueeButton(
		QWidget *parent,
		const QString &text,
		const style::SettingsButton &st);

protected:
	void paintEvent(QPaintEvent *e) override;
	void onStateChanged(State was, StateChangeSource source) override;

private:
	[[nodiscard]] int availableWidth() const;
	[[nodiscard]] int overflow() const;
	void startScrolling();

	Ui::Text::String _label;
	Ui::Animations::Simple _scroll;
	base::Timer _delay;
	bool _over = false;

};

} // namespace NovaGram
