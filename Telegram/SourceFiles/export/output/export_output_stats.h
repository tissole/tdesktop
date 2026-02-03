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
	int count = 0;
	int64 size = 0;
};

class Stats {
public:
	Stats() = default;
	Stats(const Stats &other);

	void incrementFiles();
	void incrementBytes(int count);
	void incrementUserMediaFiles(); 

	void increment(MediaSettings::Type type, int64 size = 0);
	void setExpectedFilesCount(int count);

	int filesCount() const;
	int64 bytesCount() const;
	int userMediaFilesCount() const;
	int expectedFilesCount() const;

	std::map<MediaSettings::Type, StatItem> byType() const;

private:
	std::atomic<int> _files = 0;
	std::atomic<int64> _bytes = 0;
	std::atomic<int> _userMediaFiles = 0; 
	std::atomic<int> _expectedFiles = 0;

	struct TypeStat {
		std::atomic<int> count = 0;
		std::atomic<int64> size = 0;
	};
	mutable std::map<MediaSettings::Type, std::unique_ptr<TypeStat>> _stats;

	TypeStat &typeStat(MediaSettings::Type type) const;

};

} // namespace Output
} // namespace Export
