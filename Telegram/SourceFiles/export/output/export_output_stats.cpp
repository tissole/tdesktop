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
, _expectedFiles(other._expectedFiles.load())
, _totalMessages(other._totalMessages.load()) {
	for (const auto &[type, stat] : other._stats) {
		auto item = std::make_unique<TypeStat>();
		item->uniqueCount = stat->uniqueCount.load();
		item->uniqueSize = stat->uniqueSize.load();
		item->localTotalCount = stat->localTotalCount.load();
		item->totalSize = stat->totalSize.load();
		item->messagesWithLinks = stat->messagesWithLinks.load();
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
	++stat.localTotalCount;
	stat.totalSize += size;
}

void Stats::increment(MediaSettings::Type type, int64 totalSize, int64 uniqueSize, int localTotalCount, int uniqueCount, int messagesWithLinks) {
	auto &stat = typeStat(type);
	stat.uniqueCount += uniqueCount;
	stat.uniqueSize += uniqueSize;
	stat.localTotalCount += localTotalCount;
	stat.totalSize += totalSize;
	stat.messagesWithLinks += messagesWithLinks;
}

void Stats::increment(MediaSettings::Type type, int64 size, int localTotalCount, int uniqueCount, int messagesWithLinks) {
	increment(type, size, size, localTotalCount, uniqueCount, messagesWithLinks);
}

void Stats::incrementSizeAndUnique(MediaSettings::Type type, int64 size, bool unique) {
	auto &stat = typeStat(type);
	if (unique) {
		++stat.uniqueCount;
		stat.uniqueSize += size;
	}
	++stat.localTotalCount;
	stat.totalSize += size;
}

void Stats::incrementSize(MediaSettings::Type type, int64 size) {
	auto &stat = typeStat(type);
	stat.uniqueSize += size;
	stat.totalSize += size;
}

void Stats::setLocalTotalCount(MediaSettings::Type type, int count) {
	auto &stat = typeStat(type);
	stat.localTotalCount = count;
}

void Stats::setUniqueCount(MediaSettings::Type type, int count) {
	auto &stat = typeStat(type);
	stat.uniqueCount = count;
}

void Stats::incrementUniqueCount(MediaSettings::Type type, int count) {
	auto &stat = typeStat(type);
	stat.uniqueCount += count;
}

void Stats::setMessagesWithLinks(MediaSettings::Type type, int count) {
	auto &stat = typeStat(type);
	stat.messagesWithLinks = count;
}

void Stats::setMessagesWithLinks(int count) {
	setMessagesWithLinks(MediaSettings::Type::Link, count);
}

void Stats::setExpectedFilesCount(int count) {
	_expectedFiles.store(count);
}

int Stats::expectedFilesCount() const {
	return _expectedFiles.load();
}

void Stats::incrementTotalMessages() {
	++_totalMessages;
}

void Stats::setTotalMessages(int count) {
	_totalMessages.store(count);
}

int Stats::totalMessagesCount() const {
	return _totalMessages.load();
}

void Stats::clear() {
	_files = 0;
	_bytes = 0;
	_userMediaFiles = 0;
	_expectedFiles = 0;
	_totalMessages = 0;
	_stats.clear();
}

int Stats::filesCount() const {
	return _files.load();
}

int64 Stats::bytesCount() const {
	return _bytes.load();
}

int Stats::userMediaFilesCount() const {
	return _userMediaFiles.load();
}

int Stats::localTotalCount() const {
	auto result = 0;
	for (const auto &[type, stat] : _stats) {
		result += stat->localTotalCount.load();
	}
	return result;
}

std::map<MediaSettings::Type, StatItem> Stats::byType() const {
	auto result = std::map<MediaSettings::Type, StatItem>();
	for (const auto &[type, stat] : _stats) {
		result.emplace(type, StatItem{
			stat->uniqueCount.load(),
			stat->uniqueSize.load(),
			stat->localTotalCount.load(),
			stat->totalSize.load(),
			stat->messagesWithLinks.load()
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
