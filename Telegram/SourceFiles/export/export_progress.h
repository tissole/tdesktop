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

namespace Export {

struct IncompleteFile {
	QString filename;
	int64 bytesDownloaded = 0;
	int64 totalSize = 0;
	uint64 messageId = 0;
};

struct ExportProgress {
	uint64 lastMessageId = 0;
	QString lastFilename;
	int64 lastFileSize = 0;
	std::vector<IncompleteFile> incompleteFiles;
	Settings settings; // Persisted settings
	
	// Serialization
	QJsonObject toJson() const;
	static ExportProgress fromJson(const QJsonObject &obj);
	
	// File I/O
	bool save(const QString &path) const;
	static std::unique_ptr<ExportProgress> load(const QString &path);
	static void remove(const QString &path);
	
	// Helpers
	QString partialPath(const QString &folder, const QString &filename) const;
	static QString progressFilePath(const QString &folder);
};

} // namespace Export
