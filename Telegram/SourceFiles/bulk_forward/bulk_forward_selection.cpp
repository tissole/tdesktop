/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk_forward/bulk_forward_selection.h"

#include "apiwrap.h"
#include "base/algorithm.h"
#include "base/debug_log.h"
#include "base/flat_map.h"
#include "bulk_forward/bulk_forward_classifier.h"
#include "data/data_channel.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "enhanced_forward.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"

#include <crl/crl.h>

#include <algorithm>

namespace BulkForward {
namespace {

constexpr auto kBatchSize = 100;

} // namespace

SelectionScanner::SelectionScanner(not_null<Main::Session*> session)
: _session(session) {
}

SelectionScanner::~SelectionScanner() {
	cancel();
}

void SelectionScanner::start(
		MessageIdsList ids,
		Filter filter,
		Fn<void(std::vector<SourceMessage>)> batch,
		Fn<void()> finished,
		Fn<void(QString)> fail) {
	cancel();

	_cancelled = false;
	_paused = false;
	_batchIndex = 0;
	_takeoutRequested = false;
	_takeoutFallbackTried = false;
	_matched.clear();
	_batches.clear();
	_filter = std::move(filter);
	_done = std::move(batch);
	_finished = std::move(finished);
	_fail = std::move(fail);

	auto byPeer = base::flat_map<PeerId, MessageIdsList>();
	for (const auto &id : ids) {
		if (id.msg && id.peer) {
			byPeer[id.peer].push_back(id);
		}
	}
	for (auto &[peerId, list] : byPeer) {
		std::sort(list.begin(), list.end(), [](const FullMsgId &a, const FullMsgId &b) {
			return a.msg < b.msg;
		});
		for (auto offset = 0; offset != int(list.size());) {
			const auto count = std::min(
				kBatchSize,
				int(list.size()) - offset);
			_batches.push_back({
				peerId,
				MessageIdsList(
					list.begin() + offset,
					list.begin() + offset + count),
			});
			offset += count;
		}
	}
	requestNextBatch();
}

void SelectionScanner::pause() {
	_paused = true;
}

void SelectionScanner::resume() {
	if (!_paused) {
		return;
	}
	_paused = false;
	requestNextBatch();
}

void SelectionScanner::cancel() {
	if (_requestId) {
		_session->api().request(base::take(_requestId)).cancel();
	}
	_cancelled = true;
	_paused = false;
	_done = nullptr;
	_finished = nullptr;
	_fail = nullptr;
	releaseTakeout();
}

void SelectionScanner::acquireTakeout(not_null<PeerData*> peer) {
	_takeoutRequested = true;
	_session->api().ensureTakeout(peer, crl::guard(this, [=](bool ok) {
		if (ok && !_takeoutBorrowed) {
			_takeoutBorrowed = true;
			_session->api().setTakeoutBorrowed(true);
			if (const auto id = _session->api().takeoutId()) {
				_takeoutId = *id;
			}
		}
		if (_cancelled) {
			releaseTakeout();
			return;
		}
		requestNextBatch();
	}));
}

void SelectionScanner::requestNextBatch() {
	if (_cancelled || _paused) {
		return;
	}
	PeerData *peer = nullptr;
	while (true) {
		if (_cancelled) {
			return;
		} else if (_batchIndex >= int(_batches.size())) {
			finish();
			return;
		}
		peer = _session->data().peerLoaded(_batches[_batchIndex].peerId);
		if (peer) {
			break;
		}
		++_batchIndex;
	}

	if (!_takeoutRequested) {
		acquireTakeout(not_null{ peer });
		return;
	}

	const auto peerId = _batches[_batchIndex].peerId;
	auto ids = QVector<MTPInputMessage>();
	ids.reserve(int(_batches[_batchIndex].ids.size()));
	for (const auto &id : _batches[_batchIndex].ids) {
		ids.push_back(MTP_inputMessageID(MTP_int(id.msg.bare)));
	}

	const auto weak = base::make_weak(this);
	const auto done = [=](
			const MTPmessages_Messages &result,
			mtpRequestId requestId) {
		if (!weak) {
			return;
		}
		weak->_requestId = 0;
		weak->handleBatch(result, peerId);
	};
	const auto failed = [=](const MTP::Error &error) {
		if (!weak) {
			return;
		}
		weak->_requestId = 0;
		if (weak->_takeoutId && !weak->_takeoutFallbackTried) {
			weak->_takeoutFallbackTried = true;
			LOG(("BulkForward: takeout selection read failed (%1), retrying."
				).arg(error.type()));
			weak->releaseTakeout();
			weak->requestNextBatch();
			return;
		}
		weak->fail(error.type());
	};

	const auto send = [&](auto request) {
		using Request = decltype(request);
		if (_takeoutId) {
			_requestId = _session->api().request(
				MTPInvokeWithTakeout<Request>(
					MTP_long(_takeoutId),
					std::move(request))
			).done(done).fail(failed).send();
		} else {
			_requestId = _session->api().request(std::move(request))
				.done(done)
				.fail(failed)
				.send();
		}
	};
	if (const auto channel = peer->asChannel()) {
		send(MTPchannels_GetMessages(
			channel->inputChannel(),
			MTP_vector<MTPInputMessage>(std::move(ids))
		));
	} else {
		send(MTPmessages_GetMessages(
			MTP_vector<MTPInputMessage>(std::move(ids))
		));
	}
}

void SelectionScanner::handleBatch(
		const MTPmessages_Messages &result,
		PeerId peerId) {
	if (_cancelled) {
		return;
	}
	result.match([&](const MTPDmessages_messages &data) {
		handleMessages(data.vmessages().v, peerId);
	}, [&](const MTPDmessages_messagesSlice &data) {
		handleMessages(data.vmessages().v, peerId);
	}, [&](const MTPDmessages_channelMessages &data) {
		handleMessages(data.vmessages().v, peerId);
	}, [&](const auto &) {
	});
	++_batchIndex;
	if (!_matched.empty()) {
		auto matched = std::move(_matched);
		_matched.clear();
		if (const auto done = _done) {
			done(std::move(matched));
		}
	}
	requestNextBatch();
}

void SelectionScanner::handleMessages(
		const QVector<MTPMessage> &messages,
		PeerId peerId) {
	const auto peer = _session->data().peerLoaded(peerId);
	const auto peerProtected = peer
		&& EnhancedForward::PeerNeedsEnhancedForward(peer);
	for (const auto &message : messages) {
		const auto source = Classify(message, peerId, peerProtected);
		if (source && MatchesContent(_filter, *source)) {
			_matched.push_back(*source);
		}
	}
}

void SelectionScanner::releaseTakeout() {
	if (!_takeoutBorrowed) {
		return;
	}
	_takeoutBorrowed = false;
	_takeoutId = 0;
	_session->api().setTakeoutBorrowed(false);
	// The shared session stays open for the rest of the app run. Finishing it
	// here would trigger TAKEOUT_INIT_DELAY_ on the next scan and force the
	// slow path. It is finished on exit or by another shared-session user.
}

void SelectionScanner::finish() {
	if (_cancelled) {
		return;
	}
	_cancelled = true;
	releaseTakeout();
	if (!_matched.empty()) {
		if (const auto done = _done) {
			done(std::move(_matched));
		}
		_matched.clear();
	}
	const auto finished = std::move(_finished);
	_done = nullptr;
	_finished = nullptr;
	_fail = nullptr;
	if (finished) {
		finished();
	}
}

void SelectionScanner::fail(const QString &error) {
	if (_cancelled) {
		return;
	}
	_cancelled = true;
	releaseTakeout();
	const auto fail = std::move(_fail);
	_done = nullptr;
	_fail = nullptr;
	if (fail) {
		fail(error);
	}
}

} // namespace BulkForward
