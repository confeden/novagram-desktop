/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/details/mtproto_ws_socket.h"

#include "novagram/nova_doh.h"
#include "base/invoke_queued.h"
#include "base/random.h"
#include "crl/crl_on_main.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QSslCipher>

namespace MTP::details {
namespace {

// One route gets the time TCP, TLS and the upgrade need together; two of them
// have to fit into the 8 seconds a connection attempt is given upstairs.
constexpr auto kRouteTimeout = crl::time(3500);
constexpr auto kRoutesPerAttempt = 2;
constexpr auto kMaxFrame = quint64(32) * 1024 * 1024;
constexpr auto kMaxHandshake = 16 * 1024;
constexpr auto kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
// What Yandex Browser for Windows sends on a WebSocket upgrade, in its order,
// from a real YaBrowser 26.10.0.1796 (Chromium 152) captured on 2026-10-18.
// Chromium adds no Sec-CH-UA or Sec-Fetch-* headers to an upgrade. The TLS
// profile of the terminator (Nova PC, captured from 26.6) is a little older
// than this; the differences are listed in kb/censorship-transport.md.
constexpr auto kUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
	"AppleWebKit/537.36 (KHTML, like Gecko) Chrome/152.0.0.0 "
	"YaBrowser/26.10.0.0 Safari/537.36";

// Cipher suites of the Yandex ClientHello for TLS 1.2, in its order. Qt hands
// the list to OpenSSL as it is; what it cannot do is shuffle the extensions,
// send GREASE or ALPS, so this is a part of the fingerprint and not all of it.
const char *const kYandexCiphers[] = {
	"ECDHE-ECDSA-AES128-GCM-SHA256",
	"ECDHE-RSA-AES128-GCM-SHA256",
	"ECDHE-ECDSA-AES256-GCM-SHA384",
	"ECDHE-RSA-AES256-GCM-SHA384",
	"ECDHE-ECDSA-CHACHA20-POLY1305",
	"ECDHE-RSA-CHACHA20-POLY1305",
	"ECDHE-RSA-AES128-SHA",
	"ECDHE-RSA-AES256-SHA",
	"AES128-GCM-SHA256",
	"AES256-GCM-SHA384",
	"AES128-SHA",
	"AES256-SHA",
};

void Xor(char *data, qint64 size, const uchar key[4]) {
	auto i = qint64(0);
	uint32 word = 0;
	memcpy(&word, key, 4);
	for (; i + 4 <= size; i += 4) {
		uint32 chunk = 0;
		memcpy(&chunk, data + i, 4);
		chunk ^= word;
		memcpy(data + i, &chunk, 4);
	}
	for (; i != size; ++i) {
		data[i] ^= key[i & 3];
	}
}

} // namespace

WsSocket::WsSocket(
	not_null<QThread*> thread,
	int16 protocolDcId,
	bool protocolForFiles)
: AbstractSocket(thread)
, _protocolDcId(protocolDcId)
, _protocolForFiles(protocolForFiles)
, _shared(std::make_shared<Shared>()) {
	_shared->owner = this;
	_timer.moveToThread(thread);
	_timer.setSingleShot(true);
	QObject::connect(&_timer, &QTimer::timeout, this, [=] {
		if (_state == State::Resolving
			|| _state == State::Connecting
			|| _state == State::Handshake) {
			routeFailed(u"timeout"_q);
		}
	});
}

WsSocket::~WsSocket() {
	{
		const auto lock = std::lock_guard(_shared->mutex);
		_shared->owner = nullptr;
	}
	_timer.stop();
	dropSocket();
}

void WsSocket::dropSocket() {
	if (const auto socket = _socket.release()) {
		socket->disconnect();
		socket->abort();
		socket->deleteLater();
	}
}

void WsSocket::connectToHost(const QString &address, int port) {
	// The address Telegram gave is not used: the route decides where to go,
	// and the data centre is the one named in the obfuscation header.
	_routes = NovaGram::Wss::Attempt(_protocolDcId, kRoutesPerAttempt);
	_routeIndex = 0;
	if (_routes.empty()) {
		_state = State::Closed;
		InvokeQueued(this, [=] { _error.fire({}); });
		return;
	}
	tryNextRoute();
}

void WsSocket::tryNextRoute() {
	if (_routeIndex >= int(_routes.size())) {
		_state = State::Closed;
		_timer.stop();
		_error.fire({});
		return;
	}
	dropSocket();
	_in.clear();
	_inPos = 0;
	_tunnel = Tunnel::None;
	_rx.clear();
	_rxPos = 0;
	_route = _routes[_routeIndex++];
	++_generation;
	_timer.start(kRouteTimeout);
	if (_route.ip.isEmpty()) {
		_state = State::Resolving;
		resolve();
	} else {
		startRoute(_route.ip);
	}
}

void WsSocket::resolve() {
	const auto shared = _shared;
	const auto generation = _generation;
	const auto host = _route.host;
	// The resolver belongs to the main thread, and nothing here may reach the
	// system resolver: see NovaGram::Doh::Resolve.
	crl::on_main([=] {
		NovaGram::Doh::Resolve(
			host,
			NovaGram::Doh::RecordType::A,
			[=](NovaGram::Doh::Answer answer) {
				const auto lock = std::lock_guard(shared->mutex);
				if (const auto owner = shared->owner) {
					owner->resolved(
						generation,
						answer.values.empty()
							? QString()
							: answer.values.front());
				}
			});
	});
}

void WsSocket::resolved(int generation, const QString &ip) {
	InvokeQueued(this, [=] {
		if (generation != _generation || _state != State::Resolving) {
			return;
		} else if (ip.isEmpty()) {
			routeFailed(u"no address"_q);
		} else {
			startRoute(ip);
		}
	});
}

void WsSocket::startRoute(const QString &ip) {
	_state = State::Connecting;
	_tunnel = Tunnel::None;
	if (const auto endpoint = NovaGram::Terminator::Current()) {
		startTerminatorRoute(ip, *endpoint);
	} else {
		if (NovaGram::Terminator::Enabled()
			&& NovaGram::Wss::EnabledForSocket()) {
			NovaGram::Terminator::NoteFellBack(u"not running"_q);
		}
		startQtRoute(ip);
	}
}

void WsSocket::connectCommon(QTcpSocket *socket, int generation) {
	QObject::connect(socket, &QAbstractSocket::connected, this, [=] {
		if (generation != _generation) {
			return;
		}
		// On `connected`, not in the constructor: Qt has no descriptor yet at
		// construction and quietly drops the option.
		socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
		if (_protocolForFiles) {
			socket->setSocketOption(
				QAbstractSocket::SendBufferSizeSocketOption,
				kFilesSendBufferSize);
			socket->setSocketOption(
				QAbstractSocket::ReceiveBufferSizeSocketOption,
				kFilesReceiveBufferSize);
		}
	});
	QObject::connect(socket, &QAbstractSocket::readyRead, this, [=] {
		if (generation == _generation) {
			handleRead();
		}
	});
	QObject::connect(socket, &QAbstractSocket::disconnected, this, [=] {
		if (generation != _generation) {
			return;
		} else if (_state == State::Open) {
			closedByPeer(false);
		} else if (_state == State::Connecting
			|| _state == State::Handshake) {
			routeFailed(u"closed"_q);
		}
	});
	QObject::connect(
		socket,
		&QAbstractSocket::errorOccurred,
		this,
		[=](QAbstractSocket::SocketError error) {
			if (generation != _generation) {
				return;
			}
			logError(error, socket->errorString());
			if (_state == State::Open) {
				closedByPeer(true);
			} else if (_state == State::Connecting
				|| _state == State::Handshake) {
				routeFailed(socket->errorString());
			}
		});
}

void WsSocket::startTerminatorRoute(
		const QString &ip,
		const NovaGram::Terminator::Endpoint &endpoint) {
	_endpoint = endpoint;
	_tunnelIp = ip;
	_socket = std::make_unique<QTcpSocket>();
	const auto socket = _socket.get();
	socket->moveToThread(QObject::thread());
	const auto generation = _generation;
	connectCommon(socket, generation);
	QObject::connect(socket, &QAbstractSocket::connected, this, [=] {
		if (generation == _generation) {
			_tunnel = Tunnel::Greeting;
		}
	});
	socket->connectToHost(
		QHostAddress(QHostAddress::LocalHost),
		quint16(endpoint.port));
}

// Reads the greeting and the answer of the terminator out of the buffer. False
// when the route is already failed; true also while a line is still missing.
// On success the state is Handshake and the plain bytes may follow.
bool WsSocket::handleTunnel() {
	while (_tunnel != Tunnel::None) {
		const auto end = _in.indexOf('\n', _inPos);
		if (end < 0) {
			return (_in.size() - _inPos) < kMaxHandshake;
		}
		const auto line = _in.mid(_inPos, end - _inPos).trimmed();
		_inPos = end + 1;
		if (_tunnel == Tunnel::Greeting) {
			if (!line.contains("tls-terminator/")) {
				return false;
			}
			auto target = QJsonObject();
			target.insert(u"host"_q, _tunnelIp);
			target.insert(u"port"_q, 443);
			auto egress = QJsonObject();
			egress.insert(u"kind"_q, u"direct"_q);
			auto request = QJsonObject();
			request.insert(u"token"_q, _endpoint.token);
			request.insert(u"target"_q, target);
			request.insert(u"egress"_q, egress);
			// The name sent as SNI and checked against the certificate.
			request.insert(u"sni"_q, _route.host);
			// Yandex Browser 26.10 as it greets a WebSocket server.
			request.insert(u"profile"_q, u"yandex-windows-26.10"_q);
			request.insert(u"verify"_q, true);
			request.insert(u"verify_roots"_q, _endpoint.roots);
			request.insert(u"connect_timeout_ms"_q, 2500);
			request.insert(u"handshake_timeout_ms"_q, 2500);
			_socket->write(
				QJsonDocument(request).toJson(QJsonDocument::Compact)
				+ '\n');
			_tunnel = Tunnel::Reply;
		} else {
			const auto reply = QJsonDocument::fromJson(line).object();
			if (!reply.value(u"ok"_q).toBool()) {
				LOG(("WSS: terminator refused %1: %2.").arg(
					_route.host,
					reply.value(u"error"_q).toString()));
				return false;
			}
			_tunnel = Tunnel::None;
			sendUpgrade();
		}
	}
	return true;
}

void WsSocket::startQtRoute(const QString &ip) {
	auto holder = std::make_unique<QSslSocket>();
	const auto socket = holder.get();
	_socket = std::move(holder);
	socket->moveToThread(QObject::thread());
	const auto generation = _generation;

	auto configuration = QSslConfiguration::defaultConfiguration();
	configuration.setAllowedNextProtocols({ "http/1.1" });
	{
		const auto supported = QSslConfiguration::supportedCiphers();
		auto ciphers = QList<QSslCipher>();
		for (const auto name : kYandexCiphers) {
			for (const auto &cipher : supported) {
				if (cipher.name() == QLatin1String(name)) {
					ciphers.push_back(cipher);
					break;
				}
			}
		}
		// Only when the list came out whole enough to connect with; the
		// default list is better than a broken one.
		if (ciphers.size() >= 6) {
			configuration.setCiphers(ciphers);
		}
	}
	socket->setSslConfiguration(configuration);

	connectCommon(socket, generation);
	QObject::connect(socket, &QSslSocket::encrypted, this, [=] {
		if (generation == _generation && _state == State::Connecting) {
			sendUpgrade();
		}
	});
	QObject::connect(
		socket,
		QOverload<const QList<QSslError>&>::of(&QSslSocket::sslErrors),
		this,
		[=](const QList<QSslError> &errors) {
			if (generation == _generation
				&& (_state == State::Connecting
					|| _state == State::Handshake)) {
				routeFailed(errors.isEmpty()
					? u"tls"_q
					: errors.front().errorString());
			}
		});
	socket->connectToHostEncrypted(ip, 443, _route.host);
}

void WsSocket::routeFailed(const QString &why) {
	_timer.stop();
	LOG(("WSS: route %1 failed: %2.").arg(_route.host, why));
	NovaGram::Wss::NoteFailed(_protocolDcId, _route);
	_state = State::Idle;
	tryNextRoute();
}

void WsSocket::sendUpgrade() {
	_state = State::Handshake;
	auto key = QByteArray(16, Qt::Uninitialized);
	for (auto i = 0; i != 4; ++i) {
		const auto random = base::RandomValue<uint32>();
		memcpy(key.data() + i * 4, &random, 4);
	}
	_key = key.toBase64();
	// Header names and their order as the browser writes them. The offer of
	// permessage-deflate is the browser's too; the relays ignore it, and a
	// server that accepted it would be refused, since no inflater is here.
	const auto request = QByteArray("GET ") + _route.path.toLatin1()
		+ " HTTP/1.1\r\nHost: " + _route.host.toLatin1()
		+ "\r\nConnection: Upgrade\r\nPragma: no-cache\r\n"
			"Cache-Control: no-cache\r\n"
			"User-Agent: " + kUserAgent
		+ "\r\nUpgrade: websocket\r\nOrigin: https://web.telegram.org\r\n"
			"Sec-WebSocket-Version: 13\r\n"
			"Accept-Encoding: gzip, deflate, br, zstd\r\n"
			"Accept-Language: ru,en;q=0.9\r\n"
			"Sec-WebSocket-Key: " + _key
		+ "\r\nSec-WebSocket-Extensions: permessage-deflate; "
			"client_max_window_bits\r\n"
			"Sec-WebSocket-Protocol: binary\r\n\r\n";
	_socket->write(request);
}

// False when the answer is not an upgrade. True also while the answer is not
// complete yet; the state tells the two apart.
bool WsSocket::handleHandshake() {
	const auto end = _in.indexOf("\r\n\r\n", _inPos);
	if (end < 0) {
		return (_in.size() - _inPos) < kMaxHandshake;
	}
	const auto head = _in.mid(_inPos, end - _inPos);
	_inPos = end + 4;
	const auto lines = head.split('\n');
	const auto upgraded = !lines.isEmpty()
		&& lines.front().startsWith("HTTP/1.1 101");
	const auto expected = QCryptographicHash::hash(
		_key + kGuid,
		QCryptographicHash::Sha1).toBase64();
	auto accepted = false;
	for (const auto &line : lines) {
		const auto trimmed = line.trimmed();
		if (trimmed.toLower().startsWith("sec-websocket-accept:")) {
			accepted = (trimmed.mid(trimmed.indexOf(':') + 1).trimmed()
				== expected);
		}
	}
	auto extended = false;
	for (const auto &line : lines) {
		if (line.trimmed().toLower().startsWith("sec-websocket-extensions:")) {
			extended = true;
		}
	}
	if (!upgraded || !accepted || extended) {
		LOG(("WSS: route %1 refused the upgrade: %2.").arg(
			_route.host,
			QString::fromLatin1(lines.isEmpty()
				? QByteArray()
				: lines.front().trimmed().left(80))));
		return false;
	}
	_timer.stop();
	_state = State::Open;
	NovaGram::Wss::NoteWorked(_protocolDcId, _route);
	DEBUG_LOG(("WSS: dc %1 open over %2."_q).arg(
		QString::number(_protocolDcId),
		_route.host));
	return true;
}

void WsSocket::handleRead() {
	if (!_socket) {
		return;
	}
	_in.append(_socket->readAll());
	if (_state == State::Connecting && _tunnel != Tunnel::None) {
		if (!handleTunnel()) {
			routeFailed(u"terminator"_q);
			return;
		} else if (_state != State::Handshake) {
			return;
		}
	}
	if (_state == State::Handshake) {
		if (!handleHandshake()) {
			routeFailed(u"handshake"_q);
			return;
		} else if (_state != State::Open) {
			return;
		}
		// Fired after the state moved: the first thing the listener does is
		// write, and a write in any other state is dropped.
		const auto shared = _shared;
		_connected.fire({});
		if (!shared->owner) {
			return;
		}
	}
	if (_state == State::Open) {
		parseFrames();
	}
}

void WsSocket::parseFrames() {
	auto gotData = false;
	auto closed = false;
	while (!closed) {
		const auto available = qint64(_in.size() - _inPos);
		if (available < 2) {
			break;
		}
		const auto p = reinterpret_cast<const uchar*>(_in.constData())
			+ _inPos;
		const auto opcode = int(p[0] & 0x0F);
		const auto masked = ((p[1] & 0x80) != 0);
		auto length = quint64(p[1] & 0x7F);
		auto header = qint64(2);
		if (length == 126) {
			if (available < 4) {
				break;
			}
			length = (quint64(p[2]) << 8) | p[3];
			header = 4;
		} else if (length == 127) {
			if (available < 10) {
				break;
			}
			length = 0;
			for (auto i = 2; i != 10; ++i) {
				length = (length << 8) | p[i];
			}
			header = 10;
		}
		if (length > kMaxFrame) {
			closed = true;
			break;
		}
		if (masked) {
			header += 4;
		}
		if (quint64(available) < quint64(header) + length) {
			break;
		}
		auto payload = QByteArray(
			reinterpret_cast<const char*>(p) + header,
			int(length));
		if (masked) {
			Xor(payload.data(), payload.size(), p + header - 4);
		}
		_inPos += int(header + length);
		switch (opcode) {
		case 0x0: case 0x1: case 0x2:
			// Fragmentation is of no interest: the stream is bytes.
			_rx.append(payload);
			gotData = gotData || !payload.isEmpty();
			break;
		case 0x8:
			closed = true;
			break;
		case 0x9:
			sendFrame(0xA, bytes::make_span(payload), {});
			break;
		default:
			break;
		}
	}
	if (_inPos == _in.size()) {
		_in.clear();
		_inPos = 0;
	} else if (_inPos > 64 * 1024) {
		_in.remove(0, _inPos);
		_inPos = 0;
	}
	// The listener may take this socket down from inside the signal.
	const auto shared = _shared;
	if (gotData) {
		_readyRead.fire({});
		if (!shared->owner) {
			return;
		}
	}
	if (closed) {
		closedByPeer(false);
	}
}

void WsSocket::closedByPeer(bool error) {
	if (_state != State::Open) {
		return;
	}
	_state = State::Closed;
	if (error) {
		_error.fire({});
	} else {
		_disconnected.fire({});
	}
}

bool WsSocket::isGoodStartNonce(bytes::const_span nonce) {
	Expects(nonce.size() >= 2 * sizeof(uint32));

	const auto bytes = nonce.data();
	const auto zero = *reinterpret_cast<const uchar*>(bytes);
	const auto first = *reinterpret_cast<const uint32*>(bytes);
	const auto second = *(reinterpret_cast<const uint32*>(bytes) + 1);
	return (zero != 0xEF)
		&& (first != 0x44414548U)
		&& (first != 0x54534F50U)
		&& (first != 0x20544547U)
		&& (first != 0xEEEEEEEEU)
		&& (first != 0xDDDDDDDDU)
		&& (first != 0x02010316U)
		&& (second != 0x00000000U);
}

void WsSocket::timedOut() {
}

bool WsSocket::isConnected() {
	return (_state == State::Open);
}

bool WsSocket::hasBytesAvailable() {
	return _rxPos < _rx.size();
}

int64 WsSocket::read(bytes::span buffer) {
	const auto count = std::min<qint64>(
		qint64(buffer.size()),
		qint64(_rx.size() - _rxPos));
	if (count <= 0) {
		return 0;
	}
	memcpy(buffer.data(), _rx.constData() + _rxPos, size_t(count));
	_rxPos += int(count);
	if (_rxPos == _rx.size()) {
		_rx.clear();
		_rxPos = 0;
	} else if (_rxPos > 256 * 1024) {
		_rx.remove(0, _rxPos);
		_rxPos = 0;
	}
	return count;
}

void WsSocket::write(bytes::const_span prefix, bytes::const_span buffer) {
	Expects(!buffer.empty());

	sendFrame(0x2, prefix, buffer);
}

void WsSocket::sendFrame(
		int opcode,
		bytes::const_span first,
		bytes::const_span second) {
	if (_state != State::Open || !_socket) {
		return;
	}
	const auto size = quint64(first.size() + second.size());
	auto frame = QByteArray();
	frame.reserve(int(size) + 14);
	frame.append(char(0x80 | opcode));
	if (size < 126) {
		frame.append(char(0x80 | size));
	} else if (size < 65536) {
		frame.append(char(0x80 | 126));
		frame.append(char(size >> 8));
		frame.append(char(size & 0xFF));
	} else {
		frame.append(char(0x80 | 127));
		for (auto shift = 56; shift >= 0; shift -= 8) {
			frame.append(char((size >> shift) & 0xFF));
		}
	}
	uchar key[4];
	const auto random = base::RandomValue<uint32>();
	memcpy(key, &random, 4);
	frame.append(reinterpret_cast<const char*>(key), 4);
	const auto start = frame.size();
	frame.append(
		reinterpret_cast<const char*>(first.data()),
		int(first.size()));
	frame.append(
		reinterpret_cast<const char*>(second.data()),
		int(second.size()));
	Xor(frame.data() + start, qint64(size), key);
	_socket->write(frame);
}

int32 WsSocket::debugState() {
	return _socket ? int32(_socket->state()) : -1;
}

QString WsSocket::debugPostfix() const {
	return u"_ws"_q;
}

} // namespace MTP::details
