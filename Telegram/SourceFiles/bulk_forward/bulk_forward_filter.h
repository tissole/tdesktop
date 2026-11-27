/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/flags.h"
#include "data/data_types.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>

namespace BulkForward {

enum class Scope : uchar {
	All,
	DateRange,
	IdRange,
	Selection,
};

enum class ScanMethod : uchar {
	Unknown,
	History,
	Search,
};

enum class MediaType : uint32 {
	Photo = (1U << 0),
	Video = (1U << 1),
	Voice = (1U << 2),
	RoundVideo = (1U << 3),
	Sticker = (1U << 4),
	Gif = (1U << 5),
	File = (1U << 6),
	Audio = (1U << 7),
	Text = (1U << 8),
	Link = (1U << 9),
	Poll = (1U << 10),
	Other = (1U << 11),
};

inline constexpr bool is_flag_type(MediaType) {
	return true;
}

using MediaTypes = base::flags<MediaType>;

[[nodiscard]] MediaTypes AllMediaTypes();

enum class ExtensionMode : uchar {
	None,
	Whitelist,
	Blacklist,
};

struct Filter {
	Scope scope = Scope::All;
	TimeId fromDate = 0;
	TimeId tillDate = 0;
	MsgId fromId = 0;
	MsgId tillId = 0;
	MediaTypes types = AllMediaTypes();
	ExtensionMode extensionMode = ExtensionMode::None;
	QStringList extensions;
	qint64 minSize = 0;
	qint64 maxSize = 0;
	MessageIdsList selection;
};

[[nodiscard]] bool IsValid(const Filter &filter);
[[nodiscard]] bool UsesSelection(const Filter &filter);

[[nodiscard]] QString NormalizeExtension(QStringView extension);

[[nodiscard]] QByteArray Serialize(const Filter &filter);
[[nodiscard]] std::optional<Filter> Deserialize(const QByteArray &data);

} // namespace BulkForward
