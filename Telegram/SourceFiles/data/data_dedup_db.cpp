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

	void insertResumeDl(const ResumeDlRecord &record);
	void removeResumeDl(uint64 peerId, int64 msgId);
	void removeResumeDlByDocumentId(uint64 documentId);
	void clearResumeDl();
	[[nodiscard]] std::vector<ResumeDlRecord> loadAllResumeDl() const;

	void insertResumeUl(const ResumeUlRecord &record);
	void removeResumeUl(uint64 peerId, const QString &path);
	void clearResumeUl();
	[[nodiscard]] std::vector<ResumeUlRecord> loadAllResumeUl() const;

	void beginTransaction();
	void commitTransaction();

private:
	[[nodiscard]] bool createTables();

	QString _connectionName;
	QSqlDatabase _db;
	bool _open = false;
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
	return exec(u"CREATE TABLE IF NOT EXISTS dedup_dl ("
		"hash BLOB NOT NULL, "
		"size INTEGER NOT NULL, "
		"doc_id INTEGER NOT NULL DEFAULT 0, "
		"PRIMARY KEY (hash, size))"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS dedup_ul ("
			"hash BLOB NOT NULL, "
			"size INTEGER NOT NULL, "
			"doc_id INTEGER NOT NULL DEFAULT 0, "
			"PRIMARY KEY (hash, size))"_q)
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
			"PRIMARY KEY (peer_id, path))"_q);
}

void DedupDb::Impl::insert(Table table, const DedupRecord &record) {
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO " + TableName(table)
		+ u" (hash, size, doc_id) VALUES (:hash, :size, :doc_id)"_q);
	q.bindValue(u":hash"_q, record.hash);
	q.bindValue(u":size"_q, QVariant::fromValue(record.size));
	q.bindValue(u":doc_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.documentId)));
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
		+ u" WHERE hash = :hash AND size = :size"_q);
	q.bindValue(u":hash"_q, hash);
	q.bindValue(u":size"_q, QVariant::fromValue(size));
	if (!q.exec()) {
		return false;
	}
	return q.next();
}

bool DedupDb::Impl::containsDocId(Table table, uint64 documentId) const {
	if (!_open) {
		return false;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT 1 FROM " + TableName(table)
		+ u" WHERE doc_id = :doc_id"_q);
	q.bindValue(u":doc_id"_q, QVariant::fromValue(
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
	if (!q.exec(u"SELECT hash, size, doc_id FROM " + TableName(table))) {
		LOG(("DedupDb: LoadAll failed: %1").arg(q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto record = DedupRecord();
		record.hash = q.value(0).toByteArray();
		record.size = q.value(1).toLongLong();
		record.documentId = q.value(2).toULongLong();
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

void DedupDb::beginTransaction() {
	_impl->beginTransaction();
}

void DedupDb::commitTransaction() {
	_impl->commitTransaction();
}

} // namespace Data
