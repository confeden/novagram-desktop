/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_settings.h"

#include "main/main_session.h"
#include "novagram/nova_autodelete.h"
#include "novagram/nova_night_silent.h"
#include "novagram/nova_pin.h"
#include "novagram/nova_pin_box.h"
#include "novagram/nova_read_status.h"
#include "novagram/nova_screen_guard.h"
#include "settings/settings_common_session.h"
#include "ui/layers/generic_box.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace NovaGram {
namespace {

using namespace Settings;

[[nodiscard]] QString PinAbout() {
	return UseRussianTexts()
		? u"Основной PIN шифрует локальные данные NovaGram: без него база "
			"на диске не читается. Аварийный PIN вводится под принуждением и "
			"вместо разблокировки уничтожает локальные данные.\n\nПри "
			"включённом режиме PIN разблокировка через Windows Hello "
			"отключается: она открывала бы NovaGram в обход аварийного "
			"PIN."_q
		: u"The primary PIN encrypts the local NovaGram data: without it the "
			"database on disk cannot be read. The emergency PIN is entered "
			"under coercion and destroys the local data instead of "
			"unlocking.\n\nWhile the PIN mode is on, the Windows Hello unlock "
			"is disabled: it would open NovaGram bypassing the emergency "
			"PIN."_q;
}

[[nodiscard]] QString KeypadAbout() {
	return UseRussianTexts()
		? u"Экранная клавиатура позволяет ввести PIN мышкой, а её цифры "
			"каждый раз располагаются в новом порядке, поэтому по движению "
			"курсора или по следам на экране нельзя восстановить PIN."_q
		: u"The on-screen keypad allows entering the PIN with the mouse, and "
			"its digits are laid out in a new order every time, so the PIN "
			"cannot be recovered from cursor movements or screen smudges."_q;
}

[[nodiscard]] QString ScreenGuardAbout() {
	return UseRussianTexts()
		? u"Окна NovaGram исключаются из захвата экрана: снимок экрана, "
			"запись и демонстрация экрана не увидят содержимое переписки. "
			"Защита не действует против камеры, направленной на монитор, и "
			"против программ уровня ядра. Выключайте её, только если "
			"действительно нужно показать NovaGram на записи."_q
		: u"NovaGram windows are excluded from screen capture: screenshots, "
			"recording and screen sharing will not see the conversation. It "
			"does not protect against a camera pointed at the monitor or "
			"against kernel level software. Turn it off only when NovaGram "
			"really has to be visible in a recording."_q;
}

[[nodiscard]] QString NightSilentAbout() {
	return UseRussianTexts()
		? u"С 22:00 до 07:00 по местному времени этого устройства исходящие "
			"сообщения отправляются без звука уведомления у получателя. "
			"Сообщение доставляется как обычно и видно в чате, беззвучным "
			"становится только уведомление. Время проверяется в момент "
			"отправки, поэтому отложенное сообщение получает признак по "
			"времени фактической отправки."_q
		: u"Between 22:00 and 07:00 in the local time of this device, "
			"outgoing messages are sent without a notification sound for the "
			"recipient. The message is delivered as usual and is visible in "
			"the chat, only the notification becomes silent. The time is "
			"checked at the moment of sending, so a scheduled message follows "
			"the time it is actually sent."_q;
}

not_null<Button*> AddToggle(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		bool checked,
		Fn<void(bool)> changed) {
	const auto button = AddButtonWithIcon(
		container,
		rpl::single(text),
		st::settingsButtonNoIcon);
	button->toggleOn(rpl::single(checked));
	button->toggledChanges(
	) | rpl::on_next([=](bool toggled) {
		changed(toggled);
	}, button->lifetime());
	return button;
}

void FillProtection(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	const auto russian = UseRussianTexts();

	Ui::AddSubsectionTitle(
		container,
		rpl::single(russian ? u"Защита"_q : u"Protection"_q));

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
	Ui::AddDividerText(container, rpl::single(PinAbout()));
	Ui::AddSkip(container);

	AddToggle(
		container,
		(russian
			? u"Экранная клавиатура со случайным порядком"_q
			: u"On-screen keypad in a random order"_q),
		ShuffledKeypadEnabled(),
		[](bool toggled) { SetShuffledKeypadEnabled(toggled); });

	Ui::AddSkip(container);
	Ui::AddDividerText(container, rpl::single(KeypadAbout()));

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
		Ui::AddDividerText(container, rpl::single(ScreenGuardAbout()));
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
		? u"Свои отправленные сообщения удаляются по истечении срока: сначала "
			"текст заменяется на «.», через минуту сообщение удаляется у обеих "
			"сторон. Если сообщение уже нельзя отредактировать, оно просто "
			"удаляется. Медиа удаляются без замены текста.\n\nОтсчёт идёт от "
			"момента отправки. Для сообщений, отправленных до включения "
			"функции, — от момента включения. В чатах, где у вас есть права "
			"администратора, функция по умолчанию не применяется."_q
		: u"Your own sent messages are removed once the period passes: first "
			"the text is replaced with \".\", a minute later the message is "
			"deleted for both sides. A message that can no longer be edited is "
			"simply deleted. Media is deleted without the text step.\n\nThe "
			"countdown starts when the message is sent, and for messages that "
			"existed before the feature was switched on, from the moment it "
			"was switched on. Chats where you have admin rights are excluded "
			"by default."_q;
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
	Ui::AddDividerText(container, rpl::single(AutoDeleteAbout()));
}

[[nodiscard]] QString ReadStatusAbout() {
	return UseRussianTexts()
		? u"В диалогах, которые начали не вы, NovaGram не сообщает "
			"собеседнику о прочтении: галочки прочтения у него не "
			"появляются. Настройка применяется только к новым диалогам, "
			"уже существующие переписки не затрагиваются.\n\nПобочный эффект: "
			"для Telegram сообщения остаются непрочитанными, поэтому счётчик "
			"непрочитанного может возвращаться после перезапуска и на других "
			"устройствах. Отключить скрытие можно в меню конкретного диалога, "
			"и это необратимо."_q
		: u"In dialogs you did not start, NovaGram does not tell the other "
			"side that you read them: the read marks never appear for them. "
			"This applies to new dialogs only, existing conversations are "
			"left alone.\n\nSide effect: for Telegram the messages stay "
			"unread, so the unread counter can come back after a restart and "
			"on other devices. Hiding can be turned off from the menu of a "
			"particular dialog, and that cannot be undone."_q;
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
	Ui::AddDividerText(container, rpl::single(ReadStatusAbout()));
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
	Ui::AddDividerText(container, rpl::single(NightSilentAbout()));
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
		FillSending(container);
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

} // namespace NovaGram
