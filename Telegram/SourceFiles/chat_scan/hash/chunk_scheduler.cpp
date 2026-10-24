/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_scan/hash/chunk_scheduler.h"

#include "chat_scan/hash/scan_queue.h"
#include "data/data_file_hash.h"
#include "data/data_session.h"
#include "data/file_chunk_fetcher.h"
#include "main/main_session.h"

#include "base/timer.h"

#include <algorithm>

namespace ChatScan {
namespace {

// Items, not chunks: each item may need one whole-file read or two 16 KB
// samples, and FileChunkFetcher owns the chunk pacing. This pool only keeps
// enough leased work queued that the limiter always has a next request.
constexpr auto kSchedulerItems = 192;
constexpr auto kCommitBatch = 16;
constexpr auto kLeaseMs = int64(30 * 1000);
constexpr auto kUnresolvedRetryMs = int64(2 * 1000);
constexpr auto kReferenceRetryMs = int64(5 * 1000);
constexpr auto kCreditRateFactor = 4.;
constexpr auto kMinCredit = 128;
constexpr auto kMaxCredit = 2048;

} // namespace

ChunkScheduler::ChunkScheduler(
	not_null<Main::Session*> session,
	not_null<ScanQueue*> queue,
	Hooks hooks)
: _session(session)
, _queue(queue)
, _hooks(std::move(hooks)) {
}

void ChunkScheduler::start() {
	flush();
	for (const auto mediaId : _activeMedia) {
		_queue->release(mediaId, 0);
	}
	_activeMedia.clear();
	_active = 0;
	++_generation;
	_stopped = false;
	_queue->resetLeases();
	// Retries are time based, and a retry can be the only thing left to do,
	// so a periodic tick both wakes the pump and lets the owner flush its
	// final partially filled commit batch.
	_tickTimer.setCallback([=] {
		pump();
		if (_hooks.update) {
			_hooks.update();
		}
	});
	_tickTimer.callEach(1000);
	pump();
}

void ChunkScheduler::stop() {
	if (_stopped) {
		return;
	}
	_stopped = true;
	++_generation;
	_tickTimer.cancel();
	flush();
	for (const auto mediaId : _activeMedia) {
		_queue->release(mediaId, 0);
	}
	_activeMedia.clear();
	_active = 0;
}

void ChunkScheduler::pump() {
	if (_stopped || _pumping) {
		return;
	}
	_pumping = true;
	const auto generation = _generation;
	const auto free = kSchedulerItems - _active;
	if (free > 0) {
		const auto now = int64(crl::now());
		auto items = _queue->lease(free, now, now + kLeaseMs);
		for (auto &item : items) {
			if (_stopped) {
				_queue->release(item.mediaId, 0);
				continue;
			}
			const auto document = _hooks.document
				? _hooks.document(item.mediaId)
				: nullptr;
			const auto photo = _hooks.photo
				? _hooks.photo(item.mediaId)
				: nullptr;
			if (!document && !photo) {
				if (item.attempts >= 4) {
					complete(item, QByteArray(), generation);
				} else {
					_queue->release(item.mediaId, now + kUnresolvedRetryMs);
				}
				continue;
			}
			const auto known = _queue->knownHash(item.mediaId, item.isPhoto);
			if (!known.isEmpty()) {
				complete(item, known, generation);
				continue;
			}
			++_active;
			_activeMedia.insert(item.mediaId);
			const auto weak = base::weak_ptr<ChunkScheduler>(this);
			const auto done = [weak, item, generation](QByteArray hash) mutable {
				if (const auto strong = weak.get()) {
					strong->complete(item, std::move(hash), generation);
				}
			};
			const auto expired = [weak, item, generation] {
				const auto strong = weak.get();
				return strong && strong->expired(item, generation);
			};
			if (photo) {
				Data::RemotePhotoFingerprint(
					_session,
					not_null<PhotoData*>(photo),
					done,
					0,
					expired,
					Data::kDedupChunkDcShift);
			} else {
				Data::RemoteFileFingerprint(
					_session,
					not_null<DocumentData*>(document),
					done,
					0,
					expired,
					Data::kDedupChunkDcShift);
			}
		}
	}
	_pumping = false;
}

void ChunkScheduler::flush() {
	flushCommits();
}

void ChunkScheduler::complete(
		const Data::ChatScanItem &item,
		QByteArray hash,
		uint64 generation) {
	if (generation != _generation) {
		return;
	}
	const auto wasActive = _activeMedia.erase(item.mediaId) != 0;
	if (wasActive && _active > 0) {
		--_active;
	}
	if (_stopped) {
		_queue->release(item.mediaId, 0);
		return;
	}
	if (hash.isEmpty()) {
		_queue->fail(item.mediaId);
		if (_hooks.failed) {
			_hooks.failed(item);
		}
	} else {
		if (_hooks.hashed) {
			_hooks.hashed(item, hash);
		}
		_commits.emplace_back(item.mediaId, std::move(hash));
	}
	if (_commits.size() >= kCommitBatch) {
		flushCommits();
	}
	if (_hooks.update) {
		_hooks.update();
	}
	pump();
}

bool ChunkScheduler::expired(
		const Data::ChatScanItem &item,
		uint64 generation) {
	if (generation != _generation || _stopped || !_hooks.referenceExpired) {
		return false;
	}
	if (!_hooks.referenceExpired(item)) {
		return false;
	}
	const auto wasActive = _activeMedia.erase(item.mediaId) != 0;
	if (wasActive && _active > 0) {
		--_active;
	}
	_queue->release(item.mediaId, int64(crl::now()) + kReferenceRetryMs);
	if (_hooks.update) {
		_hooks.update();
	}
	pump();
	return true;
}

void ChunkScheduler::flushCommits() {
	if (_commits.empty()) {
		return;
	}
	const auto resolved = _queue->commitHashes(_commits);
	_commits.clear();
	if (_hooks.committed) {
		_hooks.committed(resolved);
	}
}

int ChunkScheduler::active() const {
	return _active;
}

int ChunkScheduler::credit() const {
	const auto rate = _session->data().chunkFetcher().achievedRate();
	return std::clamp(
		int(rate * kCreditRateFactor),
		kMinCredit,
		kMaxCredit);
}

bool ChunkScheduler::pumping() const {
	return _pumping;
}

bool ChunkScheduler::idle() const {
	return !_active && _commits.empty();
}

} // namespace ChatScan
