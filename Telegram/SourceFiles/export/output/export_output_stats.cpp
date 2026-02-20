/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/output/export_output_stats.h"

namespace Export {
namespace Output {

Stats::Stats(const Stats &other)
: _files(other._files.load())
, _bytes(other._bytes.load())
, _userMediaFiles(other._userMediaFiles.load())
, _expectedFiles(other._expectedFiles.load()) {
	for (const auto &[type, stat] : other._stats) {
		auto item = std::make_unique<TypeStat>();
		item->uniqueCount = stat->uniqueCount.load();
		item->uniqueSize = stat->uniqueSize.load();
		item->totalCount = stat->totalCount.load();
		item->totalSize = stat->totalSize.load();
		_stats.emplace(type, std::move(item));
	}
}

void Stats::incrementFiles() {
	++_files;
}

void Stats::incrementBytes(int count) {
	_bytes += count;
}

void Stats::incrementUserMediaFiles() {
	++_userMediaFiles;
}

void Stats::increment(MediaSettings::Type type, int64 size, bool unique) {
	auto &stat = typeStat(type);
	if (unique) {
		++stat.uniqueCount;
		stat.uniqueSize += size;
	}
	++stat.totalCount;
	stat.totalSize += size;
}

void Stats::increment(MediaSettings::Type type, int64 size, int totalCount, int uniqueCount) {
	auto &stat = typeStat(type);
	stat.uniqueCount += uniqueCount;
	stat.uniqueSize += (uniqueCount > 0) ? size : 0;
	stat.totalCount += totalCount;
	stat.totalSize += size;
}

void Stats::incrementSizeAndUnique(MediaSettings::Type type, int64 size, bool unique) {
	auto &stat = typeStat(type);
	if (unique) {
		++stat.uniqueCount;
		stat.uniqueSize += size;
	}
	++stat.totalCount;
	stat.totalSize += size;
}

void Stats::incrementSize(MediaSettings::Type type, int64 size) {
	auto &stat = typeStat(type);
	stat.uniqueSize += size;
	stat.totalSize += size;
}

void Stats::setTotalCount(MediaSettings::Type type, int count) {
	auto &stat = typeStat(type);
	stat.totalCount = count;
}

void Stats::setExpectedFilesCount(int count) {
	_expectedFiles = count;
}

void Stats::clear() {
	_files = 0;
	_bytes = 0;
	_userMediaFiles = 0;
	_expectedFiles = 0;
	_stats.clear();
}

int Stats::filesCount() const {
	return _files;
}

int64 Stats::bytesCount() const {
	return _bytes;
}

int Stats::userMediaFilesCount() const {
	return _userMediaFiles;
}

int Stats::expectedFilesCount() const {
	return _expectedFiles;
}

int Stats::totalCount() const {
	auto result = 0;
	for (const auto &[type, stat] : _stats) {
		result += stat->totalCount.load();
	}
	return result;
}

std::map<MediaSettings::Type, StatItem> Stats::byType() const {
	auto result = std::map<MediaSettings::Type, StatItem>();
	for (const auto &[type, stat] : _stats) {
		result.emplace(type, StatItem{
			stat->uniqueCount.load(),
			stat->uniqueSize.load(),
			stat->totalCount.load(),
			stat->totalSize.load()
		});
	}
	return result;
}

Stats::TypeStat &Stats::typeStat(MediaSettings::Type type) const {
	auto it = _stats.find(type);
	if (it == _stats.end()) {
		it = _stats.emplace(type, std::make_unique<TypeStat>()).first;
	}
	return *it->second;
}

} // namespace Output
} // namespace Export