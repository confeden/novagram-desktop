/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_screen_guard.h"

#include "core/application.h"
#include "core/core_settings.h"

#include <QtCore/QEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

namespace NovaGram {
namespace {

constexpr auto kEnabledKey = "novagram_screen_guard"_cs;

#ifdef Q_OS_WIN

void ApplyToWindow(not_null<QWidget*> widget) {
	const auto handle = reinterpret_cast<HWND>(widget->winId());
	if (!handle) {
		return;
	}
	const auto affinity = ScreenGuardEnabled()
		? WDA_EXCLUDEFROMCAPTURE
		: WDA_NONE;
	if (!SetWindowDisplayAffinity(handle, affinity)
		&& (affinity == WDA_EXCLUDEFROMCAPTURE)) {
		// WDA_EXCLUDEFROMCAPTURE needs Windows 10 2004 or newer. On anything
		// older the window can still be hidden from capture, but the recording
		// gets a black rectangle instead of nothing at all.
		SetWindowDisplayAffinity(handle, WDA_MONITOR);
	}
}

class Watcher final : public QObject {
public:
	explicit Watcher(QObject *parent) : QObject(parent) {
	}

protected:
	bool eventFilter(QObject *object, QEvent *event) override {
		const auto type = event->type();
		if (type == QEvent::Show || type == QEvent::WinIdChange) {
			if (const auto widget = qobject_cast<QWidget*>(object)) {
				if (widget->isWindow()) {
					ApplyToWindow(widget);
				}
			}
		}
		return QObject::eventFilter(object, event);
	}

};

void ApplyToAllWindows() {
	for (const auto widget : QApplication::topLevelWidgets()) {
		if (widget->isWindow() && widget->windowHandle()) {
			ApplyToWindow(widget);
		}
	}
}

#endif // Q_OS_WIN

} // namespace

bool ScreenGuardSupported() {
#ifdef Q_OS_WIN
	return true;
#else // Q_OS_WIN
	return false;
#endif // Q_OS_WIN
}

bool ScreenGuardEnabled() {
	if (!ScreenGuardSupported()) {
		return false;
	}
	return Core::App().settings().readPref<bool>(kEnabledKey, true);
}

void SetScreenGuardEnabled(bool enabled) {
	Core::App().settings().writePref<bool>(kEnabledKey, enabled);
	Core::App().saveSettingsDelayed();
#ifdef Q_OS_WIN
	ApplyToAllWindows();
#endif // Q_OS_WIN
}

void StartScreenGuard() {
#ifdef Q_OS_WIN
	if (const auto application = QApplication::instance()) {
		application->installEventFilter(new Watcher(application));
	}
#endif // Q_OS_WIN
}

} // namespace NovaGram
