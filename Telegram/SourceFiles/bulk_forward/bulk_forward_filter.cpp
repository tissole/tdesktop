/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk_forward/bulk_forward_filter.h"

#include <QDataStream>
#include <QIODevice>

namespace BulkForward {
namespace {

constexpr auto kSerializationVersion = quint32(1);
constexpr auto kMaxSelectionCount = quint32(1000000);

} // namespace

MediaTypes AllMediaTypes() {
	return MediaType::Photo
		| MediaType::Video
		| MediaType::Voice
		| MediaType::RoundVideo
		| MediaType::Sticker
		| MediaType::Gif
		| MediaType::File
		| MediaType::Audio
		| MediaType::Text
		| MediaType::Link
		| MediaType::Poll
		| MediaType::Other;
}

bool IsValid(const Filter &filter) {
	if (!filter.types.value()) {
		return false;
	}
	if (filter.minSize < 0 || filter.maxSize < 0) {
		return false;
	}
	if (filter.maxSize > 0 && filter.minSize > filter.maxSize) {
		return false;
	}
	switch (filter.scope) {
	case Scope::All:
		return true;
	case Scope::DateRange:
		return !filter.fromDate
			|| !filter.tillDate
			|| (filter.fromDate <= filter.tillDate);
	case Scope::IdRange:
		return !filter.fromId
			|| !filter.tillId
			|| (filter.fromId <= filter.tillId);
	case Scope::Selection:
		return !filter.selection.empty();
	}
	return false;
}

bool UsesSelection(const Filter &filter) {
	return (filter.scope == Scope::Selection);
}

QString NormalizeExtension(QStringView extension) {
	auto result = extension.trimmed().toString().toLower();
	while (result.startsWith(u'.')) {
		result.remove(0, 1);
	}
	return result;
}

QByteArray Serialize(const Filter &filter) {
	auto data = QByteArray();
	auto stream = QDataStream(&data, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_6_0);
	stream << kSerializationVersion;
	stream << quint8(filter.scope);
	stream << qint32(filter.fromDate);
	stream << qint32(filter.tillDate);
	stream << qint64(filter.fromId.bare);
	stream << qint64(filter.tillId.bare);
	stream << quint32(filter.types.value());
	stream << quint8(filter.extensionMode);
	stream << filter.extensions;
	stream << qint64(filter.minSize);
	stream << qint64(filter.maxSize);
	stream << quint32(filter.selection.size());
	for (const auto &id : filter.selection) {
		stream << quint64(id.peer.value);
		stream << qint64(id.msg.bare);
	}
	return data;
}

std::optional<Filter> Deserialize(const QByteArray &data) {
	auto stream = QDataStream(data);
	stream.setVersion(QDataStream::Qt_6_0);

	auto version = quint32(0);
	stream >> version;
	if (version != kSerializationVersion) {
		return std::nullopt;
	}

	auto scope = quint8(0);
	stream >> scope;
	if (scope > quint8(Scope::Selection)) {
		return std::nullopt;
	}

	auto result = Filter();
	result.scope = Scope(scope);

	auto fromDate = qint32(0);
	auto tillDate = qint32(0);
	stream >> fromDate;
	stream >> tillDate;
	if (fromDate < 0 || tillDate < 0) {
		return std::nullopt;
	}
	result.fromDate = TimeId(fromDate);
	result.tillDate = TimeId(tillDate);

	auto fromId = qint64(0);
	auto tillId = qint64(0);
	stream >> fromId;
	stream >> tillId;
	if (fromId < 0 || tillId < 0) {
		return std::nullopt;
	}
	result.fromId = MsgId(fromId);
	result.tillId = MsgId(tillId);

	auto types = quint32(0);
	stream >> types;
	if (types & ~AllMediaTypes().value()) {
		return std::nullopt;
	}
	result.types = MediaTypes::from_raw(types);

	auto extensionMode = quint8(0);
	stream >> extensionMode;
	if (extensionMode > quint8(ExtensionMode::Blacklist)) {
		return std::nullopt;
	}
	result.extensionMode = ExtensionMode(extensionMode);

	stream >> result.extensions;

	auto minSize = qint64(0);
	auto maxSize = qint64(0);
	stream >> minSize;
	stream >> maxSize;
	if (minSize < 0 || maxSize < 0) {
		return std::nullopt;
	}
	result.minSize = minSize;
	result.maxSize = maxSize;

	auto count = quint32(0);
	stream >> count;
	if (count > kMaxSelectionCount) {
		return std::nullopt;
	}
	result.selection.reserve(count);
	for (auto i = quint32(0); i != count; ++i) {
		auto peer = quint64(0);
		auto msg = qint64(0);
		stream >> peer;
		stream >> msg;
		if (msg <= 0) {
			return std::nullopt;
		}
		result.selection.emplace_back(PeerId(peer), MsgId(msg));
	}

	if (stream.status() != QDataStream::Ok) {
		return std::nullopt;
	}
	return result;
}

} // namespace BulkForward
