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
#include <QVariant>
#include <QUuid>

#include "logs.h"

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
	void remove(Table table, const QByteArray &hash, int64 size);
	[[nodiscard]] bool contains(
		Table table,
		const QByteArray &hash,
		int64 size) const;
	[[nodiscard]] bool containsDocId(
		Table table,
		uint64 documentId) const;
	[[nodiscard]] uint64 findDocumentId(
		Table table,
		const QByteArray &hash,
		int64 size) const;
	[[nodiscard]] std::vector<DedupRecord> loadAll(Table table) const;

	void insertIdMapping(uint64 mediaId, const QByteArray &hash);
	[[nodiscard]] bool containsIdMapping(uint64 mediaId) const;
	[[nodiscard]] bool containsSize(Table table, int64 size) const;
	void updateDedupStatus(
		Table table,
		const QByteArray &hash,
		int64 size,
		const QString &status);
	void removeByDocumentId(
		Table table,
		uint64 documentId,
		const QString &status);

	void insertResumeDl(const ResumeDlRecord &record);
	void removeResumeDl(uint64 peerId, int64 msgId);
	void removeResumeDlByDocumentId(uint64 documentId);
	void clearResumeDl();
	[[nodiscard]] std::vector<ResumeDlRecord> loadAllResumeDl() const;

	void insertResumeUl(const ResumeUlRecord &record);
	void removeResumeUl(uint64 peerId, const QString &path);
	void clearResumeUl();
	[[nodiscard]] std::vector<ResumeUlRecord> loadAllResumeUl() const;

	void insertEfResumeItem(const EfResumeItem &item);
	void removeEfResumeItem(const QString &jobId, int itemIndex);
	void clearEfResumeForPeer(PeerId peerId);
	void clearEfResumeJob(const QString &jobId);
	[[nodiscard]] std::vector<EfResumeItem> loadEfResumeItemsForPeer(
		PeerId peerId) const;
	[[nodiscard]] std::vector<EfResumeItem> loadUnfinishedEfResumeItems() const;

	[[nodiscard]] std::optional<DedupRecord> findUploadDuplicateByHash(
		const QByteArray &hash,
		int64 size,
		uint64 excludeDocumentId = 0) const;

	void addPending(Table table, uint64 documentId, int64 size);
	void updatePendingHash(
		Table table,
		uint64 documentId,
		const QByteArray &hash);
	void removePending(Table table, uint64 documentId);

	void beginTransaction();
	void commitTransaction();

private:
	struct PendingEntry {
		uint64 documentId = 0;
		int64 size = 0;
		QByteArray hash;
	};

	[[nodiscard]] std::vector<PendingEntry> &pendingList(Table table);
	[[nodiscard]] const std::vector<PendingEntry> &pendingList(
		Table table) const;

	[[nodiscard]] bool createTables();

	QString _connectionName;
	QSqlDatabase _db;
	bool _open = false;
	std::vector<PendingEntry> _pending[2];
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
}

DedupDb::Impl::~Impl() {
	if (_db.isOpen()) {
		_db.close();
	}
	QSqlDatabase::removeDatabase(_connectionName);
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
	const auto addColumnIfMissing = [&](
			const QString &table,
			const QString &column,
			const QString &definition) {
		QSqlQuery check(_db);
		check.prepare(u"PRAGMA table_info('" + table + u"')"_q);
		if (!check.exec()) {
			return true;
		}
		while (check.next()) {
			if (check.value(1).toString() == column) {
				return true;
			}
		}
		return exec(u"ALTER TABLE " + table + u" ADD COLUMN "
			+ column + u" " + definition);
	};
	return exec(u"CREATE TABLE IF NOT EXISTS dedup_dl ("
		"hash BLOB NOT NULL, "
		"size INTEGER NOT NULL, "
		"doc_id INTEGER NOT NULL DEFAULT 0, "
		"status TEXT NOT NULL DEFAULT 'f', "
		"PRIMARY KEY (hash, size))"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS dedup_ul ("
			"hash BLOB NOT NULL, "
			"size INTEGER NOT NULL, "
			"doc_id INTEGER NOT NULL DEFAULT 0, "
			"status TEXT NOT NULL DEFAULT 'f', "
			"PRIMARY KEY (hash, size))"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS dedup_ids ("
			"media_id INTEGER PRIMARY KEY, "
			"hash BLOB NOT NULL)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_dedup_dl_doc_id "
			"ON dedup_dl(doc_id)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_dedup_ul_doc_id "
			"ON dedup_ul(doc_id)"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS resume_dl ("
			"peer_id INTEGER NOT NULL, "
			"peer_access_hash INTEGER NOT NULL DEFAULT 0, "
			"msg_id INTEGER NOT NULL, "
			"document_id INTEGER NOT NULL, "
			"size INTEGER NOT NULL, "
			"path TEXT NOT NULL, "
			"PRIMARY KEY (peer_id, msg_id))"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS resume_ul ("
			"peer_id INTEGER NOT NULL, "
			"path TEXT NOT NULL, "
			"size INTEGER NOT NULL, "
			"parts_sent INTEGER NOT NULL DEFAULT 0, "
			"sent_size INTEGER NOT NULL DEFAULT 0, "
			"file_id INTEGER NOT NULL DEFAULT 0, "
			"topic_root_id INTEGER NOT NULL DEFAULT 0, "
			"PRIMARY KEY (peer_id, path))"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS ef_resume ("
			"job_id TEXT NOT NULL, "
			"item_index INTEGER NOT NULL, "
			"peer_id INTEGER NOT NULL, "
			"source_peer_id INTEGER NOT NULL, "
			"source_msg_id INTEGER NOT NULL, "
			"state TEXT NOT NULL, "
			"local_path TEXT NOT NULL DEFAULT '', "
			"file_id INTEGER NOT NULL DEFAULT 0, "
			"uploaded_parts INTEGER NOT NULL DEFAULT 0, "
			"file_size INTEGER NOT NULL DEFAULT 0, "
			"file_hash BLOB, "
			"media_id INTEGER NOT NULL DEFAULT 0, "
			"created_at INTEGER NOT NULL DEFAULT 0, "
			"updated_at INTEGER NOT NULL DEFAULT 0, "
			"PRIMARY KEY (job_id, item_index))"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_ef_resume_peer "
			"ON ef_resume(peer_id, state)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_ef_resume_hash "
			"ON ef_resume(file_hash)"_q)
		&& addColumnIfMissing(u"ef_resume"_q, u"media_id"_q,
			u"INTEGER NOT NULL DEFAULT 0"_q);
}

void DedupDb::Impl::insert(Table table, const DedupRecord &record) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO " + TableName(table)
		+ u" (hash, size, doc_id, status) "
		"VALUES (:hash, :size, :doc_id, :status)"_q);
	q.bindValue(u":hash"_q, record.hash);
	q.bindValue(u":size"_q, QVariant::fromValue(record.size));
	q.bindValue(u":doc_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.documentId)));
	q.bindValue(u":status"_q, record.status);
	if (!q.exec()) {
		LOG(("DedupDb: Insert failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::remove(Table table, const QByteArray &hash, int64 size) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM " + TableName(table)
		+ u" WHERE hash = :hash AND size = :size"_q);
	q.bindValue(u":hash"_q, hash);
	q.bindValue(u":size"_q, QVariant::fromValue(size));
	if (!q.exec()) {
		LOG(("DedupDb: Remove failed: %1").arg(q.lastError().text()));
	}
}

bool DedupDb::Impl::contains(
		Table table,
		const QByteArray &hash,
		int64 size) const {
	if (!_open) {
		return false;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT 1 FROM " + TableName(table)
		+ u" WHERE hash = :hash AND size = :size AND status = 'f'"_q);
	q.bindValue(u":hash"_q, hash);
	q.bindValue(u":size"_q, QVariant::fromValue(size));
	if (!q.exec()) {
		return false;
	}
	return q.next();
}

bool DedupDb::Impl::containsDocId(Table table, uint64 documentId) const {
	for (const auto &entry : pendingList(table)) {
		if (entry.documentId == documentId) {
			return true;
		}
	}
	if (!_open) {
		return false;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT 1 FROM " + TableName(table)
		+ u" WHERE doc_id = :doc_id "
		"UNION ALL "
		"SELECT 1 FROM dedup_ids WHERE media_id = :media_id "
		"LIMIT 1"_q);
	q.bindValue(u":doc_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(documentId)));
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(documentId)));
	if (!q.exec()) {
		return false;
	}
	return q.next();
}

uint64 DedupDb::Impl::findDocumentId(
		Table table,
		const QByteArray &hash,
		int64 size) const {
	if (!_open) {
		return 0;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT doc_id FROM " + TableName(table)
		+ u" WHERE hash = :hash AND size = :size"_q);
	q.bindValue(u":hash"_q, hash);
	q.bindValue(u":size"_q, QVariant::fromValue(size));
	if (!q.exec() || !q.next()) {
		return 0;
	}
	return q.value(0).toULongLong();
}

std::vector<DedupRecord> DedupDb::Impl::loadAll(Table table) const {
	auto result = std::vector<DedupRecord>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	if (!q.exec(u"SELECT hash, size, doc_id, status FROM " + TableName(table))) {
		LOG(("DedupDb: LoadAll failed: %1").arg(q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto record = DedupRecord();
		record.hash = q.value(0).toByteArray();
		record.size = q.value(1).toLongLong();
		record.documentId = q.value(2).toULongLong();
		record.status = q.value(3).toString();
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
		"(peer_id, peer_access_hash, msg_id, document_id, size, path) "
		"VALUES (:peer_id, :peer_access_hash, :msg_id, :document_id, :size, "
		":path)"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.peerId)));
	q.bindValue(u":peer_access_hash"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.peerAccessHash)));
	q.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.msgId)));
	q.bindValue(u":document_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.documentId)));
	q.bindValue(u":size"_q, QVariant::fromValue(record.size));
	q.bindValue(u":path"_q, record.path);
	if (!q.exec()) {
		LOG(("DedupDb: InsertResumeDl failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::removeResumeDl(uint64 peerId, int64 msgId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM resume_dl "
		"WHERE peer_id = :peer_id AND msg_id = :msg_id"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId)));
	q.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(msgId)));
	if (!q.exec()) {
		LOG(("DedupDb: RemoveResumeDl failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::removeResumeDlByDocumentId(uint64 documentId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM resume_dl WHERE document_id = :document_id"_q);
	q.bindValue(u":document_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(documentId)));
	if (!q.exec()) {
		LOG(("DedupDb: RemoveResumeDlByDocumentId failed: %1").arg(
			q.lastError().text()));
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
	if (!q.exec(u"SELECT peer_id, peer_access_hash, msg_id, document_id, "
		"size, path FROM resume_dl"_q)) {
		LOG(("DedupDb: LoadAllResumeDl failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto record = ResumeDlRecord();
		record.peerId = q.value(0).toULongLong();
		record.peerAccessHash = q.value(1).toULongLong();
		record.msgId = q.value(2).toLongLong();
		record.documentId = q.value(3).toULongLong();
		record.size = q.value(4).toLongLong();
		record.path = q.value(5).toString();
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
		"(peer_id, path, size, parts_sent, sent_size, file_id, "
		"topic_root_id) "
		"VALUES (:peer_id, :path, :size, :parts_sent, :sent_size, :file_id, "
		":topic_root_id)"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.peerId)));
	q.bindValue(u":path"_q, record.path);
	q.bindValue(u":size"_q, QVariant::fromValue(record.size));
	q.bindValue(u":parts_sent"_q, record.partsSent);
	q.bindValue(u":sent_size"_q, QVariant::fromValue(record.sentSize));
	q.bindValue(u":file_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.fileId)));
	q.bindValue(u":topic_root_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.topicRootId)));
	if (!q.exec()) {
		LOG(("DedupDb: InsertResumeUl failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::removeResumeUl(uint64 peerId, const QString &path) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM resume_ul "
		"WHERE peer_id = :peer_id AND path = :path"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId)));
	q.bindValue(u":path"_q, path);
	if (!q.exec()) {
		LOG(("DedupDb: RemoveResumeUl failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::clearResumeUl() {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	if (!q.exec(u"DELETE FROM resume_ul"_q)) {
		LOG(("DedupDb: ClearResumeUl failed: %1").arg(q.lastError().text()));
	}
}

std::vector<ResumeUlRecord> DedupDb::Impl::loadAllResumeUl() const {
	auto result = std::vector<ResumeUlRecord>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	if (!q.exec(u"SELECT peer_id, path, size, parts_sent, sent_size, "
		"file_id, topic_root_id FROM resume_ul"_q)) {
		LOG(("DedupDb: LoadAllResumeUl failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto record = ResumeUlRecord();
		record.peerId = q.value(0).toULongLong();
		record.path = q.value(1).toString();
		record.size = q.value(2).toLongLong();
		record.partsSent = q.value(3).toInt();
		record.sentSize = q.value(4).toLongLong();
		record.fileId = q.value(5).toULongLong();
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
		"(job_id, item_index, peer_id, source_peer_id, source_msg_id, state, "
		"local_path, file_id, uploaded_parts, file_size, file_hash, media_id, "
		"created_at, updated_at) "
		"VALUES (:job_id, :item_index, :peer_id, :source_peer_id, "
		":source_msg_id, :state, :local_path, :file_id, :uploaded_parts, "
		":file_size, :file_hash, :media_id, :created_at, :updated_at)"_q);
	q.bindValue(u":job_id"_q, item.jobId);
	q.bindValue(u":item_index"_q, item.itemIndex);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(item.peerId.value)));
	q.bindValue(u":source_peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(item.sourceId.peer.value)));
	q.bindValue(u":source_msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(item.sourceId.msg.bare)));
	q.bindValue(u":state"_q, item.state);
	q.bindValue(u":local_path"_q, item.localPath);
	q.bindValue(u":file_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(item.fileId)));
	q.bindValue(u":uploaded_parts"_q, item.uploadedParts);
	q.bindValue(u":file_size"_q, QVariant::fromValue(item.fileSize));
	q.bindValue(u":file_hash"_q, item.fileHash);
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(item.mediaId)));
	q.bindValue(u":created_at"_q, QVariant::fromValue(item.createdAt));
	q.bindValue(u":updated_at"_q, QVariant::fromValue(item.updatedAt));
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

void DedupDb::Impl::clearEfResumeForPeer(PeerId peerId) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM ef_resume WHERE peer_id = :peer_id"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
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
		PeerId peerId) const {
	auto result = std::vector<EfResumeItem>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT job_id, item_index, source_peer_id, source_msg_id, "
		"state, local_path, file_id, uploaded_parts, file_size, file_hash, "
		"media_id, created_at, updated_at FROM ef_resume WHERE peer_id = :peer_id "
		"ORDER BY item_index"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
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
		item.fileId = q.value(6).toULongLong();
		item.uploadedParts = q.value(7).toInt();
		item.fileSize = q.value(8).toLongLong();
		item.fileHash = q.value(9).toByteArray();
		item.mediaId = q.value(10).toULongLong();
		item.createdAt = q.value(11).toLongLong();
		item.updatedAt = q.value(12).toLongLong();
		result.push_back(std::move(item));
	}
	return result;
}

std::vector<EfResumeItem> DedupDb::Impl::loadUnfinishedEfResumeItems() const {
	auto result = std::vector<EfResumeItem>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	if (!q.exec(u"SELECT job_id, item_index, peer_id, source_peer_id, "
		"source_msg_id, state, local_path, file_id, uploaded_parts, "
		"file_size, file_hash, media_id, created_at, updated_at FROM ef_resume "
		"WHERE state <> 'done' ORDER BY item_index"_q)) {
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
		item.fileId = q.value(7).toULongLong();
		item.uploadedParts = q.value(8).toInt();
		item.fileSize = q.value(9).toLongLong();
		item.fileHash = q.value(10).toByteArray();
		item.mediaId = q.value(11).toULongLong();
		item.createdAt = q.value(12).toLongLong();
		item.updatedAt = q.value(13).toLongLong();
		result.push_back(std::move(item));
	}
	return result;
}

std::optional<DedupRecord> DedupDb::Impl::findUploadDuplicateByHash(
		const QByteArray &hash,
		int64 size,
		uint64 excludeDocumentId) const {
	if (hash.isEmpty()) {
		return std::nullopt;
	}
	for (const auto &entry : pendingList(Table::Uploads)) {
		if (entry.hash == hash
			&& entry.size == size
			&& entry.documentId != excludeDocumentId) {
			return DedupRecord{ hash, size, entry.documentId, u"u"_q };
		}
	}
	if (!_open) {
		return std::nullopt;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT hash, size, doc_id FROM dedup_ul "
		"WHERE hash = :hash AND size = :size "
		"AND doc_id != :exclude"_q);
	q.bindValue(u":hash"_q, hash);
	q.bindValue(u":size"_q, QVariant::fromValue(size));
	q.bindValue(u":exclude"_q, QVariant::fromValue(
		static_cast<qulonglong>(excludeDocumentId)));
	if (!q.exec() || !q.next()) {
		return std::nullopt;
	}
	auto record = DedupRecord();
	record.hash = q.value(0).toByteArray();
	record.size = q.value(1).toLongLong();
	record.documentId = q.value(2).toULongLong();
	return record;
}

std::vector<DedupDb::Impl::PendingEntry> &DedupDb::Impl::pendingList(
		Table table) {
	return _pending[int(table)];
}

const std::vector<DedupDb::Impl::PendingEntry> &DedupDb::Impl::pendingList(
		Table table) const {
	return _pending[int(table)];
}

void DedupDb::Impl::addPending(
		Table table,
		uint64 documentId,
		int64 size) {
	if (!documentId) {
		return;
	}
	auto &list = pendingList(table);
	for (auto &entry : list) {
		if (entry.documentId == documentId) {
			entry.size = size;
			return;
		}
	}
	list.push_back({ documentId, size, {} });
}

void DedupDb::Impl::updatePendingHash(
		Table table,
		uint64 documentId,
		const QByteArray &hash) {
	for (auto &entry : pendingList(table)) {
		if (entry.documentId == documentId) {
			entry.hash = hash;
			return;
		}
	}
}

void DedupDb::Impl::removePending(Table table, uint64 documentId) {
	auto &list = pendingList(table);
	list.erase(std::remove_if(list.begin(), list.end(), [&](PendingEntry &entry) {
		return entry.documentId == documentId;
	}), list.end());
}

void DedupDb::Impl::insertIdMapping(uint64 mediaId, const QByteArray &hash) {
	if (!_open || hash.isEmpty()) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO dedup_ids (media_id, hash) "
		"VALUES (:media_id, :hash)"_q);
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(mediaId)));
	q.bindValue(u":hash"_q, hash);
	if (!q.exec()) {
		LOG(("DedupDb: InsertIdMapping failed: %1").arg(q.lastError().text()));
	}
}

bool DedupDb::Impl::containsIdMapping(uint64 mediaId) const {
	if (!_open) {
		return false;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT 1 FROM dedup_ids WHERE media_id = :media_id"_q);
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(mediaId)));
	if (!q.exec()) {
		return false;
	}
	return q.next();
}

bool DedupDb::Impl::containsSize(Table table, int64 size) const {
	for (const auto &entry : pendingList(table)) {
		if (entry.size == size) {
			return true;
		}
	}
	if (!_open) {
		return false;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT 1 FROM " + TableName(table)
		+ u" WHERE size = :size LIMIT 1"_q);
	q.bindValue(u":size"_q, QVariant::fromValue(size));
	if (!q.exec()) {
		return false;
	}
	return q.next();
}

void DedupDb::Impl::updateDedupStatus(
		Table table,
		const QByteArray &hash,
		int64 size,
		const QString &status) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE " + TableName(table)
		+ u" SET status = :status "
		"WHERE hash = :hash AND size = :size"_q);
	q.bindValue(u":status"_q, status);
	q.bindValue(u":hash"_q, hash);
	q.bindValue(u":size"_q, QVariant::fromValue(size));
	if (!q.exec()) {
		LOG(("DedupDb: UpdateDedupStatus failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::removeByDocumentId(
		Table table,
		uint64 documentId,
		const QString &status) {
	if (!_open) {
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
		static_cast<qulonglong>(documentId)));
	if (!q.exec()) {
		LOG(("DedupDb: RemoveByDocumentId failed: %1").arg(
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

void DedupDb::remove(Table table, const QByteArray &hash, int64 size) {
	_impl->remove(table, hash, size);
}

bool DedupDb::contains(Table table, const QByteArray &hash, int64 size) const {
	return _impl->contains(table, hash, size);
}

bool DedupDb::containsDocId(Table table, uint64 documentId) const {
	return _impl->containsDocId(table, documentId);
}

uint64 DedupDb::findDocumentId(
		Table table,
		const QByteArray &hash,
		int64 size) const {
	return _impl->findDocumentId(table, hash, size);
}

std::vector<DedupRecord> DedupDb::loadAll(Table table) const {
	return _impl->loadAll(table);
}

void DedupDb::insertResumeDl(const ResumeDlRecord &record) {
	_impl->insertResumeDl(record);
}

void DedupDb::removeResumeDl(uint64 peerId, int64 msgId) {
	_impl->removeResumeDl(peerId, msgId);
}

void DedupDb::removeResumeDlByDocumentId(uint64 documentId) {
	_impl->removeResumeDlByDocumentId(documentId);
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

void DedupDb::removeResumeUl(uint64 peerId, const QString &path) {
	_impl->removeResumeUl(peerId, path);
}

void DedupDb::clearResumeUl() {
	_impl->clearResumeUl();
}

std::vector<ResumeUlRecord> DedupDb::loadAllResumeUl() const {
	return _impl->loadAllResumeUl();
}

void DedupDb::insertEfResumeItem(const EfResumeItem &item) {
	_impl->insertEfResumeItem(item);
}

void DedupDb::removeEfResumeItem(const QString &jobId, int itemIndex) {
	_impl->removeEfResumeItem(jobId, itemIndex);
}

void DedupDb::clearEfResumeForPeer(PeerId peerId) {
	_impl->clearEfResumeForPeer(peerId);
}

void DedupDb::clearEfResumeJob(const QString &jobId) {
	_impl->clearEfResumeJob(jobId);
}

std::vector<EfResumeItem> DedupDb::loadEfResumeItemsForPeer(
		PeerId peerId) const {
	return _impl->loadEfResumeItemsForPeer(peerId);
}

std::vector<EfResumeItem> DedupDb::loadUnfinishedEfResumeItems() const {
	return _impl->loadUnfinishedEfResumeItems();
}

std::optional<DedupRecord> DedupDb::findUploadDuplicateByHash(
		const QByteArray &hash,
		int64 size,
		uint64 excludeDocumentId) const {
	return _impl->findUploadDuplicateByHash(
		hash,
		size,
		excludeDocumentId);
}

void DedupDb::addPending(Table table, uint64 documentId, int64 size) {
	_impl->addPending(table, documentId, size);
}

void DedupDb::updatePendingHash(
		Table table,
		uint64 documentId,
		const QByteArray &hash) {
	_impl->updatePendingHash(table, documentId, hash);
}

void DedupDb::removePending(Table table, uint64 documentId) {
	_impl->removePending(table, documentId);
}

void DedupDb::beginTransaction() {
	_impl->beginTransaction();
}

void DedupDb::commitTransaction() {
	_impl->commitTransaction();
}

void DedupDb::insertIdMapping(uint64 mediaId, const QByteArray &hash) {
	_impl->insertIdMapping(mediaId, hash);
}

bool DedupDb::containsIdMapping(uint64 mediaId) const {
	return _impl->containsIdMapping(mediaId);
}

bool DedupDb::containsSize(Table table, int64 size) const {
	return _impl->containsSize(table, size);
}

void DedupDb::updateDedupStatus(
		Table table,
		const QByteArray &hash,
		int64 size,
		const QString &status) {
	_impl->updateDedupStatus(table, hash, size, status);
}

void DedupDb::removeByDocumentId(
		Table table,
		uint64 documentId,
		const QString &status) {
	_impl->removeByDocumentId(table, documentId, status);
}

} // namespace Data
