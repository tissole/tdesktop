/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk_forward/bulk_forward_batcher.h"

#include "data/data_forward_batching.h"

#include <iterator>

namespace BulkForward {
namespace {

constexpr auto kMaxBatch = size_t(Data::ForwardBatching::kMaxBatchIds);

[[nodiscard]] bool SameBatch(
		const SourceMessage &a,
		const SourceMessage &b) {
	return (a.id.peer == b.id.peer) && (a.isProtected == b.isProtected);
}

// First index of the group run that ends at `end`.
[[nodiscard]] size_t GroupRunStart(
		const std::vector<SourceMessage> &items,
		size_t end) {
	const auto group = items[end - 1].groupId;
	auto start = end - 1;
	while (start > 0 && items[start - 1].groupId == group) {
		--start;
	}
	return start;
}

} // namespace

void Batcher::push(std::vector<SourceMessage> items) {
	_pending.reserve(_pending.size() + items.size());
	for (auto &item : items) {
		_pending.push_back(std::move(item));
	}
	drain(false);
}

void Batcher::flush() {
	drain(true);
}

void Batcher::reset() {
	_pending.clear();
}

void Batcher::drain(bool final) {
	while (!_pending.empty()) {
		auto end = size_t(0);
		while (end < _pending.size() && end < kMaxBatch) {
			if (end > 0 && !SameBatch(_pending[end - 1], _pending[end])) {
				break;
			}
			++end;
		}
		if (end < _pending.size()
			&& _pending[end].groupId
			&& _pending[end].groupId == _pending[end - 1].groupId) {
			end = GroupRunStart(_pending, end);
		} else if (end == _pending.size()
			&& !final
			&& _pending[end - 1].groupId) {
			end = GroupRunStart(_pending, end);
		}
		if (!end) {
			return;
		}

		auto batch = Batch();
		batch.sourcePeer = _pending.front().id.peer;
		batch.isProtected = _pending.front().isProtected;
		batch.items.assign(
			std::make_move_iterator(_pending.begin()),
			std::make_move_iterator(_pending.begin() + end));
		_pending.erase(_pending.begin(), _pending.begin() + end);
		if (ready) {
			ready(std::move(batch));
		}
	}
}

} // namespace BulkForward
