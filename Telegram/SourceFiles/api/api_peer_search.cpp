/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "api/api_peer_search.h"

#include "api/api_single_message_search.h"
#include "apiwrap.h"
#include "data/data_session.h"
#include "dialogs/ui/chat_search_in.h" // IsHashOrCashtagSearchQuery
#include "main/main_session.h"

namespace Api {

PeerSearch::PeerSearch(not_null<Main::Session*> session, Type type)
: _session(session)
, _type(type) {
}

PeerSearch::~PeerSearch() {
	clear();
}

void PeerSearch::request(
		const QString &query,
		Fn<void(PeerSearchResult)> callback,
		RequestType type) {
	using namespace Dialogs;
	_query = Api::ConvertPeerSearchQuery(query);
	_callback = callback;
	if (_query.isEmpty()
		|| IsHashOrCashtagSearchQuery(_query) != HashOrCashtag::None) {
		finish(PeerSearchResult{});
		return;
	}
	auto &cache = _cache[_query];
	if (cache.peersReady && cache.sponsoredReady) {
		finish(cache.result);
		return;
	} else if (type == RequestType::CacheOnly) {
		_callback = nullptr;
		return;
	} else if (cache.requested) {
		return;
	}
	cache.requested = true;
	cache.result.query = _query;

	// NovaGram: contacts.getSponsoredPeers is never sent. It carried the
	// text a person types into the chat list search - content, not an ad
	// impression - to the advertising endpoint for every query of four
	// characters or more. Sponsored messages inside chats, their display and
	// their view/click reporting are a separate mechanism and stay untouched.
	// The section is marked ready here so the search still completes.
	cache.sponsoredReady = true;

	requestPeers();
}

void PeerSearch::requestPeers() {
	const auto requestId = _session->api().request(MTPcontacts_Search(
		MTP_flags(0),
		MTP_string(_query),
		MTP_int(SearchPeopleLimit)
	)).done([=](const MTPcontacts_Found &result, mtpRequestId requestId) {
		const auto &data = result.data();
		_session->data().processUsers(data.vusers());
		_session->data().processChats(data.vchats());
		auto parsed = PeerSearchResult();
		parsed.my.reserve(data.vmy_results().v.size());
		for (const auto &id : data.vmy_results().v) {
			const auto peerId = peerFromMTP(id);
			parsed.my.push_back(_session->data().peer(peerId));
		}
		parsed.peers.reserve(data.vresults().v.size());
		for (const auto &id : data.vresults().v) {
			const auto peerId = peerFromMTP(id);
			parsed.peers.push_back(_session->data().peer(peerId));
		}
		finishPeers(requestId, std::move(parsed));
	}).fail([=](const MTP::Error &error, mtpRequestId requestId) {
		finishPeers(requestId, PeerSearchResult{});
	}).send();
	_peerRequests.emplace(requestId, _query);
}

void PeerSearch::finishPeers(
		mtpRequestId requestId,
		PeerSearchResult result) {
	const auto query = _peerRequests.take(requestId);
	Assert(query.has_value());

	auto &cache = _cache[*query];
	cache.peersReady = true;
	cache.result.my = std::move(result.my);
	cache.result.peers = std::move(result.peers);
	if (cache.sponsoredReady && _query == *query) {
		finish(cache.result);
	}
}

void PeerSearch::finish(PeerSearchResult result) {
	if (const auto onstack = base::take(_callback)) {
		onstack(std::move(result));
	}
}

void PeerSearch::clear() {
	_query = QString();
	_callback = nullptr;
	_cache.clear();
	for (const auto &[requestId, query] : base::take(_peerRequests)) {
		_session->api().request(requestId).cancel();
	}
}

} // namespace Api
