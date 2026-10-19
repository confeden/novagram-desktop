/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "novagram/nova_terminator.h"

#include "base/random.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "logs.h"
#include "novagram/nova_decoy.h"
#include "novagram/nova_pin.h"
#include "novagram/nova_wss.h"
#include "settings.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>

#include <atomic>
#include <memory>
#include <mutex>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

namespace NovaGram::Terminator {
namespace {

constexpr auto kEnabledKey = "novagram_wss_yandex"_cs;
constexpr auto kStartTimeout = 6000;
constexpr auto kMaxRestarts = 3;

struct State {
	std::mutex mutex;
	std::optional<Endpoint> endpoint;
};

State &Shared() {
	static auto result = State();
	return result;
}

QString ExePath() {
#ifdef Q_OS_WIN
	return QCoreApplication::applicationDirPath()
		+ u"/nova-tls-terminator.exe"_q;
#else // Q_OS_WIN
	return QCoreApplication::applicationDirPath()
		+ u"/nova-tls-terminator"_q;
#endif // Q_OS_WIN
}

QString RootsPath() {
	return QCoreApplication::applicationDirPath() + u"/nova_roots.pem"_q;
}

std::atomic<bool> FellBackLogged = false;
QPointer<QProcess> Child;
QPointer<QTimer> Watchdog;
int Restarts = 0;
bool Wanted = false;

void Publish(std::optional<Endpoint> value) {
	auto &shared = Shared();
	const auto lock = std::lock_guard(shared.mutex);
	shared.endpoint = std::move(value);
}

#ifdef Q_OS_WIN
// The child dies with the client even when the client is killed, so a crash
// cannot leave a listener behind.
void BindToJob(qint64 pid) {
	static const auto job = [] {
		const auto handle = CreateJobObjectW(nullptr, nullptr);
		if (handle) {
			auto info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
			info.BasicLimitInformation.LimitFlags
				= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			SetInformationJobObject(
				handle,
				JobObjectExtendedLimitInformation,
				&info,
				sizeof(info));
		}
		return handle;
	}();
	if (!job || !pid) {
		return;
	}
	if (const auto process = OpenProcess(
			PROCESS_SET_QUOTA | PROCESS_TERMINATE,
			FALSE,
			DWORD(pid))) {
		AssignProcessToJobObject(job, process);
		CloseHandle(process);
	}
}
#endif // Q_OS_WIN

int FreeLoopbackPort() {
	auto server = QTcpServer();
	if (!server.listen(QHostAddress::LocalHost, 0)) {
		return 0;
	}
	const auto port = int(server.serverPort());
	server.close();
	return port;
}

QString NewToken() {
	auto bytes = QByteArray(24, Qt::Uninitialized);
	for (auto i = 0; i != 6; ++i) {
		const auto random = base::RandomValue<uint32>();
		memcpy(bytes.data() + i * 4, &random, 4);
	}
	return QString::fromLatin1(bytes.toHex());
}

void Launch();

void Retry() {
	if (!Wanted || Restarts >= kMaxRestarts) {
		return;
	}
	++Restarts;
	QTimer::singleShot(2000 * Restarts, [] {
		if (Wanted && !Child) {
			Launch();
		}
	});
}

void Launch() {
	if (Child) {
		return;
	}
	const auto port = FreeLoopbackPort();
	if (!port) {
		NoteFellBack(u"no free loopback port"_q);
		return;
	}
	const auto token = NewToken();
	const auto process = new QProcess();
	Child = process;
	auto environment = QProcessEnvironment::systemEnvironment();
	environment.insert(u"NOVA_TLS_TERMINATOR_TOKEN"_q, token);
	process->setProcessEnvironment(environment);
	process->setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_WIN
	process->setCreateProcessArgumentsModifier(
		[](QProcess::CreateProcessArguments *args) {
			args->flags |= CREATE_NO_WINDOW;
		});
#endif // Q_OS_WIN

	const auto stderrText = std::make_shared<QByteArray>();
	const auto ready = std::make_shared<bool>(false);
	const auto endpoint = Endpoint{
		.port = port,
		.token = token,
		.roots = QFileInfo(RootsPath()).absoluteFilePath(),
	};
	QObject::connect(process, &QProcess::readyReadStandardError, [=] {
		stderrText->append(process->readAllStandardError());
		if (!*ready && stderrText->contains("listening on")) {
			*ready = true;
			if (Watchdog) {
				Watchdog->stop();
			}
			Publish(endpoint);
			LOG(("WSS: Yandex masking is on, terminator pid %1, "
				"port %2.").arg(process->processId()).arg(port));
		}
	});
	QObject::connect(process, &QProcess::started, [=] {
#ifdef Q_OS_WIN
		BindToJob(process->processId());
#endif // Q_OS_WIN
	});
	QObject::connect(
		process,
		QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
		[=](int code, QProcess::ExitStatus) {
			Publish(std::nullopt);
			if (Watchdog) {
				Watchdog->stop();
			}
			LOG(("WSS: terminator exited with %1 (%2).").arg(code).arg(
				QString::fromUtf8(stderrText->right(200)).trimmed()));
			process->deleteLater();
			Child = nullptr;
			if (!*ready) {
				NoteFellBack(u"terminator did not start"_q);
			}
			Retry();
		});
	QObject::connect(
		process,
		&QProcess::errorOccurred,
		[=](QProcess::ProcessError error) {
			if (error == QProcess::FailedToStart) {
				Publish(std::nullopt);
				NoteFellBack(u"terminator failed to start"_q);
				process->deleteLater();
				Child = nullptr;
			}
		});
	if (!Watchdog) {
		Watchdog = new QTimer();
		Watchdog->setSingleShot(true);
	}
	Watchdog->disconnect();
	QObject::connect(Watchdog.data(), &QTimer::timeout, [=] {
		if (!*ready && Child == process) {
			NoteFellBack(u"terminator was silent"_q);
			process->kill();
		}
	});
	Watchdog->start(kStartTimeout);
	process->start(ExePath(), {
		u"--port"_q,
		QString::number(port),
	});
}

} // namespace

bool Available() {
	return QFileInfo::exists(ExePath()) && QFileInfo::exists(RootsPath());
}

bool Enabled() {
	return Available()
		&& Core::App().settings().readPref<bool>(kEnabledKey, true);
}

void SetEnabled(bool enabled) {
	Core::App().settings().writePref<bool>(kEnabledKey, enabled);
	Core::App().saveSettingsDelayed();
	Refresh();
}

void Start() {
	static const auto connected = QObject::connect(
		qApp,
		&QCoreApplication::aboutToQuit,
		[] { Stop(); });
	[[maybe_unused]] const auto unused = connected;
	Refresh();
}

void Refresh() {
	Wanted = !Decoy::Active() && Wss::Enabled() && Enabled();
	if (Wanted) {
		Restarts = 0;
		FellBackLogged = false;
		Launch();
	} else {
		Stop();
	}
}

void Stop() {
	Wanted = false;
	Publish(std::nullopt);
	if (const auto process = Child.data()) {
		process->disconnect();
		process->kill();
		process->waitForFinished(500);
		process->deleteLater();
		Child = nullptr;
	}
}

std::optional<Endpoint> Current() {
	auto &shared = Shared();
	const auto lock = std::lock_guard(shared.mutex);
	return shared.endpoint;
}

void NoteFellBack(const QString &why) {
	if (!FellBackLogged.exchange(true)) {
		LOG(("WSS: Yandex masking unavailable (%1), "
			"falling back to the Qt TLS socket.").arg(why));
	}
}

QString Title() {
	return UseRussianTexts()
		? u"Маскировка под Яндекс Браузер"_q
		: u"Look like Yandex Browser"_q;
}

QString About() {
	return UseRussianTexts()
		? u"Вместо TLS средствами Qt рукопожатие делает отдельная "
			"программа nova-tls-terminator.exe (BoringSSL, тот же стек, что у "
			"Chromium): начало соединения выглядит так, как у Яндекс Браузера "
			"— набор и порядок расширений, GREASE, постквантовый обмен "
			"ключами. Это меняет то, как соединение выглядит для наблюдателя "
			"в сети. Ваш адрес оно не скрывает: реле и ретрансляторы видят его "
			"по-прежнему. Образец снят с Яндекс Браузера 26.10 при подключении "
			"по WebSocket; совпадение проверено по байтам начала соединения, "
			"но не гарантирует, что сеть примет его за браузер. "
			"Сертификат сервера проверяется по списку корневых центров "
			"Mozilla из файла nova_roots.pem рядом с программой. Если "
			"программа не запустилась, соединение идёт обычным способом, "
			"об этом одна строка в журнале. Переключатель доступен, когда оба "
			"файла лежат рядом с NovaGram.exe; действует на новые "
			"соединения."_q
		: u"Instead of Qt's TLS, a separate program, nova-tls-terminator.exe "
			"(BoringSSL, the stack Chromium uses), performs the handshake, so "
			"the start of the connection looks like Yandex Browser's: the set "
			"and order of extensions, GREASE, the post-quantum key exchange. "
			"This changes how the connection looks to someone watching the "
			"network. It does not hide your address: the relays still see it. "
			"The sample was taken from Yandex Browser 26.10 on a WebSocket "
			"connection; the match was checked byte by byte against the "
			"capture, but it does not mean a network will take it for the "
			"browser. The "
			"server certificate is checked against Mozilla's root list in "
			"nova_roots.pem next to the program. If the program does not "
			"start, the connection goes the usual way, with one line in the "
			"log. The switch is available when both files lie next to "
			"NovaGram.exe; it applies to new connections."_q;
}

} // namespace NovaGram::Terminator
