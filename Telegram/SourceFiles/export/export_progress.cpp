/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/export_progress.h"

#include "export/export_settings.h"
#include "export/output/export_output_abstract.h"
#include <QtCore/QJsonDocument>
#include <QtCore/QDir>

namespace Export {
namespace {

QString TypeToString(int type) {
	using Type = MediaSettings::Type;
	switch (static_cast<Type>(type)) {
	case Type::Photo: return u"photos"_q;
	case Type::Video: return u"videos"_q;
	case Type::File: return u"files"_q;
	case Type::Audio: return u"audio"_q;
	case Type::VoiceMessage: return u"voice_messages"_q;
	case Type::VideoMessage: return u"video_messages"_q;
	case Type::Sticker: return u"stickers"_q;
	case Type::GIF: return u"gifs"_q;
	case Type::Link: return u"links"_q;
	case Type::Text: return u"text"_q;
	}
	return QString::number(type);
}

int StringToType(const QString &type) {
	using Type = MediaSettings::Type;
	if (type == u"photos"_q) return static_cast<int>(Type::Photo);
	if (type == u"videos"_q) return static_cast<int>(Type::Video);
	if (type == u"files"_q) return static_cast<int>(Type::File);
	if (type == u"audio"_q) return static_cast<int>(Type::Audio);
	if (type == u"voice_messages"_q) return static_cast<int>(Type::VoiceMessage);
	if (type == u"video_messages"_q) return static_cast<int>(Type::VideoMessage);
	if (type == u"stickers"_q) return static_cast<int>(Type::Sticker);
	if (type == u"gifs"_q) return static_cast<int>(Type::GIF);
	if (type == u"links"_q) return static_cast<int>(Type::Link);
	if (type == u"text"_q) return static_cast<int>(Type::Text);
	return type.toInt();
}

QString FormatToString(Output::Format format) {
	switch (format) {
	case Output::Format::Html: return u"Html"_q;
	case Output::Format::Json: return u"Json"_q;
	case Output::Format::HtmlAndJson: return u"HtmlAndJson"_q;
	}
	return u"Html"_q;
}

Output::Format StringToFormat(const QString &format) {
	if (format == u"Html"_q) return Output::Format::Html;
	if (format == u"Json"_q) return Output::Format::Json;
	if (format == u"HtmlAndJson"_q) return Output::Format::HtmlAndJson;
	const int intValue = format.toInt();
	if (intValue == 0) return Output::Format::Html;
	if (intValue == 1) return Output::Format::Json;
	if (intValue == 2) return Output::Format::HtmlAndJson;
	return Output::Format::Html;
}

QString ExtFilterModeToString(MediaSettings::ExtFilterMode mode) {
	switch (mode) {
	case MediaSettings::ExtFilterMode::None: return u"None"_q;
	case MediaSettings::ExtFilterMode::Whitelist: return u"Whitelist"_q;
	case MediaSettings::ExtFilterMode::Blacklist: return u"Blacklist"_q;
	}
	return u"None"_q;
}

MediaSettings::ExtFilterMode StringToExtFilterMode(const QString &mode) {
	if (mode == u"None"_q) return MediaSettings::ExtFilterMode::None;
	if (mode == u"Whitelist"_q) return MediaSettings::ExtFilterMode::Whitelist;
	if (mode == u"Blacklist"_q) return MediaSettings::ExtFilterMode::Blacklist;
	const int intValue = mode.toInt();
	if (intValue == 0) return MediaSettings::ExtFilterMode::None;
	if (intValue == 1) return MediaSettings::ExtFilterMode::Whitelist;
	if (intValue == 2) return MediaSettings::ExtFilterMode::Blacklist;
	return MediaSettings::ExtFilterMode::None;
}

QJsonArray MediaTypesToArray(MediaSettings::Types types) {
	QJsonArray result;
	using Type = MediaSettings::Type;
	if (types & Type::Photo) result.append(u"Photo"_q);
	if (types & Type::Video) result.append(u"Video"_q);
	if (types & Type::VoiceMessage) result.append(u"VoiceMessage"_q);
	if (types & Type::VideoMessage) result.append(u"VideoMessage"_q);
	if (types & Type::Sticker) result.append(u"Sticker"_q);
	if (types & Type::GIF) result.append(u"GIF"_q);
	if (types & Type::File) result.append(u"File"_q);
	if (types & Type::Text) result.append(u"Text"_q);
	if (types & Type::Audio) result.append(u"Audio"_q);
	if (types & Type::FullHistory) result.append(u"FullHistory"_q);
	if (types & Type::Link) result.append(u"Link"_q);
	return result;
}

MediaSettings::Types ArrayToMediaTypes(const QJsonArray &array) {
	using Type = MediaSettings::Type;
	MediaSettings::Types result;
	for (const auto &val : array) {
		const auto str = val.toString();
		if (str == u"Photo"_q) result |= Type::Photo;
		else if (str == u"Video"_q) result |= Type::Video;
		else if (str == u"VoiceMessage"_q) result |= Type::VoiceMessage;
		else if (str == u"VideoMessage"_q) result |= Type::VideoMessage;
		else if (str == u"Sticker"_q) result |= Type::Sticker;
		else if (str == u"GIF"_q) result |= Type::GIF;
		else if (str == u"File"_q) result |= Type::File;
		else if (str == u"Text"_q) result |= Type::Text;
		else if (str == u"Audio"_q) result |= Type::Audio;
		else if (str == u"FullHistory"_q) result |= Type::FullHistory;
		else if (str == u"Link"_q) result |= Type::Link;
	}
	return result;
}

QJsonArray ChatTypesToArray(Settings::Types types) {
	QJsonArray result;
	using Type = Settings::Type;
	if (types & Type::PersonalInfo) result.append(u"PersonalInfo"_q);
	if (types & Type::Userpics) result.append(u"Userpics"_q);
	if (types & Type::Contacts) result.append(u"Contacts"_q);
	if (types & Type::Sessions) result.append(u"Sessions"_q);
	if (types & Type::OtherData) result.append(u"OtherData"_q);
	if (types & Type::PersonalChats) result.append(u"PersonalChats"_q);
	if (types & Type::BotChats) result.append(u"BotChats"_q);
	if (types & Type::PrivateGroups) result.append(u"PrivateGroups"_q);
	if (types & Type::PublicGroups) result.append(u"PublicGroups"_q);
	if (types & Type::PrivateChannels) result.append(u"PrivateChannels"_q);
	if (types & Type::PublicChannels) result.append(u"PublicChannels"_q);
	if (types & Type::Stories) result.append(u"Stories"_q);
	if (types & Type::ProfileMusic) result.append(u"ProfileMusic"_q);
	return result;
}

Settings::Types ArrayToChatTypes(const QJsonArray &array) {
	using Type = Settings::Type;
	Settings::Types result;
	for (const auto &val : array) {
		const auto str = val.toString();
		if (str == u"PersonalInfo"_q) result |= Type::PersonalInfo;
		else if (str == u"Userpics"_q) result |= Type::Userpics;
		else if (str == u"Contacts"_q) result |= Type::Contacts;
		else if (str == u"Sessions"_q) result |= Type::Sessions;
		else if (str == u"OtherData"_q) result |= Type::OtherData;
		else if (str == u"PersonalChats"_q) result |= Type::PersonalChats;
		else if (str == u"BotChats"_q) result |= Type::BotChats;
		else if (str == u"PrivateGroups"_q) result |= Type::PrivateGroups;
		else if (str == u"PublicGroups"_q) result |= Type::PublicGroups;
		else if (str == u"PrivateChannels"_q) result |= Type::PrivateChannels;
		else if (str == u"PublicChannels"_q) result |= Type::PublicChannels;
		else if (str == u"Stories"_q) result |= Type::Stories;
		else if (str == u"ProfileMusic"_q) result |= Type::ProfileMusic;
	}
	return result;
}

QJsonArray OnlyMyMessagesToArray(Settings::Types chatTypes, Settings::Types fullChats) {
	QJsonArray result;
	using Type = Settings::Type;
	if ((chatTypes & Type::PersonalChats) && !(fullChats & Type::PersonalChats)) result.append(u"PersonalChats"_q);
	if ((chatTypes & Type::BotChats) && !(fullChats & Type::BotChats)) result.append(u"BotChats"_q);
	if ((chatTypes & Type::PrivateGroups) && !(fullChats & Type::PrivateGroups)) result.append(u"PrivateGroups"_q);
	if ((chatTypes & Type::PublicGroups) && !(fullChats & Type::PublicGroups)) result.append(u"PublicGroups"_q);
	if ((chatTypes & Type::PrivateChannels) && !(fullChats & Type::PrivateChannels)) result.append(u"PrivateChannels"_q);
	if ((chatTypes & Type::PublicChannels) && !(fullChats & Type::PublicChannels)) result.append(u"PublicChannels"_q);
	return result;
}

Settings::Types ArrayToFullChats(Settings::Types chatTypes, const QJsonArray &onlyMyMessages) {
	Settings::Types result = chatTypes;
	using Type = Settings::Type;
	for (const auto &val : onlyMyMessages) {
		const auto str = val.toString();
		if (str == u"PersonalChats"_q) result &= ~Type::PersonalChats;
		else if (str == u"BotChats"_q) result &= ~Type::BotChats;
		else if (str == u"PrivateGroups"_q) result &= ~Type::PrivateGroups;
		else if (str == u"PublicGroups"_q) result &= ~Type::PublicGroups;
		else if (str == u"PrivateChannels"_q) result &= ~Type::PrivateChannels;
		else if (str == u"PublicChannels"_q) result &= ~Type::PublicChannels;
	}
	return result;
}

} // namespace

QJsonObject ExportProgress::toJson() const {
	QJsonObject obj;
	obj["last_message_id"] = QString::number(lastMessageId);
	obj["range_end_msg_id"] = QString::number(rangeEndMsgId);
	obj["last_filename"] = lastFilename;
	obj["last_file_size"] = QString::number(lastFileSize);
	obj["messages_processed"] = messagesProcessed;
	obj["scan_total_messages"] = scanTotalMessages;

	// Session state fields
	obj["is_complete"] = isComplete;
	obj["last_export_date"] = lastExportDate;

	const auto writeCounters = [&](const std::map<int, TypeCounter> &source) {
		QJsonObject counters;
		for (const auto &[type, counter] : source) {
			QJsonObject c;
			if (counter.uniqueCount > 0) c["unique"] = counter.uniqueCount;
			if (counter.uniqueSize > 0) c["unique_size"] = QString::number(counter.uniqueSize);
			if (counter.localTotalCount > 0) c["total"] = counter.localTotalCount;
			if (counter.totalSize > 0) c["total_size"] = QString::number(counter.totalSize);
			if (counter.messagesWithLinks > 0) c["links"] = counter.messagesWithLinks;
			
			if (!c.isEmpty()) {
				counters[TypeToString(type)] = c;
			}
		}
		return counters;
	};

	if (!typeCounters.empty()) {
		obj["type_counters"] = writeCounters(typeCounters);
	}

	if (!scanStats.empty()) {
		obj["scan_stats"] = writeCounters(scanStats);
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
			if (file.bytesDownloaded > 0) f["downloaded"] = QString::number(file.bytesDownloaded);
			if (file.totalSize > 0) f["total"] = QString::number(file.totalSize);
			if (file.messageId > 0) f["msg_id"] = QString::number(file.messageId);
			incomplete.append(f);
		}
		obj["incomplete_files"] = incomplete;
	}

	if (!visitedLinks.empty()) {
		QJsonArray links;
		for (const auto &link : visitedLinks) {
			links.append(link);
		}
		obj["visited_links"] = links;
	}

	if (settings.media.types != MediaSettings::Types(0) || settings.types != Settings::Types(0)) {
		QJsonObject s;
		s["media_types"] = MediaTypesToArray(settings.media.types);
		s["media_size_limit"] = QString::number(settings.media.sizeLimit);
		s["ext_filter_mode"] = ExtFilterModeToString(settings.media.extensionFilterMode);
		if (!settings.media.extensionFilter.isEmpty()) {
			s["ext_filter"] = settings.media.extensionFilter.join(",");
		}
		s["chat_types"] = ChatTypesToArray(settings.types);
		s["only_my_messages"] = OnlyMyMessagesToArray(settings.types, settings.fullChats);
		s["format"] = FormatToString(settings.format);
		
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
	result.scanTotalMessages = obj["scan_total_messages"].toInt();

	// Load session state fields
	result.isComplete = obj["is_complete"].toBool();
	result.lastExportDate = obj["last_export_date"].toString();

	const auto readCounters = [&](const QJsonObject &counters, std::map<int, TypeCounter> &target) {
		for (auto it = counters.begin(); it != counters.end(); ++it) {
			const int type = StringToType(it.key());
			const auto c = it.value().toObject();
			TypeCounter counter;
			counter.uniqueCount = c["unique"].toInt();
			counter.uniqueSize = c["unique_size"].toString().toLongLong();
			counter.localTotalCount = c["total"].toInt();
			counter.totalSize = c["total_size"].toString().toLongLong();
			if (c.contains("links")) {
				counter.messagesWithLinks = c["links"].toInt();
			}
			target[type] = counter;
		}
	};

	if (obj.contains("type_counters")) {
		readCounters(obj["type_counters"].toObject(), result.typeCounters);
	}

	if (obj.contains("scan_stats")) {
		readCounters(obj["scan_stats"].toObject(), result.scanStats);
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
		if (s["media_types"].isArray()) {
			result.settings.media.types = ArrayToMediaTypes(s["media_types"].toArray());
		} else {
			result.settings.media.types = static_cast<MediaSettings::Type>(s["media_types"].toInt());
		}
		result.settings.media.sizeLimit = s["media_size_limit"].toString().toLongLong();
		result.settings.media.extensionFilterMode = StringToExtFilterMode(s["ext_filter_mode"].toString());
		if (s.contains("ext_filter")) {
			result.settings.media.extensionFilter = s["ext_filter"].toString().split(",", Qt::SkipEmptyParts);
		}
		if (s.contains("chat_types")) {
			if (s["chat_types"].isArray()) {
				result.settings.types = ArrayToChatTypes(s["chat_types"].toArray());
			} else {
				result.settings.types = static_cast<Settings::Type>(s["chat_types"].toInt());
			}
		} else if (s.contains("types")) {
			result.settings.types = static_cast<Settings::Type>(s["types"].toInt());
		}
		if (s.contains("only_my_messages")) {
			result.settings.fullChats = ArrayToFullChats(result.settings.types, s["only_my_messages"].toArray());
		} else if (s.contains("full_chats")) {
			result.settings.fullChats = static_cast<Settings::Type>(s["full_chats"].toInt());
		}
		result.settings.format = StringToFormat(s["format"].toString());

		result.settings.singlePeerFrom = s["single_peer_from"].toInt();
		result.settings.singlePeerTill = s["single_peer_till"].toInt();
		
		result.settings.useIdRange = s["use_id_range"].toBool();
		result.settings.singlePeerFromId = s["single_peer_from_id"].toInt();
		result.settings.singlePeerTillId = s["single_peer_till_id"].toInt();
	}

	if (obj.contains("visited_links")) {
		const auto links = obj["visited_links"].toArray();
		result.visitedLinks.reserve(links.size());
		for (const auto &val : links) {
			result.visitedLinks.push_back(val.toString());
		}
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
