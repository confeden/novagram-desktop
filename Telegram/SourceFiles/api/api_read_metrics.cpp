/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_read_metrics.h"

#include "apiwrap.h"
#include "data/data_peer.h"

namespace Api {
namespace {

constexpr auto kSendTimeout = crl::time(5000);

} // namespace

ReadMetrics::ReadMetrics(not_null<ApiWrap*> api)
: _api(&api->instance())
, _timer([=] { send(); }) {
}

void ReadMetrics::add(
		not_null<PeerData*> /*peer*/,
		FinalizedReadMetric /*metric*/) {
	// NovaGram: messages.reportReadMetrics is never sent. It reports, per
	// message, how long it stood in the viewport, how much of that time the
	// window was active, how much of the message was on screen and how tall it
	// was against the viewport - attention measurement per message, sent to the
	// peer's server. It is not an ad mechanism and no setting governs it
	// upstream. Two promises of this fork rest on it not going out: the read
	// status withheld in a dialog says nothing about what was read, yet this
	// would report the dwell time of every message in it; and the fork sends no
	// interface telemetry at all.
	//
	// The refusal sits here, at the one funnel every tracker feeds, rather than
	// only where the trackers are built: a later base may report from a path
	// that does not exist today, and a gate placed at today's call sites would
	// not be on it (I15). The trackers are not built either, which is what
	// actually saves the work - see MaybeReadMetricsTracker and
	// HistoryInner's constructor.
	return;
}

void ReadMetrics::send() {
	for (auto i = _pending.begin(); i != _pending.end();) {
		if (_requests.contains(i->first)) {
			++i;
			continue;
		}

		auto metrics = QVector<MTPInputMessageReadMetric>();
		metrics.reserve(i->second.size());
		for (const auto &m : i->second) {
			metrics.push_back(MTP_inputMessageReadMetric(
				MTP_int(m.msgId.bare),
				MTP_long(m.viewId),
				MTP_int(m.timeInViewMs),
				MTP_int(m.activeTimeInViewMs),
				MTP_int(m.heightToViewportRatioPermille),
				MTP_int(m.seenRangeRatioPermille)));
		}
		const auto peer = i->first;
		const auto finish = [=] {
			_requests.erase(peer);
			if (!_pending.empty() && !_timer.isActive()) {
				_timer.callOnce(kSendTimeout);
			}
		};
		const auto requestId = _api.request(MTPmessages_ReportReadMetrics(
			peer->input(),
			MTP_vector<MTPInputMessageReadMetric>(std::move(metrics))
		)).done([=](const MTPBool &) {
			finish();
		}).fail([=](const MTP::Error &) {
			finish();
		}).send();

		_requests.emplace(peer, requestId);
		i = _pending.erase(i);
	}
}

} // namespace Api
