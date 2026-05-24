/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <atomic>
#include <map>
#include "export/export_settings.h"

namespace Export {
namespace Output {

struct StatItem {
	int uniqueCount = 0;
	int64 uniqueSize = 0;
	int totalCount = 0;
	int64 totalSize = 0;
	int messagesWithLinks = 0; // only meaningful for Link type
};

class Stats {
public:
	Stats() = default;
	Stats(const Stats &other);

	void incrementFiles();
	void incrementBytes(int count);
	void incrementUserMediaFiles();

	void increment(MediaSettings::Type type, int64 size, bool unique);
	void increment(MediaSettings::Type type, int64 totalSize, int64 uniqueSize, int totalCount, int uniqueCount, int messagesWithLinks = 0);
	void increment(MediaSettings::Type type, int64 size, int totalCount, int uniqueCount, int messagesWithLinks = 0);
	void incrementSizeAndUnique(MediaSettings::Type type, int64 size, bool unique);
	void incrementSize(MediaSettings::Type type, int64 size);
	void setLocalTotalCount(MediaSettings::Type type, int count);
	void setUniqueCount(MediaSettings::Type type, int count);
	void incrementUniqueCount(MediaSettings::Type type, int count);
	void setMessagesWithLinks(MediaSettings::Type type, int count);
	void setMessagesWithLinks(int count);
	void setExpectedFilesCount(int count);
	int expectedFilesCount() const;
	void incrementTotalMessages();
	void setTotalMessages(int count);
	int totalMessagesCount() const;
	void clear();

	int filesCount() const;
	int64 bytesCount() const;
	int userMediaFilesCount() const;
	int totalCount() const;

	std::map<MediaSettings::Type, StatItem> byType() const;

private:
	std::atomic<int> _files = 0;
	std::atomic<int64> _bytes = 0;
	std::atomic<int> _userMediaFiles = 0;
	std::atomic<int> _expectedFiles = 0;
	std::atomic<int> _totalMessages = 0;

	struct TypeStat {
		std::atomic<int> uniqueCount = 0;
		std::atomic<int64> uniqueSize = 0;
		std::atomic<int> totalCount = 0;
		std::atomic<int64> totalSize = 0;
		std::atomic<int> messagesWithLinks = 0;
	};
	mutable std::map<MediaSettings::Type, std::unique_ptr<TypeStat>> _stats;

	TypeStat &typeStat(MediaSettings::Type type) const;

};

} // namespace Output
} // namespace Export