/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_dedup_db.h"

#include <QByteArray>
#include <unordered_set>
#include <utility>
#include <vector>

class DocumentData;
class PhotoData;

namespace Main {
class Session;
} // namespace Main

namespace ChatScan {

class ScanQueue;

// Owns the item leases and the in-flight fingerprints. The queue is the
// durable source of truth; the scheduler never holds more than one lease
// window and hands every completed hash back through the hooks before the
// hash is committed, so duplicate accounting never sees its own row.
class ChunkScheduler final : public base::has_weak_ptr {
public:
	struct Hooks {
		Fn<DocumentData*(uint64)> document;
		Fn<PhotoData*(uint64)> photo;
		Fn<void(const Data::ChatScanItem&, const QByteArray&)> hashed;
		Fn<void(const Data::ChatScanItem&)> failed;
		Fn<bool(const Data::ChatScanItem&)> referenceExpired;
		Fn<void(int)> committed;
		Fn<void()> update;
	};

	ChunkScheduler(
		not_null<Main::Session*> session,
		not_null<ScanQueue*> queue,
		Hooks hooks);

	void start();
	void stop();
	void pump();
	void flush();

	[[nodiscard]] int active() const;
	// Items the walker may keep pending or leased; follows the shared
	// limiter's achieved rate so the walker never outruns the scheduler.
	[[nodiscard]] int credit() const;
	[[nodiscard]] bool pumping() const;
	[[nodiscard]] bool idle() const;

private:
	void complete(
		const Data::ChatScanItem &item,
		QByteArray hash,
		uint64 generation);
	bool expired(const Data::ChatScanItem &item, uint64 generation);
	void flushCommits();

	const not_null<Main::Session*> _session;
	const not_null<ScanQueue*> _queue;
	Hooks _hooks;
	base::Timer _tickTimer;
	std::vector<std::pair<uint64, QByteArray>> _commits;
	std::unordered_set<uint64> _activeMedia;
	int _active = 0;
	uint64 _generation = 0;
	bool _stopped = true;
	bool _pumping = false;

};

} // namespace ChatScan
