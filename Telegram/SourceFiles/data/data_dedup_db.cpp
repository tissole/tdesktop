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
#include <QVariant>
#include <QUuid>

#include <map>
#include <memory>
#include <mutex>
#include <tuple>
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
	void removeExResume(uint64 sessionId, PeerId peerId);
	void clearExResume(uint64 sessionId);
	[[nodiscard]] std::vector<ExResumeRecord> loadExResume(
		uint64 sessionId) const;

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
	void flushExPending();

	bool createTables();

	QString _connectionName;
	QSqlDatabase _db;
	bool _open = false;
	std::shared_ptr<SharedDownloads> _shared;
	mutable std::map<ExTmpKey, ExTmpState> _exTmpStates;
	int _exPendingWrites = 0;
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
			"remaining BLOB NOT NULL DEFAULT x'', "
			"PRIMARY KEY (session_id, dest_peer_id))"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS ex_resume ("
			"session_id INTEGER NOT NULL DEFAULT 0, "
			"peer_id INTEGER NOT NULL, "
			"last_msg_id INTEGER NOT NULL DEFAULT 0, "
			"total INTEGER NOT NULL DEFAULT 0, "
			"done INTEGER NOT NULL DEFAULT 0, "
			"skipped INTEGER NOT NULL DEFAULT 0, "
			"path TEXT NOT NULL DEFAULT '', "
			"state TEXT NOT NULL DEFAULT 'running', "
			"PRIMARY KEY (session_id, peer_id))"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS ex_tmp ("
			"session_id INTEGER NOT NULL DEFAULT 0, "
			"peer_id INTEGER NOT NULL, "
			"doc_id INTEGER NOT NULL, "
			"hash BLOB NOT NULL, "
			"status TEXT NOT NULL DEFAULT 'f', "
			"PRIMARY KEY (session_id, peer_id, doc_id))"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_ex_tmp_hash "
			"ON ex_tmp(session_id, peer_id, hash)"_q);
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
		"(session_id, peer_id, msg_id, path, file_size) "
		"VALUES (:session_id, :peer_id, :msg_id, :path, :file_size)"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.sessionId)));
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.peerId)));
	q.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.msgId)));
	q.bindValue(u":path"_q, record.path);
	q.bindValue(u":file_size"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.fileSize)));
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
		"path, file_size FROM dl_resume"_q)) {
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
		"last_msg_id, state, remaining) "
		"VALUES (:session_id, :dest_peer_id, :src_peer_id, :total, :done, "
		":skipped, :last_msg_id, :state, :remaining)"_q);
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
		"last_msg_id, state, remaining FROM nf_resume "
		"WHERE session_id = :session_id"_q);
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
		const auto raw = q.value(7).toByteArray();
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
		"(session_id, peer_id, last_msg_id, total, done, skipped, "
		"path, state) "
		"VALUES (:session_id, :peer_id, :last_msg_id, :total, :done, "
		":skipped, :path, :state)"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.sessionId)));
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.peerId.value)));
	q.bindValue(u":last_msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.lastMsgId.bare)));
	q.bindValue(u":total"_q, record.total);
	q.bindValue(u":done"_q, record.done);
	q.bindValue(u":skipped"_q, record.skipped);
	q.bindValue(u":path"_q, record.path);
	q.bindValue(u":state"_q, record.state);
	if (!q.exec()) {
		LOG(("DedupDb: InsertExResume failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::removeExResume(uint64 sessionId, PeerId peerId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM ex_resume "
		"WHERE session_id = :session_id AND peer_id = :peer_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: RemoveExResume failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::clearExResume(uint64 sessionId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM ex_resume WHERE session_id = :session_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: ClearExResume failed: %1").arg(
			q.lastError().text()));
	}
}

std::vector<ExResumeRecord> DedupDb::Impl::loadExResume(
		uint64 sessionId) const {
	auto result = std::vector<ExResumeRecord>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT peer_id, last_msg_id, total, done, skipped, "
		"path, state FROM ex_resume "
		"WHERE session_id = :session_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: LoadExResume failed: %1").arg(q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto record = ExResumeRecord();
		record.sessionId = sessionId;
		record.peerId = PeerId(q.value(0).toULongLong());
		record.lastMsgId = MsgId(q.value(1).toLongLong());
		record.total = q.value(2).toInt();
		record.done = q.value(3).toInt();
		record.skipped = q.value(4).toInt();
		record.path = q.value(5).toString();
		record.state = q.value(6).toString();
		result.push_back(std::move(record));
	}
	return result;
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

void DedupDb::removeExResume(uint64 sessionId, PeerId peerId) {
	_impl->removeExResume(sessionId, peerId);
}

void DedupDb::clearExResume(uint64 sessionId) {
	_impl->clearExResume(sessionId);
}

std::vector<ExResumeRecord> DedupDb::loadExResume(uint64 sessionId) const {
	return _impl->loadExResume(sessionId);
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

void DedupDb::beginTransaction() {
	_impl->beginTransaction();
}

void DedupDb::commitTransaction() {
	_impl->commitTransaction();
}

} // namespace Data