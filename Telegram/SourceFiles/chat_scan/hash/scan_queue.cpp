/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_scan/hash/scan_queue.h"

namespace ChatScan {

ScanQueue::ScanQueue(not_null<Data::DedupDb*> db, PeerId peerId)
: _db(db)
, _peerId(peerId) {
}

void ScanQueue::insert(const std::vector<Data::ChatScanItem> &items) {
	_db->writeChatScanItems(items);
}

std::vector<Data::ChatScanItem> ScanQueue::lease(
		int limit,
		qint64 now,
		qint64 leaseUntil) {
	return _db->chatScanLeaseItems(_peerId, limit, now, leaseUntil);
}

void ScanQueue::release(uint64 mediaId, qint64 retryAt) {
	_db->chatScanReleaseItem(_peerId, mediaId, retryAt);
}

void ScanQueue::fail(uint64 mediaId) {
	_db->chatScanFailItem(_peerId, mediaId);
}

int ScanQueue::retryFailed() {
	return _db->chatScanRetryFailed(_peerId);
}

int ScanQueue::commitHashes(
		const std::vector<std::pair<uint64, QByteArray>> &hashes) {
	return _db->chatScanCommitHashes(_peerId, hashes);
}

void ScanQueue::resetLeases() {
	_db->chatScanResetLeases(_peerId);
}

QByteArray ScanQueue::knownHash(uint64 mediaId, bool isPhoto) const {
	auto known = _db->hashForDocId(Data::DedupDb::Table::Uploads, mediaId);
	if (known.isEmpty()) {
		known = _db->chatScanHashForMedia(_peerId, mediaId, isPhoto);
	}
	return known;
}

int ScanQueue::pending() const {
	return _db->chatScanPendingCount(_peerId);
}

int ScanQueue::leased() const {
	return _db->chatScanLeasedCount(_peerId);
}

int ScanQueue::failed() const {
	return _db->chatScanFailedCount(_peerId);
}

int ScanQueue::hashed() const {
	return _db->chatScanHashedCount(_peerId);
}

} // namespace ChatScan
