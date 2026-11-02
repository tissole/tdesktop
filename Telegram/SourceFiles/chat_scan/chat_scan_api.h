/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_map.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_dedup_db.h"
#include "data/data_msg_id.h"
#include "mtproto/sender.h"
#include "rpl/variable.h"
#include "scheme.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <memory>
#include <optional>
#include <vector>
#include <unordered_map>
#include <utility>
#include <unordered_set>

namespace Data {
class DedupDb;
} // namespace Data

class DocumentData;
class PeerData;
class PhotoData;

namespace Main {
class Session;
} // namespace Main

namespace ChatScan {

class ChunkScheduler;
class ScanQueue;

struct CategoryTotals {
	int uniqueCount = 0;
	int64 uniqueBytes = 0;
	int dupCount = 0;
	int64 dupBytes = 0;
};

struct State {
	enum class Phase {
		Scanning,
		Paused,
		Resolving,
		Finished,
		Cancelled,
		Failed,
		Cancelling,
	};
	Phase phase = Phase::Scanning;
	int scannedCount = 0;
	int totalCount = 0;
	base::flat_map<int, CategoryTotals> categories;
	int dupTotal = 0;
	int64 dupBytes = 0;
	int hashDone = 0;
	int hashPending = 0;
	int hashTotal = 0;
	int mediaTotal = 0;
	int failedHashes = 0;
	int64 elapsedMs = 0;
	int64 floodUntilMs = 0;
	QString error;
};

class Api final : public base::has_weak_ptr {
public:
	Api(not_null<Main::Session*> session, not_null<PeerData*> peer);
	~Api();

	[[nodiscard]] rpl::producer<State> state() const;
	[[nodiscard]] State currentState() const;
	[[nodiscard]] bool clearing() const;

	void start();
	void rescan();
	void pause();
	void resume();
	[[nodiscard]] bool loadPaused(const Data::ChatScanState &cursor);
	void cancel();
	void finishTakeout();

private:
	void requestPage();
	void schedulePage();
	void startScheduler();
	void stopScheduler();
	[[nodiscard]] crl::time pageDelay() const;
	void requestMediaTotals();
	void requestMediaCount(const MTPMessagesFilter &filter, Fn<void(int)> done);
	[[nodiscard]] bool needsTakeout() const;
	void ensureScanTakeout(Fn<void()> ready);
	void finishBorrowedTakeout();
	void rescanAfterTakeout();
	void beginFreshScan();
	void startFullRescan();
	void dispatchResumed(int gen);
	void handlePage(const MTPmessages_Messages &result);
	void handlePageMessages(const QVector<MTPMessage> &messages);
	void handleRegularMessage(const MTPDmessage &message);
	void addDocOccurrence(
		not_null<DocumentData*> document,
		MsgId msgId);
	void addPhotoOccurrence(
		not_null<PhotoData*> photo,
		MsgId msgId);
	void commitPage();
	void maybeRequestNext();
	void rebuildStep();
	void continueRebuildStep();
	void applyRebuildProgress(
		std::vector<Data::ChatScanProgressRow> progress,
		bool success);
	void restoreRebuiltItem(const Data::ChatScanItem &item);
	[[nodiscard]] bool seedFromProgress(
		const std::vector<Data::ChatScanProgressRow> &progress);
	void rememberRestoredMedia(const Data::ChatScanItem &item);
	void recoverFromCursor(const Data::ChatScanState &cursor);
	void requestRecoveryProgress();
	void applyRecoveredProgress(
		std::vector<Data::ChatScanProgressRow> progress,
		bool success);
	void maybeRequestRecovery();
	void refillRecoveryWindow();
	void requestRecoveryBatch();
	void handleRecoveryMessages(const QVector<MTPMessage> &messages);
	void handleRecoveryMessage(const MTPDmessage &message);
	void publish(bool force = false);
	void noteFloodWait(int seconds);
	void finish();
	void fail(const QString &error);
	void schedulerUpdate();
	void logSchedulerStarvation();
	void maybeFinalize();
	void onHashed(const Data::ChatScanItem &item, const QByteArray &hash);
	void applyOwnerDecision(
		const Data::ChatScanItem &item,
		const QByteArray &hash,
		const Data::ChatScanOwnerDecision &decision);
	void onHashFailed(const Data::ChatScanItem &item);
	void demoteToDuplicate(const Data::ChatScanItem &item);
	[[nodiscard]] QString filenameForMedia(
		uint64 mediaId,
		bool isPhoto) const;
	[[nodiscard]] QByteArray knownHashForMedia(
		uint64 mediaId,
		bool isPhoto) const;
	void loadKnownHashes(Fn<void()> done);
	void loadResumeSets(Fn<void()> done);
	void loadStickerIndexAsync();
	void uniqueHashesLoaded(
		std::vector<std::pair<QByteArray, MsgId>> rows,
		bool success);
	void stickerIndexLoaded(std::vector<MsgId> ids, bool success);
	void applyStickerIndex(std::vector<MsgId> ids);
	void resumeSetLoaded();
	void finishResumeSets(bool seeded);
	bool onHashReferenceExpired(const Data::ChatScanItem &item);
	void onHashCommitted(int resolved);
	void finalizeAndWrite();
	void writeAggregateRows();
	[[nodiscard]] Data::DedupDb &db() const;
	[[nodiscard]] int64 elapsedMs() const;
	void startElapsed();
	void freezeElapsed();

	const not_null<Main::Session*> _session;
	const not_null<PeerData*> _peer;

	MTP::Sender _api;
	rpl::variable<State> _state;
	base::Timer _floodTimer;
	base::Timer _pageTimer;
	int _mediaTotal = 0;
	bool _mediaTotalsRequested = false;
	base::flat_map<int, CategoryTotals> _totals;
	std::unique_ptr<ScanQueue> _queue;
	std::optional<Data::DedupDb::TableUse> _uploadTableUse;
	std::unique_ptr<ChunkScheduler> _scheduler;
	bool _schedulerStarted = false;
	std::unordered_map<uint64, DocumentData*> _documents;
	std::unordered_map<uint64, PhotoData*> _photos;
	std::unordered_set<uint64> _seenDocs;
	std::unordered_set<uint64> _seenPhotos;
	std::unordered_set<uint64> _staleMedia;
	struct UniqueHash {
		MsgId msgId = MsgId(0);
		int category = 0;
		int64 size = 0;
	};
	std::unordered_set<int64> _storedIds;
	QHash<QByteArray, UniqueHash> _uniqueHashes;
	std::vector<Data::ChatScanItem> _pageItems;
	std::vector<MsgId> _pageStickers;
	int _stickerTotal = 0;
	std::vector<int32> _recoveryIds;
	int _recoveryPos = 0;
	int32 _recoveryAbove = 0;
	bool _recoveryDone = true;
	bool _recoveryWindowDone = true;
	bool _recoveryInFlight = false;
	int _recoveryErrorStreak = 0;
	base::Timer _recoveryTimer;
	bool _aggregating = false;
	base::Timer _rebuildTimer;
	int64 _rebuildAboveBare = 0;
	bool _rebuilding = false;
	bool _recomputingProgress = false;
	bool _loadingResumeSets = false;
	int _resumeSetsPending = 0;
	Fn<void()> _resumeSetsDone;

	int32 _offsetId = 0;
	bool _pageInFlight = false;
	int32 _highMark = 0;
	int _scanned = 0;
	int _walkedNew = 0;
	int64 _stopMark = 0;
	int _hashDone = 0;
	int _failedHashes = 0;
	int _queueOutstanding = 0;
	uint64 _sessionId = 0;
	bool _incremental = false;
	int64 _elapsedMs = 0;
	crl::time _elapsedStartedAt = 0;
	bool _walkerCreditBlocked = false;
	crl::time _starvationSince = 0;
	crl::time _lastStarvationLogMs = 0;
	int64 _floodWaitUntilMs = 0;
	int64 _lastPublishMs = 0;
	State::Phase _lastPhase = State::Phase::Scanning;
	bool _paused = false;
	int _resumeGen = 0;
	bool _clearing = false;
	bool _cancelled = false;
	bool _failed = false;
	bool _recovering = false;
	bool _rescan = false;
	bool _walkDone = false;
	bool _finished = false;
	bool _postFinishRetryUsed = false;
};

} // namespace ChatScan
