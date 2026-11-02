/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "data/data_dedup_db.h"

#include <QByteArray>

#include <unordered_map>
#include <utility>
#include <vector>

namespace Data {
class DedupDb;
} // namespace Data

namespace ChatScan {

class ScanQueue final {
public:
	ScanQueue(not_null<Data::DedupDb*> db, PeerId peerId);

	void insert(const std::vector<Data::ChatScanItem> &items);
	[[nodiscard]] std::vector<Data::ChatScanItem> lease(
		int limit,
		qint64 now,
		qint64 leaseUntil);
	void release(uint64 mediaId, bool isPhoto, qint64 retryAt);
	void releaseLeases();
	void fail(uint64 mediaId, bool isPhoto);
	[[nodiscard]] int retryFailed();
	[[nodiscard]] int commitHashes(
		const std::vector<Data::ChatScanHash> &hashes);
	void resetLeases();

	void setKnownHashes(
		std::vector<Data::ChatScanMediaHash> hashes,
		bool loaded,
		bool otherLoaded);
	void rememberHash(
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash);
	void clearKnownHashes();

	[[nodiscard]] QByteArray knownHash(
		uint64 mediaId,
		bool isPhoto,
		bool dedupOn) const;
	[[nodiscard]] int pending() const;
	[[nodiscard]] int leased() const;
	[[nodiscard]] int failed() const;
	[[nodiscard]] int hashed() const;

private:
	const not_null<Data::DedupDb*> _db;
	const PeerId _peerId;
	std::unordered_map<uint64, QByteArray> _mediaHashes;
	std::unordered_map<uint64, QByteArray> _photoHashes;
	std::unordered_map<uint64, QByteArray> _otherMediaHashes;
	std::unordered_map<uint64, QByteArray> _otherPhotoHashes;
	bool _hashesLoaded = false;
	bool _otherHashesLoaded = false;

};

} // namespace ChatScan
