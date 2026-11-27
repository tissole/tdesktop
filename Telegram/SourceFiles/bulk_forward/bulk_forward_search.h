/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "bulk_forward/bulk_forward_filter.h"
#include "scheme.h"

#include <optional>
#include <vector>

namespace BulkForward {

// The server indexes media messages in a few separate lists. A selection that
// maps onto exactly one of them can be walked with messages.search instead of
// scanning the whole history.
enum class SearchPiece : int {
	PhotoVideo,
	Photo,
	Video,
	Voice,
	Round,
	Gif,
	Document,
	Music,
	Url,
	Poll,
	Contacts,
	Geo,
};

[[nodiscard]] std::vector<SearchPiece> AllSearchPieces();
[[nodiscard]] std::vector<SearchPiece> SelectedSearchPieces(MediaTypes types);
[[nodiscard]] MTPMessagesFilter SearchFilterFor(SearchPiece piece);
[[nodiscard]] std::optional<SearchPiece> SingleSearchPiece(MediaTypes types);

} // namespace BulkForward
