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
#include <QDataStream>
#include <QIODevice>
#include <QSet>
#include <QStringList>
#include <QVariant>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "logs.h"
#include "base/unixtime.h"
#include <crl/crl.h>

namespace Data {

namespace {

constexpr auto kVacuumChunk = 1000;
constexpr auto kVacuumMinFree = 1000;
constexpr auto kVacuumFraction = 10;

std::atomic_bool gVacuuming = false;

[[nodiscard]] QString TableName(DedupDb::Table table) {
	switch (table) {
	case DedupDb::Table::Downloads: return u"dedup_dl"_q;
	case DedupDb::Table::Uploads: return u"dedup_ul"_q;
	}
	Unexpected("DedupDb::Table");
}

[[nodiscard]] int64 PragmaInt(QSqlDatabase &db, const QString &sql) {
	QSqlQuery q(db);
	return (q.exec(sql) && q.next()) ? q.value(0).toLongLong() : -1;
}

[[nodiscard]] bool ScanIsActive(QSqlDatabase &db) {
	QSqlQuery q(db);
	return q.exec(u"SELECT 1 FROM jobs WHERE kind = 'scan' "
		"AND state != 2 LIMIT 1"_q) && q.next();
}

void VacuumOpenedDatabase(
		QSqlDatabase &db,
		const std::atomic_bool &cancel) {
	{
		QSqlQuery q(db);
		if (!q.exec(u"PRAGMA busy_timeout=5000"_q)) {
			return;
		}
	}
	if (PragmaInt(db, u"PRAGMA auto_vacuum"_q) != 1) {
		return;
	}
	if (ScanIsActive(db)) {
		return;
	}
	const auto free = PragmaInt(db, u"PRAGMA freelist_count"_q);
	const auto pages = PragmaInt(db, u"PRAGMA page_count"_q);
	if (free < kVacuumMinFree
		|| pages <= 0
		|| free * kVacuumFraction < pages) {
		return;
	}
	LOG(("DedupDb: Vacuum start, %1 free of %2 pages.").arg(
		free,
		pages));
	auto remaining = free;
	auto reclaimed = int64(0);
	while (remaining > 0 && !cancel.load()) {
		{
			QSqlQuery vacuum(db);
			if (!vacuum.exec(u"PRAGMA incremental_vacuum(%1)"_q.arg(
					kVacuumChunk))) {
				LOG(("DedupDb: Incremental vacuum failed: %1").arg(
					vacuum.lastError().text()));
				break;
			}
		}
		const auto next = PragmaInt(db, u"PRAGMA freelist_count"_q);
		if (next < 0 || next >= remaining) {
			break;
		}
		reclaimed += remaining - next;
		remaining = next;
	}
	LOG(("DedupDb: Vacuum reclaimed %1 pages, %2 free left.").arg(
		reclaimed,
		remaining));
}

void OptimizeDatabase(const QString &path) {
	const auto connectionName = u"dedup-optimize-%1"_q.arg(
		QUuid::createUuid().toString());
	auto db = QSqlDatabase::addDatabase(u"QSQLITE"_q, connectionName);
	db.setDatabaseName(path);
	if (db.open()) {
		{
			QSqlQuery q(db);
			if (!q.exec(u"PRAGMA busy_timeout=5000"_q)
				|| !q.exec(u"PRAGMA optimize"_q)) {
				LOG(("DedupDb: Startup optimize failed: %1").arg(
					q.lastError().text()));
			}
		}
		db.close();
	} else {
		LOG(("DedupDb: Startup optimize open failed: %1").arg(
			db.lastError().text()));
	}
	db = QSqlDatabase();
	QSqlDatabase::removeDatabase(connectionName);
}

void VacuumDatabase(
		const QString &path,
		const std::atomic_bool &cancel) {
	if (cancel.load()) {
		return;
	}
	const auto connectionName = u"dedup-vacuum-%1"_q.arg(
		QUuid::createUuid().toString());
	auto db = QSqlDatabase::addDatabase(u"QSQLITE"_q, connectionName);
	db.setDatabaseName(path);
	if (db.open()) {
		VacuumOpenedDatabase(db, cancel);
		db.close();
	} else {
		LOG(("DedupDb: Vacuum open failed: %1").arg(
			db.lastError().text()));
	}
	db = QSqlDatabase();
	QSqlDatabase::removeDatabase(connectionName);
}

struct VacuumFlagGuard {
	~VacuumFlagGuard() {
		gVacuuming = false;
	}
};

struct TableState {
	QHash<uint64, QByteArray> docToHash[2];
	QHash<QByteArray, QSet<uint64>> hashToDoc[2];
	QSet<uint64> unfinishedDocs[2];
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
	int tableUsers[2] = {};
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

void RecomputeAllChatScanRefsIn(QSqlDatabase &db) {
	const auto exec = [&](const QString &sql) {
		auto q = QSqlQuery(db);
		if (!q.exec(sql)) {
			LOG(("DedupDb: Recompute refs failed: %1: %2").arg(
				sql,
				q.lastError().text()));
		}
	};
	exec(u"DELETE FROM media_refs"_q);
	exec(u"INSERT INTO media_refs "
		"(media_id, is_photo, hash_id, id_refs) "
		"SELECT media_id, is_photo, COALESCE(MAX(hash_id), 0), COUNT(*) "
		"FROM chat_scan_items WHERE media_id != 0 "
		"GROUP BY media_id, is_photo"_q);
	exec(u"UPDATE hashes SET hash_refs = "
		"(SELECT COUNT(*) FROM chat_scan_items "
		"WHERE chat_scan_items.hash_id = hashes.hash_id)"_q);
}

[[nodiscard]] std::optional<HashOwner> FindHashOwnerReplacementIn(
		const QSqlDatabase &db,
		uint64 hashId,
		PeerId excludePeer,
		MsgId excludeMsg) {
	if (!hashId) {
		return std::nullopt;
	}
	auto sql = u"SELECT i.peer_id, i.msg_id, i.tracked "
		"FROM chat_scan_items i "
		"WHERE i.hash_id = :hash_id "_q;
	if (excludeMsg.bare > 0) {
		sql += u"AND NOT (i.peer_id = :exclude_peer "
			"AND i.msg_id = :exclude_msg) "_q;
	} else {
		sql += u"AND i.peer_id != :exclude_peer "_q;
	}
	sql += u"ORDER BY i.tracked DESC, "
		"(i.peer_id = :old_peer) DESC, "
		"i.peer_id ASC, i.msg_id DESC LIMIT 1"_q;
	QSqlQuery q(db);
	q.prepare(sql);
	q.bindValue(u":hash_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(hashId)));
	q.bindValue(u":exclude_peer"_q, QVariant::fromValue(
		static_cast<qulonglong>(excludePeer.value)));
	q.bindValue(u":old_peer"_q, QVariant::fromValue(
		static_cast<qulonglong>(excludePeer.value)));
	if (excludeMsg.bare > 0) {
		q.bindValue(u":exclude_msg"_q, QVariant::fromValue(
			static_cast<qlonglong>(excludeMsg.bare)));
	}
	if (!q.exec()) {
		LOG(("DedupDb: HashOwner replacement failed: %1").arg(
			q.lastError().text()));
		return std::nullopt;
	}
	if (!q.next()) {
		return std::nullopt;
	}
	auto result = HashOwner();
	result.peerId = PeerId(q.value(0).toULongLong());
	result.msgId = MsgId(q.value(1).toLongLong());
	result.tracked = (q.value(2).toInt() != 0);
	result.exists = bool(result.peerId && result.msgId);
	return result.exists ? std::optional<HashOwner>(result) : std::nullopt;
}

[[nodiscard]] std::vector<ChatScanOwnerChange> CleanupOwnersForPeerIn(
		QSqlDatabase &db,
		PeerId peerId) {
	auto result = std::vector<ChatScanOwnerChange>();
	if (!peerId) {
		return result;
	}
	struct OwnedHash {
		uint64 hashId = 0;
		QByteArray hash;
		MsgId msgId;
	};
	auto owned = std::vector<OwnedHash>();
	{
		QSqlQuery q(db);
		q.prepare(u"SELECT hash_id, hash, owner_msg_id FROM hashes "
			"WHERE owner_peer_id = :peer_id"_q);
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		if (!q.exec()) {
			LOG(("DedupDb: Owner cleanup read failed: %1").arg(
				q.lastError().text()));
			return result;
		}
		while (q.next()) {
			owned.push_back({
				.hashId = q.value(0).toULongLong(),
				.hash = q.value(1).toByteArray(),
				.msgId = MsgId(q.value(2).toLongLong()),
			});
		}
	}
	for (const auto &entry : owned) {
		const auto replacement = FindHashOwnerReplacementIn(
			db,
			entry.hashId,
			peerId,
			MsgId(0));
		if (replacement) {
			{
				QSqlQuery q(db);
				q.prepare(u"UPDATE hashes SET owner_peer_id = :peer_id, "
					"owner_msg_id = :msg_id WHERE hash_id = :hash_id"_q);
				q.bindValue(u":peer_id"_q, QVariant::fromValue(
					static_cast<qulonglong>(replacement->peerId.value)));
				q.bindValue(u":msg_id"_q, QVariant::fromValue(
					static_cast<qlonglong>(replacement->msgId.bare)));
				q.bindValue(u":hash_id"_q, QVariant::fromValue(
					static_cast<qulonglong>(entry.hashId)));
				if (!q.exec()) {
					LOG(("DedupDb: Owner cleanup promote failed: %1").arg(
						q.lastError().text()));
				}
			}
			{
				QSqlQuery q(db);
				q.prepare(u"UPDATE chat_scan_items SET is_dup = 0, "
					"filename = NULL WHERE peer_id = :peer_id "
					"AND msg_id = :msg_id"_q);
				q.bindValue(u":peer_id"_q, QVariant::fromValue(
					static_cast<qulonglong>(replacement->peerId.value)));
				q.bindValue(u":msg_id"_q, QVariant::fromValue(
					static_cast<qlonglong>(replacement->msgId.bare)));
				if (!q.exec()) {
					LOG(("DedupDb: Owner cleanup row promote failed: %1")
						.arg(q.lastError().text()));
				}
			}
			result.emplace_back(replacement->peerId, entry.hash);
		} else {
			QSqlQuery q(db);
			q.prepare(u"UPDATE hashes SET owner_peer_id = 0, "
				"owner_msg_id = 0 WHERE hash_id = :hash_id"_q);
			q.bindValue(u":hash_id"_q, QVariant::fromValue(
				static_cast<qulonglong>(entry.hashId)));
			if (!q.exec()) {
				LOG(("DedupDb: Owner cleanup clear failed: %1").arg(
					q.lastError().text()));
			}
		}
		{
			QSqlQuery q(db);
			q.prepare(u"UPDATE chat_scan_items SET is_dup = 1 "
				"WHERE peer_id = :peer_id AND msg_id = :msg_id"_q);
			q.bindValue(u":peer_id"_q, QVariant::fromValue(
				static_cast<qulonglong>(peerId.value)));
			q.bindValue(u":msg_id"_q, QVariant::fromValue(
				static_cast<qlonglong>(entry.msgId.bare)));
			if (!q.exec()) {
				LOG(("DedupDb: Owner cleanup demote failed: %1").arg(
					q.lastError().text()));
			}
		}
	}
	return result;
}

constexpr auto kScanJobSerializationVersion = quint32(1);

// Mirrors ChatScan::State::Phase; the data layer only needs the terminal
// value to persist a completed scan.
constexpr auto kScanPhaseFinished = 3;

struct ScanJobOptions {
	int dedupMode = -1;
	MsgId cursor;
	int phase = 0;
	int64 elapsedMs = 0;
	MsgId scannedUpto;
	bool scanFinished = false;
	bool deleteFinished = false;
	uint64 sessionId = 0;
	MsgId baseMark;
	bool oldStateExists = false;
	MsgId oldCursor;
	int oldPhase = 0;
	int64 oldElapsedMs = 0;
	int oldDedupMode = 0;
};

[[nodiscard]] QByteArray SerializeScanJob(const ScanJobOptions &options) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream << kScanJobSerializationVersion;
	stream << qint32(options.dedupMode);
	stream << qint64(options.cursor.bare);
	stream << qint32(options.phase);
	stream << qint64(options.elapsedMs);
	stream << qint64(options.scannedUpto.bare);
	stream << quint8(options.scanFinished ? 1 : 0);
	stream << quint8(options.deleteFinished ? 1 : 0);
	stream << quint64(options.sessionId);
	stream << qint64(options.baseMark.bare);
	stream << quint8(options.oldStateExists ? 1 : 0);
	stream << qint64(options.oldCursor.bare);
	stream << qint32(options.oldPhase);
	stream << qint64(options.oldElapsedMs);
	stream << qint32(options.oldDedupMode);
	return result;
}

[[nodiscard]] std::optional<ScanJobOptions> DeserializeScanJob(
		const QByteArray &data) {
	auto stream = QDataStream(data);
	auto version = quint32();
	stream >> version;
	if (version != kScanJobSerializationVersion) {
		return std::nullopt;
	}
	auto result = ScanJobOptions();
	auto dedupMode = qint32();
	auto cursor = qint64();
	auto phase = qint32();
	auto elapsedMs = qint64();
	auto scannedUpto = qint64();
	auto scanFinished = quint8();
	auto deleteFinished = quint8();
	auto sessionId = quint64();
	auto baseMark = qint64();
	auto oldStateExists = quint8();
	auto oldCursor = qint64();
	auto oldPhase = qint32();
	auto oldElapsedMs = qint64();
	auto oldDedupMode = qint32();
	stream >> dedupMode >> cursor >> phase >> elapsedMs
		>> scannedUpto >> scanFinished >> deleteFinished
		>> sessionId >> baseMark >> oldStateExists
		>> oldCursor >> oldPhase >> oldElapsedMs >> oldDedupMode;
	if (stream.status() != QDataStream::Ok) {
		return std::nullopt;
	}
	result.dedupMode = dedupMode;
	result.cursor = MsgId(cursor);
	result.phase = phase;
	result.elapsedMs = elapsedMs;
	result.scannedUpto = MsgId(scannedUpto);
	result.scanFinished = (scanFinished != 0);
	result.deleteFinished = (deleteFinished != 0);
	result.sessionId = sessionId;
	result.baseMark = MsgId(baseMark);
	result.oldStateExists = (oldStateExists != 0);
	result.oldCursor = MsgId(oldCursor);
	result.oldPhase = oldPhase;
	result.oldElapsedMs = oldElapsedMs;
	result.oldDedupMode = oldDedupMode;
	return result;
}

[[nodiscard]] QByteArray SerializeScanUndo(
		bool isDup,
		const QString &filename) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream << quint8(isDup ? 1 : 0);
	stream << filename;
	return result;
}

[[nodiscard]] std::optional<std::pair<bool, QString>> DeserializeScanUndo(
		const QByteArray &data) {
	auto stream = QDataStream(data);
	auto isDup = quint8();
	auto filename = QString();
	stream >> isDup >> filename;
	if (stream.status() != QDataStream::Ok) {
		return std::nullopt;
	}
	return std::make_pair(isDup != 0, filename);
}

[[nodiscard]] std::optional<ScanJobOptions> ReadScanJobOptionsIn(
		const QSqlDatabase &db,
		PeerId peerId) {
	QSqlQuery q(db);
	q.prepare(u"SELECT options FROM jobs WHERE kind = 'scan' "
		"AND scope_id = :scope_id ORDER BY job_id DESC LIMIT 1"_q);
	q.bindValue(u":scope_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ReadScanJobOptions failed: %1").arg(
			q.lastError().text()));
		return std::nullopt;
	}
	if (!q.next()) {
		return std::nullopt;
	}
	return DeserializeScanJob(q.value(0).toByteArray());
}

void DeleteScanJobIn(QSqlDatabase &db, PeerId peerId) {
	{
		QSqlQuery q(db);
		q.prepare(u"DELETE FROM job_items WHERE job_id IN ("
			"SELECT job_id FROM jobs WHERE kind = 'scan' "
			"AND scope_id = :scope_id)"_q);
		q.bindValue(u":scope_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		if (!q.exec()) {
			LOG(("DedupDb: DeleteScanJob items failed: %1").arg(
				q.lastError().text()));
		}
	}
	{
		QSqlQuery q(db);
		q.prepare(u"DELETE FROM jobs WHERE kind = 'scan' "
			"AND scope_id = :scope_id"_q);
		q.bindValue(u":scope_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		if (!q.exec()) {
			LOG(("DedupDb: DeleteScanJob failed: %1").arg(
				q.lastError().text()));
		}
	}
}

[[nodiscard]] JobState ScanJobState(const ScanJobOptions &options) {
	return (options.scanFinished && options.phase == kScanPhaseFinished)
		? JobState::Done
		: JobState::InFlight;
}

std::vector<ChatScanOwnerChange> ClearChatScanIn(
		QSqlDatabase &db,
		PeerId peerId) {
	const auto started = crl::now();
	const auto peer = QVariant::fromValue(
		static_cast<qulonglong>(peerId.value));
	const auto begin = [&] {
		QSqlQuery q(db);
		if (!q.exec(u"BEGIN IMMEDIATE"_q)) {
			LOG(("DedupDb: ClearChatScan begin failed: %1").arg(
				q.lastError().text()));
		}
	};
	const auto commit = [&] {
		QSqlQuery q(db);
		if (!q.exec(u"COMMIT"_q)) {
			LOG(("DedupDb: ClearChatScan commit failed: %1").arg(
				q.lastError().text()));
		}
	};
	const auto hasOtherPeers = [&](const QString &table) {
		auto q = QSqlQuery(db);
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
		auto q = QSqlQuery(db);
		q.prepare(u"SELECT COUNT(*) FROM %1 "
			"WHERE peer_id = :peer_id"_q.arg(table));
		q.bindValue(u":peer_id"_q, peer);
		return (q.exec() && q.next())
			? q.value(0).toInt()
			: 0;
	};
	const auto deletePeer = [&](const QString &table) {
		auto q = QSqlQuery(db);
		q.prepare(u"DELETE FROM %1 "
			"WHERE peer_id = :peer_id"_q.arg(table));
		q.bindValue(u":peer_id"_q, peer);
		if (!q.exec()) {
			LOG(("DedupDb: ClearChatScan failed: %1").arg(
				q.lastError().text()));
		}
	};
	const auto deleteAll = [&](const QString &table) {
		auto q = QSqlQuery(db);
		if (!q.exec(u"DELETE FROM %1"_q.arg(table))) {
			LOG(("DedupDb: ClearChatScan failed: %1").arg(
				q.lastError().text()));
		}
	};
	const auto dropIndex = [&](const QString &index) {
		auto q = QSqlQuery(db);
		if (!q.exec(u"DROP INDEX IF EXISTS %1"_q.arg(index))) {
			LOG(("DedupDb: ClearChatScan drop index failed: %1").arg(
				q.lastError().text()));
		}
	};
	const auto createIndex = [&](
			const QString &index,
			const QString &table,
			const QString &columns) {
		auto q = QSqlQuery(db);
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
		{ u"chat_scan_items"_q, {
			{
				u"idx_chat_scan_items_media"_q,
				u"(peer_id, media_id, is_photo)"_q,
			},
			{
				u"idx_chat_scan_items_media_global"_q,
				u"(media_id, is_photo, peer_id, is_dup, msg_id)"_q,
			},
			{
				u"idx_chat_scan_items_hash_id"_q,
				u"(peer_id, hash_id)"_q,
			},
			{
				u"idx_chat_scan_items_hash_tracked"_q,
				u"(hash_id, tracked, msg_id)"_q,
			},
			{
				u"idx_chat_scan_items_hash_id_unique_msg"_q,
				u"(hash_id, peer_id, msg_id) WHERE is_dup = 0"_q,
			},
			{
				u"idx_chat_scan_items_dup"_q,
				u"(peer_id, is_dup, msg_id)"_q,
			},
			{
				u"idx_chat_scan_items_dup_category"_q,
				u"(peer_id, is_dup, category, msg_id)"_q,
			},
			{
				u"idx_chat_scan_items_state"_q,
				u"(peer_id, state, media_id)"_q,
			},
			{
				u"idx_chat_scan_items_category"_q,
				u"(peer_id, category)"_q,
			},
			{
				u"idx_chat_scan_items_session"_q,
				u"(peer_id, session_id)"_q,
			},
		} },
	};
	constexpr auto kIndexedClearThreshold = 4096;
	begin();
	auto changed = CleanupOwnersForPeerIn(db, peerId);
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
	DeleteScanJobIn(db, peerId);
	RecomputeAllChatScanRefsIn(db);
	commit();
	LOG(("DedupDb: ChatScan clear took %1 ms.").arg(
		crl::now() - started));
	return changed;
}

std::optional<std::vector<ChatScanProgressRow>> RecomputeChatScanProgressIn(
		QSqlDatabase &db,
		PeerId peerId) {
	auto result = std::vector<ChatScanProgressRow>();
	{
		QSqlQuery q(db);
		q.prepare(u"SELECT category, COUNT(*), SUM(size), "
			"SUM(CASE WHEN is_dup = 1 THEN 1 ELSE 0 END), "
			"SUM(CASE WHEN is_dup = 1 THEN size ELSE 0 END) "
			"FROM chat_scan_items WHERE peer_id = :peer_id "
			"AND tracked = 1 "
			"GROUP BY category"_q);
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		if (!q.exec()) {
			return std::nullopt;
		}
		while (q.next()) {
			auto row = ChatScanProgressRow();
			row.peerId = peerId;
			row.category = q.value(0).toInt();
			row.itemCount = q.value(1).toLongLong();
			const auto totalBytes = q.value(2).toLongLong();
			row.dupCount = q.value(3).toLongLong();
			row.dupBytes = q.value(4).toLongLong();
			row.uniqueCount = std::max(
				int64(0),
				row.itemCount - row.dupCount);
			row.uniqueBytes = std::max(
				int64(0),
				totalBytes - row.dupBytes);
			result.push_back(row);
		}
	}
	return result;
}

std::optional<std::vector<std::pair<QByteArray, MsgId>>>
ReadChatScanUniqueHashesIn(QSqlDatabase &db, PeerId peerId) {
	auto result = std::vector<std::pair<QByteArray, MsgId>>();
	QSqlQuery q(db);
	q.prepare(u"SELECT h.hash, MAX(i.msg_id) "
		"FROM chat_scan_items i "
		"JOIN hashes h ON h.hash_id = i.hash_id "
		"WHERE i.peer_id = :peer_id "
		"AND i.hash_id != 0 "
		"AND i.is_dup = 0 "
		"AND i.tracked = 1 "
		"GROUP BY i.hash_id"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		return std::nullopt;
	}
	while (q.next()) {
		result.emplace_back(
			q.value(0).toByteArray(),
			MsgId(q.value(1).toLongLong()));
	}
	return result;
}

std::optional<std::vector<ChatScanMediaHash>>
ReadChatScanMediaHashesIn(
		QSqlDatabase &db,
		PeerId peerId,
		bool includeOther) {
	auto result = std::vector<ChatScanMediaHash>();
	QSqlQuery q(db);
	auto sql = u"SELECT i.peer_id, i.media_id, i.is_photo, i.is_dup, h.hash "
		"FROM chat_scan_items i "
		"JOIN hashes h ON h.hash_id = i.hash_id "
		"WHERE i.media_id != 0 "
		"AND i.tracked = 1 "
		"AND i.hash_id != 0"_q;
	if (!includeOther) {
		sql += u" AND i.peer_id = :peer_id"_q;
	}
	q.prepare(sql);
	if (!includeOther) {
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
	}
	if (!q.exec()) {
		return std::nullopt;
	}
	while (q.next()) {
		auto row = ChatScanMediaHash();
		row.peerId = PeerId(q.value(0).toULongLong());
		row.mediaId = q.value(1).toULongLong();
		row.isPhoto = (q.value(2).toInt() != 0);
		row.isDup = (q.value(3).toInt() != 0);
		row.hash = q.value(4).toByteArray();
		if (row.peerId && row.mediaId && !row.hash.isEmpty()) {
			result.push_back(std::move(row));
		}
	}
	return result;
}

std::optional<std::vector<MsgId>> ReadChatScanItemIdsIn(
		QSqlDatabase &db,
		PeerId peerId,
		int category) {
	auto result = std::vector<MsgId>();
	QSqlQuery q(db);
	q.prepare(u"SELECT msg_id FROM chat_scan_items "
		"WHERE peer_id = :peer_id AND category = :category "
		"AND tracked = 1 "
		"ORDER BY msg_id DESC"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":category"_q, category);
	if (!q.exec()) {
		return std::nullopt;
	}
	while (q.next()) {
		result.push_back(MsgId(q.value(0).toLongLong()));
	}
	return result;
}

std::optional<std::vector<ChatScanRow>> ReadChatScanIn(
		const QSqlDatabase &db,
		PeerId peerId) {
	auto result = std::vector<ChatScanRow>();
	auto options = ReadScanJobOptionsIn(db, peerId).value_or(ScanJobOptions());
	QSqlQuery q(db);
	q.prepare(u"SELECT category, "
		"SUM(CASE WHEN is_dup = 0 THEN 1 ELSE 0 END), "
		"SUM(CASE WHEN is_dup = 0 THEN size ELSE 0 END), "
		"SUM(CASE WHEN is_dup = 1 THEN 1 ELSE 0 END), "
		"SUM(CASE WHEN is_dup = 1 THEN size ELSE 0 END) "
		"FROM chat_scan_items WHERE peer_id = :peer_id "
		"AND tracked = 1 "
		"GROUP BY category ORDER BY category"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ReadChatScan failed: %1").arg(
			q.lastError().text()));
		return std::nullopt;
	}
	while (q.next()) {
		auto row = ChatScanRow();
		row.peerId = peerId;
		row.category = q.value(0).toInt();
		row.uniqueCount = int(q.value(1).toLongLong());
		row.uniqueBytes = q.value(2).toLongLong();
		row.dupCount = int(q.value(3).toLongLong());
		row.dupBytes = q.value(4).toLongLong();
		row.scannedUpto = options.scannedUpto;
		row.scanFinished = options.scanFinished;
		row.deleteFinished = options.deleteFinished;
		result.push_back(row);
	}
	return result;
}

std::optional<std::vector<ChatScanDup>> ReadChatScanDupsBeforeIn(
		const QSqlDatabase &db,
		PeerId peerId,
		int category,
		int limit,
		MsgId before) {
	if (limit <= 0) {
		return std::vector<ChatScanDup>();
	}
	auto result = std::vector<ChatScanDup>();
	auto sql = u"SELECT msg_id, category, size, media_id, is_photo "
		"FROM chat_scan_items "
		"WHERE peer_id = :peer_id AND is_dup = 1 "
		"AND tracked = 1"_q;
	if (category >= 0) {
		sql += u" AND category = :category"_q;
	}
	if (before.bare > 0) {
		sql += u" AND msg_id < :before"_q;
	}
	sql += u" ORDER BY msg_id DESC LIMIT :limit"_q;
	QSqlQuery q(db);
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
		return std::nullopt;
	}
	while (q.next()) {
		auto dup = ChatScanDup();
		dup.peerId = peerId;
		dup.msgId = MsgId(q.value(0).toLongLong());
		dup.category = q.value(1).toInt();
		dup.size = q.value(2).toLongLong();
		dup.mediaId = q.value(3).toULongLong();
		dup.isPhoto = (q.value(4).toInt() != 0);
		result.push_back(dup);
	}
	return result;
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

[[nodiscard]] QString JobKindName(JobKind kind) {
	switch (kind) {
	case JobKind::Download: return u"download"_q;
	case JobKind::DownloadBatch: return u"download_batch"_q;
	case JobKind::Upload: return u"upload"_q;
	case JobKind::UploadBatch: return u"upload_batch"_q;
	case JobKind::Forward: return u"forward"_q;
	case JobKind::ForwardBatch: return u"forward_batch"_q;
	case JobKind::Export: return u"export"_q;
	case JobKind::ExportBatch: return u"export_batch"_q;
	case JobKind::EnhancedForward: return u"enhanced_forward"_q;
	case JobKind::Scan: return u"scan"_q;
	}
	Unexpected("JobKind");
}

[[nodiscard]] std::optional<JobKind> JobKindFromName(const QString &name) {
	if (name == u"download"_q) {
		return JobKind::Download;
	} else if (name == u"download_batch"_q) {
		return JobKind::DownloadBatch;
	} else if (name == u"upload"_q) {
		return JobKind::Upload;
	} else if (name == u"upload_batch"_q) {
		return JobKind::UploadBatch;
	} else if (name == u"forward"_q) {
		return JobKind::Forward;
	} else if (name == u"forward_batch"_q) {
		return JobKind::ForwardBatch;
	} else if (name == u"export"_q) {
		return JobKind::Export;
	} else if (name == u"export_batch"_q) {
		return JobKind::ExportBatch;
	} else if (name == u"enhanced_forward"_q) {
		return JobKind::EnhancedForward;
	} else if (name == u"scan"_q) {
		return JobKind::Scan;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<JobRecord> ReadJob(const QSqlQuery &q) {
	const auto kind = JobKindFromName(q.value(1).toString());
	if (!kind) {
		return std::nullopt;
	}
	auto result = JobRecord();
	result.jobId = q.value(0).toULongLong();
	result.kind = *kind;
	result.sessionId = q.value(2).toULongLong();
	result.scopeId = q.value(3).toULongLong();
	result.options = q.value(4).toByteArray();
	result.state = JobState(q.value(5).toInt());
	result.total = q.value(6).toInt();
	result.done = q.value(7).toInt();
	result.skipped = q.value(8).toInt();
	result.createdAt = q.value(9).toLongLong();
	result.updatedAt = q.value(10).toLongLong();
	return result;
}

void BindJobItem(QSqlQuery &q, const JobItemRecord &item) {
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(item.jobId)));
	q.bindValue(u":item_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(item.itemId)));
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(item.mediaId)));
	q.bindValue(u":is_photo"_q, item.isPhoto ? 1 : 0);
	q.bindValue(u":hash_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(item.hashId)));
	q.bindValue(u":state"_q, int(item.state));
	q.bindValue(u":payload"_q, item.payload);
	q.bindValue(u":error"_q, item.error);
	q.bindValue(u":retry"_q, item.retry);
}

[[nodiscard]] JobItemRecord ReadJobItem(const QSqlQuery &q) {
	auto result = JobItemRecord();
	result.jobId = q.value(0).toULongLong();
	result.itemId = q.value(1).toULongLong();
	result.mediaId = q.value(2).toULongLong();
	result.isPhoto = (q.value(3).toInt() != 0);
	result.hashId = q.value(4).toULongLong();
	result.state = JobState(q.value(5).toInt());
	result.payload = q.value(6).toByteArray();
	result.error = q.value(7).toString();
	result.retry = q.value(8).toInt();
	return result;
}

constexpr auto kExResumeSerializationVersion = quint32(1);

[[nodiscard]] uint64 ExResumeScope(
		PeerId peerId,
		int mode,
		const QString &folder) {
	auto result = uint64(14695981039346656037ULL);
	const auto mix = [&](uint64 value) {
		for (auto i = 0; i != 8; ++i) {
			result ^= (value >> (i * 8)) & 0xFF;
			result *= 1099511628211ULL;
		}
	};
	mix(peerId.value);
	mix(uint64(mode));
	const auto bytes = folder.toUtf8();
	mix(uint64(bytes.size()));
	for (const auto byte : bytes) {
		result ^= quint8(byte);
		result *= 1099511628211ULL;
	}
	return result & 0x7FFFFFFFFFFFFFFFULL;
}

[[nodiscard]] JobState ExportJobState(const QString &state) {
	if (state == u"done"_q) {
		return JobState::Done;
	} else if (state == u"run"_q) {
		return JobState::InFlight;
	}
	return JobState::Pending;
}

[[nodiscard]] QByteArray SerializeDlResumePayload(
		const DlResumeRecord &record) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream << record.path << qint64(record.fileSize);
	return result;
}

[[nodiscard]] bool ReadDlResumePayload(
		const QByteArray &data,
		DlResumeRecord &record) {
	auto stream = QDataStream(data);
	auto fileSize = qint64();
	stream >> record.path >> fileSize;
	record.fileSize = fileSize;
	return stream.status() == QDataStream::Ok;
}

[[nodiscard]] QByteArray SerializeExResume(const ExResumeRecord &record) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream << kExResumeSerializationVersion;
	stream << quint64(record.sessionId);
	stream << quint64(record.peerId.value);
	stream << qint32(record.mode);
	stream << qint32(record.total);
	stream << qint32(record.msgsDone);
	stream << qint32(record.skipped);
	stream << qint64(record.lastId.bare);
	stream << record.exportFolder;
	stream << record.state;
	stream << quint32(record.media);
	stream << qint64(record.size);
	stream << qint32(record.exportFormat);
	stream << qint32(record.fromDate);
	stream << qint32(record.tillDate);
	stream << qint32(record.htmlIndex);
	stream << record.repliedIndex;
	stream << qint32(record.dateIndex);
	stream << record.stats;
	stream << quint64(record.docId);
	stream << record.pausedFile;
	stream << qint64(record.pausedBytes);
	stream << record.lastMsg;
	stream << qint32(record.splitIndex);
	stream << qint32(record.selectedDone);
	stream << qint32(record.filterIndex);
	stream << quint8(record.useIdRange ? 1 : 0);
	stream << quint64(record.fromId);
	stream << quint64(record.tillId);
	stream << qint64(record.updateAnchor.bare);
	stream << qint64(record.coveredTill.bare);
	stream << qint32(record.coveredTillDate);
	stream << quint8(record.migrated ? 1 : 0);
	stream << qint32(record.failedCount);
	stream << quint64(record.resumePeerId.value);
	stream << quint32(record.dialogIds.size());
	for (const auto peerId : record.dialogIds) {
		stream << quint64(peerId.value);
	}
	stream << qint32(record.chatTypes);
	stream << qint32(record.fullChats);
	stream << qint32(record.extFilterMode);
	stream << record.extFilter;
	return result;
}

[[nodiscard]] std::optional<ExResumeRecord> DeserializeExResume(
		const QByteArray &data) {
	auto stream = QDataStream(data);
	auto version = quint32();
	stream >> version;
	if (version != kExResumeSerializationVersion) {
		return std::nullopt;
	}
	auto result = ExResumeRecord();
	const auto readUInt64 = [&](uint64 &value) {
		auto temp = quint64();
		stream >> temp;
		value = temp;
	};
	const auto readInt64 = [&](int64 &value) {
		auto temp = qint64();
		stream >> temp;
		value = temp;
	};
	const auto readInt = [&](int &value) {
		auto temp = qint32();
		stream >> temp;
		value = temp;
	};
	const auto readBool = [&](bool &value) {
		auto temp = quint8();
		stream >> temp;
		value = (temp != 0);
	};
	readUInt64(result.sessionId);
	auto peerId = quint64();
	stream >> peerId;
	result.peerId = PeerId(peerId);
	readInt(result.mode);
	readInt(result.total);
	readInt(result.msgsDone);
	readInt(result.skipped);
	readInt64(result.lastId.bare);
	stream >> result.exportFolder;
	stream >> result.state;
	auto media = quint32();
	stream >> media;
	result.media = media;
	readInt64(result.size);
	readInt(result.exportFormat);
	readInt(result.fromDate);
	readInt(result.tillDate);
	readInt(result.htmlIndex);
	stream >> result.repliedIndex;
	readInt(result.dateIndex);
	stream >> result.stats;
	readUInt64(result.docId);
	stream >> result.pausedFile;
	readInt64(result.pausedBytes);
	stream >> result.lastMsg;
	readInt(result.splitIndex);
	readInt(result.selectedDone);
	readInt(result.filterIndex);
	readBool(result.useIdRange);
	readUInt64(result.fromId);
	readUInt64(result.tillId);
	readInt64(result.updateAnchor.bare);
	readInt64(result.coveredTill.bare);
	readInt(result.coveredTillDate);
	readBool(result.migrated);
	readInt(result.failedCount);
	readUInt64(result.resumePeerId.value);
	auto dialogCount = quint32();
	stream >> dialogCount;
	result.dialogIds.reserve(dialogCount);
	for (auto i = quint32(0); i != dialogCount; ++i) {
		auto value = quint64();
		stream >> value;
		result.dialogIds.push_back(PeerId(value));
	}
	readInt(result.chatTypes);
	readInt(result.fullChats);
	readInt(result.extFilterMode);
	stream >> result.extFilter;
	if (stream.status() != QDataStream::Ok) {
		return std::nullopt;
	}
	return result;
}

struct EfJobOptions {
	QString jobId;
	PeerId destPeerId;
	PeerId srcPeerId;
	bool paused = false;
};

constexpr auto kUlResumeSerializationVersion = quint32(1);
constexpr auto kNfResumeSerializationVersion = quint32(1);
constexpr auto kEfJobSerializationVersion = quint32(1);
constexpr auto kEfItemSerializationVersion = quint32(1);

[[nodiscard]] uint64 StableStringHash(const QString &value) {
	auto result = uint64(14695981039346656037ULL);
	const auto bytes = value.toUtf8();
	for (const auto byte : bytes) {
		result ^= quint8(byte);
		result *= 1099511628211ULL;
	}
	return result;
}

[[nodiscard]] uint64 UlResumeItemId(const QString &path) {
	return StableStringHash(path) & 0x7FFFFFFFFFFFFFFFULL;
}

[[nodiscard]] QByteArray SerializeUlResume(const UlResumeRecord &record) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream << kUlResumeSerializationVersion;
	stream << quint64(record.sessionId);
	stream << quint64(record.peerId);
	stream << record.path;
	stream << qint32(record.partsSent);
	stream << qint64(record.sentSize);
	stream << quint64(record.fileId);
	stream << qint64(record.topicRootId);
	return result;
}

[[nodiscard]] std::optional<UlResumeRecord> DeserializeUlResume(
		const QByteArray &data) {
	auto stream = QDataStream(data);
	auto version = quint32();
	stream >> version;
	if (version != kUlResumeSerializationVersion) {
		return std::nullopt;
	}
	auto result = UlResumeRecord();
	auto sessionId = quint64();
	auto peerId = quint64();
	auto partsSent = qint32();
	auto sentSize = qint64();
	auto fileId = quint64();
	auto topicRootId = qint64();
	stream >> sessionId >> peerId >> result.path >> partsSent
		>> sentSize >> fileId >> topicRootId;
	if (stream.status() != QDataStream::Ok) {
		return std::nullopt;
	}
	result.sessionId = sessionId;
	result.peerId = peerId;
	result.partsSent = partsSent;
	result.sentSize = sentSize;
	result.fileId = fileId;
	result.topicRootId = topicRootId;
	return result;
}

[[nodiscard]] QByteArray SerializeNfResume(const NfResumeRecord &record) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream << kNfResumeSerializationVersion;
	stream << quint64(record.sessionId);
	stream << quint64(record.destPeerId.value);
	stream << quint64(record.srcPeerId.value);
	stream << qint32(record.total);
	stream << qint32(record.done);
	stream << qint32(record.skipped);
	stream << qint64(record.lastMsgId.bare);
	stream << record.state;
	stream << qint32(record.forwardOptions);
	stream << qint32(record.groupOptions);
	stream << quint32(record.remaining.size());
	for (const auto msgId : record.remaining) {
		stream << qint64(msgId.bare);
	}
	return result;
}

[[nodiscard]] std::optional<NfResumeRecord> DeserializeNfResume(
		const QByteArray &data) {
	auto stream = QDataStream(data);
	auto version = quint32();
	stream >> version;
	if (version != kNfResumeSerializationVersion) {
		return std::nullopt;
	}
	auto result = NfResumeRecord();
	auto sessionId = quint64();
	auto destPeerId = quint64();
	auto srcPeerId = quint64();
	auto total = qint32();
	auto done = qint32();
	auto skipped = qint32();
	auto lastMsgId = qint64();
	auto forwardOptions = qint32();
	auto groupOptions = qint32();
	auto count = quint32();
	stream >> sessionId >> destPeerId >> srcPeerId
		>> total >> done >> skipped >> lastMsgId
		>> result.state >> forwardOptions >> groupOptions >> count;
	if (stream.status() != QDataStream::Ok) {
		return std::nullopt;
	}
	result.sessionId = sessionId;
	result.destPeerId = PeerId(destPeerId);
	result.srcPeerId = PeerId(srcPeerId);
	result.total = total;
	result.done = done;
	result.skipped = skipped;
	result.lastMsgId = MsgId(lastMsgId);
	result.forwardOptions = forwardOptions;
	result.groupOptions = groupOptions;
	result.remaining.reserve(count);
	for (auto i = quint32(0); i != count; ++i) {
		auto value = qint64();
		stream >> value;
		if (stream.status() != QDataStream::Ok) {
			return std::nullopt;
		}
		result.remaining.push_back(MsgId(value));
	}
	return result;
}

[[nodiscard]] QByteArray SerializeEfJobOptions(const EfJobOptions &options) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream << kEfJobSerializationVersion;
	stream << options.jobId;
	stream << quint64(options.destPeerId.value);
	stream << quint64(options.srcPeerId.value);
	stream << quint8(options.paused ? 1 : 0);
	return result;
}

[[nodiscard]] std::optional<EfJobOptions> DeserializeEfJobOptions(
		const QByteArray &data) {
	auto stream = QDataStream(data);
	auto version = quint32();
	stream >> version;
	if (version != kEfJobSerializationVersion) {
		return std::nullopt;
	}
	auto result = EfJobOptions();
	auto destPeerId = quint64();
	auto srcPeerId = quint64();
	auto paused = quint8();
	stream >> result.jobId >> destPeerId >> srcPeerId >> paused;
	if (stream.status() != QDataStream::Ok) {
		return std::nullopt;
	}
	result.destPeerId = PeerId(destPeerId);
	result.srcPeerId = PeerId(srcPeerId);
	result.paused = (paused != 0);
	return result;
}

[[nodiscard]] QByteArray SerializeEfResumeItem(const EfResumeItem &item) {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream << kEfItemSerializationVersion;
	stream << quint64(item.sessionId);
	stream << item.jobId;
	stream << qint32(item.itemIndex);
	stream << quint64(item.peerId.value);
	stream << quint64(item.sourceId.peer.value);
	stream << qint64(item.sourceId.msg.bare);
	stream << item.state;
	stream << item.localPath;
	stream << quint64(item.fileId);
	stream << qint32(item.uploadedParts);
	stream << item.fileHash;
	stream << quint64(item.mediaId);
	stream << qint64(item.fileSize);
	stream << quint8(item.paused ? 1 : 0);
	return result;
}

[[nodiscard]] std::optional<EfResumeItem> DeserializeEfResumeItem(
		const QByteArray &data) {
	auto stream = QDataStream(data);
	auto version = quint32();
	stream >> version;
	if (version != kEfItemSerializationVersion) {
		return std::nullopt;
	}
	auto result = EfResumeItem();
	auto sessionId = quint64();
	auto itemIndex = qint32();
	auto peerId = quint64();
	auto sourcePeerId = quint64();
	auto sourceMsgId = qint64();
	auto fileId = quint64();
	auto uploadedParts = qint32();
	auto mediaId = quint64();
	auto fileSize = qint64();
	auto paused = quint8();
	stream >> sessionId >> result.jobId >> itemIndex
		>> peerId >> sourcePeerId >> sourceMsgId
		>> result.state >> result.localPath
		>> fileId >> uploadedParts >> result.fileHash
		>> mediaId >> fileSize >> paused;
	if (stream.status() != QDataStream::Ok) {
		return std::nullopt;
	}
	result.sessionId = sessionId;
	result.itemIndex = itemIndex;
	result.peerId = PeerId(peerId);
	result.sourceId = FullMsgId(PeerId(sourcePeerId), MsgId(sourceMsgId));
	result.fileId = fileId;
	result.uploadedParts = uploadedParts;
	result.mediaId = mediaId;
	result.fileSize = fileSize;
	result.paused = (paused != 0);
	return result;
}

[[nodiscard]] JobState ForwardJobState(const QString &state) {
	return (state == u"paused"_q)
		? JobState::Pending
		: JobState::InFlight;
}


} // namespace

class DedupDb::Impl {
public:
	explicit Impl(const QString &path, bool purgePending);
	~Impl();

	[[nodiscard]] bool isOpen() const;
	void ensureTableLoaded(Table table) const;
	void acquireTable(Table table);
	void releaseTable(Table table);
	void invalidateTable(Table table);
	void acquireChatTable(PeerId peerId, bool allPeers);
	void releaseChatTable(PeerId peerId, bool allPeers);
	void loadChatTable(PeerId peerId, bool allPeers);
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
	void invalidateChatTable(PeerId peerId);
	void invalidateAllPeersChatTable();
	[[nodiscard]] uint64 resolveHashId(const QByteArray &hash);
	[[nodiscard]] uint64 findHashId(const QByteArray &hash) const;
	[[nodiscard]] QByteArray hashForHashId(uint64 hashId) const;
	[[nodiscard]] std::optional<HashOwner> hashOwner(uint64 hashId) const;
	void setHashOwner(uint64 hashId, PeerId peerId, MsgId msgId);
	void clearHashOwner(uint64 hashId);
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
	[[nodiscard]] bool promoteOwner(
		uint64 hashId,
		PeerId peerId,
		MsgId msgId);
	[[nodiscard]] std::optional<HashOwner> refreshOwner(
		uint64 hashId,
		PeerId excludePeer,
		MsgId excludeMsg);
	[[nodiscard]] bool promoteOwnerLocked(
		uint64 hashId,
		PeerId peerId,
		MsgId msgId);
	[[nodiscard]] std::optional<ChatScanOwnerDecision>
	chatScanApplyHashOwner(
		PeerId peerId,
		MsgId msgId,
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash,
		bool mediaDuplicate,
		const QString &filename);
	[[nodiscard]] bool recordUploadDestination(
		const QByteArray &hash,
		uint64 mediaId,
		bool isPhoto,
		PeerId peerId,
		MsgId msgId,
		bool tracked);
	[[nodiscard]] std::vector<ChatScanOwnerChange> removeChatScan(
		PeerId peerId);
	[[nodiscard]] uint64 createJob(const JobRecord &job);
	[[nodiscard]] std::optional<JobRecord> job(uint64 jobId) const;
	[[nodiscard]] std::optional<JobRecord> findJob(
		JobKind kind,
		uint64 scopeId,
		bool activeOnly) const;
	void setJobState(uint64 jobId, JobState state);
	void setJobProgress(
		uint64 jobId,
		int total,
		int done,
		int skipped);
	void pauseJob(uint64 jobId);
	void finishJob(uint64 jobId);
	void cancelJob(uint64 jobId);
	void finishScanJob(uint64 jobId);
	void removeJobItems(uint64 jobId);
	void writeJobItems(const std::vector<JobItemRecord> &items);
	void writeJobItem(const JobItemRecord &item);
	[[nodiscard]] std::vector<JobItemRecord> jobItems(uint64 jobId) const;
	[[nodiscard]] std::vector<JobItemRecord> jobResumeItems(
		uint64 jobId) const;
	void setJobItemState(
		uint64 jobId,
		uint64 itemId,
		JobState state,
		const QString &error,
		int retry);
	[[nodiscard]] bool jobContainsMedia(
		uint64 jobId,
		uint64 mediaId,
		bool isPhoto) const;
	[[nodiscard]] bool jobContainsHash(
		uint64 jobId,
		uint64 hashId) const;
	void insert(Table table, const DedupRecord &record);
	void insertLoaded(Table table, const DedupRecord &record);
	void insertDeferred(Table table, const DedupRecord &record);
	void forgetLoadedDoc(Table table, uint64 documentId, bool isPhoto);
	[[nodiscard]] bool containsDocId(
		Table table,
		uint64 documentId,
		bool isPhoto) const;
	[[nodiscard]] bool containsDocIdInDb(
		Table table,
		uint64 documentId,
		bool isPhoto) const;
	[[nodiscard]] bool containsHash(
		Table table,
		const QByteArray &hash) const;
	[[nodiscard]] bool containsFinishedHash(
		Table table,
		const QByteArray &hash) const;
	void rekey(
		Table table,
		uint64 oldDocumentId,
		uint64 newDocumentId,
		bool isPhoto);
	void updateDedupStatus(
		Table table,
		const QByteArray &hash,
		const QString &status);
	void removeUnfinishedByHash(Table table, const QByteArray &hash);
	[[nodiscard]] QByteArray hashForDocId(
		Table table,
		uint64 documentId,
		bool isPhoto) const;
	[[nodiscard]] uint64 seekDocumentId(
		Table table,
		const QByteArray &hash,
		uint64 excludeDocumentId,
		bool excludeIsPhoto) const;
	void addPending(
		Table table,
		uint64 documentId,
		const QByteArray &hash,
		bool isPhoto);
	void removePending(Table table, uint64 documentId, bool isPhoto);
	[[nodiscard]] std::vector<DedupRecord> loadAll(Table table) const;
	void refreshHashRefs(uint64 hashId);
	void refreshMediaRefs(uint64 mediaId, bool isPhoto);
	void cleanupMediaRefs(uint64 mediaId, bool isPhoto);
	void cleanupHashRefs(uint64 hashId);
	void refreshChatScanRefs(
		const std::set<uint64> &hashIds,
		const std::set<std::pair<uint64, bool>> &media);

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
	void flushExPending();

	[[nodiscard]] std::vector<ChatScanRow> readChatScan(
		PeerId peerId) const;
	void readChatScanAsync(
		PeerId peerId,
		Fn<void(std::vector<ChatScanRow>, bool)> done);
	[[nodiscard]] bool isChatScanned(PeerId peerId) const;
	void markChatDeleteFinished(PeerId peerId);
	void requestIncrementalVacuum();
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
	[[nodiscard]] bool chatScanCategoryHasItems(
		PeerId peerId,
		int category) const;
	[[nodiscard]] MsgId chatScanMaxMsgId(PeerId peerId) const;
	[[nodiscard]] bool chatScanContainsHash(
		PeerId peerId,
		const QByteArray &hash) const;
	[[nodiscard]] MsgId chatScanUniqueHash(
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
		bool isPhoto,
		qint64 retryAt);
	void chatScanReleaseLeases(PeerId peerId);
	[[nodiscard]] int chatScanCommitHashes(
		PeerId peerId,
		const std::vector<ChatScanHash> &hashes);
	void chatScanFailItem(PeerId peerId, uint64 mediaId, bool isPhoto);
	[[nodiscard]] int chatScanRetryFailed(PeerId peerId);
	[[nodiscard]] int chatScanLeasedCount(PeerId peerId) const;
	[[nodiscard]] int chatScanFailedCount(PeerId peerId) const;
	[[nodiscard]] QByteArray chatScanHashForMedia(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto) const;
	[[nodiscard]] QByteArray chatScanHashForMediaOther(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto) const;
	void recomputeChatScanProgressAsync(
		PeerId peerId,
		Fn<void(std::vector<ChatScanProgressRow>, bool)> done);
	void chatScanUniqueHashesAsync(
		PeerId peerId,
		Fn<void(std::vector<std::pair<QByteArray, MsgId>>, bool)> done);
	void chatScanItemIdsAsync(
		PeerId peerId,
		int category,
		Fn<void(std::vector<MsgId>, bool)> done);
	void chatScanMediaHashesAsync(
		PeerId peerId,
		bool includeOther,
		Fn<void(std::vector<ChatScanMediaHash>, bool)> done);
	[[nodiscard]] std::vector<ChatScanItem> chatScanDistinctItems(
		PeerId peerId) const;
	void clearChatScan(PeerId peerId);
	void clearChatScanAsync(
		PeerId peerId,
		Fn<void(bool, std::vector<ChatScanOwnerChange>)> done);
	void writeScanCursor(const ChatScanState &state);
	[[nodiscard]] std::optional<ChatScanState> readScanCursor(
		PeerId peerId) const;
	void finishChatScan(
		PeerId peerId,
		MsgId scannedUpto,
		int mode,
		bool deleteFinished,
		int64 elapsedMs);
	[[nodiscard]] MsgId chatScanScannedUpto(PeerId peerId) const;
	[[nodiscard]] int chatScanMode(PeerId peerId) const;
	void writeChatScanMode(PeerId peerId, int mode);
	[[nodiscard]] std::optional<uint64> findScanJobId(PeerId peerId) const;
	[[nodiscard]] ScanJobOptions readScanOptions(PeerId peerId) const;
	void writeScanOptions(PeerId peerId, const ScanJobOptions &options);
	void writeChatScanItems(const std::vector<ChatScanItem> &items);
	[[nodiscard]] std::vector<ChatScanItem> readChatScanItems(
		PeerId peerId) const;
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
		const QString &filename);
	void saveChatScanUndo(PeerId peerId, MsgId msgId);
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

	enum class TmpTable {
		Dl,
		Ul,
		Fw,
	};
	[[nodiscard]] static JobKind TmpJobKind(TmpTable table);

	void insertTmp(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		uint64 documentId,
		bool isPhoto,
		const QByteArray &hash);
	[[nodiscard]] bool containsTmpDocId(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		uint64 documentId,
		bool isPhoto) const;
	[[nodiscard]] bool containsTmpHash(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		const QByteArray &hash) const;
	[[nodiscard]] QByteArray hashForTmpDocId(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		uint64 documentId,
		bool isPhoto) const;
	void removeTmpDoc(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		uint64 documentId,
		bool isPhoto);
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
		QHash<uint64, QByteArray> docToHash[2];
		QHash<QByteArray, QSet<uint64>> hashToDoc[2];
		struct Pending {
			uint64 docId = 0;
			bool isPhoto = false;
			QByteArray hash;
		};
		std::vector<Pending> pending;
		uint64 jobId = 0;
		uint64 nextItemId = 1;
		bool loaded = false;
	};

	void ensureLoaded(Table table) const;
	[[nodiscard]] ExTmpState &exTmpState(uint64 sessionId, PeerId peerId);
	[[nodiscard]] const ExTmpState &exTmpState(
		uint64 sessionId,
		PeerId peerId) const;
	void ensureExTmpLoaded(uint64 sessionId, PeerId peerId) const;
	void flushExTmpState(const ExTmpKey &key, ExTmpState &state);

	bool createTables();

	struct ChatCacheKey {
		PeerId peerId;
		bool allPeers = false;
		inline bool operator<(const ChatCacheKey &other) const {
			return std::tie(peerId.value, allPeers)
				< std::tie(other.peerId.value, other.allPeers);
		}
	};

	struct ChatCacheState {
		QHash<uint64, QByteArray> photoHashes;
		QHash<uint64, QByteArray> docHashes;
		QSet<QByteArray> hashes;
		int users = 0;
		bool loaded = false;
	};

	void removeJob(uint64 jobId);
	[[nodiscard]] std::vector<JobItemRecord> readJobItems(
		uint64 jobId,
		bool resumeOnly) const;
	[[nodiscard]] uint64 findJobId(
		JobKind kind,
		uint64 sessionId,
		uint64 scopeId) const;
	[[nodiscard]] std::vector<std::pair<uint64, ExResumeRecord>>
	loadExportJobs(uint64 sessionId) const;
	[[nodiscard]] uint64 findExportJob(
		uint64 sessionId,
		const ExResumeRecord &record) const;
	void updateExportJob(uint64 jobId, const ExResumeRecord &record);
	void purgeOrphanExportBatchJobs();
	void purgeOrphanBatchJobs(
		JobKind batchKind,
		const std::vector<JobKind> &ownerKinds);

	[[nodiscard]] std::optional<std::pair<uint64, EfJobOptions>> findEfJob(
		uint64 sessionId,
		PeerId destPeerId,
		const QString &jobId) const;
	[[nodiscard]] std::vector<uint64> findEfJobIds(
		const QString &jobId) const;
	[[nodiscard]] std::vector<EfResumeItem> readEfResumeItems(
		uint64 jobId,
		const EfJobOptions &options) const;
	void updateEfJobOptions(uint64 jobId, const EfJobOptions &options);
	void removeEfJobIfEmpty(uint64 jobId);

	QString _connectionName;
	QString _path;
	std::shared_ptr<std::atomic_bool> _vacuumCancel;
	QSqlDatabase _db;
	bool _open = false;
	int _transactionDepth = 0;
	std::shared_ptr<SharedDownloads> _shared;
	std::map<ChatCacheKey, ChatCacheState> _chatCaches;
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
		QHash<uint64, QByteArray> docToHash[2];
		QHash<QByteArray, QSet<uint64>> hashToDoc[2];
		struct Pending {
			uint64 docId = 0;
			bool isPhoto = false;
			QByteArray hash;
		};
		std::vector<Pending> pending;
		uint64 jobId = 0;
		uint64 nextItemId = 1;
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
	void flushTmpState(const TmpKey &key, TmpState &state);

	mutable std::map<TmpKey, TmpState> _tmpStates;
	int _tmpPendingWrites = 0;
};

DedupDb::Impl::Impl(const QString &path, bool purgePending)
: _connectionName(QUuid::createUuid().toString())
, _path(path)
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
	pragma.exec(u"PRAGMA auto_vacuum=INCREMENTAL"_q);
	pragma.exec(u"PRAGMA journal_mode=WAL"_q);
	pragma.exec(u"PRAGMA synchronous=NORMAL"_q);
	pragma.exec(u"PRAGMA busy_timeout=5000"_q);
	// The database grows to hundreds of megabytes, far past the two megabyte
	// page cache SQLite keeps by default, so every scan statement that walks a
	// few dozen rows ends up reading pages back from the file. A crash dump
	// caught the main thread inside exactly that read, called from the chunk
	// commit. Keep the hot pages in memory and let reads be mapped instead of
	// issued as file reads.
	pragma.exec(u"PRAGMA cache_size=-65536"_q);
	pragma.exec(u"PRAGMA mmap_size=268435456"_q);
	_open = createTables();
	if (_open && purgePending) {
		// 'u' rows can only survive a crash; at startup nothing is in flight.
		// Second connections must not purge another thread's rows.
		purgeOrphanExportBatchJobs();
		purgeOrphanBatchJobs(
			JobKind::ForwardBatch,
			{ JobKind::Forward, JobKind::EnhancedForward });
		QSqlQuery cleanup(_db);
		if (!cleanup.exec(u"DELETE FROM dedup_dl WHERE status = 'u'"_q)
			|| !cleanup.exec(u"DELETE FROM dedup_ul WHERE status = 'u'"_q)
			|| !cleanup.exec(u"DELETE FROM job_items WHERE job_id IN ("
				"SELECT job_id FROM jobs "
				"WHERE kind = 'download_batch')"_q)
			|| !cleanup.exec(u"DELETE FROM jobs "
				"WHERE kind = 'download_batch'"_q)
			|| !cleanup.exec(u"DELETE FROM job_items WHERE job_id IN ("
				"SELECT job_id FROM jobs "
				"WHERE kind = 'upload_batch')"_q)
			|| !cleanup.exec(u"DELETE FROM jobs "
				"WHERE kind = 'upload_batch'"_q)
			|| !cleanup.exec(u"DELETE FROM media_refs WHERE id_refs = 0 "
				"AND NOT EXISTS (SELECT 1 FROM dedup_ul "
					"WHERE dedup_ul.doc_id = media_refs.media_id "
					"AND dedup_ul.is_photo = media_refs.is_photo "
					"AND dedup_ul.status = 'u') "
				"AND NOT EXISTS (SELECT 1 FROM dedup_ul "
					"WHERE dedup_ul.doc_id = media_refs.media_id "
					"AND dedup_ul.is_photo = media_refs.is_photo "
					"AND dedup_ul.status = 'f')"_q)
			|| !cleanup.exec(u"DELETE FROM hashes WHERE "
				"NOT EXISTS (SELECT 1 FROM chat_scan_items "
					"WHERE chat_scan_items.hash_id = hashes.hash_id) "
				"AND NOT EXISTS (SELECT 1 FROM dedup_ul "
					"WHERE dedup_ul.hash_id = hashes.hash_id) "
				"AND NOT EXISTS (SELECT 1 FROM dedup_dl "
					"WHERE dedup_dl.hash_id = hashes.hash_id) "
				"AND NOT EXISTS (SELECT 1 FROM media_refs "
					"WHERE media_refs.hash_id = hashes.hash_id) "
				"AND NOT EXISTS (SELECT 1 FROM job_items "
					"WHERE job_items.hash_id = hashes.hash_id)"_q)) {
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
	if (_open) {
		crl::async([path = _path] {
			OptimizeDatabase(path);
		});
	}
}

DedupDb::Impl::~Impl() {
	if (_vacuumCancel) {
		_vacuumCancel->store(true);
	}
	if (_db.isOpen()) {
		if (!gVacuuming.load()) {
			QSqlQuery q(_db);
			if (!q.exec(u"PRAGMA optimize"_q)) {
				LOG(("DedupDb: Close optimize failed: %1").arg(
					q.lastError().text()));
			}
			if (!q.exec(u"PRAGMA wal_checkpoint(TRUNCATE)"_q)) {
				LOG(("DedupDb: Close checkpoint failed: %1").arg(
					q.lastError().text()));
			}
		}
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
	s.jobId = findJobId(
		JobKind::ExportBatch,
		sessionId,
		peerId.value);
	if (!s.jobId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT item_id, media_id, is_photo, hash_id "
		"FROM job_items WHERE job_id = :job_id ORDER BY item_id"_q);
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(s.jobId)));
	if (!q.exec()) {
		LOG(("DedupDb: LoadExTmp failed: %1").arg(q.lastError().text()));
		return;
	}
	while (q.next()) {
		const auto itemId = q.value(0).toULongLong();
		const auto docId = q.value(1).toULongLong();
		const auto type = (q.value(2).toInt() != 0) ? 1 : 0;
		const auto hash = hashForHashId(q.value(3).toULongLong());
		s.nextItemId = std::max(s.nextItemId, itemId + 1);
		if (!docId || hash.isEmpty()) {
			continue;
		}
		s.docToHash[type][docId] = hash;
		s.hashToDoc[type][hash].insert(docId);
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
	if (!q.exec(u"SELECT h.hash, d.doc_id, d.is_photo, d.status FROM "
			+ TableName(table)
			+ u" d JOIN hashes h ON h.hash_id = d.hash_id"_q)) {
		LOG(("DedupDb: Load failed: %1").arg(q.lastError().text()));
		return;
	}
	while (q.next()) {
		const auto hash = q.value(0).toByteArray();
		const auto docId = q.value(1).toULongLong();
		const auto type = (q.value(2).toInt() != 0) ? 1 : 0;
		if (hash.isEmpty() || !docId) {
			continue;
		}
		s.docToHash[type][docId] = hash;
		s.hashToDoc[type][hash].insert(docId);
		if (q.value(3).toString() == u"u") {
			s.unfinishedDocs[type].insert(docId);
		}
	}
}

bool DedupDb::Impl::isOpen() const {
	return _open;
}

void DedupDb::Impl::ensureTableLoaded(Table table) const {
	ensureLoaded(table);
}

void DedupDb::Impl::acquireTable(Table table) {
	{
		std::lock_guard<std::mutex> lock(_shared->mutex);
		++_shared->tableUsers[int(table)];
	}
	ensureLoaded(table);
}

void DedupDb::Impl::releaseTable(Table table) {
	std::lock_guard<std::mutex> lock(_shared->mutex);
	auto &users = _shared->tableUsers[int(table)];
	if (users > 0 && --users == 0) {
		_shared->state[int(table)] = TableState();
	}
}

void DedupDb::Impl::invalidateTable(Table table) {
	std::lock_guard<std::mutex> lock(_shared->mutex);
	_shared->state[int(table)] = TableState();
}

void DedupDb::Impl::acquireChatTable(PeerId peerId, bool allPeers) {
	const auto key = ChatCacheKey{
		allPeers ? PeerId() : peerId,
		allPeers,
	};
	auto &cache = _chatCaches[key];
	++cache.users;
	loadChatTable(peerId, allPeers);
}

void DedupDb::Impl::releaseChatTable(PeerId peerId, bool allPeers) {
	const auto key = ChatCacheKey{
		allPeers ? PeerId() : peerId,
		allPeers,
	};
	const auto i = _chatCaches.find(key);
	if (i == _chatCaches.end()) {
		return;
	}
	if (i->second.users > 0 && --i->second.users == 0) {
		_chatCaches.erase(i);
	}
}

void DedupDb::Impl::loadChatTable(PeerId peerId, bool allPeers) {
	if (!_open) {
		return;
	}
	const auto key = ChatCacheKey{
		allPeers ? PeerId() : peerId,
		allPeers,
	};
	auto &cache = _chatCaches[key];
	if (cache.loaded) {
		return;
	}
	auto query = u"SELECT i.media_id, i.is_photo, h.hash "
		"FROM chat_scan_items i "
		"JOIN hashes h ON h.hash_id = i.hash_id "
		"WHERE i.is_dup = 0 AND i.hash_id != 0"_q;
	if (!allPeers) {
		query += u" AND i.peer_id = :peer_id"_q;
	}
	QSqlQuery q(_db);
	q.prepare(query);
	if (!allPeers) {
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
	}
	if (!q.exec()) {
		LOG(("DedupDb: LoadChatTable failed: %1").arg(
			q.lastError().text()));
		return;
	}
	cache.photoHashes.clear();
	cache.docHashes.clear();
	cache.hashes.clear();
	while (q.next()) {
		const auto mediaId = q.value(0).toULongLong();
		const auto isPhoto = (q.value(1).toInt() != 0);
		const auto hash = q.value(2).toByteArray();
		if (!mediaId || hash.isEmpty()) {
			continue;
		}
		(isPhoto ? cache.photoHashes : cache.docHashes)[mediaId] = hash;
		cache.hashes.insert(hash);
	}
	cache.loaded = true;
}

bool DedupDb::Impl::chatTableContainsDocId(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto,
		bool allPeers) const {
	if (!mediaId) {
		return false;
	}
	const auto key = ChatCacheKey{
		allPeers ? PeerId() : peerId,
		allPeers,
	};
	const auto i = _chatCaches.find(key);
	if (i == _chatCaches.end() || !i->second.loaded) {
		return false;
	}
	const auto &hashes = isPhoto
		? i->second.photoHashes
		: i->second.docHashes;
	return hashes.contains(mediaId);
}

bool DedupDb::Impl::chatTableContainsHash(
		PeerId peerId,
		const QByteArray &hash,
		bool allPeers) const {
	if (hash.isEmpty()) {
		return false;
	}
	const auto key = ChatCacheKey{
		allPeers ? PeerId() : peerId,
		allPeers,
	};
	const auto i = _chatCaches.find(key);
	return (i != _chatCaches.end())
		&& i->second.loaded
		&& i->second.hashes.contains(hash);
}

void DedupDb::Impl::clearChatTable() {
	for (auto i = _chatCaches.begin(); i != _chatCaches.end();) {
		if (i->second.users == 0) {
			i = _chatCaches.erase(i);
		} else {
			++i;
		}
	}
}

void DedupDb::Impl::addChatTableUnique(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash) {
	if (!_open || !peerId || !mediaId || hash.isEmpty()) {
		return;
	}
	const auto add = [&](PeerId keyPeer, bool allPeers) {
		const auto key = ChatCacheKey{
			allPeers ? PeerId() : keyPeer,
			allPeers,
		};
		const auto i = _chatCaches.find(key);
		if (i == _chatCaches.end() || !i->second.loaded) {
			return;
		}
		auto &hashes = isPhoto
			? i->second.photoHashes
			: i->second.docHashes;
		hashes[mediaId] = hash;
		i->second.hashes.insert(hash);
	};
	add(peerId, false);
	add(PeerId(), true);
}

void DedupDb::Impl::removeChatTableUnique(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash) {
	if (!_open || !peerId || !mediaId || hash.isEmpty()) {
		return;
	}
	const auto remove = [&](PeerId keyPeer, bool allPeers) {
		const auto key = ChatCacheKey{
			allPeers ? PeerId() : keyPeer,
			allPeers,
		};
		const auto i = _chatCaches.find(key);
		if (i == _chatCaches.end() || !i->second.loaded) {
			return;
		}
		auto &hashes = isPhoto
			? i->second.photoHashes
			: i->second.docHashes;
		if (hashes.value(mediaId) == hash) {
			hashes.remove(mediaId);
		}
		i->second.hashes.remove(hash);
	};
	remove(peerId, false);
	remove(PeerId(), true);
}

void DedupDb::Impl::invalidateChatTable(PeerId peerId) {
	if (!_open || !peerId) {
		return;
	}
	const auto key = ChatCacheKey{ peerId, false };
	const auto i = _chatCaches.find(key);
	if (i == _chatCaches.end()) {
		return;
	}
	i->second.loaded = false;
	loadChatTable(peerId, false);
}

void DedupDb::Impl::invalidateAllPeersChatTable() {
	if (!_open) {
		return;
	}
	const auto key = ChatCacheKey{ PeerId(), true };
	const auto i = _chatCaches.find(key);
	if (i == _chatCaches.end()) {
		return;
	}
	i->second.loaded = false;
	loadChatTable(PeerId(), true);
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
	// hash_id. No size column: size is never used as a dedup criterion.
	const auto created = exec(u"CREATE TABLE IF NOT EXISTS hashes ("
		"hash_id INTEGER PRIMARY KEY AUTOINCREMENT, "
		"hash BLOB NOT NULL UNIQUE, "
		"hash_refs INTEGER NOT NULL DEFAULT 0, "
		"owner_peer_id INTEGER NOT NULL DEFAULT 0, "
		"owner_msg_id INTEGER NOT NULL DEFAULT 0)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_hashes_owner "
			"ON hashes(owner_peer_id, owner_msg_id)"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS media_refs ("
			"media_id INTEGER NOT NULL, "
			"is_photo INTEGER NOT NULL DEFAULT 0, "
			"hash_id INTEGER NOT NULL DEFAULT 0, "
			"id_refs INTEGER NOT NULL DEFAULT 0, "
			"PRIMARY KEY (media_id, is_photo))"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_media_refs_hash_id "
			"ON media_refs(hash_id)"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS dedup_dl ("
			"doc_id INTEGER NOT NULL, "
			"is_photo INTEGER NOT NULL DEFAULT 0, "
			"hash_id INTEGER NOT NULL DEFAULT 0, "
			"status TEXT NOT NULL DEFAULT 'f', "
			"PRIMARY KEY (doc_id, is_photo))"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS dedup_ul ("
			"doc_id INTEGER NOT NULL, "
			"is_photo INTEGER NOT NULL DEFAULT 0, "
			"hash_id INTEGER NOT NULL DEFAULT 0, "
			"status TEXT NOT NULL DEFAULT 'f', "
			"PRIMARY KEY (doc_id, is_photo))"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_dedup_dl_hash_id "
			"ON dedup_dl(hash_id)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_dedup_ul_hash_id "
			"ON dedup_ul(hash_id)"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS jobs ("
			"job_id INTEGER PRIMARY KEY AUTOINCREMENT, "
			"kind TEXT NOT NULL, "
			"session_id INTEGER NOT NULL DEFAULT 0, "
			"scope_id INTEGER NOT NULL DEFAULT 0, "
			"options BLOB, "
			"state INTEGER NOT NULL DEFAULT 0, "
			"total INTEGER NOT NULL DEFAULT 0, "
			"done INTEGER NOT NULL DEFAULT 0, "
			"skipped INTEGER NOT NULL DEFAULT 0, "
			"created_at INTEGER NOT NULL DEFAULT 0, "
			"updated_at INTEGER NOT NULL DEFAULT 0)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_jobs_kind_scope "
			"ON jobs(kind, scope_id, state)"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS job_items ("
			"job_id INTEGER NOT NULL, "
			"item_id INTEGER NOT NULL, "
			"media_id INTEGER NOT NULL DEFAULT 0, "
			"is_photo INTEGER NOT NULL DEFAULT 0, "
			"hash_id INTEGER NOT NULL DEFAULT 0, "
			"state INTEGER NOT NULL DEFAULT 0, "
			"payload BLOB, "
			"error TEXT, "
			"retry INTEGER NOT NULL DEFAULT 0, "
			"PRIMARY KEY (job_id, item_id))"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_job_items_state "
			"ON job_items(job_id, state)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_job_items_media "
			"ON job_items(job_id, media_id, is_photo)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_job_items_hash "
			"ON job_items(job_id, hash_id)"_q)
		&& exec(u"CREATE TABLE IF NOT EXISTS chat_scan_items ("
			"peer_id INTEGER NOT NULL, "
			"msg_id INTEGER NOT NULL, "
			"category INTEGER NOT NULL DEFAULT 0, "
			"size INTEGER NOT NULL DEFAULT 0, "
			"media_id INTEGER NOT NULL DEFAULT 0, "
			"is_photo INTEGER NOT NULL DEFAULT 0, "
			"hash_id INTEGER NOT NULL DEFAULT 0, "
			"filename TEXT, "
			"is_dup INTEGER NOT NULL DEFAULT 0 CHECK (is_dup IN (0, 1)), "
			"session_id INTEGER NOT NULL DEFAULT 0, "
			"state INTEGER NOT NULL DEFAULT 0, "
			"attempts INTEGER NOT NULL DEFAULT 0, "
			"lease_until INTEGER NOT NULL DEFAULT 0, "
			"tracked INTEGER NOT NULL DEFAULT 1 CHECK (tracked IN (0, 1)), "
			"PRIMARY KEY (peer_id, msg_id))"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_chat_scan_items_media "
			"ON chat_scan_items(peer_id, media_id, is_photo)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_chat_scan_items_media_global "
			"ON chat_scan_items(media_id, is_photo, peer_id, is_dup, msg_id)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_chat_scan_items_hash_id "
			"ON chat_scan_items(peer_id, hash_id)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_chat_scan_items_hash_tracked "
			"ON chat_scan_items(hash_id, tracked, msg_id)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_chat_scan_items_hash_id_unique_msg "
			"ON chat_scan_items(hash_id, peer_id, msg_id) WHERE is_dup = 0"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_chat_scan_items_dup "
			"ON chat_scan_items(peer_id, is_dup, msg_id)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_chat_scan_items_dup_category "
			"ON chat_scan_items(peer_id, is_dup, category, msg_id)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_chat_scan_items_state "
			"ON chat_scan_items(peer_id, state, media_id)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_chat_scan_items_category "
			"ON chat_scan_items(peer_id, category)"_q)
		&& exec(u"CREATE INDEX IF NOT EXISTS idx_chat_scan_items_session "
			"ON chat_scan_items(peer_id, session_id)"_q);
	return created;
}

uint64 DedupDb::Impl::resolveHashId(const QByteArray &hash) {
	if (!_open || hash.isEmpty()) {
		return 0;
	}
	QSqlQuery insert(_db);
	insert.prepare(u"INSERT OR IGNORE INTO hashes(hash) VALUES(:hash)"_q);
	insert.bindValue(u":hash"_q, hash);
	if (!insert.exec()) {
		LOG(("DedupDb: ResolveHashId insert failed: %1").arg(
			insert.lastError().text()));
		return 0;
	}
	QSqlQuery select(_db);
	select.prepare(u"SELECT hash_id FROM hashes WHERE hash = :hash"_q);
	select.bindValue(u":hash"_q, hash);
	if (!select.exec() || !select.next()) {
		LOG(("DedupDb: ResolveHashId select failed: %1").arg(
			select.lastError().text()));
		return 0;
	}
	return select.value(0).toULongLong();
}

uint64 DedupDb::Impl::findHashId(const QByteArray &hash) const {
	if (!_open || hash.isEmpty()) {
		return 0;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT hash_id FROM hashes WHERE hash = :hash"_q);
	q.bindValue(u":hash"_q, hash);
	if (!q.exec() || !q.next()) {
		return 0;
	}
	return q.value(0).toULongLong();
}

QByteArray DedupDb::Impl::hashForHashId(uint64 hashId) const {
	if (!_open || !hashId) {
		return {};
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT hash FROM hashes WHERE hash_id = :hash_id"_q);
	q.bindValue(u":hash_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(hashId)));
	if (!q.exec() || !q.next()) {
		return {};
	}
	return q.value(0).toByteArray();
}

std::optional<HashOwner> DedupDb::Impl::hashOwner(uint64 hashId) const {
	if (!_open || !hashId) {
		return std::nullopt;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT h.owner_peer_id, h.owner_msg_id, "
		"COALESCE(i.tracked, 0), (i.peer_id IS NOT NULL) "
		"FROM hashes h "
		"LEFT JOIN chat_scan_items i "
		"ON i.peer_id = h.owner_peer_id AND i.msg_id = h.owner_msg_id "
		"WHERE h.hash_id = :hash_id"_q);
	q.bindValue(u":hash_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(hashId)));
	if (!q.exec()) {
		LOG(("DedupDb: HashOwner read failed: %1").arg(
			q.lastError().text()));
		return std::nullopt;
	}
	if (!q.next()) {
		return std::nullopt;
	}
	auto result = HashOwner();
	result.peerId = PeerId(q.value(0).toULongLong());
	result.msgId = MsgId(q.value(1).toLongLong());
	if (!result.peerId || !result.msgId) {
		return std::nullopt;
	}
	result.tracked = (q.value(2).toInt() != 0);
	result.exists = (q.value(3).toInt() != 0);
	return result;
}

void DedupDb::Impl::setHashOwner(
		uint64 hashId,
		PeerId peerId,
		MsgId msgId) {
	if (!_open || !hashId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE hashes SET owner_peer_id = :peer_id, "
		"owner_msg_id = :msg_id WHERE hash_id = :hash_id"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(msgId.bare)));
	q.bindValue(u":hash_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(hashId)));
	if (!q.exec()) {
		LOG(("DedupDb: HashOwner write failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::clearHashOwner(uint64 hashId) {
	setHashOwner(hashId, PeerId(0), MsgId(0));
}

std::optional<HashOwner> DedupDb::Impl::findHashOwnerReplacement(
		uint64 hashId,
		PeerId excludePeer,
		MsgId excludeMsg) const {
	if (!_open) {
		return std::nullopt;
	}
	return FindHashOwnerReplacementIn(
		_db,
		hashId,
		excludePeer,
		excludeMsg);
}

bool DedupDb::Impl::claimOwner(
		uint64 hashId,
		PeerId peerId,
		MsgId msgId) {
	if (!_open || !hashId || !peerId || !msgId) {
		return false;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE hashes SET owner_peer_id = :peer_id, "
		"owner_msg_id = :msg_id "
		"WHERE hash_id = :hash_id "
		"AND owner_peer_id = 0 AND owner_msg_id = 0"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(msgId.bare)));
	q.bindValue(u":hash_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(hashId)));
	if (!q.exec()) {
		LOG(("DedupDb: ClaimOwner failed: %1").arg(q.lastError().text()));
		return false;
	}
	return q.numRowsAffected() > 0;
}

void DedupDb::Impl::setOwner(uint64 hashId, PeerId peerId, MsgId msgId) {
	setHashOwner(hashId, peerId, msgId);
}

void DedupDb::Impl::clearOwner(uint64 hashId) {
	clearHashOwner(hashId);
}

std::optional<HashOwner> DedupDb::Impl::findReplacementOwner(
		uint64 hashId,
		PeerId excludePeer,
		MsgId excludeMsg) const {
	return findHashOwnerReplacement(hashId, excludePeer, excludeMsg);
}

bool DedupDb::Impl::promoteOwnerLocked(
		uint64 hashId,
		PeerId peerId,
		MsgId msgId) {
	const auto old = hashOwner(hashId);
	QSqlQuery promote(_db);
	promote.prepare(u"UPDATE chat_scan_items SET is_dup = 0, "
		"filename = NULL WHERE peer_id = :peer_id "
		"AND msg_id = :msg_id"_q);
	promote.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	promote.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(msgId.bare)));
	if (!promote.exec()) {
		LOG(("DedupDb: PromoteOwner row failed: %1").arg(
			promote.lastError().text()));
		return false;
	}
	if (promote.numRowsAffected() <= 0) {
		return false;
	}
	setHashOwner(hashId, peerId, msgId);
	if (old && old->exists
		&& (old->peerId != peerId || old->msgId != msgId)) {
		QSqlQuery demote(_db);
		demote.prepare(u"UPDATE chat_scan_items SET is_dup = 1 "
			"WHERE peer_id = :peer_id AND msg_id = :msg_id"_q);
		demote.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(old->peerId.value)));
		demote.bindValue(u":msg_id"_q, QVariant::fromValue(
			static_cast<qlonglong>(old->msgId.bare)));
		if (!demote.exec()) {
			LOG(("DedupDb: PromoteOwner demote failed: %1").arg(
				demote.lastError().text()));
		}
	}
	return true;
}

bool DedupDb::Impl::promoteOwner(
		uint64 hashId,
		PeerId peerId,
		MsgId msgId) {
	if (!_open || !hashId || !peerId || !msgId) {
		return false;
	}
	beginTransaction();
	const auto result = promoteOwnerLocked(hashId, peerId, msgId);
	commitTransaction();
	return result;
}

std::optional<HashOwner> DedupDb::Impl::refreshOwner(
		uint64 hashId,
		PeerId excludePeer,
		MsgId excludeMsg) {
	if (!_open || !hashId) {
		return std::nullopt;
	}
	beginTransaction();
	auto replacement = findHashOwnerReplacement(
		hashId,
		excludePeer,
		excludeMsg);
	if (replacement) {
		const auto promoted = promoteOwnerLocked(
			hashId,
			replacement->peerId,
			replacement->msgId);
		if (!promoted) {
			clearHashOwner(hashId);
			replacement = std::nullopt;
		}
	} else {
		clearHashOwner(hashId);
	}
	commitTransaction();
	return replacement;
}

std::optional<ChatScanOwnerDecision>
DedupDb::Impl::chatScanApplyHashOwner(
		PeerId peerId,
		MsgId msgId,
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash,
		bool mediaDuplicate,
		const QString &filename) {
	if (!_open || !peerId || !msgId || hash.isEmpty()) {
		return std::nullopt;
	}
	const auto hashId = resolveHashId(hash);
	if (!hashId) {
		return std::nullopt;
	}
	beginTransaction();
	auto decision = ChatScanOwnerDecision();
	{
		QSqlQuery q(_db);
		q.prepare(u"SELECT is_dup FROM chat_scan_items "
			"WHERE peer_id = :peer_id AND msg_id = :msg_id"_q);
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		q.bindValue(u":msg_id"_q, QVariant::fromValue(
			static_cast<qlonglong>(msgId.bare)));
		if (!q.exec()) {
			LOG(("DedupDb: ScanOwner current read failed: %1").arg(
				q.lastError().text()));
			commitTransaction();
			return std::nullopt;
		}
		if (!q.next()) {
			commitTransaction();
			return std::nullopt;
		}
		decision.wasDup = (q.value(0).toInt() != 0);
	}
	{
		QSqlQuery q(_db);
		q.prepare(u"UPDATE chat_scan_items SET hash_id = :hash_id, "
			"tracked = 1 WHERE peer_id = :peer_id "
			"AND msg_id = :msg_id"_q);
		q.bindValue(u":hash_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(hashId)));
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		q.bindValue(u":msg_id"_q, QVariant::fromValue(
			static_cast<qlonglong>(msgId.bare)));
		if (!q.exec() || q.numRowsAffected() <= 0) {
			if (q.lastError().isValid()) {
				LOG(("DedupDb: ScanOwner row update failed: %1").arg(
					q.lastError().text()));
			}
			commitTransaction();
			return std::nullopt;
		}
	}
	if (mediaId) {
		insert(Table::Uploads, DedupRecord{
			.hash = hash,
			.documentId = mediaId,
			.isPhoto = isPhoto,
			.status = u"f"_q,
		});
	}
	const auto setCurrent = [&](bool isDup) {
		markChatScanItemDup(
			peerId,
			msgId,
			isDup,
			isDup ? filename : QString());
		decision.isDup = isDup;
	};
	if (mediaDuplicate) {
		setCurrent(true);
	} else {
		const auto owner = hashOwner(hashId);
		if (!owner || !owner->exists) {
			if (owner) {
				clearOwner(hashId);
			}
			if (!claimOwner(hashId, peerId, msgId)) {
				setOwner(hashId, peerId, msgId);
			}
			setCurrent(false);
		} else if (owner->peerId == peerId
			&& owner->msgId == msgId) {
			setCurrent(false);
		} else {
			const auto promoteCurrent = [&] {
				const auto old = chatScanItem(
					owner->peerId,
					owner->msgId);
				if (old) {
					decision.demoted = *old;
				}
				saveChatScanUndo(peerId, msgId);
				if (old) {
					saveChatScanUndo(owner->peerId, owner->msgId);
				}
				if (promoteOwnerLocked(hashId, peerId, msgId)) {
					decision.isDup = false;
				} else {
					decision.demoted.reset();
					setCurrent(true);
				}
			};
			if (!owner->tracked
				|| (owner->peerId == peerId
					&& msgId.bare > owner->msgId.bare)) {
				promoteCurrent();
			} else {
				setCurrent(true);
			}
		}
	}
	decision.owner = hashOwner(hashId);
	commitTransaction();
	if (decision.demoted) {
		removeChatTableUnique(
			decision.demoted->peerId,
			decision.demoted->mediaId,
			decision.demoted->isPhoto,
			hash);
	}
	if (!decision.isDup) {
		addChatTableUnique(peerId, mediaId, isPhoto, hash);
	} else if (decision.wasDup != decision.isDup) {
		invalidateChatTable(peerId);
		invalidateAllPeersChatTable();
	}
	return decision;
}

bool DedupDb::Impl::recordUploadDestination(
		const QByteArray &hash,
		uint64 mediaId,
		bool isPhoto,
		PeerId peerId,
		MsgId msgId,
		bool tracked) {
	if (!_open || hash.isEmpty() || !mediaId || !peerId || !msgId) {
		return false;
	}
	const auto hashId = resolveHashId(hash);
	if (!hashId) {
		return false;
	}
	beginTransaction();
	insert(Table::Uploads, DedupRecord{
		.hash = hash,
		.documentId = mediaId,
		.isPhoto = isPhoto,
		.status = u"f"_q,
	});
	auto isDup = false;
	auto demoted = std::optional<HashOwner>();
	const auto owner = hashOwner(hashId);
	if (!owner || !owner->exists) {
		setOwner(hashId, peerId, msgId);
	} else if (owner->peerId == peerId && owner->msgId == msgId) {
		isDup = false;
	} else if (owner->peerId == peerId) {
		if (owner->msgId.bare < msgId.bare) {
			demoted = owner;
			setOwner(hashId, peerId, msgId);
		} else {
			isDup = true;
		}
	} else if (!owner->tracked && tracked) {
		demoted = owner;
		setOwner(hashId, peerId, msgId);
	} else {
		isDup = true;
	}
	if (demoted) {
		QSqlQuery q(_db);
		q.prepare(u"UPDATE chat_scan_items SET is_dup = 1 "
			"WHERE peer_id = :peer_id AND msg_id = :msg_id"_q);
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(demoted->peerId.value)));
		q.bindValue(u":msg_id"_q, QVariant::fromValue(
			static_cast<qlonglong>(demoted->msgId.bare)));
		if (!q.exec()) {
			LOG(("DedupDb: UploadDestination demote failed: %1").arg(
				q.lastError().text()));
		}
	}
	if (tracked || !isDup) {
		QSqlQuery q(_db);
		q.prepare(u"INSERT INTO chat_scan_items "
			"(peer_id, msg_id, category, size, media_id, is_photo, "
			"hash_id, is_dup, tracked, state) "
			"VALUES (:peer_id, :msg_id, 0, 0, :media_id, :is_photo, "
			":hash_id, :is_dup, :tracked, 4) "
			"ON CONFLICT(peer_id, msg_id) DO UPDATE SET "
			"hash_id = excluded.hash_id, "
			"media_id = excluded.media_id, "
			"is_photo = excluded.is_photo, "
			"is_dup = excluded.is_dup, "
			"filename = CASE WHEN excluded.is_dup = 0 "
			"THEN NULL ELSE chat_scan_items.filename END, "
			"tracked = MAX(chat_scan_items.tracked, excluded.tracked)"_q);
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		q.bindValue(u":msg_id"_q, QVariant::fromValue(
			static_cast<qlonglong>(msgId.bare)));
		q.bindValue(u":media_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(mediaId)));
		q.bindValue(u":is_photo"_q, isPhoto ? 1 : 0);
		q.bindValue(u":hash_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(hashId)));
		q.bindValue(u":is_dup"_q, isDup ? 1 : 0);
		q.bindValue(u":tracked"_q, tracked ? 1 : 0);
		if (!q.exec()) {
			LOG(("DedupDb: UploadDestination row failed: %1").arg(
				q.lastError().text()));
		} else {
			refreshChatScanRefs(
				{ hashId },
				{ { mediaId, isPhoto } });
		}
	}
	commitTransaction();
	if (demoted) {
		if (const auto old = chatScanItem(
				demoted->peerId,
				demoted->msgId)) {
			removeChatTableUnique(
				demoted->peerId,
				old->mediaId,
				old->isPhoto,
				hash);
		}
	}
	if (!isDup) {
		addChatTableUnique(peerId, mediaId, isPhoto, hash);
	}
	return isDup;
}

uint64 DedupDb::Impl::createJob(const JobRecord &job) {
	if (!_open) {
		return 0;
	}
	const auto now = base::unixtime::now();
	const auto created = job.createdAt ? job.createdAt : int64(now);
	const auto updated = job.updatedAt ? job.updatedAt : created;
	QSqlQuery q(_db);
	q.prepare(u"INSERT INTO jobs(kind, session_id, scope_id, options, "
		"state, total, done, skipped, created_at, updated_at) "
		"VALUES(:kind, :session_id, :scope_id, :options, :state, "
		":total, :done, :skipped, :created_at, :updated_at)"_q);
	q.bindValue(u":kind"_q, JobKindName(job.kind));
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(job.sessionId)));
	q.bindValue(u":scope_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(job.scopeId)));
	q.bindValue(u":options"_q, job.options);
	q.bindValue(u":state"_q, int(job.state));
	q.bindValue(u":total"_q, job.total);
	q.bindValue(u":done"_q, job.done);
	q.bindValue(u":skipped"_q, job.skipped);
	q.bindValue(u":created_at"_q, QVariant::fromValue(
		static_cast<qlonglong>(created)));
	q.bindValue(u":updated_at"_q, QVariant::fromValue(
		static_cast<qlonglong>(updated)));
	if (!q.exec()) {
		LOG(("DedupDb: CreateJob failed: %1").arg(q.lastError().text()));
		return 0;
	}
	const auto id = q.lastInsertId();
	return id.isValid() ? id.toULongLong() : 0;
}

std::optional<JobRecord> DedupDb::Impl::job(uint64 jobId) const {
	if (!_open || !jobId) {
		return std::nullopt;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT job_id, kind, session_id, scope_id, options, state, "
		"total, done, skipped, created_at, updated_at "
		"FROM jobs WHERE job_id = :job_id"_q);
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	if (!q.exec()) {
		LOG(("DedupDb: LoadJob failed: %1").arg(q.lastError().text()));
		return std::nullopt;
	}
	return q.next() ? ReadJob(q) : std::nullopt;
}

std::optional<JobRecord> DedupDb::Impl::findJob(
		JobKind kind,
		uint64 scopeId,
		bool activeOnly) const {
	if (!_open) {
		return std::nullopt;
	}
	auto sql = u"SELECT job_id, kind, session_id, scope_id, options, state, "
		"total, done, skipped, created_at, updated_at "
		"FROM jobs WHERE kind = :kind AND scope_id = :scope_id"_q;
	if (activeOnly) {
		sql += u" AND state IN (0, 1)"_q;
	}
	sql += u" ORDER BY updated_at DESC, job_id DESC LIMIT 1"_q;
	QSqlQuery q(_db);
	q.prepare(sql);
	q.bindValue(u":kind"_q, JobKindName(kind));
	q.bindValue(u":scope_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(scopeId)));
	if (!q.exec()) {
		LOG(("DedupDb: FindJob failed: %1").arg(q.lastError().text()));
		return std::nullopt;
	}
	return q.next() ? ReadJob(q) : std::nullopt;
}

void DedupDb::Impl::setJobState(uint64 jobId, JobState state) {
	if (!_open || !jobId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE jobs SET state = :state, updated_at = :updated_at "
		"WHERE job_id = :job_id"_q);
	q.bindValue(u":state"_q, int(state));
	q.bindValue(u":updated_at"_q, QVariant::fromValue(
		static_cast<qlonglong>(base::unixtime::now())));
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	if (!q.exec()) {
		LOG(("DedupDb: SetJobState failed: %1").arg(q.lastError().text()));
	}
}

void DedupDb::Impl::setJobProgress(
		uint64 jobId,
		int total,
		int done,
		int skipped) {
	if (!_open || !jobId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE jobs SET total = :total, done = :done, "
		"skipped = :skipped, updated_at = :updated_at "
		"WHERE job_id = :job_id"_q);
	q.bindValue(u":total"_q, total);
	q.bindValue(u":done"_q, done);
	q.bindValue(u":skipped"_q, skipped);
	q.bindValue(u":updated_at"_q, QVariant::fromValue(
		static_cast<qlonglong>(base::unixtime::now())));
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	if (!q.exec()) {
		LOG(("DedupDb: SetJobProgress failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::pauseJob(uint64 jobId) {
	setJobState(jobId, JobState::Pending);
}

void DedupDb::Impl::finishJob(uint64 jobId) {
	removeJob(jobId);
}

void DedupDb::Impl::cancelJob(uint64 jobId) {
	removeJob(jobId);
}

void DedupDb::Impl::finishScanJob(uint64 jobId) {
	setJobState(jobId, JobState::Done);
	removeJobItems(jobId);
}

void DedupDb::Impl::removeJobItems(uint64 jobId) {
	if (!_open || !jobId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM job_items WHERE job_id = :job_id"_q);
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	if (!q.exec()) {
		LOG(("DedupDb: RemoveJobItems failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::removeJob(uint64 jobId) {
	if (!_open || !jobId) {
		return;
	}
	beginTransaction();
	removeJobItems(jobId);
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM jobs WHERE job_id = :job_id"_q);
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	if (!q.exec()) {
		LOG(("DedupDb: RemoveJob failed: %1").arg(q.lastError().text()));
	}
	commitTransaction();
}

void DedupDb::Impl::writeJobItems(
		const std::vector<JobItemRecord> &items) {
	if (!_open || items.empty()) {
		return;
	}
	beginTransaction();
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO job_items "
		"(job_id, item_id, media_id, is_photo, hash_id, state, "
		"payload, error, retry) "
		"VALUES (:job_id, :item_id, :media_id, :is_photo, :hash_id, "
		":state, :payload, :error, :retry)"_q);
	for (const auto &item : items) {
		if (!item.jobId || !item.itemId) {
			continue;
		}
		BindJobItem(q, item);
		if (!q.exec()) {
			LOG(("DedupDb: WriteJobItem failed: %1").arg(
				q.lastError().text()));
		}
	}
	commitTransaction();
}

void DedupDb::Impl::writeJobItem(const JobItemRecord &item) {
	if (!_open || !item.jobId || !item.itemId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO job_items "
		"(job_id, item_id, media_id, is_photo, hash_id, state, "
		"payload, error, retry) "
		"VALUES (:job_id, :item_id, :media_id, :is_photo, :hash_id, "
		":state, :payload, :error, :retry)"_q);
	BindJobItem(q, item);
	if (!q.exec()) {
		LOG(("DedupDb: WriteJobItem failed: %1").arg(
			q.lastError().text()));
	}
}

std::vector<JobItemRecord> DedupDb::Impl::jobItems(uint64 jobId) const {
	return readJobItems(jobId, false);
}

std::vector<JobItemRecord> DedupDb::Impl::jobResumeItems(
		uint64 jobId) const {
	return readJobItems(jobId, true);
}

std::vector<JobItemRecord> DedupDb::Impl::readJobItems(
		uint64 jobId,
		bool resumeOnly) const {
	auto result = std::vector<JobItemRecord>();
	if (!_open || !jobId) {
		return result;
	}
	auto sql = u"SELECT job_id, item_id, media_id, is_photo, hash_id, "
		"state, payload, error, retry FROM job_items "
		"WHERE job_id = :job_id"_q;
	if (resumeOnly) {
		sql += u" AND state IN (0, 1)"_q;
	}
	sql += u" ORDER BY item_id"_q;
	QSqlQuery q(_db);
	q.prepare(sql);
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	if (!q.exec()) {
		LOG(("DedupDb: ReadJobItems failed: %1").arg(q.lastError().text()));
		return result;
	}
	while (q.next()) {
		result.push_back(ReadJobItem(q));
	}
	return result;
}

void DedupDb::Impl::setJobItemState(
		uint64 jobId,
		uint64 itemId,
		JobState state,
		const QString &error,
		int retry) {
	if (!_open || !jobId || !itemId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE job_items SET state = :state, error = :error, "
		"retry = :retry WHERE job_id = :job_id AND item_id = :item_id"_q);
	q.bindValue(u":state"_q, int(state));
	q.bindValue(u":error"_q, error);
	q.bindValue(u":retry"_q, retry);
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	q.bindValue(u":item_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(itemId)));
	if (!q.exec()) {
		LOG(("DedupDb: SetJobItemState failed: %1").arg(
			q.lastError().text()));
	}
}

bool DedupDb::Impl::jobContainsMedia(
		uint64 jobId,
		uint64 mediaId,
		bool isPhoto) const {
	if (!_open || !jobId || !mediaId) {
		return false;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT 1 FROM job_items WHERE job_id = :job_id "
		"AND media_id = :media_id AND is_photo = :is_photo "
		"AND state IN (2, 3) LIMIT 1"_q);
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(mediaId)));
	q.bindValue(u":is_photo"_q, isPhoto ? 1 : 0);
	return q.exec() && q.next();
}

bool DedupDb::Impl::jobContainsHash(
		uint64 jobId,
		uint64 hashId) const {
	if (!_open || !jobId || !hashId) {
		return false;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT 1 FROM job_items WHERE job_id = :job_id "
		"AND hash_id = :hash_id AND state IN (2, 3) LIMIT 1"_q);
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	q.bindValue(u":hash_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(hashId)));
	return q.exec() && q.next();
}

uint64 DedupDb::Impl::findJobId(
		JobKind kind,
		uint64 sessionId,
		uint64 scopeId) const {
	if (!_open) {
		return 0;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT job_id FROM jobs WHERE kind = :kind "
		"AND session_id = :session_id AND scope_id = :scope_id "
		"ORDER BY job_id DESC LIMIT 1"_q);
	q.bindValue(u":kind"_q, JobKindName(kind));
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":scope_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(scopeId)));
	if (!q.exec() || !q.next()) {
		return 0;
	}
	return q.value(0).toULongLong();
}

std::vector<std::pair<uint64, ExResumeRecord>>
DedupDb::Impl::loadExportJobs(uint64 sessionId) const {
	auto result = std::vector<std::pair<uint64, ExResumeRecord>>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT job_id, options FROM jobs "
		"WHERE kind = :kind AND session_id = :session_id "
		"ORDER BY job_id"_q);
	q.bindValue(u":kind"_q, JobKindName(JobKind::Export));
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: LoadExportJobs failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		if (auto record = DeserializeExResume(q.value(1).toByteArray())) {
			result.emplace_back(
				q.value(0).toULongLong(),
				std::move(*record));
		}
	}
	return result;
}

uint64 DedupDb::Impl::findExportJob(
		uint64 sessionId,
		const ExResumeRecord &record) const {
	if (!_open) {
		return 0;
	}
	const auto scope = ExResumeScope(
		record.peerId,
		record.mode,
		record.exportFolder);
	QSqlQuery q(_db);
	q.prepare(u"SELECT job_id, options FROM jobs "
		"WHERE kind = :kind AND session_id = :session_id "
		"AND scope_id = :scope_id ORDER BY job_id"_q);
	q.bindValue(u":kind"_q, JobKindName(JobKind::Export));
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":scope_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(scope)));
	if (!q.exec()) {
		LOG(("DedupDb: FindExportJob failed: %1").arg(
			q.lastError().text()));
		return 0;
	}
	while (q.next()) {
		const auto candidate = DeserializeExResume(q.value(1).toByteArray());
		if (candidate
			&& candidate->peerId == record.peerId
			&& candidate->mode == record.mode
			&& candidate->exportFolder == record.exportFolder) {
			return q.value(0).toULongLong();
		}
	}
	return 0;
}

void DedupDb::Impl::updateExportJob(
		uint64 jobId,
		const ExResumeRecord &record) {
	if (!_open || !jobId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE jobs SET scope_id = :scope_id, "
		"options = :options, state = :state, total = :total, "
		"done = :done, skipped = :skipped, updated_at = :updated_at "
		"WHERE job_id = :job_id"_q);
	q.bindValue(u":scope_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(ExResumeScope(
			record.peerId,
			record.mode,
			record.exportFolder))));
	q.bindValue(u":options"_q, SerializeExResume(record));
	q.bindValue(u":state"_q, int(ExportJobState(record.state)));
	q.bindValue(u":total"_q, record.total);
	q.bindValue(u":done"_q, record.msgsDone);
	q.bindValue(u":skipped"_q, record.skipped);
	q.bindValue(u":updated_at"_q, QVariant::fromValue(
		static_cast<qlonglong>(base::unixtime::now())));
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	if (!q.exec()) {
		LOG(("DedupDb: UpdateExportJob failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::purgeOrphanExportBatchJobs() {
	if (!_open) {
		return;
	}
	auto recordsBySession = std::map<uint64, std::vector<ExResumeRecord>>();
	{
		QSqlQuery q(_db);
		q.prepare(u"SELECT session_id, options FROM jobs "
			"WHERE kind = :kind"_q);
		q.bindValue(u":kind"_q, JobKindName(JobKind::Export));
		if (!q.exec()) {
			LOG(("DedupDb: PurgeExportBatch read failed: %1").arg(
				q.lastError().text()));
			return;
		}
		while (q.next()) {
			if (auto record = DeserializeExResume(q.value(1).toByteArray())) {
				recordsBySession[q.value(0).toULongLong()].push_back(
					std::move(*record));
			}
		}
	}
	auto orphaned = std::vector<uint64>();
	{
		QSqlQuery q(_db);
		q.prepare(u"SELECT job_id, session_id, scope_id FROM jobs "
			"WHERE kind = :kind"_q);
		q.bindValue(u":kind"_q, JobKindName(JobKind::ExportBatch));
		if (!q.exec()) {
			LOG(("DedupDb: PurgeExportBatch list failed: %1").arg(
				q.lastError().text()));
			return;
		}
		while (q.next()) {
			const auto jobId = q.value(0).toULongLong();
			const auto sessionId = q.value(1).toULongLong();
			const auto peerId = PeerId(q.value(2).toULongLong());
			const auto i = recordsBySession.find(sessionId);
			auto found = false;
			if (i != recordsBySession.end()) {
				for (const auto &record : i->second) {
					if (record.peerId == peerId) {
						found = true;
						break;
					}
				}
			}
			if (!found) {
				orphaned.push_back(jobId);
			}
		}
	}
	for (const auto jobId : orphaned) {
		removeJob(jobId);
	}
}

void DedupDb::Impl::purgeOrphanBatchJobs(
		JobKind batchKind,
		const std::vector<JobKind> &ownerKinds) {
	if (!_open) {
		return;
	}
	auto owners = std::set<std::pair<uint64, uint64>>();
	for (const auto kind : ownerKinds) {
		QSqlQuery q(_db);
		q.prepare(u"SELECT session_id, scope_id FROM jobs "
			"WHERE kind = :kind"_q);
		q.bindValue(u":kind"_q, JobKindName(kind));
		if (!q.exec()) {
			LOG(("DedupDb: PurgeBatch owners failed: %1").arg(
				q.lastError().text()));
			return;
		}
		while (q.next()) {
			owners.emplace(
				q.value(0).toULongLong(),
				q.value(1).toULongLong());
		}
	}
	auto orphaned = std::vector<uint64>();
	{
		QSqlQuery q(_db);
		q.prepare(u"SELECT job_id, session_id, scope_id FROM jobs "
			"WHERE kind = :kind"_q);
		q.bindValue(u":kind"_q, JobKindName(batchKind));
		if (!q.exec()) {
			LOG(("DedupDb: PurgeBatch list failed: %1").arg(
				q.lastError().text()));
			return;
		}
		while (q.next()) {
			const auto key = std::make_pair(
				q.value(1).toULongLong(),
				q.value(2).toULongLong());
			if (!owners.contains(key)) {
				orphaned.push_back(q.value(0).toULongLong());
			}
		}
	}
	for (const auto jobId : orphaned) {
		removeJob(jobId);
	}
}

void DedupDb::Impl::insert(Table table, const DedupRecord &record) {
	auto loaded = false;
	{
		std::lock_guard<std::mutex> lock(_shared->mutex);
		loaded = _shared->state[int(table)].loaded;
	}
	if (loaded) {
		insertLoaded(table, record);
	} else {
		insertDeferred(table, record);
	}
}

void DedupDb::Impl::insertLoaded(
		Table table,
		const DedupRecord &record) {
	const auto type = record.isPhoto ? 1 : 0;
	auto exists = false;
	auto oldHash = QByteArray();
	auto oldStatus = QString();
	if (record.documentId) {
		std::lock_guard<std::mutex> lock(_shared->mutex);
		auto &s = _shared->state[int(table)];
		const auto i = s.docToHash[type].constFind(record.documentId);
		exists = (i != s.docToHash[type].constEnd());
		if (exists) {
			oldHash = i.value();
			oldStatus = s.unfinishedDocs[type].contains(record.documentId)
				? u"u"_q
				: u"f"_q;
		}
		if (!record.hash.isEmpty()) {
			if (exists && oldHash != record.hash && !oldHash.isEmpty()) {
				const auto hashes = s.hashToDoc[type].find(oldHash);
				if (hashes != s.hashToDoc[type].end()) {
					hashes.value().remove(record.documentId);
					if (hashes.value().isEmpty()) {
						s.hashToDoc[type].erase(hashes);
					}
				}
			}
			s.docToHash[type][record.documentId] = record.hash;
			s.hashToDoc[type][record.hash].insert(record.documentId);
		} else if (!exists) {
			s.docToHash[type].insert(record.documentId, QByteArray());
		}
		if (record.status == u"u") {
			s.unfinishedDocs[type].insert(record.documentId);
		} else {
			s.unfinishedDocs[type].remove(record.documentId);
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
	const auto hasRow = exists && !oldHash.isEmpty();
	if (hasRow && oldHash == record.hash && oldStatus == record.status) {
		return;
	}
	const auto hashId = resolveHashId(record.hash);
	if (!hashId) {
		return;
	}
	QSqlQuery q(_db);
	if (hasRow) {
		q.prepare(u"UPDATE " + TableName(table)
			+ u" SET hash_id = :hash_id, status = :status "
			"WHERE doc_id = :doc_id AND is_photo = :is_photo"_q);
	} else {
		q.prepare(u"INSERT INTO " + TableName(table)
			+ u" (doc_id, is_photo, hash_id, status) "
			"VALUES (:doc_id, :is_photo, :hash_id, :status)"_q);
	}
	q.bindValue(u":doc_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.documentId)));
	q.bindValue(u":is_photo"_q, type);
	q.bindValue(u":hash_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(hashId)));
	q.bindValue(u":status"_q, record.status);
	if (!q.exec()) {
		LOG(("DedupDb: Insert failed: %1").arg(q.lastError().text()));
	} else if (hasRow && oldHash != record.hash) {
		QSqlQuery select(_db);
		select.prepare(u"SELECT hash_id FROM hashes WHERE hash = :hash"_q);
		select.bindValue(u":hash"_q, oldHash);
		if (select.exec() && select.next()) {
			cleanupHashRefs(select.value(0).toULongLong());
		}
	}
}

void DedupDb::Impl::insertDeferred(
		Table table,
		const DedupRecord &record) {
	if (!_open || record.hash.isEmpty()) {
		return;
	}
	const auto hashId = resolveHashId(record.hash);
	if (!hashId) {
		return;
	}
	auto oldHashId = uint64(0);
	{
		QSqlQuery select(_db);
		select.prepare(u"SELECT hash_id FROM " + TableName(table)
			+ u" WHERE doc_id = :doc_id AND is_photo = :is_photo"_q);
		select.bindValue(u":doc_id"_q, QVariant::fromValue(
			static_cast<qlonglong>(record.documentId)));
		select.bindValue(u":is_photo"_q, record.isPhoto ? 1 : 0);
		if (select.exec() && select.next()) {
			oldHashId = select.value(0).toULongLong();
		}
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT INTO " + TableName(table)
		+ u" (doc_id, is_photo, hash_id, status) "
		"VALUES (:doc_id, :is_photo, :hash_id, :status) "
		"ON CONFLICT(doc_id, is_photo) DO UPDATE SET "
		"hash_id = excluded.hash_id, status = excluded.status"_q);
	q.bindValue(u":doc_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(record.documentId)));
	q.bindValue(u":is_photo"_q, record.isPhoto ? 1 : 0);
	q.bindValue(u":hash_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(hashId)));
	q.bindValue(u":status"_q, record.status);
	if (!q.exec()) {
		LOG(("DedupDb: Deferred insert failed: %1").arg(
			q.lastError().text()));
	} else if (oldHashId && oldHashId != hashId) {
		cleanupHashRefs(oldHashId);
	}
}

void DedupDb::Impl::forgetLoadedDoc(
		Table table,
		uint64 documentId,
		bool isPhoto) {
	std::lock_guard<std::mutex> lock(_shared->mutex);
	auto &s = _shared->state[int(table)];
	if (!s.loaded) {
		return;
	}
	const auto type = isPhoto ? 1 : 0;
	const auto it = s.docToHash[type].find(documentId);
	if (it == s.docToHash[type].end()) {
		return;
	}
	const auto hash = it.value();
	s.docToHash[type].erase(it);
	s.unfinishedDocs[type].remove(documentId);
	const auto hashes = s.hashToDoc[type].find(hash);
	if (hashes != s.hashToDoc[type].end()) {
		hashes.value().remove(documentId);
		if (hashes.value().isEmpty()) {
			s.hashToDoc[type].erase(hashes);
		}
	}
}

bool DedupDb::Impl::containsDocId(
		Table table,
		uint64 documentId,
		bool isPhoto) const {
	ensureLoaded(table);
	std::lock_guard<std::mutex> lock(_shared->mutex);
	const auto type = isPhoto ? 1 : 0;
	return _shared->state[int(table)].docToHash[type].contains(documentId);
}

bool DedupDb::Impl::containsDocIdInDb(
		Table table,
		uint64 documentId,
		bool isPhoto) const {
	if (!_open || !documentId) {
		return false;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT EXISTS(SELECT 1 FROM " + TableName(table)
		+ u" WHERE doc_id = :doc_id AND is_photo = :is_photo)"_q);
	q.bindValue(u":doc_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(documentId)));
	q.bindValue(u":is_photo"_q, isPhoto ? 1 : 0);
	if (!q.exec() || !q.next()) {
		return false;
	}
	return q.value(0).toBool();
}

bool DedupDb::Impl::containsHash(Table table, const QByteArray &hash) const {
	ensureLoaded(table);
	std::lock_guard<std::mutex> lock(_shared->mutex);
	const auto &s = _shared->state[int(table)];
	return s.hashToDoc[0].contains(hash) || s.hashToDoc[1].contains(hash);
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
	for (auto type = 0; type != 2; ++type) {
		const auto it = s.hashToDoc[type].find(hash);
		if (it == s.hashToDoc[type].end()) {
			continue;
		}
		for (const auto docId : it.value()) {
			if (!s.unfinishedDocs[type].contains(docId)) {
				return true;
			}
		}
	}
	return false;
}

void DedupDb::Impl::rekey(
		Table table,
		uint64 oldDocumentId,
		uint64 newDocumentId,
		bool isPhoto) {
	if (!oldDocumentId || !newDocumentId || oldDocumentId == newDocumentId) {
		return;
	}
	const auto type = isPhoto ? 1 : 0;
	{
		std::lock_guard<std::mutex> lock(_shared->mutex);
		auto &s = _shared->state[int(table)];
		const auto hadOld = s.docToHash[type].contains(oldDocumentId);
		const auto hash = s.docToHash[type].take(oldDocumentId);
		const auto wasUnfinished = s.unfinishedDocs[type].remove(
			oldDocumentId);
		if (hadOld) {
			s.docToHash[type].insert(
				newDocumentId,
				hash.isEmpty() ? QByteArray() : hash);
		}
		if (wasUnfinished) {
			s.unfinishedDocs[type].insert(newDocumentId);
		}
		if (!hash.isEmpty()) {
			const auto hashes = s.hashToDoc[type].find(hash);
			if (hashes != s.hashToDoc[type].end()) {
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
		"WHERE doc_id = :old_doc_id AND is_photo = :is_photo"_q);
	q.bindValue(u":new_doc_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(newDocumentId)));
	q.bindValue(u":old_doc_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(oldDocumentId)));
	q.bindValue(u":is_photo"_q, type);
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
		for (auto type = 0; type != 2; ++type) {
			const auto hashes = s.hashToDoc[type].find(hash);
			if (hashes == s.hashToDoc[type].end()) {
				continue;
			}
			for (const auto docId : hashes.value()) {
				if (status == u"u") {
					s.unfinishedDocs[type].insert(docId);
				} else {
					s.unfinishedDocs[type].remove(docId);
				}
			}
		}
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE " + TableName(table)
		+ u" SET status = :status "
		"WHERE hash_id = (SELECT hash_id FROM hashes WHERE hash = :hash)"_q);
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
		for (auto type = 0; type != 2; ++type) {
			const auto hashes = s.hashToDoc[type].find(hash);
			if (hashes == s.hashToDoc[type].end()) {
				continue;
			}
			const auto ids = hashes.value();
			for (const auto docId : ids) {
				if (!s.unfinishedDocs[type].contains(docId)) {
					continue;
				}
				s.docToHash[type].remove(docId);
				s.unfinishedDocs[type].remove(docId);
				hashes.value().remove(docId);
			}
			if (hashes.value().isEmpty()) {
				s.hashToDoc[type].erase(hashes);
			}
		}
	}
	if (!_open) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM " + TableName(table)
		+ u" WHERE hash_id = (SELECT hash_id FROM hashes WHERE hash = :hash) "
		"AND status = 'u'"_q);
	q.bindValue(u":hash"_q, hash);
	if (!q.exec()) {
		LOG(("DedupDb: RemoveUnfinishedByHash failed: %1").arg(
			q.lastError().text()));
	} else if (table == Table::Uploads && q.numRowsAffected() > 0) {
		QSqlQuery select(_db);
		select.prepare(u"SELECT hash_id FROM hashes WHERE hash = :hash"_q);
		select.bindValue(u":hash"_q, hash);
		if (select.exec() && select.next()) {
			cleanupHashRefs(select.value(0).toULongLong());
		}
	}
}

QByteArray DedupDb::Impl::hashForDocId(
		Table table,
		uint64 documentId,
		bool isPhoto) const {
	ensureLoaded(table);
	std::lock_guard<std::mutex> lock(_shared->mutex);
	const auto type = isPhoto ? 1 : 0;
	return _shared->state[int(table)].docToHash[type].value(documentId);
}

uint64 DedupDb::Impl::seekDocumentId(
		Table table,
		const QByteArray &hash,
		uint64 excludeDocumentId,
		bool excludeIsPhoto) const {
	ensureLoaded(table);
	std::lock_guard<std::mutex> lock(_shared->mutex);
	const auto &s = _shared->state[int(table)];
	for (auto type = 0; type != 2; ++type) {
		const auto it = s.hashToDoc[type].find(hash);
		if (it == s.hashToDoc[type].end()) {
			continue;
		}
		for (const auto docId : it.value()) {
			if (docId != excludeDocumentId
				|| type != (excludeIsPhoto ? 1 : 0)) {
				return docId;
			}
		}
	}
	return 0;
}

void DedupDb::Impl::addPending(
		Table table,
		uint64 documentId,
		const QByteArray &hash,
		bool isPhoto) {
	insert(table, {
		.hash = hash,
		.documentId = documentId,
		.isPhoto = isPhoto,
		.status = u"u"_q,
	});
}

void DedupDb::Impl::removePending(
		Table table,
		uint64 documentId,
		bool isPhoto) {
	const auto type = isPhoto ? 1 : 0;
	{
		std::lock_guard<std::mutex> lock(_shared->mutex);
		auto &s = _shared->state[int(table)];
		if (s.loaded) {
			const auto it = s.docToHash[type].constFind(documentId);
			if (it != s.docToHash[type].constEnd()
				&& s.unfinishedDocs[type].contains(documentId)) {
				const auto hash = it.value();
				s.docToHash[type].remove(documentId);
				s.unfinishedDocs[type].remove(documentId);
				const auto hashes = s.hashToDoc[type].find(hash);
				if (hashes != s.hashToDoc[type].end()) {
					hashes.value().remove(documentId);
					if (hashes.value().isEmpty()) {
						s.hashToDoc[type].erase(hashes);
					}
				}
			}
		}
	}
	if (!_open || !documentId) {
		return;
	}
	auto hashId = uint64(0);
	{
		QSqlQuery select(_db);
		select.prepare(u"SELECT hash_id FROM " + TableName(table)
			+ u" WHERE doc_id = :doc_id AND is_photo = :is_photo "
			"AND status = 'u'"_q);
		select.bindValue(u":doc_id"_q, QVariant::fromValue(
			static_cast<qlonglong>(documentId)));
		select.bindValue(u":is_photo"_q, type);
		if (select.exec() && select.next()) {
			hashId = select.value(0).toULongLong();
		}
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM " + TableName(table)
		+ u" WHERE doc_id = :doc_id AND is_photo = :is_photo "
		"AND status = 'u'"_q);
	q.bindValue(u":doc_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(documentId)));
	q.bindValue(u":is_photo"_q, type);
	if (!q.exec()) {
		LOG(("DedupDb: Remove pending failed: %1").arg(
			q.lastError().text()));
	} else if (table == Table::Uploads
		&& q.numRowsAffected() > 0
		&& hashId) {
		cleanupHashRefs(hashId);
	}
}

std::vector<DedupRecord> DedupDb::Impl::loadAll(Table table) const {
	ensureLoaded(table);
	{
		std::lock_guard<std::mutex> lock(_shared->mutex);
		const auto &s = _shared->state[int(table)];
		if (s.loaded) {
			auto result = std::vector<DedupRecord>();
			auto total = 0;
			for (auto type = 0; type != 2; ++type) {
				total += s.docToHash[type].size();
			}
			result.reserve(total);
			for (auto type = 0; type != 2; ++type) {
				for (auto i = s.docToHash[type].constBegin();
					i != s.docToHash[type].constEnd();
					++i) {
					result.push_back({
						.hash = i.value(),
						.documentId = i.key(),
						.isPhoto = (type == 1),
					});
				}
			}
			return result;
		}
	}
	auto result = std::vector<DedupRecord>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	if (!q.exec(u"SELECT h.hash, d.doc_id, d.is_photo, d.status FROM "
			+ TableName(table)
			+ u" d JOIN hashes h ON h.hash_id = d.hash_id"_q)) {
		LOG(("DedupDb: LoadAll failed: %1").arg(q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto record = DedupRecord();
		record.hash = q.value(0).toByteArray();
		record.documentId = q.value(1).toULongLong();
		record.isPhoto = (q.value(2).toInt() != 0);
		record.status = q.value(3).toString();
		result.push_back(std::move(record));
	}
	return result;
}

void DedupDb::Impl::refreshHashRefs(uint64 hashId) {
	if (!_open || !hashId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE hashes SET hash_refs = "
		"(SELECT COUNT(*) FROM chat_scan_items WHERE hash_id = :hash_id) "
		"WHERE hash_id = :hash_id"_q);
	q.bindValue(u":hash_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(hashId)));
	if (!q.exec()) {
		LOG(("DedupDb: RefreshHashRefs failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::refreshMediaRefs(uint64 mediaId, bool isPhoto) {
	if (!_open || !mediaId) {
		return;
	}
	const auto type = isPhoto ? 1 : 0;
	QSqlQuery count(_db);
	count.prepare(u"SELECT COUNT(*), COALESCE(MAX(NULLIF(hash_id, 0)), 0) "
		"FROM chat_scan_items "
		"WHERE media_id = :media_id AND is_photo = :is_photo"_q);
	count.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(mediaId)));
	count.bindValue(u":is_photo"_q, type);
	if (!count.exec() || !count.next()) {
		LOG(("DedupDb: RefreshMediaRefs count failed: %1").arg(
			count.lastError().text()));
		return;
	}
	const auto refs = count.value(0).toLongLong();
	const auto hashId = count.value(1).toULongLong();
	if (refs > 0) {
		QSqlQuery q(_db);
		q.prepare(u"INSERT OR REPLACE INTO media_refs "
			"(media_id, is_photo, hash_id, id_refs) "
			"VALUES (:media_id, :is_photo, :hash_id, :id_refs)"_q);
		q.bindValue(u":media_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(mediaId)));
		q.bindValue(u":is_photo"_q, type);
		q.bindValue(u":hash_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(hashId)));
		q.bindValue(u":id_refs"_q, QVariant::fromValue(refs));
		if (!q.exec()) {
			LOG(("DedupDb: RefreshMediaRefs failed: %1").arg(
				q.lastError().text()));
		}
	} else {
		QSqlQuery q(_db);
		q.prepare(u"UPDATE media_refs SET hash_id = 0, id_refs = 0 "
			"WHERE media_id = :media_id AND is_photo = :is_photo"_q);
		q.bindValue(u":media_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(mediaId)));
		q.bindValue(u":is_photo"_q, type);
		if (!q.exec()) {
			LOG(("DedupDb: RefreshMediaRefs clear failed: %1").arg(
				q.lastError().text()));
		}
	}
}

void DedupDb::Impl::cleanupMediaRefs(uint64 mediaId, bool isPhoto) {
	if (!_open || !mediaId) {
		return;
	}
	const auto type = isPhoto ? 1 : 0;
	{
		QSqlQuery check(_db);
		check.prepare(u"SELECT id_refs FROM media_refs "
			"WHERE media_id = :media_id AND is_photo = :is_photo"_q);
		check.bindValue(u":media_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(mediaId)));
		check.bindValue(u":is_photo"_q, type);
		if (!check.exec() || !check.next()) {
			return;
		} else if (check.value(0).toLongLong() > 0) {
			return;
		}
	}
	auto hashIds = std::set<uint64>();
	{
		QSqlQuery q(_db);
		q.prepare(u"SELECT hash_id FROM dedup_ul "
			"WHERE doc_id = :media_id AND is_photo = :is_photo "
			"AND status = 'f'"_q);
		q.bindValue(u":media_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(mediaId)));
		q.bindValue(u":is_photo"_q, type);
		if (q.exec()) {
			while (q.next()) {
				const auto hashId = q.value(0).toULongLong();
				if (hashId) {
					hashIds.insert(hashId);
				}
			}
		} else {
			LOG(("DedupDb: CleanupMediaRefs select failed: %1").arg(
				q.lastError().text()));
		}
	}
	{
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM dedup_ul "
			"WHERE doc_id = :media_id AND is_photo = :is_photo "
			"AND status = 'f' "
			"AND NOT EXISTS (SELECT 1 FROM chat_scan_items "
				"WHERE media_id = :media_id AND is_photo = :is_photo)"_q);
		q.bindValue(u":media_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(mediaId)));
		q.bindValue(u":is_photo"_q, type);
		if (!q.exec()) {
			LOG(("DedupDb: CleanupMediaRefs delete failed: %1").arg(
				q.lastError().text()));
		} else if (q.numRowsAffected() > 0) {
			forgetLoadedDoc(Table::Uploads, mediaId, isPhoto);
		}
	}
	{
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM media_refs "
			"WHERE media_id = :media_id AND is_photo = :is_photo "
			"AND id_refs = 0 "
			"AND NOT EXISTS (SELECT 1 FROM dedup_ul "
				"WHERE doc_id = :media_id AND is_photo = :is_photo "
				"AND status = 'u')"_q);
		q.bindValue(u":media_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(mediaId)));
		q.bindValue(u":is_photo"_q, type);
		if (!q.exec()) {
			LOG(("DedupDb: CleanupMediaRefs row failed: %1").arg(
				q.lastError().text()));
		}
	}
	for (const auto hashId : hashIds) {
		cleanupHashRefs(hashId);
	}
}

void DedupDb::Impl::cleanupHashRefs(uint64 hashId) {
	if (!_open || !hashId) {
		return;
	}
	refreshHashRefs(hashId);
	auto removed = std::vector<std::pair<uint64, bool>>();
	auto selectionFailed = false;
	{
		QSqlQuery select(_db);
		select.prepare(u"SELECT doc_id, is_photo FROM dedup_ul "
			"WHERE hash_id = :hash_id AND status = 'f' "
			"AND NOT EXISTS (SELECT 1 FROM chat_scan_items c "
				"WHERE c.media_id = dedup_ul.doc_id "
				"AND c.is_photo = dedup_ul.is_photo) "
			"AND NOT EXISTS (SELECT 1 FROM media_refs m "
				"WHERE m.media_id = dedup_ul.doc_id "
				"AND m.is_photo = dedup_ul.is_photo "
				"AND m.id_refs > 0)"_q);
		select.bindValue(u":hash_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(hashId)));
		if (select.exec()) {
			while (select.next()) {
				removed.emplace_back(
					select.value(0).toULongLong(),
					select.value(1).toInt() != 0);
			}
		} else {
			selectionFailed = true;
			LOG(("DedupDb: CleanupHashRefs select failed: %1").arg(
				select.lastError().text()));
		}
	}
	{
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM dedup_ul "
			"WHERE hash_id = :hash_id AND status = 'f' "
			"AND NOT EXISTS (SELECT 1 FROM chat_scan_items c "
				"WHERE c.media_id = dedup_ul.doc_id "
				"AND c.is_photo = dedup_ul.is_photo) "
			"AND NOT EXISTS (SELECT 1 FROM media_refs m "
				"WHERE m.media_id = dedup_ul.doc_id "
				"AND m.is_photo = dedup_ul.is_photo "
				"AND m.id_refs > 0)"_q);
		q.bindValue(u":hash_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(hashId)));
		if (!q.exec()) {
			LOG(("DedupDb: CleanupHashRefs uploads failed: %1").arg(
				q.lastError().text()));
		} else if (q.numRowsAffected() > 0) {
			if (selectionFailed) {
				invalidateTable(Table::Uploads);
			} else {
				for (const auto &[docId, isPhoto] : removed) {
					forgetLoadedDoc(Table::Uploads, docId, isPhoto);
				}
			}
		}
	}
	{
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM media_refs "
			"WHERE hash_id = :hash_id AND id_refs = 0 "
			"AND NOT EXISTS (SELECT 1 FROM dedup_ul "
				"WHERE doc_id = media_refs.media_id "
				"AND is_photo = media_refs.is_photo "
				"AND status = 'u')"_q);
		q.bindValue(u":hash_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(hashId)));
		if (!q.exec()) {
			LOG(("DedupDb: CleanupHashRefs media failed: %1").arg(
				q.lastError().text()));
		}
	}
	{
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM hashes "
			"WHERE hash_id = :hash_id AND hash_refs = 0 "
			"AND NOT EXISTS (SELECT 1 FROM chat_scan_items "
				"WHERE hash_id = :hash_id) "
			"AND NOT EXISTS (SELECT 1 FROM dedup_ul "
				"WHERE hash_id = :hash_id) "
			"AND NOT EXISTS (SELECT 1 FROM dedup_dl "
				"WHERE hash_id = :hash_id) "
			"AND NOT EXISTS (SELECT 1 FROM media_refs "
				"WHERE hash_id = :hash_id AND id_refs > 0) "
			"AND NOT EXISTS (SELECT 1 FROM job_items "
				"WHERE hash_id = :hash_id)"_q);
		q.bindValue(u":hash_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(hashId)));
		if (!q.exec()) {
			LOG(("DedupDb: CleanupHashRefs hash failed: %1").arg(
				q.lastError().text()));
		}
	}
}

void DedupDb::Impl::refreshChatScanRefs(
		const std::set<uint64> &hashIds,
		const std::set<std::pair<uint64, bool>> &media) {
	if (!_open) {
		return;
	}
	for (const auto hashId : hashIds) {
		refreshHashRefs(hashId);
	}
	for (const auto &[mediaId, isPhoto] : media) {
		refreshMediaRefs(mediaId, isPhoto);
	}
	for (const auto &[mediaId, isPhoto] : media) {
		cleanupMediaRefs(mediaId, isPhoto);
	}
	for (const auto hashId : hashIds) {
		cleanupHashRefs(hashId);
	}
}

void DedupDb::Impl::insertDlResume(const DlResumeRecord &record) {
	if (!_open) {
		return;
	}
	auto jobId = findJobId(
		JobKind::Download,
		record.sessionId,
		record.peerId);
	if (!jobId) {
		auto job = JobRecord();
		job.kind = JobKind::Download;
		job.sessionId = record.sessionId;
		job.scopeId = record.peerId;
		job.state = JobState::Pending;
		jobId = createJob(job);
		if (!jobId) {
			return;
		}
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO job_items "
		"(job_id, item_id, media_id, is_photo, hash_id, state, payload) "
		"VALUES (:job_id, :item_id, :media_id, 0, 0, :state, :payload)"_q);
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	q.bindValue(u":item_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.msgId)));
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.docId)));
	q.bindValue(u":state"_q, int(JobState::Pending));
	q.bindValue(u":payload"_q, SerializeDlResumePayload(record));
	if (!q.exec()) {
		LOG(("DedupDb: InsertDlResume failed: %1").arg(q.lastError().text()));
		return;
	}
}

void DedupDb::Impl::removeDlResume(
		uint64 sessionId,
		uint64 peerId,
		int64 msgId) {
	if (!_open) {
		return;
	}
	const auto jobId = findJobId(JobKind::Download, sessionId, peerId);
	if (!jobId) {
		return;
	}
	{
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM job_items "
			"WHERE job_id = :job_id AND item_id = :item_id"_q);
		q.bindValue(u":job_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(jobId)));
		q.bindValue(u":item_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(msgId)));
		if (!q.exec()) {
			LOG(("DedupDb: RemoveDlResume failed: %1").arg(
				q.lastError().text()));
			return;
		}
	}
	{
		QSqlQuery q(_db);
		q.prepare(u"SELECT 1 FROM job_items "
			"WHERE job_id = :job_id LIMIT 1"_q);
		q.bindValue(u":job_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(jobId)));
		if (q.exec() && !q.next()) {
			removeJob(jobId);
			return;
		}
	}
}

std::vector<DlResumeRecord> DedupDb::Impl::loadAllDlResume() const {
	auto result = std::vector<DlResumeRecord>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT j.session_id, j.scope_id, i.item_id, "
		"i.media_id, i.payload FROM jobs j "
		"JOIN job_items i ON i.job_id = j.job_id "
		"WHERE j.kind = :kind ORDER BY j.job_id, i.item_id"_q);
	q.bindValue(u":kind"_q, JobKindName(JobKind::Download));
	if (!q.exec()) {
		LOG(("DedupDb: LoadAllDlResume failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		auto record = DlResumeRecord();
		record.sessionId = q.value(0).toULongLong();
		record.peerId = q.value(1).toULongLong();
		record.msgId = q.value(2).toLongLong();
		record.docId = q.value(3).toULongLong();
		if (!ReadDlResumePayload(q.value(4).toByteArray(), record)) {
			continue;
		}
		result.push_back(std::move(record));
	}
	return result;
}

void DedupDb::Impl::insertUlResume(const UlResumeRecord &record) {
	if (!_open) {
		return;
	}
	auto jobId = findJobId(
		JobKind::Upload,
		record.sessionId,
		record.peerId);
	if (!jobId) {
		auto job = JobRecord();
		job.kind = JobKind::Upload;
		job.sessionId = record.sessionId;
		job.scopeId = record.peerId;
		job.state = JobState::Pending;
		jobId = createJob(job);
		if (!jobId) {
			return;
		}
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO job_items "
		"(job_id, item_id, media_id, is_photo, hash_id, state, payload) "
		"VALUES (:job_id, :item_id, :media_id, 0, 0, :state, :payload)"_q);
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	q.bindValue(u":item_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(UlResumeItemId(record.path))));
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(record.fileId)));
	q.bindValue(u":state"_q, int(JobState::Pending));
	q.bindValue(u":payload"_q, SerializeUlResume(record));
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
	const auto jobId = findJobId(JobKind::Upload, sessionId, peerId);
	if (!jobId) {
		return;
	}
	{
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM job_items "
			"WHERE job_id = :job_id AND item_id = :item_id"_q);
		q.bindValue(u":job_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(jobId)));
		q.bindValue(u":item_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(UlResumeItemId(path))));
		if (!q.exec()) {
			LOG(("DedupDb: RemoveUlResume failed: %1").arg(
				q.lastError().text()));
			return;
		}
	}
	{
		QSqlQuery q(_db);
		q.prepare(u"SELECT 1 FROM job_items "
			"WHERE job_id = :job_id LIMIT 1"_q);
		q.bindValue(u":job_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(jobId)));
		if (q.exec() && !q.next()) {
			removeJob(jobId);
		}
	}
}

void DedupDb::Impl::clearUlResume(uint64 sessionId) {
	if (!_open) {
		return;
	}
	beginTransaction();
	{
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM job_items WHERE job_id IN ("
			"SELECT job_id FROM jobs WHERE kind = :kind "
			"AND session_id = :session_id)"_q);
		q.bindValue(u":kind"_q, JobKindName(JobKind::Upload));
		q.bindValue(u":session_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(sessionId)));
		if (!q.exec()) {
			LOG(("DedupDb: ClearUlResume items failed: %1").arg(
				q.lastError().text()));
		}
	}
	{
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM jobs WHERE kind = :kind "
			"AND session_id = :session_id"_q);
		q.bindValue(u":kind"_q, JobKindName(JobKind::Upload));
		q.bindValue(u":session_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(sessionId)));
		if (!q.exec()) {
			LOG(("DedupDb: ClearUlResume failed: %1").arg(
				q.lastError().text()));
		}
	}
	commitTransaction();
}

std::vector<UlResumeRecord> DedupDb::Impl::loadAllUlResume(
		uint64 sessionId) const {
	auto result = std::vector<UlResumeRecord>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT j.scope_id, i.payload FROM jobs j "
		"JOIN job_items i ON i.job_id = j.job_id "
		"WHERE j.kind = :kind AND j.session_id = :session_id "
		"ORDER BY j.job_id, i.item_id"_q);
	q.bindValue(u":kind"_q, JobKindName(JobKind::Upload));
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: LoadAllUlResume failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		if (auto record = DeserializeUlResume(q.value(1).toByteArray())) {
			record->sessionId = sessionId;
			if (!record->peerId) {
				record->peerId = q.value(0).toULongLong();
			}
			result.push_back(std::move(*record));
		}
	}
	return result;
}

std::optional<std::pair<uint64, EfJobOptions>> DedupDb::Impl::findEfJob(
		uint64 sessionId,
		PeerId destPeerId,
		const QString &jobId) const {
	if (!_open) {
		return std::nullopt;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT job_id, options FROM jobs "
		"WHERE kind = :kind AND session_id = :session_id "
		"AND scope_id = :scope_id ORDER BY job_id DESC"_q);
	q.bindValue(u":kind"_q, JobKindName(JobKind::EnhancedForward));
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":scope_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(destPeerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: FindEfJob failed: %1").arg(q.lastError().text()));
		return std::nullopt;
	}
	while (q.next()) {
		if (auto options = DeserializeEfJobOptions(
				q.value(1).toByteArray())) {
			if (options->jobId == jobId) {
				return std::make_pair(
					q.value(0).toULongLong(),
					std::move(*options));
			}
		}
	}
	return std::nullopt;
}

std::vector<uint64> DedupDb::Impl::findEfJobIds(
		const QString &jobId) const {
	auto result = std::vector<uint64>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT job_id, options FROM jobs "
		"WHERE kind = :kind"_q);
	q.bindValue(u":kind"_q, JobKindName(JobKind::EnhancedForward));
	if (!q.exec()) {
		LOG(("DedupDb: FindEfJobIds failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		if (const auto options = DeserializeEfJobOptions(
				q.value(1).toByteArray())) {
			if (options->jobId == jobId) {
				result.push_back(q.value(0).toULongLong());
			}
		}
	}
	return result;
}

std::vector<EfResumeItem> DedupDb::Impl::readEfResumeItems(
		uint64 jobId,
		const EfJobOptions &options) const {
	auto result = std::vector<EfResumeItem>();
	if (!_open || !jobId) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT item_id, payload FROM job_items "
		"WHERE job_id = :job_id ORDER BY item_id"_q);
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	if (!q.exec()) {
		LOG(("DedupDb: ReadEfResumeItems failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		if (auto item = DeserializeEfResumeItem(
				q.value(1).toByteArray())) {
			item->jobId = options.jobId;
			item->peerId = options.destPeerId;
			item->paused = options.paused;
			result.push_back(std::move(*item));
		}
	}
	return result;
}

void DedupDb::Impl::updateEfJobOptions(
		uint64 jobId,
		const EfJobOptions &options) {
	if (!_open || !jobId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE jobs SET options = :options, "
		"updated_at = :updated_at WHERE job_id = :job_id"_q);
	q.bindValue(u":options"_q, SerializeEfJobOptions(options));
	q.bindValue(u":updated_at"_q, QVariant::fromValue(
		static_cast<qlonglong>(base::unixtime::now())));
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	if (!q.exec()) {
		LOG(("DedupDb: UpdateEfJobOptions failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::removeEfJobIfEmpty(uint64 jobId) {
	if (!_open || !jobId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT 1 FROM job_items WHERE job_id = :job_id LIMIT 1"_q);
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	if (q.exec() && !q.next()) {
		removeJob(jobId);
	}
}

void DedupDb::Impl::insertEfResumeItem(const EfResumeItem &item) {
	if (!_open) {
		return;
	}
	auto found = findEfJob(item.sessionId, item.peerId, item.jobId);
	auto jobId = found ? found->first : uint64(0);
	if (!jobId) {
		auto options = EfJobOptions{
			.jobId = item.jobId,
			.destPeerId = item.peerId,
			.srcPeerId = item.sourceId.peer,
			.paused = item.paused,
		};
		auto job = JobRecord();
		job.kind = JobKind::EnhancedForward;
		job.sessionId = item.sessionId;
		job.scopeId = item.peerId.value;
		job.options = SerializeEfJobOptions(options);
		job.state = JobState::Pending;
		jobId = createJob(job);
		if (!jobId) {
			return;
		}
	} else if (found->second.paused != item.paused) {
		found->second.paused = item.paused;
		updateEfJobOptions(jobId, found->second);
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO job_items "
		"(job_id, item_id, media_id, is_photo, hash_id, state, payload) "
		"VALUES (:job_id, :item_id, :media_id, 0, :hash_id, :state, "
		":payload)"_q);
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	q.bindValue(u":item_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(item.itemIndex)));
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(item.mediaId)));
	q.bindValue(u":hash_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(resolveHashId(item.fileHash))));
	q.bindValue(u":state"_q, int(JobState::Pending));
	q.bindValue(u":payload"_q, SerializeEfResumeItem(item));
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
	for (const auto dbJobId : findEfJobIds(jobId)) {
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM job_items "
			"WHERE job_id = :job_id AND item_id = :item_id"_q);
		q.bindValue(u":job_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(dbJobId)));
		q.bindValue(u":item_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(itemIndex)));
		if (!q.exec()) {
			LOG(("DedupDb: RemoveEfResumeItem failed: %1").arg(
				q.lastError().text()));
			continue;
		}
		removeEfJobIfEmpty(dbJobId);
	}
}

void DedupDb::Impl::removeEfResumeBySource(
		uint64 sessionId,
		PeerId destPeerId,
		MsgId sourceMsgId) {
	if (!_open || !sourceMsgId) {
		return;
	}
	auto jobIds = std::vector<uint64>();
	{
		QSqlQuery q(_db);
		q.prepare(u"SELECT job_id FROM jobs WHERE kind = :kind "
			"AND session_id = :session_id AND scope_id = :scope_id"_q);
		q.bindValue(u":kind"_q, JobKindName(JobKind::EnhancedForward));
		q.bindValue(u":session_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(sessionId)));
		q.bindValue(u":scope_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(destPeerId.value)));
		if (!q.exec()) {
			LOG(("DedupDb: RemoveEfResumeBySource jobs failed: %1").arg(
				q.lastError().text()));
			return;
		}
		while (q.next()) {
			jobIds.push_back(q.value(0).toULongLong());
		}
	}
	for (const auto jobId : jobIds) {
		auto itemIds = std::vector<uint64>();
		{
			QSqlQuery q(_db);
			q.prepare(u"SELECT item_id, payload FROM job_items "
				"WHERE job_id = :job_id"_q);
			q.bindValue(u":job_id"_q, QVariant::fromValue(
				static_cast<qulonglong>(jobId)));
			if (!q.exec()) {
				LOG(("DedupDb: RemoveEfResumeBySource items failed: %1").arg(
					q.lastError().text()));
				continue;
			}
			while (q.next()) {
				if (const auto item = DeserializeEfResumeItem(
						q.value(1).toByteArray())) {
					if (item->sourceId.msg == sourceMsgId) {
						itemIds.push_back(q.value(0).toULongLong());
					}
				}
			}
		}
		for (const auto itemId : itemIds) {
			QSqlQuery q(_db);
			q.prepare(u"DELETE FROM job_items "
				"WHERE job_id = :job_id AND item_id = :item_id"_q);
			q.bindValue(u":job_id"_q, QVariant::fromValue(
				static_cast<qulonglong>(jobId)));
			q.bindValue(u":item_id"_q, QVariant::fromValue(
				static_cast<qulonglong>(itemId)));
			if (!q.exec()) {
				LOG(("DedupDb: RemoveEfResumeBySource delete failed: %1").arg(
					q.lastError().text()));
			}
		}
		removeEfJobIfEmpty(jobId);
	}
}

void DedupDb::Impl::clearEfResumeForPeer(PeerId peerId) {
	if (!_open) {
		return;
	}
	auto jobIds = std::vector<uint64>();
	{
		QSqlQuery q(_db);
		q.prepare(u"SELECT job_id FROM jobs WHERE kind = :kind "
			"AND scope_id = :scope_id"_q);
		q.bindValue(u":kind"_q, JobKindName(JobKind::EnhancedForward));
		q.bindValue(u":scope_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		if (!q.exec()) {
			LOG(("DedupDb: ClearEfResumeForPeer failed: %1").arg(
				q.lastError().text()));
			return;
		}
		while (q.next()) {
			jobIds.push_back(q.value(0).toULongLong());
		}
	}
	for (const auto jobId : jobIds) {
		removeJob(jobId);
	}
}

void DedupDb::Impl::clearEfResumeJob(const QString &jobId) {
	if (!_open) {
		return;
	}
	for (const auto dbJobId : findEfJobIds(jobId)) {
		removeJob(dbJobId);
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
	q.prepare(u"SELECT job_id, options FROM jobs "
		"WHERE kind = :kind AND session_id = :session_id "
		"AND scope_id = :scope_id ORDER BY job_id"_q);
	q.bindValue(u":kind"_q, JobKindName(JobKind::EnhancedForward));
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	q.bindValue(u":scope_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: LoadEfResumeItemsForPeer failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		if (auto options = DeserializeEfJobOptions(
				q.value(1).toByteArray())) {
			for (auto &item : readEfResumeItems(
					q.value(0).toULongLong(),
					*options)) {
				item.peerId = peerId;
				result.push_back(std::move(item));
			}
		}
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
	q.prepare(u"SELECT job_id, options FROM jobs "
		"WHERE kind = :kind AND session_id = :session_id "
		"ORDER BY job_id"_q);
	q.bindValue(u":kind"_q, JobKindName(JobKind::EnhancedForward));
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: LoadUnfinishedEfResumeItems failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		if (auto options = DeserializeEfJobOptions(
				q.value(1).toByteArray())) {
			for (auto &item : readEfResumeItems(
					q.value(0).toULongLong(),
					*options)) {
				if (item.state != u"done"_q) {
					result.push_back(std::move(item));
				}
			}
		}
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
	auto jobs = std::vector<std::pair<uint64, EfJobOptions>>();
	{
		auto sql = u"SELECT job_id, options FROM jobs "
			"WHERE kind = :kind AND session_id = :session_id"_q;
		if (peerId) {
			sql += u" AND scope_id = :scope_id"_q;
		}
		QSqlQuery q(_db);
		q.prepare(sql);
		q.bindValue(u":kind"_q, JobKindName(JobKind::EnhancedForward));
		q.bindValue(u":session_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(sessionId)));
		if (peerId) {
			q.bindValue(u":scope_id"_q, QVariant::fromValue(
				static_cast<qulonglong>(peerId.value)));
		}
		if (!q.exec()) {
			LOG(("DedupDb: SetEfResumePaused select failed: %1").arg(
				q.lastError().text()));
			return;
		}
		while (q.next()) {
			if (auto options = DeserializeEfJobOptions(
					q.value(1).toByteArray())) {
				options->paused = paused;
				jobs.emplace_back(
					q.value(0).toULongLong(),
					std::move(*options));
			}
		}
	}
	for (const auto &[jobId, options] : jobs) {
		updateEfJobOptions(jobId, options);
	}
}

void DedupDb::Impl::beginTransaction() {
	if (!_open) {
		return;
	}
	if (_transactionDepth > 0) {
		++_transactionDepth;
		return;
	}
	QSqlQuery q(_db);
	if (!q.exec(u"BEGIN IMMEDIATE"_q)) {
		LOG(("DedupDb: Begin transaction failed: %1").arg(
			q.lastError().text()));
		return;
	}
	_transactionDepth = 1;
}

void DedupDb::Impl::commitTransaction() {
	if (!_open || _transactionDepth <= 0) {
		return;
	}
	if (_transactionDepth > 1) {
		--_transactionDepth;
		return;
	}
	_transactionDepth = 0;
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

DedupDb::TableUse::TableUse(DedupDb *db, Table table)
: _db(db)
, _table(table) {
}

DedupDb::TableUse::TableUse(TableUse &&other) noexcept
: _db(std::exchange(other._db, nullptr))
, _table(other._table) {
}

DedupDb::TableUse &DedupDb::TableUse::operator=(TableUse &&other) noexcept {
	if (this != &other) {
		if (_db) {
			_db->releaseTable(_table);
		}
		_db = std::exchange(other._db, nullptr);
		_table = other._table;
	}
	return *this;
}

DedupDb::TableUse::~TableUse() {
	if (_db) {
		_db->releaseTable(_table);
	}
}

DedupDb::ChatTableUse::ChatTableUse(
	DedupDb *db,
	PeerId peerId,
	bool allPeers)
: _db(db)
, _peerId(peerId)
, _allPeers(allPeers) {
}

DedupDb::ChatTableUse::ChatTableUse(ChatTableUse &&other) noexcept
: _db(std::exchange(other._db, nullptr))
, _peerId(other._peerId)
, _allPeers(other._allPeers) {
}

DedupDb::ChatTableUse &DedupDb::ChatTableUse::operator=(
		ChatTableUse &&other) noexcept {
	if (this != &other) {
		if (_db) {
			_db->releaseChatTable(_peerId, _allPeers);
		}
		_db = std::exchange(other._db, nullptr);
		_peerId = other._peerId;
		_allPeers = other._allPeers;
	}
	return *this;
}

DedupDb::ChatTableUse::~ChatTableUse() {
	if (_db) {
		_db->releaseChatTable(_peerId, _allPeers);
	}
}

bool DedupDb::isOpen() const {
	return _impl->isOpen();
}

DedupDb::TableUse DedupDb::useTable(Table table) {
	_impl->acquireTable(table);
	return TableUse(this, table);
}

DedupDb::ChatTableUse DedupDb::useChatTable(
		PeerId peerId,
		bool allPeers) {
	_impl->acquireChatTable(peerId, allPeers);
	return ChatTableUse(this, peerId, allPeers);
}

void DedupDb::releaseTable(Table table) {
	_impl->releaseTable(table);
}

void DedupDb::releaseChatTable(PeerId peerId, bool allPeers) {
	_impl->releaseChatTable(peerId, allPeers);
}

void DedupDb::ensureTableLoaded(Table table) {
	_impl->ensureTableLoaded(table);
}

bool DedupDb::chatTableContainsDocId(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto,
		bool allPeers) const {
	return _impl->chatTableContainsDocId(
		peerId,
		mediaId,
		isPhoto,
		allPeers);
}

bool DedupDb::chatTableContainsHash(
		PeerId peerId,
		const QByteArray &hash,
		bool allPeers) const {
	return _impl->chatTableContainsHash(peerId, hash, allPeers);
}

void DedupDb::clearChatTable() {
	_impl->clearChatTable();
}

void DedupDb::addChatTableUnique(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash) {
	_impl->addChatTableUnique(peerId, mediaId, isPhoto, hash);
}

void DedupDb::removeChatTableUnique(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash) {
	_impl->removeChatTableUnique(peerId, mediaId, isPhoto, hash);
}

void DedupDb::invalidateChatTable(PeerId peerId) {
	_impl->invalidateChatTable(peerId);
}

void DedupDb::invalidateAllPeersChatTable() {
	_impl->invalidateAllPeersChatTable();
}

void DedupDb::insert(Table table, const DedupRecord &record) {
	_impl->insert(table, record);
}

bool DedupDb::containsDocId(
		Table table,
		uint64 documentId,
		bool isPhoto) const {
	return _impl->containsDocId(table, documentId, isPhoto);
}

bool DedupDb::containsDocIdInDb(
		Table table,
		uint64 documentId,
		bool isPhoto) const {
	return _impl->containsDocIdInDb(table, documentId, isPhoto);
}

bool DedupDb::containsHash(Table table, const QByteArray &hash) const {
	return _impl->containsHash(table, hash);
}

uint64 DedupDb::hashIdForHash(const QByteArray &hash) {
	return _impl->resolveHashId(hash);
}

uint64 DedupDb::findHashId(const QByteArray &hash) const {
	return _impl->findHashId(hash);
}

QByteArray DedupDb::hashForHashId(uint64 hashId) const {
	return _impl->hashForHashId(hashId);
}

std::optional<HashOwner> DedupDb::hashOwner(uint64 hashId) const {
	return _impl->hashOwner(hashId);
}

void DedupDb::setHashOwner(uint64 hashId, PeerId peerId, MsgId msgId) {
	_impl->setHashOwner(hashId, peerId, msgId);
}

void DedupDb::clearHashOwner(uint64 hashId) {
	_impl->clearHashOwner(hashId);
}

std::optional<HashOwner> DedupDb::findHashOwnerReplacement(
		uint64 hashId,
		PeerId excludePeer,
		MsgId excludeMsg) const {
	return _impl->findHashOwnerReplacement(hashId, excludePeer, excludeMsg);
}

bool DedupDb::claimOwner(uint64 hashId, PeerId peerId, MsgId msgId) {
	return _impl->claimOwner(hashId, peerId, msgId);
}

void DedupDb::setOwner(uint64 hashId, PeerId peerId, MsgId msgId) {
	_impl->setOwner(hashId, peerId, msgId);
}

void DedupDb::clearOwner(uint64 hashId) {
	_impl->clearOwner(hashId);
}

std::optional<HashOwner> DedupDb::findReplacementOwner(
		uint64 hashId,
		PeerId excludePeer,
		MsgId excludeMsg) const {
	return _impl->findReplacementOwner(hashId, excludePeer, excludeMsg);
}

bool DedupDb::promoteOwner(uint64 hashId, PeerId peerId, MsgId msgId) {
	return _impl->promoteOwner(hashId, peerId, msgId);
}

std::optional<HashOwner> DedupDb::refreshOwner(
		uint64 hashId,
		PeerId excludePeer,
		MsgId excludeMsg) {
	return _impl->refreshOwner(hashId, excludePeer, excludeMsg);
}

std::optional<ChatScanOwnerDecision> DedupDb::chatScanApplyHashOwner(
		PeerId peerId,
		MsgId msgId,
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash,
		bool mediaDuplicate,
		const QString &filename) {
	return _impl->chatScanApplyHashOwner(
		peerId,
		msgId,
		mediaId,
		isPhoto,
		hash,
		mediaDuplicate,
		filename);
}

bool DedupDb::recordUploadDestination(
		const QByteArray &hash,
		uint64 mediaId,
		bool isPhoto,
		PeerId peerId,
		MsgId msgId,
		bool tracked) {
	return _impl->recordUploadDestination(
		hash,
		mediaId,
		isPhoto,
		peerId,
		msgId,
		tracked);
}

std::vector<ChatScanOwnerChange> DedupDb::removeChatScan(PeerId peerId) {
	return _impl->removeChatScan(peerId);
}

bool DedupDb::containsFinishedHash(
		Table table,
		const QByteArray &hash) const {
	return _impl->containsFinishedHash(table, hash);
}

void DedupDb::rekey(
		Table table,
		uint64 oldDocumentId,
		uint64 newDocumentId,
		bool isPhoto) {
	_impl->rekey(table, oldDocumentId, newDocumentId, isPhoto);
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
		uint64 documentId,
		bool isPhoto) const {
	return _impl->hashForDocId(table, documentId, isPhoto);
}

uint64 DedupDb::seekDocumentId(
		Table table,
		const QByteArray &hash,
		uint64 excludeDocumentId,
		bool excludeIsPhoto) const {
	return _impl->seekDocumentId(
		table,
		hash,
		excludeDocumentId,
		excludeIsPhoto);
}

void DedupDb::addPending(
		Table table,
		uint64 documentId,
		const QByteArray &hash,
		bool isPhoto) {
	_impl->addPending(table, documentId, hash, isPhoto);
}

void DedupDb::removePending(
		Table table,
		uint64 documentId,
		bool isPhoto) {
	_impl->removePending(table, documentId, isPhoto);
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
	const auto jobId = findJobId(
		JobKind::Forward,
		record.sessionId,
		record.destPeerId.value);
	if (!jobId) {
		auto job = JobRecord();
		job.kind = JobKind::Forward;
		job.sessionId = record.sessionId;
		job.scopeId = record.destPeerId.value;
		job.options = SerializeNfResume(record);
		job.state = ForwardJobState(record.state);
		job.total = record.total;
		job.done = record.done;
		job.skipped = record.skipped;
		if (!createJob(job)) {
			LOG(("DedupDb: InsertNfResume job failed."));
		}
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE jobs SET options = :options, state = :state, "
		"total = :total, done = :done, skipped = :skipped, "
		"updated_at = :updated_at WHERE job_id = :job_id"_q);
	q.bindValue(u":options"_q, SerializeNfResume(record));
	q.bindValue(u":state"_q, int(ForwardJobState(record.state)));
	q.bindValue(u":total"_q, record.total);
	q.bindValue(u":done"_q, record.done);
	q.bindValue(u":skipped"_q, record.skipped);
	q.bindValue(u":updated_at"_q, QVariant::fromValue(
		static_cast<qlonglong>(base::unixtime::now())));
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	if (!q.exec()) {
		LOG(("DedupDb: InsertNfResume failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::removeNfResume(uint64 sessionId, PeerId destPeerId) {
	if (!_open) {
		return;
	}
	const auto jobId = findJobId(
		JobKind::Forward,
		sessionId,
		destPeerId.value);
	if (jobId) {
		removeJob(jobId);
	}
}

void DedupDb::Impl::clearNfResume(uint64 sessionId) {
	if (!_open) {
		return;
	}
	beginTransaction();
	{
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM job_items WHERE job_id IN ("
			"SELECT job_id FROM jobs WHERE kind = :kind "
			"AND session_id = :session_id)"_q);
		q.bindValue(u":kind"_q, JobKindName(JobKind::Forward));
		q.bindValue(u":session_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(sessionId)));
		if (!q.exec()) {
			LOG(("DedupDb: ClearNfResume items failed: %1").arg(
				q.lastError().text()));
		}
	}
	{
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM jobs WHERE kind = :kind "
			"AND session_id = :session_id"_q);
		q.bindValue(u":kind"_q, JobKindName(JobKind::Forward));
		q.bindValue(u":session_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(sessionId)));
		if (!q.exec()) {
			LOG(("DedupDb: ClearNfResume failed: %1").arg(
				q.lastError().text()));
		}
	}
	commitTransaction();
}

std::vector<NfResumeRecord> DedupDb::Impl::loadNfResume(
		uint64 sessionId) const {
	auto result = std::vector<NfResumeRecord>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT options FROM jobs WHERE kind = :kind "
		"AND session_id = :session_id ORDER BY job_id"_q);
	q.bindValue(u":kind"_q, JobKindName(JobKind::Forward));
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: LoadNfResume failed: %1").arg(q.lastError().text()));
		return result;
	}
	while (q.next()) {
		if (auto record = DeserializeNfResume(q.value(0).toByteArray())) {
			record->sessionId = sessionId;
			result.push_back(std::move(*record));
		}
	}
	return result;
}

void DedupDb::Impl::insertExResume(const ExResumeRecord &record) {
	if (!_open) {
		return;
	}
	const auto jobId = findExportJob(record.sessionId, record);
	if (jobId) {
		updateExportJob(jobId, record);
		return;
	}
	auto job = JobRecord();
	job.kind = JobKind::Export;
	job.sessionId = record.sessionId;
	job.scopeId = ExResumeScope(
		record.peerId,
		record.mode,
		record.exportFolder);
	job.options = SerializeExResume(record);
	job.state = ExportJobState(record.state);
	job.total = record.total;
	job.done = record.msgsDone;
	job.skipped = record.skipped;
	if (!createJob(job)) {
		LOG(("DedupDb: InsertExResume job failed."));
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
	const auto scope = ExResumeScope(peerId, mode, folder);
	auto jobIds = std::vector<uint64>();
	{
		QSqlQuery q(_db);
		q.prepare(u"SELECT job_id, options FROM jobs "
			"WHERE kind = :kind AND session_id = :session_id "
			"AND scope_id = :scope_id"_q);
		q.bindValue(u":kind"_q, JobKindName(JobKind::Export));
		q.bindValue(u":session_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(sessionId)));
		q.bindValue(u":scope_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(scope)));
		if (!q.exec()) {
			LOG(("DedupDb: RemoveExResumeFolder read failed: %1").arg(
				q.lastError().text()));
			return;
		}
		while (q.next()) {
			const auto candidate = DeserializeExResume(
				q.value(1).toByteArray());
			if (candidate
				&& candidate->peerId == peerId
				&& candidate->mode == mode
				&& candidate->exportFolder == folder) {
				jobIds.push_back(q.value(0).toULongLong());
			}
		}
	}
	for (const auto jobId : jobIds) {
		removeJob(jobId);
	}
}

void DedupDb::Impl::clearExResume(uint64 sessionId, int mode) {
	if (!_open) {
		return;
	}
	auto jobIds = std::vector<uint64>();
	for (const auto &[jobId, record] : loadExportJobs(sessionId)) {
		if (record.mode == mode) {
			jobIds.push_back(jobId);
		}
	}
	for (const auto jobId : jobIds) {
		removeJob(jobId);
	}
}

std::vector<ExResumeRecord> DedupDb::Impl::loadExResume(
		uint64 sessionId,
		int mode) const {
	auto result = std::vector<ExResumeRecord>();
	for (auto &entry : loadExportJobs(sessionId)) {
		if (entry.second.mode == mode) {
			result.push_back(std::move(entry.second));
		}
	}
	return result;
}

void DedupDb::Impl::rebaseExResumeFolder(
		uint64 sessionId,
		int mode,
		const QString &oldPrefix,
		const QString &newPrefix) {
	if (!_open) {
		return;
	}
	for (auto &[jobId, record] : loadExportJobs(sessionId)) {
		if (record.mode != mode
			|| !record.exportFolder.startsWith(oldPrefix)) {
			continue;
		}
		record.exportFolder = newPrefix
			+ record.exportFolder.mid(oldPrefix.size());
		updateExportJob(jobId, record);
	}
}

void DedupDb::Impl::insertExTmp(
		uint64 sessionId,
		PeerId peerId,
		uint64 documentId,
		bool isPhoto,
		const QByteArray &hash) {
	if (!_open || !documentId || hash.isEmpty()) {
		return;
	}
	const auto type = isPhoto ? 1 : 0;
	auto &s = exTmpState(sessionId, peerId);
	s.docToHash[type][documentId] = hash;
	s.hashToDoc[type][hash].insert(documentId);
	s.pending.push_back({ documentId, isPhoto, hash });
	if (++_exPendingWrites >= kExWriteBatchLimit) {
		flushExPending();
	}
}

void DedupDb::Impl::flushExTmpState(
		const ExTmpKey &key,
		ExTmpState &state) {
	if (state.pending.empty()) {
		return;
	}
	if (!state.jobId) {
		state.jobId = findJobId(
			JobKind::ExportBatch,
			key.sessionId,
			key.peerId);
	}
	if (!state.jobId) {
		auto job = JobRecord();
		job.kind = JobKind::ExportBatch;
		job.sessionId = key.sessionId;
		job.scopeId = key.peerId;
		job.state = JobState::Pending;
		state.jobId = createJob(job);
		if (!state.jobId) {
			return;
		}
	}
	QSqlQuery del(_db);
	del.prepare(u"DELETE FROM job_items WHERE job_id = :job_id "
		"AND media_id = :media_id AND is_photo = :is_photo"_q);
	QSqlQuery ins(_db);
	ins.prepare(u"INSERT OR REPLACE INTO job_items "
		"(job_id, item_id, media_id, is_photo, hash_id, state) "
		"VALUES (:job_id, :item_id, :media_id, :is_photo, "
		":hash_id, :state)"_q);
	for (const auto &row : state.pending) {
		const auto hashId = resolveHashId(row.hash);
		del.bindValue(u":job_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(state.jobId)));
		del.bindValue(u":media_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(row.docId)));
		del.bindValue(u":is_photo"_q, row.isPhoto ? 1 : 0);
		if (!del.exec()) {
			LOG(("DedupDb: RemoveExTmpDoc failed: %1").arg(
				del.lastError().text()));
		}
		ins.bindValue(u":job_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(state.jobId)));
		ins.bindValue(u":item_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(state.nextItemId++)));
		ins.bindValue(u":media_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(row.docId)));
		ins.bindValue(u":is_photo"_q, row.isPhoto ? 1 : 0);
		ins.bindValue(u":hash_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(hashId)));
		ins.bindValue(u":state"_q, int(JobState::Done));
		if (!ins.exec()) {
			LOG(("DedupDb: InsertExTmp failed: %1").arg(
				ins.lastError().text()));
		}
	}
	state.pending.clear();
}

void DedupDb::Impl::flushExPending() {
	if (_exPendingWrites == 0 || !_open) {
		return;
	}
	_exPendingWrites = 0;
	beginTransaction();
	for (auto &[key, state] : _exTmpStates) {
		flushExTmpState(key, state);
	}
	commitTransaction();
}

bool DedupDb::Impl::containsExTmpDocId(
		uint64 sessionId,
		PeerId peerId,
		uint64 documentId,
		bool isPhoto) const {
	if (!documentId) {
		return false;
	}
	const auto type = isPhoto ? 1 : 0;
	return exTmpState(sessionId, peerId).docToHash[type].contains(documentId);
}

bool DedupDb::Impl::containsExTmpHash(
		uint64 sessionId,
		PeerId peerId,
		const QByteArray &hash) const {
	if (hash.isEmpty()) {
		return false;
	}
	const auto &s = exTmpState(sessionId, peerId);
	for (auto type = 0; type != 2; ++type) {
		const auto i = s.hashToDoc[type].find(hash);
		if (i != s.hashToDoc[type].end() && !i->isEmpty()) {
			return true;
		}
	}
	return false;
}

void DedupDb::Impl::clearExTmpRun(uint64 sessionId, PeerId peerId) {
	auto jobId = uint64(0);
	if (const auto i = _exTmpStates.find({ sessionId, peerId.value });
		i != _exTmpStates.end()) {
		jobId = i->second.jobId;
		_exPendingWrites -= int(i->second.pending.size());
		_exTmpStates.erase(i);
	}
	if (!_open) {
		return;
	}
	if (!jobId) {
		jobId = findJobId(
			JobKind::ExportBatch,
			sessionId,
			peerId.value);
	}
	if (jobId) {
		removeJob(jobId);
	}
}

std::vector<ChatScanRow> DedupDb::Impl::readChatScan(
		PeerId peerId) const {
	if (!_open) {
		return {};
	}
	return ReadChatScanIn(_db, peerId).value_or(std::vector<ChatScanRow>());
}

bool DedupDb::Impl::isChatScanned(PeerId peerId) const {
	return _open && readScanOptions(peerId).scanFinished;
}

void DedupDb::Impl::markChatDeleteFinished(PeerId peerId) {
	if (!_open) {
		return;
	}
	auto options = readScanOptions(peerId);
	options.deleteFinished = true;
	writeScanOptions(peerId, options);
	requestIncrementalVacuum();
}

void DedupDb::Impl::requestIncrementalVacuum() {
	if (!_open || _path.isEmpty() || gVacuuming.exchange(true)) {
		return;
	}
	auto cancel = std::make_shared<std::atomic_bool>(false);
	_vacuumCancel = cancel;
	const auto path = _path;
	crl::async([path, cancel] {
		auto guard = VacuumFlagGuard();
		VacuumDatabase(path, *cancel);
	});
}

std::vector<ChatScanDup> DedupDb::Impl::readChatScanDupsBefore(
		PeerId peerId,
		int category,
		int limit,
		MsgId before) const {
	if (!_open) {
		return {};
	}
	return ReadChatScanDupsBeforeIn(
		_db,
		peerId,
		category,
		limit,
		before).value_or(std::vector<ChatScanDup>());
}

int DedupDb::Impl::chatScanDupCount(PeerId peerId, int category) const {
	if (!_open) {
		return 0;
	}
	auto sql = u"SELECT COUNT(*) FROM chat_scan_items "
		"WHERE peer_id = :peer_id AND is_dup = 1 "
		"AND tracked = 1"_q;
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
	q.prepare(u"SELECT SUM(size) FROM chat_scan_items "
		"WHERE peer_id = :peer_id AND is_dup = 1 "
		"AND tracked = 1"_q);
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
	q.prepare(u"SELECT category, "
		"SUM(CASE WHEN is_dup = 0 THEN 1 ELSE 0 END), "
		"SUM(CASE WHEN is_dup = 0 THEN size ELSE 0 END), "
		"SUM(CASE WHEN is_dup = 1 THEN 1 ELSE 0 END), "
		"SUM(CASE WHEN is_dup = 1 THEN size ELSE 0 END) "
		"FROM chat_scan_items "
		"WHERE peer_id = :peer_id "
		"AND tracked = 1 "
		"GROUP BY category"_q);
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
		"WHERE peer_id = :peer_id "
		"AND tracked = 1"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanItemCount failed: %1").arg(
			q.lastError().text()));
		return 0;
	}
	return q.next() ? qint64(q.value(0).toLongLong()) : 0;
}

bool DedupDb::Impl::chatScanCategoryHasItems(
		PeerId peerId,
		int category) const {
	if (!_open) {
		return false;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT 1 FROM chat_scan_items "
		"WHERE peer_id = :peer_id AND category = :category "
		"AND tracked = 1 LIMIT 1"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":category"_q, category);
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanCategoryHasItems failed: %1").arg(
			q.lastError().text()));
		return false;
	}
	return q.next();
}

MsgId DedupDb::Impl::chatScanMaxMsgId(PeerId peerId) const {
	if (!_open) {
		return MsgId(0);
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT MAX(msg_id) FROM chat_scan_items "
		"WHERE peer_id = :peer_id "
		"AND tracked = 1"_q);
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
		"AND hash_id = (SELECT hash_id FROM hashes WHERE hash = :hash) "
		"AND is_dup = 0 AND tracked = 1 LIMIT 1"_q);
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

MsgId DedupDb::Impl::chatScanUniqueHash(
		PeerId peerId,
		const QByteArray &hash) const {
	if (!_open || hash.isEmpty()) {
		return MsgId(0);
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT MAX(msg_id) FROM chat_scan_items "
		"WHERE peer_id = :peer_id "
		"AND hash_id = (SELECT hash_id FROM hashes WHERE hash = :hash) "
		"AND is_dup = 0 AND tracked = 1"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":hash"_q, hash);
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanUniqueHash failed: %1").arg(
			q.lastError().text()));
		return MsgId(0);
	}
	return q.next() ? MsgId(q.value(0).toLongLong()) : MsgId(0);
}

QByteArray DedupDb::Impl::chatScanHashForMedia(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto) const {
	if (!_open || !mediaId) {
		return QByteArray();
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT h.hash FROM chat_scan_items i "
		"JOIN hashes h ON h.hash_id = i.hash_id "
		"WHERE i.peer_id = :peer_id AND i.media_id = :media_id "
		"AND i.is_photo = :is_photo "
		"AND i.tracked = 1 "
		"AND i.hash_id != 0 LIMIT 1"_q);
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

QByteArray DedupDb::Impl::chatScanHashForMediaOther(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto) const {
	if (!_open || !mediaId) {
		return QByteArray();
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT h.hash FROM chat_scan_items i "
		"JOIN hashes h ON h.hash_id = i.hash_id "
		"WHERE i.peer_id != :peer_id AND i.media_id = :media_id "
		"AND i.is_photo = :is_photo "
		"AND i.tracked = 1 "
		"AND i.hash_id != 0 "
		"ORDER BY i.is_dup ASC, i.msg_id DESC LIMIT 1"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(mediaId)));
	q.bindValue(u":is_photo"_q, isPhoto ? 1 : 0);
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanHashForMediaOther failed: %1").arg(
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
		"is_photo, is_dup FROM chat_scan_items "
		"WHERE peer_id = :peer_id "
		"AND hash_id = 0 "
		"AND tracked = 1 "
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
		item.isDup = (q.value(5).toInt() != 0);
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
	q.prepare(u"SELECT i.msg_id, i.category, i.size, i.media_id, "
		"i.is_photo, NULL AS hash, i.attempts, i.is_dup "
		"FROM chat_scan_items i "
		"WHERE i.peer_id = :peer_id "
		"AND i.hash_id = 0 "
		"AND i.tracked = 1 "
		"AND i.state IN (0, 1) "
		"AND i.msg_id < :above "
		"ORDER BY i.msg_id DESC LIMIT :limit"_q);
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
		item.hash = q.value(5).toByteArray();
		item.attempts = q.value(6).toInt();
		item.isDup = (q.value(7).toInt() != 0);
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
	auto sql = u"SELECT i.msg_id, i.category, i.size, i.media_id, "
		"i.is_photo, h.hash, i.is_dup "
		"FROM chat_scan_items i "
		"LEFT JOIN hashes h ON h.hash_id = i.hash_id "
		"WHERE i.peer_id = :peer_id "
		"AND i.tracked = 1"_q;
	if (above.bare > 0) {
		sql += u" AND i.msg_id < :above"_q;
	}
	sql += u" ORDER BY i.msg_id DESC LIMIT :limit"_q;
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
		item.hash = q.value(5).toByteArray();
		item.isDup = (q.value(6).toInt() != 0);
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
		"AND tracked = 1 "
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
	q.prepare(u"SELECT COUNT(DISTINCT media_id) FROM chat_scan_items "
		"WHERE peer_id = :peer_id "
		"AND tracked = 1 "
		"AND hash_id != 0"_q);
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
		"WHERE peer_id = :peer_id AND tracked = 1 "
		"AND state IN (0, 1)"_q);
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
		"i.is_photo, i.attempts "
		"FROM chat_scan_items AS i "
		"WHERE i.peer_id = :peer_id AND i.state = 0 "
		"AND i.tracked = 1 "
		"AND i.msg_id IN ("
			"SELECT MAX(msg_id) "
			"FROM chat_scan_items "
			"WHERE peer_id = :rep_peer_id AND state = 0 "
			"AND tracked = 1 "
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
		item.attempts = q.value(5).toInt() + 1;
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
		bool isPhoto,
		qint64 retryAt) {
	if (!_open || !mediaId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE chat_scan_items "
		"SET state = 0, lease_until = :retry_at "
		"WHERE peer_id = :peer_id AND media_id = :media_id "
		"AND is_photo = :is_photo AND state = 1"_q);
	q.bindValue(u":retry_at"_q, retryAt);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(mediaId)));
	q.bindValue(u":is_photo"_q, isPhoto ? 1 : 0);
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
		const std::vector<ChatScanHash> &hashes) {
	if (!_open || hashes.empty()) {
		return 0;
	}
	auto resolved = 0;
	auto hashIds = std::set<uint64>();
	auto media = std::set<std::pair<uint64, bool>>();
	beginTransaction();
	for (const auto &entry : hashes) {
		if (!entry.msgId || !entry.mediaId || entry.hash.isEmpty()) {
			continue;
		}
		media.insert({ entry.mediaId, entry.isPhoto });
		{
			QSqlQuery old(_db);
			old.prepare(u"SELECT DISTINCT hash_id FROM chat_scan_items "
				"WHERE peer_id = :peer_id AND media_id = :media_id "
				"AND is_photo = :is_photo AND hash_id != 0"_q);
			old.bindValue(u":peer_id"_q, QVariant::fromValue(
				static_cast<qulonglong>(peerId.value)));
			old.bindValue(u":media_id"_q, QVariant::fromValue(
				static_cast<qulonglong>(entry.mediaId)));
			old.bindValue(u":is_photo"_q, entry.isPhoto ? 1 : 0);
			if (old.exec()) {
				while (old.next()) {
					const auto oldHashId = old.value(0).toULongLong();
					if (oldHashId) {
						hashIds.insert(oldHashId);
					}
				}
			} else {
				LOG(("DedupDb: ChatScanCommitHash old failed: %1").arg(
					old.lastError().text()));
			}
		}
		const auto hashId = resolveHashId(entry.hash);
		if (!hashId) {
			continue;
		}
		hashIds.insert(hashId);
		QSqlQuery q(_db);
		q.prepare(u"UPDATE chat_scan_items SET "
			"hash_id = :hash_id, "
			"filename = CASE "
				"WHEN msg_id = :msg_id AND is_dup = 1 THEN :filename "
				"WHEN msg_id = :msg_id THEN NULL "
				"ELSE filename END, "
			"state = 2, lease_until = 0 "
			"WHERE peer_id = :peer_id AND media_id = :media_id "
			"AND is_photo = :is_photo"_q);
		q.bindValue(u":hash_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(hashId)));
		q.bindValue(u":filename"_q, entry.filename.isEmpty()
			? QVariant()
			: QVariant(entry.filename));
		q.bindValue(u":msg_id"_q, QVariant::fromValue(
			static_cast<qlonglong>(entry.msgId.bare)));
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		q.bindValue(u":media_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(entry.mediaId)));
		q.bindValue(u":is_photo"_q, entry.isPhoto ? 1 : 0);
		if (!q.exec()) {
			LOG(("DedupDb: ChatScanCommitHash failed: %1").arg(
				q.lastError().text()));
		} else if (q.numRowsAffected() > 0) {
			++resolved;
		}
	}
	refreshChatScanRefs(hashIds, media);
	commitTransaction();
	return resolved;
}

void DedupDb::Impl::chatScanFailItem(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto) {
	if (!_open || !mediaId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE chat_scan_items "
		"SET state = 3, lease_until = 0 "
		"WHERE peer_id = :peer_id AND media_id = :media_id "
		"AND is_photo = :is_photo AND state = 1"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(mediaId)));
	q.bindValue(u":is_photo"_q, isPhoto ? 1 : 0);
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

void DedupDb::Impl::recomputeChatScanProgressAsync(
		PeerId peerId,
		Fn<void(std::vector<ChatScanProgressRow>, bool)> done) {
	if (!_open) {
		if (done) {
			crl::on_main([done = std::move(done)]() mutable {
				done({}, false);
			});
		}
		return;
	}
	const auto path = _path;
	crl::async([path, peerId, done = std::move(done)]() mutable {
		const auto connectionName = u"dedup-progress-%1"_q.arg(
			QUuid::createUuid().toString());
		auto db = QSqlDatabase::addDatabase(u"QSQLITE"_q, connectionName);
		db.setDatabaseName(path);
		auto result = std::vector<ChatScanProgressRow>();
		auto success = false;
		if (db.open()) {
			{
				QSqlQuery q(db);
				q.exec(u"PRAGMA busy_timeout=5000"_q);
			}
			if (auto computed = RecomputeChatScanProgressIn(db, peerId)) {
				result = std::move(*computed);
				success = true;
			}
			db.close();
		} else {
			LOG(("DedupDb: RecomputeChatScanProgress open failed: %1").arg(
				db.lastError().text()));
		}
		db = QSqlDatabase();
		QSqlDatabase::removeDatabase(connectionName);
		if (done) {
			crl::on_main([
				done = std::move(done),
				result = std::move(result),
				success
			]() mutable {
				done(std::move(result), success);
			});
		}
	});
}

void DedupDb::Impl::chatScanUniqueHashesAsync(
		PeerId peerId,
		Fn<void(std::vector<std::pair<QByteArray, MsgId>>, bool)> done) {
	if (!_open) {
		if (done) {
			crl::on_main([done = std::move(done)]() mutable {
				done({}, false);
			});
		}
		return;
	}
	const auto path = _path;
	crl::async([path, peerId, done = std::move(done)]() mutable {
		const auto connectionName = u"dedup-unique-hashes-%1"_q.arg(
			QUuid::createUuid().toString());
		auto db = QSqlDatabase::addDatabase(u"QSQLITE"_q, connectionName);
		db.setDatabaseName(path);
		auto result = std::vector<std::pair<QByteArray, MsgId>>();
		auto success = false;
		if (db.open()) {
			{
				QSqlQuery q(db);
				q.exec(u"PRAGMA busy_timeout=5000"_q);
			}
			if (auto read = ReadChatScanUniqueHashesIn(db, peerId)) {
				result = std::move(*read);
				success = true;
			}
			db.close();
		} else {
			LOG(("DedupDb: ChatScanUniqueHashes open failed: %1").arg(
				db.lastError().text()));
		}
		db = QSqlDatabase();
		QSqlDatabase::removeDatabase(connectionName);
		if (done) {
			crl::on_main([
				done = std::move(done),
				result = std::move(result),
				success
			]() mutable {
				done(std::move(result), success);
			});
		}
	});
}

void DedupDb::Impl::chatScanMediaHashesAsync(
		PeerId peerId,
		bool includeOther,
		Fn<void(std::vector<ChatScanMediaHash>, bool)> done) {
	if (!_open) {
		if (done) {
			crl::on_main([done = std::move(done)]() mutable {
				done({}, false);
			});
		}
		return;
	}
	const auto path = _path;
	crl::async([path, peerId, includeOther, done = std::move(done)]() mutable {
		const auto connectionName = u"dedup-media-hashes-%1"_q.arg(
			QUuid::createUuid().toString());
		auto db = QSqlDatabase::addDatabase(u"QSQLITE"_q, connectionName);
		db.setDatabaseName(path);
		auto result = std::vector<ChatScanMediaHash>();
		auto success = false;
		if (db.open()) {
			{
				QSqlQuery q(db);
				q.exec(u"PRAGMA busy_timeout=5000"_q);
			}
			if (auto read = ReadChatScanMediaHashesIn(
					db,
					peerId,
					includeOther)) {
				result = std::move(*read);
				success = true;
			}
			db.close();
		} else {
			LOG(("DedupDb: ChatScanMediaHashes open failed: %1").arg(
				db.lastError().text()));
		}
		db = QSqlDatabase();
		QSqlDatabase::removeDatabase(connectionName);
		if (done) {
			crl::on_main([
				done = std::move(done),
				result = std::move(result),
				success
			]() mutable {
				done(std::move(result), success);
			});
		}
	});
}

void DedupDb::Impl::chatScanItemIdsAsync(
		PeerId peerId,
		int category,
		Fn<void(std::vector<MsgId>, bool)> done) {
	if (!_open) {
		if (done) {
			crl::on_main([done = std::move(done)]() mutable {
				done({}, false);
			});
		}
		return;
	}
	const auto path = _path;
	crl::async([
		path,
		peerId,
		category,
		done = std::move(done)
	]() mutable {
		const auto connectionName = u"dedup-item-ids-%1"_q.arg(
			QUuid::createUuid().toString());
		auto db = QSqlDatabase::addDatabase(u"QSQLITE"_q, connectionName);
		db.setDatabaseName(path);
		auto result = std::vector<MsgId>();
		auto success = false;
		if (db.open()) {
			{
				QSqlQuery q(db);
				q.exec(u"PRAGMA busy_timeout=5000"_q);
			}
			if (auto read = ReadChatScanItemIdsIn(db, peerId, category)) {
				result = std::move(*read);
				success = true;
			}
			db.close();
		} else {
			LOG(("DedupDb: ChatScanItemIds open failed: %1").arg(
				db.lastError().text()));
		}
		db = QSqlDatabase();
		QSqlDatabase::removeDatabase(connectionName);
		if (done) {
			crl::on_main([
				done = std::move(done),
				result = std::move(result),
				success
			]() mutable {
				done(std::move(result), success);
			});
		}
	});
}

void DedupDb::Impl::readChatScanAsync(
		PeerId peerId,
		Fn<void(std::vector<ChatScanRow>, bool)> done) {
	if (!_open) {
		if (done) {
			crl::on_main([done = std::move(done)]() mutable {
				done({}, false);
			});
		}
		return;
	}
	const auto path = _path;
	crl::async([path, peerId, done = std::move(done)]() mutable {
		const auto connectionName = u"dedup-scan-rows-%1"_q.arg(
			QUuid::createUuid().toString());
		auto db = QSqlDatabase::addDatabase(u"QSQLITE"_q, connectionName);
		db.setDatabaseName(path);
		auto result = std::vector<ChatScanRow>();
		auto success = false;
		if (db.open()) {
			{
				QSqlQuery q(db);
				q.exec(u"PRAGMA busy_timeout=5000"_q);
			}
			if (auto read = ReadChatScanIn(db, peerId)) {
				result = std::move(*read);
				success = true;
			}
			db.close();
		} else {
			LOG(("DedupDb: ReadChatScan open failed: %1").arg(
				db.lastError().text()));
		}
		db = QSqlDatabase();
		QSqlDatabase::removeDatabase(connectionName);
		if (done) {
			crl::on_main([
				done = std::move(done),
				result = std::move(result),
				success
			]() mutable {
				done(std::move(result), success);
			});
		}
	});
}

std::vector<ChatScanItem> DedupDb::Impl::chatScanDistinctItems(
		PeerId peerId) const {
	auto result = std::vector<ChatScanItem>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT MIN(i.msg_id) AS msg_id, MIN(i.category) AS category, "
		"MIN(i.size) AS size, i.media_id, MIN(i.is_photo) AS is_photo, "
		"MIN(h.hash) AS hash "
		"FROM chat_scan_items i "
		"JOIN hashes h ON h.hash_id = i.hash_id "
		"WHERE i.peer_id = :peer_id "
		"AND i.hash_id != 0 "
		"AND i.tracked = 1 "
		"AND i.media_id != 0 "
		"GROUP BY i.media_id "
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
		result.push_back(item);
	}
	return result;
}

void DedupDb::Impl::clearChatScan(PeerId peerId) {
	if (!_open) {
		return;
	}
	ClearChatScanIn(_db, peerId);
}

void DedupDb::Impl::clearChatScanAsync(
		PeerId peerId,
		Fn<void(bool, std::vector<ChatScanOwnerChange>)> done) {
	if (!_open) {
		if (done) {
			crl::on_main([done = std::move(done)]() mutable {
				done(false, std::vector<ChatScanOwnerChange>());
			});
		}
		return;
	}
	const auto path = _path;
	crl::async([path, peerId, done = std::move(done)]() mutable {
		const auto connectionName = u"dedup-clear-%1"_q.arg(
			QUuid::createUuid().toString());
		auto db = QSqlDatabase::addDatabase(u"QSQLITE"_q, connectionName);
		db.setDatabaseName(path);
		auto success = false;
		auto changed = std::vector<ChatScanOwnerChange>();
		if (db.open()) {
			{
				QSqlQuery q(db);
				q.exec(u"PRAGMA busy_timeout=5000"_q);
			}
			changed = ClearChatScanIn(db, peerId);
			success = true;
			db.close();
		} else {
			LOG(("DedupDb: ClearChatScan open failed: %1").arg(
				db.lastError().text()));
		}
		db = QSqlDatabase();
		QSqlDatabase::removeDatabase(connectionName);
		if (done) {
			crl::on_main([
				done = std::move(done),
				success,
				changed = std::move(changed)
			]() mutable {
				done(success, std::move(changed));
			});
		}
	});
}

std::optional<uint64> DedupDb::Impl::findScanJobId(PeerId peerId) const {
	if (!_open) {
		return std::nullopt;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT job_id FROM jobs WHERE kind = 'scan' "
		"AND scope_id = :scope_id ORDER BY job_id DESC LIMIT 1"_q);
	q.bindValue(u":scope_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	if (!q.exec()) {
		LOG(("DedupDb: FindScanJob failed: %1").arg(q.lastError().text()));
		return std::nullopt;
	}
	return q.next()
		? std::optional<uint64>(q.value(0).toULongLong())
		: std::nullopt;
}

ScanJobOptions DedupDb::Impl::readScanOptions(PeerId peerId) const {
	if (!_open) {
		return ScanJobOptions();
	}
	return ReadScanJobOptionsIn(_db, peerId).value_or(ScanJobOptions());
}

void DedupDb::Impl::writeScanOptions(
		PeerId peerId,
		const ScanJobOptions &options) {
	if (!_open || !peerId) {
		return;
	}
	auto jobId = findScanJobId(peerId).value_or(0);
	if (!jobId) {
		auto job = JobRecord();
		job.kind = JobKind::Scan;
		job.scopeId = peerId.value;
		job.state = ScanJobState(options);
		job.options = SerializeScanJob(options);
		jobId = createJob(job);
		if (!jobId) {
			return;
		}
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE jobs SET options = :options, state = :state, "
		"updated_at = :updated_at WHERE job_id = :job_id"_q);
	q.bindValue(u":options"_q, SerializeScanJob(options));
	q.bindValue(u":state"_q, int(ScanJobState(options)));
	q.bindValue(u":updated_at"_q, QVariant::fromValue(
		static_cast<qlonglong>(base::unixtime::now())));
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	if (!q.exec()) {
		LOG(("DedupDb: WriteScanOptions failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::writeScanCursor(const ChatScanState &state) {
	if (!_open) {
		return;
	}
	auto options = readScanOptions(state.peerId);
	options.cursor = state.cursor;
	options.phase = state.phase;
	options.elapsedMs = state.elapsedMs;
	writeScanOptions(state.peerId, options);
}

int DedupDb::Impl::chatScanMode(PeerId peerId) const {
	return _open ? readScanOptions(peerId).dedupMode : -1;
}

void DedupDb::Impl::writeChatScanMode(PeerId peerId, int mode) {
	if (!_open) {
		return;
	}
	auto options = readScanOptions(peerId);
	options.dedupMode = mode;
	writeScanOptions(peerId, options);
}

std::optional<ChatScanState> DedupDb::Impl::readScanCursor(
		PeerId peerId) const {
	if (!_open || !findScanJobId(peerId)) {
		return std::nullopt;
	}
	const auto options = readScanOptions(peerId);
	auto state = ChatScanState();
	state.peerId = peerId;
	state.cursor = options.cursor;
	state.phase = options.phase;
	state.elapsedMs = options.elapsedMs;
	return state;
}

void DedupDb::Impl::finishChatScan(
		PeerId peerId,
		MsgId scannedUpto,
		int mode,
		bool deleteFinished,
		int64 elapsedMs) {
	if (!_open) {
		return;
	}
	auto options = readScanOptions(peerId);
	options.cursor = scannedUpto;
	options.scannedUpto = scannedUpto;
	options.phase = kScanPhaseFinished;
	options.elapsedMs = elapsedMs;
	options.dedupMode = mode;
	options.scanFinished = true;
	options.deleteFinished = deleteFinished;
	writeScanOptions(peerId, options);
}

MsgId DedupDb::Impl::chatScanScannedUpto(PeerId peerId) const {
	return _open ? readScanOptions(peerId).scannedUpto : MsgId(0);
}

void DedupDb::Impl::writeChatScanItems(
		const std::vector<ChatScanItem> &items) {
	if (!_open || items.empty()) {
		return;
	}
	auto hashIds = std::set<uint64>();
	auto media = std::set<std::pair<uint64, bool>>();
	beginTransaction();
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR REPLACE INTO chat_scan_items "
		"(peer_id, msg_id, category, size, media_id, "
		"is_photo, hash_id, state, attempts, lease_until, is_dup, "
		"session_id, filename, tracked) "
		"VALUES (:peer_id, :msg_id, :category, :size, "
		":media_id, :is_photo, :hash_id, "
		":state, :attempts, :lease_until, :is_dup, :session_id, "
		":filename, :tracked)"_q);
	for (const auto &item : items) {
		if (!item.msgId) {
			continue;
		}
		auto oldHashId = uint64(0);
		auto oldMediaId = uint64(0);
		auto oldIsPhoto = false;
		{
			QSqlQuery old(_db);
			old.prepare(u"SELECT hash_id, media_id, is_photo "
				"FROM chat_scan_items "
				"WHERE peer_id = :peer_id AND msg_id = :msg_id"_q);
			old.bindValue(u":peer_id"_q, QVariant::fromValue(
				static_cast<qulonglong>(item.peerId.value)));
			old.bindValue(u":msg_id"_q, QVariant::fromValue(
				static_cast<qlonglong>(item.msgId.bare)));
			if (old.exec() && old.next()) {
				oldHashId = old.value(0).toULongLong();
				oldMediaId = old.value(1).toULongLong();
				oldIsPhoto = (old.value(2).toInt() != 0);
			} else if (old.lastError().isValid()) {
				LOG(("DedupDb: WriteChatScanItems old failed: %1").arg(
					old.lastError().text()));
			}
		}
		const auto sameMedia = (oldMediaId == item.mediaId)
			&& (oldIsPhoto == item.isPhoto);
		auto hashId = sameMedia ? oldHashId : uint64(0);
		if (!item.hash.isEmpty() && (sameMedia || !oldMediaId)) {
			hashId = resolveHashId(item.hash);
			if (!hashId) {
				continue;
			}
		}
		if (oldHashId) {
			hashIds.insert(oldHashId);
		}
		if (hashId) {
			hashIds.insert(hashId);
		}
		if (oldMediaId) {
			media.insert({ oldMediaId, oldIsPhoto });
		}
		if (item.mediaId) {
			media.insert({ item.mediaId, item.isPhoto });
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
		q.bindValue(u":hash_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(hashId)));
		q.bindValue(u":state"_q, item.state);
		q.bindValue(u":attempts"_q, item.attempts);
		q.bindValue(u":lease_until"_q, item.leaseUntil);
		q.bindValue(u":is_dup"_q, item.isDup ? 1 : 0);
		q.bindValue(u":session_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(item.sessionId)));
		q.bindValue(u":filename"_q, (item.isDup
			&& !item.isPhoto
			&& !item.filename.isEmpty())
			? QVariant(item.filename)
			: QVariant());
		q.bindValue(u":tracked"_q, item.tracked ? 1 : 0);
		if (!q.exec()) {
			LOG(("DedupDb: WriteChatScanItems failed: %1").arg(
				q.lastError().text()));
		}
	}
	refreshChatScanRefs(hashIds, media);
	commitTransaction();
}

std::vector<ChatScanItem> DedupDb::Impl::readChatScanItems(
		PeerId peerId) const {
	auto result = std::vector<ChatScanItem>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT i.msg_id, i.category, i.size, i.media_id, "
		"i.is_photo, h.hash, i.is_dup, i.session_id "
		"FROM chat_scan_items i "
		"LEFT JOIN hashes h ON h.hash_id = i.hash_id "
		"WHERE i.peer_id = :peer_id "
		"AND i.tracked = 1 "
		"ORDER BY i.msg_id DESC"_q);
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
		item.isDup = (q.value(6).toInt() != 0);
		item.sessionId = q.value(7).toULongLong();
		result.push_back(item);
	}
	return result;
}

int DedupDb::Impl::updateChatScanItemHash(
		PeerId peerId,
		MsgId msgId,
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash) {
	if (!_open || !msgId || !mediaId || hash.isEmpty()) {
		return 0;
	}
	const auto hashId = resolveHashId(hash);
	if (!hashId) {
		return 0;
	}
	auto hashIds = std::set<uint64> { hashId };
	auto media = std::set<std::pair<uint64, bool>> {
		{ mediaId, isPhoto },
	};
	beginTransaction();
	{
		QSqlQuery old(_db);
		old.prepare(u"SELECT DISTINCT hash_id FROM chat_scan_items "
			"WHERE peer_id = :peer_id AND media_id = :media_id "
			"AND is_photo = :is_photo AND hash_id != 0"_q);
		old.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		old.bindValue(u":media_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(mediaId)));
		old.bindValue(u":is_photo"_q, isPhoto ? 1 : 0);
		if (old.exec()) {
			while (old.next()) {
				const auto oldHashId = old.value(0).toULongLong();
				if (oldHashId) {
					hashIds.insert(oldHashId);
				}
			}
		} else {
			LOG(("DedupDb: UpdateChatScanItemHash old failed: %1").arg(
				old.lastError().text()));
		}
	}
	QSqlQuery q(_db);
	q.prepare(u"UPDATE chat_scan_items SET "
		"hash_id = :hash_id, "
		"state = 2, lease_until = 0 "
		"WHERE peer_id = :peer_id AND media_id = :media_id "
		"AND is_photo = :is_photo"_q);
	q.bindValue(u":hash_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(hashId)));
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(mediaId)));
	q.bindValue(u":is_photo"_q, isPhoto ? 1 : 0);
	auto updated = 0;
	if (!q.exec()) {
		LOG(("DedupDb: UpdateChatScanItemHash failed: %1").arg(
			q.lastError().text()));
	} else {
		updated = q.numRowsAffected();
	}
	refreshChatScanRefs(hashIds, media);
	commitTransaction();
	return updated;
}

std::vector<PeerId> DedupDb::Impl::scannedPeerIds() const {
	auto result = std::vector<PeerId>();
	if (!_open) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT scope_id, options FROM jobs WHERE kind = 'scan'"_q);
	if (!q.exec()) {
		LOG(("DedupDb: ScannedPeerIds failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		if (const auto options = DeserializeScanJob(q.value(1).toByteArray())) {
			if (options->scanFinished) {
				result.push_back(PeerId(q.value(0).toULongLong()));
			}
		}
	}
	return result;
}

std::vector<ChatScanOwnerChange> DedupDb::Impl::removeChatScan(
		PeerId peerId) {
	auto result = std::vector<ChatScanOwnerChange>();
	if (!_open || !peerId) {
		return result;
	}
	auto hashIds = std::set<uint64>();
	auto media = std::set<std::pair<uint64, bool>>();
	beginTransaction();
	{
		QSqlQuery q(_db);
		q.prepare(u"SELECT hash_id FROM hashes "
			"WHERE owner_peer_id = :peer_id"_q);
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		if (q.exec()) {
			while (q.next()) {
				const auto hashId = q.value(0).toULongLong();
				if (hashId) {
					hashIds.insert(hashId);
				}
			}
		} else {
			LOG(("DedupDb: RemoveChatScan owners failed: %1").arg(
				q.lastError().text()));
		}
	}
	result = CleanupOwnersForPeerIn(_db, peerId);
	{
		QSqlQuery q(_db);
		q.prepare(u"SELECT hash_id, media_id, is_photo "
			"FROM chat_scan_items WHERE peer_id = :peer_id"_q);
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		if (q.exec()) {
			while (q.next()) {
				const auto hashId = q.value(0).toULongLong();
				const auto mediaId = q.value(1).toULongLong();
				if (hashId) {
					hashIds.insert(hashId);
				}
				if (mediaId) {
					media.insert({ mediaId, q.value(2).toInt() != 0 });
				}
			}
		} else {
			LOG(("DedupDb: RemoveChatScan refs failed: %1").arg(
				q.lastError().text()));
		}
	}
	{
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM chat_scan_items "
			"WHERE peer_id = :peer_id"_q);
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		if (!q.exec()) {
			LOG(("DedupDb: RemoveChatScan items failed: %1").arg(
				q.lastError().text()));
		}
	}
	DeleteScanJobIn(_db, peerId);
	for (const auto hashId : hashIds) {
		refreshHashRefs(hashId);
	}
	for (const auto &[mediaId, isPhoto] : media) {
		refreshMediaRefs(mediaId, isPhoto);
	}
	for (const auto &[mediaId, isPhoto] : media) {
		cleanupMediaRefs(mediaId, isPhoto);
	}
	for (const auto hashId : hashIds) {
		cleanupHashRefs(hashId);
	}
	commitTransaction();
	return result;
}

void DedupDb::Impl::removeChatScanItem(PeerId peerId, MsgId msgId) {
	if (!_open || !msgId) {
		return;
	}
	auto hashIds = std::set<uint64>();
	auto media = std::set<std::pair<uint64, bool>>();
	const auto savepoint = u"dedup_chat_scan_remove"_q;
	{
		QSqlQuery q(_db);
		if (!q.exec(u"SAVEPOINT "_q + savepoint)) {
			LOG(("DedupDb: RemoveChatScanItem savepoint failed: %1").arg(
				q.lastError().text()));
			return;
		}
	}
	const auto release = [&] {
		QSqlQuery q(_db);
		if (!q.exec(u"RELEASE "_q + savepoint)) {
			LOG(("DedupDb: RemoveChatScanItem release failed: %1").arg(
				q.lastError().text()));
		}
	};
	{
		QSqlQuery old(_db);
		old.prepare(u"SELECT hash_id, media_id, is_photo "
			"FROM chat_scan_items "
			"WHERE peer_id = :peer_id AND msg_id = :msg_id"_q);
		old.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		old.bindValue(u":msg_id"_q, QVariant::fromValue(
			static_cast<qlonglong>(msgId.bare)));
		if (!old.exec() || !old.next()) {
			release();
			return;
		}
		const auto hashId = old.value(0).toULongLong();
		const auto mediaId = old.value(1).toULongLong();
		const auto isPhoto = (old.value(2).toInt() != 0);
		if (hashId) {
			hashIds.insert(hashId);
		}
		if (mediaId) {
			media.insert({ mediaId, isPhoto });
		}
	}
	{
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
	refreshChatScanRefs(hashIds, media);
	release();
}

void DedupDb::Impl::saveChatScanUndo(PeerId peerId, MsgId msgId) {
	if (!_open || !msgId) {
		return;
	}
	const auto jobId = findScanJobId(peerId).value_or(0);
	if (!jobId) {
		return;
	}
	const auto options = readScanOptions(peerId);
	if (!options.sessionId) {
		return;
	}
	auto isDup = false;
	auto filename = QString();
	{
		QSqlQuery q(_db);
		q.prepare(u"SELECT is_dup, filename FROM chat_scan_items "
			"WHERE peer_id = :peer_id AND msg_id = :msg_id "
			"AND session_id != :session_id"_q);
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		q.bindValue(u":msg_id"_q, QVariant::fromValue(
			static_cast<qlonglong>(msgId.bare)));
		q.bindValue(u":session_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(options.sessionId)));
		if (!q.exec()) {
			LOG(("DedupDb: SaveChatScanUndo read failed: %1").arg(
				q.lastError().text()));
			return;
		}
		if (!q.next()) {
			return;
		}
		isDup = (q.value(0).toInt() != 0);
		filename = q.value(1).toString();
	}
	QSqlQuery q(_db);
	q.prepare(u"INSERT OR IGNORE INTO job_items "
		"(job_id, item_id, state, payload) "
		"VALUES (:job_id, :item_id, :state, :payload)"_q);
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	q.bindValue(u":item_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(msgId.bare)));
	q.bindValue(u":state"_q, int(JobState::Done));
	q.bindValue(u":payload"_q, SerializeScanUndo(isDup, filename));
	if (!q.exec()) {
		LOG(("DedupDb: SaveChatScanUndo failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::markChatScanItemDup(
		PeerId peerId,
		MsgId msgId,
		bool isDup,
		const QString &filename) {
	if (!_open || !msgId) {
		return;
	}
	saveChatScanUndo(peerId, msgId);
	QSqlQuery q(_db);
	q.prepare(u"UPDATE chat_scan_items SET "
		"is_dup = :is_dup, "
		"filename = CASE WHEN :is_dup = 1 THEN :filename ELSE NULL END "
		"WHERE peer_id = :peer_id AND msg_id = :msg_id"_q);
	q.bindValue(u":is_dup"_q, isDup ? 1 : 0);
	q.bindValue(u":filename"_q, (isDup && !filename.isEmpty())
		? QVariant(filename)
		: QVariant());
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":msg_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(msgId.bare)));
	if (!q.exec()) {
		LOG(("DedupDb: MarkChatScanItemDup failed: %1").arg(
			q.lastError().text()));
	}
}

bool DedupDb::Impl::beginChatScanSession(
		PeerId peerId,
		uint64 sessionId,
		MsgId baseMark) {
	if (!_open || !sessionId) {
		return false;
	}
	beginTransaction();
	auto options = readScanOptions(peerId);
	options.oldStateExists = findScanJobId(peerId).has_value();
	options.oldCursor = options.cursor;
	options.oldPhase = options.phase;
	options.oldElapsedMs = options.elapsedMs;
	options.oldDedupMode = options.dedupMode;
	options.sessionId = sessionId;
	options.baseMark = baseMark;
	writeScanOptions(peerId, options);
	if (const auto jobId = findScanJobId(peerId).value_or(0)) {
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM job_items WHERE job_id = :job_id"_q);
		q.bindValue(u":job_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(jobId)));
		if (!q.exec()) {
			LOG(("DedupDb: BeginChatScanSession cleanup failed: %1").arg(
				q.lastError().text()));
			commitTransaction();
			return false;
		}
	}
	commitTransaction();
	return true;
}

bool DedupDb::Impl::cancelChatScanSession(PeerId peerId) {
	if (!_open) {
		return false;
	}
	const auto jobId = findScanJobId(peerId).value_or(0);
	if (!jobId) {
		return false;
	}
	auto options = readScanOptions(peerId);
	if (!options.sessionId) {
		return false;
	}
	const auto sessionId = options.sessionId;
	beginTransaction();
	{
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM chat_scan_items "
			"WHERE peer_id = :peer_id AND session_id = :session_id"_q);
		q.bindValue(u":peer_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(peerId.value)));
		q.bindValue(u":session_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(sessionId)));
		if (!q.exec()) {
			LOG(("DedupDb: CancelChatScanSession delete failed: %1").arg(
				q.lastError().text()));
		}
	}
	{
		auto undoEntries = std::vector<std::pair<qint64, std::pair<bool, QString>>>();
		{
			QSqlQuery q(_db);
			q.prepare(u"SELECT item_id, payload FROM job_items "
				"WHERE job_id = :job_id"_q);
			q.bindValue(u":job_id"_q, QVariant::fromValue(
				static_cast<qulonglong>(jobId)));
			if (!q.exec()) {
				LOG(("DedupDb: CancelChatScanSession undo read failed: %1")
					.arg(q.lastError().text()));
			} else {
				while (q.next()) {
					const auto undo = DeserializeScanUndo(
						q.value(1).toByteArray());
					if (undo) {
						undoEntries.emplace_back(
							q.value(0).toLongLong(),
							*undo);
					}
				}
			}
		}
		for (const auto &[msgId, undo] : undoEntries) {
			QSqlQuery update(_db);
			update.prepare(u"UPDATE chat_scan_items SET "
				"is_dup = :is_dup, filename = :filename "
				"WHERE peer_id = :peer_id AND msg_id = :msg_id"_q);
			update.bindValue(u":is_dup"_q, undo.first ? 1 : 0);
			update.bindValue(u":filename"_q,
				undo.second.isEmpty()
					? QVariant()
					: QVariant(undo.second));
			update.bindValue(u":peer_id"_q, QVariant::fromValue(
				static_cast<qulonglong>(peerId.value)));
			update.bindValue(u":msg_id"_q, QVariant::fromValue(msgId));
			if (!update.exec()) {
				LOG(("DedupDb: CancelChatScanSession undo failed: %1")
					.arg(update.lastError().text()));
			}
		}
	}
	if (options.oldStateExists) {
		options.cursor = options.oldCursor;
		options.phase = options.oldPhase;
		options.elapsedMs = options.oldElapsedMs;
		options.dedupMode = options.oldDedupMode;
		options.sessionId = 0;
		options.baseMark = MsgId(0);
		options.oldStateExists = false;
		options.oldCursor = MsgId(0);
		options.oldPhase = 0;
		options.oldElapsedMs = 0;
		options.oldDedupMode = 0;
		writeScanOptions(peerId, options);
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM job_items WHERE job_id = :job_id"_q);
		q.bindValue(u":job_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(jobId)));
		if (!q.exec()) {
			LOG(("DedupDb: CancelChatScanSession undo cleanup failed: %1")
				.arg(q.lastError().text()));
		}
	} else {
		DeleteScanJobIn(_db, peerId);
	}
	RecomputeAllChatScanRefsIn(_db);
	commitTransaction();
	LOG(("DedupDb: canceled chat scan session."));
	return true;
}

void DedupDb::Impl::finishChatScanSession(PeerId peerId) {
	if (!_open) {
		return;
	}
	const auto jobId = findScanJobId(peerId).value_or(0);
	if (!jobId) {
		return;
	}
	auto options = readScanOptions(peerId);
	beginTransaction();
	{
		QSqlQuery q(_db);
		q.prepare(u"DELETE FROM job_items WHERE job_id = :job_id"_q);
		q.bindValue(u":job_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(jobId)));
		q.exec();
	}
	options.sessionId = 0;
	options.baseMark = MsgId(0);
	options.oldStateExists = false;
	options.oldCursor = MsgId(0);
	options.oldPhase = 0;
	options.oldElapsedMs = 0;
	options.oldDedupMode = 0;
	writeScanOptions(peerId, options);
	commitTransaction();
}

std::optional<std::pair<uint64, MsgId>> DedupDb::Impl::chatScanSession(
		PeerId peerId) const {
	if (!_open) {
		return std::nullopt;
	}
	const auto options = readScanOptions(peerId);
	if (!options.sessionId) {
		return std::nullopt;
	}
	return std::make_pair(options.sessionId, options.baseMark);
}

std::vector<MsgId> DedupDb::Impl::chatScanSessionItemIds(
		PeerId peerId,
		int category) const {
	auto result = std::vector<MsgId>();
	if (!_open) {
		return result;
	}
	const auto options = readScanOptions(peerId);
	if (!options.sessionId) {
		return result;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT msg_id FROM chat_scan_items "
		"WHERE peer_id = :peer_id AND category = :category "
		"AND tracked = 1 AND session_id = :session_id "
		"ORDER BY msg_id DESC"_q);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":category"_q, category);
	q.bindValue(u":session_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(options.sessionId)));
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanSessionItemIds failed: %1").arg(
			q.lastError().text()));
		return result;
	}
	while (q.next()) {
		result.push_back(MsgId(q.value(0).toLongLong()));
	}
	return result;
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
	q.prepare(u"SELECT i.category, i.size, i.media_id, i.is_photo, "
		"h.hash, i.is_dup, i.session_id, i.filename, i.tracked "
		"FROM chat_scan_items i "
		"LEFT JOIN hashes h ON h.hash_id = i.hash_id "
		"WHERE i.peer_id = :peer_id "
		"AND i.msg_id = :msg_id"_q);
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
	item.isDup = (q.value(5).toInt() != 0);
	item.sessionId = q.value(6).toULongLong();
	item.filename = q.value(7).toString();
	item.tracked = (q.value(8).toInt() != 0);
	return item;
}

std::optional<ChatScanItem> DedupDb::Impl::chatScanDuplicateSuccessor(
		PeerId peerId,
		MsgId deletedId,
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash) const {
	if (!_open || !deletedId || (!mediaId && hash.isEmpty())) {
		return std::nullopt;
	}
	auto conditions = QStringList();
	if (mediaId) {
		conditions.push_back(
			u"(i.media_id = :media_id AND i.is_photo = :is_photo)"_q);
	}
	if (!hash.isEmpty()) {
		conditions.push_back(
			u"i.hash_id = (SELECT hash_id FROM hashes WHERE hash = :hash)"_q);
	}
	auto sql = u"SELECT i.msg_id, i.category, i.size, i.media_id, i.is_photo, "
		"h.hash, i.is_dup FROM chat_scan_items i "
		"LEFT JOIN hashes h ON h.hash_id = i.hash_id "
		"WHERE i.peer_id = :peer_id AND i.is_dup = 1 "
		"AND i.tracked = 1 "
		"AND i.msg_id != :deleted_id AND (%1) "
		"ORDER BY i.msg_id DESC LIMIT 1"_q.arg(conditions.join(u" OR "_q));
	QSqlQuery q(_db);
	q.prepare(sql);
	q.bindValue(u":peer_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(peerId.value)));
	q.bindValue(u":deleted_id"_q, QVariant::fromValue(
		static_cast<qlonglong>(deletedId.bare)));
	if (mediaId) {
		q.bindValue(u":media_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(mediaId)));
		q.bindValue(u":is_photo"_q, isPhoto ? 1 : 0);
	}
	if (!hash.isEmpty()) {
		q.bindValue(u":hash"_q, hash);
	}
	if (!q.exec()) {
		LOG(("DedupDb: ChatScanDuplicateSuccessor failed: %1").arg(
			q.lastError().text()));
		return std::nullopt;
	}
	if (!q.next()) {
		return std::nullopt;
	}
	auto item = ChatScanItem();
	item.peerId = peerId;
	item.msgId = MsgId(q.value(0).toLongLong());
	item.category = q.value(1).toInt();
	item.size = q.value(2).toLongLong();
	item.mediaId = q.value(3).toULongLong();
	item.isPhoto = (q.value(4).toInt() != 0);
	item.hash = q.value(5).toByteArray();
	item.isDup = (q.value(6).toInt() != 0);
	return item;
}

JobKind DedupDb::Impl::TmpJobKind(TmpTable table) {
	switch (table) {
	case TmpTable::Dl: return JobKind::DownloadBatch;
	case TmpTable::Ul: return JobKind::UploadBatch;
	case TmpTable::Fw: return JobKind::ForwardBatch;
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
	s.jobId = findJobId(TmpJobKind(table), sessionId, scopeId);
	if (!s.jobId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"SELECT item_id, media_id, is_photo, hash_id "
		"FROM job_items WHERE job_id = :job_id "
		"ORDER BY item_id"_q);
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(s.jobId)));
	if (!q.exec()) {
		LOG(("DedupDb: LoadTmp failed: %1").arg(q.lastError().text()));
		return;
	}
	while (q.next()) {
		const auto itemId = q.value(0).toULongLong();
		const auto docId = q.value(1).toULongLong();
		const auto type = (q.value(2).toInt() != 0) ? 1 : 0;
		const auto hash = hashForHashId(q.value(3).toULongLong());
		s.nextItemId = std::max(s.nextItemId, itemId + 1);
		if (!docId || hash.isEmpty()) {
			continue;
		}
		s.docToHash[type][docId] = hash;
		s.hashToDoc[type][hash].insert(docId);
	}
}

void DedupDb::Impl::insertTmp(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		uint64 documentId,
		bool isPhoto,
		const QByteArray &hash) {
	if (!_open || !documentId || hash.isEmpty()) {
		return;
	}
	const auto type = isPhoto ? 1 : 0;
	auto &s = tmpState(table, sessionId, scopeId);
	s.docToHash[type][documentId] = hash;
	s.hashToDoc[type][hash].insert(documentId);
	s.pending.push_back({ documentId, isPhoto, hash });
	if (++_tmpPendingWrites >= kExWriteBatchLimit) {
		flushTmpPending();
	}
}

void DedupDb::Impl::removeTmpDoc(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		uint64 documentId,
		bool isPhoto) {
	if (!documentId) {
		return;
	}
	const auto type = isPhoto ? 1 : 0;
	auto jobId = uint64(0);
	if (const auto i = _tmpStates.find({ table, sessionId, scopeId });
		i != _tmpStates.end()) {
		auto &s = i->second;
		jobId = s.jobId;
		if (const auto j = s.docToHash[type].find(documentId);
			j != s.docToHash[type].end()) {
			const auto hash = j.value();
			s.docToHash[type].erase(j);
			if (const auto k = s.hashToDoc[type].find(hash);
				k != s.hashToDoc[type].end()) {
				k.value().remove(documentId);
				if (k.value().isEmpty()) {
					s.hashToDoc[type].erase(k);
				}
			}
		}
		auto &pending = s.pending;
		auto w = pending.begin();
		for (auto r = pending.begin(); r != pending.end(); ++r) {
			if (r->docId != documentId || r->isPhoto != isPhoto) {
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
	if (!jobId) {
		jobId = findJobId(TmpJobKind(table), sessionId, scopeId);
	}
	if (!jobId) {
		return;
	}
	QSqlQuery q(_db);
	q.prepare(u"DELETE FROM job_items WHERE job_id = :job_id "
		"AND media_id = :media_id AND is_photo = :is_photo"_q);
	q.bindValue(u":job_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(jobId)));
	q.bindValue(u":media_id"_q, QVariant::fromValue(
		static_cast<qulonglong>(documentId)));
	q.bindValue(u":is_photo"_q, type);
	if (!q.exec()) {
		LOG(("DedupDb: RemoveTmpDoc failed: %1").arg(
			q.lastError().text()));
	}
}

void DedupDb::Impl::flushTmpState(
		const TmpKey &key,
		TmpState &state) {
	if (state.pending.empty()) {
		return;
	}
	const auto kind = TmpJobKind(key.table);
	if (!state.jobId) {
		state.jobId = findJobId(kind, key.sessionId, key.scopeId);
	}
	if (!state.jobId) {
		auto job = JobRecord();
		job.kind = kind;
		job.sessionId = key.sessionId;
		job.scopeId = key.scopeId;
		job.state = JobState::Pending;
		state.jobId = createJob(job);
		if (!state.jobId) {
			return;
		}
	}
	QSqlQuery del(_db);
	del.prepare(u"DELETE FROM job_items WHERE job_id = :job_id "
		"AND media_id = :media_id AND is_photo = :is_photo"_q);
	QSqlQuery ins(_db);
	ins.prepare(u"INSERT OR REPLACE INTO job_items "
		"(job_id, item_id, media_id, is_photo, hash_id, state) "
		"VALUES (:job_id, :item_id, :media_id, :is_photo, "
		":hash_id, :state)"_q);
	for (const auto &row : state.pending) {
		const auto hashId = resolveHashId(row.hash);
		del.bindValue(u":job_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(state.jobId)));
		del.bindValue(u":media_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(row.docId)));
		del.bindValue(u":is_photo"_q, row.isPhoto ? 1 : 0);
		if (!del.exec()) {
			LOG(("DedupDb: RemoveTmpDoc failed: %1").arg(
				del.lastError().text()));
		}
		ins.bindValue(u":job_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(state.jobId)));
		ins.bindValue(u":item_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(state.nextItemId++)));
		ins.bindValue(u":media_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(row.docId)));
		ins.bindValue(u":is_photo"_q, row.isPhoto ? 1 : 0);
		ins.bindValue(u":hash_id"_q, QVariant::fromValue(
			static_cast<qulonglong>(hashId)));
		ins.bindValue(u":state"_q, int(JobState::Done));
		if (!ins.exec()) {
			LOG(("DedupDb: InsertTmp failed: %1").arg(
				ins.lastError().text()));
		}
	}
	state.pending.clear();
}

void DedupDb::Impl::flushTmpPending() {
	if (_tmpPendingWrites == 0 || !_open) {
		return;
	}
	_tmpPendingWrites = 0;
	beginTransaction();
	for (auto &[key, state] : _tmpStates) {
		flushTmpState(key, state);
	}
	commitTransaction();
}

bool DedupDb::Impl::containsTmpDocId(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		uint64 documentId,
		bool isPhoto) const {
	if (!documentId) {
		return false;
	}
	const auto type = isPhoto ? 1 : 0;
	return tmpState(table, sessionId, scopeId).docToHash[type].contains(
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
	for (auto type = 0; type != 2; ++type) {
		const auto i = s.hashToDoc[type].find(hash);
		if (i != s.hashToDoc[type].end() && !i->isEmpty()) {
			return true;
		}
	}
	return false;
}

QByteArray DedupDb::Impl::hashForTmpDocId(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId,
		uint64 documentId,
		bool isPhoto) const {
	if (!documentId) {
		return {};
	}
	const auto type = isPhoto ? 1 : 0;
	return tmpState(table, sessionId, scopeId).docToHash[type].value(
		documentId);
}

void DedupDb::Impl::clearTmpRun(
		TmpTable table,
		uint64 sessionId,
		uint64 scopeId) {
	auto jobId = uint64(0);
	if (const auto i = _tmpStates.find({ table, sessionId, scopeId });
		i != _tmpStates.end()) {
		jobId = i->second.jobId;
		_tmpPendingWrites -= int(i->second.pending.size());
		_tmpStates.erase(i);
	}
	if (!_open) {
		return;
	}
	if (!jobId) {
		jobId = findJobId(TmpJobKind(table), sessionId, scopeId);
	}
	if (jobId) {
		removeJob(jobId);
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
		bool isPhoto,
		const QByteArray &hash) {
	_impl->insertExTmp(sessionId, peerId, documentId, isPhoto, hash);
}

bool DedupDb::containsExTmpDocId(
		uint64 sessionId,
		PeerId peerId,
		uint64 documentId,
		bool isPhoto) const {
	return _impl->containsExTmpDocId(
		sessionId,
		peerId,
		documentId,
		isPhoto);
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

void DedupDb::clearExTmpRun(uint64 sessionId, PeerId peerId) {
	_impl->clearExTmpRun(sessionId, peerId);
}

void DedupDb::insertDlTmp(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId,
		bool isPhoto,
		const QByteArray &hash) {
	_impl->insertTmp(
		Impl::TmpTable::Dl,
		sessionId,
		batchId,
		documentId,
		isPhoto,
		hash);
}

bool DedupDb::containsDlTmpDocId(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId,
		bool isPhoto) const {
	return _impl->containsTmpDocId(
		Impl::TmpTable::Dl,
		sessionId,
		batchId,
		documentId,
		isPhoto);
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

void DedupDb::clearDlTmpRun(uint64 sessionId, uint64 batchId) {
	_impl->clearTmpRun(Impl::TmpTable::Dl, sessionId, batchId);
}

void DedupDb::removeDlTmpDoc(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId,
		bool isPhoto) {
	_impl->removeTmpDoc(
		Impl::TmpTable::Dl,
		sessionId,
		batchId,
		documentId,
		isPhoto);
}

void DedupDb::insertUlTmp(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId,
		bool isPhoto,
		const QByteArray &hash) {
	_impl->insertTmp(
		Impl::TmpTable::Ul,
		sessionId,
		batchId,
		documentId,
		isPhoto,
		hash);
}

bool DedupDb::containsUlTmpDocId(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId,
		bool isPhoto) const {
	return _impl->containsTmpDocId(
		Impl::TmpTable::Ul,
		sessionId,
		batchId,
		documentId,
		isPhoto);
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

void DedupDb::clearUlTmpRun(uint64 sessionId, uint64 batchId) {
	_impl->clearTmpRun(Impl::TmpTable::Ul, sessionId, batchId);
}

void DedupDb::insertFwTmp(
		uint64 sessionId,
		PeerId dstPeer,
		uint64 itemId,
		bool isPhoto,
		const QByteArray &hash) {
	_impl->insertTmp(
		Impl::TmpTable::Fw,
		sessionId,
		dstPeer.value,
		itemId,
		isPhoto,
		hash);
}

bool DedupDb::containsFwTmpItem(
		uint64 sessionId,
		PeerId dstPeer,
		uint64 itemId,
		bool isPhoto) const {
	return _impl->containsTmpDocId(
		Impl::TmpTable::Fw,
		sessionId,
		dstPeer.value,
		itemId,
		isPhoto);
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
		uint64 itemId,
		bool isPhoto) const {
	return _impl->hashForTmpDocId(
		Impl::TmpTable::Fw,
		sessionId,
		dstPeer.value,
		itemId,
		isPhoto);
}

void DedupDb::clearFwTmpRun(uint64 sessionId, PeerId dstPeer) {
	_impl->clearTmpRun(Impl::TmpTable::Fw, sessionId, dstPeer.value);
}

std::vector<ChatScanRow> DedupDb::readChatScan(PeerId peerId) const {
	return _impl->readChatScan(peerId);
}

void DedupDb::readChatScanAsync(
		PeerId peerId,
		Fn<void(std::vector<ChatScanRow>, bool)> done) {
	_impl->readChatScanAsync(peerId, std::move(done));
}

bool DedupDb::isChatScanned(PeerId peerId) const {
	return _impl->isChatScanned(peerId);
}

void DedupDb::markChatDeleteFinished(PeerId peerId) {
	_impl->markChatDeleteFinished(peerId);
}

void DedupDb::requestIncrementalVacuum() {
	_impl->requestIncrementalVacuum();
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

bool DedupDb::chatScanCategoryHasItems(
		PeerId peerId,
		int category) const {
	return _impl->chatScanCategoryHasItems(peerId, category);
}

MsgId DedupDb::chatScanMaxMsgId(PeerId peerId) const {
	return _impl->chatScanMaxMsgId(peerId);
}

bool DedupDb::chatScanContainsHash(
		PeerId peerId,
		const QByteArray &hash) const {
	return _impl->chatScanContainsHash(peerId, hash);
}

MsgId DedupDb::chatScanUniqueHash(
		PeerId peerId,
		const QByteArray &hash) const {
	return _impl->chatScanUniqueHash(peerId, hash);
}

QByteArray DedupDb::chatScanHashForMedia(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto) const {
	return _impl->chatScanHashForMedia(peerId, mediaId, isPhoto);
}

QByteArray DedupDb::chatScanHashForMediaOther(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto) const {
	return _impl->chatScanHashForMediaOther(peerId, mediaId, isPhoto);
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
		bool isPhoto,
		qint64 retryAt) {
	_impl->chatScanReleaseItem(peerId, mediaId, isPhoto, retryAt);
}

void DedupDb::chatScanReleaseLeases(PeerId peerId) {
	_impl->chatScanReleaseLeases(peerId);
}

int DedupDb::chatScanCommitHashes(
		PeerId peerId,
		const std::vector<ChatScanHash> &hashes) {
	return _impl->chatScanCommitHashes(peerId, hashes);
}

void DedupDb::chatScanFailItem(
		PeerId peerId,
		uint64 mediaId,
		bool isPhoto) {
	_impl->chatScanFailItem(peerId, mediaId, isPhoto);
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

void DedupDb::recomputeChatScanProgressAsync(
		PeerId peerId,
		Fn<void(std::vector<ChatScanProgressRow>, bool)> done) {
	_impl->recomputeChatScanProgressAsync(peerId, std::move(done));
}

void DedupDb::chatScanUniqueHashesAsync(
		PeerId peerId,
		Fn<void(std::vector<std::pair<QByteArray, MsgId>>, bool)> done) {
	_impl->chatScanUniqueHashesAsync(peerId, std::move(done));
}

void DedupDb::chatScanItemIdsAsync(
		PeerId peerId,
		int category,
		Fn<void(std::vector<MsgId>, bool)> done) {
	_impl->chatScanItemIdsAsync(peerId, category, std::move(done));
}

void DedupDb::chatScanMediaHashesAsync(
		PeerId peerId,
		bool includeOther,
		Fn<void(std::vector<ChatScanMediaHash>, bool)> done) {
	_impl->chatScanMediaHashesAsync(
		peerId,
		includeOther,
		std::move(done));
}

std::vector<ChatScanItem> DedupDb::chatScanDistinctItems(
		PeerId peerId) const {
	return _impl->chatScanDistinctItems(peerId);
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

void DedupDb::clearChatScan(PeerId peerId) {
	_impl->clearChatScan(peerId);
}

void DedupDb::clearChatScanAsync(
		PeerId peerId,
		Fn<void(bool, std::vector<ChatScanOwnerChange>)> done) {
	_impl->clearChatScanAsync(peerId, std::move(done));
}

void DedupDb::writeScanCursor(const ChatScanState &state) {
	_impl->writeScanCursor(state);
}

std::optional<ChatScanState> DedupDb::readScanCursor(PeerId peerId) const {
	return _impl->readScanCursor(peerId);
}

void DedupDb::finishChatScan(
		PeerId peerId,
		MsgId scannedUpto,
		int mode,
		bool deleteFinished,
		int64 elapsedMs) {
	_impl->finishChatScan(peerId, scannedUpto, mode, deleteFinished, elapsedMs);
}

MsgId DedupDb::chatScanScannedUpto(PeerId peerId) const {
	return _impl->chatScanScannedUpto(peerId);
}

int DedupDb::chatScanMode(PeerId peerId) const {
	return _impl->chatScanMode(peerId);
}

void DedupDb::writeChatScanMode(PeerId peerId, int mode) {
	_impl->writeChatScanMode(peerId, mode);
}

void DedupDb::writeChatScanItems(const std::vector<ChatScanItem> &items) {
	_impl->writeChatScanItems(items);
}

std::vector<ChatScanItem> DedupDb::readChatScanItems(PeerId peerId) const {
	return _impl->readChatScanItems(peerId);
}

int DedupDb::updateChatScanItemHash(
		PeerId peerId,
		MsgId msgId,
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash) {
	return _impl->updateChatScanItemHash(
		peerId,
		msgId,
		mediaId,
		isPhoto,
		hash);
}

std::vector<PeerId> DedupDb::scannedPeerIds() const {
	return _impl->scannedPeerIds();
}

void DedupDb::removeChatScanItem(PeerId peerId, MsgId msgId) {
	_impl->removeChatScanItem(peerId, msgId);
}

void DedupDb::markChatScanItemDup(
		PeerId peerId,
		MsgId msgId,
		bool isDup,
		const QString &filename) {
	_impl->markChatScanItemDup(peerId, msgId, isDup, filename);
}

bool DedupDb::beginChatScanSession(
		PeerId peerId,
		uint64 sessionId,
		MsgId baseMark) {
	return _impl->beginChatScanSession(peerId, sessionId, baseMark);
}

bool DedupDb::cancelChatScanSession(PeerId peerId) {
	return _impl->cancelChatScanSession(peerId);
}

void DedupDb::finishChatScanSession(PeerId peerId) {
	_impl->finishChatScanSession(peerId);
}

std::optional<std::pair<uint64, MsgId>> DedupDb::chatScanSession(
		PeerId peerId) const {
	return _impl->chatScanSession(peerId);
}

std::vector<MsgId> DedupDb::chatScanSessionItemIds(
		PeerId peerId,
		int category) const {
	return _impl->chatScanSessionItemIds(peerId, category);
}

bool DedupDb::hasChatScanItem(PeerId peerId, MsgId msgId) const {
	return _impl->hasChatScanItem(peerId, msgId);
}

std::optional<ChatScanItem> DedupDb::chatScanItem(
		PeerId peerId,
		MsgId msgId) const {
	return _impl->chatScanItem(peerId, msgId);
}

std::optional<ChatScanItem> DedupDb::chatScanDuplicateSuccessor(
		PeerId peerId,
		MsgId deletedId,
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash) const {
	return _impl->chatScanDuplicateSuccessor(
		peerId,
		deletedId,
		mediaId,
		isPhoto,
		hash);
}

uint64 DedupDb::createJob(const JobRecord &job) {
	return _impl->createJob(job);
}

std::optional<JobRecord> DedupDb::job(uint64 jobId) const {
	return _impl->job(jobId);
}

std::optional<JobRecord> DedupDb::findJob(
		JobKind kind,
		uint64 scopeId) const {
	return _impl->findJob(kind, scopeId, false);
}

std::optional<JobRecord> DedupDb::findActiveJob(
		JobKind kind,
		uint64 scopeId) const {
	return _impl->findJob(kind, scopeId, true);
}

void DedupDb::setJobState(uint64 jobId, JobState state) {
	_impl->setJobState(jobId, state);
}

void DedupDb::setJobProgress(
		uint64 jobId,
		int total,
		int done,
		int skipped) {
	_impl->setJobProgress(jobId, total, done, skipped);
}

void DedupDb::pauseJob(uint64 jobId) {
	_impl->pauseJob(jobId);
}

void DedupDb::finishJob(uint64 jobId) {
	_impl->finishJob(jobId);
}

void DedupDb::cancelJob(uint64 jobId) {
	_impl->cancelJob(jobId);
}

void DedupDb::finishScanJob(uint64 jobId) {
	_impl->finishScanJob(jobId);
}

void DedupDb::removeJobItems(uint64 jobId) {
	_impl->removeJobItems(jobId);
}

void DedupDb::writeJobItems(const std::vector<JobItemRecord> &items) {
	_impl->writeJobItems(items);
}

void DedupDb::writeJobItem(const JobItemRecord &item) {
	_impl->writeJobItem(item);
}

std::vector<JobItemRecord> DedupDb::jobItems(uint64 jobId) const {
	return _impl->jobItems(jobId);
}

std::vector<JobItemRecord> DedupDb::jobResumeItems(uint64 jobId) const {
	return _impl->jobResumeItems(jobId);
}

void DedupDb::setJobItemState(
		uint64 jobId,
		uint64 itemId,
		JobState state,
		const QString &error,
		int retry) {
	_impl->setJobItemState(jobId, itemId, state, error, retry);
}

bool DedupDb::jobContainsMedia(
		uint64 jobId,
		uint64 mediaId,
		bool isPhoto) const {
	return _impl->jobContainsMedia(jobId, mediaId, isPhoto);
}

bool DedupDb::jobContainsHash(
		uint64 jobId,
		uint64 hashId) const {
	return _impl->jobContainsHash(jobId, hashId);
}

void DedupDb::beginTransaction() {
	_impl->beginTransaction();
}

void DedupDb::commitTransaction() {
	_impl->commitTransaction();
}

} // namespace Data