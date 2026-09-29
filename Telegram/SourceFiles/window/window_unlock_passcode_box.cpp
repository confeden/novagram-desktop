/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_unlock_passcode_box.h"

#include "core/application.h"
#include "lang/lang_keys.h"
#include "lottie/lottie_icon.h"
#include "novagram/nova_pin.h"
#include "settings/cloud_password/settings_cloud_password_common.h"
#include "settings/settings_common.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/vertical_list.h"
#include "window/window_lock_widgets.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_window_lock_widgets.h"

#include <QtWidgets/QLineEdit>

namespace Window {
namespace {

// Both an unlock from another window and the box's own successful submit
// arrive here, because unlockPasscode() clears the lock synchronously and
// fires passcodeLockChanges() from the inside. So the action runs once.
void Finish(
		not_null<Ui::GenericBox*> box,
		not_null<Fn<void()>*> deferred) {
	const auto weak = base::make_weak(box);
	if (const auto onstack = base::take(*deferred)) {
		onstack();
	}
	if (const auto strong = weak.get()) {
		strong->closeBox();
	}
}

void Submit(
		not_null<Ui::GenericBox*> box,
		not_null<Ui::PasswordInput*> field,
		not_null<Ui::FlatLabel*> error,
		Fn<void()> finish) {
	const auto fail = [&](const QString &text) {
		error->setText(text);
		error->show();
	};
	// NovaGram: this box accepts a passcode while the application is locked, so
	// it answers to the fork's pin the same way the lock screen does. Without
	// this it went straight to upstream's check: the emergency pin only
	// reported a wrong passcode, the fork's lockout was neither consulted nor
	// advanced, and a correct entry here left it armed against the next screen.
	const auto entered = field->text();
	const auto clearField = [=] { field->setText(QString()); };
	switch (NovaGram::GatePasscodeEntry(entered, clearField)) {
	case NovaGram::PasscodeGate::EmergencyWipeStarted:
		// The field was cleared inside, before anything was destroyed. This box
		// may already be gone, so nothing of it may be touched here.
		return;
	case NovaGram::PasscodeGate::LockedOut:
		field->setText(QString());
		field->showError();
		fail(NovaGram::LockoutMessage(NovaGram::LockoutRemaining()));
		return;
	case NovaGram::PasscodeGate::Proceed:
		break;
	}
	switch (TryPasscode(entered)) {
	case PasscodeAttempt::Empty:
		field->showError();
		return;
	case PasscodeAttempt::Flood:
		field->showError();
		fail(tr::lng_flood_error(tr::now));
		return;
	case PasscodeAttempt::Wrong:
		NovaGram::NotePasscodeChecked(false);
		// In pin mode a wrong value is cleared rather than left selected: the
		// lock screen does the same, because its keypad appends instead of
		// replacing, and the two screens must not disagree about the pin.
		if (NovaGram::PinModeEnabled()) {
			field->setText(QString());
		} else {
			field->selectAll();
		}
		field->showError();
		fail(NovaGram::PinModeEnabled()
			? NovaGram::WrongPin()
			: tr::lng_passcode_wrong(tr::now));
		return;
	case PasscodeAttempt::Correct:
		break;
	}
	NovaGram::NotePasscodeChecked(true);
	const auto weak = base::make_weak(box);
	Core::App().unlockPasscode();
	if (weak) {
		finish();
	}
}

void UnlockPasscodeBox(
		not_null<Ui::GenericBox*> box,
		UnlockPasscodeBoxStyle st,
		Fn<void()> unlocked) {
	box->setStyle(*st.box);
	box->setWidth(st::boxWideWidth);
	box->setNoContentMargin(true);
	box->addTopButton(*st.close, [=] { box->closeBox(); });

	const auto deferred = box->lifetime().make_state<Fn<void()>>(
		std::move(unlocked));
	const auto finish = [=] { Finish(box, deferred); };
	Core::App().passcodeLockChanges(
	) | rpl::filter(!rpl::mappers::_1) | rpl::on_next([=] {
		finish();
	}, box->lifetime());

	const auto content = box->verticalLayout();
	auto icon = Settings::CreateLottieIcon(
		content,
		{
			.name = u"local_passcode_enter"_q,
			.sizeOverride = st::normalBoxLottieSize,
		},
		st::unlockPasscodeIconPadding);
	content->add(std::move(icon.widget), {}, style::al_top);
	box->setShowFinishedCallback([animate = std::move(icon.animate)] {
		animate(anim::repeat::once);
	});

	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_passcode_check_title(),
			st.box->title),
		st::boxRowPadding + st::unlockPasscodeTitlePadding,
		style::al_top);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::lng_passcode_unlock_about(),
			*st.description),
		st::boxRowPadding,
		style::al_top
	)->setTryMakeSimilarLines(true);

	Ui::AddSkip(content, st::settingLocalPasscodeDescriptionBottomSkip);

	const auto field = Settings::CloudPassword::AddPasswordField(
		content,
		tr::lng_passcode_enter(),
		QString(),
		*st.field);
	if (NovaGram::PinModeEnabled()) {
		// NovaGram: the same field as the lock screen's, for the same reason -
		// the dot echo counts out the digits of the pin to anyone watching, and
		// the fork promises that the entry shows nothing at all.
		field->setEchoMode(QLineEdit::NoEcho);
		field->setMaxLength(NovaGram::kMaxPinLength);
		field->setPlaceholder(rpl::single(NovaGram::PinPlaceholder()));
	}
	const auto error = Settings::CloudPassword::AddError(
		content,
		field,
		*st.error,
		st::boxRowPadding + st::unlockPasscodeErrorPadding);

	const auto submit = [=] { Submit(box, field, error, finish); };
	QObject::connect(field, &Ui::MaskedInputField::submitted, submit);
	box->addButton(tr::lng_passcode_submit(), submit);
	box->setFocusCallback([=] { field->setFocusFast(); });
}

} // namespace

bool ShowUnlockPasscodeBox(
		std::shared_ptr<Ui::Show> show,
		UnlockPasscodeBoxStyle st,
		Fn<void()> unlocked) {
	if (!Core::App().passcodeLocked()) {
		return false;
	}
	show->showBox(Box(UnlockPasscodeBox, st, std::move(unlocked)));
	return true;
}

} // namespace Window
