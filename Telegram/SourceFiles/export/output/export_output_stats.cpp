/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/output/export_output_stats.h"

#include <QtCore/QDataStream>

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

QByteArray Stats::serialize() const {
	auto result = QByteArray();
	auto stream = QDataStream(&result, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream << int64(_files.load()) << _bytes.load();
	for (auto i = 0; i != kGroups; ++i) {
		stream
			<< _groupFiles[i].load()
			<< _groupBytes[i].load()
			<< _groupSkipped[i].load()
			<< _groupSkippedBytes[i].load();
	}
	stream
		<< _mediaWrittenFiles.load()
		<< _mediaWrittenBytes.load()
		<< _textMessages.load()
		<< _linkMessages.load()
		<< _linkTotal.load()
		<< _linkDuplicates.load()
		<< _messagesTotal.load();
	return result;
}

bool Stats::restore(const QByteArray &data) {
	auto stream = QDataStream(data);
	stream.setVersion(QDataStream::Qt_5_1);
	auto files = int64(0);
	auto bytes = int64(0);
	stream >> files >> bytes;
	if (stream.status() != QDataStream::Ok) {
		return false;
	}
	auto groupFiles = std::array<int64, kGroups>{};
	auto groupBytes = std::array<int64, kGroups>{};
	auto groupSkipped = std::array<int64, kGroups>{};
	auto groupSkippedBytes = std::array<int64, kGroups>{};
	for (auto i = 0; i != kGroups; ++i) {
		stream
			>> groupFiles[i]
			>> groupBytes[i]
			>> groupSkipped[i]
			>> groupSkippedBytes[i];
	}
	auto mediaWrittenFiles = int64(0);
	auto mediaWrittenBytes = int64(0);
	auto textMessages = int64(0);
	auto linkMessages = int64(0);
	auto linkTotal = int64(0);
	auto linkDuplicates = int64(0);
	auto messagesTotal = int64(0);
	stream
		>> mediaWrittenFiles
		>> mediaWrittenBytes
		>> textMessages
		>> linkMessages
		>> linkTotal
		>> linkDuplicates
		>> messagesTotal;
	if (stream.status() != QDataStream::Ok || !stream.atEnd()) {
		return false;
	}
	_files = int(files);
	_bytes = bytes;
	for (auto i = 0; i != kGroups; ++i) {
		_groupFiles[i] = groupFiles[i];
		_groupBytes[i] = groupBytes[i];
		_groupSkipped[i] = groupSkipped[i];
		_groupSkippedBytes[i] = groupSkippedBytes[i];
	}
	_mediaWrittenFiles = mediaWrittenFiles;
	_mediaWrittenBytes = mediaWrittenBytes;
	_textMessages = textMessages;
	_linkMessages = linkMessages;
	_linkTotal = linkTotal;
	_linkDuplicates = linkDuplicates;
	_messagesTotal = messagesTotal;
	return true;
}

} // namespace Output
} // namespace Export