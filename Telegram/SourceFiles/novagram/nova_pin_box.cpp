/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_pin_box.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "main/main_domain.h"
#include "novagram/nova_keypad.h"
#include "novagram/nova_pin.h"
#include "settings.h"
#include "storage/storage_domain.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/rp_widget.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtWidgets/QLineEdit>

namespace NovaGram {
namespace {

constexpr auto kPromptAnsweredKey = "novagram_pin_prompt_answered"_cs;

enum class EnterMode {
	SetPrimary,
	RemovePrimary,
	SetEmergency,
	RemoveEmergency,
};

[[nodiscard]] bool HasPrimaryPin() {
	return Core::App().domain().local().hasLocalPasscode();
}

[[nodiscard]] bool CheckPrimaryPin(const QString &pin) {
	return Core::App().domain().local().checkPasscode(pin.toUtf8());
}

void ApplyPrimaryPin(const QString &pin) {
	cSetPasscodeBadTries(0);
	Core::App().domain().local().setPasscode(pin.toUtf8());
	Core::App().localPasscodeChanged();
}

[[nodiscard]] QString ModeTitle(EnterMode mode) {
	const auto russian = UseRussianTexts();
	switch (mode) {
	case EnterMode::SetPrimary:
		return russian
			? (HasPrimaryPin() ? u"Смена основного PIN"_q : u"Основной PIN"_q)
			: (HasPrimaryPin() ? u"Change primary PIN"_q : u"Primary PIN"_q);
	case EnterMode::RemovePrimary:
		return russian ? u"Отключение PIN"_q : u"Disable the PIN"_q;
	case EnterMode::SetEmergency:
		return russian ? u"Аварийный PIN"_q : u"Emergency PIN"_q;
	case EnterMode::RemoveEmergency:
		return russian
			? u"Удаление аварийного PIN"_q
			: u"Remove the emergency PIN"_q;
	}
	Unexpected("Mode in NovaGram::ModeTitle.");
}

[[nodiscard]] QString ModeAbout(EnterMode mode) {
	const auto russian = UseRussianTexts();
	switch (mode) {
	case EnterMode::SetPrimary:
		return russian
			? u"Основной PIN состоит из 4–6 цифр и служит ключом шифрования "
				"локальных данных NovaGram. Введённые цифры и длина PIN не "
				"отображаются. Восстановления нет: при потере PIN "
				"потребуется повторный вход в аккаунт."_q
			: u"The primary PIN is 4-6 digits and is the encryption key of "
				"the local NovaGram data. Entered digits and the PIN length "
				"are not displayed. There is no recovery: losing the PIN "
				"means signing in again."_q;
	case EnterMode::RemovePrimary:
		return russian
			? u"После отключения локальные данные перестают быть "
				"зашифрованы отдельным PIN, а аварийный PIN удаляется."_q
			: u"After disabling, local data is no longer encrypted with a "
				"separate PIN and the emergency PIN is removed."_q;
	case EnterMode::SetEmergency:
		return russian
			? u"Аварийный PIN вводится вместо основного под принуждением. "
				"Он не разблокирует NovaGram: вместо этого выполняется выход "
				"из всех аккаунтов и локальное уничтожение данных. Облачная "
				"история Telegram и копии у собеседников не удаляются. "
				"Аварийный PIN должен отличаться от основного."_q
			: u"The emergency PIN is entered instead of the primary one "
				"under coercion. It does not unlock NovaGram: it logs out of "
				"every account and destroys local data instead. The Telegram "
				"cloud history and copies held by other people are not "
				"removed. The emergency PIN must differ from the primary "
				"one."_q;
	case EnterMode::RemoveEmergency:
		return russian
			? u"Подтвердите основной PIN, чтобы удалить аварийный PIN."_q
			: u"Confirm the primary PIN to remove the emergency PIN."_q;
	}
	Unexpected("Mode in NovaGram::ModeAbout.");
}

[[nodiscard]] Ui::PasswordInput *AddPinField(
		not_null<Ui::GenericBox*> box,
		const QString &placeholder) {
	const auto &st = st::defaultInputField;
	auto container = object_ptr<Ui::RpWidget>(box);
	container->resize(container->width(), st.heightMin);
	const auto field = Ui::CreateChild<Ui::PasswordInput>(
		container.data(),
		st,
		rpl::single(placeholder));
	container->geometryValue(
	) | rpl::on_next([=](const QRect &r) {
		field->resize(r.width(), field->height());
		field->moveToLeft(0, 0);
	}, container->lifetime());
	box->addRow(std::move(container));
	field->setEchoMode(QLineEdit::NoEcho);
	field->setMaxLength(kMaxPinLength);
	return field;
}

void EnterBox(not_null<Ui::GenericBox*> box, EnterMode mode) {
	const auto russian = UseRussianTexts();
	box->setTitle(rpl::single(ModeTitle(mode)));

	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(ModeAbout(mode)),
		st::boxLabel));
	Ui::AddSkip(box->verticalLayout());

	const auto needNew = (mode == EnterMode::SetPrimary)
		|| (mode == EnterMode::SetEmergency);
	const auto needCurrent = HasPrimaryPin();

	const auto current = needCurrent
		? AddPinField(box, russian
			? u"Текущий основной PIN"_q
			: u"Current primary PIN"_q)
		: nullptr;
	const auto created = needNew
		? AddPinField(box, (mode == EnterMode::SetEmergency)
			? (russian ? u"Аварийный PIN"_q : u"Emergency PIN"_q)
			: (russian ? u"Новый PIN"_q : u"New PIN"_q))
		: nullptr;
	const auto confirm = needNew
		? AddPinField(box, russian ? u"Повторите PIN"_q : u"Repeat the PIN"_q)
		: nullptr;

	const auto error = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(QString()),
		st::boxLabel));
	error->setTextColorOverride(st::boxTextFgError->c);
	error->hide();

	const auto showError = [=](const QString &text) {
		error->setText(text);
		error->show();
	};

	const auto save = [=] {
		// Reading text() and not getLastText(): the latter is only refreshed
		// from the field's own edit handling, so a value put in by the keypad
		// could still be missing from it.
		const auto currentText = current ? current->text() : QString();
		const auto createdText = created ? created->text() : QString();
		const auto confirmText = confirm ? confirm->text() : QString();
		if (current && !CheckPrimaryPin(currentText)) {
			current->showError();
			showError(WrongPin());
			return;
		}
		if (needNew) {
			if (!ValidPin(createdText)) {
				created->showError();
				showError(InvalidLength());
				return;
			} else if (createdText != confirmText) {
				confirm->showError();
				showError(russian
					? u"Значения PIN не совпали."_q
					: u"The PIN values did not match."_q);
				return;
			}
		}
		switch (mode) {
		case EnterMode::SetPrimary:
			if (CheckEmergencyPin(createdText)) {
				created->showError();
				showError(russian
					? u"Основной PIN должен отличаться от аварийного."_q
					: u"The primary PIN must differ from the emergency."_q);
				return;
			}
			ApplyPrimaryPin(createdText);
			SetPinModeEnabled(true);
			ResetFailedAttempts();
			// The system unlock bypasses both the pin and the emergency pin
			// check, so turning the pin mode on always turns it off.
			Core::App().settings().setSystemUnlockEnabled(false);
			Core::App().saveSettingsDelayed();
			break;
		case EnterMode::RemovePrimary:
			ApplyPrimaryPin(QString());
			SetPinModeEnabled(false);
			Core::App().settings().setSystemUnlockEnabled(false);
			Core::App().saveSettingsDelayed();
			break;
		case EnterMode::SetEmergency:
			if (CheckPrimaryPin(createdText)) {
				created->showError();
				showError(russian
					? u"Аварийный PIN должен отличаться от основного."_q
					: u"The emergency PIN must differ from the primary."_q);
				return;
			}
			SetEmergencyPin(createdText);
			break;
		case EnterMode::RemoveEmergency:
			SetEmergencyPin(QString());
			break;
		}
		box->closeBox();
	};

	const auto active = box->lifetime().make_state<Ui::PasswordInput*>(
		current ? current : created);
	// The focus signal alone is not enough: a field can lose the focus to
	// something that never reports it back, and then the keypad would keep
	// typing into a field the user has already left. The real focus at the
	// moment of the click wins, and the remembered one is only a fallback.
	const auto target = [=] {
		for (const auto field : { current, created, confirm }) {
			if (field && field->hasFocus()) {
				return field;
			}
		}
		return *active;
	};
	for (const auto field : { current, created, confirm }) {
		if (!field) {
			continue;
		}
		QObject::connect(
			field,
			&Ui::MaskedInputField::changed,
			field,
			[=] { error->hide(); });
		QObject::connect(
			field,
			&Ui::MaskedInputField::submitted,
			field,
			[=] { save(); });
		QObject::connect(
			field,
			&Ui::MaskedInputField::focused,
			field,
			[=] { *active = field; });
	}

	if (ShuffledKeypadEnabled()) {
		Ui::AddSkip(box->verticalLayout());
		box->addRow(object_ptr<PinKeypad>(box, PinKeypad::Descriptor{
			.digit = [=](QChar digit) {
				const auto field = target();
				field->setText(field->text() + digit);
				field->setFocusFast();
			},
			.backspace = [=] {
				const auto field = target();
				const auto text = field->text();
				if (!text.isEmpty()) {
					field->setText(text.mid(0, text.size() - 1));
				}
				field->setFocusFast();
			},
			.submit = [=] { save(); },
		}));
	}

	box->addButton(
		rpl::single(russian ? u"Сохранить"_q : u"Save"_q),
		save);
	box->addButton(
		rpl::single(russian ? u"Отмена"_q : u"Cancel"_q),
		[=] { box->closeBox(); });
	box->setFocusCallback([=] {
		if (current) {
			current->setFocusFast();
		} else if (created) {
			created->setFocusFast();
		}
	});
}

} // namespace

void SuggestPinSetup(not_null<Window::SessionController*> controller) {
	if (HasPrimaryPin()
		|| Core::App().settings().readPref<bool>(kPromptAnsweredKey, false)) {
		return;
	}
	// Written before the box appears, so a second window opening at the same
	// time does not produce a second offer, and a crash before the answer does
	// not turn the offer into a recurring nag.
	Core::App().settings().writePref<bool>(kPromptAnsweredKey, true);
	Core::App().saveSettingsDelayed();

	const auto russian = UseRussianTexts();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(PinSettingsTitle()));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(russian
				? u"Локальные данные NovaGram на этом компьютере пока не "
					"защищены отдельным PIN: их читает любой, кто получит "
					"доступ к папке приложения.\n\nОсновной PIN становится "
					"ключом шифрования этих данных, а аварийный PIN вводится "
					"под принуждением и уничтожает их вместо разблокировки. "
					"PIN можно установить позже в разделе «Настройки → "
					"NovaGram»."_q
				: u"The local NovaGram data on this computer is not protected "
					"by a separate PIN yet: anyone with access to the "
					"application folder can read it.\n\nThe primary PIN "
					"becomes the encryption key of that data, and the "
					"emergency PIN is entered under coercion and destroys it "
					"instead of unlocking. The PIN can also be set later in "
					"Settings → NovaGram."_q),
			st::boxLabel));
		box->addButton(
			rpl::single(russian ? u"Установить PIN"_q : u"Set the PIN"_q),
			[=] {
				const auto show = box->uiShow();
				box->closeBox();
				show->showBox(Box(PinSetupBox));
			});
		box->addButton(
			rpl::single(russian ? u"Не устанавливать"_q : u"Not now"_q),
			[=] { box->closeBox(); });
	}));
}

QString PinSettingsTitle() {
	return UseRussianTexts() ? u"PIN NovaGram"_q : u"NovaGram PIN"_q;
}

QString PinSettingsLabel() {
	const auto russian = UseRussianTexts();
	if (!HasPrimaryPin()) {
		return russian ? u"выключен"_q : u"off"_q;
	} else if (HasEmergencyPin()) {
		return russian
			? u"основной и аварийный"_q
			: u"primary and emergency"_q;
	}
	return russian ? u"только основной"_q : u"primary only"_q;
}

void PinSetupBox(not_null<Ui::GenericBox*> box) {
	const auto russian = UseRussianTexts();
	box->setTitle(rpl::single(PinSettingsTitle()));
	box->setWidth(st::boxWideWidth);

	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(russian
			? u"Основной PIN шифрует локальные данные NovaGram и "
				"запрашивается при запуске и после автоблокировки. "
				"Аварийный PIN вводится под принуждением и вместо "
				"разблокировки уничтожает локальные данные."_q
			: u"The primary PIN encrypts the local NovaGram data and is "
				"requested at start and after auto-lock. The emergency PIN "
				"is entered under coercion and destroys local data instead "
				"of unlocking."_q),
		st::boxLabel));
	Ui::AddSkip(box->verticalLayout());

	const auto refreshed = box->lifetime().make_state<rpl::event_stream<>>();
	auto updates = rpl::single(rpl::empty) | rpl::then(refreshed->events());

	const auto show = box->uiShow();
	const auto openEnter = [=](EnterMode mode) {
		show->showBox(Box([=](not_null<Ui::GenericBox*> inner) {
			EnterBox(inner, mode);
			inner->boxClosing() | rpl::on_next([=] {
				refreshed->fire({});
			}, inner->lifetime());
		}));
	};

	const auto addButton = [&](
			Fn<QString()> text,
			Fn<void()> callback,
			const style::SettingsButton &st) {
		const auto button = box->addRow(
			object_ptr<Ui::SettingsButton>(
				box,
				rpl::duplicate(updates) | rpl::map(text),
				st),
			style::margins());
		button->setClickedCallback(callback);
		return button;
	};

	addButton([=] {
		return HasPrimaryPin()
			? (russian ? u"Сменить основной PIN"_q : u"Change primary PIN"_q)
			: (russian ? u"Установить основной PIN"_q : u"Set primary PIN"_q);
	}, [=] {
		openEnter(EnterMode::SetPrimary);
	}, st::settingsButtonNoIcon);

	addButton([=] {
		return !HasPrimaryPin()
			? (russian
				? u"Аварийный PIN (нужен основной PIN)"_q
				: u"Emergency PIN (requires the primary PIN)"_q)
			: HasEmergencyPin()
			? (russian
				? u"Сменить аварийный PIN"_q
				: u"Change emergency PIN"_q)
			: (russian
				? u"Установить аварийный PIN"_q
				: u"Set emergency PIN"_q);
	}, [=] {
		if (!HasPrimaryPin()) {
			show->showBox(Ui::MakeInformBox(russian
				? u"Сначала установите основной PIN."_q
				: u"Set the primary PIN first."_q));
			return;
		}
		openEnter(EnterMode::SetEmergency);
	}, st::settingsButtonNoIcon);

	const auto removeEmergency = addButton([=] {
		return russian
			? u"Удалить аварийный PIN"_q
			: u"Remove emergency PIN"_q;
	}, [=] {
		openEnter(EnterMode::RemoveEmergency);
	}, st::settingsAttentionButton);
	removeEmergency->showOn(rpl::duplicate(
		updates
	) | rpl::map([=] { return HasEmergencyPin(); }));

	const auto removePrimary = addButton([=] {
		return russian ? u"Отключить PIN"_q : u"Disable the PIN"_q;
	}, [=] {
		openEnter(EnterMode::RemovePrimary);
	}, st::settingsAttentionButton);
	removePrimary->showOn(rpl::duplicate(
		updates
	) | rpl::map([=] { return HasPrimaryPin(); }));

	box->addButton(
		rpl::single(russian ? u"Закрыть"_q : u"Close"_q),
		[=] { box->closeBox(); });
}

} // namespace NovaGram
