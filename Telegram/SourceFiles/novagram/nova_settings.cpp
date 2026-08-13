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
#include "novagram/nova_filenames.h"
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
#include "settings/settings_common_session.h"
#include "ui/layers/generic_box.h"
#include "ui/ui_utility.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
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
	Ui::AddDividerText(container, rpl::single(AutoDeleteAbout()));
}

[[nodiscard]] QString ReadStatusAbout() {
	return UseRussianTexts()
		? u"Когда вам пишет незнакомец, а вы ещё не отвечали, галочки "
			"прочтения ему не отправляются. Правило заводится по первому "
			"такому сообщению; группы, каналы, боты и «Избранное» не "
			"затрагиваются. Любой ваш ответ снимает скрытие, а для Telegram "
			"такая переписка остаётся непрочитанной."_q
		: u"When a stranger writes to you and you have not answered yet, the "
			"read marks are never sent to them. The rule is made on the first "
			"such message; groups, channels, bots and Saved Messages are left "
			"alone. Any answer of yours lifts it, and for Telegram such a "
			"conversation stays unread."_q;
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
	Ui::AddDividerText(container, rpl::single(FileNamesAbout()));

	Ui::AddSkip(container);
	AddToggle(
		container,
		StripMetadataTitle(),
		StripMetadataEnabled(),
		[](bool toggled) { SetStripMetadataEnabled(toggled); });

	Ui::AddSkip(container);
	Ui::AddDividerText(container, rpl::single(StripMetadataAbout()));
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
		? u"На ПК текст уведомления собирается здесь, из уже полученного "
			"сообщения, и никуда не отправляется — скрывать его не от кого, "
			"поэтому переключатель неактивен. Убрать имя и текст с экрана "
			"можно стоковыми флажками в настройках уведомлений Telegram."_q
		: u"On the desktop the text of a notification is composed here, out of "
			"a message already received, and is sent nowhere — there is no one "
			"to hide it from, which is why this switch is inactive. The stock "
			"Name and Text checkboxes in the Telegram notification settings "
			"take the name and the text off the screen."_q;
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
		? u"Не чаще раза в восемь часов NovaGram читает файл о последней версии "
			"на raw.githubusercontent.com — единственный сетевой запрос мимо "
			"Telegram, по нему видно только то, что клиент запущен. "
			"Установщик проверяется по контрольной сумме и заменяет только "
			"программу: данные, профили и PIN остаются на месте."_q
		: u"At most once every eight hours NovaGram reads a file with the "
			"latest version from raw.githubusercontent.com — the only network "
			"request it makes outside Telegram, and all it reveals is that the "
			"client is running. The installer is verified against a checksum "
			"and replaces only the program: data, profiles and PINs stay where "
			"they are."_q;
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
		// Two different failures wear the same phase, and telling the user
		// that checking went wrong when the check succeeded and the download
		// did not would send them looking in the wrong place.
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
		FillFiles(container);
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
