/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QByteArray>
#include <QString>
#include <atomic>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "data/data_msg_id.h"

namespace Data {

struct DedupRecord {
	QByteArray hash;
	uint64 documentId = 0;
	bool isPhoto = false;
	QString status = u"f"_q;
};

enum class JobKind {
	Download,
	DownloadBatch,
	Upload,
	UploadBatch,
	Forward,
	ForwardBatch,
	Export,
	ExportBatch,
	EnhancedForward,
	Scan,
};

enum class JobState : int {
	Pending = 0,
	InFlight = 1,
	Done = 2,
	Skipped = 3,
	Failed = 4,
};

struct JobRecord {
	uint64 jobId = 0;
	JobKind kind = JobKind::Download;
	uint64 sessionId = 0;
	uint64 scopeId = 0;
	QByteArray options;
	JobState state = JobState::Pending;
	int total = 0;
	int done = 0;
	int skipped = 0;
	int64 createdAt = 0;
	int64 updatedAt = 0;
};

struct JobItemRecord {
	uint64 jobId = 0;
	uint64 itemId = 0;
	uint64 mediaId = 0;
	bool isPhoto = false;
	uint64 hashId = 0;
	JobState state = JobState::Pending;
	QByteArray payload;
	QString error;
	int retry = 0;
};

struct DlResumeRecord {
	uint64 sessionId = 0;
	uint64 peerId = 0;
	int64 msgId = 0;
	QString path;
	qint64 fileSize = 0;
	uint64 docId = 0;
};

struct UlResumeRecord {
	uint64 sessionId = 0;
	uint64 peerId = 0;
	QString path;
	int partsSent = 0;
	int64 sentSize = 0;
	uint64 fileId = 0;
	int64 topicRootId = 0;
};

struct EfResumeItem {
	uint64 sessionId = 0;
	QString jobId;
	int itemIndex = 0;
	PeerId peerId;
	FullMsgId sourceId;
	QString state;
	QString localPath;
	uint64 fileId = 0;
	int uploadedParts = 0;
	QByteArray fileHash;
	uint64 mediaId = 0;
	qint64 fileSize = 0;
	bool paused = false;
};

struct NfResumeRecord {
	uint64 sessionId = 0;
	PeerId destPeerId = PeerId();
	PeerId srcPeerId = PeerId();
	int total = 0;
	int done = 0;
	int skipped = 0;
	MsgId lastMsgId = 0;
	QString state;
	// int-typed on purpose (like ExResumeRecord::exportFormat): a resumed
	// job has to rebuild the same copy and grouping behaviour, otherwise a
	// RegroupAll forward would silently continue as GroupAsIs.
	int forwardOptions = 0;
	int groupOptions = 0;
	std::vector<MsgId> remaining;
};

struct ExResumeRecord {
	uint64 sessionId = 0;
	PeerId peerId = PeerId();
	// 0 = single chat, 1 = global run. Both workflows can keep a
	// record for the same chat.
	int mode = 0;
	int total = 0;
	int msgsDone = 0;
	int skipped = 0;
	MsgId lastId = 0;
	QString exportFolder;
	QString state;
	uint32 media = 0;
	int64 size = 0;
	int exportFormat = 0;
	int fromDate = 0;
	int tillDate = 0;
	int htmlIndex = 0;
	QByteArray repliedIndex;
	int dateIndex = 0;
	QByteArray stats;
	uint64 docId = 0;
	QString pausedFile;
	int64 pausedBytes = 0;
	QByteArray lastMsg;
	int splitIndex = 0;
	int selectedDone = 0;
	int filterIndex = 0;
	bool useIdRange = false;
	uint64 fromId = 0;
	uint64 tillId = 0;
	MsgId updateAnchor = 0;
	MsgId coveredTill = 0;
	int coveredTillDate = 0;
	bool migrated = false;
	// Global-run marker (peerId == 0, mode == 1) and scope snapshot.
	int failedCount = 0;
	PeerId resumePeerId = PeerId();
	// Ordered snapshot of the run's chat list (marker only).
	std::vector<PeerId> dialogIds;
	int chatTypes = 0;
	int fullChats = 0;
	int extFilterMode = 0;
	QString extFilter;

	// Update is offered only when the run already covered the chat up to
	// the newest message as it stood at export time. A run cut short by a
	// date or id range must not offer it, since updating would pull in the
	// excluded messages. Compared against coveredTill/coveredTillDate (a
	// fact stored then), never against the chat's newest id now, which
	// grows the moment anything is added.
	[[nodiscard]] bool coversWholeChat() const {
		const auto anchor = updateAnchor ? updateAnchor : lastId;
		if (!anchor) {
			return false;
		}
		if (useIdRange) {
			if (!tillId) {
				return true;
			}
			return coveredTill && tillId == coveredTill.bare;
		}
		if (!tillDate) {
			return true;
		}
		if (!coveredTillDate) {
			return false;
		}
		return uint64(coveredTillDate) <= uint64(tillDate);
	}
};

struct ChatScanRow {
	PeerId peerId;
	int category = 0;
	int uniqueCount = 0;
	qint64 uniqueBytes = 0;
	int dupCount = 0;
	qint64 dupBytes = 0;
	MsgId scannedUpto;
	bool scanFinished = false;
	bool deleteFinished = false;
};

struct ChatScanDup {
	PeerId peerId;
	MsgId msgId;
	int category = 0;
	qint64 size = 0;
	uint64 mediaId = 0;
	bool isPhoto = false;
};

struct ChatScanState {
	PeerId peerId;
	MsgId cursor;
	int phase = 0;
	int64 elapsedMs = 0;
};

// Precomputed per-category totals for one peer's stored items, split by
// the is_dup flag. Backs a lightweight paused restore that must not
// materialize up to a million item rows.
struct ChatScanCategoryTotals {
	int category = 0;
	int64 uniqueCount = 0;
	int64 uniqueBytes = 0;
	int64 dupCount = 0;
	int64 dupBytes = 0;
};

struct ChatScanProgressRow {
	PeerId peerId = 0;
	int category = 0;
	int64 itemCount = 0;
	int64 uniqueCount = 0;
	int64 uniqueBytes = 0;
	int64 dupCount = 0;
	int64 dupBytes = 0;
};

struct ChatScanItem {
	PeerId peerId;
	MsgId msgId;
	int category = 0;
	qint64 size = 0;
	uint64 mediaId = 0;
	bool isPhoto = false;
	QByteArray hash;
	QString filename;
	// 0 pending, 1 leased, 2 hashed, 3 failed, 4 skipped/unhashable.
	int state = 0;
	int attempts = 0;
	qint64 leaseUntil = 0;
	bool isDup = false;
	bool tracked = true;
	uint64 sessionId = 0;
};

struct HashOwner {
	PeerId peerId;
	MsgId msgId;
	bool tracked = false;
	bool exists = false;
};

using ChatScanOwnerChange = std::pair<PeerId, QByteArray>;

struct ChatScanOwnerDecision {
	bool wasDup = false;
	bool isDup = false;
	std::optional<ChatScanItem> demoted;
	std::optional<HashOwner> owner;
};

struct ChatScanHash {
	MsgId msgId;
	uint64 mediaId = 0;
	bool isPhoto = false;
	QByteArray hash;
	QString filename;
};

struct ChatScanMediaHash {
	PeerId peerId;
	uint64 mediaId = 0;
	QByteArray hash;
	bool isPhoto = false;
	bool isDup = false;
};

class DedupDb {
public:
	enum class Table {
		Downloads,
		Uploads,
	};

	class TableUse {
	public:
		TableUse() = default;
		TableUse(TableUse &&other) noexcept;
		TableUse &operator=(TableUse &&other) noexcept;
		~TableUse();

	private:
		friend class DedupDb;
		TableUse(DedupDb *db, Table table);

		DedupDb *_db = nullptr;
		Table _table = Table::Downloads;
	};

	class ChatTableUse {
	public:
		ChatTableUse() = default;
		ChatTableUse(ChatTableUse &&other) noexcept;
		ChatTableUse &operator=(ChatTableUse &&other) noexcept;
		~ChatTableUse();

	private:
		friend class DedupDb;
		ChatTableUse(DedupDb *db, PeerId peerId, bool allPeers);

		DedupDb *_db = nullptr;
		PeerId _peerId;
		bool _allPeers = false;
	};

	explicit DedupDb(const QString &path, bool purgePending = true);
	~DedupDb();

	[[nodiscard]] TableUse useTable(Table table);
	[[nodiscard]] ChatTableUse useChatTable(PeerId peerId, bool allPeers);

	[[nodiscard]] bool isOpen() const;
	void ensureTableLoaded(Table table);
	// Upload/forward chat dedup: load the unique hashed rows of one peer or
	// of every peer once, then use the in-memory maps for each file.
	[[nodiscard]] bool chatTableContainsDocId(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto,
		bool allPeers) const;
	[[nodiscard]] bool chatTableContainsHash(
		PeerId peerId,
		const QByteArray &hash,
		bool allPeers) const;
	void clearChatTable();
	// Adds/removes one unique media/hash mapping in the live chat caches
	// without reloading them. Used when an owner moves between peers.
	void addChatTableUnique(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash);
	void removeChatTableUnique(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash);
	// Reloads one peer's chat cache in place, keeping active handles valid.
	void invalidateChatTable(PeerId peerId);
	// Same for the all-peers cache.
	void invalidateAllPeersChatTable();

	void insert(Table table, const DedupRecord &record);

	[[nodiscard]] bool containsDocId(
		Table table,
		uint64 documentId,
		bool isPhoto = false) const;
	// Direct on-disk existence check (memory maps may lag right after the
	// load window); used by one-shot prechecks, not per-tick paths.
	[[nodiscard]] bool containsDocIdInDb(
		Table table,
		uint64 documentId,
		bool isPhoto = false) const;
	[[nodiscard]] bool containsHash(
		Table table,
		const QByteArray &hash) const;
	[[nodiscard]] uint64 hashIdForHash(const QByteArray &hash);
	[[nodiscard]] uint64 findHashId(const QByteArray &hash) const;
	[[nodiscard]] QByteArray hashForHashId(uint64 hashId) const;
	[[nodiscard]] std::optional<HashOwner> hashOwner(uint64 hashId) const;
	void setHashOwner(uint64 hashId, PeerId peerId, MsgId msgId);
	void clearHashOwner(uint64 hashId);
	// With excludeMsg > 0 the exact row is excluded; with excludeMsg == 0 the
	// whole excludePeer is excluded (full-rescan case).
	[[nodiscard]] std::optional<HashOwner> findHashOwnerReplacement(
		uint64 hashId,
		PeerId excludePeer,
		MsgId excludeMsg) const;
	[[nodiscard]] bool claimOwner(
		uint64 hashId,
		PeerId peerId,
		MsgId msgId);
	void setOwner(uint64 hashId, PeerId peerId, MsgId msgId);
	void clearOwner(uint64 hashId);
	[[nodiscard]] std::optional<HashOwner> findReplacementOwner(
		uint64 hashId,
		PeerId excludePeer,
		MsgId excludeMsg) const;
	// Makes the given live row unique and the current owner duplicate.
	// Returns false when the new owner row does not exist.
	[[nodiscard]] bool promoteOwner(
		uint64 hashId,
		PeerId peerId,
		MsgId msgId);
	// Finds and promotes another live row, or clears the owner when none
	// remains. Returns the new owner when one was promoted.
	[[nodiscard]] std::optional<HashOwner> refreshOwner(
		uint64 hashId,
		PeerId excludePeer,
		MsgId excludeMsg);
	// Ensures the scan row/hash/content row, applies the global owner rule,
	// and returns the resulting current-row state. `mediaDuplicate` keeps the
	// row duplicate without changing the global owner.
	[[nodiscard]] std::optional<ChatScanOwnerDecision>
	chatScanApplyHashOwner(
		PeerId peerId,
		MsgId msgId,
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash,
		bool mediaDuplicate,
		const QString &filename);
	// Records a successfully sent upload/forward destination: ensures the
	// finished dedup_ul row, applies the global owner rule and returns whether
	// the destination is a duplicate. A tracked destination always gets a
	// chat row; an untracked one gets a minimal tracked = 0 owner row only
	// when it becomes the owner.
	[[nodiscard]] bool recordUploadDestination(
		const QByteArray &hash,
		uint64 mediaId,
		bool isPhoto,
		PeerId peerId,
		MsgId msgId,
		bool tracked);
	// Removes all scan rows of a really deleted chat (not a rescan), promotes
	// or clears the owners it held, and cleans up the finished dedup_ul rows
	// and hashes that no live row references anymore.
	[[nodiscard]] std::vector<ChatScanOwnerChange> removeChatScan(
		PeerId peerId);
	[[nodiscard]] bool containsFinishedHash(
		Table table,
		const QByteArray &hash) const;
	// Re-keys the row for an upload from the local temporary document id to
	// the definitive server-returned id (document or photo). Keeps the hash.
	void rekey(
		Table table,
		uint64 oldDocumentId,
		uint64 newDocumentId,
		bool isPhoto = false);
	void updateDedupStatus(
		Table table,
		const QByteArray &hash,
		const QString &status);
	void removeUnfinishedByHash(Table table, const QByteArray &hash);
	[[nodiscard]] QByteArray hashForDocId(
		Table table,
		uint64 documentId,
		bool isPhoto = false) const;
	[[nodiscard]] uint64 seekDocumentId(
		Table table,
		const QByteArray &hash,
		uint64 excludeDocumentId = 0,
		bool excludeIsPhoto = false) const;

	// In-flight (RAM only) registration: content that is being downloaded /
	// uploaded right now and doesn't have a durable row yet.
	void addPending(
		Table table,
		uint64 documentId,
		const QByteArray &hash,
		bool isPhoto = false);
	void removePending(
		Table table,
		uint64 documentId,
		bool isPhoto = false);

	[[nodiscard]] std::vector<DedupRecord> loadAll(Table table) const;

	void insertDlResume(const DlResumeRecord &record);
	void removeDlResume(uint64 sessionId, uint64 peerId, int64 msgId);
	[[nodiscard]] std::vector<DlResumeRecord> loadAllDlResume() const;

	void insertUlResume(const UlResumeRecord &record);
	void removeUlResume(
		uint64 sessionId,
		uint64 peerId,
		const QString &path);
	void clearUlResume(uint64 sessionId);
	[[nodiscard]] std::vector<UlResumeRecord> loadAllUlResume(
		uint64 sessionId) const;

	void insertEfResumeItem(const EfResumeItem &item);
	void removeEfResumeItem(
		const QString &jobId,
		int itemIndex);
	void removeEfResumeBySource(
		uint64 sessionId,
		PeerId destPeerId,
		MsgId sourceMsgId);
	void clearEfResumeForPeer(PeerId peerId);
	void clearEfResumeJob(const QString &jobId);
	[[nodiscard]] std::vector<EfResumeItem> loadEfResumeItemsForPeer(
		uint64 sessionId,
		PeerId peerId) const;
	[[nodiscard]] std::vector<EfResumeItem> loadUnfinishedEfResumeItems(
		uint64 sessionId) const;
	void setEfResumePaused(uint64 sessionId, PeerId peerId, bool paused);

	void insertNfResume(const NfResumeRecord &record);
	void removeNfResume(uint64 sessionId, PeerId destPeerId);
	void clearNfResume(uint64 sessionId);
	[[nodiscard]] std::vector<NfResumeRecord> loadNfResume(
		uint64 sessionId) const;

	void insertExResume(const ExResumeRecord &record);
	void removeExResumeFolder(
		uint64 sessionId,
		PeerId peerId,
		int mode,
		const QString &folder);
	void clearExResume(uint64 sessionId, int mode);
	[[nodiscard]] std::vector<ExResumeRecord> loadExResume(
		uint64 sessionId,
		int mode) const;
	// Root rename support: moves every stored absolute folder under
	// oldPrefix to newPrefix (both with trailing slash) for one
	// session/mode, after the tree itself was renamed on disk.
	void rebaseExResumeFolder(
		uint64 sessionId,
		int mode,
		const QString &oldPrefix,
		const QString &newPrefix);

	// Same-run export dedup: temp per-chat rows, deleted on finish/cancel.
	void insertExTmp(
		uint64 sessionId,
		PeerId peerId,
		uint64 documentId,
		bool isPhoto,
		const QByteArray &hash);
	[[nodiscard]] bool containsExTmpDocId(
		uint64 sessionId,
		PeerId peerId,
		uint64 documentId,
		bool isPhoto) const;
	[[nodiscard]] bool containsExTmpHash(
		uint64 sessionId,
		PeerId peerId,
		const QByteArray &hash) const;
	void clearExTmpRun(uint64 sessionId, PeerId peerId);
	void flushExTmp();

	void insertDlTmp(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId,
		bool isPhoto,
		const QByteArray &hash);
	[[nodiscard]] bool containsDlTmpDocId(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId,
		bool isPhoto) const;
	[[nodiscard]] bool containsDlTmpHash(
		uint64 sessionId,
		uint64 batchId,
		const QByteArray &hash) const;
	void removeDlTmpDoc(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId,
		bool isPhoto);
	void clearDlTmpRun(uint64 sessionId, uint64 batchId);

	void insertUlTmp(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId,
		bool isPhoto,
		const QByteArray &hash);
	[[nodiscard]] bool containsUlTmpDocId(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId,
		bool isPhoto) const;
	[[nodiscard]] bool containsUlTmpHash(
		uint64 sessionId,
		uint64 batchId,
		const QByteArray &hash) const;
	void clearUlTmpRun(uint64 sessionId, uint64 batchId);

	void insertFwTmp(
		uint64 sessionId,
		PeerId dstPeer,
		uint64 itemId,
		bool isPhoto,
		const QByteArray &hash);
	[[nodiscard]] bool containsFwTmpItem(
		uint64 sessionId,
		PeerId dstPeer,
		uint64 itemId,
		bool isPhoto) const;
	[[nodiscard]] bool containsFwTmpHash(
		uint64 sessionId,
		PeerId dstPeer,
		const QByteArray &hash) const;
	[[nodiscard]] QByteArray hashForFwTmpItem(
		uint64 sessionId,
		PeerId dstPeer,
		uint64 itemId,
		bool isPhoto) const;
	void clearFwTmpRun(uint64 sessionId, PeerId dstPeer);

	// In-place chat scan: duplicate flags and walk cursors. Totals are
	// derived from chat_scan_items; the durable per-chat cursor and mode
	// live in the Scan job options.
	[[nodiscard]] std::vector<ChatScanRow> readChatScan(
		PeerId peerId) const;
	// Worker-connection variant used while preparing the final report.
	void readChatScanAsync(
		PeerId peerId,
		Fn<void(std::vector<ChatScanRow>, bool)> done);
	[[nodiscard]] bool isChatScanned(PeerId peerId) const;
	void markChatDeleteFinished(PeerId peerId);
	// Reclaims free pages on a worker thread after a delete; skips when a
	// scan is active or the freelist is not large enough.
	void requestIncrementalVacuum();
	// Keyed variant of the duplicate listing: `before.bare <= 0` starts from
	// the newest row, otherwise only rows below it are returned. Paging a
	// long list this way never rescans the rows already returned, which is
	// what made the OFFSET variant take minutes on big chats.
	[[nodiscard]] std::vector<ChatScanDup> readChatScanDupsBefore(
		PeerId peerId,
		int category,
		int limit,
		MsgId before) const;
	// Item rows below `above`, newest first; `above.bare <= 0` starts from
	// the newest one. Used to walk a whole stored scan in bounded slices.
	[[nodiscard]] std::vector<ChatScanItem> chatScanItemsWindow(
		PeerId peerId,
		MsgId above,
		int limit) const;
	[[nodiscard]] int chatScanDupCount(PeerId peerId, int category) const;
	[[nodiscard]] int64 chatScanDupBytes(PeerId peerId) const;
	// Aggregate paused-restore helpers: totals and resume metadata are
	// computed by SQL so a million-message scan never gets rebuilt by
	// reading every item row back into memory.
	[[nodiscard]] std::vector<ChatScanCategoryTotals> chatScanCategoryTotals(
		PeerId peerId) const;
	[[nodiscard]] qint64 chatScanItemCount(PeerId peerId) const;
	[[nodiscard]] bool chatScanCategoryHasItems(
		PeerId peerId,
		int category) const;
	[[nodiscard]] MsgId chatScanMaxMsgId(PeerId peerId) const;
	[[nodiscard]] MsgId chatScanScannedUpto(PeerId peerId) const;
	// True when a unique stored item row for the peer already has this
	// hash, i.e. the content top was walked before the pause.
	[[nodiscard]] bool chatScanContainsHash(
		PeerId peerId,
		const QByteArray &hash) const;
	[[nodiscard]] MsgId chatScanUniqueHash(
		PeerId peerId,
		const QByteArray &hash) const;
	// Any stored hash for this media id, from an earlier run; lets the
	// queue resolve a repeated media id without fetching it again.
	[[nodiscard]] QByteArray chatScanHashForMedia(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto) const;
	[[nodiscard]] QByteArray chatScanHashForMediaOther(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto) const;
	// Items whose hash never resolved before the pause; fetched in
	// bounded windows during recovery, never as one bulk read.
	[[nodiscard]] std::vector<ChatScanItem> chatScanUnresolvedItems(
		PeerId peerId) const;
	// Next window of pending items with msg_id below `above`, newest
	// first; the recovery pump advances `above` after every window.
	[[nodiscard]] std::vector<ChatScanItem> chatScanPendingItemsWindow(
		PeerId peerId,
		MsgId above,
		int limit) const;
	// How many distinct media ids still wait for a hash; seeds the
	// restored backlog display without loading the rows.
	[[nodiscard]] int chatScanPendingCount(PeerId peerId) const;
	// How many distinct media ids already carry a hash; a resumed scan
	// starts its hashed counter exactly there instead of estimating it
	// from the walked message count.
	[[nodiscard]] int chatScanHashedCount(PeerId peerId) const;
	// Durable hash-work queue: rows are pending until leased by the
	// scheduler. A start resets every lease and retry delay, so neither
	// a crash nor a previous process's clock can keep an item blocked.
	void chatScanResetLeases(PeerId peerId);
	[[nodiscard]] std::vector<ChatScanItem> chatScanLeaseItems(
		PeerId peerId,
		int limit,
		qint64 now,
		qint64 leaseUntil);
	void chatScanReleaseItem(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto,
		qint64 retryAt);
	// Releases every active lease for the peer in one statement; pause uses
	// this instead of one update per media id.
	void chatScanReleaseLeases(PeerId peerId);
	// Stores each hash on its media representative, clears the other rows
	// of that media id, and returns how many media ids were resolved.
	[[nodiscard]] int chatScanCommitHashes(
		PeerId peerId,
		const std::vector<ChatScanHash> &hashes);
	void chatScanFailItem(PeerId peerId, uint64 mediaId, bool isPhoto);
	// Puts durably failed rows back into the pending queue. Called when a
	// persisted scan is opened, so a failure from an earlier process is
	// retried instead of remaining state=3 forever.
	[[nodiscard]] int chatScanRetryFailed(PeerId peerId);
	[[nodiscard]] int chatScanLeasedCount(PeerId peerId) const;
	[[nodiscard]] int chatScanFailedCount(PeerId peerId) const;
	// Computes the per-category totals from chat_scan_items with one keyed
	// GROUP BY on a worker connection; returns them on the main thread.
	void recomputeChatScanProgressAsync(
		PeerId peerId,
		Fn<void(std::vector<ChatScanProgressRow>, bool)> done);
	// Resume-set reads run on a worker connection so a large paused scan
	// does not block the UI while its in-memory indexes are restored.
	void chatScanUniqueHashesAsync(
		PeerId peerId,
		Fn<void(std::vector<std::pair<QByteArray, MsgId>>, bool)> done);
	void chatScanItemIdsAsync(
		PeerId peerId,
		int category,
		Fn<void(std::vector<MsgId>, bool)> done);
	// Loads known media-id -> hash pairs for the scan. With cross-chat
	// dedup it reads every peer, otherwise only the current peer.
	void chatScanMediaHashesAsync(
		PeerId peerId,
		bool includeOther,
		Fn<void(std::vector<ChatScanMediaHash>, bool)> done);
	// One representative item per distinct media id with a resolved hash,
	// in walk order; backs finalize-time registry registration for the
	// portion of the scan that was never held in memory.
	[[nodiscard]] std::vector<ChatScanItem> chatScanDistinctItems(
		PeerId peerId) const;
	void clearChatScan(PeerId peerId);
	// Runs the full-chat cleanup on a worker connection; done is called on
	// the main thread with whether the database connection was opened and
	// the promoted owner hashes whose caches need invalidation.
	void clearChatScanAsync(
		PeerId peerId,
		Fn<void(bool, std::vector<ChatScanOwnerChange>)> done);
	void writeScanCursor(const ChatScanState &state);
	[[nodiscard]] std::optional<ChatScanState> readScanCursor(
		PeerId peerId) const;
	// Stores the final scan coverage mark, dedup mode and completion flags
	// in the durable Scan job.
	void finishChatScan(
		PeerId peerId,
		MsgId scannedUpto,
		int mode,
		bool deleteFinished,
		int64 elapsedMs);
	// -1 = full scan/rescan pending or incomplete, 0 = OFF, 1 = ON.
	[[nodiscard]] int chatScanMode(PeerId peerId) const;
	void writeChatScanMode(PeerId peerId, int mode);
	// Durable per-message walk record: every classified message is stored
	// as it is walked, so any resume (including after a restart) continues
	// from the cursor without recounting. Hashes are filled in alongside.
	void writeChatScanItems(const std::vector<ChatScanItem> &items);
	[[nodiscard]] std::vector<ChatScanItem> readChatScanItems(
		PeerId peerId) const;
	// Fills the hash of every still-unresolved row of that media id and
	// returns how many rows it resolved, so the scan can report item counts
	// that match the stored table. Rows that already carry a hash are never
	// touched, which keeps a re-walk from erasing a stored one.
	int updateChatScanItemHash(
		PeerId peerId,
		MsgId msgId,
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash);
	[[nodiscard]] std::vector<PeerId> scannedPeerIds() const;
	void removeChatScanItem(PeerId peerId, MsgId msgId);
	void markChatScanItemDup(
		PeerId peerId,
		MsgId msgId,
		bool isDup,
		const QString &filename = QString());
	// An incremental scan keeps a small duplicate-flag undo set and the
	// previous cursor in the Scan job so cancel can remove only this
	// session's additions.
	[[nodiscard]] bool beginChatScanSession(
		PeerId peerId,
		uint64 sessionId,
		MsgId baseMark);
	[[nodiscard]] bool cancelChatScanSession(PeerId peerId);
	void finishChatScanSession(PeerId peerId);
	[[nodiscard]] std::optional<std::pair<uint64, MsgId>> chatScanSession(
		PeerId peerId) const;
	[[nodiscard]] std::vector<MsgId> chatScanSessionItemIds(
		PeerId peerId,
		int category) const;
	[[nodiscard]] bool hasChatScanItem(PeerId peerId, MsgId msgId) const;
	[[nodiscard]] std::optional<ChatScanItem> chatScanItem(
		PeerId peerId,
		MsgId msgId) const;
	[[nodiscard]] std::optional<ChatScanItem> chatScanDuplicateSuccessor(
		PeerId peerId,
		MsgId deletedId,
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash) const;

	// Durable operation state, replacing the per-operation resume and
	// temporary batch tables.
	[[nodiscard]] uint64 createJob(const JobRecord &job);
	[[nodiscard]] std::optional<JobRecord> job(uint64 jobId) const;
	// Latest job of one kind and scope; a finished scan keeps its row as
	// durable per-peer state.
	[[nodiscard]] std::optional<JobRecord> findJob(
		JobKind kind,
		uint64 scopeId) const;
	// Latest unfinished job of one kind and scope.
	[[nodiscard]] std::optional<JobRecord> findActiveJob(
		JobKind kind,
		uint64 scopeId) const;
	void setJobState(uint64 jobId, JobState state);
	void setJobProgress(
		uint64 jobId,
		int total,
		int done,
		int skipped);
	// Pause keeps the job and its items for a later resume.
	void pauseJob(uint64 jobId);
	// Finish and cancel both remove the job and its items.
	void finishJob(uint64 jobId);
	void cancelJob(uint64 jobId);
	// A finished scan keeps its job row and drops only its undo items.
	void finishScanJob(uint64 jobId);
	void removeJobItems(uint64 jobId);

	void writeJobItems(const std::vector<JobItemRecord> &items);
	void writeJobItem(const JobItemRecord &item);
	[[nodiscard]] std::vector<JobItemRecord> jobItems(uint64 jobId) const;
	// Pending and in-flight items are what a resume must retry.
	[[nodiscard]] std::vector<JobItemRecord> jobResumeItems(
		uint64 jobId) const;
	void setJobItemState(
		uint64 jobId,
		uint64 itemId,
		JobState state,
		const QString &error = QString(),
		int retry = 0);
	// Batch dedup: only done and skipped items count as present.
	[[nodiscard]] bool jobContainsMedia(
		uint64 jobId,
		uint64 mediaId,
		bool isPhoto) const;
	[[nodiscard]] bool jobContainsHash(
		uint64 jobId,
		uint64 hashId) const;

	void beginTransaction();
	void commitTransaction();

private:
	void releaseTable(Table table);
	void releaseChatTable(PeerId peerId, bool allPeers);

	class Impl;
	std::unique_ptr<Impl> _impl;
};

} // namespace Data