/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include "export/export_settings.h"
#include <memory>
#include <map>

namespace Export {

struct IncompleteFile {
	QString filename;
	int64 bytesDownloaded = 0;
	int64 totalSize = 0;
	uint64 messageId = 0;
};

struct TypeCounter {
	int uniqueCount = 0;
	int64 uniqueSize = 0;
	int totalCount = 0;
	int64 totalSize = 0;
	int messagesWithLinks = 0;
};

struct ExportProgress {
	uint64 lastMessageId = 0;
	uint64 rangeEndMsgId = 0;
	QString lastFilename;
	int64 lastFileSize = 0;

	int messagesProcessed = 0;
	int scanTotalMessages = 0;
	std::map<int, TypeCounter> typeCounters;
	std::map<int, TypeCounter> scanStats;

	std::vector<IncompleteFile> incompleteFiles;

	// Session state for resume/update flow
	bool isComplete = false;  // Export completed (not interrupted)
	bool hasMedia = false;  // True if export included media file downloads
	QString lastExportDate;       // ISO format date of last export
	Settings settings;
	
	QJsonObject toJson() const;
	static ExportProgress fromJson(const QJsonObject &obj);
	
	bool save(const QString &path) const;
	static std::unique_ptr<ExportProgress> load(const QString &path);
	static void remove(const QString &path);
	
	QString partialPath(const QString &folder, const QString &filename) const;
	static QString progressFilePath(const QString &folder);
};

} // namespace Export
