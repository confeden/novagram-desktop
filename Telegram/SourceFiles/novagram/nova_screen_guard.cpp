/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_screen_guard.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "logs.h"

#include <QtCore/QEvent>
#include <QtCore/QTimer>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

namespace NovaGram {
namespace {

constexpr auto kEnabledKey = "novagram_screen_guard"_cs;

#ifdef Q_OS_WIN

// Never QWidget::winId() here, however much it looks like the way to get the
// handle: it creates the native window when there is none, and this filter is
// called at the one moment when that is fatal. QWidget::destroy() clears
// WA_WState_Created first and sends QEvent::WinIdChange with a null id after
// (Qt-5.15.19 qwidget.cpp:12172 and 12194), so answering that event with
// winId() builds a fresh native window in the middle of taking the old one
// down, and nothing owns what it builds. That is what left the empty white
// frames titled "TelegramDesktop" standing on the screen from the launch until
// the application quit, one per Ui::GL::CheckCapabilities call site: its probe
// widget lives on the stack and is meant to take its window with it
// (lib_ui/ui/gl/gl_detection.cpp). Reported by the user 2026-08-09.
//
// Creating a handle ahead of time is just as bad from the other end: the probe
// takes down only the handle it made itself ("if
// (!tester.window()->windowHandle())" there), so one made for it beforehand
// stays for good.
//
// A window with no native handle also has nothing to hide, so nothing is lost
// by leaving it alone until it gets one.
void ApplyToWindow(not_null<QWidget*> widget) {
	const auto window = widget->windowHandle();
	if (!window || !window->handle()) {
		return;
	}
	const auto handle = reinterpret_cast<HWND>(window->winId());
	if (!handle) {
		return;
	}
	const auto enabled = ScreenGuardEnabled();
	const auto affinity = enabled
		? WDA_EXCLUDEFROMCAPTURE
		: WDA_NONE;
	const auto ok = SetWindowDisplayAffinity(handle, affinity);
	DEBUG_LOG(("Screen guard: window %1, enabled %2, applied %3, error %4."
		).arg(QString::number(reinterpret_cast<quintptr>(handle), 16)
		).arg(Logs::b(enabled)
		).arg(Logs::b(ok)
		).arg(ok ? 0 : int(GetLastError())));
	if (!ok && (affinity == WDA_EXCLUDEFROMCAPTURE)) {
		// WDA_EXCLUDEFROMCAPTURE needs Windows 10 2004 or newer. On anything
		// older the window can still be hidden from capture, but the recording
		// gets a black rectangle instead of nothing at all.
		SetWindowDisplayAffinity(handle, WDA_MONITOR);
	}
}

// The window that asks the video card what it can do stands in the middle of
// the desktop for as long as the driver takes to answer - about two seconds on
// a cold start - as an empty white frame titled "TelegramDesktop", reported by
// the user 2026-08-09. It cannot be destroyed early, the answer needs it, but
// where it stands is nobody's business: Ui::GL::CheckCapabilities only ever
// grabs a frame from it (lib_ui/ui/gl/gl_detection.cpp).
//
// It is recognised by its class and not by a guess: every call site passes
// nullptr as the parent, so the probe is a top level QOpenGLWidget, and that
// class does have Q_OBJECT, so the name is its own. Nothing else in the
// application makes a top level QOpenGLWidget - the surfaces of the media
// viewer are children of their window.
// The probe brings a second window along, and that one reports itself as a
// plain QWidget. A class without Q_OBJECT answers with the name of its nearest
// ancestor that has one, so the name alone would also catch windows of the
// application: the icon refresher of MainWindow::forceIconRefresh, the sample
// of the notifications settings, the windows of the crash reporter, the hidden
// helpers that hold an HWND to hear from the system. Two more conditions tell
// them apart, and both are properties of a window nobody means to show: it has
// no parent widget, and neither show() nor hide() was ever called on it -
// QWidget::setVisible sets WA_WState_ExplicitShowHide before anything else, so
// every window of the application carries that attribute by the time it is on
// the screen. The probe only ever asks for a handle.
[[nodiscard]] bool IsProbeWindow(not_null<QWidget*> widget) {
	if (!widget->isWindow()) {
		return false;
	} else if (!qstrcmp(widget->metaObject()->className(), "QOpenGLWidget")) {
		// The probe itself, while it holds the window and waits for an answer.
		return true;
	} else if (qstrcmp(widget->metaObject()->className(), "QWidget")
		|| widget->isVisible()) {
		// The class matters as much as the visibility. A window of the
		// application is at least a Ui::RpWidget, and the main window is
		// "hidden" for all of the time it spends in the tray - moving that one
		// would send it off the screen and leave the user hunting for it.
		return false;
	}
	// What the probe holds and leaves behind. Measured on a running client
	// 2026-08-09: class QWidget, no parent, 640x480 in the middle of the
	// screen, a native window Qt calls hidden - and the compositor draws it all
	// the same. Qt's own answer is the whole test, and it is what tells this
	// window from the windows of the application: a window Telegram shows is
	// visible to Qt at that moment, every one of them - the icon refresher of
	// the tray, the sample of the notification settings, the windows of the
	// crash reporter. Moving a window nobody shows changes nothing for anyone,
	// and whoever might show it later gives it a place first, the way all of
	// those do.
	const auto window = widget->windowHandle();
	return window && window->handle();
}

void MoveProbeAway(not_null<QWidget*> widget) {
	if (!IsProbeWindow(widget)) {
		return;
	}
	const auto window = widget->windowHandle();
	if (!window || !window->handle()) {
		return;
	}
	const auto handle = reinterpret_cast<HWND>(window->winId());
	if (!handle) {
		return;
	}
	DEBUG_LOG(("Screen guard: probe window %1x%2 moved out of sight."
		).arg(widget->width()).arg(widget->height()));
	SetWindowPos(
		handle,
		nullptr,
		-32000,
		-32000,
		0,
		0,
		SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOREDRAW);
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
					MoveProbeAway(widget);
					ApplyToWindow(widget);
					if (type == QEvent::Show
						&& (!widget->windowHandle()
							|| !widget->windowHandle()->handle())) {
						// Qt makes the native window after it sends Show, so
						// a window being shown for the first time has nothing
						// to protect yet. Asking again on the next turn of the
						// event loop catches it as soon as it is real, without
						// building a window that was never asked for.
						const auto guarded = QPointer<QWidget>(widget);
						QMetaObject::invokeMethod(qApp, [=] {
							if (const auto strong = guarded.data()) {
								ApplyToWindow(strong);
							}
						}, Qt::QueuedConnection);
					}
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

// There is deliberately no sweep over QApplication::topLevelWidgets() hunting
// for windows the probe might have left behind, and a plain QWidget must never
// be taken for such a leftover. A class without Q_OBJECT reports the class name
// of its nearest ancestor that has one, so className() == "QWidget" holds for
// the icon refresher of MainWindow::forceIconRefresh()
// (platform/win/main_window_win.cpp), for NotificationsCount::SampleWidget
// (settings/sections/settings_notifications.cpp), for PreLaunchWindow and its
// heirs (core/crash_report_window.h) and for the hidden helpers that keep an
// HWND only to hear from the system - BatterySaving and SystemMediaControls in
// lib_base/base/platform/win. The probe of Ui::GL::CheckCapabilities is the one
// thing the name does not point at: every call site passes nullptr, so its
// tester is a top level QOpenGLWidget, and that class does have Q_OBJECT.
// Moving or destroying windows on such a guess only ever reaches the
// application's own.

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

		// A probe made before this filter existed leaves no event to answer,
		// and its window is on the screen for as long as the driver takes.
		// Nothing here is ever destroyed - a window is only taken out of
		// sight, and one that nobody ever shows is not missed anywhere.
		const auto timer = new QTimer(application);
		const auto until = crl::now() + 20 * crl::time(1000);
		const auto reported = std::make_shared<base::flat_set<QWidget*>>();
		timer->callOnTimeout([=] {
			for (const auto widget : QApplication::topLevelWidgets()) {
				MoveProbeAway(widget);
				if (Logs::DebugEnabled()
					&& reported->emplace(widget).second) {
					const auto window = widget->windowHandle();
					const auto native = (window && window->handle());
					const auto handle = native
						? reinterpret_cast<HWND>(window->winId())
						: nullptr;
					auto title = std::array<wchar_t, 64>{ 0 };
					auto klass = std::array<wchar_t, 64>{ 0 };
					if (handle) {
						GetWindowTextW(handle, title.data(), 63);
						GetClassNameW(handle, klass.data(), 63);
					}
					DEBUG_LOG(("Top level: class '%1' name '%2' %3x%4 at %5,%6"
						", parent %7, explicit show %8, created %9,"
						" qt visible %10, flags %11, native %12,"
						" win32 visible %13, win32 title '%14', win32 class"
						" '%15'."
						).arg(widget->metaObject()->className()
						).arg(widget->objectName()
						).arg(widget->width()).arg(widget->height()
						).arg(widget->x()).arg(widget->y()
						).arg(Logs::b(widget->parentWidget() != nullptr)
						).arg(Logs::b(widget->testAttribute(
							Qt::WA_WState_ExplicitShowHide))
						).arg(Logs::b(widget->testAttribute(
							Qt::WA_WState_Created))
						).arg(Logs::b(widget->isVisible())
						).arg(QString::number(
							quint32(widget->windowFlags()), 16)
						).arg(Logs::b(native)
						).arg(Logs::b(handle && IsWindowVisible(handle))
						).arg(QString::fromWCharArray(title.data())
						).arg(QString::fromWCharArray(klass.data())));
				}
			}
			if (crl::now() >= until) {
				timer->stop();
				timer->deleteLater();
			}
		});
		timer->start(100);
	}
#endif // Q_OS_WIN
}

} // namespace NovaGram
