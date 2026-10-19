/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/details/mtproto_abstract_socket.h"
#include "novagram/nova_terminator.h"
#include "novagram/nova_wss.h"

#include <QtCore/QTimer>
#include <QtNetwork/QSslSocket>

#include <memory>
#include <mutex>

namespace MTP::details {

// The transport stream of an obfuscated MTProto connection carried in binary
// WebSocket frames over TLS (RFC 6455, written out here because nothing else
// in the client needs it). The first frame carries the whole 64-byte
// obfuscation header together with the first packet: a relay that gets a
// shorter first frame answers nothing at all.
//
// A connection attempt walks at most two routes of the ladder in
// novagram/nova_wss.h and reports an error only when both failed; the ladder
// remembers which one worked, so the next attempt starts there.
class WsSocket final : public AbstractSocket {
public:
	WsSocket(
		not_null<QThread*> thread,
		int16 protocolDcId,
		bool protocolForFiles);
	~WsSocket();

	void connectToHost(const QString &address, int port) override;
	bool isGoodStartNonce(bytes::const_span nonce) override;
	void timedOut() override;
	bool isConnected() override;
	bool hasBytesAvailable() override;
	int64 read(bytes::span buffer) override;
	void write(bytes::const_span prefix, bytes::const_span buffer) override;

	int32 debugState() override;
	QString debugPostfix() const override;

private:
	// With the terminator the TLS leg is made by another process: a loopback
	// connection first says hello, takes one JSON request and answers one line,
	// and only then carries the plain bytes of the upgrade.
	enum class Tunnel {
		None,
		Greeting,
		Reply,
	};
	enum class State {
		Idle,
		Resolving,
		Connecting,
		Handshake,
		Open,
		Closed,
	};
	struct Shared {
		std::mutex mutex;
		WsSocket *owner = nullptr;
	};

	void tryNextRoute();
	void resolve();
	void resolved(int generation, const QString &ip);
	void startRoute(const QString &ip);
	void startTerminatorRoute(
		const QString &ip,
		const NovaGram::Terminator::Endpoint &endpoint);
	void startQtRoute(const QString &ip);
	void connectCommon(QTcpSocket *socket, int generation);
	bool handleTunnel();
	void routeFailed(const QString &why);
	void dropSocket();
	void sendUpgrade();
	void handleRead();
	bool handleHandshake();
	void parseFrames();
	void sendFrame(int opcode, bytes::const_span first, bytes::const_span second);
	void closedByPeer(bool error);

	const int16 _protocolDcId = 0;
	const bool _protocolForFiles = false;
	const std::shared_ptr<Shared> _shared;

	std::unique_ptr<QTcpSocket> _socket;
	Tunnel _tunnel = Tunnel::None;
	NovaGram::Terminator::Endpoint _endpoint;
	QString _tunnelIp;
	QTimer _timer;
	State _state = State::Idle;
	int _generation = 0;

	std::vector<NovaGram::Wss::Route> _routes;
	int _routeIndex = 0;
	NovaGram::Wss::Route _route;
	QByteArray _key;

	QByteArray _in;
	int _inPos = 0;
	QByteArray _rx;
	int _rxPos = 0;

};

} // namespace MTP::details
