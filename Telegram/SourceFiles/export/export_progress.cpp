/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/export_progress.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QDir>

namespace Export {

QJsonObject ExportProgress::toJson() const {
	QJsonObject obj;
	obj["last_message_id"] = QString::number(lastMessageId);
	obj["range_end_msg_id"] = QString::number(rangeEndMsgId);
	obj["last_filename"] = lastFilename;
	obj["last_file_size"] = QString::number(lastFileSize);
	obj["messages_processed"] = messagesProcessed;
	obj["messages_total_count"] = messagesTotalCount;
	obj["scan_total_messages"] = scanTotalMessages;

	// Session state fields
	obj["is_complete"] = isComplete;
	obj["range_end_msg_id"] = QString::number(rangeEndMsgId);
	obj["last_export_date"] = lastExportDate;

	if (!typeCounters.empty()) {
		QJsonObject counters;
		for (const auto &[type, counter] : typeCounters) {
			QJsonObject c;
			c["unique"] = counter.uniqueCount;
			c["unique_size"] = QString::number(counter.uniqueSize);
			c["total"] = counter.localTotalCount;
			c["total_size"] = QString::number(counter.totalSize);
			if (counter.messagesWithLinks > 0) {
				c["links"] = counter.messagesWithLinks;
			}
			counters[QString::number(type)] = c;
		}
		obj["type_counters"] = counters;
	}

	if (!scanStats.empty()) {
		QJsonObject counters;
		for (const auto &[type, counter] : scanStats) {
			QJsonObject c;
			c["unique"] = counter.uniqueCount;
			c["unique_size"] = QString::number(counter.uniqueSize);
			c["total"] = counter.localTotalCount;
			c["total_size"] = QString::number(counter.totalSize);
			if (counter.messagesWithLinks > 0) {
				c["links"] = counter.messagesWithLinks;
			}
			counters[QString::number(type)] = c;
		}
		obj["scan_stats"] = counters;
	}

	if (!dedupById.empty()) {
		QJsonObject dedup;
		for (const auto &[id, path] : dedupById) {
			dedup[QString::number(id)] = path;
		}
		obj["dedup_by_id"] = dedup;
	}

	if (!dedupBySizeName.empty()) {
		QJsonObject dedup;
		for (const auto &[key, path] : dedupBySizeName) {
			dedup[key] = path;
		}
		obj["dedup_by_size_name"] = dedup;
	}

	if (!incompleteFiles.empty()) {
		QJsonArray incomplete;
		for (const auto &file : incompleteFiles) {
			QJsonObject f;
			f["filename"] = file.filename;
			f["downloaded"] = QString::number(file.bytesDownloaded);
			f["total"] = QString::number(file.totalSize);
			f["msg_id"] = QString::number(file.messageId);
			incomplete.append(f);
		}
		obj["incomplete_files"] = incomplete;
	}

	if (settings.media.types != MediaSettings::Types(0) || settings.types != Settings::Types(0)) {
		QJsonObject s;
		s["media_types"] = static_cast<int>(settings.media.types.value());
		s["media_size_limit"] = QString::number(settings.media.sizeLimit);
		s["ext_filter_mode"] = static_cast<int>(settings.media.extensionFilterMode);
		if (!settings.media.extensionFilter.isEmpty()) {
			s["ext_filter"] = settings.media.extensionFilter.join(",");
		}
		s["types"] = static_cast<int>(settings.types.value());
		s["full_chats"] = static_cast<int>(settings.fullChats.value());
		s["format"] = static_cast<int>(settings.format);
		
		if (settings.singlePeerFrom) s["single_peer_from"] = static_cast<int>(settings.singlePeerFrom);
		if (settings.singlePeerTill) s["single_peer_till"] = static_cast<int>(settings.singlePeerTill);
		
		s["use_id_range"] = settings.useIdRange;
		if (settings.singlePeerFromId) s["single_peer_from_id"] = settings.singlePeerFromId;
		if (settings.singlePeerTillId) s["single_peer_till_id"] = settings.singlePeerTillId;

		obj["settings"] = s;
	}

	return obj;
}

ExportProgress ExportProgress::fromJson(const QJsonObject &obj) {
	ExportProgress result;
	result.lastMessageId = obj["last_message_id"].toString().toULongLong();
	result.rangeEndMsgId = obj["range_end_msg_id"].toString().toULongLong();
	result.lastFilename = obj["last_filename"].toString();
	result.lastFileSize = obj["last_file_size"].toString().toLongLong();
	result.messagesProcessed = obj["messages_processed"].toInt();
	result.messagesTotalCount = obj["messages_total_count"].toInt();
	result.scanTotalMessages = obj["scan_total_messages"].toInt();

	// Load session state fields
	result.isComplete = obj["is_complete"].toBool();
	result.lastExportDate = obj["last_export_date"].toString();

	if (obj.contains("type_counters")) {
		const auto counters = obj["type_counters"].toObject();
		for (auto it = counters.begin(); it != counters.end(); ++it) {
			const int type = it.key().toInt();
			const auto c = it.value().toObject();
			TypeCounter counter;
			counter.uniqueCount = c["unique"].toInt();
			counter.uniqueSize = c["unique_size"].toString().toLongLong();
			counter.localTotalCount = c["total"].toInt();
			counter.totalSize = c["total_size"].toString().toLongLong();
			if (c.contains("links")) {
				counter.messagesWithLinks = c["links"].toInt();
			}
			result.typeCounters[type] = counter;
		}
	}

	if (obj.contains("scan_stats")) {
		const auto counters = obj["scan_stats"].toObject();
		for (auto it = counters.begin(); it != counters.end(); ++it) {
			const int type = it.key().toInt();
			const auto c = it.value().toObject();
			TypeCounter counter;
			counter.uniqueCount = c["unique"].toInt();
			counter.uniqueSize = c["unique_size"].toString().toLongLong();
			counter.localTotalCount = c["total"].toInt();
			counter.totalSize = c["total_size"].toString().toLongLong();
			if (c.contains("links")) {
				counter.messagesWithLinks = c["links"].toInt();
			}
			result.scanStats[type] = counter;
		}
	}

	if (obj.contains("dedup_by_id")) {
		const auto dedup = obj["dedup_by_id"].toObject();
		for (auto it = dedup.begin(); it != dedup.end(); ++it) {
			const uint64 id = it.key().toULongLong();
			const QString path = it.value().toString();
			result.dedupById[id] = path;
		}
	}

	if (obj.contains("dedup_by_size_name")) {
		const auto dedup = obj["dedup_by_size_name"].toObject();
		for (auto it = dedup.begin(); it != dedup.end(); ++it) {
			result.dedupBySizeName[it.key()] = it.value().toString();
		}
	}

	if (obj.contains("incomplete_files")) {
		const auto incomplete = obj["incomplete_files"].toArray();
		for (const auto &val : incomplete) {
			const auto f = val.toObject();
			IncompleteFile file;
			file.filename = f["filename"].toString();
			file.bytesDownloaded = f["downloaded"].toString().toLongLong();
			file.totalSize = f["total"].toString().toLongLong();
			file.messageId = f["msg_id"].toString().toULongLong();
			result.incompleteFiles.push_back(file);
		}
	}

	if (obj.contains("settings")) {
		const auto s = obj["settings"].toObject();
		result.settings.media.types = static_cast<MediaSettings::Type>(s["media_types"].toInt());
		result.settings.media.sizeLimit = s["media_size_limit"].toString().toLongLong();
		result.settings.media.extensionFilterMode = static_cast<MediaSettings::ExtFilterMode>(s["ext_filter_mode"].toInt());
		if (s.contains("ext_filter")) {
			result.settings.media.extensionFilter = s["ext_filter"].toString().split(",", Qt::SkipEmptyParts);
		}
		result.settings.types = static_cast<Settings::Type>(s["types"].toInt());
		result.settings.fullChats = static_cast<Settings::Type>(s["full_chats"].toInt());
		result.settings.format = static_cast<Output::Format>(s["format"].toInt());

		result.settings.singlePeerFrom = s["single_peer_from"].toInt();
		result.settings.singlePeerTill = s["single_peer_till"].toInt();
		
		result.settings.useIdRange = s["use_id_range"].toBool();
		result.settings.singlePeerFromId = s["single_peer_from_id"].toInt();
		result.settings.singlePeerTillId = s["single_peer_till_id"].toInt();
	}

	return result;
}

bool ExportProgress::save(const QString &path) const {
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	file.write(QJsonDocument(toJson()).toJson());
	return true;
}

std::unique_ptr<ExportProgress> ExportProgress::load(const QString &path) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return nullptr;
	}
	const auto doc = QJsonDocument::fromJson(file.readAll());
	if (doc.isNull() || !doc.isObject()) {
		return nullptr;
	}
	
	return std::make_unique<ExportProgress>(fromJson(doc.object()));
}

void ExportProgress::remove(const QString &path) {
	QFile::remove(path);
}

QString ExportProgress::partialPath(const QString &folder, const QString &filename) const {
	return folder + (folder.endsWith('/') ? "" : "/") + filename + ".partial";
}

QString ExportProgress::progressFilePath(const QString &folder) {
	return folder + (folder.endsWith('/') ? "" : "/") + "progress.json";
}

} // namespace Export
