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
#include <vector>

#include "data/data_msg_id.h"

namespace Data {

struct DedupRecord {
	QByteArray hash;
	int64 size = 0;
	uint64 documentId = 0;
	QString status = u"f"_q;
};

struct ResumeDlRecord {
	uint64 peerId = 0;
	uint64 peerAccessHash = 0;
	int64 msgId = 0;
	uint64 documentId = 0;
	int64 size = 0;
	QString path;
};

struct ResumeUlRecord {
	uint64 peerId = 0;
	QString path;
	int64 size = 0;
	int partsSent = 0;
	int64 sentSize = 0;
	uint64 fileId = 0;
	int64 topicRootId = 0;
};

struct EfResumeItem {
	QString jobId;
	int itemIndex = 0;
	PeerId peerId;
	FullMsgId sourceId;
	QString state;
	QString localPath;
	uint64 fileId = 0;
	int uploadedParts = 0;
	int64 fileSize = 0;
	QByteArray fileHash;
	uint64 mediaId = 0;
	int64 createdAt = 0;
	int64 updatedAt = 0;
};

class DedupDb {
public:
	enum class Table {
		Downloads,
		Uploads,
	};

	explicit DedupDb(const QString &path);
	~DedupDb();

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
	class Impl;
	std::unique_ptr<Impl> _impl;
};

} // namespace Data
