/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/export_progress.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QFileInfo>
#include <algorithm>

namespace Export {

constexpr auto kProgressFileName = "progress.json";

QString ExportProgress::progressFilePath(const QString &folder) {
	return folder + '/' + kProgressFileName;
}

QString ExportProgress::partialPath(const QString &folder, const QString &filename) const {
	return folder + '/' + filename + ".partial";
}

QJsonObject ExportProgress::toJson() const {
	QJsonObject obj;

	if (!lastFilename.isEmpty()) {
		obj["last_filename"] = lastFilename;
		obj["last_file_size"] = lastFileSize;
	}
	if (lastMessageId > 0) {
		obj["last_message_id"] = static_cast<qint64>(lastMessageId);
	}

	if (!incompleteFiles.empty()) {
		QJsonArray arr;
		for (const auto &file : incompleteFiles) {
			QJsonObject entry;
			entry["filename"] = file.filename;
			entry["bytes_downloaded"] = file.bytesDownloaded;
			entry["total_size"] = file.totalSize;
			entry["message_id"] = static_cast<qint64>(file.messageId);
			arr.append(entry);
		}
		obj["incomplete_files"] = arr;
	}

	// Serialize Settings
	QJsonObject s;
	s["types"] = static_cast<qint64>(settings.types);
	s["full_chats"] = static_cast<qint64>(settings.fullChats);
	s["format"] = static_cast<int>(settings.format);
	
	QJsonObject m;
	m["types"] = static_cast<qint64>(settings.media.types);
	m["size_limit"] = settings.media.sizeLimit;
	m["ext_mode"] = static_cast<int>(settings.media.extensionFilterMode);
	m["extensions"] = QJsonArray::fromStringList(settings.media.extensionFilter);
	s["media"] = m;

	obj["settings"] = s;

	return obj;
}

ExportProgress ExportProgress::fromJson(const QJsonObject &obj) {
	ExportProgress result;
	
	if (obj.contains("last_message_id")) {
		result.lastMessageId = static_cast<uint64>(obj["last_message_id"].toInteger());
	}
	if (obj.contains("last_filename")) {
		result.lastFilename = obj["last_filename"].toString();
		result.lastFileSize = obj["last_file_size"].toInteger();
	}
	
	if (obj.contains("incomplete_files")) {
		const auto arr = obj["incomplete_files"].toArray();
		for (const auto val : arr) {
			const auto entry = val.toObject();
			IncompleteFile file;
			file.filename = entry["filename"].toString();
			file.bytesDownloaded = entry["bytes_downloaded"].toInteger();
			file.totalSize = entry["total_size"].toInteger();
			file.messageId = static_cast<uint64>(entry["message_id"].toInteger());
			result.incompleteFiles.push_back(std::move(file));
		}
	}

	if (obj.contains("settings")) {
		const auto s = obj["settings"].toObject();
		result.settings.types = static_cast<Settings::Type>(static_cast<uint32>(s["types"].toInteger()));
		result.settings.fullChats = static_cast<Settings::Type>(static_cast<uint32>(s["full_chats"].toInteger()));
		result.settings.format = static_cast<Output::Format>(s["format"].toInt());
		
		const auto m = s["media"].toObject();
		result.settings.media.types = static_cast<MediaSettings::Type>(static_cast<uint32>(m["types"].toInteger()));
		result.settings.media.sizeLimit = m["size_limit"].toVariant().toLongLong();
		result.settings.media.extensionFilterMode = static_cast<MediaSettings::ExtFilterMode>(m["ext_mode"].toInt());
		
		const auto exts = m["extensions"].toArray();
		for (const auto &ext : exts) {
			result.settings.media.extensionFilter.append(ext.toString());
		}
	}
	
	return result;
}

bool ExportProgress::save(const QString &path) const {
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		return false;
	}
	
	const auto json = QJsonDocument(toJson()).toJson(QJsonDocument::Compact);
	if (file.write(json) != json.size()) {
		file.close();
		QFile::remove(path);
		return false;
	}
	
	return true;
}

std::unique_ptr<ExportProgress> ExportProgress::load(const QString &path) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return nullptr;
	}
	
	const auto data = file.readAll();
	file.close();
	
	const auto doc = QJsonDocument::fromJson(data);
	if (doc.isNull() || !doc.isObject()) {
		return nullptr;
	}
	
	return std::make_unique<ExportProgress>(fromJson(doc.object()));
}

void ExportProgress::remove(const QString &path) {
	QFile::remove(path);
}

} // namespace Export
