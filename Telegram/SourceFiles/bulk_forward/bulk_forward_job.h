/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "bulk_forward/bulk_forward_batcher.h"
#include "bulk_forward/bulk_forward_carrier.h"
#include "bulk_forward/bulk_forward_count.h"
#include "bulk_forward/bulk_forward_filter.h"
#include "bulk_forward/bulk_forward_forwarder.h"
#include "bulk_forward/bulk_forward_resume.h"
#include "bulk_forward/bulk_forward_scanner.h"
#include "bulk_forward/bulk_forward_selection.h"
#include "base/basic_types.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "base/weak_ptr.h"
#include "data/data_msg_id.h"
#include "enhanced_forward.h"

#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace Main {
class Session;
} // namespace Main

class PeerData;

namespace BulkForward {

struct Destination {
	not_null<PeerData*> peer;
	MsgId topicRootId;
};

struct JobRequest {
	not_null<PeerData*> source;
	MsgId sourceTopicRootId;
	Filter filter;
	std::vector<Destination> destinations;
	// Present when a paused run is continued from its durable record.
	std::optional<ResumeState> resume;
};

// Runs one bulk forward: reads the source chat, turns the matches into send
// batches and fans every batch out to all destinations.
//
// The scan streams, so the first batches are on their way while the rest of the
// chat is still being read. Each destination keeps its own send order, so all
// of them receive the same messages in the same order.
class Job final : public base::has_weak_ptr {
public:
	explicit Job(not_null<Main::Session*> session);
	~Job();

	void start(JobRequest &&request);
	void pause();
	void resume();
	void cancel();

	[[nodiscard]] bool paused() const {
		return _paused;
	}
	[[nodiscard]] bool finished() const {
		return _finished;
	}
	[[nodiscard]] not_null<Main::Session*> session() const {
		return _session;
	}
	[[nodiscard]] int scanned() const {
		return _scanned;
	}
	[[nodiscard]] int total() const {
		return _total;
	}
	[[nodiscard]] int matched() const {
		return _matched;
	}
	[[nodiscard]] int sent() const;
	[[nodiscard]] int skipped() const;

	// One entry per destination that stopped with an error, "name (error)".
	// The other destinations keep running.
	[[nodiscard]] const std::vector<QString> &failures() const {
		return _failures;
	}

	// Any counter changed.
	Fn<void()> updated;
	// The scan and every destination are finished.
	Fn<void()> done;
	Fn<void(QString)> failed;

private:
	void scanBatch(std::vector<SourceMessage> items);
	void scanProgress(int scanned, int total, int matched);
	void scanFinished();
	void scanFailed(const QString &error);
	void destinationFailed(const PeerId &destination, QString error);
	void enqueueProtected(Batch batch);
	void processProtected();
	void protectedResolved(Batch batch);
	void protectedCopySettled(
		int token,
		PeerId destination,
		int lostBefore,
		std::vector<EnhancedForward::SettledItem> &&results);
	void protectedItemSettled(const EnhancedForward::SettledItem &item);
	void startCarrierAttempt();
	void carrierReported(
		int token,
		PeerId candidate,
		std::vector<EnhancedForward::SettledItem> &&results);
	void startCarrierFanout();
	void carrierForwarded(
		PeerId destination,
		CarrierForwarder::Result &&result);
	void startCarrierFallback(
		PeerId destination,
		std::vector<CarrierItem> items);
	bool markCarrierDone(
		PeerId destination,
		const std::vector<CarrierItem> &items);
	void scheduleFinishCarrierFanout();
	void finishCarrierFanout();
	[[nodiscard]] MsgId forwarderTopicRoot(PeerId destination) const;
	[[nodiscard]] bool forwarderAlive(PeerId destination) const;
	void flushBatcher();
	void enqueue(Batch batch);
	void updateScanFlow();
	void holdScan(bool held);
	[[nodiscard]] int pendingAhead() const;
	[[nodiscard]] qint64 pendingBytes() const;
	void checkDone();
	void notifyUpdated();
	void updateCounters();
	void finishCounters();
	void saveCheckpoint(bool paused);

	const not_null<Main::Session*> _session;
	HistoryScanner _scanner;
	SelectionScanner _selection;
	Batcher _batcher;
	std::vector<std::unique_ptr<Forwarder>> _forwarders;
	std::shared_ptr<RunSeen> _runSeen;
	std::unique_ptr<CountProbe> _previewProbe;
	int _previewTotal = 0;
	MsgId _previewLastId;
	Filter _filter;
	PeerId _source;
	MsgId _sourceTopicRootId;
	int _scanned = 0;
	int _total = 0;
	int _matched = 0;
	MsgId _firstMatchedId;
	MsgId _lastMatchedId;
	int _scannedBase = 0;
	int _matchedBase = 0;
	qint64 _knownBytes = 0;
	qint64 _doneBytes = 0;
	qint64 _skippedBytes = 0;
	base::flat_set<FullMsgId> _byteCounted;
	int64 _startedAt = 0;
	crl::time _lastCheckpoint = 0;
	bool _scanDone = false;
	bool _scanHeld = false;
	bool _finished = false;
	bool _failed = false;
	bool _paused = false;
	bool _inFlush = false;
	bool _donePending = false;
	bool _protectedBusy = false;
	PeerId _protectedBusyPeer;
	MsgId _protectedBusyFrom;
	int _protectedBusyCount = 0;
	qint64 _protectedBusyBytes = 0;
	std::vector<SourceMessage> _protectedBusyItems;
	base::flat_set<FullMsgId> _protectedSettled;
	bool _protectedBusyDirectCopy = false;
	PeerId _protectedBusyDestination;
	int _protectedCopyToken = 0;
	std::deque<Batch> _protectedQueue;
	struct ProtectedCount {
		int handed = 0;
		int duplicates = 0;
		int failed = 0;
	};
	base::flat_map<PeerId, ProtectedCount> _protectedCounts;

	struct FanoutItem {
		CarrierItem item;
		base::flat_set<PeerId> alreadyHave;
		base::flat_set<PeerId> done;
	};
	struct CarrierBatch {
		std::vector<FullMsgId> resolved;
		std::vector<FullMsgId> retry;
		std::vector<FanoutItem> created;
		std::vector<PeerId> candidates;
		base::flat_map<FullMsgId, qint64> sizes;
		base::flat_map<FullMsgId, base::flat_set<PeerId>> skippedAt;
		std::vector<std::unique_ptr<CarrierForwarder>> forwarders;
		std::shared_ptr<RunSeen> attemptBefore;
		int candidateIndex = 0;
		int runLost = 0;
		int pendingForwarders = 0;
		bool dispatching = false;
		bool finishScheduled = false;
	};
	std::unique_ptr<CarrierBatch> _fanout;
	std::vector<ResumeCarrierItem> _carrierResume;
	int _carrierToken = 0;
	std::vector<QString> _failures;

};

} // namespace BulkForward
