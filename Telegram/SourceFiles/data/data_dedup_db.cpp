/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_dedup_db.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QSet>
#include <QStringList>
#include <QVariant>
#include <QUuid>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "logs.h"
#include <crl/crl.h>

#include <algorithm>

namespace Data {

namespace {

[[nodiscard]] QString TableName(DedupDb::Table table) {
	switch (table) {
	case DedupDb::Table::Downloads: return u"dedup_dl"_q;
	case DedupDb::Table::Uploads: return u"dedup_ul"_q;
	}
	Unexpected("DedupDb::Table");
}

struct TableState {
	QHash<uint64, QByteArray> docToHash;
	QHash<QByteArray, QSet<uint64>> hashToDoc;
	QSet<uint64> unfinishedDocs;
	bool loaded = false;
};

// Single process-wide memory copy of the downloads/uploads tables per
// database file. Downloads run on the main thread, exports on the export
// thread, and a SQLite connection cannot cross threads, so each side opens
// its own connection but they share these maps under a mutex. Reads stay
// O(1) memory lookups, writes update memory under lock and then the
// caller's own connection. Whoever touches a table first loads it once.
struct SharedDownloads {
	std::mutex mutex;
	TableState state[2];
};

[[nodiscard]] std::shared_ptr<SharedDownloads> SharedDownloadsForPath(
		const QString &path) {
	static std::mutex mutex;
	static std::map<QString, std::weak_ptr<SharedDownloads>> stores;
	std::lock_guard<std::mutex> lock(mutex);
	if (const auto i = stores.find(path); i != stores.end()) {
		if (auto alive = i->second.lock()) {
			return alive;
		}
	}
	auto fresh = std::make_shared<SharedDownloads>();
	stores[path] = fresh;
	return fresh;
}

// Temp export rows are write-buffered: checks read memory, so disk writes
// can wait until 100 pile up and go out in one transaction. A crash loses
// at most unflushed temp rows, which the next startup purges anyway.
constexpr auto kExWriteBatchLimit = 100;

[[nodiscard]] QByteArray EncodePeerIds(const std::vector<PeerId> &ids) {
	auto result = QByteArray();
	result.reserve(int(ids.size()) * 8);
	for (const auto peerId : ids) {
		const auto value = quint64(peerId.value);
		char buffer[8] = {};
		memcpy(buffer, &value, 8);
		result.append(buffer, 8);
	}
	return result;
}

} // namespace

class DedupDb::Impl {
public:
	explicit Impl(const QString &path, bool purgePending);
	~Impl();

	[[nodiscard]] bool isOpen() const;
	void insert(Table table, const DedupRecord &record);
	void removeByDocumentId(
		Table table,
		uint64 documentId,
		const QString &status);
	[[nodiscard]] bool containsDocId(
		Table table,
		uint64 documentId) const;
	[[nodiscard]] bool containsDocIdInDb(
		Table table,
		uint64 documentId) const;
	[[nodiscard]] bool containsHash(
		Table table,
		const QByteArray &hash) const;
	[[nodiscard]] bool containsFinishedHash(
		Table table,
		const QByteArray &hash) const;
	void rekey(Table table, uint64 oldDocumentId, uint64 newDocumentId);
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
		uint64 excludeDocumentId) const;
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
	void removeEfResumeItem(const QString &jobId, int itemIndex);
	void removeEfResumeBySource(
		uint64 sessionId,
		PeerId destPeerId,
		MsgId sourceMsgId);
	void clearEfResumeForPeer(PeerId peerId);
	void clearEfResumeJob(const QString &jobId);
	void setEfResumePaused(uint64 sessionId, PeerId peerId, bool paused);
	[[nodiscard]] std::vector<EfResumeItem> loadEfResumeItemsForPeer(
		uint64 sessionId,
		PeerId peerId) const;
	[[nodiscard]] std::vector<EfResumeItem> loadUnfinishedEfResumeItems(
		uint64 sessionId) const;

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
	void rebaseExResumeFolder(
		uint64 sessionId,
		int mode,
		const QString &oldPrefix,
		const QString &newPrefix);

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
	void flushExPending();

	void writeChatScanRow(const ChatScanRow &row);
	[[nodiscard]] std::vector<ChatScanRow> readChatScan(
		PeerId peerId) const;
	[[nodiscard]] bool isChatScanned(PeerId peerId) const;
	void markChatDeleteFinished(PeerId peerId);
	void writeChatScanDup(const ChatScanDup &dup);
	[[nodiscard]] std::vector<ChatScanDup> readChatScanDups(
		PeerId peerId,
		int category,
		int limit,
		int offset) const;
	[[nodiscard]] std::vector<ChatScanDup> readChatScanDupsBefore(
		PeerId peerId,
		int category,
		int limit,
		MsgId before) const;
	[[nodiscard]] int chatScanDupCount(PeerId peerId, int category) const;
	[[nodiscard]] int64 chatScanDupBytes(PeerId peerId) const;
	[[nodiscard]] std::vector<ChatScanCategoryTotals> chatScanCategoryTotals(
		PeerId peerId) const;
	[[nodiscard]] qint64 chatScanItemCount(PeerId peerId) const;
	[[nodiscard]] MsgId chatScanMaxMsgId(PeerId peerId) const;
	[[nodiscard]] bool chatScanContainsHash(
		PeerId peerId,
		const QByteArray &hash) const;
	[[nodiscard]] std::vector<ChatScanItem> chatScanUnresolvedItems(
		PeerId peerId) const;
	[[nodiscard]] std::vector<ChatScanItem> chatScanPendingItemsWindow(
		PeerId peerId,
		MsgId above,
		int limit) const;
	[[nodiscard]] std::vector<ChatScanItem> chatScanItemsWindow(
		PeerId peerId,
		MsgId above,
		int limit) const;
	[[nodiscard]] std::vector<ChatScanItem> chatScanHashedItemsWindow(
		PeerId peerId,
		MsgId above,
		int limit) const;
	[[nodiscard]] int chatScanPendingCount(PeerId peerId) const;
	[[nodiscard]] int chatScanHashedCount(PeerId peerId) const;
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
	void chatScanReleaseLeases(PeerId peerId);
	[[nodiscard]] int chatScanCommitHashes(
		PeerId peerId,
		const std::vector<std::pair<uint64, QByteArray>> &hashes);
	void chatScanFailItem(PeerId peerId, uint64 mediaId);
	[[nodiscard]] int chatScanRetryFailed(PeerId peerId);
	[[nodiscard]] int chatScanLeasedCount(PeerId peerId) const;
	[[nodiscard]] int chatScanFailedCount(PeerId peerId) const;
	[[nodiscard]] QByteArray chatScanHashForMedia(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto) const;
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
	[[nodiscard]] std::vector<ChatScanProgressRow> recomputeChatScanProgress(
		PeerId peerId);
	[[nodiscard]] std::vector<ChatScanItem> chatScanDistinctItems(
		PeerId peerId) const;
	void writeGlobal(const QString &key, const QString &value);
	[[nodiscard]] QString readGlobal(const QString &key) const;
	void removeChatScanDup(PeerId peerId, MsgId msgId);
	void clearChatScan(PeerId peerId);
	void writeScanCursor(const ChatScanState &state);
	[[nodiscard]] std::optional<ChatScanState> readScanCursor(
		PeerId peerId) const;
	void writeChatScanItems(const std::vector<ChatScanItem> &items);
	[[nodiscard]] std::vector<ChatScanItem> readChatScanItems(
		PeerId peerId) const;
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

	enum class TmpTable {
		Dl,
		Ul,
		Fw,
	};
	[[nodiscard]] static QString TmpTableName(TmpTable table);

	void insertTmp(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		uint64 documentId,
		const QByteArray &hash);
	[[nodiscard]] bool containsTmpDocId(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		uint64 documentId) const;
	[[nodiscard]] bool containsTmpHash(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		const QByteArray &hash) const;
	[[nodiscard]] QByteArray hashForTmpDocId(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		uint64 documentId) const;
	void removeTmpDoc(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		uint64 documentId);
	void clearTmpRun(TmpTable table, uint64 sessionId, uint64 scopeId);
	void flushTmpPending();

	void beginTransaction();
	void commitTransaction();

private:
	struct ExTmpKey {
		uint64 sessionId = 0;
		uint64 peerId = 0;
		inline bool operator<(const ExTmpKey &other) const {
			return std::tie(sessionId, peerId)
				< std::tie(other.sessionId, other.peerId);
		}
	};

	struct ExTmpState {
		QHash<uint64, QByteArray> docToHash;
		QHash<QByteArray, QSet<uint64>> hashToDoc;
		struct Pending {
			uint64 docId = 0;
			QByteArray hash;
		};
		std::vector<Pending> pending;
		bool loaded = false;
	};

	void ensureLoaded(Table table) const;
	[[nodiscard]] ExTmpState &exTmpState(uint64 sessionId, PeerId peerId);
	[[nodiscard]] const ExTmpState &exTmpState(
		uint64 sessionId,
		PeerId peerId) const;
	void ensureExTmpLoaded(uint64 sessionId, PeerId peerId) const;

	bool createTables();

	QString _connectionName;
	QSqlDatabase _db;
	bool _open = false;
	std::shared_ptr<SharedDownloads> _shared;
	mutable std::map<ExTmpKey, ExTmpState> _exTmpStates;
	int _exPendingWrites = 0;

	struct TmpKey {
		TmpTable table = TmpTable::Dl;
		uint64 sessionId = 0;
		uint64 scopeId = 0;
		inline bool operator<(const TmpKey &other) const {
			return std::tie(table, sessionId, scopeId)
				< std::tie(other.table, other.sessionId, other.scopeId);
		}
	};

	struct TmpState {
		QHash<uint64, QByteArray> docToHash;
		QHash<QByteArray, QSet<uint64>> hashToDoc;
		struct Pending {
			uint64 docId = 0;
			QByteArray hash;
		};
		std::vector<Pending> pending;
		bool loaded = false;
	};

	[[nodiscard]] TmpState &tmpState(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId);
	[[nodiscard]] const TmpState &tmpState(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId) const;
	void ensureTmpLoaded(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId) const;

	mutable std::map<TmpKey, TmpState> _tmpStates;
	int _tmpPendingWrites = 0;
};

DedupDb::Impl::Impl(const QString &path, bool purgePending)
: _connectionName(QUuid::createUuid().toString())
, _shared(SharedDownloadsForPath(path)) {
	_db = QSqlDatabase::addDatabase(u"QSQLITE"_q, _connectionName);
	_db.setDatabaseName(path);
	_open = _db.open();
	if (!_open) {
		LOG(("DedupDb: Failed to open %1: %2").arg(
			path,
			_db.lastError().text()));
		return;
	}
	QSqlQuery pragma(_db);
	pragma.exec(u"PRAGMA journal_mode=WAL"_q);
	pragma.exec(u"PRAGMA synchronous=NORMAL"_q);
	pragma.exec(u"PRAGMA busy_timeout=5000"_q);
	_open = createTables();
	if (_open && purgePending) {
		// 'u' rows can only survive a crash; at startup nothing is in flight.
		// Second connections must not purge another thread's rows.
		QSqlQuery cleanup(_db);
		if (!cleanup.exec(u"DELETE FROM dedup_dl WHERE status = 'u'"_q)
			|| !cleanup.exec(u"DELETE FROM dedup_ul WHERE status = 'u'"_q)
		|| !cleanup.exec(u"DELETE FROM dl_tmp"_q)
		|| !cleanup.exec(u"DELETE FROM ul_tmp"_q)
		|| !cleanup.exec(u"DELETE FROM ex_tmp WHERE NOT EXISTS ("
				"SELECT 1 FROM ex_resume "
				"WHERE ex_resume.session_id = ex_tmp.session_id "
				"AND ex_resume.peer_id = ex_tmp.peer_id)"_q)) {
			LOG(("DedupDb: Failed to purge stale pending rows: %1").arg(
				cleanup.lastError().text()));
		}
		// The purge deleted rows straight in the database, so a memory
		// copy filled before it would be stale. Drop it, the next check
		// reloads from disk.
		std::lock_guard<std::mutex> lock(_shared->mutex);
		_shared->state[0] = TableState();
		_shared->state[1] = TableState();
	}
}

DedupDb::Impl::~Impl() {
	if (_db.isOpen()) {
		_db.close();
	}
	QSqlDatabase::removeDatabase(_connectionName);
}

DedupDb::Impl::ExTmpState &DedupDb::Impl::exTmpState(
		uint64 sessionId,
		PeerId peerId) {
	ensureExTmpLoaded(sessionId, peerId);
	return _exTmpStates[{ sessionId, peerId.value }];
}

const DedupDb::Impl::ExTmpState &DedupDb::Impl::exTmpState(
		uint64 sessionId,
		PeerId peerId) const {
	ensureExTmpLoaded(sessionId, peerId);
	return _exTmpStates[{ sessionId, peerId.value }];
}

void DedupDb::Impl::ensureExTmpLoaded(
		uint64 sessionId,
		PeerId peerId) const {
	auto &s = _exTmpStates[{ sessionId, peerId.value }];
	if (s.loaded || !_open) {
		return;
	}
	s.loaded = true;
	QSqlQuery q(_db);
	q.prepare(u"SELECT doc_id, hash FROM ex_tmp "
		"WHERE session_id = :session_id AND peer_id = :peer_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: LoadExTmp failed: %1").arg(q.lastError().text()));
		return;
	}
	while (q.next()) {
		const auto docId = q.value(0).toULongLong();
		const auto hash = q.value(1).toByteArray();
		if (!docId || hash.isEmpty()) {
			continue;
		}
		s.docToHash[docId] = hash;
		s.hashToDoc[hash].insert(docId);
	}
}

void DedupDb::Impl::ensureLoaded(Table table) const {
	std::lock_guard<std::mutex> lock(_shared->mutex);
	auto &s = _shared->state[int(table)];
	if (s.loaded || !_open) {
		return;
	}
	s.loaded = true;
	QSqlQuery q(_db);
	if (!q.exec(u"SELECT hash, doc_id, status FROM " + TableName(table))) {
		LOG(("DedupDb: Load failed: %1").arg(q.lastError().text()));
		return;
	}
	while (q.next()) {
		const auto hash = q.value(0).toByteArray();
		const auto docId = q.value(1).toULongLong();
		if (hash.isEmpty() || !docId) {
			continue;
		}
		s.docToHash[docId] = hash;
		s.hashToDoc[hash].insert(docId);
		if (q.value(2).toString() == u"u") {
			s.unfinishedDocs.insert(docId);
		}
	}
}

bool DedupDb::Impl::isOpen() const {
	return _open;
}

bool DedupDb::Impl::createTables() {
	const auto exec = [&](const QString &sql) {
		QSqlQuery q(_db);
		if (!q.exec(sql)) {
			LOG(("DedupDb: Failed to exec: %1: %2").arg(
				sql,
				q.lastError().text()));
			return false;
		}
		return true;
	};
	// Compact single-row-per-content layout: one row per doc/media id,
	// identical content with a different id is an extra row with the same
	// hash. No size column: size is never used as a dedup criterion.
	const auto created = exec(u"CREATE TABLE IF NOT EXISTS dedup_dl ("
		"doc_id INTEGER PRIMARY KEY, "
		"hash BLOB NOT NULL, "
		"status TEXT NOT NULL DEFAULT 'f')"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS dedup_ul ("
			"doc_id INTEGER PRIMARY KEY, "
			"hash BLOB NOT NULL, "
			"status TEXT NOT NULL DEFAULT 'f')"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_dedup_dl_hash "
			"ON dedup_dl(hash)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_dedup_ul_hash "
			"ON dedup_ul(hash)"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS dl_resume ("
			"session_id INTEGER NOT NULL DEFAULT 0, "
			"peer_id INTEGER NOT NULL, "
			"msg_id INTEGER NOT NULL, "
			"path TEXT NOT NULL, "
			"file_size INTEGER NOT NULL DEFAULT 0, "
			"doc_id INTEGER NOT NULL DEFAULT 0, "
			"PRIMARY KEY (peer_id, msg_id))"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS ul_resume ("
			"session_id INTEGER NOT NULL DEFAULT 0, "
			"peer_id INTEGER NOT NULL, "
			"path TEXT NOT NULL, "
			"parts_sent INTEGER NOT NULL DEFAULT 0, "
			"sent_size INTEGER NOT NULL DEFAULT 0, "
			"file_id INTEGER NOT NULL DEFAULT 0, "
			"topic_root_id INTEGER NOT NULL DEFAULT 0, "
			"PRIMARY KEY (session_id, peer_id, path))"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS ef_resume ("
			"session_id INTEGER NOT NULL DEFAULT 0, "
			"job_id TEXT NOT NULL, "
			"item_index INTEGER NOT NULL, "
			"dest_peer_id INTEGER NOT NULL, "
			"source_peer_id INTEGER NOT NULL, "
			"source_msg_id INTEGER NOT NULL, "
			"state TEXT NOT NULL, "
			"local_path TEXT NOT NULL DEFAULT '', "
			"file_id INTEGER NOT NULL DEFAULT 0, "
			"uploaded_parts INTEGER NOT NULL DEFAULT 0, "
			"file_hash BLOB, "
			"media_id INTEGER NOT NULL DEFAULT 0, "
			"file_size INTEGER NOT NULL DEFAULT 0, "
			"paused INTEGER NOT NULL DEFAULT 0, "
			"PRIMARY KEY (job_id, item_index))"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_ef_resume_peer "
			"ON ef_resume(dest_peer_id, state)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_ef_resume_hash "
			"ON ef_resume(file_hash)"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS nf_resume ("
			"session_id INTEGER NOT NULL DEFAULT 0, "
			"dest_peer_id INTEGER NOT NULL, "
			"src_peer_id INTEGER NOT NULL, "
			"total INTEGER NOT NULL DEFAULT 0, "
			"done INTEGER NOT NULL DEFAULT 0, "
			"skipped INTEGER NOT NULL DEFAULT 0, "
			"last_msg_id INTEGER NOT NULL DEFAULT 0, "
			"state TEXT NOT NULL DEFAULT 'running', "
			"forward_options INTEGER NOT NULL DEFAULT 0, "
			"group_options INTEGER NOT NULL DEFAULT 0, "
			"remaining BLOB NOT NULL DEFAULT x'', "
			"PRIMARY KEY (session_id, dest_peer_id))"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS ex_resume ("
			"session_id INTEGER NOT NULL DEFAULT 0, "
			"peer_id INTEGER NOT NULL, "
			"mode INTEGER NOT NULL DEFAULT 0, "
			"last_id INTEGER NOT NULL DEFAULT 0, "
			"total INTEGER NOT NULL DEFAULT 0, "
			"msgs_done INTEGER NOT NULL DEFAULT 0, "
			"skipped INTEGER NOT NULL DEFAULT 0, "
			"export_folder TEXT NOT NULL DEFAULT '', "
			"state TEXT NOT NULL DEFAULT 'running', "
			"media INTEGER NOT NULL DEFAULT 0, "
			"size INTEGER NOT NULL DEFAULT 0, "
			"export_format INTEGER NOT NULL DEFAULT 0, "
			"from_date INTEGER NOT NULL DEFAULT 0, "
			"till_date INTEGER NOT NULL DEFAULT 0, "
			"html_index INTEGER NOT NULL DEFAULT 0, "
			"replied_index BLOB NOT NULL DEFAULT x'', "
			"date_index INTEGER NOT NULL DEFAULT 0, "
			"stats BLOB NOT NULL DEFAULT x'', "
			"doc_id INTEGER NOT NULL DEFAULT 0, "
			"paused_file TEXT NOT NULL DEFAULT '', "
			"paused_bytes INTEGER NOT NULL DEFAULT 0, "
			"last_msg BLOB NOT NULL DEFAULT x'', "
			"split_index INTEGER NOT NULL DEFAULT 0, "
			"selected_done INTEGER NOT NULL DEFAULT 0, "
			"filter_index INTEGER NOT NULL DEFAULT 0, "
			"use_id_range INTEGER NOT NULL DEFAULT 0, "
			"from_id INTEGER NOT NULL DEFAULT 0, "
			"till_id INTEGER NOT NULL DEFAULT 0, "
			"update_anchor INTEGER NOT NULL DEFAULT 0, "
			"covered_till INTEGER NOT NULL DEFAULT 0, "
			"covered_till_date INTEGER NOT NULL DEFAULT 0, "
			"migrated INTEGER NOT NULL DEFAULT 0, "
			"failed_count INTEGER NOT NULL DEFAULT 0, "
			"resume_peer_id INTEGER NOT NULL DEFAULT 0, "
			"dialog_ids BLOB NOT NULL DEFAULT x'', "
			"chat_types INTEGER NOT NULL DEFAULT 0, "
			"full_chats INTEGER NOT NULL DEFAULT 0, "
			"ext_filter_mode INTEGER NOT NULL DEFAULT 0, "
			"ext_filter TEXT NOT NULL DEFAULT '', "
			"PRIMARY KEY (session_id, peer_id, mode, export_folder))"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS ex_tmp ("
			"session_id INTEGER NOT NULL DEFAULT 0, "
			"peer_id INTEGER NOT NULL, "
			"doc_id INTEGER NOT NULL, "
			"hash BLOB NOT NULL, "
			"status TEXT NOT NULL DEFAULT 'f', "
			"PRIMARY KEY (session_id, peer_id, doc_id))"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_ex_tmp_hash "
			"ON ex_tmp(session_id, peer_id, hash)"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS dl_tmp ("
			"session_id INTEGER NOT NULL DEFAULT 0, "
			"scope_id INTEGER NOT NULL, "
			"doc_id INTEGER NOT NULL, "
			"hash BLOB NOT NULL, "
			"status TEXT NOT NULL DEFAULT 'f', "
			"PRIMARY KEY (session_id, scope_id, doc_id))"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_dl_tmp_hash "
			"ON dl_tmp(session_id, scope_id, hash)"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS ul_tmp ("
			"session_id INTEGER NOT NULL DEFAULT 0, "
			"scope_id INTEGER NOT NULL, "
			"doc_id INTEGER NOT NULL, "
			"hash BLOB NOT NULL, "
			"status TEXT NOT NULL DEFAULT 'f', "
			"PRIMARY KEY (session_id, scope_id, doc_id))"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_ul_tmp_hash "
			"ON ul_tmp(session_id, scope_id, hash)"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS fw_tmp ("
			"session_id INTEGER NOT NULL DEFAULT 0, "
			"scope_id INTEGER NOT NULL, "
			"doc_id INTEGER NOT NULL, "
			"hash BLOB NOT NULL, "
			"status TEXT NOT NULL DEFAULT 'f', "
			"PRIMARY KEY (session_id, scope_id, doc_id))"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_fw_tmp_hash "
			"ON fw_tmp(session_id, scope_id, hash)"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS chat_scan ("
			"peer_id INTEGER NOT NULL, "
			"category INTEGER NOT NULL, "
			"unique_count INTEGER NOT NULL DEFAULT 0, "
			"unique_bytes INTEGER NOT NULL DEFAULT 0, "
			"dup_count INTEGER NOT NULL DEFAULT 0, "
			"dup_bytes INTEGER NOT NULL DEFAULT 0, "
			"scanned_upto INTEGER NOT NULL DEFAULT 0, "
			"scan_finished INTEGER NOT NULL DEFAULT 0, "
			"delete_finished INTEGER NOT NULL DEFAULT 0, "
			"PRIMARY KEY (peer_id, category))"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS chat_scan_dups ("
			"peer_id INTEGER NOT NULL, "
			"msg_id INTEGER NOT NULL, "
			"category INTEGER NOT NULL DEFAULT 0, "
			"size INTEGER NOT NULL DEFAULT 0, "
			"flags INTEGER NOT NULL DEFAULT 0, "
			"filename TEXT NOT NULL DEFAULT '', "
			"PRIMARY KEY (peer_id, msg_id))"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_chat_scan_dups_cat "
			"ON chat_scan_dups(peer_id, category)"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS chat_scan_state ("
			"peer_id INTEGER NOT NULL, "
			"cursor_msg_id INTEGER NOT NULL DEFAULT 0, "
			"phase INTEGER NOT NULL DEFAULT 0, "
			"PRIMARY KEY (peer_id))"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS chat_scan_items ("
			"peer_id INTEGER NOT NULL, "
			"msg_id INTEGER NOT NULL, "
			"category INTEGER NOT NULL DEFAULT 0, "
			"size INTEGER NOT NULL DEFAULT 0, "
			"media_id INTEGER NOT NULL DEFAULT 0, "
			"is_photo INTEGER NOT NULL DEFAULT 0, "
			"hash BLOB, "
			"filename TEXT NOT NULL DEFAULT '', "
			"state INTEGER NOT NULL DEFAULT 0, "
			"attempts INTEGER NOT NULL DEFAULT 0, "
			"lease_until INTEGER NOT NULL DEFAULT 0, "
			"PRIMARY KEY (peer_id, msg_id))"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_chat_scan_items_media "
			"ON chat_scan_items(peer_id, media_id)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_chat_scan_items_hash "
			"ON chat_scan_items(peer_id, hash)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_chat_scan_items_state "
			"ON chat_scan_items(peer_id, state, media_id)"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS chat_scan_progress ("
			"peer_id INTEGER NOT NULL, "
			"category INTEGER NOT NULL, "
			"item_count INTEGER NOT NULL DEFAULT 0, "
			"unique_count INTEGER NOT NULL DEFAULT 0, "
			"unique_bytes INTEGER NOT NULL DEFAULT 0, "
			"dup_count INTEGER NOT NULL DEFAULT 0, "
			"dup_bytes INTEGER NOT NULL DEFAULT 0, "
			"PRIMARY KEY (peer_id, category))"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS client_globals ("
			"key TEXT PRIMARY KEY, "
			"value TEXT NOT NULL DEFAULT '')"_q);
	return created;
}

void DedupDb::Impl::insert(Table table, const DedupRecord &record) {
	ensureLoaded(table);
	if (record.documentId && !record.hash.isEmpty()) {
		std::lock_guard<std::mutex> lock(_shared->mutex);
		auto &s = _shared->state[int(table)];
		s.docToHash[record.documentId] = record.hash;
		s.hashToDoc[record.hash].insert(record.documentId);
		if (record.status == u"u") {
			s.unfinishedDocs.insert(record.documentId);
		} else {
			s.unfinishedDocs.remove(record.documentId);
		}
	}
	if (!_open) {
		return;
	}
	if (record.hash.isEmpty()) {
		// An empty hash (in-flight 'u' row) can't be used for dedup and the
		// column is NOT NULL, so skip the DB write. The in-memory state above
		// still tracks the doc id for containsDocId().
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO " + TableName(table)
		+ u" (doc_id, hash, status) "
		"VALUES (:doc_id, :hash, :status)"_q);
	q.bindValue(u":doc_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.documentId)));
	q.bindValue(u":hash"_q, record.hash);
	q.bindValue(u":status"_q, record.status);
	if (!q.exec()) {
		LOG(("DedupDb: Insert failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::removeByDocumentId(
		Table table,
		uint64 documentId,
		const QString &status) {
	ensureLoaded(table);
	{
		std::lock_guard<std::mutex> lock(_shared->mutex);
		auto &s = _shared->state[int(table)];
		const auto it = s.docToHash.constFind(documentId);
		if (it != s.docToHash.constEnd()) {
			const auto hash = it.value();
			if (status.isEmpty() || s.unfinishedDocs.contains(documentId)) {
				s.docToHash.remove(documentId);
				s.unfinishedDocs.remove(documentId);
				const auto hashes = s.hashToDoc.find(hash);
				if (hashes != s.hashToDoc.end()) {
					hashes.value().remove(documentId);
					if (hashes.value().isEmpty()) {
						s.hashToDoc.erase(hashes);
					}
				}
			}
		}
	}
	if (!_open || !documentId) {
		return;
	}
	QSqlQuery q(_db);
	if (status.isEmpty()) {
		q.prepare(u"DELETE FROM " + TableName(table)
			+ u" WHERE doc_id = :doc_id"_q);
	} else {
		q.prepare(u"DELETE FROM " + TableName(table)
			+ u" WHERE doc_id = :doc_id AND status = :status"_q);
		q.bindValue(u":status"_q, status);
	}
	q.bindValue(u":doc_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(documentId)));
	if (!q.exec()) {
		LOG(("DedupDb: Remove failed: %1").arg(q.lastError().text()));
	}
}

bool DedupDb::Impl::containsDocId(Table table, uint64 documentId) const {
	ensureLoaded(table);
	std::lock_guard<std::mutex> lock(_shared->mutex);
	return _shared->state[int(table)].docToHash.contains(documentId);
}

bool DedupDb::Impl::containsDocIdInDb(
		Table table,
		uint64 documentId) const {
	if (!_open || !documentId) {
		return false;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT EXISTS(SELECT 1 FROM " + TableName(table)
		+ u" WHERE doc_id = :doc_id)"_q);
	q.bindValue(u":doc_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(documentId)));
	if (!q.exec() || !q.next()) {
		return false;
	}
	return q.value(0).toBool();
}

bool DedupDb::Impl::containsHash(Table table, const QByteArray &hash) const {
	ensureLoaded(table);
	std::lock_guard<std::mutex> lock(_shared->mutex);
	return _shared->state[int(table)].hashToDoc.contains(hash);
}

bool DedupDb::Impl::containsFinishedHash(
		Table table,
		const QByteArray &hash) const {
	if (!_open || hash.isEmpty()) {
		return false;
	}
	ensureLoaded(table);
	std::lock_guard<std::mutex> lock(_shared->mutex);
	const auto &s = _shared->state[int(table)];
	const auto it = s.hashToDoc.find(hash);
	if (it == s.hashToDoc.end()) {
		return false;
	}
	for (const auto docId : it.value()) {
		if (!s.unfinishedDocs.contains(docId)) {
			return true;
		}
	}
	return false;
}

void DedupDb::Impl::rekey(
		Table table,
		uint64 oldDocumentId,
		uint64 newDocumentId) {
	if (!oldDocumentId || !newDocumentId || oldDocumentId == newDocumentId) {
		return;
	}
	ensureLoaded(table);
	{
		std::lock_guard<std::mutex> lock(_shared->mutex);
		auto &s = _shared->state[int(table)];
		const auto hash = s.docToHash.take(oldDocumentId);
		const auto wasUnfinished = s.unfinishedDocs.remove(oldDocumentId);
		if (!hash.isEmpty()) {
			s.docToHash[newDocumentId] = hash;
			if (wasUnfinished) {
				s.unfinishedDocs.insert(newDocumentId);
			}
			const auto hashes = s.hashToDoc.find(hash);
			if (hashes != s.hashToDoc.end()) {
				hashes.value().remove(oldDocumentId);
				hashes.value().insert(newDocumentId);
			}
		}
	}
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE " + TableName(table)
		+ u" SET doc_id = :new_doc_id "
		"WHERE doc_id = :old_doc_id"_q);
	q.bindValue(u":new_doc_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(newDocumentId)));
	q.bindValue(u":old_doc_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(oldDocumentId)));
	if (!q.exec()) {
		LOG(("DedupDb: Rekey failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::updateDedupStatus(
		Table table,
		const QByteArray &hash,
		const QString &status) {
	ensureLoaded(table);
	if (!_open || hash.isEmpty()) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(_shared->mutex);
		auto &s = _shared->state[int(table)];
		const auto hashes = s.hashToDoc.find(hash);
		if (hashes != s.hashToDoc.end()) {
			for (const auto docId : hashes.value()) {
				if (status == u"u") {
					s.unfinishedDocs.insert(docId);
				} else {
					s.unfinishedDocs.remove(docId);
				}
			}
		}
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE " + TableName(table)
		+ u" SET status = :status "
		"WHERE hash = :hash"_q);
	q.bindValue(u":status"_q, status);
	q.bindValue(u":hash"_q, hash);
	if (!q.exec()) {
		LOG(("DedupDb: UpdateDedupStatus failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::removeUnfinishedByHash(
		Table table,
		const QByteArray &hash) {
	ensureLoaded(table);
	{
		std::lock_guard<std::mutex> lock(_shared->mutex);
		auto &s = _shared->state[int(table)];
		const auto hashes = s.hashToDoc.find(hash);
		if (hashes != s.hashToDoc.end()) {
			auto ids = hashes.value();
			for (const auto docId : ids) {
				if (!s.unfinishedDocs.contains(docId)) {
					continue;
				}
				s.docToHash.remove(docId);
				s.unfinishedDocs.remove(docId);
				hashes.value().remove(docId);
			}
			if (hashes.value().isEmpty()) {
				s.hashToDoc.erase(hashes);
			}
		}
	}
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM " + TableName(table)
		+ u" WHERE hash = :hash AND status = 'u'"_q);
	q.bindValue(u":hash"_q, hash);
	if (!q.exec()) {
		LOG(("DedupDb: RemoveUnfinishedByHash failed: %1").arg(
			q.lastError().text()));
	}
}

QByteArray DedupDb::Impl::hashForDocId(
		Table table,
		uint64 documentId) const {
	ensureLoaded(table);
	std::lock_guard<std::mutex> lock(_shared->mutex);
	return _shared->state[int(table)].docToHash.value(documentId);
}

uint64 DedupDb::Impl::seekDocumentId(
		Table table,
		const QByteArray &hash,
		uint64 excludeDocumentId) const {
	ensureLoaded(table);
	std::lock_guard<std::mutex> lock(_shared->mutex);
	const auto &docs = _shared->state[int(table)].hashToDoc;
	const auto it = docs.find(hash);
	if (it == docs.end()) {
		return 0;
	}
	for (const auto docId : it.value()) {
		if (docId != excludeDocumentId) {
			return docId;
		}
	}
	return 0;
}

void DedupDb::Impl::addPending(
		Table table,
		uint64 documentId,
		const QByteArray &hash) {
	insert(table, {
		.hash = hash,
		.documentId = documentId,
		.status = u"u"_q,
	});
}

void DedupDb::Impl::removePending(Table table, uint64 documentId) {
	removeByDocumentId(table, documentId, u"u"_q);
}

std::vector<DedupRecord> DedupDb::Impl::loadAll(Table table) const {
	ensureLoaded(table);
	{
		std::lock_guard<std::mutex> lock(_shared->mutex);
		const auto &s = _shared->state[int(table)];
		if (s.loaded) {
			auto result = std::vector<DedupRecord>();
			result.reserve(s.docToHash.size());
			for (auto i = s.docToHash.constBegin(); i != s.docToHash.constEnd(); ++i) {
				result.push_back({
					.hash = i.value(),
					.documentId = i.key(),
				});
			}
			return result;
		}
	}
	auto result = std::vector<DedupRecord>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	if (!q.exec(u"SELECT hash, doc_id, status FROM " + TableName(table))) {
		LOG(("DedupDb: LoadAll failed: %1").arg(q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto record = DedupRecord();
		record.hash = q.value(0).toByteArray();
		record.documentId = q.value(1).toULongLong();
		record.status = q.value(2).toString();
		result.push_back(std::move(record));
	}
	return result;
}

void DedupDb::Impl::insertDlResume(const DlResumeRecord &record) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO dl_resume "
		"(session_id, peer_id, msg_id, path, file_size, doc_id) "
		"VALUES (:session_id, :peer_id, :msg_id, :path, :file_size, "
		":doc_id)"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.sessionId)));
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.peerId)));
	q.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.msgId)));
	q.bindValue(u":path"_q, record.path);
	q.bindValue(u":file_size"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.fileSize)));
	q.bindValue(u":doc_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.docId)));
	if (!q.exec()) {
		LOG(("DedupDb: InsertDlResume failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::removeDlResume(
		uint64 sessionId,
		uint64 peerId,
		int64 msgId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM dl_resume "
		"WHERE peer_id = :peer_id AND msg_id = :msg_id "
		"AND session_id = :session_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId)));
	q.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(msgId)));
	if (!q.exec()) {
		LOG(("DedupDb: RemoveDlResume failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::clearDlResume() {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	if (!q.exec(u"DELETE FROM dl_resume"_q)) {
		LOG(("DedupDb: ClearDlResume failed: %1").arg(q.lastError().text()));
	}
}

std::vector<DlResumeRecord> DedupDb::Impl::loadAllDlResume() const {
	auto result = std::vector<DlResumeRecord>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	if (!q.exec(u"SELECT session_id, peer_id, msg_id, "
		"path, file_size, doc_id FROM dl_resume"_q)) {
		LOG(("DedupDb: LoadAllDlResume failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto record = DlResumeRecord();
		record.sessionId = q.value(0).toULongLong();
		record.peerId = q.value(1).toULongLong();
		record.msgId = q.value(2).toLongLong();
		record.path = q.value(3).toString();
		record.fileSize = q.value(4).toLongLong();
		record.docId = q.value(5).toULongLong();
		result.push_back(std::move(record));
	}
	return result;
}

void DedupDb::Impl::insertUlResume(const UlResumeRecord &record) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO ul_resume "
		"(session_id, peer_id, path, parts_sent, sent_size, file_id, "
		"topic_root_id) "
		"VALUES (:session_id, :peer_id, :path, :parts_sent, :sent_size, "
		":file_id, :topic_root_id)"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.sessionId)));
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.peerId)));
	q.bindValue(u":path"_q, record.path);
	q.bindValue(u":parts_sent"_q, record.partsSent);
	q.bindValue(u":sent_size"_q, QVariant::fromValue(record.sentSize));
	q.bindValue(u":file_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.fileId)));
	q.bindValue(u":topic_root_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.topicRootId)));
	if (!q.exec()) {
		LOG(("DedupDb: InsertUlResume failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::removeUlResume(
		uint64 sessionId,
		uint64 peerId,
		const QString &path) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM ul_resume "
		"WHERE peer_id = :peer_id AND path = :path "
		"AND session_id = :session_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId)));
	q.bindValue(u":path"_q, path);
	if (!q.exec()) {
		LOG(("DedupDb: RemoveUlResume failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::clearUlResume(uint64 sessionId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM ul_resume WHERE session_id = :session_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: ClearUlResume failed: %1").arg(q.lastError().text()));
	}
}

std::vector<UlResumeRecord> DedupDb::Impl::loadAllUlResume(
		uint64 sessionId) const {
	auto result = std::vector<UlResumeRecord>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT session_id, peer_id, path, parts_sent, sent_size, "
		"file_id, topic_root_id FROM ul_resume "
		"WHERE session_id = :session_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: LoadAllUlResume failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto record = UlResumeRecord();
		record.sessionId = q.value(0).toULongLong();
		record.peerId = q.value(1).toULongLong();
		record.path = q.value(2).toString();
		record.partsSent = q.value(3).toInt();
		record.sentSize = q.value(4).toLongLong();
		record.fileId = static_cast<uint64>(q.value(5).toLongLong());
		record.topicRootId = q.value(6).toLongLong();
		result.push_back(std::move(record));
	}
	return result;
}

void DedupDb::Impl::insertEfResumeItem(const EfResumeItem &item) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO ef_resume "
		"(session_id, job_id, item_index, dest_peer_id, source_peer_id, "
		"source_msg_id, state, local_path, file_id, uploaded_parts, "
		"file_hash, media_id, file_size, paused) "
		"VALUES (:session_id, :job_id, :item_index, :dest_peer_id, :source_peer_id, "
		":source_msg_id, :state, :local_path, :file_id, :uploaded_parts, "
		":file_hash, :media_id, :file_size, :paused)"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(item.sessionId)));
	q.bindValue(u":job_id"_q, item.jobId);
	q.bindValue(u":item_index"_q, item.itemIndex);
	q.bindValue(u":dest_peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(item.peerId.value)));
	q.bindValue(u":source_peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(item.sourceId.peer.value)));
	q.bindValue(u":source_msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(item.sourceId.msg.bare)));
	q.bindValue(u":state"_q, item.state);
	q.bindValue(u":local_path"_q, item.localPath);
	q.bindValue(u":file_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(item.fileId)));
	q.bindValue(u":uploaded_parts"_q, item.uploadedParts);
	q.bindValue(u":file_hash"_q, item.fileHash);
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(item.mediaId)));
	q.bindValue(u":file_size"_q, QVariant::fromValue(
		static_cast<qlonglong>(item.fileSize)));
	q.bindValue(u":paused"_q, item.paused ? 1 : 0);
	if (!q.exec()) {
		LOG(("DedupDb: InsertEfResumeItem failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::removeEfResumeItem(
		const QString &jobId,
		int itemIndex) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM ef_resume "
		"WHERE job_id = :job_id AND item_index = :item_index"_q);
	q.bindValue(u":job_id"_q, jobId);
	q.bindValue(u":item_index"_q, itemIndex);
	if (!q.exec()) {
		LOG(("DedupDb: RemoveEfResumeItem failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::removeEfResumeBySource(
		uint64 sessionId,
		PeerId destPeerId,
		MsgId sourceMsgId) {
	if (!_open || !sourceMsgId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM ef_resume "
		"WHERE session_id = :session_id AND dest_peer_id = :dest_peer_id "
		"AND source_msg_id = :source_msg_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":dest_peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(destPeerId.value)));
	q.bindValue(u":source_msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(sourceMsgId.bare)));
	if (!q.exec()) {
		LOG(("DedupDb: RemoveEfResumeBySource failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::clearEfResumeForPeer(PeerId peerId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM ef_resume WHERE dest_peer_id = :dest_peer_id"_q);
	q.bindValue(u":dest_peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ClearEfResumeForPeer failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::clearEfResumeJob(const QString &jobId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM ef_resume WHERE job_id = :job_id"_q);
	q.bindValue(u":job_id"_q, jobId);
	if (!q.exec()) {
		LOG(("DedupDb: ClearEfResumeJob failed: %1").arg(
			q.lastError().text()));
	}
}

std::vector<EfResumeItem> DedupDb::Impl::loadEfResumeItemsForPeer(
		uint64 sessionId,
		PeerId peerId) const {
	auto result = std::vector<EfResumeItem>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT job_id, item_index, source_peer_id, source_msg_id, "
		"state, local_path, file_id, uploaded_parts, file_hash, "
		"media_id, file_size, paused FROM ef_resume "
		"WHERE dest_peer_id = :dest_peer_id AND session_id = :session_id "
		"ORDER BY item_index"_q);
	q.bindValue(u":dest_peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: LoadEfResumeItemsForPeer failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto item = EfResumeItem();
		item.jobId = q.value(0).toString();
		item.itemIndex = q.value(1).toInt();
		item.peerId = peerId;
		item.sourceId = FullMsgId(
			PeerId(q.value(2).toULongLong()),
			MsgId(q.value(3).toLongLong()));
		item.state = q.value(4).toString();
		item.localPath = q.value(5).toString();
		item.fileId = static_cast<uint64>(q.value(6).toLongLong());
		item.uploadedParts = q.value(7).toInt();
		item.fileHash = q.value(8).toByteArray();
		item.mediaId = static_cast<uint64>(q.value(9).toLongLong());
		item.fileSize = q.value(10).toLongLong();
		item.paused = (q.value(11).toInt() != 0);
		result.push_back(std::move(item));
	}
	return result;
}

std::vector<EfResumeItem> DedupDb::Impl::loadUnfinishedEfResumeItems(
		uint64 sessionId) const {
	auto result = std::vector<EfResumeItem>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT job_id, item_index, dest_peer_id, source_peer_id, "
		"source_msg_id, state, local_path, file_id, uploaded_parts, "
		"file_hash, media_id, file_size, paused FROM ef_resume "
		"WHERE state <> 'done' AND session_id = :session_id ORDER BY item_index"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: LoadUnfinishedEfResumeItems failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto item = EfResumeItem();
		item.jobId = q.value(0).toString();
		item.itemIndex = q.value(1).toInt();
		item.peerId = PeerId(q.value(2).toULongLong());
		item.sourceId = FullMsgId(
			PeerId(q.value(3).toULongLong()),
			MsgId(q.value(4).toLongLong()));
		item.state = q.value(5).toString();
		item.localPath = q.value(6).toString();
		item.fileId = static_cast<uint64>(q.value(7).toLongLong());
		item.uploadedParts = q.value(8).toInt();
		item.fileHash = q.value(9).toByteArray();
		item.mediaId = static_cast<uint64>(q.value(10).toLongLong());
		item.fileSize = q.value(11).toLongLong();
		item.paused = (q.value(12).toInt() != 0);
		result.push_back(std::move(item));
	}
	return result;
}

void DedupDb::Impl::setEfResumePaused(
		uint64 sessionId,
		PeerId peerId,
		bool paused) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	if (peerId) {
		q.prepare(u"UPDATE ef_resume SET paused = :paused "
			"WHERE session_id = :session_id AND dest_peer_id = :peer_id"_q);
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
	} else {
		q.prepare(u"UPDATE ef_resume SET paused = :paused "
			"WHERE session_id = :session_id"_q);
	}
	q.bindValue(u":paused"_q, paused ? 1 : 0);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: SetEfResumePaused failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::beginTransaction() {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	if (!q.exec(u"BEGIN IMMEDIATE"_q)) {
		LOG(("DedupDb: Begin transaction failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::commitTransaction() {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	if (!q.exec(u"COMMIT"_q)) {
		LOG(("DedupDb: Commit transaction failed: %1").arg(
			q.lastError().text()));
	}
}

DedupDb::DedupDb(const QString &path, bool purgePending)
: _impl(std::make_unique<Impl>(path, purgePending)) {
}

DedupDb::~DedupDb() = default;

bool DedupDb::isOpen() const {
	return _impl->isOpen();
}

void DedupDb::insert(Table table, const DedupRecord &record) {
	_impl->insert(table, record);
}

void DedupDb::removeByDocumentId(
		Table table,
		uint64 documentId,
		const QString &status) {
	_impl->removeByDocumentId(table, documentId, status);
}

bool DedupDb::containsDocId(Table table, uint64 documentId) const {
	return _impl->containsDocId(table, documentId);
}

bool DedupDb::containsDocIdInDb(Table table, uint64 documentId) const {
	return _impl->containsDocIdInDb(table, documentId);
}

bool DedupDb::containsHash(Table table, const QByteArray &hash) const {
	return _impl->containsHash(table, hash);
}

bool DedupDb::containsFinishedHash(
		Table table,
		const QByteArray &hash) const {
	return _impl->containsFinishedHash(table, hash);
}

void DedupDb::rekey(
		Table table,
		uint64 oldDocumentId,
		uint64 newDocumentId) {
	_impl->rekey(table, oldDocumentId, newDocumentId);
}

void DedupDb::updateDedupStatus(
		Table table,
		const QByteArray &hash,
		const QString &status) {
	_impl->updateDedupStatus(table, hash, status);
}

void DedupDb::removeUnfinishedByHash(
		Table table,
		const QByteArray &hash) {
	_impl->removeUnfinishedByHash(table, hash);
}

QByteArray DedupDb::hashForDocId(
		Table table,
		uint64 documentId) const {
	return _impl->hashForDocId(table, documentId);
}

uint64 DedupDb::seekDocumentId(
		Table table,
		const QByteArray &hash,
		uint64 excludeDocumentId) const {
	return _impl->seekDocumentId(table, hash, excludeDocumentId);
}

void DedupDb::addPending(Table table, uint64 documentId, const QByteArray &hash) {
	_impl->addPending(table, documentId, hash);
}

void DedupDb::removePending(Table table, uint64 documentId) {
	_impl->removePending(table, documentId);
}

std::vector<DedupRecord> DedupDb::loadAll(Table table) const {
	return _impl->loadAll(table);
}

void DedupDb::insertDlResume(const DlResumeRecord &record) {
	_impl->insertDlResume(record);
}

void DedupDb::removeDlResume(
		uint64 sessionId,
		uint64 peerId,
		int64 msgId) {
	_impl->removeDlResume(sessionId, peerId, msgId);
}

void DedupDb::clearDlResume() {
	_impl->clearDlResume();
}

std::vector<DlResumeRecord> DedupDb::loadAllDlResume() const {
	return _impl->loadAllDlResume();
}

void DedupDb::insertUlResume(const UlResumeRecord &record) {
	_impl->insertUlResume(record);
}

void DedupDb::removeUlResume(
		uint64 sessionId,
		uint64 peerId,
		const QString &path) {
	_impl->removeUlResume(sessionId, peerId, path);
}

void DedupDb::clearUlResume(uint64 sessionId) {
	_impl->clearUlResume(sessionId);
}

std::vector<UlResumeRecord> DedupDb::loadAllUlResume(
		uint64 sessionId) const {
	return _impl->loadAllUlResume(sessionId);
}

void DedupDb::Impl::insertNfResume(const NfResumeRecord &record) {
	if (!_open) {
		return;
	}
	auto remaining = QByteArray();
	remaining.reserve(int(record.remaining.size()) * 4);
	for (const auto &msgId : record.remaining) {
		const auto value = qint32(msgId.bare);
		char buffer[4] = {};
		memcpy(buffer, &value, 4);
		remaining.append(buffer, 4);
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO nf_resume "
		"(session_id, dest_peer_id, src_peer_id, total, done, skipped, "
		"last_msg_id, state, forward_options, group_options, remaining) "
		"VALUES (:session_id, :dest_peer_id, :src_peer_id, :total, :done, "
		":skipped, :last_msg_id, :state, :forward_options, :group_options, "
		":remaining)"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.sessionId)));
	q.bindValue(u":dest_peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.destPeerId.value)));
	q.bindValue(u":src_peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.srcPeerId.value)));
	q.bindValue(u":total"_q, record.total);
	q.bindValue(u":done"_q, record.done);
	q.bindValue(u":skipped"_q, record.skipped);
	q.bindValue(u":last_msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.lastMsgId.bare)));
	q.bindValue(u":state"_q, record.state);
	q.bindValue(u":forward_options"_q, record.forwardOptions);
	q.bindValue(u":group_options"_q, record.groupOptions);
	q.bindValue(u":remaining"_q, remaining);
	if (!q.exec()) {
		LOG(("DedupDb: InsertNfResume failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::removeNfResume(uint64 sessionId, PeerId destPeerId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM nf_resume "
		"WHERE session_id = :session_id AND dest_peer_id = :dest_peer_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":dest_peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(destPeerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: RemoveNfResume failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::clearNfResume(uint64 sessionId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM nf_resume WHERE session_id = :session_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: ClearNfResume failed: %1").arg(
			q.lastError().text()));
	}
}

std::vector<NfResumeRecord> DedupDb::Impl::loadNfResume(
		uint64 sessionId) const {
	auto result = std::vector<NfResumeRecord>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT dest_peer_id, src_peer_id, total, done, skipped, "
		"last_msg_id, state, forward_options, group_options, remaining "
		"FROM nf_resume WHERE session_id = :session_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: LoadNfResume failed: %1").arg(q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto record = NfResumeRecord();
		record.sessionId = sessionId;
		record.destPeerId = PeerId(q.value(0).toULongLong());
		record.srcPeerId = PeerId(q.value(1).toULongLong());
		record.total = q.value(2).toInt();
		record.done = q.value(3).toInt();
		record.skipped = q.value(4).toInt();
		record.lastMsgId = MsgId(q.value(5).toLongLong());
		record.state = q.value(6).toString();
		record.forwardOptions = q.value(7).toInt();
		record.groupOptions = q.value(8).toInt();
		const auto raw = q.value(9).toByteArray();
		for (int i = 0; i + 4 <= raw.size(); i += 4) {
			auto value = qint32();
			memcpy(&value, raw.constData() + i, 4);
			record.remaining.push_back(MsgId(value));
		}
		result.push_back(std::move(record));
	}
	return result;
}

void DedupDb::Impl::insertExResume(const ExResumeRecord &record) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO ex_resume "
		"(session_id, peer_id, mode, last_id, total, msgs_done, skipped, "
		"export_folder, state, media, size, export_format, from_date, "
		"till_date, html_index, replied_index, "
		"date_index, stats, "
		"doc_id, paused_file, paused_bytes, last_msg, split_index, "
		"selected_done, filter_index, use_id_range, from_id, till_id, "
		"update_anchor, covered_till, covered_till_date, migrated, "
		"failed_count, resume_peer_id, dialog_ids, chat_types, full_chats, "
		"ext_filter_mode, ext_filter) "
		"VALUES (:session_id, :peer_id, :mode, :last_id, :total, :msgs_done, "
		":skipped, :export_folder, :state, :media, :size, "
		":export_format, :from_date, :till_date, :html_index, "
		":replied_index, "
		":date_index, :stats, :doc_id, :paused_file, "
		":paused_bytes, :last_msg, :split_index, :selected_done, "
		":filter_index, :use_id_range, :from_id, :till_id, "
		":update_anchor, :covered_till, :covered_till_date, :migrated, "
		":failed_count, :resume_peer_id, :dialog_ids, :chat_types, "
		":full_chats, :ext_filter_mode, :ext_filter)"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.sessionId)));
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.peerId.value)));
	q.bindValue(u":mode"_q, record.mode);
	q.bindValue(u":last_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.lastId.bare)));
	q.bindValue(u":total"_q, record.total);
	q.bindValue(u":msgs_done"_q, record.msgsDone);
	q.bindValue(u":skipped"_q, record.skipped);
	q.bindValue(u":export_folder"_q, record.exportFolder);
	q.bindValue(u":state"_q, record.state);
	q.bindValue(u":media"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.media)));
	q.bindValue(u":size"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.size)));
	q.bindValue(u":export_format"_q, record.exportFormat);
	q.bindValue(u":from_date"_q, record.fromDate);
	q.bindValue(u":till_date"_q, record.tillDate);
	q.bindValue(u":html_index"_q, record.htmlIndex);
	q.bindValue(u":replied_index"_q, record.repliedIndex);
	q.bindValue(u":date_index"_q, record.dateIndex);
	q.bindValue(u":stats"_q, record.stats);
	q.bindValue(u":doc_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.docId)));
	q.bindValue(u":paused_file"_q, record.pausedFile);
	q.bindValue(u":paused_bytes"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.pausedBytes)));
	q.bindValue(u":last_msg"_q, record.lastMsg);
	q.bindValue(u":split_index"_q, record.splitIndex);
	q.bindValue(u":selected_done"_q, record.selectedDone);
	q.bindValue(u":filter_index"_q, record.filterIndex);
	q.bindValue(u":use_id_range"_q, record.useIdRange ? 1 : 0);
	q.bindValue(u":from_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.fromId)));
	q.bindValue(u":till_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.tillId)));
	q.bindValue(u":update_anchor"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.updateAnchor.bare)));
	q.bindValue(u":covered_till"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.coveredTill.bare)));
	q.bindValue(u":covered_till_date"_q, record.coveredTillDate);
	q.bindValue(u":migrated"_q, record.migrated ? 1 : 0);
	q.bindValue(u":failed_count"_q, record.failedCount);
	q.bindValue(u":resume_peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.resumePeerId.value)));
	q.bindValue(u":dialog_ids"_q, EncodePeerIds(record.dialogIds));
	q.bindValue(u":chat_types"_q, record.chatTypes);
	q.bindValue(u":full_chats"_q, record.fullChats);
	q.bindValue(u":ext_filter_mode"_q, record.extFilterMode);
	q.bindValue(u":ext_filter"_q, record.extFilter);
	if (!q.exec()) {
		LOG(("DedupDb: InsertExResume failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::removeExResumeFolder(
		uint64 sessionId,
		PeerId peerId,
		int mode,
		const QString &folder) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM ex_resume "
		"WHERE session_id = :session_id AND peer_id = :peer_id "
		"AND mode = :mode AND export_folder = :folder"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":mode"_q, mode);
	q.bindValue(u":folder"_q, folder);
	if (!q.exec()) {
		LOG(("DedupDb: RemoveExResumeFolder failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::clearExResume(uint64 sessionId, int mode) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM ex_resume "
		"WHERE session_id = :session_id AND mode = :mode"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":mode"_q, mode);
	if (!q.exec()) {
		LOG(("DedupDb: ClearExResume failed: %1").arg(
			q.lastError().text()));
	}
}

std::vector<ExResumeRecord> DedupDb::Impl::loadExResume(
		uint64 sessionId,
		int mode) const {
	auto result = std::vector<ExResumeRecord>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT peer_id, mode, last_id, total, msgs_done, skipped, "
		"export_folder, state, media, size, export_format, from_date, "
		"till_date, html_index, replied_index, "
		"date_index, stats, "
		"doc_id, paused_file, paused_bytes, last_msg, split_index, "
		"selected_done, filter_index, use_id_range, from_id, till_id, "
		"update_anchor, covered_till, covered_till_date, migrated, "
		"failed_count, resume_peer_id, dialog_ids, chat_types, full_chats, "
		"ext_filter_mode, ext_filter "
		"FROM ex_resume "
		"WHERE session_id = :session_id AND mode = :mode"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":mode"_q, mode);
	if (!q.exec()) {
		LOG(("DedupDb: LoadExResume failed: %1").arg(q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto record = ExResumeRecord();
		record.sessionId = sessionId;
		record.peerId = PeerId(q.value(0).toULongLong());
		record.mode = q.value(1).toInt();
		record.lastId = MsgId(q.value(2).toLongLong());
		record.total = q.value(3).toInt();
		record.msgsDone = q.value(4).toInt();
		record.skipped = q.value(5).toInt();
		record.exportFolder = q.value(6).toString();
		record.state = q.value(7).toString();
		record.media = uint32(q.value(8).toULongLong());
		record.size = q.value(9).toLongLong();
		record.exportFormat = q.value(10).toInt();
		record.fromDate = q.value(11).toInt();
		record.tillDate = q.value(12).toInt();
		record.htmlIndex = q.value(13).toInt();
		record.repliedIndex = q.value(14).toByteArray();
		record.dateIndex = q.value(15).toInt();
		record.stats = q.value(16).toByteArray();
		record.docId = q.value(17).toULongLong();
		record.pausedFile = q.value(18).toString();
		record.pausedBytes = q.value(19).toLongLong();
		record.lastMsg = q.value(20).toByteArray();
		record.splitIndex = q.value(21).toInt();
		record.selectedDone = q.value(22).toInt();
		record.filterIndex = q.value(23).toInt();
		record.useIdRange = q.value(24).toInt() != 0;
		record.fromId = q.value(25).toULongLong();
		record.tillId = q.value(26).toULongLong();
		record.updateAnchor = MsgId(q.value(27).toLongLong());
		record.coveredTill = MsgId(q.value(28).toLongLong());
		record.coveredTillDate = q.value(29).toInt();
		record.migrated = q.value(30).toInt() != 0;
		record.failedCount = q.value(31).toInt();
		record.resumePeerId = PeerId(q.value(32).toULongLong());
		const auto idsRaw = q.value(33).toByteArray();
		for (auto i = 0; i + 8 <= idsRaw.size(); i += 8) {
			auto value = quint64(0);
			memcpy(&value, idsRaw.constData() + i, 8);
			record.dialogIds.push_back(PeerId(value));
		}
		record.chatTypes = q.value(34).toInt();
		record.fullChats = q.value(35).toInt();
		record.extFilterMode = q.value(36).toInt();
		record.extFilter = q.value(37).toString();
		result.push_back(std::move(record));
	}
	return result;
}

void DedupDb::Impl::rebaseExResumeFolder(
		uint64 sessionId,
		int mode,
		const QString &oldPrefix,
		const QString &newPrefix) {
	if (!_open || oldPrefix.isEmpty() || newPrefix.isEmpty()) {
		return;
	}
	QSqlQuery q(_db);
	// substr() prefix match, not LIKE: folder paths contain '_' wildcards.
	q.prepare(u"UPDATE ex_resume SET export_folder = :new_prefix "
		"|| substr(export_folder, :old_len + 1) "
		"WHERE session_id = :session_id AND mode = :mode "
		"AND substr(export_folder, 1, :old_len) = :old_prefix"_q);
	q.bindValue(u":new_prefix"_q, newPrefix);
	q.bindValue(u":old_len"_q, oldPrefix.size());
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":mode"_q, mode);
	q.bindValue(u":old_prefix"_q, oldPrefix);
	if (!q.exec()) {
		LOG(("DedupDb: RebaseExResumeFolder failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::insertExTmp(
		uint64 sessionId,
		PeerId peerId,
		uint64 documentId,
		const QByteArray &hash) {
	if (!_open || !documentId || hash.isEmpty()) {
		return;
	}
	auto &s = exTmpState(sessionId, peerId);
	s.docToHash[documentId] = hash;
	s.hashToDoc[hash].insert(documentId);
	s.pending.push_back({ documentId, hash });
	if (++_exPendingWrites >= kExWriteBatchLimit) {
		flushExPending();
	}
}

void DedupDb::Impl::flushExPending() {
	if (_exPendingWrites == 0 || !_open) {
		return;
	}
	_exPendingWrites = 0;
	beginTransaction();
	QSqlQuery tmp(_db);
	tmp.prepare(u"INSERT OR REPLACE INTO ex_tmp "
		"(session_id, peer_id, doc_id, hash, status) "
		"VALUES (:session_id, :peer_id, :doc_id, :hash, 'f')"_q);
	for (auto &[key, s] : _exTmpStates) {
		for (const auto &row : s.pending) {
			tmp.bindValue(u":session_id"_q, QVariant::fromValue(
				static_cast<qulonglong>(key.sessionId)));
			tmp.bindValue(u":peer_id"_q, QVariant::fromValue(
				static_cast<qulonglong>(key.peerId)));
			tmp.bindValue(u":doc_id"_q, QVariant::fromValue(
				static_cast<qulonglong>(row.docId)));
			tmp.bindValue(u":hash"_q, row.hash);
			if (!tmp.exec()) {
				LOG(("DedupDb: InsertExTmp failed: %1").arg(
					tmp.lastError().text()));
			}
		}
		s.pending.clear();
	}
	commitTransaction();
}

bool DedupDb::Impl::containsExTmpDocId(
		uint64 sessionId,
		PeerId peerId,
		uint64 documentId) const {
	if (!documentId) {
		return false;
	}
	return exTmpState(sessionId, peerId).docToHash.contains(documentId);
}

bool DedupDb::Impl::containsExTmpHash(
		uint64 sessionId,
		PeerId peerId,
		const QByteArray &hash) const {
	if (hash.isEmpty()) {
		return false;
	}
	const auto &s = exTmpState(sessionId, peerId);
	const auto i = s.hashToDoc.find(hash);
	return (i != s.hashToDoc.end()) && !i->isEmpty();
}

QByteArray DedupDb::Impl::hashForExTmpDocId(
		uint64 sessionId,
		PeerId peerId,
		uint64 documentId) const {
	if (!documentId) {
		return {};
	}
	return exTmpState(sessionId, peerId).docToHash.value(documentId);
}

void DedupDb::Impl::clearExTmpRun(uint64 sessionId, PeerId peerId) {
	if (const auto i = _exTmpStates.find({ sessionId, peerId.value });
		i != _exTmpStates.end()) {
		_exPendingWrites -= int(i->second.pending.size());
		_exTmpStates.erase(i);
	}
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM ex_tmp "
		"WHERE session_id = :session_id AND peer_id = :peer_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ClearExTmpRun failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::clearExTmpSession(uint64 sessionId) {
	for (auto i = _exTmpStates.begin(); i != _exTmpStates.end();) {
		if (i->first.sessionId == sessionId) {
			_exPendingWrites -= int(i->second.pending.size());
			i = _exTmpStates.erase(i);
		} else {
			++i;
		}
	}
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM ex_tmp WHERE session_id = :session_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: ClearExTmpSession failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::writeChatScanRow(const ChatScanRow &row) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO chat_scan "
		"(peer_id, category, unique_count, unique_bytes, "
		"dup_count, dup_bytes, scanned_upto, scan_finished, "
		"delete_finished) "
		"VALUES (:peer_id, :category, :unique_count, "
		":unique_bytes, :dup_count, :dup_bytes, :scanned_upto, "
		":scan_finished, :delete_finished)"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(row.peerId.value)));
	q.bindValue(u":category"_q, row.category);
	q.bindValue(u":unique_count"_q, row.uniqueCount);
	q.bindValue(u":unique_bytes"_q, QVariant::fromValue(
		static_cast<qlonglong>(row.uniqueBytes)));
	q.bindValue(u":dup_count"_q, row.dupCount);
	q.bindValue(u":dup_bytes"_q, QVariant::fromValue(
		static_cast<qlonglong>(row.dupBytes)));
	q.bindValue(u":scanned_upto"_q, QVariant::fromValue(
		static_cast<qlonglong>(row.scannedUpto.bare)));
	q.bindValue(u":scan_finished"_q, row.scanFinished ? 1 : 0);
	q.bindValue(u":delete_finished"_q, row.deleteFinished ? 1 : 0);
	if (!q.exec()) {
		LOG(("DedupDb: WriteChatScanRow failed: %1").arg(
			q.lastError().text()));
	}
}

std::vector<ChatScanRow> DedupDb::Impl::readChatScan(
		PeerId peerId) const {
	auto result = std::vector<ChatScanRow>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT category, unique_count, unique_bytes, dup_count, "
		"dup_bytes, scanned_upto, scan_finished, delete_finished "
		"FROM chat_scan "
		"WHERE peer_id = :peer_id "
		"ORDER BY category"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ReadChatScan failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto row = ChatScanRow();
		row.peerId = peerId;
		row.category = q.value(0).toInt();
		row.uniqueCount = q.value(1).toInt();
		row.uniqueBytes = q.value(2).toLongLong();
		row.dupCount = q.value(3).toInt();
		row.dupBytes = q.value(4).toLongLong();
		row.scannedUpto = MsgId(q.value(5).toLongLong());
		row.scanFinished = (q.value(6).toInt() != 0);
		row.deleteFinished = (q.value(7).toInt() != 0);
		result.push_back(row);
	}
	return result;
}

bool DedupDb::Impl::isChatScanned(PeerId peerId) const {
	if (!_open) {
		return false;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT 1 FROM chat_scan "
		"WHERE peer_id = :peer_id "
		"AND scan_finished != 0 LIMIT 1"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: IsChatScanned failed: %1").arg(
			q.lastError().text()));
		return false;
	}
	return q.next();
}

void DedupDb::Impl::markChatDeleteFinished(PeerId peerId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE chat_scan SET delete_finished = 1, "
		"dup_count = 0, dup_bytes = 0 "
		"WHERE peer_id = :peer_id"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: MarkChatDeleteFinished failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::writeChatScanDup(const ChatScanDup &dup) {
	if (!_open || !dup.msgId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO chat_scan_dups "
		"(peer_id, msg_id, category, size, flags, filename) "
		"VALUES (:peer_id, :msg_id, :category, :size, "
		":flags, :filename)"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(dup.peerId.value)));
	q.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(dup.msgId.bare)));
	q.bindValue(u":category"_q, dup.category);
	q.bindValue(u":size"_q, QVariant::fromValue(
		static_cast<qlonglong>(dup.size)));
	q.bindValue(u":flags"_q, dup.flags);
	q.bindValue(u":filename"_q, dup.filename);
	if (!q.exec()) {
		LOG(("DedupDb: WriteChatScanDup failed: %1").arg(
			q.lastError().text()));
	}
}

std::vector<ChatScanDup> DedupDb::Impl::readChatScanDups(
		PeerId peerId,
		int category,
		int limit,
		int offset) const {
	auto result = std::vector<ChatScanDup>();
	if (!_open) {
		return result;
	}
	auto sql = u"SELECT msg_id, category, size, flags, filename "
		"FROM chat_scan_dups "
		"WHERE peer_id = :peer_id"_q;
	if (category >= 0) {
		sql += u" AND category = :category"_q;
	}
	sql += u" ORDER BY msg_id DESC LIMIT :limit OFFSET :offset"_q;
	QSqlQuery q(_db);
	q.prepare(sql);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (category >= 0) {
		q.bindValue(u":category"_q, category);
	}
	q.bindValue(u":limit"_q, limit);
	q.bindValue(u":offset"_q, offset);
	if (!q.exec()) {
		LOG(("DedupDb: ReadChatScanDups failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto dup = ChatScanDup();
		dup.peerId = peerId;
		dup.msgId = MsgId(q.value(0).toLongLong());
		dup.category = q.value(1).toInt();
		dup.size = q.value(2).toLongLong();
		dup.flags = q.value(3).toInt();
		dup.filename = q.value(4).toString();
		result.push_back(dup);
	}
	return result;
}

std::vector<ChatScanDup> DedupDb::Impl::readChatScanDupsBefore(
		PeerId peerId,
		int category,
		int limit,
		MsgId before) const {
	auto result = std::vector<ChatScanDup>();
	if (!_open || limit <= 0) {
		return result;
	}
	auto sql = u"SELECT msg_id, category, size, flags, filename "
		"FROM chat_scan_dups "
		"WHERE peer_id = :peer_id"_q;
	if (category >= 0) {
		sql += u" AND category = :category"_q;
	}
	if (before.bare > 0) {
		sql += u" AND msg_id < :before"_q;
	}
	sql += u" ORDER BY msg_id DESC LIMIT :limit"_q;
	QSqlQuery q(_db);
	q.prepare(sql);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (category >= 0) {
		q.bindValue(u":category"_q, category);
	}
	if (before.bare > 0) {
		q.bindValue(u":before"_q, before.bare);
	}
	q.bindValue(u":limit"_q, limit);
	if (!q.exec()) {
		LOG(("DedupDb: ReadChatScanDupsBefore failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto dup = ChatScanDup();
		dup.peerId = peerId;
		dup.msgId = MsgId(q.value(0).toLongLong());
		dup.category = q.value(1).toInt();
		dup.size = q.value(2).toLongLong();
		dup.flags = q.value(3).toInt();
		dup.filename = q.value(4).toString();
		result.push_back(dup);
	}
	return result;
}

int DedupDb::Impl::chatScanDupCount(PeerId peerId, int category) const {
	if (!_open) {
		return 0;
	}
	auto sql = u"SELECT COUNT(*) FROM chat_scan_dups "
		"WHERE peer_id = :peer_id"_q;
	if (category >= 0) {
		sql += u" AND category = :category"_q;
	}
	QSqlQuery q(_db);
	q.prepare(sql);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (category >= 0) {
		q.bindValue(u":category"_q, category);
	}
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanDupCount failed: %1").arg(
			q.lastError().text()));
		return 0;
	}
	return q.next() ? q.value(0).toInt() : 0;
}

int64 DedupDb::Impl::chatScanDupBytes(PeerId peerId) const {
	if (!_open) {
		return 0;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT SUM(size) FROM chat_scan_dups "
		"WHERE peer_id = :peer_id"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanDupBytes failed: %1").arg(
			q.lastError().text()));
		return 0;
	}
	return q.next() ? int64(q.value(0).toLongLong()) : 0;
}

std::vector<ChatScanCategoryTotals> DedupDb::Impl::chatScanCategoryTotals(
		PeerId peerId) const {
	auto result = std::vector<ChatScanCategoryTotals>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT i.category, "
		"SUM(CASE WHEN d.msg_id IS NULL THEN 1 ELSE 0 END), "
		"SUM(CASE WHEN d.msg_id IS NULL THEN i.size ELSE 0 END), "
		"SUM(CASE WHEN d.msg_id IS NULL THEN 0 ELSE 1 END), "
		"SUM(CASE WHEN d.msg_id IS NULL THEN 0 ELSE i.size END) "
		"FROM chat_scan_items i "
		"LEFT JOIN chat_scan_dups d "
		"ON d.peer_id = i.peer_id AND d.msg_id = i.msg_id "
		"WHERE i.peer_id = :peer_id "
		"GROUP BY i.category"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanCategoryTotals failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto row = ChatScanCategoryTotals();
		row.category = q.value(0).toInt();
		row.uniqueCount = q.value(1).toLongLong();
		row.uniqueBytes = q.value(2).toLongLong();
		row.dupCount = q.value(3).toLongLong();
		row.dupBytes = q.value(4).toLongLong();
		result.push_back(row);
	}
	return result;
}

qint64 DedupDb::Impl::chatScanItemCount(PeerId peerId) const {
	if (!_open) {
		return 0;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT COUNT(*) FROM chat_scan_items "
		"WHERE peer_id = :peer_id"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanItemCount failed: %1").arg(
			q.lastError().text()));
		return 0;
	}
	return q.next() ? qint64(q.value(0).toLongLong()) : 0;
}

MsgId DedupDb::Impl::chatScanMaxMsgId(PeerId peerId) const {
	if (!_open) {
		return MsgId(0);
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT MAX(msg_id) FROM chat_scan_items "
		"WHERE peer_id = :peer_id"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanMaxMsgId failed: %1").arg(
			q.lastError().text()));
		return MsgId(0);
	}
	return q.next() ? MsgId(q.value(0).toLongLong()) : MsgId(0);
}

bool DedupDb::Impl::chatScanContainsHash(
		PeerId peerId,
		const QByteArray &hash) const {
	if (!_open || hash.isEmpty()) {
		return false;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT 1 FROM chat_scan_items "
		"WHERE peer_id = :peer_id "
		"AND hash = :hash LIMIT 1"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":hash"_q, hash);
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanContainsHash failed: %1").arg(
			q.lastError().text()));
		return false;
	}
	return q.next();
}

QByteArray DedupDb::Impl::chatScanHashForMedia(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto) const {
	if (!_open || !mediaId) {
		return QByteArray();
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT hash FROM chat_scan_items "
		"WHERE peer_id = :peer_id AND media_id = :media_id "
		"AND is_photo = :is_photo "
		"AND hash IS NOT NULL AND length(hash) > 0 LIMIT 1"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(mediaId)));
	q.bindValue(u":is_photo"_q, isPhoto ? 1 : 0);
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanHashForMedia failed: %1").arg(
			q.lastError().text()));
		return QByteArray();
	}
	return q.next() ? q.value(0).toByteArray() : QByteArray();
}

std::vector<ChatScanItem> DedupDb::Impl::chatScanUnresolvedItems(
		PeerId peerId) const {
	auto result = std::vector<ChatScanItem>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT msg_id, category, size, media_id, "
		"is_photo, filename FROM chat_scan_items "
		"WHERE peer_id = :peer_id "
		"AND (hash IS NULL OR length(hash) = 0) "
		"ORDER BY msg_id ASC"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanUnresolvedItems failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto item = ChatScanItem();
		item.peerId = peerId;
		item.msgId = MsgId(q.value(0).toLongLong());
		item.category = q.value(1).toInt();
		item.size = q.value(2).toLongLong();
		item.mediaId = q.value(3).toULongLong();
		item.isPhoto = (q.value(4).toInt() != 0);
		item.filename = q.value(5).toString();
		result.push_back(item);
	}
	return result;
}

std::vector<ChatScanItem> DedupDb::Impl::chatScanPendingItemsWindow(
		PeerId peerId,
		MsgId above,
		int limit) const {
	auto result = std::vector<ChatScanItem>();
	if (!_open || limit <= 0 || above.bare <= 0) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT msg_id, category, size, media_id, "
		"is_photo, filename, hash, attempts FROM chat_scan_items "
		"WHERE peer_id = :peer_id "
		"AND hash IS NULL "
		"AND state IN (0, 1) "
		"AND msg_id < :above "
		"ORDER BY msg_id DESC LIMIT :limit"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":above"_q, above.bare);
	q.bindValue(u":limit"_q, limit);
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanPendingItemsWindow failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto item = ChatScanItem();
		item.peerId = peerId;
		item.msgId = MsgId(q.value(0).toLongLong());
		item.category = q.value(1).toInt();
		item.size = q.value(2).toLongLong();
		item.mediaId = q.value(3).toULongLong();
		item.isPhoto = (q.value(4).toInt() != 0);
		item.filename = q.value(5).toString();
		item.hash = q.value(6).toByteArray();
		item.attempts = q.value(7).toInt();
		result.push_back(item);
	}
	return result;
}

std::vector<ChatScanItem> DedupDb::Impl::chatScanItemsWindow(
		PeerId peerId,
		MsgId above,
		int limit) const {
	auto result = std::vector<ChatScanItem>();
	if (!_open || limit <= 0) {
		return result;
	}
	auto sql = u"SELECT msg_id, category, size, media_id, "
		"is_photo, filename, hash FROM chat_scan_items "
		"WHERE peer_id = :peer_id"_q;
	if (above.bare > 0) {
		sql += u" AND msg_id < :above"_q;
	}
	sql += u" ORDER BY msg_id DESC LIMIT :limit"_q;
	QSqlQuery q(_db);
	q.prepare(sql);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (above.bare > 0) {
		q.bindValue(u":above"_q, above.bare);
	}
	q.bindValue(u":limit"_q, limit);
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanItemsWindow failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto item = ChatScanItem();
		item.peerId = peerId;
		item.msgId = MsgId(q.value(0).toLongLong());
		item.category = q.value(1).toInt();
		item.size = q.value(2).toLongLong();
		item.mediaId = q.value(3).toULongLong();
		item.isPhoto = (q.value(4).toInt() != 0);
		item.filename = q.value(5).toString();
		item.hash = q.value(6).toByteArray();
		result.push_back(item);
	}
	return result;
}

std::vector<ChatScanItem> DedupDb::Impl::chatScanHashedItemsWindow(
		PeerId peerId,
		MsgId above,
		int limit) const {
	auto result = std::vector<ChatScanItem>();
	if (!_open || limit <= 0) {
		return result;
	}
	auto sql = u"SELECT msg_id, category, size, media_id, "
		"is_photo, filename, hash FROM chat_scan_items "
		"WHERE peer_id = :peer_id "
		"AND hash IS NOT NULL AND length(hash) != 0"_q;
	if (above.bare > 0) {
		sql += u" AND msg_id < :above"_q;
	}
	sql += u" ORDER BY msg_id DESC LIMIT :limit"_q;
	QSqlQuery q(_db);
	q.prepare(sql);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (above.bare > 0) {
		q.bindValue(u":above"_q, above.bare);
	}
	q.bindValue(u":limit"_q, limit);
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanHashedItemsWindow failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto item = ChatScanItem();
		item.peerId = peerId;
		item.msgId = MsgId(q.value(0).toLongLong());
		item.category = q.value(1).toInt();
		item.size = q.value(2).toLongLong();
		item.mediaId = q.value(3).toULongLong();
		item.isPhoto = (q.value(4).toInt() != 0);
		item.filename = q.value(5).toString();
		item.hash = q.value(6).toByteArray();
		result.push_back(item);
	}
	return result;
}

int DedupDb::Impl::chatScanPendingCount(PeerId peerId) const {
	if (!_open) {
		return 0;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT COUNT(DISTINCT media_id) FROM chat_scan_items "
		"WHERE peer_id = :peer_id "
		"AND state = 0 "
		"AND media_id != 0"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanPendingCount failed: %1").arg(
			q.lastError().text()));
		return 0;
	}
	return q.next() ? int(q.value(0).toLongLong()) : 0;
}

int DedupDb::Impl::chatScanHashedCount(PeerId peerId) const {
	if (!_open) {
		return 0;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT COUNT(*) FROM chat_scan_items "
		"WHERE peer_id = :peer_id "
		"AND hash IS NOT NULL "
		"AND length(hash) > 0"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanHashedCount failed: %1").arg(
			q.lastError().text()));
		return 0;
	}
	return q.next() ? int(q.value(0).toLongLong()) : 0;
}

void DedupDb::Impl::chatScanResetLeases(PeerId peerId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE chat_scan_items "
		"SET state = 0, lease_until = 0, attempts = 0 "
		"WHERE peer_id = :peer_id AND state IN (0, 1)"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanResetLeases failed: %1").arg(
			q.lastError().text()));
	}
}

std::vector<ChatScanItem> DedupDb::Impl::chatScanLeaseItems(
		PeerId peerId,
		int limit,
		qint64 now,
		qint64 leaseUntil) {
	auto result = std::vector<ChatScanItem>();
	if (!_open || limit <= 0) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT i.msg_id, i.media_id, i.category, i.size, "
		"i.is_photo, i.filename, i.attempts "
		"FROM chat_scan_items AS i "
		"WHERE i.peer_id = :peer_id AND i.state = 0 "
		"AND i.msg_id IN ("
			"SELECT MAX(msg_id) "
			"FROM chat_scan_items "
			"WHERE peer_id = :rep_peer_id AND state = 0 "
			"AND media_id != 0 AND lease_until <= :now "
			"GROUP BY media_id "
			"ORDER BY MAX(msg_id) DESC LIMIT :limit"
		") "
		"ORDER BY i.msg_id DESC"_q);
	q.bindValue(u":rep_peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":now"_q, now);
	q.bindValue(u":limit"_q, limit);
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanLeaseItems failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto item = ChatScanItem();
		item.peerId = peerId;
		item.msgId = MsgId(q.value(0).toLongLong());
		item.mediaId = q.value(1).toULongLong();
		item.category = q.value(2).toInt();
		item.size = q.value(3).toLongLong();
		item.isPhoto = (q.value(4).toInt() != 0);
		item.filename = q.value(5).toString();
		item.attempts = q.value(6).toInt() + 1;
		item.state = 1;
		result.push_back(item);
	}
	if (result.empty()) {
		return result;
	}
	auto placeholders = QStringList();
	for (auto i = 0; i != int(result.size()); ++i) {
		placeholders.push_back(u":m%1"_q.arg(i));
	}
	QSqlQuery mark(_db);
	mark.prepare(u"UPDATE chat_scan_items "
		"SET state = 1, lease_until = :lease_until, "
		"attempts = attempts + 1 "
		"WHERE peer_id = :peer_id AND state = 0 "
		"AND msg_id IN (" + placeholders.join(u", "_q) + u")"_q);
	mark.bindValue(u":lease_until"_q, leaseUntil);
	mark.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	for (auto i = 0; i != int(result.size()); ++i) {
		mark.bindValue(u":m%1"_q.arg(i), QVariant::fromValue(
			static_cast<qint64>(result[i].msgId.bare)));
	}
	if (!mark.exec()) {
		LOG(("DedupDb: ChatScanLeaseMark failed: %1").arg(
			mark.lastError().text()));
		return {};
	}
	return result;
}

void DedupDb::Impl::chatScanReleaseItem(
		PeerId peerId,
		uint64 mediaId,
		qint64 retryAt) {
	if (!_open || !mediaId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE chat_scan_items "
		"SET state = 0, lease_until = :retry_at "
		"WHERE peer_id = :peer_id AND media_id = :media_id "
		"AND state = 1"_q);
	q.bindValue(u":retry_at"_q, retryAt);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(mediaId)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanReleaseItem failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::chatScanReleaseLeases(PeerId peerId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE chat_scan_items "
		"SET state = 0, lease_until = 0 "
		"WHERE peer_id = :peer_id AND state = 1"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanReleaseLeases failed: %1").arg(
			q.lastError().text()));
	}
}

int DedupDb::Impl::chatScanCommitHashes(
		PeerId peerId,
		const std::vector<std::pair<uint64, QByteArray>> &hashes) {
	if (!_open || hashes.empty()) {
		return 0;
	}
	auto resolved = 0;
	beginTransaction();
	for (const auto &[mediaId, hash] : hashes) {
		if (!mediaId || hash.isEmpty()) {
			continue;
		}
		QSqlQuery count(_db);
		count.prepare(u"SELECT COUNT(*) FROM chat_scan_items "
			"WHERE peer_id = :peer_id AND media_id = :media_id "
			"AND (hash IS NULL OR length(hash) = 0)"_q);
		count.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		count.bindValue(u":media_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(mediaId)));
		if (count.exec() && count.next()) {
			resolved += count.value(0).toInt();
		}
		QSqlQuery q(_db);
		q.prepare(u"UPDATE chat_scan_items "
			"SET hash = :hash, state = 2, lease_until = 0 "
			"WHERE peer_id = :peer_id AND media_id = :media_id"_q);
		q.bindValue(u":hash"_q, hash);
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		q.bindValue(u":media_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(mediaId)));
		if (!q.exec()) {
			LOG(("DedupDb: ChatScanCommitHash failed: %1").arg(
				q.lastError().text()));
		}
	}
	commitTransaction();
	return resolved;
}

void DedupDb::Impl::chatScanFailItem(PeerId peerId, uint64 mediaId) {
	if (!_open || !mediaId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE chat_scan_items "
		"SET state = 3, lease_until = 0 "
		"WHERE peer_id = :peer_id AND media_id = :media_id "
		"AND state = 1"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(mediaId)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanFailItem failed: %1").arg(
			q.lastError().text()));
	}
}

int DedupDb::Impl::chatScanRetryFailed(PeerId peerId) {
	if (!_open) {
		return 0;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE chat_scan_items "
		"SET state = 0, lease_until = 0, attempts = 0 "
		"WHERE peer_id = :peer_id AND state = 3"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanRetryFailed failed: %1").arg(
			q.lastError().text()));
		return 0;
	}
	return q.numRowsAffected();
}

int DedupDb::Impl::chatScanLeasedCount(PeerId peerId) const {
	if (!_open) {
		return 0;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT COUNT(DISTINCT media_id) FROM chat_scan_items "
		"WHERE peer_id = :peer_id AND state = 1"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanLeasedCount failed: %1").arg(
			q.lastError().text()));
		return 0;
	}
	return q.next() ? int(q.value(0).toLongLong()) : 0;
}

int DedupDb::Impl::chatScanFailedCount(PeerId peerId) const {
	if (!_open) {
		return 0;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT COUNT(DISTINCT media_id) FROM chat_scan_items "
		"WHERE peer_id = :peer_id AND state = 3"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanFailedCount failed: %1").arg(
			q.lastError().text()));
		return 0;
	}
	return q.next() ? int(q.value(0).toLongLong()) : 0;
}

std::vector<ChatScanProgressRow> DedupDb::Impl::readChatScanProgress(
		PeerId peerId) const {
	auto result = std::vector<ChatScanProgressRow>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT category, item_count, unique_count, "
		"unique_bytes, dup_count, dup_bytes FROM chat_scan_progress "
		"WHERE peer_id = :peer_id"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ReadChatScanProgress failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto row = ChatScanProgressRow();
		row.peerId = peerId;
		row.category = q.value(0).toInt();
		row.itemCount = q.value(1).toLongLong();
		row.uniqueCount = q.value(2).toLongLong();
		row.uniqueBytes = q.value(3).toLongLong();
		row.dupCount = q.value(4).toLongLong();
		row.dupBytes = q.value(5).toLongLong();
		result.push_back(row);
	}
	return result;
}

void DedupDb::Impl::updateChatScanProgress(
		PeerId peerId,
		int category,
		int64 itemCount,
		int64 uniqueCount,
		int64 uniqueBytes,
		int64 dupCount,
		int64 dupBytes) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT INTO chat_scan_progress "
		"(peer_id, category, item_count, unique_count, "
		"unique_bytes, dup_count, dup_bytes) "
		"VALUES (:peer_id, :category, :item_count, :unique_count, "
		":unique_bytes, :dup_count, :dup_bytes) "
		"ON CONFLICT(peer_id, category) DO UPDATE SET "
		"item_count = item_count + excluded.item_count, "
		"unique_count = unique_count + excluded.unique_count, "
		"unique_bytes = unique_bytes + excluded.unique_bytes, "
		"dup_count = dup_count + excluded.dup_count, "
		"dup_bytes = dup_bytes + excluded.dup_bytes"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":category"_q, category);
	q.bindValue(u":item_count"_q, QVariant::fromValue(
		static_cast<qlonglong>(itemCount)));
	q.bindValue(u":unique_count"_q, QVariant::fromValue(
		static_cast<qlonglong>(uniqueCount)));
	q.bindValue(u":unique_bytes"_q, QVariant::fromValue(
		static_cast<qlonglong>(uniqueBytes)));
	q.bindValue(u":dup_count"_q, QVariant::fromValue(
		static_cast<qlonglong>(dupCount)));
	q.bindValue(u":dup_bytes"_q, QVariant::fromValue(
		static_cast<qlonglong>(dupBytes)));
	if (!q.exec()) {
		LOG(("DedupDb: UpdateChatScanProgress failed: %1").arg(
			q.lastError().text()));
	}
}

std::vector<ChatScanProgressRow> DedupDb::Impl::recomputeChatScanProgress(
		PeerId peerId) {
	auto result = std::vector<ChatScanProgressRow>();
	if (!_open) {
		return result;
	}
	auto itemAgg = std::unordered_map<int, std::pair<int64, int64>>();
	auto dupAgg = std::unordered_map<int, std::pair<int64, int64>>();
	{
		QSqlQuery q(_db);
		q.prepare(u"SELECT category, COUNT(*), SUM(size) "
			"FROM chat_scan_items WHERE peer_id = :peer_id "
			"GROUP BY category"_q);
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		if (q.exec()) {
			while (q.next()) {
				itemAgg[int(q.value(0).toInt())] = std::make_pair(
					q.value(1).toLongLong(),
					q.value(2).toLongLong());
			}
		}
	}
	{
		QSqlQuery q(_db);
		q.prepare(u"SELECT category, COUNT(*), SUM(size) "
			"FROM chat_scan_dups WHERE peer_id = :peer_id "
			"GROUP BY category"_q);
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		if (q.exec()) {
			while (q.next()) {
				dupAgg[int(q.value(0).toInt())] = std::make_pair(
					q.value(1).toLongLong(),
					q.value(2).toLongLong());
			}
		}
	}
	for (const auto &[category, itemPair] : itemAgg) {
		auto row = ChatScanProgressRow();
		row.peerId = peerId;
		row.category = category;
		row.itemCount = itemPair.first;
		auto dupIt = dupAgg.find(category);
		if (dupIt != dupAgg.end()) {
			row.dupCount = dupIt->second.first;
			row.dupBytes = dupIt->second.second;
		}
		row.uniqueCount = std::max(int64(0), row.itemCount - row.dupCount);
		row.uniqueBytes = std::max(int64(0), itemPair.second - row.dupBytes);
		result.push_back(row);
	}
	{
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM chat_scan_progress "
			"WHERE peer_id = :peer_id"_q);
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		q.exec();
	}
	beginTransaction();
	for (const auto &row : result) {
		QSqlQuery q(_db);
		q.prepare(u"INSERT OR REPLACE INTO chat_scan_progress "
			"(peer_id, category, item_count, unique_count, "
			"unique_bytes, dup_count, dup_bytes) VALUES "
			"(:peer_id, :category, :item_count, :unique_count, "
			":unique_bytes, :dup_count, :dup_bytes)"_q);
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(row.peerId.value)));
		q.bindValue(u":category"_q, row.category);
		q.bindValue(u":item_count"_q, QVariant::fromValue(
			static_cast<qlonglong>(row.itemCount)));
		q.bindValue(u":unique_count"_q, QVariant::fromValue(
			static_cast<qlonglong>(row.uniqueCount)));
		q.bindValue(u":unique_bytes"_q, QVariant::fromValue(
			static_cast<qlonglong>(row.uniqueBytes)));
		q.bindValue(u":dup_count"_q, QVariant::fromValue(
			static_cast<qlonglong>(row.dupCount)));
		q.bindValue(u":dup_bytes"_q, QVariant::fromValue(
			static_cast<qlonglong>(row.dupBytes)));
		q.exec();
	}
	commitTransaction();
	return result;
}

std::vector<ChatScanItem> DedupDb::Impl::chatScanDistinctItems(
		PeerId peerId) const {
	auto result = std::vector<ChatScanItem>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT MIN(msg_id) AS msg_id, MIN(category) AS category, "
		"MIN(size) AS size, media_id, MIN(is_photo) AS is_photo, "
		"MIN(hash) AS hash, MIN(filename) AS filename "
		"FROM chat_scan_items "
		"WHERE peer_id = :peer_id "
		"AND hash IS NOT NULL AND length(hash) != 0 "
		"AND media_id != 0 "
		"GROUP BY media_id "
		"ORDER BY msg_id ASC"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanDistinctItems failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto item = ChatScanItem();
		item.peerId = peerId;
		item.msgId = MsgId(q.value(0).toLongLong());
		item.category = q.value(1).toInt();
		item.size = q.value(2).toLongLong();
		item.mediaId = q.value(3).toULongLong();
		item.isPhoto = (q.value(4).toInt() != 0);
		item.hash = q.value(5).toByteArray();
		item.filename = q.value(6).toString();
		result.push_back(item);
	}
	return result;
}

void DedupDb::Impl::writeGlobal(const QString &key, const QString &value) {
	if (!_open || key.isEmpty()) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO client_globals (key, value) "
		"VALUES (:key, :value)"_q);
	q.bindValue(u":key"_q, key);
	q.bindValue(u":value"_q, value);
	if (!q.exec()) {
		LOG(("DedupDb: Global Write failed: %1").arg(q.lastError().text()));
	}
}

QString DedupDb::Impl::readGlobal(const QString &key) const {
	if (!_open || key.isEmpty()) {
		return QString();
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT value FROM client_globals WHERE key = :key"_q);
	q.bindValue(u":key"_q, key);
	if (!q.exec()) {
		LOG(("DedupDb: Global Read failed: %1").arg(q.lastError().text()));
		return QString();
	}
	return q.next() ? q.value(0).toString() : QString();
}

void DedupDb::Impl::removeChatScanDup(PeerId peerId, MsgId msgId) {
	if (!_open || !msgId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM chat_scan_dups "
		"WHERE peer_id = :peer_id "
		"AND msg_id = :msg_id"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(msgId.bare)));
	if (!q.exec()) {
		LOG(("DedupDb: RemoveChatScanDup failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::clearChatScan(PeerId peerId) {
	if (!_open) {
		return;
	}
	const auto started = crl::now();
	const auto peer = QVariant::fromValue(
		static_cast<qulonglong>(peerId.value));
	const auto hasOtherPeers = [&](const QString &table) {
		auto q = QSqlQuery(_db);
		q.prepare(u"SELECT 1 FROM %1 "
			"WHERE peer_id != :peer_id LIMIT 1"_q.arg(table));
		q.bindValue(u":peer_id"_q, peer);
		if (!q.exec()) {
			LOG(("DedupDb: ClearChatScan check failed: %1").arg(
				q.lastError().text()));
			return true;
		}
		return q.next();
	};
	const auto peerRows = [&](const QString &table) {
		auto q = QSqlQuery(_db);
		q.prepare(u"SELECT COUNT(*) FROM %1 "
			"WHERE peer_id = :peer_id"_q.arg(table));
		q.bindValue(u":peer_id"_q, peer);
		return (q.exec() && q.next())
			? q.value(0).toInt()
			: 0;
	};
	const auto deletePeer = [&](const QString &table) {
		auto q = QSqlQuery(_db);
		q.prepare(u"DELETE FROM %1 "
			"WHERE peer_id = :peer_id"_q.arg(table));
		q.bindValue(u":peer_id"_q, peer);
		if (!q.exec()) {
			LOG(("DedupDb: ClearChatScan failed: %1").arg(
				q.lastError().text()));
		}
	};
	const auto deleteAll = [&](const QString &table) {
		auto q = QSqlQuery(_db);
		if (!q.exec(u"DELETE FROM %1"_q.arg(table))) {
			LOG(("DedupDb: ClearChatScan failed: %1").arg(
				q.lastError().text()));
		}
	};
	const auto dropIndex = [&](const QString &index) {
		auto q = QSqlQuery(_db);
		if (!q.exec(u"DROP INDEX IF EXISTS %1"_q.arg(index))) {
			LOG(("DedupDb: ClearChatScan drop index failed: %1").arg(
				q.lastError().text()));
		}
	};
	const auto createIndex = [&](
			const QString &index,
			const QString &table,
			const QString &columns) {
		auto q = QSqlQuery(_db);
		if (!q.exec(u"CREATE INDEX IF NOT EXISTS %1 "
				"ON %2 %3"_q.arg(index, table, columns))) {
			LOG(("DedupDb: ClearChatScan create index failed: %1").arg(
				q.lastError().text()));
		}
	};
	struct ClearTable {
		QString name;
		std::vector<std::pair<QString, QString>> indexes;
	};
	const auto tables = std::vector<ClearTable>{
		{ u"chat_scan_dups"_q, { {
			u"idx_chat_scan_dups_cat"_q,
			u"(peer_id, category)"_q,
		} } },
		{ u"chat_scan_items"_q, {
			{
				u"idx_chat_scan_items_media"_q,
				u"(peer_id, media_id)"_q,
			},
			{
				u"idx_chat_scan_items_hash"_q,
				u"(peer_id, hash)"_q,
			},
			{
				u"idx_chat_scan_items_state"_q,
				u"(peer_id, state, media_id)"_q,
			},
		} },
	};
	constexpr auto kIndexedClearThreshold = 4096;
	beginTransaction();
	for (const auto &table : tables) {
		if (!hasOtherPeers(table.name)) {
			deleteAll(table.name);
		} else if (peerRows(table.name) > kIndexedClearThreshold) {
			for (const auto &index : table.indexes) {
				dropIndex(index.first);
			}
			deletePeer(table.name);
			for (const auto &[index, definition] : table.indexes) {
				createIndex(index, table.name, definition);
			}
		} else {
			deletePeer(table.name);
		}
	}
	for (const auto &table : {
			u"chat_scan"_q,
			u"chat_scan_state"_q,
			u"chat_scan_progress"_q }) {
		deletePeer(table);
	}
	commitTransaction();
	LOG(("DedupDb: ChatScan clear took %1 ms.").arg(
		crl::now() - started));
}

void DedupDb::Impl::writeScanCursor(const ChatScanState &state) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO chat_scan_state "
		"(peer_id, cursor_msg_id, phase) "
		"VALUES (:peer_id, :cursor_msg_id, :phase)"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(state.peerId.value)));
	q.bindValue(u":cursor_msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(state.cursor.bare)));
	q.bindValue(u":phase"_q, state.phase);
	if (!q.exec()) {
		LOG(("DedupDb: WriteScanCursor failed: %1").arg(
			q.lastError().text()));
	}
}

std::optional<ChatScanState> DedupDb::Impl::readScanCursor(
		PeerId peerId) const {
	if (!_open) {
		return std::nullopt;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT cursor_msg_id, phase FROM chat_scan_state "
		"WHERE peer_id = :peer_id"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ReadScanCursor failed: %1").arg(
			q.lastError().text()));
		return std::nullopt;
	}
	if (!q.next()) {
		return std::nullopt;
	}
	auto state = ChatScanState();
	state.peerId = peerId;
	state.cursor = MsgId(q.value(0).toLongLong());
	state.phase = q.value(1).toInt();
	return state;
}

void DedupDb::Impl::writeChatScanItems(
		const std::vector<ChatScanItem> &items) {
	if (!_open || items.empty()) {
		return;
	}
	beginTransaction();
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO chat_scan_items "
		"(peer_id, msg_id, category, size, media_id, "
		"is_photo, filename, state, attempts, lease_until) "
		"VALUES (:peer_id, :msg_id, :category, :size, "
		":media_id, :is_photo, :filename, "
		":state, :attempts, :lease_until)"_q);
	for (const auto &item : items) {
		if (!item.msgId) {
			continue;
		}
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(item.peerId.value)));
		q.bindValue(u":msg_id"_q, QVariant::fromValue(
			static_cast<qlonglong>(item.msgId.bare)));
		q.bindValue(u":category"_q, item.category);
		q.bindValue(u":size"_q, QVariant::fromValue(
			static_cast<qlonglong>(item.size)));
		q.bindValue(u":media_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(item.mediaId)));
		q.bindValue(u":is_photo"_q, item.isPhoto ? 1 : 0);
		q.bindValue(u":filename"_q, item.filename);
		q.bindValue(u":state"_q, item.state);
		q.bindValue(u":attempts"_q, item.attempts);
		q.bindValue(u":lease_until"_q, item.leaseUntil);
		if (!q.exec()) {
			LOG(("DedupDb: WriteChatScanItems failed: %1").arg(
				q.lastError().text()));
		}
	}
	commitTransaction();
}

std::vector<ChatScanItem> DedupDb::Impl::readChatScanItems(
		PeerId peerId) const {
	auto result = std::vector<ChatScanItem>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT msg_id, category, size, media_id, "
		"is_photo, hash, filename FROM chat_scan_items "
		"WHERE peer_id = :peer_id "
		"ORDER BY msg_id DESC"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ReadChatScanItems failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto item = ChatScanItem();
		item.peerId = peerId;
		item.msgId = MsgId(q.value(0).toLongLong());
		item.category = q.value(1).toInt();
		item.size = q.value(2).toLongLong();
		item.mediaId = q.value(3).toULongLong();
		item.isPhoto = (q.value(4).toInt() != 0);
		item.hash = q.value(5).toByteArray();
		item.filename = q.value(6).toString();
		result.push_back(item);
	}
	return result;
}

int DedupDb::Impl::updateChatScanItemHash(
		PeerId peerId,
		uint64 mediaId,
		const QByteArray &hash) {
	if (!_open || !mediaId || hash.isEmpty()) {
		return 0;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE chat_scan_items SET hash = :hash "
		"WHERE peer_id = :peer_id "
		"AND media_id = :media_id "
		"AND (hash IS NULL OR length(hash) = 0)"_q);
	q.bindValue(u":hash"_q, hash);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(mediaId)));
	if (!q.exec()) {
		LOG(("DedupDb: UpdateChatScanItemHash failed: %1").arg(
			q.lastError().text()));
		return 0;
	}
	return q.numRowsAffected();
}

std::vector<PeerId> DedupDb::Impl::scannedPeerIds() const {
	auto result = std::vector<PeerId>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT DISTINCT peer_id FROM chat_scan "
		"WHERE scan_finished != 0"_q);
	if (!q.exec()) {
		LOG(("DedupDb: ScannedPeerIds failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		result.push_back(PeerId(q.value(0).toULongLong()));
	}
	return result;
}

std::optional<ChatScanDup> DedupDb::Impl::takeChatScanDup(
		PeerId peerId,
		MsgId msgId) {
	if (!_open || !msgId) {
		return std::nullopt;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT category, size, flags, filename "
		"FROM chat_scan_dups "
		"WHERE peer_id = :peer_id "
		"AND msg_id = :msg_id"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(msgId.bare)));
	if (!q.exec()) {
		LOG(("DedupDb: TakeChatScanDup failed: %1").arg(
			q.lastError().text()));
		return std::nullopt;
	}
	if (!q.next()) {
		return std::nullopt;
	}
	auto dup = ChatScanDup();
	dup.peerId = peerId;
	dup.msgId = msgId;
	dup.category = q.value(0).toInt();
	dup.size = q.value(1).toLongLong();
	dup.flags = q.value(2).toInt();
	dup.filename = q.value(3).toString();
	removeChatScanDup(peerId, msgId);
	return dup;
}

void DedupDb::Impl::removeChatScanItem(PeerId peerId, MsgId msgId) {
	if (!_open || !msgId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM chat_scan_items "
		"WHERE peer_id = :peer_id "
		"AND msg_id = :msg_id"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(msgId.bare)));
	if (!q.exec()) {
		LOG(("DedupDb: RemoveChatScanItem failed: %1").arg(
			q.lastError().text()));
	}
}

bool DedupDb::Impl::hasChatScanItem(PeerId peerId, MsgId msgId) const {
	if (!_open || !msgId) {
		return false;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT 1 FROM chat_scan_items "
		"WHERE peer_id = :peer_id "
		"AND msg_id = :msg_id LIMIT 1"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(msgId.bare)));
	if (!q.exec()) {
		LOG(("DedupDb: HasChatScanItem failed: %1").arg(
			q.lastError().text()));
		return false;
	}
	return q.next();
}

std::optional<ChatScanItem> DedupDb::Impl::chatScanItem(
		PeerId peerId,
		MsgId msgId) const {
	if (!_open || !msgId) {
		return std::nullopt;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT category, size, media_id, is_photo, "
		"hash, filename FROM chat_scan_items "
		"WHERE peer_id = :peer_id "
		"AND msg_id = :msg_id"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(msgId.bare)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanItem failed: %1").arg(
			q.lastError().text()));
		return std::nullopt;
	}
	if (!q.next()) {
		return std::nullopt;
	}
	auto item = ChatScanItem();
	item.peerId = peerId;
	item.msgId = msgId;
	item.category = q.value(0).toInt();
	item.size = q.value(1).toLongLong();
	item.mediaId = q.value(2).toULongLong();
	item.isPhoto = (q.value(3).toInt() != 0);
	item.hash = q.value(4).toByteArray();
	item.filename = q.value(5).toString();
	return item;
}

void DedupDb::Impl::clearChatScanDups(PeerId peerId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM chat_scan_dups "
		"WHERE peer_id = :peer_id"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ClearChatScanDups failed: %1").arg(
			q.lastError().text()));
	}
}

QString DedupDb::Impl::TmpTableName(TmpTable table) {
	switch (table) {
	case TmpTable::Dl: return u"dl_tmp"_q;
	case TmpTable::Ul: return u"ul_tmp"_q;
	case TmpTable::Fw: return u"fw_tmp"_q;
	}
	Unexpected("Tmp table.");
}

DedupDb::Impl::TmpState &DedupDb::Impl::tmpState(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId) {
	ensureTmpLoaded(table, sessionId, scopeId);
	return _tmpStates[{ table, sessionId, scopeId }];
}

const DedupDb::Impl::TmpState &DedupDb::Impl::tmpState(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId) const {
	ensureTmpLoaded(table, sessionId, scopeId);
	return _tmpStates[{ table, sessionId, scopeId }];
}

void DedupDb::Impl::ensureTmpLoaded(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId) const {
	auto &s = _tmpStates[{ table, sessionId, scopeId }];
	if (s.loaded || !_open) {
		return;
	}
	s.loaded = true;
	QSqlQuery q(_db);
	q.prepare(u"SELECT doc_id, hash FROM %1 "
		"WHERE session_id = :session_id AND scope_id = :scope_id"_q.arg(
			TmpTableName(table)));
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":scope_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(scopeId)));
	if (!q.exec()) {
		LOG(("DedupDb: LoadTmp failed: %1").arg(q.lastError().text()));
		return;
	}
	while (q.next()) {
		const auto docId = q.value(0).toULongLong();
		const auto hash = q.value(1).toByteArray();
		if (!docId || hash.isEmpty()) {
			continue;
		}
		s.docToHash[docId] = hash;
		s.hashToDoc[hash].insert(docId);
	}
}

void DedupDb::Impl::insertTmp(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		uint64 documentId,
		const QByteArray &hash) {
	if (!_open || !documentId || hash.isEmpty()) {
		return;
	}
	auto &s = tmpState(table, sessionId, scopeId);
	s.docToHash[documentId] = hash;
	s.hashToDoc[hash].insert(documentId);
	s.pending.push_back({ documentId, hash });
	if (++_tmpPendingWrites >= kExWriteBatchLimit) {
		flushTmpPending();
	}
}

void DedupDb::Impl::removeTmpDoc(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		uint64 documentId) {
	if (!documentId) {
		return;
	}
	if (const auto i = _tmpStates.find({ table, sessionId, scopeId });
		i != _tmpStates.end()) {
		auto &s = i->second;
		if (const auto j = s.docToHash.find(documentId);
			j != s.docToHash.end()) {
			const auto hash = j.value();
			s.docToHash.erase(j);
			if (const auto k = s.hashToDoc.find(hash);
				k != s.hashToDoc.end()) {
				k.value().remove(documentId);
				if (k.value().isEmpty()) {
					s.hashToDoc.erase(k);
				}
			}
		}
		auto &pending = s.pending;
		auto w = pending.begin();
		for (auto r = pending.begin(); r != pending.end(); ++r) {
			if (r->docId != documentId) {
				if (w != r) {
					*w = std::move(*r);
				}
				++w;
			} else {
				--_tmpPendingWrites;
			}
		}
		pending.erase(w, pending.end());
	}
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM %1 "
		"WHERE session_id = :session_id AND scope_id = :scope_id "
		"AND doc_id = :doc_id"_q.arg(TmpTableName(table)));
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":scope_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(scopeId)));
	q.bindValue(u":doc_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(documentId)));
	if (!q.exec()) {
		LOG(("DedupDb: RemoveTmpDoc failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::flushTmpPending() {
	if (_tmpPendingWrites == 0 || !_open) {
		return;
	}
	_tmpPendingWrites = 0;
	beginTransaction();
	for (const auto table : { TmpTable::Dl, TmpTable::Ul, TmpTable::Fw }) {
		QSqlQuery tmp(_db);
		tmp.prepare(u"INSERT OR REPLACE INTO %1 "
			"(session_id, scope_id, doc_id, hash, status) "
			"VALUES (:session_id, :scope_id, :doc_id, :hash, 'f')"_q.arg(
				TmpTableName(table)));
		for (auto &[key, s] : _tmpStates) {
			if (key.table != table) {
				continue;
			}
			for (const auto &row : s.pending) {
				tmp.bindValue(u":session_id"_q, QVariant::fromValue(
					static_cast<qulonglong>(key.sessionId)));
				tmp.bindValue(u":scope_id"_q, QVariant::fromValue(
					static_cast<qulonglong>(key.scopeId)));
				tmp.bindValue(u":doc_id"_q, QVariant::fromValue(
					static_cast<qulonglong>(row.docId)));
				tmp.bindValue(u":hash"_q, row.hash);
				if (!tmp.exec()) {
					LOG(("DedupDb: InsertTmp failed: %1").arg(
						tmp.lastError().text()));
				}
			}
			s.pending.clear();
		}
	}
	commitTransaction();
}

bool DedupDb::Impl::containsTmpDocId(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		uint64 documentId) const {
	if (!documentId) {
		return false;
	}
	return tmpState(table, sessionId, scopeId).docToHash.contains(
		documentId);
}

bool DedupDb::Impl::containsTmpHash(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		const QByteArray &hash) const {
	if (hash.isEmpty()) {
		return false;
	}
	const auto &s = tmpState(table, sessionId, scopeId);
	const auto i = s.hashToDoc.find(hash);
	return (i != s.hashToDoc.end()) && !i->isEmpty();
}

QByteArray DedupDb::Impl::hashForTmpDocId(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		uint64 documentId) const {
	if (!documentId) {
		return {};
	}
	return tmpState(table, sessionId, scopeId).docToHash.value(documentId);
}

void DedupDb::Impl::clearTmpRun(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId) {
	if (const auto i = _tmpStates.find({ table, sessionId, scopeId });
		i != _tmpStates.end()) {
		_tmpPendingWrites -= int(i->second.pending.size());
		_tmpStates.erase(i);
	}
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM %1 "
		"WHERE session_id = :session_id AND scope_id = :scope_id"_q.arg(
			TmpTableName(table)));
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":scope_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(scopeId)));
	if (!q.exec()) {
		LOG(("DedupDb: ClearTmpRun failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::insertEfResumeItem(const EfResumeItem &item) {
	_impl->insertEfResumeItem(item);
}

void DedupDb::removeEfResumeItem(const QString &jobId, int itemIndex) {
	_impl->removeEfResumeItem(jobId, itemIndex);
}

void DedupDb::removeEfResumeBySource(
		uint64 sessionId,
		PeerId destPeerId,
		MsgId sourceMsgId) {
	_impl->removeEfResumeBySource(sessionId, destPeerId, sourceMsgId);
}

void DedupDb::clearEfResumeForPeer(PeerId peerId) {
	_impl->clearEfResumeForPeer(peerId);
}

void DedupDb::clearEfResumeJob(const QString &jobId) {
	_impl->clearEfResumeJob(jobId);
}

std::vector<EfResumeItem> DedupDb::loadEfResumeItemsForPeer(
		uint64 sessionId,
		PeerId peerId) const {
	return _impl->loadEfResumeItemsForPeer(sessionId, peerId);
}

std::vector<EfResumeItem> DedupDb::loadUnfinishedEfResumeItems(
		uint64 sessionId) const {
	return _impl->loadUnfinishedEfResumeItems(sessionId);
}

void DedupDb::setEfResumePaused(
		uint64 sessionId,
		PeerId peerId,
		bool paused) {
	_impl->setEfResumePaused(sessionId, peerId, paused);
}

void DedupDb::insertNfResume(const NfResumeRecord &record) {
	_impl->insertNfResume(record);
}

void DedupDb::removeNfResume(uint64 sessionId, PeerId destPeerId) {
	_impl->removeNfResume(sessionId, destPeerId);
}

void DedupDb::clearNfResume(uint64 sessionId) {
	_impl->clearNfResume(sessionId);
}

std::vector<NfResumeRecord> DedupDb::loadNfResume(uint64 sessionId) const {
	return _impl->loadNfResume(sessionId);
}

void DedupDb::insertExResume(const ExResumeRecord &record) {
	_impl->insertExResume(record);
}

void DedupDb::removeExResumeFolder(
		uint64 sessionId,
		PeerId peerId,
		int mode,
		const QString &folder) {
	_impl->removeExResumeFolder(sessionId, peerId, mode, folder);
}

void DedupDb::clearExResume(uint64 sessionId, int mode) {
	_impl->clearExResume(sessionId, mode);
}

std::vector<ExResumeRecord> DedupDb::loadExResume(
		uint64 sessionId,
		int mode) const {
	return _impl->loadExResume(sessionId, mode);
}

void DedupDb::rebaseExResumeFolder(
		uint64 sessionId,
		int mode,
		const QString &oldPrefix,
		const QString &newPrefix) {
	_impl->rebaseExResumeFolder(sessionId, mode, oldPrefix, newPrefix);
}

void DedupDb::insertExTmp(
		uint64 sessionId,
		PeerId peerId,
		uint64 documentId,
		const QByteArray &hash) {
	_impl->insertExTmp(sessionId, peerId, documentId, hash);
}

bool DedupDb::containsExTmpDocId(
		uint64 sessionId,
		PeerId peerId,
		uint64 documentId) const {
	return _impl->containsExTmpDocId(sessionId, peerId, documentId);
}

bool DedupDb::containsExTmpHash(
		uint64 sessionId,
		PeerId peerId,
		const QByteArray &hash) const {
	return _impl->containsExTmpHash(sessionId, peerId, hash);
}

void DedupDb::flushExTmp() {
	_impl->flushExPending();
}

QByteArray DedupDb::hashForExTmpDocId(
	uint64 sessionId,
	PeerId peerId,
	uint64 documentId) const {
	return _impl->hashForExTmpDocId(sessionId, peerId, documentId);
}

void DedupDb::clearExTmpRun(uint64 sessionId, PeerId peerId) {
	_impl->clearExTmpRun(sessionId, peerId);
}

void DedupDb::clearExTmpSession(uint64 sessionId) {
	_impl->clearExTmpSession(sessionId);
}

void DedupDb::insertDlTmp(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId,
		const QByteArray &hash) {
	_impl->insertTmp(Impl::TmpTable::Dl, sessionId, batchId, documentId, hash);
}

bool DedupDb::containsDlTmpDocId(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId) const {
	return _impl->containsTmpDocId(
		Impl::TmpTable::Dl,
		sessionId,
		batchId,
		documentId);
}

bool DedupDb::containsDlTmpHash(
		uint64 sessionId,
		uint64 batchId,
		const QByteArray &hash) const {
	return _impl->containsTmpHash(
		Impl::TmpTable::Dl,
		sessionId,
		batchId,
		hash);
}

QByteArray DedupDb::hashForDlTmpDocId(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId) const {
	return _impl->hashForTmpDocId(
		Impl::TmpTable::Dl,
		sessionId,
		batchId,
		documentId);
}

void DedupDb::clearDlTmpRun(uint64 sessionId, uint64 batchId) {
	_impl->clearTmpRun(Impl::TmpTable::Dl, sessionId, batchId);
}

void DedupDb::removeDlTmpDoc(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId) {
	_impl->removeTmpDoc(Impl::TmpTable::Dl, sessionId, batchId, documentId);
}

void DedupDb::insertUlTmp(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId,
		const QByteArray &hash) {
	_impl->insertTmp(Impl::TmpTable::Ul, sessionId, batchId, documentId, hash);
}

bool DedupDb::containsUlTmpDocId(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId) const {
	return _impl->containsTmpDocId(
		Impl::TmpTable::Ul,
		sessionId,
		batchId,
		documentId);
}

bool DedupDb::containsUlTmpHash(
		uint64 sessionId,
		uint64 batchId,
		const QByteArray &hash) const {
	return _impl->containsTmpHash(
		Impl::TmpTable::Ul,
		sessionId,
		batchId,
		hash);
}

QByteArray DedupDb::hashForUlTmpDocId(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId) const {
	return _impl->hashForTmpDocId(
		Impl::TmpTable::Ul,
		sessionId,
		batchId,
		documentId);
}

void DedupDb::clearUlTmpRun(uint64 sessionId, uint64 batchId) {
	_impl->clearTmpRun(Impl::TmpTable::Ul, sessionId, batchId);
}

void DedupDb::insertFwTmp(
		uint64 sessionId,
		PeerId dstPeer,
		uint64 itemId,
		const QByteArray &hash) {
	_impl->insertTmp(
		Impl::TmpTable::Fw,
		sessionId,
		dstPeer.value,
		itemId,
		hash);
}

bool DedupDb::containsFwTmpItem(
		uint64 sessionId,
		PeerId dstPeer,
		uint64 itemId) const {
	return _impl->containsTmpDocId(
		Impl::TmpTable::Fw,
		sessionId,
		dstPeer.value,
		itemId);
}

bool DedupDb::containsFwTmpHash(
		uint64 sessionId,
		PeerId dstPeer,
		const QByteArray &hash) const {
	return _impl->containsTmpHash(
		Impl::TmpTable::Fw,
		sessionId,
		dstPeer.value,
		hash);
}

QByteArray DedupDb::hashForFwTmpItem(
		uint64 sessionId,
		PeerId dstPeer,
		uint64 itemId) const {
	return _impl->hashForTmpDocId(
		Impl::TmpTable::Fw,
		sessionId,
		dstPeer.value,
		itemId);
}

void DedupDb::clearFwTmpRun(uint64 sessionId, PeerId dstPeer) {
	_impl->clearTmpRun(Impl::TmpTable::Fw, sessionId, dstPeer.value);
}

void DedupDb::writeChatScanRow(const ChatScanRow &row) {
	_impl->writeChatScanRow(row);
}

std::vector<ChatScanRow> DedupDb::readChatScan(PeerId peerId) const {
	return _impl->readChatScan(peerId);
}

bool DedupDb::isChatScanned(PeerId peerId) const {
	return _impl->isChatScanned(peerId);
}

void DedupDb::markChatDeleteFinished(PeerId peerId) {
	_impl->markChatDeleteFinished(peerId);
}

int64 DedupDb::chatScanDupBytes(PeerId peerId) const {
	return _impl->chatScanDupBytes(peerId);
}

std::vector<ChatScanCategoryTotals> DedupDb::chatScanCategoryTotals(
		PeerId peerId) const {
	return _impl->chatScanCategoryTotals(peerId);
}

qint64 DedupDb::chatScanItemCount(PeerId peerId) const {
	return _impl->chatScanItemCount(peerId);
}

MsgId DedupDb::chatScanMaxMsgId(PeerId peerId) const {
	return _impl->chatScanMaxMsgId(peerId);
}

bool DedupDb::chatScanContainsHash(
		PeerId peerId,
		const QByteArray &hash) const {
	return _impl->chatScanContainsHash(peerId, hash);
}

QByteArray DedupDb::chatScanHashForMedia(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto) const {
	return _impl->chatScanHashForMedia(peerId, mediaId, isPhoto);
}

std::vector<ChatScanItem> DedupDb::chatScanUnresolvedItems(
		PeerId peerId) const {
	return _impl->chatScanUnresolvedItems(peerId);
}

std::vector<ChatScanItem> DedupDb::chatScanPendingItemsWindow(
		PeerId peerId,
		MsgId above,
		int limit) const {
	return _impl->chatScanPendingItemsWindow(peerId, above, limit);
}

std::vector<ChatScanItem> DedupDb::chatScanItemsWindow(
		PeerId peerId,
		MsgId above,
		int limit) const {
	return _impl->chatScanItemsWindow(peerId, above, limit);
}

std::vector<ChatScanItem> DedupDb::chatScanHashedItemsWindow(
		PeerId peerId,
		MsgId above,
		int limit) const {
	return _impl->chatScanHashedItemsWindow(peerId, above, limit);
}

int DedupDb::chatScanPendingCount(PeerId peerId) const {
	return _impl->chatScanPendingCount(peerId);
}

int DedupDb::chatScanHashedCount(PeerId peerId) const {
	return _impl->chatScanHashedCount(peerId);
}

void DedupDb::chatScanResetLeases(PeerId peerId) {
	_impl->chatScanResetLeases(peerId);
}

std::vector<ChatScanItem> DedupDb::chatScanLeaseItems(
		PeerId peerId,
		int limit,
		qint64 now,
		qint64 leaseUntil) {
	return _impl->chatScanLeaseItems(peerId, limit, now, leaseUntil);
}

void DedupDb::chatScanReleaseItem(
		PeerId peerId,
		uint64 mediaId,
		qint64 retryAt) {
	_impl->chatScanReleaseItem(peerId, mediaId, retryAt);
}

void DedupDb::chatScanReleaseLeases(PeerId peerId) {
	_impl->chatScanReleaseLeases(peerId);
}

int DedupDb::chatScanCommitHashes(
		PeerId peerId,
		const std::vector<std::pair<uint64, QByteArray>> &hashes) {
	return _impl->chatScanCommitHashes(peerId, hashes);
}

void DedupDb::chatScanFailItem(PeerId peerId, uint64 mediaId) {
	_impl->chatScanFailItem(peerId, mediaId);
}

int DedupDb::chatScanRetryFailed(PeerId peerId) {
	return _impl->chatScanRetryFailed(peerId);
}

int DedupDb::chatScanLeasedCount(PeerId peerId) const {
	return _impl->chatScanLeasedCount(peerId);
}

int DedupDb::chatScanFailedCount(PeerId peerId) const {
	return _impl->chatScanFailedCount(peerId);
}

std::vector<ChatScanProgressRow> DedupDb::readChatScanProgress(
		PeerId peerId) const {
	return _impl->readChatScanProgress(peerId);
}

void DedupDb::updateChatScanProgress(
		PeerId peerId,
		int category,
		int64 itemCount,
		int64 uniqueCount,
		int64 uniqueBytes,
		int64 dupCount,
		int64 dupBytes) {
	_impl->updateChatScanProgress(
		peerId,
		category,
		itemCount,
		uniqueCount,
		uniqueBytes,
		dupCount,
		dupBytes);
}

std::vector<ChatScanProgressRow> DedupDb::recomputeChatScanProgress(
		PeerId peerId) {
	return _impl->recomputeChatScanProgress(peerId);
}

std::vector<ChatScanItem> DedupDb::chatScanDistinctItems(
		PeerId peerId) const {
	return _impl->chatScanDistinctItems(peerId);
}

void DedupDb::writeGlobal(const QString &key, const QString &value) {
	_impl->writeGlobal(key, value);
}

QString DedupDb::readGlobal(const QString &key) const {
	return _impl->readGlobal(key);
}

void DedupDb::writeChatScanDup(const ChatScanDup &dup) {
	_impl->writeChatScanDup(dup);
}

std::vector<ChatScanDup> DedupDb::readChatScanDups(
		PeerId peerId,
		int category,
		int limit,
		int offset) const {
	return _impl->readChatScanDups(peerId, category, limit, offset);
}

std::vector<ChatScanDup> DedupDb::readChatScanDupsBefore(
		PeerId peerId,
		int category,
		int limit,
		MsgId before) const {
	return _impl->readChatScanDupsBefore(peerId, category, limit, before);
}

int DedupDb::chatScanDupCount(PeerId peerId, int category) const {
	return _impl->chatScanDupCount(peerId, category);
}

void DedupDb::removeChatScanDup(PeerId peerId, MsgId msgId) {
	_impl->removeChatScanDup(peerId, msgId);
}

void DedupDb::clearChatScan(PeerId peerId) {
	_impl->clearChatScan(peerId);
}

void DedupDb::writeScanCursor(const ChatScanState &state) {
	_impl->writeScanCursor(state);
}

std::optional<ChatScanState> DedupDb::readScanCursor(PeerId peerId) const {
	return _impl->readScanCursor(peerId);
}

void DedupDb::writeChatScanItems(const std::vector<ChatScanItem> &items) {
	_impl->writeChatScanItems(items);
}

std::vector<ChatScanItem> DedupDb::readChatScanItems(PeerId peerId) const {
	return _impl->readChatScanItems(peerId);
}

int DedupDb::updateChatScanItemHash(
		PeerId peerId,
		uint64 mediaId,
		const QByteArray &hash) {
	return _impl->updateChatScanItemHash(peerId, mediaId, hash);
}

std::vector<PeerId> DedupDb::scannedPeerIds() const {
	return _impl->scannedPeerIds();
}

std::optional<ChatScanDup> DedupDb::takeChatScanDup(
		PeerId peerId,
		MsgId msgId) {
	return _impl->takeChatScanDup(peerId, msgId);
}

void DedupDb::removeChatScanItem(PeerId peerId, MsgId msgId) {
	_impl->removeChatScanItem(peerId, msgId);
}

bool DedupDb::hasChatScanItem(PeerId peerId, MsgId msgId) const {
	return _impl->hasChatScanItem(peerId, msgId);
}

std::optional<ChatScanItem> DedupDb::chatScanItem(
		PeerId peerId,
		MsgId msgId) const {
	return _impl->chatScanItem(peerId, msgId);
}

void DedupDb::clearChatScanDups(PeerId peerId) {
	_impl->clearChatScanDups(peerId);
}

void DedupDb::beginTransaction() {
	_impl->beginTransaction();
}

void DedupDb::commitTransaction() {
	_impl->commitTransaction();
}

} // namespace Data