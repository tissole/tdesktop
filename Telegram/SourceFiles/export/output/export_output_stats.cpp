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
		item->count = stat->count.load();
		item->size = stat->size.load();
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

void Stats::increment(MediaSettings::Type type, int64 size) {
	auto &stat = typeStat(type);
	++stat.count;
	stat.size += size;
}

void Stats::setExpectedFilesCount(int count) {
	_expectedFiles = count;
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

std::map<MediaSettings::Type, StatItem> Stats::byType() const {
	auto result = std::map<MediaSettings::Type, StatItem>();
	for (const auto &[type, stat] : _stats) {
		result.emplace(type, StatItem{ stat->count.load(), stat->size.load() });
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
