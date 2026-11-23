/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "data/data_types.h"

#include <cstddef>

namespace Data {

class Media;

} // namespace Data

namespace Data::ForwardBatching {

// One request carries at most this many ids, and one regrouped album at
// most this many members; the server rejects larger requests and longer
// albums.
inline constexpr auto kMaxBatchIds = 100;
inline constexpr auto kMaxAlbumIds = 10;

enum class AlbumKind : uint8 {
	NotGroupable,
	AlbumsPhoto,
	AlbumsVideo,
	AlbumsMusic,
	AlbumsDocuments,
};

[[nodiscard]] AlbumKind KindOf(const Media *media);

// Number of ids that may travel in one request in the given grouping mode.
[[nodiscard]] int MaxBatchSize(GroupingOptions options);

// Whether the item at `index` may join the request that already holds the
// preceding items. `index` is its position inside that request, so the
// decision compares `index - 1` and `index`. Kinds are requested lazily,
// because a boundary between two albums or between two singles is decided
// by the group ids alone.
[[nodiscard]] bool ContinuesBatch(
	GroupingOptions options,
	ForwardOptions forwardOptions,
	std::size_t index,
	MessageGroupId previous,
	MessageGroupId current,
	Fn<AlbumKind(std::size_t)> kindAt);

} // namespace Data::ForwardBatching
