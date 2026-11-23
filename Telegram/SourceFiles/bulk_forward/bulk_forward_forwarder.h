/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "bulk_forward/bulk_forward_batcher.h"
#include "base/basic_types.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "base/weak_ptr.h"
#include "data/data_forward_seen.h"
#include "data/data_msg_id.h"

#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace Main {
class Session;
} // namespace Main

class PeerData;

namespace BulkForward {

// Shared with the copy pipeline, so both halves of a mixed run agree on what
// this run is already sending to its other destinations.
using RunSeen = Data::ForwardRunSeen;

// Sends ready batches to one destination chat, one request at a time and in
// the order they were enqueued, so nothing lands out of order.
//
// Before a batch is sent every media item is checked against the destination
// the same way the other transfer paths do it: first by document or photo id,
// then by the content fingerprint sampled from the file itself. The sample is
// resolved for the whole batch before the request, so a duplicate never leaves
// the client and the check never blocks the next batch for long.
//
// Batches that cannot be sent as a plain forward - protected ones, or ones the
// server silently refuses - are handed to the job through `unhandled`, which
// copies them through the enhanced forward pipeline instead of dropping them.
class Forwarder final : public base::has_weak_ptr {
public:
	// Range of source messages still waiting for a send: `from` is the lowest
	// pending id per source peer, `count` how many matched messages it covers.
	// Used by the resume checkpoint, so it must stay conservative.
	struct PendingInfo {
		base::flat_map<PeerId, MsgId> from;
		int count = 0;
	};

	Forwarder(
		not_null<Main::Session*> session,
		not_null<PeerData*> destination,
		MsgId topicRootId,
		std::shared_ptr<RunSeen> runSeen,
		int sent = 0,
		int skipped = 0);
	~Forwarder();

	void enqueue(Batch batch);
	void cancel();
	void pause();
	void resume();

	[[nodiscard]] bool paused() const {
		return _paused;
	}
	[[nodiscard]] bool cancelled() const {
		return _cancelled;
	}
	[[nodiscard]] PeerId destinationId() const;
	[[nodiscard]] MsgId topicRootId() const {
		return _topicRootId;
	}
	[[nodiscard]] bool idle() const {
		return _queue.empty() && !_sending && !_preparing && !_busy;
	}
	[[nodiscard]] int sent() const {
		return _sent;
	}
	[[nodiscard]] int skipped() const {
		return _skipped;
	}
	// Source messages that disappeared before they could be sent; they stay
	// in the run totals so the bar does not claim a full delivery.
	[[nodiscard]] int failedCount() const {
		return _failed;
	}
	[[nodiscard]] PendingInfo pendingInfo() const;

	// Running total of messages the server accepted.
	Fn<void(int sent)> progress;
	// Called once when the queue turns from busy to empty.
	Fn<void()> drained;
	Fn<void(Batch)> unhandled;
	Fn<void(QString)> failed;

private:
	void pump();
	void beginPrepare();
	[[nodiscard]] bool scanPrepared();
	[[nodiscard]] bool flushPrepared();
	[[nodiscard]] bool sendApproved();
	[[nodiscard]] bool dropMissingApproved();
	void startSample(int index);
	void sampleResolved(int index, QByteArray hash);
	void approve(int index, QByteArray hash);
	void skip(int index);
	void forget(const SourceMessage &item, const QByteArray &hash);
	void handoffRefused(PeerId sourcePeer, std::vector<SourceMessage> items);
	void releasePending();
	void clearPrepared();
	void handleSuccess(const MTPUpdates &result);
	void handleFailure(const QString &error);

	const not_null<Main::Session*> _session;
	const not_null<PeerData*> _destination;
	const MsgId _topicRootId;
	std::deque<Batch> _queue;
	Batch _current;
	std::vector<uchar> _decisions;
	std::vector<QByteArray> _hashes;
	std::vector<SourceMessage> _approved;
	std::vector<QByteArray> _approvedHashes;
	std::vector<uint64> _approvedRandomIds;
	std::vector<std::pair<uint64, bool>> _pendingKeys;
	// Kept for the whole run, not per batch, so the same content is not sent
	// twice when the source chat lists it under several messages.
	base::flat_set<std::pair<uint64, bool>> _seenMedia;
	base::flat_set<QByteArray> _seenHashes;
	const std::shared_ptr<RunSeen> _runSeen;
	int _scanIndex = 0;
	int _inflight = 0;
	int _sent = 0;
	int _skipped = 0;
	int _failed = 0;
	mtpRequestId _requestId = 0;
	bool _preparing = false;
	bool _sending = false;
	bool _busy = false;
	bool _paused = false;
	bool _cancelled = false;

};

} // namespace BulkForward
