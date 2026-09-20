/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "export/export_settings.h"

#include <QtCore/QByteArray>

#include <array>
#include <atomic>

namespace Export {
namespace Output {

class Stats {
public:
	Stats() = default;
	Stats(const Stats &other);

	void incrementFiles();
	void incrementBytes(int count);

	// One finished media file of the given group.
	void incrementType(MediaSettings::Type type, int64 bytes);
	// One skipped duplicate of the given group, with its size.
	void incrementSkipped(MediaSettings::Type type, int64 bytes);
	// One written media file and its bytes (totals only, pages excluded).
	void incrementMediaWritten(int64 bytes);

	void incrementTextMessage();
	void incrementLinkMessage(int links);
	void incrementLinkDuplicates(int links);
	void incrementMessage();

	int filesCount() const;
	int64 bytesCount() const;

	int64 typeFiles(MediaSettings::Type type) const;
	int64 typeBytes(MediaSettings::Type type) const;
	int64 typeSkipped(MediaSettings::Type type) const;
	int64 typeSkippedBytes(MediaSettings::Type type) const;

	int64 mediaWrittenFiles() const;
	int64 mediaWrittenBytes() const;

	int64 textMessages() const;
	int64 linkMessages() const;
	int64 linkTotal() const;
	int64 linkDuplicates() const;
	int64 messagesTotal() const;

	int64 mediaFiles() const;
	int64 mediaBytes() const;

	[[nodiscard]] QByteArray serialize() const;
	bool restore(const QByteArray &data);

	static constexpr auto kGroups = 12;

	struct GroupStat {
		MediaSettings::Type type;
		const char *key;
		const char *name;
	};
	static constexpr auto kGroupStats = std::array<GroupStat, kGroups>{ {
		{ MediaSettings::Type::Photo, "photos", "Photos" },
		{ MediaSettings::Type::Video, "videos", "Videos" },
		{ MediaSettings::Type::VoiceMessage, "voice_messages", "Voice messages" },
		{ MediaSettings::Type::VideoMessage, "video_messages", "Video messages" },
		{ MediaSettings::Type::Sticker, "stickers", "Stickers" },
		{ MediaSettings::Type::GIF, "gifs", "GIFs" },
		{ MediaSettings::Type::File, "files", "Files" },
		{ MediaSettings::Type::Text, "text_messages", "Text messages" },
		{ MediaSettings::Type::Audio, "audio", "Audio files" },
		{ MediaSettings::Type::FullHistory, "full_history", "Full history" },
		{ MediaSettings::Type::Link, "links", "Links" },
		{ MediaSettings::Type::Poll, "polls", "Polls" },
	} };

	// File-group display order matching the export settings window.
	static constexpr auto kDisplayOrder = std::array<int, 8>{
		0, 1, 8, 6, 3, 2, 5, 4,
	};

private:
	[[nodiscard]] static int groupIndex(MediaSettings::Type type);

	std::atomic<int> _files;
	std::atomic<int64> _bytes;
	std::array<std::atomic<int64>, kGroups> _groupFiles;
	std::array<std::atomic<int64>, kGroups> _groupBytes;
	std::array<std::atomic<int64>, kGroups> _groupSkipped;
	std::array<std::atomic<int64>, kGroups> _groupSkippedBytes;
	std::atomic<int64> _mediaWrittenFiles = 0;
	std::atomic<int64> _mediaWrittenBytes = 0;
	std::atomic<int64> _textMessages = 0;
	std::atomic<int64> _linkMessages = 0;
	std::atomic<int64> _linkTotal = 0;
	std::atomic<int64> _linkDuplicates = 0;
	std::atomic<int64> _messagesTotal = 0;

};

} // namespace Output
} // namespace Export