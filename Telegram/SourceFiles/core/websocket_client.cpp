/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/websocket_client.h"

#include "base/random.h"
#include "novagram/nova_doh.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QPointer>
#include <QtNetwork/QHostAddress>

namespace Core {
namespace {

constexpr auto kOpcodeContinuation = quint8(0x0);
constexpr auto kOpcodeText = quint8(0x1);
constexpr auto kOpcodeBinary = quint8(0x2);
constexpr auto kOpcodeClose = quint8(0x8);
constexpr auto kOpcodePing = quint8(0x9);
constexpr auto kOpcodePong = quint8(0xA);

constexpr auto kMaxFramePayload = quint64(4 * 1024 * 1024);
constexpr auto kMaxUpgradeResponse = 8 * 1024;
constexpr auto kAcceptGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr auto kSecureDefaultPort = 443;

// NovaGram: the one place the wallet streams from. The URL comes from the
// server inside a TL answer, so the host it names is an input, not a fact: a
// stream that may open to any host lets whoever controls that answer pick where
// this client connects and what it hands over in the query string.
[[nodiscard]] bool AllowedStreamHost(const QString &host) {
	const auto name = host.toLower();
	return (name == u"toncenter.com"_q)
		|| name.endsWith(u".toncenter.com"_q);
}

} // namespace

WebSocketClient::WebSocketClient(QObject *parent) : QObject(parent) {
	connect(&_socket, &QSslSocket::encrypted, this, [=] {
		onSslConnected();
	});
	connect(&_socket, &QSslSocket::readyRead, this, [=] {
		onReadyRead();
	});
	connect(&_socket, &QSslSocket::disconnected, this, [=] {
		fail();
	});
	connect(
		&_socket,
		&QAbstractSocket::errorOccurred,
		this,
		[=](QAbstractSocket::SocketError) { fail(); });
}

WebSocketClient::~WebSocketClient() {
	_socket.abort();
}

void WebSocketClient::connectTo(
		const QString &host,
		int port,
		const QString &requestTarget,
		const QString &subprotocol) {
	if (_state != State::Idle) {
		return;
	}
	_host = host;
	_port = port;
	_target = requestTarget;
	_subprotocol = subprotocol;
	_state = State::Connecting;

	// NovaGram: this socket used to go straight out of connectToHostEncrypted -
	// the system resolver, whatever the application proxy happened to be, any
	// certificate authority the machine trusts. Here the name is allow-listed,
	// resolved over the pinned DoH endpoints only (a miss is a failure, never a
	// fall-back to getaddrinfo), reached through the proxy the fork decides on,
	// and verified against the pinned roots under the real name.
	if (!AllowedStreamHost(host) || port <= 0 || port > 65535) {
		fail();
		return;
	}
	const auto weak = QPointer<WebSocketClient>(this);
	const auto proxy = NovaGram::Doh::ProxyFor(
		QUrl(u"https://"_q + host + u"/"_q));
	const auto open = [=](QString address) {
		if (!weak || weak->_state != State::Connecting) {
			return;
		}
		auto config = NovaGram::Doh::PinnedConfiguration();
		config.setPeerVerifyMode(QSslSocket::VerifyPeer);
		weak->_socket.setProxy(proxy);
		weak->_socket.setSslConfiguration(config);
		weak->_socket.setPeerVerifyName(host);
		weak->_socket.connectToHostEncrypted(
			address,
			quint16(port),
			host);
	};
	NovaGram::Doh::Resolve(host, NovaGram::Doh::RecordType::A, [=](
			NovaGram::Doh::Answer a) {
		for (const auto &value : a.values) {
			if (!QHostAddress(value).isNull()) {
				open(value);
				return;
			}
		}
		NovaGram::Doh::Resolve(host, NovaGram::Doh::RecordType::AAAA, [=](
				NovaGram::Doh::Answer b) {
			for (const auto &value : b.values) {
				if (!QHostAddress(value).isNull()) {
					open(value);
					return;
				}
			}
			if (weak) {
				weak->fail();
			}
		});
	});
}

void WebSocketClient::onSslConnected() {
	if (_state != State::Connecting) {
		return;
	}
	_state = State::WaitingUpgrade;
	sendUpgradeRequest();
}

void WebSocketClient::sendUpgradeRequest() {
	auto keyBytes = std::array<uint8_t, 16>();
	base::RandomFill(keyBytes.data(), keyBytes.size());
	_key = QByteArray(
		reinterpret_cast<const char*>(keyBytes.data()),
		int(keyBytes.size())).toBase64();

	const auto host = (_port == kSecureDefaultPort)
		? _host
		: (_host + u":"_q + QString::number(_port));
	auto request = u"GET "_q + _target + u" HTTP/1.1\r\n"_q
		+ u"Host: "_q + host + u"\r\n"_q
		+ u"Upgrade: websocket\r\n"_q
		+ u"Connection: Upgrade\r\n"_q
		+ u"Sec-WebSocket-Key: "_q + QString::fromLatin1(_key) + u"\r\n"_q
		+ u"Sec-WebSocket-Version: 13\r\n"_q;
	if (!_subprotocol.isEmpty()) {
		request += u"Sec-WebSocket-Protocol: "_q + _subprotocol + u"\r\n"_q;
	}
	request += u"\r\n"_q;
	_socket.write(request.toLatin1());
}

bool WebSocketClient::readUpgradeResponse() {
	const auto end = _incoming.indexOf("\r\n\r\n");
	if (end < 0) {
		return (_incoming.size() < kMaxUpgradeResponse);
	}
	const auto head = _incoming.left(end);
	const auto lines = head.split('\n');
	const auto status = lines.isEmpty()
		? QByteArray()
		: lines.front().trimmed();
	if (!status.contains(" 101")) {
		return false;
	}
	const auto accept = _key + kAcceptGuid;
	const auto expected = QCryptographicHash::hash(
		accept,
		QCryptographicHash::Sha1).toBase64();
	auto accepted = false;
	for (const auto &line : lines) {
		const auto colon = line.indexOf(':');
		if (colon < 0) {
			continue;
		} else if (line.left(colon).trimmed().toLower()
			!= "sec-websocket-accept") {
			continue;
		}
		accepted = (line.mid(colon + 1).trimmed() == expected);
		break;
	}
	if (!accepted) {
		return false;
	}
	_incoming.remove(0, end + 4);
	_state = State::Connected;
	if (const auto handler = onConnected) {
		handler();
	}
	return true;
}

void WebSocketClient::onReadyRead() {
	if (_state == State::WaitingUpgrade) {
		_incoming.append(_socket.readAll());
		if (!readUpgradeResponse()) {
			fail();
			return;
		} else if (_state != State::Connected) {
			return;
		}
	} else if (_state == State::Connected) {
		_incoming.append(_socket.readAll());
	} else {
		return;
	}
	if (!parseFrames()) {
		fail();
	}
}

bool WebSocketClient::parseFrames() {
	auto offset = 0;
	auto closed = false;
	const auto size = _incoming.size();
	const auto data = reinterpret_cast<const uchar*>(_incoming.constData());
	while (!closed) {
		if (size - offset < 2) {
			break;
		}
		const auto first = data[offset];
		const auto second = data[offset + 1];
		const auto opcode = quint8(first & 0x0F);
		const auto masked = ((second & 0x80) != 0);
		auto length = quint64(second & 0x7F);
		auto header = 2;
		if (length == 126) {
			if (size - offset < 4) {
				break;
			}
			length = (quint64(data[offset + 2]) << 8)
				| quint64(data[offset + 3]);
			header = 4;
		} else if (length == 127) {
			if (size - offset < 10) {
				break;
			}
			length = 0;
			for (auto i = 0; i != 8; ++i) {
				length = (length << 8) | quint64(data[offset + 2 + i]);
			}
			header = 10;
		}
		if (length > kMaxFramePayload) {
			return false;
		}
		if (masked) {
			header += 4;
		}
		if (quint64(size - offset) < quint64(header) + length) {
			break;
		}
		auto payload = QByteArray(
			_incoming.constData() + offset + header,
			int(length));
		if (masked) {
			const auto mask = data + offset + header - 4;
			for (auto i = 0; i != int(length); ++i) {
				payload[i] = char(payload[i] ^ char(mask[i % 4]));
			}
		}
		offset += header + int(length);
		if (const auto handler = onActivity) {
			handler();
		}

		switch (opcode) {
		case kOpcodeContinuation:
		case kOpcodeBinary:
			if (!payload.isEmpty()) {
				if (const auto handler = onBinary) {
					handler(payload);
					closed = (_state != State::Connected);
				}
			}
			break;
		case kOpcodeText:
			if (!payload.isEmpty()) {
				if (const auto handler = onText) {
					handler(payload);
					closed = (_state != State::Connected);
				}
			}
			break;
		case kOpcodeClose:
			return false;
		case kOpcodePing:
			writeFrame(payload, kOpcodePong);
			break;
		case kOpcodePong:
			break;
		default:
			return false;
		}
	}
	if (offset > 0) {
		_incoming.remove(0, offset);
	}
	return true;
}

void WebSocketClient::writeFrame(const QByteArray &payload, quint8 opcode) {
	const auto length = quint64(payload.size());
	auto frame = QByteArray();
	frame.reserve(int(length) + 14);
	frame.append(char(0x80 | opcode));
	if (length < 126) {
		frame.append(char(0x80 | char(length)));
	} else if (length <= 0xFFFF) {
		frame.append(char(0x80 | 126));
		frame.append(char((length >> 8) & 0xFF));
		frame.append(char(length & 0xFF));
	} else {
		frame.append(char(0x80 | 127));
		for (auto i = 7; i >= 0; --i) {
			frame.append(char((length >> (i * 8)) & 0xFF));
		}
	}
	auto maskBytes = std::array<uint8_t, 4>();
	base::RandomFill(maskBytes.data(), maskBytes.size());
	const auto mask = reinterpret_cast<const char*>(maskBytes.data());
	frame.append(mask, 4);

	const auto from = payload.constData();
	for (auto i = 0; i != int(length); ++i) {
		frame.append(char(from[i] ^ mask[i % 4]));
	}
	_socket.write(frame);
}

void WebSocketClient::sendText(const QByteArray &message) {
	if (_state != State::Connected) {
		return;
	}
	writeFrame(message, kOpcodeText);
}

void WebSocketClient::sendBinary(const QByteArray &message) {
	if (_state != State::Connected) {
		return;
	}
	writeFrame(message, kOpcodeBinary);
}

void WebSocketClient::close() {
	if (_state == State::Connected) {
		writeFrame(QByteArray(), kOpcodeClose);
	}
	_state = State::Closed;
	_socket.abort();
}

void WebSocketClient::fail() {
	if (_failed || _state == State::Closed) {
		return;
	}
	_failed = true;
	_state = State::Closed;
	if (const auto handler = onClosed) {
		handler();
	}
}

} // namespace Core
