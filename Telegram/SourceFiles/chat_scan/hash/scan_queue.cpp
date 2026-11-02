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

void ScanQueue::release(uint64 mediaId, bool isPhoto, qint64 retryAt) {
	_db->chatScanReleaseItem(_peerId, mediaId, isPhoto, retryAt);
}

void ScanQueue::releaseLeases() {
	_db->chatScanReleaseLeases(_peerId);
}

void ScanQueue::fail(uint64 mediaId, bool isPhoto) {
	_db->chatScanFailItem(_peerId, mediaId, isPhoto);
}

int ScanQueue::retryFailed() {
	return _db->chatScanRetryFailed(_peerId);
}

int ScanQueue::commitHashes(
		const std::vector<Data::ChatScanHash> &hashes) {
	return _db->chatScanCommitHashes(_peerId, hashes);
}

void ScanQueue::resetLeases() {
	_db->chatScanResetLeases(_peerId);
}

void ScanQueue::setKnownHashes(
		std::vector<Data::ChatScanMediaHash> hashes,
		bool loaded,
		bool otherLoaded) {
	_hashesLoaded = loaded;
	_otherHashesLoaded = otherLoaded;
	for (auto &row : hashes) {
		if (!row.mediaId || row.hash.isEmpty()) {
			continue;
		}
		const auto current = (row.peerId == _peerId);
		auto &map = row.isPhoto
			? (current ? _photoHashes : _otherPhotoHashes)
			: (current ? _mediaHashes : _otherMediaHashes);
		if (map.find(row.mediaId) == map.end()) {
			map.emplace(row.mediaId, std::move(row.hash));
		}
	}
}

void ScanQueue::rememberHash(
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash) {
	if (!mediaId || hash.isEmpty()) {
		return;
	}
	(isPhoto ? _photoHashes : _mediaHashes)[mediaId] = hash;
}

void ScanQueue::clearKnownHashes() {
	_mediaHashes.clear();
	_photoHashes.clear();
	_otherMediaHashes.clear();
	_otherPhotoHashes.clear();
	_hashesLoaded = false;
	_otherHashesLoaded = false;
}

QByteArray ScanQueue::knownHash(
		uint64 mediaId,
		bool isPhoto,
		bool dedupOn) const {
	if (!mediaId) {
		return QByteArray();
	}
	const auto &hashes = isPhoto ? _photoHashes : _mediaHashes;
	if (const auto i = hashes.find(mediaId); i != hashes.end()) {
		return i->second;
	}
	auto known = dedupOn
		? _db->hashForDocId(
			Data::DedupDb::Table::Uploads,
			mediaId,
			isPhoto)
		: QByteArray();
	if (!known.isEmpty()) {
		return known;
	}
	if (dedupOn) {
		if (_otherHashesLoaded) {
			const auto &other = isPhoto
				? _otherPhotoHashes
				: _otherMediaHashes;
			if (const auto i = other.find(mediaId); i != other.end()) {
				return i->second;
			}
		} else {
			known = _db->chatScanHashForMediaOther(
				_peerId,
				mediaId,
				isPhoto);
			if (!known.isEmpty()) {
				return known;
			}
		}
	}
	if (_hashesLoaded) {
		return QByteArray();
	}
	return _db->chatScanHashForMedia(_peerId, mediaId, isPhoto);
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
