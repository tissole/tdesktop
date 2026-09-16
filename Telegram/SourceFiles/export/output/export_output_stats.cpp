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
, _bytes(other._bytes.load()) {
	for (auto i = 0; i != kGroups; ++i) {
		_groupFiles[i] = other._groupFiles[i].load();
		_groupBytes[i] = other._groupBytes[i].load();
		_groupSkipped[i] = other._groupSkipped[i].load();
		_groupSkippedBytes[i] = other._groupSkippedBytes[i].load();
	}
	_mediaWrittenFiles = other._mediaWrittenFiles.load();
	_mediaWrittenBytes = other._mediaWrittenBytes.load();
	_textMessages = other._textMessages.load();
	_linkMessages = other._linkMessages.load();
	_linkTotal = other._linkTotal.load();
	_linkDuplicates = other._linkDuplicates.load();
	_messagesTotal = other._messagesTotal.load();
}

void Stats::incrementFiles() {
	++_files;
}

void Stats::incrementBytes(int count) {
	_bytes += count;
}

int Stats::groupIndex(MediaSettings::Type type) {
	switch (type) {
	case MediaSettings::Type::Photo: return 0;
	case MediaSettings::Type::Video: return 1;
	case MediaSettings::Type::VoiceMessage: return 2;
	case MediaSettings::Type::VideoMessage: return 3;
	case MediaSettings::Type::Sticker: return 4;
	case MediaSettings::Type::GIF: return 5;
	case MediaSettings::Type::File: return 6;
	case MediaSettings::Type::Text: return 7;
	case MediaSettings::Type::Audio: return 8;
	case MediaSettings::Type::FullHistory: return 9;
	case MediaSettings::Type::Link: return 10;
	case MediaSettings::Type::Poll: return 11;
	default: return -1;
	}
	return -1;
}

void Stats::incrementType(MediaSettings::Type type, int64 bytes) {
	const auto i = groupIndex(type);
	if (i < 0) {
		return;
	}
	++_groupFiles[i];
	_groupBytes[i] += bytes;
}

void Stats::incrementSkipped(MediaSettings::Type type, int64 bytes) {
	const auto i = groupIndex(type);
	if (i < 0) {
		return;
	}
	++_groupSkipped[i];
	_groupSkippedBytes[i] += bytes;
}

void Stats::incrementMediaWritten(int64 bytes) {
	++_mediaWrittenFiles;
	_mediaWrittenBytes += bytes;
}

void Stats::incrementTextMessage() {
	++_textMessages;
}

void Stats::incrementLinkMessage(int links) {
	++_linkMessages;
	_linkTotal += links;
}

void Stats::incrementLinkDuplicates(int links) {
	_linkDuplicates += links;
}

void Stats::incrementMessage() {
	++_messagesTotal;
}

int Stats::filesCount() const {
	return _files;
}

int64 Stats::bytesCount() const {
	return _bytes;
}

int64 Stats::typeFiles(MediaSettings::Type type) const {
	const auto i = groupIndex(type);
	return (i < 0) ? 0 : _groupFiles[i].load();
}

int64 Stats::typeBytes(MediaSettings::Type type) const {
	const auto i = groupIndex(type);
	return (i < 0) ? 0 : _groupBytes[i].load();
}

int64 Stats::typeSkipped(MediaSettings::Type type) const {
	const auto i = groupIndex(type);
	return (i < 0) ? 0 : _groupSkipped[i].load();
}

int64 Stats::typeSkippedBytes(MediaSettings::Type type) const {
	const auto i = groupIndex(type);
	return (i < 0) ? 0 : _groupSkippedBytes[i].load();
}

int64 Stats::mediaWrittenFiles() const {
	return _mediaWrittenFiles.load();
}

int64 Stats::mediaWrittenBytes() const {
	return _mediaWrittenBytes.load();
}

int64 Stats::textMessages() const {
	return _textMessages.load();
}

int64 Stats::linkMessages() const {
	return _linkMessages.load();
}

int64 Stats::linkTotal() const {
	return _linkTotal.load();
}

int64 Stats::linkDuplicates() const {
	return _linkDuplicates.load();
}

int64 Stats::messagesTotal() const {
	return _messagesTotal.load();
}

int64 Stats::mediaFiles() const {
	auto result = int64(0);
	for (const auto &count : _groupFiles) {
		result += count.load();
	}
	return result;
}

int64 Stats::mediaBytes() const {
	auto result = int64(0);
	for (const auto &bytes : _groupBytes) {
		result += bytes.load();
	}
	return result;
}

} // namespace Output
} // namespace Export