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
#include <vector>

namespace Data {

struct DedupRecord {
	QByteArray hash;
	int64 size = 0;
	uint64 documentId = 0;
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
	class Impl;
	std::unique_ptr<Impl> _impl;
};

} // namespace Data
