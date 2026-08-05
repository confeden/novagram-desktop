/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <memory>

class PeerData;

namespace Ui {
class GenericBox;
class Show;
} // namespace Ui

namespace NovaGram {

[[nodiscard]] QString EraseMenuText();

void EraseEvidenceBox(
	not_null<Ui::GenericBox*> box,
	std::shared_ptr<Ui::Show> show,
	not_null<PeerData*> peer);

} // namespace NovaGram
