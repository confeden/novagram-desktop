/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_settings.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "main/main_session.h"
#include "novagram/nova_autodelete.h"
#include "novagram/nova_branding.h"
#include "novagram/nova_calls.h"
#include "novagram/nova_doh.h"
#include "novagram/nova_decoy.h"
#include "novagram/nova_device_lock.h"
#include "novagram/nova_filenames.h"
#include "novagram/nova_icon_design.h"
#include "novagram/nova_marquee_button.h"
#include "novagram/nova_metadata.h"
#include "novagram/nova_update.h"
#include "novagram/nova_night_silent.h"
#include "novagram/nova_notify_previews.h"
#include "novagram/nova_pin.h"
#include "novagram/nova_pin_policy.h"
#include "novagram/nova_pin_box.h"
#include "novagram/nova_read_status.h"
#include "novagram/nova_screen_guard.h"
#include "novagram/nova_crash_stickers.h"
#include "novagram/nova_stories.h"
#include "novagram/nova_sync_deauth.h"
#include "settings/settings_common_session.h"
#include "ui/layers/generic_box.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "tray.h"
#include "window/notifications_manager.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace NovaGram {
namespace {

using namespace Settings;

constexpr auto kHideNotificationContentKey
	= "novagram_hide_notification_content"_cs;

// Every description in this section is deliberately one or two sentences: what
// the switch does, and the one honest boundary that changes a decision. The
// full reasoning lives in docs/ROADMAP.md, not on screen - a wall of text in
// the settings is not read, and a caveat nobody reads has not been given.

[[nodiscard]] QString PinAbout() {
	return UseRussianTexts()
		? u"Основной PIN шифрует локальные данные: без него база на диске не "
			"читается. Аварийный PIN вводится под принуждением и вместо "
			"разблокировки уничтожает данные. Пока PIN задан, разблокировка "
			"через Windows Hello отключена."_q
		: u"The primary PIN encrypts the local data: without it the database "
			"on disk cannot be read. The emergency PIN is entered under "
			"coercion and destroys the data instead of unlocking. While a PIN "
			"is set, the Windows Hello unlock is disabled."_q;
}

[[nodiscard]] QString DeviceLockAbout() {
	const auto russian = UseRussianTexts();
	const auto mechanism = DeviceLock::BackendName(DeviceLock::CurrentBackend());
	const auto about = russian
		? u"Скопированная папка tdata не открывается на другом компьютере и в "
			"другой учётной записи Windows — войти в аккаунт по ней нельзя даже "
			"без PIN. От программы, запущенной под вашей же учётной записью "
			"Windows, защищает только PIN. Механизм: "_q + mechanism + u"."_q
		: u"A copied tdata folder opens neither on another computer nor under "
			"another Windows account - it cannot sign anyone in, even with no "
			"PIN set. Against a program running under your own Windows account "
			"only a PIN helps. Mechanism: "_q + mechanism + u"."_q;
	if (!DeviceLock::Enabled() || DeviceLock::DataSealed()) {
		return about;
	}
	// The switch draws what is on the disk, so it is already off here while the
	// preference says otherwise. Saying only "off" would leave the owner
	// wondering why it moved by itself.
	return (russian
		? u"Привязка включена в настройках, но данные на диске сейчас не "
			"привязаны: этому компьютеру нечем хранить ключ. Включите "
			"переключатель ещё раз, чтобы повторить попытку.\n\n"_q
		: u"Binding is on in the settings, but the data on the disk is not "
			"bound right now: this computer has nothing to keep the key in. "
			"Switch it on again to retry.\n\n"_q) + about;
}

[[nodiscard]] QString KeypadAbout() {
	return UseRussianTexts()
		? u"Ввод PIN мышью. Цифры каждый раз располагаются в новом порядке, "
			"поэтому по движению курсора и следам на экране PIN не "
			"восстановить."_q
		: u"Enter the PIN with the mouse. The digits are laid out in a new "
			"order every time, so the PIN cannot be recovered from cursor "
			"movements or screen smudges."_q;
}

[[nodiscard]] QString ScreenGuardAbout() {
	return UseRussianTexts()
		? u"Окна NovaGram исключаются из захвата экрана: снимок, запись и "
			"демонстрация экрана не увидят переписку. Не защищает от камеры, "
			"направленной на монитор."_q
		: u"NovaGram windows are excluded from screen capture: screenshots, "
			"recording and screen sharing will not see the conversation. It "
			"does not protect against a camera pointed at the monitor."_q;
}

[[nodiscard]] QString NightSilentAbout() {
	return UseRussianTexts()
		? u"С 22:00 до 07:00 по местному времени исходящие сообщения приходят "
			"получателю без звука. Сообщение доставляется как обычно, "
			"беззвучным становится только уведомление."_q
		: u"Between 22:00 and 07:00 in local time, outgoing messages arrive "
			"without a notification sound. The message is delivered as usual; "
			"only the notification is silent."_q;
}

// Every description in this section used to sit open under its switch, and
// eighteen of them turned the page into a wall nobody reads. Collapsed into
// one row that says what it is, opened by pressing it. The text itself may now
// run longer than the two sentences the old rule allowed: it costs no screen
// space until somebody asks for it.
//
// Drawn quietly - the subtitle colour, a size below the switches - and headed
// by a chevron rather than a colon: a colon announces a text that is not there
// until the row is pressed, while the chevron says the row opens something,
// which is the Telegram gesture for exactly that.
//
// Takes a producer and not a string: a description whose wording depends on
// what is on the disk - the device binding's does - has to be able to change
// under a row that is already on the screen.
void AddAbout(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> text) {
	const auto button = container->add(
		object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(UseRussianTexts()
				? u"Описание функции ›"_q
				: u"What this does ›"_q),
			st::novaSettingsAbout));
	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			container,
			object_ptr<Ui::FlatLabel>(
				container,
				std::move(text),
				st::boxDividerLabel),
			st::defaultBoxDividerLabelPadding));
	wrap->hide(anim::type::instant);
	button->setClickedCallback([=] {
		wrap->toggle(!wrap->toggled(), anim::type::normal);
	});
}

void AddAbout(not_null<Ui::VerticalLayout*> container, const QString &text) {
	AddAbout(container, rpl::single(text));
}

not_null<Button*> AddToggle(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		bool checked,
		Fn<void(bool)> changed,
		const style::SettingsButton &st = st::settingsButtonNoIcon) {
	// Not AddButtonWithIcon: these titles are long enough to be cut off, and
	// MarqueeButton scrolls the rest into view while the cursor is on the row.
	const auto button = container->add(
		object_ptr<MarqueeButton>(
			container,
			text,
			st));
	button->toggleOn(rpl::single(checked));
	button->toggledChanges(
	) | rpl::on_next([=](bool toggled) {
		changed(toggled);
	}, button->lifetime());
	return button;
}

void PinPolicyBox(not_null<Ui::GenericBox*> box, Fn<void()> changed) {
	const auto russian = UseRussianTexts();
	box->setTitle(rpl::single(PinPolicyTitle()));
	for (const auto option : PinPolicyOptions()) {
		const auto button = box->addRow(
			object_ptr<Ui::SettingsButton>(
				box,
				rpl::single(PinPolicyName(option)),
				st::settingsButtonNoIcon),
			style::margins());
		button->setClickedCallback([=] {
			SetPinPolicy(option);
			changed();
			box->closeBox();
		});
	}
	box->addButton(
		rpl::single(russian ? u"Закрыть"_q : u"Close"_q),
		[=] { box->closeBox(); });
}

void FillProtection(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	const auto russian = UseRussianTexts();

	Ui::AddSubsectionTitle(
		container,
		rpl::single(russian ? u"Защита"_q : u"Protection"_q));

	// First in the section on purpose: this one works with no PIN set, and it
	// is the only thing standing between a copied folder and the account.
	//
	// Drawn from DataSealed() and not from the preference: a preference can
	// outlive the mechanism behind it, and a switch that says "bound" over a
	// tdata folder that is portable again states the promise backwards.
	const auto bindingRefreshed = container->lifetime().make_state<
		rpl::event_stream<>
	>();
	const auto bindingApplying = container->lifetime().make_state<bool>(false);
	const auto binding = container->add(
		object_ptr<MarqueeButton>(
			container,
			(russian
				? u"Привязать данные к этому компьютеру"_q
				: u"Bind the data to this computer"_q),
			st::settingsButtonNoIcon));
	binding->toggleOn(rpl::single(
		rpl::empty
	) | rpl::then(
		bindingRefreshed->events()
	) | rpl::map([] {
		return DeviceLock::DataSealed();
	}));
	binding->toggledChanges(
	) | rpl::on_next([=](bool toggled) {
		if (*bindingApplying) {
			// The switch is being put back to what actually landed on the
			// disk. That is an answer, not a request from the owner.
			return;
		}
		*bindingApplying = true;
		DeviceLock::SetEnabled(toggled);
		bindingRefreshed->fire({});
		*bindingApplying = false;
	}, binding->lifetime());

	// Collapsed like every other description on this page. It used to be an
	// always-open divider text, and it is the longest text in the section, so
	// the one row that stayed open was the one that pushed everything else
	// off the first screen.
	Ui::AddSkip(container);
	AddAbout(
		container,
		rpl::single(rpl::empty) | rpl::then(
			bindingRefreshed->events()
		) | rpl::map([] { return DeviceLockAbout(); }));
	Ui::AddSkip(container);

	const auto refreshed = container->lifetime().make_state<
		rpl::event_stream<>
	>();
	auto updates = rpl::single(rpl::empty) | rpl::then(refreshed->events());

	const auto pin = AddButtonWithLabel(
		container,
		rpl::single(PinSettingsTitle()),
		rpl::duplicate(updates) | rpl::map([] { return PinSettingsLabel(); }),
		st::settingsButton,
		{ &st::menuIconLock });
	pin->setClickedCallback([=] {
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			PinSetupBox(box);
			box->boxClosing() | rpl::on_next([=] {
				refreshed->fire({});
			}, box->lifetime());
		}));
	});

	Ui::AddSkip(container);
	AddAbout(container, PinAbout());
	Ui::AddSkip(container);

	const auto policyRefreshed = container->lifetime().make_state<
		rpl::event_stream<>
	>();
	auto policyUpdates = rpl::single(rpl::empty)
		| rpl::then(policyRefreshed->events());
	const auto policy = AddButtonWithLabel(
		container,
		rpl::single(PinPolicyTitle()),
		rpl::duplicate(policyUpdates) | rpl::map([] {
			return PinPolicyName(PinPolicy());
		}),
		st::settingsButton,
		{ &st::menuIconTimer });
	policy->setClickedCallback([=] {
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			PinPolicyBox(box, [=] { policyRefreshed->fire({}); });
		}));
	});

	Ui::AddSkip(container);
	AddAbout(container, PinPolicyAbout());
	Ui::AddSkip(container);

	// Directly under the emergency PIN it extends, and above the two switches
	// that are about this screen only: it is the one row on this page whose
	// effect is not local to this computer.
	AddToggle(
		container,
		SyncDeauth::Title(),
		SyncDeauth::Enabled(),
		[](bool toggled) { SyncDeauth::SetEnabled(toggled); });

	Ui::AddSkip(container);
	AddAbout(container, SyncDeauth::About());
	Ui::AddSkip(container);

	AddToggle(
		container,
		(russian
			? u"Экранная клавиатура со случайным порядком"_q
			: u"On-screen keypad in a random order"_q),
		ShuffledKeypadEnabled(),
		[](bool toggled) { SetShuffledKeypadEnabled(toggled); });

	Ui::AddSkip(container);
	AddAbout(container, KeypadAbout());

	if (ScreenGuardSupported()) {
		Ui::AddSkip(container);
		AddToggle(
			container,
			(russian
				? u"Запретить снимки и запись экрана"_q
				: u"Block screenshots and screen recording"_q),
			ScreenGuardEnabled(),
			[](bool toggled) { SetScreenGuardEnabled(toggled); });

		Ui::AddSkip(container);
		AddAbout(container, ScreenGuardAbout());
	}
}

void PeriodBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		Fn<void()> changed) {
	const auto russian = UseRussianTexts();
	box->setTitle(rpl::single(russian ? u"Срок"_q : u"Period"_q));

	const auto options = std::vector<int>{ 24, 48, 72, 120, 168 };
	for (const auto hours : options) {
		const auto button = box->addRow(
			object_ptr<Ui::SettingsButton>(
				box,
				rpl::single(FormatPeriod(hours)),
				st::settingsButtonNoIcon),
			style::margins());
		button->setClickedCallback([=] {
			SetPeriodHours(session, hours);
			changed();
			box->closeBox();
		});
	}

	box->addButton(
		rpl::single(russian ? u"Закрыть"_q : u"Close"_q),
		[=] { box->closeBox(); });
}

[[nodiscard]] QString AutoDeleteAbout() {
	return UseRussianTexts()
		? u"Свои отправленные сообщения удаляются по истечении срока, отсчёт "
			"идёт от отправки. Отправленные до включения функции не удаляются "
			"— для них есть «Erase evidence» в меню чата. Чаты с правами "
			"администратора и «Избранное» не затрагиваются."_q
		: u"Your own sent messages are deleted once the period passes, counted "
			"from the moment they were sent. Messages sent before this was "
			"switched on are not queued — use Erase evidence in the chat menu "
			"for those. Chats where you have admin rights and Saved Messages "
			"are left alone."_q;
}

void FillAutoDelete(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	const auto russian = UseRussianTexts();
	const auto session = &controller->session();

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(russian
			? u"Автоудаление своих сообщений"_q
			: u"Auto-delete own messages"_q));

	const auto shown = container->lifetime().make_state<rpl::variable<bool>>(
		Enabled(session));
	AddToggle(
		container,
		(russian ? u"Удалять мои сообщения"_q : u"Delete my messages"_q),
		Enabled(session),
		[=](bool toggled) {
			SetEnabled(session, toggled);
			*shown = toggled;
		});

	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	wrap->toggleOn(shown->value());
	wrap->finishAnimating();

	const auto inner = wrap->entity();
	const auto refreshed = inner->lifetime().make_state<rpl::event_stream<>>();
	auto updates = rpl::single(rpl::empty) | rpl::then(refreshed->events());

	const auto period = AddButtonWithLabel(
		inner,
		rpl::single(russian ? u"Срок"_q : u"Period"_q),
		rpl::duplicate(updates) | rpl::map([=] {
			return FormatPeriod(PeriodHours(session));
		}),
		st::settingsButtonNoIcon);
	period->setClickedCallback([=] {
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			PeriodBox(box, session, [=] { refreshed->fire({}); });
		}));
	});

	Ui::AddSkip(container);
	AddAbout(container, AutoDeleteAbout());
}

[[nodiscard]] QString ReadStatusAbout() {
	return UseRussianTexts()
		? u"Когда вам пишет незнакомец, а вы ещё не отвечали, галочки "
			"прочтения ему не отправляются, а его истории смотрятся без "
			"уведомления автора. Правило заводится по первому такому "
			"сообщению; группы, каналы, боты и «Избранное» не затрагиваются. "
			"Ваш ответ или реакция снимают скрытие навсегда."_q
		: u"When a stranger writes to you and you have not answered yet, the "
			"read marks are never sent to them, and their stories are watched "
			"without the author being told. The rule is made on the first "
			"such message; groups, channels, bots and Saved Messages are left "
			"alone. An answer or a reaction of yours lifts it for good."_q;
}

void FillReadStatus(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();

	Ui::AddSkip(container);
	AddToggle(
		container,
		ReadStatusTitle(),
		ReadStatusEnabled(session),
		[=](bool toggled) { SetReadStatusEnabled(session, toggled); });

	Ui::AddSkip(container);
	AddAbout(container, ReadStatusAbout());
}

[[nodiscard]] QString DohTitle() {
	return UseRussianTexts()
		? u"Защищённый DNS"_q
		: u"Encrypted DNS"_q;
}

[[nodiscard]] QString DohAbout() {
	return UseRussianTexts()
		? u"Имена, которые NovaGram разрешает сам, идут только по этому списку. "
			"Ни система, ни сеть, ни файл hosts на него не влияют и не "
			"подставляются взамен. Свой сервер разрешается через включённые "
			"встроенные."_q
		: u"Names NovaGram resolves itself go through this list and nothing "
			"else: neither the system, nor the network, nor the hosts file "
			"affects it or stands in for it. A server of your own is resolved "
			"through the built-in ones that are on."_q;
}

void AddCustomDohBox(
		not_null<Ui::GenericBox*> box,
		Fn<void(Doh::Endpoint)> done) {
	const auto russian = UseRussianTexts();
	box->setTitle(rpl::single(russian
		? u"Свой сервер"_q
		: u"Your own server"_q));
	const auto host = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		rpl::single(u"dns.example.org"_q)));
	const auto addresses = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		rpl::single(russian
			? u"IP-адреса через запятую, можно не указывать"_q
			: u"IP addresses, comma separated, optional"_q)));
	box->setFocusCallback([=] { host->setFocusFast(); });
	box->addButton(rpl::single(russian ? u"Добавить"_q : u"Add"_q), [=] {
		auto endpoint = Doh::Endpoint();
		// Only a host is asked for: the path is the one every RFC 8484
		// endpoint uses, and a field for it would be a way to get it wrong.
		endpoint.host = host->getLastText().trimmed().toLower();
		endpoint.path = u"/dns-query"_q;
		endpoint.builtin = false;
		endpoint.enabled = true;
		if (endpoint.host.isEmpty() || endpoint.host.contains('/')) {
			host->showError();
			return;
		}
		for (const auto &part : addresses->getLastText().split(',')) {
			const auto trimmed = part.trimmed();
			if (!trimmed.isEmpty()) {
				endpoint.addresses.push_back(trimmed);
			}
		}
		done(std::move(endpoint));
		box->closeBox();
	});
	box->addButton(
		rpl::single(russian ? u"Отмена"_q : u"Cancel"_q),
		[=] { box->closeBox(); });
}

void DohBox(not_null<Ui::GenericBox*> box) {
	const auto russian = UseRussianTexts();
	box->setTitle(rpl::single(DohTitle()));

	const auto state = box->lifetime().make_state<std::vector<Doh::Endpoint>>(
		Doh::Endpoints());
	const auto rebuild = box->lifetime().make_state<rpl::event_stream<>>();
	const auto container = box->verticalLayout();
	const auto wrap = container->add(
		object_ptr<Ui::VerticalLayout>(container));

	const auto fill = [=] {
		while (wrap->count()) {
			delete wrap->widgetAt(0);
		}
		for (auto i = 0; i != int(state->size()); ++i) {
			const auto &endpoint = (*state)[i];
			const auto button = wrap->add(
				object_ptr<Ui::SettingsButton>(
					wrap,
					rpl::single(endpoint.host),
					st::settingsButtonNoIcon));
			button->toggleOn(rpl::single(endpoint.enabled));
			button->toggledChanges(
			) | rpl::on_next([=](bool toggled) {
				(*state)[i].enabled = toggled;
			}, button->lifetime());
			if (!endpoint.builtin) {
				// A server of one's own is the only kind that can be taken
				// away: the built-in four are the floor this stands on.
				button->setClickedCallback([=] {
					state->erase(begin(*state) + i);
					rebuild->fire({});
				});
			}
		}
		wrap->resizeToWidth(box->width());
	};
	rebuild->events() | rpl::on_next(fill, box->lifetime());
	fill();

	Ui::AddSkip(container);
	AddAbout(container, DohAbout());
	Ui::AddSkip(container);

	const auto add = container->add(
		object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(russian ? u"Добавить свой сервер"_q : u"Add your own"_q),
			st::settingsButtonNoIcon));
	add->setClickedCallback([=] {
		box->uiShow()->showBox(Box(AddCustomDohBox, [=](Doh::Endpoint value) {
			state->push_back(std::move(value));
			rebuild->fire({});
		}));
	});

	box->addButton(rpl::single(russian ? u"Сохранить"_q : u"Save"_q), [=] {
		const auto error = Doh::ValidateEndpoints(*state);
		if (!error.isEmpty()) {
			box->uiShow()->showToast(error);
			return;
		}
		Doh::SetEndpoints(*state);
		box->closeBox();
	});
	box->addButton(
		rpl::single(russian ? u"Отмена"_q : u"Cancel"_q),
		[=] { box->closeBox(); });
}

void FillDoh(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	Ui::AddSkip(container);
	const auto refreshed = container->lifetime().make_state<
		rpl::event_stream<>
	>();
	auto updates = rpl::single(rpl::empty) | rpl::then(refreshed->events());
	const auto row = AddButtonWithLabel(
		container,
		rpl::single(DohTitle()),
		rpl::duplicate(updates) | rpl::map([] {
			auto on = 0;
			for (const auto &endpoint : Doh::Endpoints()) {
				if (endpoint.enabled) {
					++on;
				}
			}
			return QString::number(on);
		}),
		st::settingsButtonNoIcon);
	row->setClickedCallback([=] {
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			DohBox(box);
			box->boxClosing() | rpl::on_next([=] {
				refreshed->fire({});
			}, box->lifetime());
		}));
	});

	Ui::AddSkip(container);
	AddAbout(container, DohAbout());
}

void FillCalls(not_null<Ui::VerticalLayout*> container) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(UseRussianTexts() ? u"Звонки"_q : u"Calls"_q));

	AddToggle(
		container,
		Calls::RelayOnlyTitle(),
		Calls::RelayOnly(),
		[=](bool toggled) { Calls::SetRelayOnly(toggled); });

	Ui::AddSkip(container);
	AddAbout(container, Calls::RelayOnlyAbout());
}

[[nodiscard]] QString FileNamesAbout() {
	return UseRussianTexts()
		? u"Файл, сохранённый без вопроса, получает обезличенное имя вместо "
			"присланного, поэтому список папки «Загрузки» ничего не "
			"рассказывает. Явное «Сохранить как» имя не меняет."_q
		: u"A file saved without asking gets a meaningless name instead of the "
			"one it came with, so a listing of the Downloads folder tells "
			"nothing. An explicit Save as keeps the name."_q;
}

[[nodiscard]] QString StripMetadataAbout() {
	return UseRussianTexts()
		? u"Из отправляемых JPEG и PNG вырезаются блоки метаданных — Exif, XMP, "
			"IPTC, комментарии, — то есть камера, время и координаты съёмки. "
			"Остальные форматы, включая HEIC, PDF и видео, уходят как есть."_q
		: u"Metadata blocks - Exif, XMP, IPTC, comments - are cut out of the "
			"JPEG and PNG files you send, which is the camera, the time and the "
			"place. Other formats, HEIC, PDF and video among them, are sent as "
			"they are."_q;
}

void FillFiles(not_null<Ui::VerticalLayout*> container) {
	const auto russian = UseRussianTexts();

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(russian ? u"Файлы"_q : u"Files"_q));

	AddToggle(
		container,
		MaskedFileNamesTitle(),
		MaskedFileNamesEnabled(),
		[](bool toggled) { SetMaskedFileNamesEnabled(toggled); });

	Ui::AddSkip(container);
	AddAbout(container, FileNamesAbout());

	Ui::AddSkip(container);
	AddToggle(
		container,
		StripMetadataTitle(),
		StripMetadataEnabled(),
		[](bool toggled) { SetStripMetadataEnabled(toggled); });

	Ui::AddSkip(container);
	AddAbout(container, StripMetadataAbout());
}

// One axis of the icon picker, drawn as a strip: the chosen variant in the
// middle at full size, its neighbours smaller on either side, and a click on
// any of them chooses it. Arrows moved one step at a time and showed nothing
// of what was coming; a strip is what the choosing actually feels like.
class DesignStrip final : public Ui::RpWidget {
public:
	DesignStrip(
		QWidget *parent,
		int count,
		Fn<QImage(int index, int size)> render,
		Fn<QString(int index)> name,
		Fn<void(int index)> chosen)
	: RpWidget(parent)
	, _count(count)
	, _render(std::move(render))
	, _name(std::move(name))
	, _chosen(std::move(chosen)) {
		resize(width(), style::ConvertScale(94));
		setCursor(style::cur_pointer);
	}

	void setCurrent(int index) {
		_current = ((index % _count) + _count) % _count;
		_cache.clear();
		update();
	}

	[[nodiscard]] int current() const {
		return _current;
	}

	void refresh() {
		_cache.clear();
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		p.setRenderHint(QPainter::Antialiasing, true);
		const auto ratio = style::DevicePixelRatio();
		const auto center = width() / 2;
		const auto middle = (height() - style::ConvertScale(20)) / 2;
		for (auto step = -kSide; step <= kSide; ++step) {
			const auto index = ((_current + step) % _count + _count) % _count;
			const auto side = step ? kSmall : kLarge;
			const auto x = center + step * kStep - side / 2;
			const auto y = middle - side / 2;
			auto i = _cache.find(index * 2 + (step ? 0 : 1));
			if (i == end(_cache)) {
				auto image = _render(index, side * ratio);
				image.setDevicePixelRatio(ratio);
				i = _cache.emplace(
					index * 2 + (step ? 0 : 1),
					std::move(image)).first;
			}
			if (!step) {
				auto pen = QPen(st::activeButtonBg->c);
				pen.setWidthF(st::lineWidth * 2);
				p.setPen(pen);
				p.setBrush(Qt::NoBrush);
				const auto ring = side / 2 + style::ConvertScale(4);
				p.drawEllipse(QPoint(center, middle), ring, ring);
			}
			p.setOpacity(step ? (1. - 0.22 * std::abs(step)) : 1.);
			p.drawImage(QRect(x, y, side, side), i->second);
			p.setOpacity(1.);
		}
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			QRect(0, height() - style::ConvertScale(20), width(), style::ConvertScale(18)),
			style::al_top,
			_name(_current));
	}

	void mousePressEvent(QMouseEvent *e) override {
		const auto center = width() / 2;
		const auto dx = e->pos().x() - center;
		const auto step = (dx > kStep / 2)
			? ((dx + kStep / 2) / kStep)
			: (dx < -kStep / 2)
			? -((-dx + kStep / 2) / kStep)
			: 0;
		if (step) {
			choose(_current + step);
		}
	}

	// Deliberately no wheelEvent: three of these strips sit one under another
	// on a page that scrolls, so a wheel turn meant for the page changed the
	// icon instead - and the one gesture nobody expects to alter a setting is
	// scrolling past it. Not overriding it at all is what lets the event
	// through to the scroll area; a neighbour is chosen by clicking it.

private:
	static constexpr auto kSide = 3;

	void choose(int index) {
		setCurrent(index);
		_chosen(_current);
	}

	const int _count = 0;
	const Fn<QImage(int, int)> _render;
	const Fn<QString(int)> _name;
	const Fn<void(int)> _chosen;
	base::flat_map<int, QImage> _cache;
	int _current = 0;
	const int kLarge = style::ConvertScale(52);
	const int kSmall = style::ConvertScale(34);
	const int kStep = style::ConvertScale(46);

};

void FillIcon(not_null<Ui::VerticalLayout*> container) {
	using IconDesign::Design;

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(IconDesign::SectionTitle()));

	struct State {
		Design draft;
		DesignStrip *style = nullptr;
		DesignStrip *texture = nullptr;
		DesignStrip *accent = nullptr;
		Ui::RpWidget *preview = nullptr;
	};
	const auto state = container->lifetime().make_state<State>();
	state->draft = IconDesign::Current();

	const auto side = style::ConvertScale(112);
	const auto skip = st::defaultVerticalListSkip;
	const auto preview = container->add(
		object_ptr<Ui::FixedHeightWidget>(container, side + 2 * skip));
	state->preview = preview;
	preview->paintRequest(
	) | rpl::on_next([=] {
		const auto ratio = style::DevicePixelRatio();
		auto image = IconDesign::Render(state->draft, side * ratio, false);
		image.setDevicePixelRatio(ratio);
		auto p = QPainter(preview);
		p.drawImage(
			QRect((preview->width() - side) / 2, skip, side, side),
			image);
	}, preview->lifetime());

	// Every strip draws the other two axes as they stand, so changing the style
	// changes what the texture strip is showing - which is the whole reason
	// these are pictures and not names.
	const auto refreshAll = [=] {
		preview->update();
		for (const auto strip : { state->style, state->texture, state->accent }) {
			if (strip) {
				strip->refresh();
			}
		}
	};
	const auto add = [&](
			int count,
			Fn<Design(int index)> variant,
			Fn<QString(int index)> name,
			Fn<void(int index)> chosen) {
		const auto strip = container->add(
			object_ptr<DesignStrip>(
				container,
				count,
				[=](int index, int size) {
					return IconDesign::Render(variant(index), size, false);
				},
				name,
				[=](int index) {
					chosen(index);
					refreshAll();
				}));
		return strip;
	};

	state->style = add(
		IconDesign::kStyles,
		[=](int index) {
			auto result = state->draft;
			result.style = index;
			return result;
		},
		[](int index) { return IconDesign::StyleName(index); },
		[=](int index) { state->draft.style = index; });
	state->style->setCurrent(state->draft.style);

	state->texture = add(
		IconDesign::kTextures,
		[=](int index) {
			auto result = state->draft;
			result.texture = index;
			return result;
		},
		[](int index) { return IconDesign::TextureName(index); },
		[=](int index) { state->draft.texture = index; });
	state->texture->setCurrent(state->draft.texture);

	state->accent = add(
		IconDesign::kAccents,
		[=](int index) {
			auto result = state->draft;
			result.accent = index;
			return result;
		},
		[](int index) { return IconDesign::AccentName(index); },
		[=](int index) { state->draft.accent = index; });
	state->accent->setCurrent(state->draft.accent);

	Ui::AddSkip(container);
	const auto russian = UseRussianTexts();
	const auto apply = container->add(
		object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(russian ? u"Применить"_q : u"Apply"_q),
			st::settingsButtonNoIcon));
	apply->setClickedCallback([=] {
		IconDesign::SetCurrent(state->draft);
		// The window and the taskbar take the icon from the application, the
		// tray keeps its own copy - so both are told, and neither waits for a
		// restart.
		Core::App().refreshApplicationIcon();
		Core::App().tray().updateIconCounters();
	});

	Ui::AddSkip(container);
	AddAbout(container, IconDesign::SectionAbout());
}

void FillStickers(not_null<Ui::VerticalLayout*> container) {
	const auto russian = UseRussianTexts();

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(russian ? u"Стикеры"_q : u"Stickers"_q));

	AddToggle(
		container,
		CrashStickers::Title(),
		CrashStickers::Enabled(),
		[](bool toggled) { CrashStickers::SetEnabled(toggled); });

	Ui::AddSkip(container);
	AddAbout(container, CrashStickers::About());
}

void FillStories(not_null<Ui::VerticalLayout*> container) {
	const auto russian = UseRussianTexts();

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(russian ? u"Истории"_q : u"Stories"_q));

	AddToggle(
		container,
		StoriesHiddenTitle(),
		StoriesHidden(),
		[](bool toggled) { SetStoriesHidden(toggled); });

	Ui::AddSkip(container);
	AddAbout(container, StoriesHiddenAbout());
}

[[nodiscard]] QString NotifyPreviewsAbout() {
	return UseRussianTexts()
		? u"Текст сообщения в push кладёт сервер Telegram, а не клиент. "
			"Переключатель запрещает ему это для всего аккаунта: в push "
			"остаётся имя отправителя, а текст подгружается на телефоне из "
			"самого сообщения. Настройка аккаунтная — она видна во всех "
			"остальных клиентах, и вернуть показ текста можно только из "
			"мобильного."_q
		: u"The text of a message is put into a push by the Telegram server, "
			"not by the client. This switch forbids that for the whole "
			"account: a push carries the sender's name, and the text is loaded "
			"on the phone from the message itself. It is an account setting — "
			"it shows up in every other client, and previews can only be "
			"turned back on from a mobile one."_q;
}

[[nodiscard]] QString NotificationsAbout() {
	return UseRussianTexts()
		? u"Уведомление рисует Windows, и его текст остаётся в Центре "
			"уведомлений — за пределами tdata, а значит и стирания. "
			"Переключатель оставляет имя отправителя, а текст заменяет "
			"заглушкой; имя и аватар убираются стоковыми флажками Telegram."_q
		: u"The notification is drawn by Windows, and its text stays in the "
			"Action Center — outside tdata, and so outside the wipe. This "
			"switch leaves the sender's name and replaces the text with a "
			"placeholder; the name and the avatar are taken off by the stock "
			"Telegram checkboxes."_q;
}

void FillNotifications(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	const auto russian = UseRussianTexts();
	const auto session = &controller->session();

	// Two switches, two descriptions. They are easy to mistake for one
	// another and they promise opposite halves of the same question: the
	// first is about what the Telegram servers are allowed to compose, the
	// second about what this computer is allowed to draw. One shared text
	// under both would have to open by saying that there is no network leak
	// on this path, which is true of the second and is exactly what the first
	// exists to fix.
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(russian ? u"Уведомления"_q : u"Notifications"_q));

	AddToggle(
		container,
		NotifyPreviewsTitle(),
		NotifyPreviewsWithheld(session),
		[=](bool toggled) { SetNotifyPreviewsWithheld(session, toggled); });

	Ui::AddSkip(container);
	AddAbout(container, NotifyPreviewsAbout());

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(russian
			? u"Содержимое уведомлений"_q
			: u"Notification contents"_q));

	AddToggle(
		container,
		HideNotificationContentTitle(),
		HideNotificationContentEnabled(),
		[](bool toggled) {
			SetHideNotificationContentEnabled(toggled);
		});

	Ui::AddSkip(container);
	AddAbout(container, NotificationsAbout());
}

void FillSending(not_null<Ui::VerticalLayout*> container) {
	const auto russian = UseRussianTexts();

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(russian ? u"Отправка сообщений"_q : u"Sending messages"_q));

	const auto shown = container->lifetime().make_state<rpl::variable<bool>>(
		NightSilentEnabled());
	AddToggle(
		container,
		NightSilentTitle() + u" (22:00 – 07:00)"_q,
		NightSilentEnabled(),
		[=](bool toggled) {
			SetNightSilentEnabled(toggled);
			*shown = toggled;
		});

	const auto wrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	wrap->toggleOn(shown->value());
	wrap->finishAnimating();

	const auto inner = wrap->entity();
	AddToggle(
		inner,
		russian ? u"Личные диалоги"_q : u"Private chats"_q,
		NightSilentForUsers(),
		[](bool toggled) { SetNightSilentForUsers(toggled); });
	AddToggle(
		inner,
		russian ? u"Группы"_q : u"Groups"_q,
		NightSilentForGroups(),
		[](bool toggled) { SetNightSilentForGroups(toggled); });
	AddToggle(
		inner,
		russian ? u"Каналы"_q : u"Channels"_q,
		NightSilentForChannels(),
		[](bool toggled) { SetNightSilentForChannels(toggled); });

	Ui::AddSkip(container);
	AddAbout(container, NightSilentAbout());
}

[[nodiscard]] QString UpdateAbout() {
	return UseRussianTexts()
		? u"Не чаще раза в восемь часов NovaGram спрашивает api.github.com о "
			"последней версии — единственный сетевой запрос мимо Telegram, по "
			"нему видно только то, что клиент запущен. Установщик проверяется "
			"по контрольной сумме и по подписи NovaGram и заменяет только "
			"программу: данные, профили и PIN остаются на месте."_q
		: u"At most once every eight hours NovaGram asks api.github.com about "
			"the latest version — the only network request it makes outside "
			"Telegram, and all it reveals is that the client is running. The "
			"installer is checked against a checksum and against the NovaGram "
			"signature, and replaces only the program: data, profiles and PINs "
			"stay where they are."_q;
}

// The line under the switch, the way the official client writes it: what the
// checker is doing right now, or the version that is installed when it is doing
// nothing. The action moved out of this line and onto the blue button below,
// which is why "found" and "ready" read as states here and not as offers.
[[nodiscard]] QString UpdateStateText(const Update::Status &status) {
	const auto russian = UseRussianTexts();
	switch (status.phase) {
	case Update::Phase::Checking:
		return russian ? u"Проверка…"_q : u"Checking…"_q;
	case Update::Phase::Found:
		return (russian ? u"Доступна версия "_q : u"Version available: "_q)
			+ status.release.version;
	case Update::Phase::Downloading:
		return (russian ? u"Загрузка… "_q : u"Downloading… "_q)
			+ QString::number(status.progress)
			+ u"%"_q;
	case Update::Phase::Ready:
		return russian
			? u"Новая версия готова к установке"_q
			: u"A new version is ready to install"_q;
	case Update::Phase::Failed:
		// Four different failures wear the same phase. Two of them have
		// nothing to do with the connection, and sending the user to look at
		// their network would be wrong in both - especially for the refused
		// installer, which is the one case where they have to know that the
		// client stopped an update on purpose.
		switch (status.failure) {
		case Update::Failure::Rejected:
			return russian
				? u"Обновление отклонено: установщик не прошёл проверку"_q
				: u"Update rejected: the installer failed verification"_q;
		case Update::Failure::RateLimited:
			// Worded for both halves: the same answer comes from the API when
			// too many checks were made and from the asset host when it
			// refuses a download outright.
			return russian
				? u"GitHub временно ограничил обращения, попробуйте позже"_q
				: u"GitHub is limiting requests for now, try again later"_q;
		}
		// Telling the user that checking went wrong when the check succeeded
		// and the download did not would send them looking in the wrong place.
		return status.release.url.isEmpty()
			? (russian
				? u"Не удалось проверить обновления"_q
				: u"Could not check for updates"_q)
			: (russian
				? u"Не удалось загрузить обновление"_q
				: u"Could not download the update"_q);
	}
	return (russian ? u"Установлена версия "_q : u"Version installed: "_q)
		+ AppVersion();
}

// Built to the same shape as the official update block in
// settings/sections/settings_advanced.cpp: a taller switch with the state line
// drawn inside it, and a blue button that covers the "check now" row as soon as
// there is something to do. Upstream downloads by itself and therefore only
// ever shows that button as "restart"; this checker downloads on request, so
// the same button walks through offer, progress and install.
void FillUpdates(not_null<Ui::VerticalLayout*> container) {
	const auto russian = UseRussianTexts();

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(russian ? u"Обновления"_q : u"Updates"_q));

	const auto shown = container->lifetime().make_state<rpl::variable<bool>>(
		Update::CheckEnabled());
	const auto toggle = AddToggle(
		container,
		russian ? u"Проверять обновления"_q : u"Check for updates"_q,
		Update::CheckEnabled(),
		[=](bool toggled) {
			Update::SetCheckEnabled(toggled);
			*shown = toggled;
		},
		st::settingsUpdateToggle);

	const auto state = Ui::CreateChild<Ui::FlatLabel>(
		toggle,
		Update::StatusValue() | rpl::map(UpdateStateText),
		st::settingsUpdateState);
	state->setAttribute(Qt::WA_TransparentForMouseEvents);
	rpl::combine(
		toggle->widthValue(),
		state->widthValue()
	) | rpl::on_next([=] {
		state->moveToLeft(
			st::settingsUpdateStatePosition.x(),
			st::settingsUpdateStatePosition.y());
	}, state->lifetime());

	const auto options = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	options->toggleOn(shown->value());
	options->finishAnimating();

	const auto inner = options->entity();
	const auto check = inner->add(
		object_ptr<Ui::SettingsButton>(
			inner,
			rpl::single(russian ? u"Проверить сейчас"_q : u"Check now"_q),
			st::settingsButtonNoIcon));
	check->setClickedCallback([] { Update::CheckNow(); });

	// Covers the row above it rather than being added next to it, so that the
	// block does not change height when an update appears. That is upstream's
	// trick and the reason st::settingsUpdate exists.
	const auto update = Ui::CreateChild<Ui::SettingsButton>(
		check,
		Update::StatusValue() | rpl::map(Update::ActionText),
		st::settingsUpdate);
	update->hide();
	check->widthValue() | rpl::on_next([=](int width) {
		update->resizeToWidth(width);
		update->moveToLeft(0, 0);
	}, update->lifetime());
	update->setClickedCallback([] { Update::ActOnBar(); });

	Update::StatusValue(
	) | rpl::on_next([=](const Update::Status &status) {
		update->setVisible(Update::BarVisible(status));
	}, update->lifetime());

	const auto notes = AddButtonWithIcon(
		inner,
		rpl::single(russian ? u"Что нового"_q : u"What's new"_q),
		st::settingsButton,
		{ &st::menuIconInfo });
	notes->setClickedCallback([] {
		const auto release = Update::Current().release;
		File::OpenUrl(release.releaseUrl.isEmpty()
			? ReleaseUrl()
			: release.releaseUrl);
	});

	Ui::AddSkip(container);
	AddAbout(container, UpdateAbout());
}

class NovaGramSection final : public Section<NovaGramSection> {
public:
	NovaGramSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

};

NovaGramSection::NovaGramSection(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> NovaGramSection::title() {
	return rpl::single(SettingsSectionTitle());
}

void NovaGramSection::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);
	build(content, [](
			not_null<Ui::VerticalLayout*> container,
			not_null<Window::SessionController*> controller,
			Fn<void(Type)> showOther,
			rpl::producer<> showFinished) {
		Ui::AddSkip(container);
		FillProtection(container, controller);
		FillAutoDelete(container, controller);
		FillReadStatus(container, controller);
		FillCalls(container);
		FillDoh(container, controller);
		FillFiles(container);
		FillNotifications(container, controller);
		FillSending(container);
		FillStickers(container);
		FillStories(container);
		FillIcon(container);
		if (!Decoy::Active()) {
			// The decoy never checks for updates and must not offer a row
			// that would name the fork by its release page.
			FillUpdates(container);
		}
		Ui::AddSkip(container);
	});
	Ui::ResizeFitChild(this, content);
}

} // namespace

Settings::Type SettingsSectionId() {
	return NovaGramSection::Id();
}

QString SettingsSectionTitle() {
	return u"NovaGram"_q;
}

// On by default. A pref that was never touched has no record at all, so
// the contents stay hidden for everyone who never opened the section,
// while an explicit "off" is a stored false and stays off.
bool HideNotificationContentEnabled() {
	// This used to return a hard-coded false, reasoning that the desktop
	// composes the notification itself and no push service ever sees the
	// text. The first half is true; the conclusion was wrong for Windows.
	// The client does not draw the notification - it hands the title, the
	// subtitle and the whole body to the system, and Windows copies all
	// three into its own Action Center store under the user profile. That
	// store is outside tdata, so it is outside the local key, outside the
	// device binding and outside the emergency wipe, and it keeps them for
	// days. The third party is the operating system.
	return Core::App().settings().readPref<bool>(
		kHideNotificationContentKey,
		true);
}

void SetHideNotificationContentEnabled(bool enabled) {
	Core::App().settings().writePref<bool>(
		kHideNotificationContentKey,
		enabled);
	Core::App().saveSettingsDelayed();
	// What is already on the screen was built with the previous answer and
	// would go on showing the contents. For the native manager this also
	// takes those notifications out of the system notification centre,
	// which is the last moment the application can still reach them.
	Core::App().notifications().updateAll();
}

QString HideNotificationContentTitle() {
	return UseRussianTexts()
		? u"Скрывать содержимое уведомлений"_q
		: u"Hide notification contents"_q;
}

} // namespace NovaGram
