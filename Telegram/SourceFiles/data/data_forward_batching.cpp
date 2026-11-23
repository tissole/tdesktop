/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_forward_batching.h"

#include "data/data_document.h"
#include "data/data_media_types.h"

namespace Data::ForwardBatching {
namespace {

[[nodiscard]] bool GroupedForRegroup(
		ForwardOptions forwardOptions,
		AlbumKind kind) {
	if (kind == AlbumKind::NotGroupable) {
		return false;
	}
	if (forwardOptions != ForwardOptions::PreserveInfo) {
		return true;
	}
	return kind == AlbumKind::AlbumsMusic
		|| kind == AlbumKind::AlbumsDocuments;
}

} // namespace

AlbumKind KindOf(const Media *media) {
	if (!media) {
		return AlbumKind::NotGroupable;
	}
	if (media->photo()) {
		return AlbumKind::AlbumsPhoto;
	}
	if (const auto document = media->document()) {
		if (document->isVideoFile()) {
			return AlbumKind::AlbumsVideo;
		}
		if (document->isSong()) {
			return AlbumKind::AlbumsMusic;
		}
		if (media->canBeGrouped()) {
			return AlbumKind::AlbumsDocuments;
		}
	}
	return AlbumKind::NotGroupable;
}

int MaxBatchSize(GroupingOptions options) {
	if (options == GroupingOptions::Separate) {
		return 1;
	}
	if (options == GroupingOptions::RegroupAll) {
		return kMaxAlbumIds;
	}
	return kMaxBatchIds;
}

bool ContinuesBatch(
		GroupingOptions options,
		ForwardOptions forwardOptions,
		std::size_t index,
		MessageGroupId previous,
		MessageGroupId current,
		Fn<AlbumKind(std::size_t)> kindAt) {
	if (!index) {
		return true;
	}
	if (options == GroupingOptions::GroupAsIs) {
		// Preserve source grouping: only members of the same album travel
		// in one request, anything else (including two singles) goes
		// separately. Batched media would otherwise arrive grouped as a
		// new album.
		return previous != MessageGroupId()
			&& current != MessageGroupId()
			&& previous == current;
	} else if (options != GroupingOptions::RegroupAll) {
		return false;
	}
	const auto sameAlbum = previous != MessageGroupId()
		&& current != MessageGroupId()
		&& previous == current;
	if (sameAlbum) {
		return index < std::size_t(kMaxAlbumIds);
	}
	const auto boundary = (previous != MessageGroupId())
		? (current != previous)
		: (current != MessageGroupId());
	if (boundary) {
		return false;
	}
	const auto before = kindAt(index - 1);
	const auto after = kindAt(index);
	return GroupedForRegroup(forwardOptions, before)
		&& GroupedForRegroup(forwardOptions, after)
		&& before == after;
}

} // namespace Data::ForwardBatching
