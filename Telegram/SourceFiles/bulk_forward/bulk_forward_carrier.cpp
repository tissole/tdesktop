/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk_forward/bulk_forward_carrier.h"

#include "apiwrap.h"
#include "base/algorithm.h"
#include "base/random.h"
#include "data/data_forward_dedup.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"

#include <algorithm>
#include <limits>

namespace BulkForward {
namespace {

[[nodiscard]] int IntId(MsgId id) {
	return int(std::clamp<int64>(
		id.bare,
		int64(0),
		int64(std::numeric_limits<int>::max())));
}

} // namespace

CarrierForwarder::CarrierForwarder(
	not_null<Main::Session*> session,
	not_null<PeerData*> destination,
	MsgId topicRootId)
: _session(session)
, _destination(destination)
, _topicRootId(topicRootId) {
}

CarrierForwarder::~CarrierForwarder() {
	cancel();
}

void CarrierForwarder::cancel() {
	_cancelled = true;
	if (_requestId) {
		_session->api().request(base::take(_requestId)).cancel();
	}
	_sent.clear();
	done = nullptr;
}

void CarrierForwarder::send(
		not_null<PeerData*> carrier,
		std::vector<CarrierItem> items) {
	if (_cancelled || items.empty()) {
		finish(Result());
		return;
	}
	const auto context = Data::ForwardDedup::Context{
		.destination = _destination->id,
		.checkFwTmp = true,
		.sessionId = _session->uniqueId(),
	};
	auto kept = std::vector<CarrierItem>();
	kept.reserve(items.size());
	for (auto &item : items) {
		auto duplicate = false;
		if (item.mediaId) {
			duplicate = Data::ForwardDedup::CheckMedia(
				context,
				item.mediaId,
				item.isPhoto,
				false) != Data::ForwardDedup::Verdict::Allowed;
		}
		if (!duplicate && !item.hash.isEmpty()) {
			duplicate = Data::ForwardDedup::CheckHash(
				context,
				item.hash,
				false) != Data::ForwardDedup::Verdict::Allowed;
		}
		if (duplicate) {
			++_skipped;
		} else {
			kept.push_back(std::move(item));
		}
	}
	if (kept.empty()) {
		auto result = Result();
		result.skipped = _skipped;
		finish(std::move(result));
		return;
	}

	auto ids = QVector<MTPint>();
	auto randoms = QVector<MTPlong>();
	ids.reserve(int(kept.size()));
	randoms.reserve(int(kept.size()));
	for (const auto &item : kept) {
		ids.push_back(MTP_int(IntId(item.carrierId)));
		randoms.push_back(MTP_long(base::RandomValue<uint64>()));
	}
	_sent = std::move(kept);

	using Flag = MTPmessages_ForwardMessages::Flag;
	const auto realTopMsgId = (_topicRootId == Data::ForumTopic::kGeneralId)
		? MsgId(0)
		: _topicRootId;
	const auto flags = Flag(0)
		| Flag::f_drop_author
		| (realTopMsgId ? Flag::f_top_msg_id : Flag(0));
	const auto weak = base::make_weak(this);
	_requestId = _session->api().request(MTPmessages_ForwardMessages(
		MTP_flags(flags),
		carrier->input(),
		MTP_vector<MTPint>(ids),
		MTP_vector<MTPlong>(randoms),
		_destination->input(),
		MTP_int(IntId(realTopMsgId)),
		MTPInputReplyTo(),
		MTP_int(0),
		MTP_int(0),
		MTP_inputPeerEmpty(),
		MTPInputQuickReplyShortcut(),
		MTP_long(0),
		MTPint(),
		MTP_long(0),
		MTPSuggestedPost()
	)).done([weak](const MTPUpdates &result) {
		if (const auto strong = weak.get()) {
			strong->handleSuccess(result);
		}
	}).fail([weak](const MTP::Error &error) {
		if (const auto strong = weak.get()) {
			strong->handleFailure(error.type());
		}
	}).send();
}

void CarrierForwarder::handleSuccess(const MTPUpdates &result) {
	_requestId = 0;
	auto out = Result();
	out.forwarded = int(_sent.size());
	out.sent = std::move(_sent);
	out.skipped = _skipped;
	_sent.clear();
	_session->api().applyUpdates(result);
	finish(std::move(out));
}

void CarrierForwarder::handleFailure(QString error) {
	_requestId = 0;
	auto out = Result();
	out.skipped = _skipped;
	out.error = std::move(error);
	out.fallback = std::move(_sent);
	_sent.clear();
	finish(std::move(out));
}

void CarrierForwarder::finish(Result &&result) {
	const auto callback = std::move(done);
	done = nullptr;
	if (callback) {
		callback(std::move(result));
	}
}

} // namespace BulkForward
