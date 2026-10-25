/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QByteArray>
#include <QString>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "data/data_msg_id.h"

namespace Data {

struct DedupRecord {
	QByteArray hash;
	uint64 documentId = 0;
	QString status = u"f"_q;
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
	// 0 = single chat, 1 = global run. Part of the primary key, so the
	// two workflows' rows for the same chat coexist.
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
	// Bit 0: only copy in this chat, content known from another upload.
	int flags = 0;
	QString filename;
};

struct ChatScanState {
	PeerId peerId;
	MsgId cursor;
	int phase = 0;
};

// Precomputed per-category totals for one peer's stored items, split by
// duplicate membership from chat_scan_dups. Backs a lightweight paused
// restore that must not materialize up to a million item rows.
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
	// 0 pending, 1 leased, 2 hashed, 3 failed.
	int state = 0;
	int attempts = 0;
	qint64 leaseUntil = 0;
};

class DedupDb {
public:
	enum class Table {
		Downloads,
		Uploads,
	};

	explicit DedupDb(const QString &path, bool purgePending = true);
	~DedupDb();

	[[nodiscard]] bool isOpen() const;

	void insert(Table table, const DedupRecord &record);
	void removeByDocumentId(
		Table table,
		uint64 documentId,
		const QString &status = QString());

	[[nodiscard]] bool containsDocId(
		Table table,
		uint64 documentId) const;
	// Direct on-disk existence check (memory maps may lag right after the
	// load window); used by one-shot prechecks, not per-tick paths.
	[[nodiscard]] bool containsDocIdInDb(
		Table table,
		uint64 documentId) const;
	[[nodiscard]] bool containsHash(
		Table table,
		const QByteArray &hash) const;
	[[nodiscard]] bool containsFinishedHash(
		Table table,
		const QByteArray &hash) const;
	// Re-keys the row for an upload from the local temporary document id to
	// the definitive server-returned id (document or photo). Keeps the hash.
	void rekey(
		Table table,
		uint64 oldDocumentId,
		uint64 newDocumentId);
	void updateDedupStatus(
		Table table,
		const QByteArray &hash,
		const QString &status);
	void removeUnfinishedByHash(Table table, const QByteArray &hash);
	[[nodiscard]] QByteArray hashForDocId(
		Table table,
		uint64 documentId) const;
	[[nodiscard]] uint64 seekDocumentId(
		Table table,
		const QByteArray &hash,
		uint64 excludeDocumentId = 0) const;

	// In-flight (RAM only) registration: content that is being downloaded /
	// uploaded right now and doesn't have a durable row yet.
	void addPending(Table table, uint64 documentId, const QByteArray &hash);
	void removePending(Table table, uint64 documentId);

	[[nodiscard]] std::vector<DedupRecord> loadAll(Table table) const;

	void insertDlResume(const DlResumeRecord &record);
	void removeDlResume(uint64 sessionId, uint64 peerId, int64 msgId);
	void clearDlResume();
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
		const QByteArray &hash);
	[[nodiscard]] bool containsExTmpDocId(
		uint64 sessionId,
		PeerId peerId,
		uint64 documentId) const;
	[[nodiscard]] bool containsExTmpHash(
		uint64 sessionId,
		PeerId peerId,
		const QByteArray &hash) const;
	[[nodiscard]] QByteArray hashForExTmpDocId(
		uint64 sessionId,
		PeerId peerId,
		uint64 documentId) const;
	void clearExTmpRun(uint64 sessionId, PeerId peerId);
	void clearExTmpSession(uint64 sessionId);
	void flushExTmp();

	void insertDlTmp(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId,
		const QByteArray &hash);
	[[nodiscard]] bool containsDlTmpDocId(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId) const;
	[[nodiscard]] bool containsDlTmpHash(
		uint64 sessionId,
		uint64 batchId,
		const QByteArray &hash) const;
	[[nodiscard]] QByteArray hashForDlTmpDocId(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId) const;
	void removeDlTmpDoc(uint64 sessionId, uint64 batchId, uint64 documentId);
	void clearDlTmpRun(uint64 sessionId, uint64 batchId);

	void insertUlTmp(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId,
		const QByteArray &hash);
	[[nodiscard]] bool containsUlTmpDocId(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId) const;
	[[nodiscard]] bool containsUlTmpHash(
		uint64 sessionId,
		uint64 batchId,
		const QByteArray &hash) const;
	[[nodiscard]] QByteArray hashForUlTmpDocId(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId) const;
	void clearUlTmpRun(uint64 sessionId, uint64 batchId);

	void insertFwTmp(
		uint64 sessionId,
		PeerId dstPeer,
		uint64 itemId,
		const QByteArray &hash);
	[[nodiscard]] bool containsFwTmpItem(
		uint64 sessionId,
		PeerId dstPeer,
		uint64 itemId) const;
	[[nodiscard]] bool containsFwTmpHash(
		uint64 sessionId,
		PeerId dstPeer,
		const QByteArray &hash) const;
	[[nodiscard]] QByteArray hashForFwTmpItem(
		uint64 sessionId,
		PeerId dstPeer,
		uint64 itemId) const;
	void clearFwTmpRun(uint64 sessionId, PeerId dstPeer);

	// In-place chat scan: durable per-chat category totals, duplicate
	// message lists and walk cursors. One shared record per chat (chat
	// ids are account-independent), written by the chat scanner on the
	// main thread with transaction-batched writes; reads are UI-driven,
	// so unlike the hot dedup tables above there is no in-memory mirror.
	void writeChatScanRow(const ChatScanRow &row);
	[[nodiscard]] std::vector<ChatScanRow> readChatScan(
		PeerId peerId) const;
	[[nodiscard]] bool isChatScanned(PeerId peerId) const;
	void markChatDeleteFinished(PeerId peerId);
	void writeChatScanDup(const ChatScanDup &dup);
	// category < 0 lists and counts every category.
	[[nodiscard]] std::vector<ChatScanDup> readChatScanDups(
		PeerId peerId,
		int category,
		int limit,
		int offset) const;
	// Keyed variant of the same listing: `before.bare <= 0` starts from
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
	// Same window restricted to rows with a resolved hash, which is what
	// the finalize pass registers in the uploads table.
	[[nodiscard]] std::vector<ChatScanItem> chatScanHashedItemsWindow(
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
	[[nodiscard]] MsgId chatScanMaxMsgId(PeerId peerId) const;
	// True when any stored item row for the peer already has this hash,
	// i.e. the content top was walked before the pause.
	[[nodiscard]] bool chatScanContainsHash(
		PeerId peerId,
		const QByteArray &hash) const;
	// Any stored hash for this media id, from an earlier run; lets the
	// queue resolve a repeated media id without fetching it again.
	[[nodiscard]] QByteArray chatScanHashForMedia(
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
	// How many item rows already carry a hash; a resumed scan starts its
	// hashed counter exactly there instead of estimating it from the
	// walked message count.
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
		qint64 retryAt);
	// Releases every active lease for the peer in one statement; pause uses
	// this instead of one update per media id.
	void chatScanReleaseLeases(PeerId peerId);
	// Marks every row of each media id hashed and returns how many rows
	// received the hash (duplicate occurrences included).
	[[nodiscard]] int chatScanCommitHashes(
		PeerId peerId,
		const std::vector<std::pair<uint64, QByteArray>> &hashes);
	void chatScanFailItem(PeerId peerId, uint64 mediaId);
	// Puts durably failed rows back into the pending queue. Called when a
	// persisted scan is opened, so a failure from an earlier process is
	// retried instead of remaining state=3 forever.
	[[nodiscard]] int chatScanRetryFailed(PeerId peerId);
	[[nodiscard]] int chatScanLeasedCount(PeerId peerId) const;
	[[nodiscard]] int chatScanFailedCount(PeerId peerId) const;
	// Incrementally maintained per-category snapshot (deltas applied at
	// flush time); open reads this instead of recomputing anything.
	[[nodiscard]] std::vector<ChatScanProgressRow> readChatScanProgress(
		PeerId peerId) const;
	void updateChatScanProgress(
		PeerId peerId,
		int category,
		int64 itemCount,
		int64 uniqueCount,
		int64 uniqueBytes,
		int64 dupCount,
		int64 dupBytes);
	// Rebuilds and persists the snapshot from the item and dup tables
	// with two keyed GROUP BYs; no row joins.
	[[nodiscard]] std::vector<ChatScanProgressRow> recomputeChatScanProgress(
		PeerId peerId);
	// One representative item per distinct media id with a resolved hash,
	// in walk order; backs finalize-time registry registration for the
	// portion of the scan that was never held in memory.
	[[nodiscard]] std::vector<ChatScanItem> chatScanDistinctItems(
		PeerId peerId) const;
	// Small install-global text rows shared by the dedup features; used to
	// persist the flood-wait register across app restarts.
	void writeGlobal(const QString &key, const QString &value);
	[[nodiscard]] QString readGlobal(const QString &key) const;
	void removeChatScanDup(PeerId peerId, MsgId msgId);
	void clearChatScan(PeerId peerId);
	void writeScanCursor(const ChatScanState &state);
	[[nodiscard]] std::optional<ChatScanState> readScanCursor(
		PeerId peerId) const;
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
		uint64 mediaId,
		const QByteArray &hash);
	[[nodiscard]] std::vector<PeerId> scannedPeerIds() const;
	[[nodiscard]] std::optional<ChatScanDup> takeChatScanDup(
		PeerId peerId,
		MsgId msgId);
	void removeChatScanItem(PeerId peerId, MsgId msgId);
	[[nodiscard]] bool hasChatScanItem(PeerId peerId, MsgId msgId) const;
	[[nodiscard]] std::optional<ChatScanItem> chatScanItem(
		PeerId peerId,
		MsgId msgId) const;
	void clearChatScanDups(PeerId peerId);

	void beginTransaction();
	void commitTransaction();

private:
	class Impl;
	std::unique_ptr<Impl> _impl;
};

} // namespace Data