/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_pin_policy.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "main/main_domain.h"
#include "novagram/nova_pin.h"
#include "storage/storage_domain.h"

namespace NovaGram {
namespace {

// Spelled exactly as on Android, so that a value written by one description of
// the feature is read by the other. The key itself is the fork's, not
// upstream's, and lives next to the other novagram_ prefs.
constexpr auto kPolicyKey = "novagram_pin_lock_policy"_cs;

constexpr auto kInactivity10m = 10 * 60;
constexpr auto kInactivity60m = 60 * 60;
constexpr auto kSessionCeilingMs = crl::time(24 * 60 * 60 * 1000);

[[nodiscard]] QString StorageKey(PinLockPolicy policy) {
	switch (policy) {
	case PinLockPolicy::OnMinimize: return u"on_minimize"_q;
	case PinLockPolicy::OnScreenLock: return u"on_screen_lock"_q;
	case PinLockPolicy::Inactivity10m: return u"inactivity_10m"_q;
	case PinLockPolicy::Inactivity60m: return u"inactivity_60m"_q;
	case PinLockPolicy::OnStart: return u"on_start"_q;
	}
	return u"on_minimize"_q;
}

// Zero until the passcode is entered in this run. "Ask at start" is then
// answered by the ceiling being measured from a moment that never came, which
// is the same thing as "the session has not started yet".
crl::time PinUnlockedAt/* = 0*/;

[[nodiscard]] bool HasPasscode() {
	return Core::App().domain().local().hasLocalPasscode();
}

// Locking is only ever a tightening: without a passcode there is nothing to
// ask for, and locking a client that is already locked would just restart the
// screen under the user.
void LockNow() {
	if (HasPasscode() && !Core::App().passcodeLocked()) {
		Core::App().lockByPasscode();
	}
}

} // namespace

PinLockPolicy PinPolicy() {
	const auto stored = QString::fromUtf8(
		Core::App().settings().readPref<QByteArray>(kPolicyKey, QByteArray()));
	for (const auto policy : PinPolicyOptions()) {
		if (StorageKey(policy) == stored) {
			return policy;
		}
	}
	// Locking the screen is the moment a machine actually stops being under
	// its owner's eyes, so that is what the untouched setting answers. A pref
	// that was never written has no record at all, and an unreadable one falls
	// here too rather than to something weaker.
	return PinLockPolicy::OnScreenLock;
}

void SetPinPolicy(PinLockPolicy policy) {
	Core::App().settings().writePref<QByteArray>(
		kPolicyKey,
		StorageKey(policy).toUtf8());
	// The two inactivity options are served by the stock machinery, so the
	// stock Auto-lock row is kept honest rather than left showing a number
	// that stopped meaning anything.
	if (const auto seconds = PinPolicyAutoLockSeconds()) {
		Core::App().settings().setAutoLock(seconds);
	}
	Core::App().saveSettingsDelayed();
	// The unlock stands - it was granted by a correct passcode - but the
	// deadlines it is measured against are new.
	Core::App().checkAutoLock(crl::now());
}

std::vector<PinLockPolicy> PinPolicyOptions() {
	return {
		PinLockPolicy::OnMinimize,
		PinLockPolicy::OnScreenLock,
		PinLockPolicy::Inactivity10m,
		PinLockPolicy::Inactivity60m,
		PinLockPolicy::OnStart,
	};
}

int PinPolicyAutoLockSeconds() {
	switch (PinPolicy()) {
	case PinLockPolicy::Inactivity10m: return kInactivity10m;
	case PinLockPolicy::Inactivity60m: return kInactivity60m;
	default: return 0;
	}
}

void NotePinUnlocked() {
	PinUnlockedAt = crl::now();
}

void NotePinWindowHidden() {
	if (PinPolicy() == PinLockPolicy::OnMinimize) {
		LockNow();
	}
}

void NotePinScreenLocked() {
	if (PinPolicy() == PinLockPolicy::OnScreenLock) {
		LockNow();
	}
}

std::optional<crl::time> PinSessionRemaining() {
	if (PinPolicy() != PinLockPolicy::OnStart) {
		return std::nullopt;
	} else if (!PinUnlockedAt) {
		// Nothing was unlocked in this run, so there is no session to age.
		// The passcode screen at start is upstream's and is not this file's to
		// bring about.
		return std::nullopt;
	}
	return kSessionCeilingMs - (crl::now() - PinUnlockedAt);
}

QString PinPolicyTitle() {
	return UseRussianTexts()
		? u"Запрашивать PIN"_q
		: u"Ask for the PIN"_q;
}

QString PinPolicyName(PinLockPolicy policy) {
	const auto russian = UseRussianTexts();
	switch (policy) {
	case PinLockPolicy::OnMinimize:
		return russian
			? u"При каждом сворачивании окна"_q
			: u"Every time the window is minimised"_q;
	case PinLockPolicy::OnScreenLock:
		return russian
			? u"При каждой блокировке экрана"_q
			: u"Every time the screen is locked"_q;
	case PinLockPolicy::Inactivity10m:
		return russian
			? u"После 10 минут бездействия"_q
			: u"After 10 minutes of inactivity"_q;
	case PinLockPolicy::Inactivity60m:
		return russian
			? u"После 60 минут бездействия"_q
			: u"After 60 minutes of inactivity"_q;
	case PinLockPolicy::OnStart:
		return russian
			? u"Только при запуске программы"_q
			: u"Only when the application starts"_q;
	}
	return QString();
}

QString PinPolicyAbout() {
	return UseRussianTexts()
		? u"Выбранный вариант — единственное, что решает, когда PIN "
			"спросят снова. Работает ровно то, что выбрано:\n\n• при "
			"блокировке экрана (по умолчанию) — PIN спросят, как только "
			"заблокирован Windows;\n• при сворачивании — как только окно ушло "
			"с глаз, в том числе в трей;\n• через 10 или 60 минут "
			"бездействия — PIN спросят по истечении срока, даже если окно всё "
			"это время открыто;\n• только при запуске — один раз за запуск, и "
			"дополнительно через сутки, потому что программа может не "
			"закрываться неделями.\n\nСтоковая строка «Автоблокировка» в "
			"настройках конфиденциальности — это тот же счётчик бездействия: "
			"при двух вариантах со сроком там виден тот же срок, при "
			"остальных трёх она не участвует. Настройка общая с "
			"Android-версией NovaGram и называется там так же."_q
		: u"The chosen option is the only thing that decides when the PIN is "
			"asked for again. What is chosen is what happens:\n\n• on screen "
			"lock (the default) - the PIN is asked as soon as Windows is "
			"locked;\n• on minimising - as soon as the window leaves the "
			"screen, the tray included;\n• after 10 or 60 minutes of "
			"inactivity - the PIN is asked when the period is up, even if the "
			"window stayed open all that time;\n• only at start - once per "
			"launch, and again after a day, because the application can stay "
			"open for weeks.\n\nThe stock Auto-lock row in the privacy "
			"settings is the same idle counter: with the two timed options it "
			"shows the same period, with the other three it takes no part. "
			"The setting is shared with the Android NovaGram and is named the "
			"same there."_q;
}

} // namespace NovaGram
