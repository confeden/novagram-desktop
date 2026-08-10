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
#include "novagram/nova_decoy.h"
#include "novagram/nova_marquee_button.h"
#include "novagram/nova_update.h"
#include "novagram/nova_night_silent.h"
#include "novagram/nova_notify_previews.h"
#include "novagram/nova_pin.h"
#include "novagram/nova_pin_policy.h"
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
#include "window/notifications_manager.h"
#include "window/window_session_controller.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace NovaGram {
namespace {

using namespace Settings;

constexpr auto kHideNotificationContentKey
	= "novagram_hide_notification_content"_cs;

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
	// Not AddButtonWithIcon: these titles are long enough to be cut off, and
	// MarqueeButton scrolls the rest into view while the cursor is on the row.
	const auto button = container->add(
		object_ptr<MarqueeButton>(
			container,
			text,
			st::settingsButtonNoIcon));
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
	Ui::AddDividerText(container, rpl::single(PinPolicyAbout()));
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
			"удаляется; так же удаляются медиа — замена подписи оставила бы сам "
			"файл. Telegram обычно разрешает редактирование 48 часов, а это "
			"меньше здешнего срока, поэтому большинство сообщений удаляется без "
			"шага с заменой.\n\nОтсчёт идёт от момента отправки. Сообщения, "
			"отправленные до включения функции, в очередь не попадают: для них "
			"есть «Erase evidence» в меню чата. В чатах, где у вас есть права "
			"администратора, функция по умолчанию не применяется, а «Избранное» "
			"не трогается никогда."_q
		: u"Your own sent messages are removed once the period passes: first "
			"the text is replaced with \".\", a minute later the message is "
			"deleted for both sides. A message that can no longer be edited is "
			"simply deleted, and so is media: replacing a caption would leave "
			"the file. Telegram usually allows editing for 48 hours, which is "
			"shorter than the period here, so most messages are deleted without "
			"the text step.\n\nThe countdown starts when the message is sent. "
			"Messages sent before the feature was switched on are not queued; "
			"use Erase evidence in the chat menu for those. Chats where you "
			"have admin rights are excluded by default, and Saved Messages are "
			"never touched."_q;
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
		? u"Когда вам пишет незнакомец, а вы ещё не отвечали, NovaGram не "
			"сообщает ему о прочтении: галочки прочтения у него не "
			"появляются. Правило заводится в момент, когда такое сообщение "
			"приходит, и дальше не меняется; переписки, которые уже шли до "
			"включения настройки, она не затрагивает. Группы, каналы, боты и "
			"«Избранное» не затрагиваются никогда.\n\nПобочный эффект: "
			"для Telegram сообщения остаются непрочитанными, поэтому счётчик "
			"непрочитанного может возвращаться после перезапуска и на других "
			"устройствах. Отключить скрытие можно в самом диалоге — плашкой "
			"над перепиской или из меню чата, — и это необратимо. Скрытие "
			"снимается и само: любое ваше сообщение в таком диалоге, "
			"отправленное с любого устройства, снимает его — ответ и так "
			"говорит собеседнику, что вы прочитали. Черновик и служебные "
			"сообщения не в счёт."_q
		: u"When a stranger writes to you and you have not answered yet, "
			"NovaGram does not tell them that you read it: the read marks "
			"never appear for them. The rule is made when such a message "
			"arrives and does not change afterwards; conversations that were "
			"already going before the setting was switched on are left alone. "
			"Groups, channels, bots and Saved Messages are never touched."
			"\n\nSide effect: for Telegram the messages stay "
			"unread, so the unread counter can come back after a restart and "
			"on other devices. Hiding can be turned off in the dialog itself "
			"— from the note above the chat or from the chat menu — and that "
			"cannot be undone. It also stops by itself: any message you send "
			"to such a dialog, from any device, lifts it — an answer tells "
			"the other side that you read it anyway. Drafts and service "
			"messages do not count."_q;
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

[[nodiscard]] QString NotifyPreviewsAbout() {
	return UseRussianTexts()
		? u"Текст сообщения в push-уведомление кладёт не клиент, а Telegram: "
			"сервер составляет его у себя, шифрует и отдаёт Google или Apple "
			"для доставки на телефон. Переключатель говорит аккаунту так не "
			"делать — серверу уходит show_previews=false, и в push остаётся "
			"имя отправителя без текста. На телефоне текст появляется "
			"мгновением позже, уже из самого сообщения, которое клиент "
			"получает по своему соединению.\n\nНа этом компьютере не меняется "
			"ничего: у NovaGram для ПК своего push нет, всё приходит по её "
			"собственному соединению. Настройка принадлежит аккаунту, а не "
			"устройству, поэтому отправлять её обязан и ПК: аккаунт слушает "
			"того клиента, который сказал последним.\n\nЧем это оплачено. "
			"Настройка видна во всех остальных клиентах и на всех "
			"устройствах — там она выглядит как выключенный показ текста в "
			"уведомлениях. На телефоне при экономии батареи, в режиме Doze "
			"или без сети текст может не подгрузиться вовсе, и в уведомлении "
			"останется одно имя. Выключение переключателя ничего не "
			"возвращает: клиент знает только то значение, которое сам туда и "
			"положил. Показ текста включается обратно стоковыми настройками "
				"уведомлений Telegram — но не отсюда: на ПК такого "
				"переключателя нет вовсе, стоковые флажки «Имя» и «Текст» в "
				"настройках уведомлений меняют только вид уведомления на этом "
				"компьютере и серверу не отправляются. Вернуть показ текста "
				"можно из мобильного клиента. Тем же значением "
				"Telegram управляет уведомлениями о реакциях: в push на "
				"телефон не попадёт имя того, кто поставил реакцию — на этом "
				"компьютере оно по-прежнему видно.\n\nЧего это не чинит. "
			"Google и Apple по-прежнему видят, что push доставлен, когда и "
			"какого он размера. Чаты, для которых показ текста задан "
			"отдельно, перекрываются по мере того, как загружается список "
			"чатов."_q
		: u"The text of a message is put into a push notification not by the "
			"client but by Telegram: the server composes it, encrypts it and "
			"hands it to Google or Apple for delivery to the phone. This "
			"switch tells the account not to do that — the server is sent "
			"show_previews=false, and the push carries the sender's name "
			"without the text. On the phone the text appears a moment later, "
			"taken from the message itself, which the client receives over "
			"its own connection.\n\nOn this computer nothing changes: the "
			"desktop NovaGram has no push of its own, everything arrives over "
			"its own connection. The setting belongs to the account and not "
			"to a device, so the desktop is obliged to send it too: the "
			"account listens to whichever client spoke last.\n\nWhat it "
			"costs. The setting is visible in every other client and on every "
			"device, where it looks like message previews being turned off. "
			"On the phone, under battery saving, in Doze or without network "
			"the text may not load at all and the notification keeps only the "
			"name. Turning this switch off restores nothing: the client knows "
			"only the value it put there itself. Previews are turned back on "
				"from the stock Telegram notification settings — but not from "
				"here: the desktop has no such switch at all, and the stock "
				"Name and Text checkboxes in its notification settings "
				"only change how a notification looks on this computer and "
				"are never sent to the server. Previews are restored from a "
				"mobile client. Telegram governs the reaction notifications "
				"by the same value: a push to the phone will not carry the "
				"name of whoever reacted — on this computer it is still "
				"visible.\n\nWhat it does not fix. Google and Apple still see "
				"that a push "
			"was delivered, when, and how large it was. Chats with a preview "
			"setting of their own are overridden as the chat list loads."_q;
}

[[nodiscard]] QString NotificationsAbout() {
	return UseRussianTexts()
		? u"Текст уведомления NovaGram собирает здесь, из уже полученного "
			"сообщения, и никуда его не отправляет: у версии для ПК своего "
			"push нет вовсе, поэтому утечки текста уведомлений на сторонние "
			"серверы на этой платформе не происходит. Скрывать его от них "
			"нечего, и переключатель поэтому неактивен — текст уведомления "
			"показывается.\n\nЧто остаётся: уведомление видно тому, кто "
			"смотрит на экран, и оседает в центре уведомлений Windows, пока "
			"его не закроют. Убрать оттуда имя и текст можно стоковыми "
			"флажками «Имя» и «Текст» в настройках уведомлений Telegram. "
			"О тексте, который серверы Telegram кладут в push для телефона, "
			"речь в переключателе выше."_q
		: u"NovaGram composes the text of a notification here, out of a "
			"message it already has, and sends it nowhere: the desktop "
			"version has no push of its own, so on this platform the text of "
			"a notification does not leak to any third-party server. There is "
			"nothing to hide from them, which is why this switch is inactive "
			"and the text is shown.\n\nWhat remains: a notification is "
			"visible to whoever can see the screen and stays in the Windows "
			"notification centre until it is dismissed. The stock Name and "
			"Text checkboxes in the Telegram notification settings take those "
			"away. The text that the Telegram servers put into a push for the "
			"phone is what the switch above is about."_q;
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
	Ui::AddDividerText(container, rpl::single(NotifyPreviewsAbout()));

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(russian
			? u"Содержимое уведомлений"_q
			: u"Notification contents"_q));

	// Shown, and deliberately not usable. On this platform the text of a
	// notification is composed here, out of a message the client already
	// received over its own connection, and no push service ever sees it - so
	// there is nothing for this switch to protect against that the account
	// setting above does not already cover. It stays visible because the
	// Android fork has it and the two settings screens are read side by side;
	// a row that quietly disappeared would look like a feature that had been
	// lost. The divider under it says why it is grey.
	const auto hide = AddToggle(
		container,
		HideNotificationContentTitle(),
		HideNotificationContentEnabled(),
		[](bool) {});
	hide->setDisabled(true);

	Ui::AddSkip(container);
	Ui::AddDividerText(container, rpl::single(NotificationsAbout()));
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

[[nodiscard]] QString UpdateAbout() {
	return UseRussianTexts()
		? u"Раз в восемь часов NovaGram читает небольшой файл со сведениями о "
			"последней версии на raw.githubusercontent.com. Это единственный "
			"сетевой запрос NovaGram мимо Telegram, и по нему видно только "
			"то, что клиент запущен: ни аккаунт, ни переписка в нём не "
			"участвуют. Установщик проверяется по контрольной сумме из того "
			"же файла и заменяет только программу — данные, профили и PIN "
			"остаются на месте. В режиме заглушки проверка не выполняется "
			"вообще."_q
		: u"Every eight hours NovaGram reads a small file with the latest "
			"version from raw.githubusercontent.com. It is the only network "
			"request NovaGram makes outside Telegram, and all it reveals is "
			"that the client is running: no account and no conversation take "
			"part in it. The installer is verified against the checksum from "
			"the same file and replaces only the program — data, profiles and "
			"PINs stay where they are. In decoy mode the check does not run at "
			"all."_q;
}

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
			? u"Установить и перезапустить"_q
			: u"Install and restart"_q;
	case Update::Phase::Failed:
		return russian
			? u"Не удалось проверить обновления"_q
			: u"Could not check for updates"_q;
	case Update::Phase::UpToDate:
		return russian
			? u"Установлена последняя версия"_q
			: u"The latest version is installed"_q;
	}
	return russian ? u"Проверить обновления"_q : u"Check for updates"_q;
}

void FillUpdates(not_null<Ui::VerticalLayout*> container) {
	const auto russian = UseRussianTexts();

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		rpl::single(russian ? u"Обновления"_q : u"Updates"_q));

	AddToggle(
		container,
		russian ? u"Проверять обновления"_q : u"Check for updates"_q,
		Update::CheckEnabled(),
		[](bool toggled) { Update::SetCheckEnabled(toggled); });

	const auto state = AddButtonWithLabel(
		container,
		Update::StatusValue() | rpl::map(UpdateStateText),
		(Update::StatusValue()
			| rpl::map([](const Update::Status &status) {
				return (status.phase == Update::Phase::Found)
					? (UseRussianTexts() ? u"Скачать"_q : u"Download"_q)
					: AppVersion();
			})),
		st::settingsButton,
		{ &st::menuIconDownload });
	state->setClickedCallback([] {
		switch (Update::Current().phase) {
		case Update::Phase::Found: Update::Download(); return;
		case Update::Phase::Ready: Update::InstallAndRestart(); return;
		case Update::Phase::Checking:
		case Update::Phase::Downloading: return;
		}
		Update::CheckNow();
	});

	const auto notes = AddButtonWithIcon(
		container,
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
	Ui::AddDividerText(container, rpl::single(UpdateAbout()));
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
		FillNotifications(container, controller);
		FillSending(container);
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
	// Off, and not offered. The desktop composes the text of a notification
	// itself, from a message it already received over its own connection, and
	// has no push service of any kind - so on this platform the text of a
	// notification never reaches a third party and there is nothing here for
	// hiding it to protect. What it did protect against, a person looking at
	// the screen, is what the stock Name and Text checkboxes are for.
	//
	// The machinery below and its reader in notifications_manager are left in
	// place: the account-level promise above is the one that matters here, and
	// if this is ever wanted again it is one line.
	return false;
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
