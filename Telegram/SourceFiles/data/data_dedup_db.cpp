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

} // namespace

class DedupDb::Impl {
public:
	explicit Impl(const QString &path);
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

	void insertResumeDl(const ResumeDlRecord &record);
	void removeResumeDl(uint64 sessionId, uint64 peerId, int64 msgId);
	void clearResumeDl();
	[[nodiscard]] std::vector<ResumeDlRecord> loadAllResumeDl() const;

	void insertResumeUl(const ResumeUlRecord &record);
	void removeResumeUl(
		uint64 sessionId,
		uint64 peerId,
		const QString &path);
	void clearResumeUl(uint64 sessionId);
	[[nodiscard]] std::vector<ResumeUlRecord> loadAllResumeUl(
		uint64 sessionId) const;

	void insertEfResumeItem(const EfResumeItem &item);
	void removeEfResumeItem(const QString &jobId, int itemIndex);
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
	[[nodiscard]] std::vector<EfResumeItem> loadFinishedEfResumeItems(
		uint64 sessionId) const;
	void clearDoneEfResumeForPeer(PeerId peerId);

	void insertNfResume(const NfResumeRecord &record);
	void removeNfResume(uint64 sessionId, PeerId destPeerId);
	void clearNfResume(uint64 sessionId);
	[[nodiscard]] std::vector<NfResumeRecord> loadNfResume(
		uint64 sessionId) const;

	void insertForwardedDone(
		const FullMsgId &sourceId,
		const QByteArray &hash);
	[[nodiscard]] std::vector<FullMsgId> loadForwardedDone() const;
	void removeForwardedDone(const FullMsgId &sourceId);
	void clearForwardedDone();

	void saveLastBatchCounts(int done, int total);
	[[nodiscard]] std::pair<int, int> loadLastBatchCounts() const;

	void beginTransaction();
	void commitTransaction();

private:
	struct TableState {
		QHash<uint64, QByteArray> docToHash;
		QHash<QByteArray, QSet<uint64>> hashToDoc;
		QSet<uint64> unfinishedDocs;
		bool loaded = false;
	};

	[[nodiscard]] TableState &state(Table table);
	[[nodiscard]] const TableState &state(Table table) const;
	void ensureLoaded(Table table) const;

	bool createTables();

	QString _connectionName;
	QSqlDatabase _db;
	bool _open = false;
	mutable TableState _state[2];
};

DedupDb::Impl::Impl(const QString &path)
: _connectionName(QUuid::createUuid().toString()) {
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
	_open = createTables();
	if (_open) {
		// 'u' rows can only survive a crash; at startup nothing is in flight.
		QSqlQuery cleanup(_db);
		if (!cleanup.exec(u"DELETE FROM dedup_dl WHERE status = 'u'"_q)
			|| !cleanup.exec(u"DELETE FROM dedup_ul WHERE status = 'u'"_q)) {
			LOG(("DedupDb: Failed to purge stale pending rows: %1").arg(
				cleanup.lastError().text()));
		}
	}
}

DedupDb::Impl::~Impl() {
	if (_db.isOpen()) {
		_db.close();
	}
	QSqlDatabase::removeDatabase(_connectionName);
}

DedupDb::Impl::TableState &DedupDb::Impl::state(Table table) {
	ensureLoaded(table);
	return _state[int(table)];
}

const DedupDb::Impl::TableState &DedupDb::Impl::state(Table table) const {
	ensureLoaded(table);
	return _state[int(table)];
}

void DedupDb::Impl::ensureLoaded(Table table) const {
	auto &s = _state[int(table)];
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
		&& exec(u"CREATE TABLE IF NOT EXISTS resume_dl ("
			"session_id INTEGER NOT NULL DEFAULT 0, "
			"peer_id INTEGER NOT NULL, "
			"msg_id INTEGER NOT NULL, "
			"path TEXT NOT NULL, "
			"file_size INTEGER NOT NULL DEFAULT 0, "
			"PRIMARY KEY (peer_id, msg_id))"_q)
		&& [] (QSqlDatabase &db) {
			QSqlQuery q(db);
			q.exec(u"ALTER TABLE resume_dl ADD COLUMN file_size INTEGER NOT NULL DEFAULT 0"_q);
			return true;
		}(_db)
		&& exec(u"CREATE TABLE IF NOT EXISTS resume_ul ("
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
			"PRIMARY KEY (job_id, item_index))"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_ef_resume_peer "
			"ON ef_resume(dest_peer_id, state)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_ef_resume_hash "
			"ON ef_resume(file_hash)"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS ef_done ("
			"source_peer_id INTEGER NOT NULL, "
			"source_msg_id INTEGER NOT NULL, "
			"hash BLOB NOT NULL DEFAULT '', "
			"created_at INTEGER NOT NULL DEFAULT 0, "
			"PRIMARY KEY (source_peer_id, source_msg_id))"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS ef_last ("
			"id INTEGER PRIMARY KEY CHECK (id = 1), "
			"done INTEGER NOT NULL, "
			"total INTEGER NOT NULL)"_q)
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
			"PRIMARY KEY (session_id, dest_peer_id))"_q);
	return created;
}

void DedupDb::Impl::insert(Table table, const DedupRecord &record) {
	auto &s = state(table);
	if (record.documentId && !record.hash.isEmpty()) {
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
	auto &s = state(table);
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
	const auto &s = state(table);
	return s.docToHash.contains(documentId);
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
	const auto &s = state(table);
	return s.hashToDoc.contains(hash);
}

bool DedupDb::Impl::containsFinishedHash(
		Table table,
		const QByteArray &hash) const {
	if (!_open || hash.isEmpty()) {
		return false;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT EXISTS(SELECT 1 FROM " + TableName(table)
		+ u" WHERE hash = :hash AND status = 'f')"_q);
	q.bindValue(u":hash"_q, hash);
	if (!q.exec() || !q.next()) {
		return false;
	}
	return q.value(0).toBool();
}

void DedupDb::Impl::rekey(
		Table table,
		uint64 oldDocumentId,
		uint64 newDocumentId) {
	if (!oldDocumentId || !newDocumentId || oldDocumentId == newDocumentId) {
		return;
	}
	auto &s = state(table);
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
	auto &s = state(table);
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
	auto &s = state(table);
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
	const auto &s = state(table);
	return s.docToHash.value(documentId);
}

uint64 DedupDb::Impl::seekDocumentId(
		Table table,
		const QByteArray &hash,
		uint64 excludeDocumentId) const {
	const auto &s = state(table);
	const auto it = s.hashToDoc.find(hash);
	if (it == s.hashToDoc.end()) {
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
	auto &s = state(table);
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

void DedupDb::Impl::insertResumeDl(const ResumeDlRecord &record) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO resume_dl "
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
		LOG(("DedupDb: InsertResumeDl failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::removeResumeDl(
		uint64 sessionId,
		uint64 peerId,
		int64 msgId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM resume_dl "
		"WHERE peer_id = :peer_id AND msg_id = :msg_id "
		"AND session_id = :session_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId)));
	q.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(msgId)));
	if (!q.exec()) {
		LOG(("DedupDb: RemoveResumeDl failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::clearResumeDl() {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	if (!q.exec(u"DELETE FROM resume_dl"_q)) {
		LOG(("DedupDb: ClearResumeDl failed: %1").arg(q.lastError().text()));
	}
}

std::vector<ResumeDlRecord> DedupDb::Impl::loadAllResumeDl() const {
	auto result = std::vector<ResumeDlRecord>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	if (!q.exec(u"SELECT session_id, peer_id, msg_id, "
		"path, file_size FROM resume_dl"_q)) {
		LOG(("DedupDb: LoadAllResumeDl failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto record = ResumeDlRecord();
		record.sessionId = q.value(0).toULongLong();
		record.peerId = q.value(1).toULongLong();
		record.msgId = q.value(2).toLongLong();
		record.path = q.value(3).toString();
		record.fileSize = q.value(4).toLongLong();
		result.push_back(std::move(record));
	}
	return result;
}

void DedupDb::Impl::insertResumeUl(const ResumeUlRecord &record) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO resume_ul "
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
		LOG(("DedupDb: InsertResumeUl failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::removeResumeUl(
		uint64 sessionId,
		uint64 peerId,
		const QString &path) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM resume_ul "
		"WHERE peer_id = :peer_id AND path = :path "
		"AND session_id = :session_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId)));
	q.bindValue(u":path"_q, path);
	if (!q.exec()) {
		LOG(("DedupDb: RemoveResumeUl failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::clearResumeUl(uint64 sessionId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM resume_ul WHERE session_id = :session_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: ClearResumeUl failed: %1").arg(q.lastError().text()));
	}
}

std::vector<ResumeUlRecord> DedupDb::Impl::loadAllResumeUl(
		uint64 sessionId) const {
	auto result = std::vector<ResumeUlRecord>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT session_id, peer_id, path, parts_sent, sent_size, "
		"file_id, topic_root_id FROM resume_ul "
		"WHERE session_id = :session_id"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: LoadAllResumeUl failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto record = ResumeUlRecord();
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
		"file_hash, media_id, file_size) "
		"VALUES (:session_id, :job_id, :item_index, :dest_peer_id, :source_peer_id, "
		":source_msg_id, :state, :local_path, :file_id, :uploaded_parts, "
		":file_hash, :media_id, :file_size)"_q);
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
		"media_id, file_size FROM ef_resume "
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
		"file_hash, media_id, file_size FROM ef_resume "
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
		result.push_back(std::move(item));
	}
	return result;
}

std::vector<EfResumeItem> DedupDb::Impl::loadFinishedEfResumeItems(
		uint64 sessionId) const {
	auto result = std::vector<EfResumeItem>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT job_id, item_index, dest_peer_id, source_peer_id, "
		"source_msg_id, state, local_path, file_id, uploaded_parts, "
		"file_hash, media_id, file_size FROM ef_resume "
		"WHERE state = 'done' AND session_id = :session_id ORDER BY item_index"_q);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: LoadFinishedEfResumeItems failed: %1").arg(
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
		result.push_back(std::move(item));
	}
	return result;
}

void DedupDb::Impl::clearDoneEfResumeForPeer(PeerId peerId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM ef_resume "
		"WHERE dest_peer_id = :dest_peer_id AND state = 'done'"_q);
	q.bindValue(u":dest_peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ClearDoneEfResumeForPeer failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::insertForwardedDone(
		const FullMsgId &sourceId,
		const QByteArray &hash) {
	if (!_open || !sourceId.peer || !sourceId.msg) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR IGNORE INTO ef_done "
		"(source_peer_id, source_msg_id, hash, created_at) "
		"VALUES (:peer_id, :msg_id, :hash, :created_at)"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sourceId.peer.value)));
	q.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(sourceId.msg.bare)));
	q.bindValue(u":hash"_q, hash);
	q.bindValue(u":created_at"_q, QVariant::fromValue(
		static_cast<qlonglong>(crl::now())));
	if (!q.exec()) {
		LOG(("DedupDb: InsertForwardedDone failed: %1").arg(
			q.lastError().text()));
	}
}

std::vector<FullMsgId> DedupDb::Impl::loadForwardedDone() const {
	auto result = std::vector<FullMsgId>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT source_peer_id, source_msg_id FROM ef_done "
		"ORDER BY created_at"_q);
	if (!q.exec()) {
		LOG(("DedupDb: LoadForwardedDone failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		result.emplace_back(
			PeerId(q.value(0).toULongLong()),
			MsgId(q.value(1).toLongLong()));
	}
	return result;
}

void DedupDb::Impl::removeForwardedDone(const FullMsgId &sourceId) {
	if (!_open || !sourceId.peer || !sourceId.msg) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM ef_done "
		"WHERE source_peer_id = :peer_id AND source_msg_id = :msg_id"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sourceId.peer.value)));
	q.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(sourceId.msg.bare)));
	if (!q.exec()) {
		LOG(("DedupDb: RemoveForwardedDone failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::clearForwardedDone() {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	if (!q.exec(u"DELETE FROM ef_done"_q)) {
		LOG(("DedupDb: ClearForwardedDone failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::saveLastBatchCounts(int done, int total) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO ef_last (id, done, total) "
		"VALUES (1, :done, :total)"_q);
	q.bindValue(u":done"_q, done);
	q.bindValue(u":total"_q, total);
	if (!q.exec()) {
		LOG(("DedupDb: SaveLastBatchCounts failed: %1").arg(
			q.lastError().text()));
	}
}

std::pair<int, int> DedupDb::Impl::loadLastBatchCounts() const {
	if (!_open) {
		return { 0, 0 };
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT done, total FROM ef_last WHERE id = 1"_q);
	if (!q.exec() || !q.next()) {
		return { 0, 0 };
	}
	return { q.value(0).toInt(), q.value(1).toInt() };
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

DedupDb::DedupDb(const QString &path)
: _impl(std::make_unique<Impl>(path)) {
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

void DedupDb::insertResumeDl(const ResumeDlRecord &record) {
	_impl->insertResumeDl(record);
}

void DedupDb::removeResumeDl(
		uint64 sessionId,
		uint64 peerId,
		int64 msgId) {
	_impl->removeResumeDl(sessionId, peerId, msgId);
}

void DedupDb::clearResumeDl() {
	_impl->clearResumeDl();
}

std::vector<ResumeDlRecord> DedupDb::loadAllResumeDl() const {
	return _impl->loadAllResumeDl();
}

void DedupDb::insertResumeUl(const ResumeUlRecord &record) {
	_impl->insertResumeUl(record);
}

void DedupDb::removeResumeUl(
		uint64 sessionId,
		uint64 peerId,
		const QString &path) {
	_impl->removeResumeUl(sessionId, peerId, path);
}

void DedupDb::clearResumeUl(uint64 sessionId) {
	_impl->clearResumeUl(sessionId);
}

std::vector<ResumeUlRecord> DedupDb::loadAllResumeUl(
		uint64 sessionId) const {
	return _impl->loadAllResumeUl(sessionId);
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

std::vector<EfResumeItem> DedupDb::loadFinishedEfResumeItems(
		uint64 sessionId) const {
	return _impl->loadFinishedEfResumeItems(sessionId);
}

void DedupDb::clearDoneEfResumeForPeer(PeerId peerId) {
	_impl->clearDoneEfResumeForPeer(peerId);
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

void DedupDb::insertForwardedDone(
		const FullMsgId &sourceId,
		const QByteArray &hash) {
	_impl->insertForwardedDone(sourceId, hash);
}

std::vector<FullMsgId> DedupDb::loadForwardedDone() const {
	return _impl->loadForwardedDone();
}

void DedupDb::removeForwardedDone(const FullMsgId &sourceId) {
	_impl->removeForwardedDone(sourceId);
}

void DedupDb::clearForwardedDone() {
	_impl->clearForwardedDone();
}

void DedupDb::saveLastBatchCounts(int done, int total) {
	_impl->saveLastBatchCounts(done, total);
}

std::pair<int, int> DedupDb::loadLastBatchCounts() const {
	return _impl->loadLastBatchCounts();
}

void DedupDb::beginTransaction() {
	_impl->beginTransaction();
}

void DedupDb::commitTransaction() {
	_impl->commitTransaction();
}

} // namespace Data