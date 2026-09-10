/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_lock_widgets.h"

#include "base/platform/base_platform_info.h"
#include "base/call_delayed.h"
#include "base/system_unlock.h"
#include "lang/lang_keys.h"
#include "novagram/nova_device_lock.h"
#include "novagram/nova_keypad.h"
#include "novagram/nova_pin.h"
#include "storage/storage_domain.h"
#include "mainwindow.h"
#include "core/application.h"
#include "api/api_text_entities.h"
#include "ui/boxes/confirm_box.h"
#include "ui/text/text.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/toast/toast.h"
#include "ui/ui_utility.h"
#include "window/window_controller.h"
#include "window/window_slide_animation.h"
#include "window/window_session_controller.h"
#include "main/main_domain.h"
#include "styles/style_layers.h"
#include "styles/style_passcode_box.h"
#include "styles/style_window_lock_widgets.h"

namespace Window {
namespace {

constexpr auto kSystemUnlockDelay = crl::time(1000);

} // namespace

PasscodeAttempt TryPasscode(const QString &passcode) {
	if (passcode.isEmpty()) {
		return PasscodeAttempt::Empty;
	} else if (!passcodeCanTry()) {
		return PasscodeAttempt::Flood;
	}
	const auto utf8 = passcode.toUtf8();
	auto &domain = Core::App().domain();
	const auto correct = domain.started()
		? domain.local().checkPasscode(utf8)
		: (domain.start(utf8) == Storage::StartResult::Success);
	if (!correct) {
		cSetPasscodeBadTries(cPasscodeBadTries() + 1);
		cSetPasscodeLastTry(crl::now());
		return PasscodeAttempt::Wrong;
	}
	return PasscodeAttempt::Correct;
}

LockWidget::LockWidget(QWidget *parent, not_null<Controller*> window)
: RpWidget(parent)
, _window(window) {
	show();
}

LockWidget::~LockWidget() = default;

not_null<Controller*> LockWidget::window() const {
	return _window;
}

void LockWidget::setInnerFocus() {
	setFocus();
}

void LockWidget::showAnimated(QPixmap oldContentCache) {
	_showAnimation = nullptr;

	showChildren();
	setInnerFocus();
	auto newContentCache = Ui::GrabWidget(this);
	hideChildren();

	_showAnimation = std::make_unique<Window::SlideAnimation>();
	_showAnimation->setRepaintCallback([=] { update(); });
	_showAnimation->setFinishedCallback([=] { showFinished(); });
	_showAnimation->setPixmaps(oldContentCache, newContentCache);
	_showAnimation->start();

	show();
}

void LockWidget::showFinished() {
	showChildren();
	_window->widget()->setInnerFocus();
	_showAnimation = nullptr;
	if (const auto controller = _window->sessionController()) {
		controller->clearSectionStack();
	}
}

void LockWidget::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);

	if (_showAnimation) {
		_showAnimation->paintContents(p);
		return;
	}
	paintContent(p);
}

void LockWidget::paintContent(QPainter &p) {
	p.fillRect(rect(), st::windowBg);
}

PasscodeLockWidget::PasscodeLockWidget(
	QWidget *parent,
	not_null<Controller*> window)
: LockWidget(parent, window)
, _passcode(this, st::passcodeInput, tr::lng_passcode_ph())
, _submit(this, tr::lng_passcode_submit(), st::passcodeSubmit)
, _logout(this, tr::lng_passcode_logout(tr::now)) {
	connect(_passcode, &Ui::MaskedInputField::changed, [=] { changed(); });
	connect(_passcode, &Ui::MaskedInputField::submitted, [=] { submit(); });
	_submit->setClickedCallback([=] { submit(); });
	_logout->setClickedCallback([=] {
		window->showLogoutConfirmation();
	});

	_novaDeviceBlocked = NovaGram::DeviceLock::Blocked();
	if (_novaDeviceBlocked) {
		// Nothing below applies: there is no key on this machine, so neither
		// a passcode nor Windows Hello can lead anywhere.
		setupNovaDeviceBlocked();
		return;
	}

	_novaPinMode = NovaGram::PinModeEnabled();
	if (_novaPinMode) {
		setupNovaPinMode();
	}

	using namespace rpl::mappers;
	// The system unlock would let anyone who can pass Windows Hello in and
	// skip the emergency pin check altogether, which defeats the whole point
	// of the NovaGram pin, so in that mode it is never even offered.
	if (!_novaPinMode && Core::App().settings().systemUnlockEnabled()) {
		_systemUnlockAvailable = base::SystemUnlockStatus(
			true
		) | rpl::map([](base::SystemUnlockAvailability status) {
			return status.withBiometrics
				? SystemUnlockType::Biometrics
				: status.withCompanion
				? SystemUnlockType::Companion
				: status.available
				? SystemUnlockType::Default
				: SystemUnlockType::None;
		});
		if (Core::App().domain().started()) {
			_systemUnlockAllowed = _systemUnlockAvailable.value();
			setupSystemUnlock();
		} else {
			setupSystemUnlockInfo();
		}
	}
}

void PasscodeLockWidget::setupNovaPinMode() {
	_passcode->setEchoMode(QLineEdit::NoEcho);
	_passcode->setMaxLength(NovaGram::kMaxPinLength);
	_passcode->setPlaceholder(rpl::single(NovaGram::PinPlaceholder()));
	_submit->setText(rpl::single(NovaGram::SubmitButton()));

	_novaHint = Ui::CreateChild<Ui::FlatLabel>(
		this,
		NovaGram::HiddenInputHint(),
		st::passcodeSystemUnlockLater);
	if (NovaGram::ShuffledKeypadEnabled()) {
		_novaKeypad = Ui::CreateChild<NovaGram::PinKeypad>(
			this,
			NovaGram::PinKeypad::Descriptor{
				.digit = [=](QChar digit) {
					_passcode->setText(_passcode->text() + digit);
					_passcode->setFocusFast();
				},
				.backspace = [=] {
					const auto text = _passcode->text();
					if (!text.isEmpty()) {
						_passcode->setText(text.mid(0, text.size() - 1));
					}
					_passcode->setFocusFast();
				},
				.submit = [=] { submit(); },
			});
	}
	_novaLockoutTimer.setCallback([=] { refreshNovaLockout(); });
	refreshNovaLockout();
}

void PasscodeLockWidget::setupNovaDeviceBlocked() {
	// Destroyed, not hidden: LockWidget::showFinished() and showAnimated()
	// both end in showChildren(), which sets *every* child visible again, so a
	// hidden passcode field would come back the moment the screen is shown.
	// Logging out is gone for a different reason - it needs a session, and the
	// account record on this disk was never opened.
	_passcode.destroy();
	_submit.destroy();
	_logout.destroy();

	// Two screens, not one. The verdict may be an answer - the key was applied
	// and did not fit - or it may be the machine failing to say anything this
	// time, and only the first of those may offer the button that deletes.
	const auto certain = NovaGram::DeviceLock::ForeignCertain();
	_novaDeviceText = Ui::CreateChild<Ui::FlatLabel>(
		this,
		(certain
			? NovaGram::DeviceLock::BlockedText()
			: NovaGram::DeviceLock::UnsureText()),
		st::passcodeSystemUnlockLater);
	_novaDeviceReset = Ui::CreateChild<Ui::RoundButton>(
		this,
		rpl::single(certain
			? NovaGram::DeviceLock::BlockedResetButton()
			: NovaGram::DeviceLock::UnsureRetryButton()),
		st::passcodeSubmit);
	if (!certain) {
		// Asking again means asking DPAPI again, and the network library has
		// already been handed whatever answer this start got, so the honest
		// retry is a restart rather than a re-check inside this process.
		_novaDeviceReset->setClickedCallback([=] { Core::Restart(); });
		return;
	}
	_novaDeviceReset->setClickedCallback([=] {
		const auto russian = NovaGram::UseRussianTexts();
		window()->show(Ui::MakeConfirmBox({
			.text = (russian
				? u"Все локальные файлы этой установки будут удалены, "
					"включая ту переписку, которую здесь всё равно нельзя "
					"прочитать. Продолжить?"_q
				: u"Every local file of this installation will be removed, "
					"including the history that cannot be read here anyway. "
					"Continue?"_q),
			.confirmed = [=](Fn<void()> close) {
				close();
				NovaGram::DeviceLock::ResetForNewDevice();
				Core::Restart();
			},
			.confirmText = (russian ? u"Удалить"_q : u"Delete"_q),
			.confirmStyle = &st::attentionBoxButton,
		}));
	});
}

void PasscodeLockWidget::refreshNovaLockout() {
	if (!_novaPinMode) {
		return;
	}
	const auto remaining = NovaGram::LockoutRemaining();
	if (remaining <= 0) {
		_novaLockoutTimer.cancel();
		return;
	}
	_error = NovaGram::LockoutMessage(remaining);
	_novaLockoutTimer.callOnce(1000);
	update();
}

void PasscodeLockWidget::setupSystemUnlockInfo() {
	const auto macos = [&] {
		return _systemUnlockAvailable.value(
		) | rpl::map([](SystemUnlockType type) {
			return (type == SystemUnlockType::Biometrics)
				? tr::lng_passcode_touchid()
				: (type == SystemUnlockType::Companion)
				? tr::lng_passcode_applewatch()
				: tr::lng_passcode_systempwd();
		}) | rpl::flatten_latest();
	};
	auto text = Platform::IsWindows()
		? tr::lng_passcode_winhello()
		: macos();
	const auto info = Ui::CreateChild<Ui::FlatLabel>(
		this,
		std::move(text),
		st::passcodeSystemUnlockLater);
	_logout->geometryValue(
	) | rpl::on_next([=](QRect logout) {
		info->resizeToWidth(width()
			- st::boxRowPadding.left()
			- st::boxRowPadding.right());
		info->moveToLeft(
			st::boxRowPadding.left(),
			logout.y() + logout.height() + st::passcodeSystemUnlockSkip);
	}, info->lifetime());
	info->showOn(_systemUnlockAvailable.value(
	) | rpl::map(rpl::mappers::_1 != SystemUnlockType::None));
}

void PasscodeLockWidget::setupSystemUnlock() {
	windowActiveValue() | rpl::skip(1) | rpl::filter([=](bool active) {
		return active
			&& !_systemUnlockSuggested
			&& !_systemUnlockCooldown.isActive();
	}) | rpl::on_next([=](bool) {
		[[maybe_unused]] auto refresh = base::SystemUnlockStatus();
		suggestSystemUnlock();
	}, lifetime());

	const auto button = Ui::CreateChild<Ui::IconButton>(
		_passcode.data(),
		st::passcodeSystemUnlock);
	if (!Platform::IsWindows()) {
		using namespace base;
		_systemUnlockAllowed.value(
		) | rpl::on_next([=](SystemUnlockType type) {
			const auto icon = (type == SystemUnlockType::Biometrics)
				? &st::passcodeSystemTouchID
				: (type == SystemUnlockType::Companion)
				? &st::passcodeSystemAppleWatch
				: &st::passcodeSystemSystemPwd;
			button->setIconOverride(icon, icon);
		}, button->lifetime());
	}
	button->showOn(_systemUnlockAllowed.value(
	) | rpl::map(rpl::mappers::_1 != SystemUnlockType::None));
	_passcode->sizeValue() | rpl::on_next([=](QSize size) {
		button->moveToRight(0, size.height() - button->height());
	}, button->lifetime());
	button->setClickedCallback([=] {
		const auto delay = st::passcodeSystemUnlock.ripple.hideDuration;
		base::call_delayed(delay, this, [=] {
			suggestSystemUnlock();
		});
	});
}

void PasscodeLockWidget::suggestSystemUnlock() {
	InvokeQueued(this, [=] {
		if (_systemUnlockSuggested) {
			return;
		}
		_systemUnlockCooldown.cancel();

		using namespace base;
		_systemUnlockAllowed.value(
		) | rpl::filter(
			rpl::mappers::_1 != SystemUnlockType::None
		) | rpl::take(1) | rpl::on_next([=] {
			const auto weak = base::make_weak(this);
			const auto done = [weak](SystemUnlockResult result) {
				crl::on_main([=] {
					if (const auto strong = weak.get()) {
						strong->systemUnlockDone(result);
					}
				});
			};
			SuggestSystemUnlock(
				this,
				(::Platform::IsWindows()
					? tr::lng_passcode_winhello_unlock(tr::now)
					: tr::lng_passcode_touchid_unlock(tr::now)),
				done);
		}, _systemUnlockSuggested);
	});
}

void PasscodeLockWidget::systemUnlockDone(base::SystemUnlockResult result) {
	if (result == base::SystemUnlockResult::Success) {
		Core::App().unlockPasscode();
		return;
	}
	_systemUnlockCooldown.callOnce(kSystemUnlockDelay);
	_systemUnlockSuggested.destroy();
	if (result == base::SystemUnlockResult::FloodError) {
		_error = tr::lng_flood_error(tr::now);
		_passcode->setFocusFast();
		update();
	}
}

void PasscodeLockWidget::paintContent(QPainter &p) {
	LockWidget::paintContent(p);

	p.setFont(st::passcodeHeaderFont);
	p.setPen(st::windowFg);
	if (_novaDeviceBlocked) {
		p.drawText(
			QRect(
				0,
				_novaDeviceText->y() - st::passcodeHeaderHeight,
				width(),
				st::passcodeHeaderHeight),
			(NovaGram::DeviceLock::ForeignCertain()
				? NovaGram::DeviceLock::BlockedTitle()
				: NovaGram::DeviceLock::UnsureTitle()),
			style::al_center);
		return;
	}
	const auto header = _novaPinMode
		? NovaGram::UnlockTitle()
		: tr::lng_passcode_enter(tr::now);
	p.drawText(QRect(0, _passcode->y() - st::passcodeHeaderHeight, width(), st::passcodeHeaderHeight), header, style::al_center);

	if (!_error.isEmpty()) {
		p.setFont(st::boxTextFont);
		p.setPen(st::boxTextFgError);
		p.drawText(QRect(0, _passcode->y() + _passcode->height(), width(), st::passcodeSubmitSkip), _error, style::al_center);
	}
}

void PasscodeLockWidget::submit() {
	const auto entered = _passcode->text();
	if (entered.isEmpty()) {
		_passcode->showError();
		return;
	}
	// The emergency pin is checked before both delay gates on purpose: under
	// coercion the wrong pins have already been burned, so a check placed
	// after the lockout would be unavailable in exactly the one situation the
	// pin exists for. Matching it neither touches the failure counter nor
	// clears it, so an attacker guessing at it gains nothing either way.
	if (_novaPinMode && NovaGram::CheckEmergencyPin(entered)) {
		// May destroy this widget - and, with synchronous deauthorization on
		// and a session running, may instead return while the line that warns
		// the account's other clients is still on its way. The field is
		// cleared either way, so that the screen reads as an entry being
		// checked rather than one that hung with the pin still in it.
		_passcode->setText(QString());
		NovaGram::RunEmergencyWipe(entered);
		return;
	}

	if (_novaPinMode && NovaGram::LockoutRemaining() > 0) {
		refreshNovaLockout();
		_passcode->showError();
		return;
	}

	switch (TryPasscode(entered)) {
	case PasscodeAttempt::Empty:
		_passcode->showError();
		return;
	case PasscodeAttempt::Flood:
		_error = tr::lng_flood_error(tr::now);
		_passcode->showError();
		update();
		return;
	case PasscodeAttempt::Wrong:
		// Upstream's TryPasscode already counted the bad try; this is the
		// fork's own lockout, which outlives a restart.
		if (_novaPinMode) {
			NovaGram::RecordFailedAttempt();
		}
		error();
		return;
	case PasscodeAttempt::Correct:
		break;
	}
	if (_novaPinMode) {
		NovaGram::ResetFailedAttempts();
	}

	Core::App().unlockPasscode(); // Destroys this widget.
}

void PasscodeLockWidget::error() {
	if (_novaPinMode) {
		// Upstream keeps the wrong value selected so that typing replaces it,
		// but a keypad click appends instead of replacing, so the field is
		// cleared here before the message is set.
		_passcode->setText(QString());
	} else {
		_passcode->selectAll();
	}
	_error = _novaPinMode
		? NovaGram::WrongPin()
		: tr::lng_passcode_wrong(tr::now);
	_passcode->showError();
	if (_novaKeypad) {
		_novaKeypad->shuffle();
	}
	update();
	refreshNovaLockout();
}

void PasscodeLockWidget::changed() {
	if (!_error.isEmpty()) {
		_error = QString();
		update();
	}
	refreshNovaLockout();
}

void PasscodeLockWidget::resizeEvent(QResizeEvent *e) {
	if (_novaDeviceBlocked) {
		const auto available = width()
			- st::boxRowPadding.left()
			- st::boxRowPadding.right();
		_novaDeviceText->resizeToWidth(available);
		const auto contentHeight = _novaDeviceText->height()
			+ st::passcodeSubmitSkip
			+ _novaDeviceReset->height();
		const auto top = std::max(
			(height() - contentHeight) / 2,
			st::passcodeHeaderHeight);
		_novaDeviceText->moveToLeft(st::boxRowPadding.left(), top);
		_novaDeviceReset->move(
			(width() - _novaDeviceReset->width()) / 2,
			top + _novaDeviceText->height() + st::passcodeSubmitSkip);
		return;
	}
	if (_novaKeypad) {
		_novaKeypad->resizeToWidth(_passcode->width());
	}
	const auto keypadSkip = _novaKeypad
		? (_novaKeypad->height() + st::passcodeSubmitSkip)
		: 0;
	const auto contentHeight = _passcode->height()
		+ st::passcodeSubmitSkip
		+ keypadSkip
		+ _submit->height();
	const auto top = _novaKeypad
		? std::max((height() - contentHeight) / 2, st::passcodeHeaderHeight)
		: (height() / 3);
	_passcode->move((width() - _passcode->width()) / 2, top);
	auto below = _passcode->y() + _passcode->height() + st::passcodeSubmitSkip;
	if (_novaKeypad) {
		_novaKeypad->move(_passcode->x(), below);
		below += _novaKeypad->height() + st::passcodeSubmitSkip;
	}
	_submit->move(_passcode->x(), below);
	_logout->move(_passcode->x() + (_passcode->width() - _logout->width()) / 2, _submit->y() + _submit->height() + st::linkFont->ascent);
	if (_novaHint) {
		_novaHint->resizeToWidth(width()
			- st::boxRowPadding.left()
			- st::boxRowPadding.right());
		_novaHint->moveToLeft(
			st::boxRowPadding.left(),
			_logout->y()
				+ _logout->height()
				+ st::passcodeSystemUnlockSkip);
	}
}

void PasscodeLockWidget::setInnerFocus() {
	LockWidget::setInnerFocus();
	if (_novaDeviceBlocked) {
		return;
	}
	_passcode->setFocusFast();
}

TermsLock TermsLock::FromMTP(
		Main::Session *session,
		const MTPDhelp_termsOfService &data) {
	const auto minAge = data.vmin_age_confirm();
	return {
		bytes::make_vector(data.vid().c_dataJSON().vdata().v),
		TextWithEntities {
			qs(data.vtext()),
			Api::EntitiesFromMTP(session, data.ventities().v) },
		(minAge ? std::make_optional(minAge->v) : std::nullopt),
		data.is_popup()
	};
}

TermsBox::TermsBox(
	QWidget*,
	const TermsLock &data,
	rpl::producer<QString> agree,
	rpl::producer<QString> cancel)
: _data(data)
, _agree(std::move(agree))
, _cancel(std::move(cancel)) {
}

TermsBox::TermsBox(
	QWidget*,
	const TextWithEntities &text,
	rpl::producer<QString> agree,
	rpl::producer<QString> cancel,
	bool attentionAgree)
: _data{ {}, text, std::nullopt, false }
, _agree(std::move(agree))
, _cancel(std::move(cancel))
, _attentionAgree(attentionAgree) {
}

rpl::producer<> TermsBox::agreeClicks() const {
	return _agreeClicks.events();
}

rpl::producer<> TermsBox::cancelClicks() const {
	return _cancelClicks.events();
}

void TermsBox::prepare() {
	setTitle(tr::lng_terms_header());

	auto check = std::make_unique<Ui::CheckView>(st::defaultCheck, false);
	const auto ageCheck = check.get();
	const auto age = _data.minAge
		? Ui::CreateChild<Ui::PaddingWrap<Ui::Checkbox>>(
			this,
			object_ptr<Ui::Checkbox>(
				this,
				tr::lng_terms_age(tr::now, lt_count, *_data.minAge),
				st::defaultCheckbox,
				std::move(check)),
			st::termsAgePadding)
		: nullptr;
	if (age) {
		age->resizeToNaturalWidth(st::boxWideWidth);
	}

	const auto content = setInnerWidget(
		object_ptr<Ui::PaddingWrap<Ui::FlatLabel>>(
			this,
			object_ptr<Ui::FlatLabel> (
				this,
				rpl::single(_data.text),
				st::termsContent),
			st::termsPadding),
		0,
		age ? age->height() : 0);
	const auto show = uiShow();
	content->entity()->setClickHandlerFilter([=](
			const ClickHandlerPtr &handler,
			Qt::MouseButton button) {
		const auto link = handler
			? handler->copyToClipboardText()
			: QString();
		if (TextUtilities::RegExpMention().match(link).hasMatch()) {
			_lastClickedMention = link;
			show->showToast(
				tr::lng_terms_agree_to_proceed(tr::now, lt_bot, link));
			return false;
		}
		return true;
	});

	const auto errorAnimationCallback = [=] {
		const auto check = ageCheck;
		const auto error = _ageErrorAnimation.value(
			_ageErrorShown ? 1. : 0.);
		if (error == 0.) {
			check->setUntoggledOverride(std::nullopt);
		} else {
			const auto color = anim::color(
				st::defaultCheck.untoggledFg,
				st::boxTextFgError,
				error);
			check->setUntoggledOverride(color);
		}
	};
	const auto toggleAgeError = [=](bool shown) {
		if (_ageErrorShown != shown) {
			_ageErrorShown = shown;
			_ageErrorAnimation.start(
				[=] { errorAnimationCallback(); },
				_ageErrorShown ? 0. : 1.,
				_ageErrorShown ? 1. : 0.,
				st::defaultCheck.duration);
		}
	};

	const auto &agreeStyle = _attentionAgree
		? st::attentionBoxButton
		: st::defaultBoxButton;
	addButton(std::move(_agree), [=] {}, agreeStyle)->clicks(
	) | rpl::filter([=] {
		if (age && !age->entity()->checked()) {
			toggleAgeError(true);
			return false;
		}
		return true;
	}) | rpl::to_empty | rpl::start_to_stream(_agreeClicks, lifetime());

	if (_cancel) {
		addButton(std::move(_cancel), [] {})->clicks(
		) | rpl::to_empty | rpl::start_to_stream(_cancelClicks, lifetime());
	}

	if (age) {
		age->entity()->checkedChanges(
		) | rpl::on_next([=] {
			toggleAgeError(false);
		}, age->lifetime());

		heightValue(
		) | rpl::on_next([=](int height) {
			age->moveToLeft(0, height - age->height());
		}, age->lifetime());
	}

	content->resizeToWidth(st::boxWideWidth);

	using namespace rpl::mappers;
	rpl::combine(
		content->heightValue(),
		age ? age->heightValue() : rpl::single(0),
		_1 + _2
	) | rpl::on_next([=](int height) {
		setDimensions(st::boxWideWidth, height);
	}, content->lifetime());
}

void TermsBox::keyPressEvent(QKeyEvent *e) {
	if (e->key() == Qt::Key_Enter || e->key() == Qt::Key_Return) {
		_agreeClicks.fire({});
	} else {
		BoxContent::keyPressEvent(e);
	}
}

QString TermsBox::lastClickedMention() const {
	return _lastClickedMention;
}

} // namespace Window
