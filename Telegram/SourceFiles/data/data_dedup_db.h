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
	std::vector<MsgId> remaining;
};

struct ExResumeRecord {
	uint64 sessionId = 0;
	PeerId peerId = PeerId();
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
	int jsonState = 0;
	QByteArray stats;
	uint64 docId = 0;
	QString pausedFile;
	int64 pausedBytes = 0;
	QByteArray lastMsg;
	int splitIndex = 0;
	int selectedDone = 0;
	int filterIndex = 0;
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
	void removeExResume(uint64 sessionId, PeerId peerId);
	void clearExResume(uint64 sessionId);
	[[nodiscard]] std::vector<ExResumeRecord> loadExResume(
		uint64 sessionId) const;

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

	void beginTransaction();
	void commitTransaction();

private:
	class Impl;
	std::unique_ptr<Impl> _impl;
};

} // namespace Data