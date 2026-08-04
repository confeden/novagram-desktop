/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace NovaGram {

void PinSetupBox(not_null<Ui::GenericBox*> box);

// Offers the pin right after a login, once per installation. Does nothing if
// a pin already exists or if the offer has been answered before.
void SuggestPinSetup(not_null<Window::SessionController*> controller);

[[nodiscard]] QString PinSettingsTitle();
[[nodiscard]] QString PinSettingsLabel();

} // namespace NovaGram
