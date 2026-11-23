/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "bulk_forward/bulk_forward_filter.h"

namespace BulkForward {

// The selection snapshot is never stored: it belongs to one chat and can be
// large, so a stored "Selection" scope comes back as "All messages".
[[nodiscard]] Filter LoadLastFilter();
void SaveLastFilter(const Filter &filter);

} // namespace BulkForward
