/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_scan/chat_scan_api.h"

#include "chat_scan/chat_scan_index.h"
#include "chat_scan/hash/chunk_scheduler.h"
#include "chat_scan/hash/scan_queue.h"
#include "logs.h"

#include "apiwrap.h"
#include "core/application.h"
#include "core/enhanced_settings.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_dedup_db.h"
#include "data/data_document.h"
#include "data/data_download_manager.h"
#include "data/data_file_hash.h"
#include "data/data_media_category.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/file_chunk_fetcher.h"
#include "export/export_settings.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace ChatScan {
namespace {

constexpr auto kPageLimit = 100;
constexpr auto kRestoreWindow = 500;
constexpr auto kRecoverySpacingMs = 250;
// History pages are paced per chat and jittered; chunks are paced per DC and
// never wait on this. The walk ahead is bounded by the scheduler's credit.
constexpr auto kPageSpacingMs = 250;
constexpr auto kPageJitterMs = 260;
constexpr auto kStarvationDelayMs = crl::time(5000);
constexpr auto kStarvationLogMs = crl::time(10000);
constexpr auto kRecoveryBatchErrorCeil = 3;
constexpr auto kScanTakeoutMaxFileBytes = 4000LL * 1024 * 1024;
[[nodiscard]] crl::time RecoveryDelay(int32 above, int pos) {
	return crl::time(kRecoverySpacingMs)
		+ crl::time((uint64(above) * 13 + uint64(pos) * 7) % kPageJitterMs);
}

[[nodiscard]] int ScanMediaCategory(
		not_null<DocumentData*> document,
		bool wrapperVideo) {
	using Type = Export::MediaSettings::Type;
	const auto category = DocumentMediaCategory(document);
	return (category == Type::File && wrapperVideo)
		? int(Type::Video)
		: int(category);
}

} // namespace

Api::Api(not_null<Main::Session*> session, not_null<PeerData*> peer)
: _session(session)
, _peer(peer)
, _api(&session->mtp()) {
	_queue = std::make_unique<ScanQueue>(&db(), peer->id);
	auto hooks = ChunkScheduler::Hooks();
	hooks.document = [this](uint64 mediaId) {
		const auto i = _documents.find(mediaId);
		return (i != _documents.end()) ? i->second : nullptr;
	};
	hooks.photo = [this](uint64 mediaId) {
		const auto i = _photos.find(mediaId);
		return (i != _photos.end()) ? i->second : nullptr;
	};
	hooks.hashed = [this](
			const Data::ChatScanItem &item,
			const QByteArray &hash) {
		onHashed(item, hash);
	};
	hooks.failed = [this](const Data::ChatScanItem &item) {
		onHashFailed(item);
	};
	hooks.referenceExpired = [this](const Data::ChatScanItem &item) {
		return onHashReferenceExpired(item);
	};
	hooks.committed = [this](int resolved) {
		onHashCommitted(resolved);
	};
	hooks.update = [this] {
		schedulerUpdate();
	};
	_scheduler = std::make_unique<ChunkScheduler>(
		session,
		_queue.get(),
		std::move(hooks));
}

Api::~Api() = default;

rpl::producer<State> Api::state() const {
	return _state.value();
}

State Api::currentState() const {
	return _state.current();
}

void Api::startScheduler() {
	if (_schedulerStarted) {
		return;
	}
	_schedulerStarted = true;
	_starvationSince = 0;
	_scheduler->start();
}

void Api::stopScheduler() {
	_schedulerStarted = false;
	_starvationSince = 0;
	_scheduler->stop();
}

void Api::start() {
	if (_finished || _cancelled || _walkDone || _recovering) {
		return;
	}
	requestMediaTotals();
	const auto begin = [=] {
		if (_finished || _cancelled || _paused || _walkDone || _recovering) {
			return;
		}
		if (const auto cursor = db().readScanCursor(
				_peer->id)) {
			if (cursor->phase == int(State::Phase::Finished)) {
				return;
			}
			recoverFromCursor(*cursor);
			return;
		}
		startScheduler();
		requestPage();
	};
	if (needsTakeout()) {
		ensureScanTakeout(begin);
	} else {
		begin();
	}
}

bool Api::needsTakeout() const {
	if (const auto channel = _peer->asChannel()) {
		return !(channel->flags() & ChannelDataFlag::NoForwards);
	}
	if (const auto chat = _peer->asChat()) {
		return !(chat->flags() & ChatDataFlag::NoForwards);
	}
	return true;
}

void Api::ensureScanTakeout(Fn<void()> ready) {
	const auto session = _session;
	session->api().ensureTakeout(
		_peer,
		[=](bool ok) {
			if (ok) {
				session->api().setTakeoutBorrowed(true);
			}
			ready();
		},
		kScanTakeoutMaxFileBytes);
}

void Api::finishBorrowedTakeout() {
	const auto session = _session;
	session->api().setTakeoutBorrowed(false);
	if (session->api().takeoutMayFinish()) {
		session->api().finishTakeout();
	}
}

void Api::finishTakeout() {
	finishBorrowedTakeout();
}

void Api::pause() {
	if (_finished || _cancelled) {
		return;
	}
	const auto started = crl::now();
	finishBorrowedTakeout();
	_paused = true;
	++_resumeGen;
	_pageInFlight = false;
	stopScheduler();
	_floodTimer.cancel();
	_recoveryTimer.cancel();
	_pageTimer.cancel();
	_aggregateTimer.cancel();
	_aggregating = false;
	flushHashUpdates();
	db().writeScanCursor({
		.peerId = _peer->id,
		.cursor = MsgId(_offsetId),
		.phase = int(State::Phase::Paused),
	});
	auto state = _state.current();
	state.phase = State::Phase::Paused;
	_state = std::move(state);
	const auto elapsed = crl::now() - started;
	if (elapsed > 50) {
		LOG(("ChatScan: pause took %1 ms.").arg(elapsed));
	}
}

void Api::resume() {
	if (_finished || _cancelled) {
		return;
	}
	requestMediaTotals();
	const auto gen = ++_resumeGen;
	const auto begin = [=] {
		if (_finished || _cancelled || gen != _resumeGen) {
			return;
		}
		const auto started = crl::now();
		_failed = false;
		_paused = false;
		const auto retried = _queue->retryFailed();
		if (retried > 0) {
			LOG(("ChatScan: retrying %1 failed items.").arg(retried));
		}
		auto state = _state.current();
		state.phase = _walkDone
			? State::Phase::Resolving
			: State::Phase::Scanning;
		state.error = QString();
		db().writeScanCursor({
			.peerId = _peer->id,
			.cursor = MsgId(_offsetId),
			.phase = int(state.phase),
		});
		_state = std::move(state);
		_queueOutstanding = _queue->pending() + _queue->leased();
		_failedHashes = _queue->failed();
		dispatchResumed(gen);
		const auto elapsed = crl::now() - started;
		if (elapsed > 50) {
			LOG(("ChatScan: resume begin took %1 ms.").arg(elapsed));
		}
	};
	if (needsTakeout()) {
		ensureScanTakeout(begin);
	} else {
		begin();
	}
}

void Api::dispatchResumed(int gen) {
	if (_finished || _cancelled || gen != _resumeGen) {
		return;
	}
	if (!_walkDone) {
		maybeRequestNext();
	}
	maybeRequestRecovery();
	if (_recoveryDone && _recoveryWindowDone) {
		startScheduler();
	}
	_scheduler->pump();
}

void Api::cancel() {
	if (_finished) {
		return;
	}
	const auto started = crl::now();
	finishBorrowedTakeout();
	_cancelled = true;
	stopScheduler();
	_pageInFlight = false;
	_floodTimer.cancel();
	_recoveryTimer.cancel();
	_pageTimer.cancel();
	_aggregateTimer.cancel();
	_aggregating = false;
	_rebuildTimer.cancel();
	_rebuilding = false;
	_documents.clear();
	_photos.clear();
	_seenDocs.clear();
	_seenPhotos.clear();
	_staleMedia.clear();
	_recoveryIds.clear();
	_recovering = false;
	_recoveryDone = true;
	_recoveryWindowDone = true;
	_queueOutstanding = 0;
	db().clearChatScan(_peer->id);
	ChatScan::ForgetScannedPeer(_peer->id);
	auto state = _state.current();
	state.phase = State::Phase::Cancelled;
	_state = std::move(state);
	const auto elapsed = crl::now() - started;
	if (elapsed > 50) {
		LOG(("ChatScan: cancel took %1 ms.").arg(elapsed));
	}
}

void Api::requestMediaCount(
		const MTPMessagesFilter &filter,
		Fn<void(int)> done) {
	_api.request(MTPmessages_Search(
		MTP_flags(0),
		_peer->input(),
		MTP_string(),
		MTPInputPeer(),
		MTPInputPeer(),
		MTPVector<MTPReaction>(),
		MTPint(),
		filter,
		MTP_int(0),
		MTP_int(0),
		MTP_int(0),
		MTP_int(0),
		MTP_int(1),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0)
	)).done([=](const MTPmessages_Messages &result) {
		done(result.match(
			[](const MTPDmessages_messagesSlice &data) {
				return data.vcount().v;
			},
			[](const MTPDmessages_channelMessages &data) {
				return data.vcount().v;
			},
			[](const auto &) {
				return 0;
			}));
	}).fail([=](const MTP::Error &) {
		done(0);
	}).send();
}

void Api::requestMediaTotals() {
	if (_mediaTotalsRequested || _finished || _cancelled) {
		return;
	}
	_mediaTotalsRequested = true;
	// The six filters partition the media the server indexes, so their counts
	// add up to the number of media messages in the chat. Stickers have no
	// index of their own and are the only type left out.
	const auto total = std::make_shared<int>(0);
	const auto left = std::make_shared<int>(6);
	const auto add = [=](int count) {
		*total += count;
		if (--*left == 0) {
			_mediaTotal = *total;
			publish();
		}
	};
	requestMediaCount(
		MTPMessagesFilter(MTP_inputMessagesFilterPhotoVideo()),
		add);
	requestMediaCount(
		MTPMessagesFilter(MTP_inputMessagesFilterVoice()),
		add);
	requestMediaCount(
		MTPMessagesFilter(MTP_inputMessagesFilterRoundVideo()),
		add);
	requestMediaCount(
		MTPMessagesFilter(MTP_inputMessagesFilterGif()),
		add);
	requestMediaCount(
		MTPMessagesFilter(MTP_inputMessagesFilterDocument()),
		add);
	requestMediaCount(
		MTPMessagesFilter(MTP_inputMessagesFilterMusic()),
		add);
}

void Api::requestPage() {
	if (_paused || _cancelled || _failed || _finished || _walkDone) {
		return;
	}
	_pageInFlight = true;
	auto history = MTPmessages_GetHistory(
		_peer->input(),
		MTP_int(_offsetId),
		MTP_int(0), // offset_date
		MTP_int(0), // add_offset
		MTP_int(kPageLimit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	);
	const auto done = [=](const MTPmessages_Messages &result) {
		handlePage(result);
	};
	const auto failed = [=](const MTP::Error &error) {
		_pageInFlight = false;
		const auto wait = Data::FloodWaitSeconds(error.type());
		if (wait > 0) {
			noteFloodWait(wait);
			_floodTimer.setCallback([=] {
				dispatchResumed(_resumeGen);
			});
			_floodTimer.callOnce(wait * crl::time(1000));
			return;
		}
		fail(error.type());
	};
	if (needsTakeout()) {
		if (const auto takeoutId = _session->api().takeoutId()) {
			_api.request(MTPInvokeWithTakeout<MTPmessages_GetHistory>(
				MTP_long(*takeoutId),
				history
			)).done(done).fail(failed).send();
			return;
		}
	}
	_api.request(std::move(history)).done(done).fail(failed).send();
}

void Api::schedulePage() {
	if (_paused || _cancelled || _failed || _finished || _walkDone) {
		return;
	}
	if (_pageInFlight || _pageTimer.isActive()) {
		return;
	}
	_pageTimer.setCallback([=] {
		requestPage();
	});
	_pageTimer.callOnce(pageDelay());
}

crl::time Api::pageDelay() const {
	const auto jitter = crl::time(
		(uint64(_offsetId) * 31 + uint64(_scanned) * 17) % kPageJitterMs);
	return crl::time(kPageSpacingMs) + jitter;
}

void Api::maybeRequestNext() {
	if (_paused || _cancelled || _failed || _finished || _walkDone) {
		return;
	}
	if (_pageInFlight) {
		return;
	}
	const auto credit = _scheduler->credit();
	const auto pageLimit = std::max(0, credit - kPageLimit);
	if (_queueOutstanding > pageLimit) {
		_walkerCreditBlocked = true;
	} else if (_queueOutstanding * 2 <= credit) {
		_walkerCreditBlocked = false;
	}
	if (_walkerCreditBlocked) {
		return;
	}
	schedulePage();
}

void Api::handlePage(const MTPmessages_Messages &result) {
	_pageInFlight = false;
	if (_cancelled || _failed || _paused) {
		return;
	}
	result.match([&](const MTPDmessages_messages &data) {
		handlePageMessages(data.vmessages().v);
	}, [&](const MTPDmessages_messagesSlice &data) {
		auto state = _state.current();
		state.totalCount = data.vcount().v;
		_state = std::move(state);
		handlePageMessages(data.vmessages().v);
	}, [&](const MTPDmessages_channelMessages &data) {
		auto state = _state.current();
		state.totalCount = data.vcount().v;
		_state = std::move(state);
		handlePageMessages(data.vmessages().v);
	}, [&](const MTPDmessages_messagesNotModified &) {
		finish();
	});
}

void Api::handlePageMessages(const QVector<MTPMessage> &messages) {
	if (messages.isEmpty()) {
		finish();
		return;
	}
	auto minId = std::numeric_limits<int32>::max();
	auto stopped = false;
	for (const auto &message : messages) {
		const auto id = message.match([](const MTPDmessage &data) {
			return data.vid().v;
		}, [](const MTPDmessageService &data) {
			return data.vid().v;
		}, [](const MTPDmessageEmpty &data) {
			return data.vid().v;
		}, [](const auto &) {
			return 0;
		});
		if (id <= 0) {
			continue;
		}
		minId = std::min(minId, id);
		if (_rescan) {
			if (id <= _stopMark) {
				stopped = true;
				continue;
			}
			if (_storedIds.contains(int64(id))) {
				continue;
			}
			++_walkedNew;
		}
		if ((!_rescan ? !_scanned : !_walkedNew) && !_highMark) {
			_highMark = id;
		}
		message.match([&](const MTPDmessage &data) {
			handleRegularMessage(data);
		}, [&](const auto &) {
		});
		++_scanned;
	}
	if (minId == std::numeric_limits<int32>::max()) {
		finish();
		return;
	}
	_offsetId = minId;
	commitPage();
	publish();
	if (_paused) {
		auto state = _state.current();
		state.phase = State::Phase::Paused;
		_state = std::move(state);
		return;
	}
	_scheduler->pump();
	if (stopped) {
		finish();
		return;
	}
	maybeRequestNext();
}

void Api::handleRegularMessage(const MTPDmessage &message) {
	const auto id = MsgId(message.vid().v);
	const auto media = message.vmedia();
	if (!media) {
		return;
	}
	media->match([&](const MTPDmessageMediaDocument &data) {
		if (const auto document = data.vdocument()) {
			addDocOccurrence(
				_session->data().processDocument(*document),
				id,
				data.is_video());
		}
	}, [&](const MTPDmessageMediaPhoto &data) {
		if (const auto photo = data.vphoto()) {
			addPhotoOccurrence(
				_session->data().processPhoto(*photo),
				id);
		}
	}, [&](const auto &) {
	});
}

void Api::addDocOccurrence(
		not_null<DocumentData*> document,
		MsgId msgId,
		bool wrapperVideo) {
	const auto type = ScanMediaCategory(document, wrapperVideo);
	auto item = Data::ChatScanItem();
	item.peerId = _peer->id;
	item.msgId = msgId;
	item.category = type;
	item.size = document->size;
	item.mediaId = document->id;
	item.isPhoto = false;
	item.filename = document->filename();
	_documents[document->id] = document.get();
	if (!_seenDocs.insert(document->id).second) {
		// A second occurrence needs no hash of its own: the representative
		// row's commit fills this row too. Keeping it out of the queue is
		// what makes the walker's outstanding counter match one lease per
		// media id.
		item.state = 2;
		_pageItems.push_back(item);
		auto &totals = _totals[type];
		totals.dupCount++;
		totals.dupBytes += document->size;
		pushDupRow(
			msgId,
			type,
			document->size,
			document->filename());
		bumpProgressDelta(type, 1, 0, 0, 1, document->size);
		return;
	}
	_pageItems.push_back(item);
	auto &totals = _totals[type];
	totals.uniqueCount++;
	totals.uniqueBytes += document->size;
	bumpProgressDelta(type, 1, 1, document->size, 0, 0);
	++_queueOutstanding;
}

void Api::addPhotoOccurrence(
		not_null<PhotoData*> photo,
		MsgId msgId) {
	using Type = Export::MediaSettings::Type;
	auto size = int64(photo->imageByteSize(Data::PhotoSize::Large));
	if (!size) {
		size = photo->videoByteSize(Data::PhotoSize::Large);
	}
	auto item = Data::ChatScanItem();
	item.peerId = _peer->id;
	item.msgId = msgId;
	item.category = int(Type::Photo);
	item.size = size;
	item.mediaId = photo->id;
	item.isPhoto = true;
	_photos[photo->id] = photo.get();
	if (!_seenPhotos.insert(photo->id).second) {
		item.state = 2;
		_pageItems.push_back(item);
		auto &totals = _totals[int(Type::Photo)];
		totals.dupCount++;
		totals.dupBytes += size;
		pushDupRow(msgId, int(Type::Photo), size, QString());
		bumpProgressDelta(int(Type::Photo), 1, 0, 0, 1, size);
		return;
	}
	_pageItems.push_back(item);
	auto &totals = _totals[int(Type::Photo)];
	totals.uniqueCount++;
	totals.uniqueBytes += size;
	bumpProgressDelta(int(Type::Photo), 1, 1, size, 0, 0);
	++_queueOutstanding;
}

void Api::pushDupRow(
		MsgId msgId,
		int category,
		int64 size,
		const QString &filename) {
	auto dup = Data::ChatScanDup();
	dup.peerId = _peer->id;
	dup.msgId = msgId;
	dup.category = category;
	dup.size = size;
	dup.flags = 0;
	dup.filename = filename;
	_pendingDupRows.push_back(dup);
}

void Api::commitPage() {
	_scheduler->flush();
	flushHashUpdates();
	auto &database = db();
	_queue->insert(_pageItems);
	_pageItems.clear();
	database.beginTransaction();
	for (const auto &dup : _pageDups) {
		database.writeChatScanDup(dup);
	}
	_pageDups.clear();
	database.writeScanCursor({
		.peerId = _peer->id,
		.cursor = MsgId(_offsetId),
		.phase = int(State::Phase::Scanning),
	});
	database.commitTransaction();
}

void Api::flushHashUpdates() {
	if (_pendingDupRows.empty() && _progressDeltas.empty()) {
		return;
	}
	auto &database = db();
	database.beginTransaction();
	for (const auto &dup : _pendingDupRows) {
		database.writeChatScanDup(dup);
	}
	_pendingDupRows.clear();
	for (const auto &[category, delta] : _progressDeltas) {
		database.updateChatScanProgress(
			_peer->id,
			category,
			delta.itemCount,
			delta.uniqueCount,
			delta.uniqueBytes,
			delta.dupCount,
			delta.dupBytes);
	}
	_progressDeltas.clear();
	database.commitTransaction();
}

void Api::bumpProgressDelta(
		int category,
		int64 dItemCount,
		int64 dUniqueCount,
		int64 dUniqueBytes,
		int64 dDupCount,
		int64 dDupBytes) {
	auto &delta = _progressDeltas[category];
	delta.itemCount += dItemCount;
	delta.uniqueCount += dUniqueCount;
	delta.uniqueBytes += dUniqueBytes;
	delta.dupCount += dDupCount;
	delta.dupBytes += dDupBytes;
}

void Api::rescan() {
	if (_finished || _cancelled || _walkDone || _recovering || _rescan) {
		return;
	}
	requestMediaTotals();
	const auto begin = [=] {
		if (_finished || _cancelled || _walkDone || _recovering || _rescan) {
			return;
		}
		rescanAfterTakeout();
	};
	if (needsTakeout()) {
		ensureScanTakeout(begin);
	} else {
		begin();
	}
}

void Api::rescanAfterTakeout() {
	const auto peerId = _peer->id;
	auto mark = int64(0);
	for (const auto &row : db().readChatScan(peerId)) {
		mark = std::max(mark, row.scannedUpto.bare);
	}
	if (!mark) {
		return;
	}
	_rescan = true;
	_stopMark = mark;
	_queueOutstanding = _queue->pending() + _queue->leased();
	_failedHashes = _queue->failed();
	_progressDeltas.clear();
	_rebuildDupIds.clear();
	_rebuildAboveBare = 0;
	_rebuildStage = 0;
	_rebuilding = true;
	rebuildStep();
}

void Api::rebuildStep() {
	constexpr auto kBatch = 5000;
	auto &database = db();
	const auto peerId = _peer->id;
	if (!_rebuilding || _cancelled || _finished) {
		_rebuilding = false;
		return;
	}
	if (_rebuildStage == 0) {
		const auto batch = database.readChatScanDupsBefore(
			peerId,
			-1,
			kBatch,
			MsgId(_rebuildAboveBare));
		for (const auto &dup : batch) {
			_rebuildAboveBare = int64(dup.msgId.bare);
			_rebuildDupIds.insert(_rebuildAboveBare);
		}
		if (int(batch.size()) == kBatch) {
			continueRebuildStep();
			return;
		}
		_rebuildStage = 1;
		_rebuildAboveBare = 0;
	}
	const auto batch = database.chatScanItemsWindow(
		peerId,
		MsgId(_rebuildAboveBare),
		kBatch);
	for (const auto &item : batch) {
		restoreRebuiltItem(item);
	}
	if (int(batch.size()) == kBatch) {
		continueRebuildStep();
		return;
	}
	_rebuilding = false;
	_highMark = 0;
	for (const auto &row : database.recomputeChatScanProgress(peerId)) {
		auto &totals = _totals[row.category];
		totals.uniqueCount = int(row.uniqueCount);
		totals.uniqueBytes = row.uniqueBytes;
		totals.dupCount = int(row.dupCount);
		totals.dupBytes = row.dupBytes;
	}
	if (_queueOutstanding > 0) {
		_recoveryWindowDone = true;
		_recoveryDone = false;
		_recoveryAbove = int32(
			database.chatScanMaxMsgId(peerId).bare) + 1;
	}
	publish();
	maybeRequestRecovery();
	if (_recoveryDone && _recoveryWindowDone) {
		startScheduler();
	}
	requestPage();
}

void Api::continueRebuildStep() {
	_rebuildTimer.setCallback([=] {
		rebuildStep();
	});
	_rebuildTimer.callOnce(0);
}

void Api::restoreRebuiltItem(const Data::ChatScanItem &item) {
	++_scanned;
	_storedIds.insert(int64(item.msgId.bare));
	if (!item.hash.isEmpty()) {
		_seenHashes.insert(item.hash);
	}
	auto &totals = _totals[item.category];
	if (_rebuildDupIds.find(int64(item.msgId.bare)) != _rebuildDupIds.end()) {
		totals.dupCount++;
		totals.dupBytes += item.size;
	} else {
		totals.uniqueCount++;
		totals.uniqueBytes += item.size;
	}
	if (!item.mediaId) {
		return;
	}
	if (item.isPhoto) {
		_seenPhotos.insert(item.mediaId);
	} else {
		_seenDocs.insert(item.mediaId);
	}
}

bool Api::loadPaused(const Data::ChatScanState &cursor) {
	if (_finished || _cancelled || _walkDone) {
		return false;
	}
	_paused = true;
	++_resumeGen;
	const auto retried = _queue->retryFailed();
	if (retried > 0) {
		LOG(("ChatScan: retrying %1 failed items.").arg(retried));
	}
	requestMediaTotals();
	_floodTimer.cancel();
	_offsetId = int32(cursor.cursor.bare);
	_totals.clear();
	_scanned = 0;
	_highMark = 0;
	if (!seedFromDbAggregates()) {
		return false;
	}
	publish();
	auto state = _state.current();
	state.phase = State::Phase::Paused;
	_state = std::move(state);
	return true;
}

bool Api::seedFromDbAggregates() {
	auto &database = db();
	if (!database.isOpen()) {
		return false;
	}
	auto progress = database.readChatScanProgress(_peer->id);
	if (progress.empty()) {
		progress = database.recomputeChatScanProgress(_peer->id);
	}
	return seedFromProgress(progress);
}

bool Api::seedFromProgress(
		const std::vector<Data::ChatScanProgressRow> &progress) {
	if (progress.empty()) {
		return false;
	}
	auto itemCount = int64(0);
	for (const auto &row : progress) {
		itemCount += row.itemCount;
		auto &totals = _totals[row.category];
		totals.uniqueCount = int(row.uniqueCount);
		totals.uniqueBytes = row.uniqueBytes;
		totals.dupCount = int(row.dupCount);
		totals.dupBytes = row.dupBytes;
	}
	_scanned = int(itemCount);
	_highMark = int32(db().chatScanMaxMsgId(_peer->id).bare);
	_hashDone = db().chatScanHashedCount(_peer->id);
	_failedHashes = _queue->failed();
	_queueOutstanding = _queue->pending() + _queue->leased();
	_recoveryIds.clear();
	_recoveryPos = 0;
	_recoveryWindowDone = true;
	_recovering = false;
	_recoveryDone = false;
	_recoveryAbove = _highMark + 1;
	_recoveryErrorStreak = 0;
	return true;
}

void Api::rememberRestoredMedia(const Data::ChatScanItem &item) {
	if (!item.mediaId) {
		return;
	}
	if (item.isPhoto) {
		_seenPhotos.insert(item.mediaId);
	} else {
		_seenDocs.insert(item.mediaId);
	}
}

void Api::recoverFromCursor(const Data::ChatScanState &cursor) {
	stopScheduler();
	const auto retried = _queue->retryFailed();
	if (retried > 0) {
		LOG(("ChatScan: retrying %1 failed items.").arg(retried));
	}
	_documents.clear();
	_photos.clear();
	_seenDocs.clear();
	_seenPhotos.clear();
	_seenHashes.clear();
	_staleMedia.clear();
	_pageDups.clear();
	_pendingDupRows.clear();
	_totals.clear();
	_pageItems.clear();
	_recoveryIds.clear();
	_recoveryPos = 0;
	_recoveryErrorStreak = 0;
	_storedIds.clear();
	_scanned = 0;
	_highMark = 0;
	_hashDone = 0;
	_failedHashes = 0;
	_queueOutstanding = 0;
	_pageInFlight = false;
	_offsetId = int32(cursor.cursor.bare);
	auto progress = std::vector<Data::ChatScanProgressRow>();
	if (cursor.phase != int(State::Phase::Paused)) {
		// An unclean stop leaves the cursor and the incremental progress
		// snapshot at slightly different points; the item rows are the
		// source of truth, so rebuild the snapshot once before resuming.
		progress = db().recomputeChatScanProgress(_peer->id);
	} else {
		progress = db().readChatScanProgress(_peer->id);
		if (progress.empty()) {
			progress = db().recomputeChatScanProgress(_peer->id);
		}
	}
	if (!seedFromProgress(progress)) {
		requestPage();
		maybeRequestRecovery();
		if (_recoveryDone && _recoveryWindowDone) {
			startScheduler();
		}
		return;
	}
	publish();
	auto state = _state.current();
	state.phase = State::Phase::Scanning;
	_state = std::move(state);
	if (!_walkDone) {
		maybeRequestNext();
	}
	maybeRequestRecovery();
	if (_recoveryDone && _recoveryWindowDone) {
		startScheduler();
	}
}

void Api::refillRecoveryWindow() {
	_recoveryIds.clear();
	_recoveryPos = 0;
	_recoveryWindowDone = true;
	if (_recoveryAbove <= 0) {
		_recoveryDone = true;
		return;
	}
	const auto items = db().chatScanPendingItemsWindow(
		_peer->id,
		MsgId(_recoveryAbove),
		kRestoreWindow);
	for (const auto &item : items) {
		rememberRestoredMedia(item);
		if (!item.mediaId) {
			continue;
		}
		const auto loaded = item.isPhoto
			? (_photos.find(item.mediaId) != _photos.end())
			: (_documents.find(item.mediaId) != _documents.end());
		const auto stale = (_staleMedia.find(item.mediaId) != _staleMedia.end());
		if ((!loaded || stale) && item.msgId) {
			_recoveryIds.push_back(int32(item.msgId.bare));
		}
	}
	if (_recoveryIds.empty()) {
		_recoveryDone = true;
		return;
	}
	if (_recoveryIds.size() < size_t(kRestoreWindow)) {
		_recoveryDone = true;
	}
	_recoveryAbove = _recoveryIds.back() - 1;
	_recoveryWindowDone = false;
}

void Api::maybeRequestRecovery() {
	if (_finished || _cancelled || _paused || _failed) {
		return;
	}
	if (_recoveryDone && _recoveryWindowDone) {
		_recovering = false;
		startScheduler();
		return;
	}
	_recovering = true;
	if (_recoveryInFlight) {
		return;
	}
	requestRecoveryBatch();
}

void Api::requestRecoveryBatch() {
	if (_paused || _cancelled || _failed || _finished || _recoveryInFlight) {
		return;
	}
	if (_recoveryWindowDone) {
		if (!_recoveryDone) {
			refillRecoveryWindow();
		}
		if (_recoveryDone && _recoveryWindowDone) {
			_recovering = false;
			startScheduler();
			_scheduler->pump();
			maybeRequestNext();
			return;
		}
	}
	const auto from = _recoveryPos;
	const auto count = std::min(
		int(kPageLimit),
		int(_recoveryIds.size()) - from);
	if (count <= 0) {
		_recoveryWindowDone = true;
		_recovering = false;
		startScheduler();
		_scheduler->pump();
		maybeRequestNext();
		return;
	}
	auto ids = QVector<MTPInputMessage>();
	ids.reserve(count);
	for (auto i = 0; i != count; ++i) {
		ids.push_back(MTP_inputMessageID(MTP_int(_recoveryIds[from + i])));
	}
	_recoveryInFlight = true;
	const auto advance = [=] {
		_recoveryPos = from + count;
		if (_recoveryPos >= int(_recoveryIds.size())) {
			_recoveryWindowDone = true;
		}
	};
	const auto done = [=](const MTPmessages_Messages &result) {
		if (_cancelled || _failed) {
			return;
		}
		_recoveryInFlight = false;
		_recoveryErrorStreak = 0;
		advance();
		result.match([&](const MTPDmessages_messages &data) {
			handleRecoveryMessages(data.vmessages().v);
		}, [&](const MTPDmessages_messagesSlice &data) {
			handleRecoveryMessages(data.vmessages().v);
		}, [&](const MTPDmessages_channelMessages &data) {
			handleRecoveryMessages(data.vmessages().v);
		}, [&](const MTPDmessages_messagesNotModified &) {
		});
		_scheduler->pump();
		_recoveryTimer.setCallback([=] {
			maybeRequestRecovery();
		});
		_recoveryTimer.callOnce(RecoveryDelay(_recoveryAbove, _recoveryPos));
	};
	const auto failed = [=](const MTP::Error &error) {
		if (_cancelled || _failed) {
			return;
		}
		_recoveryInFlight = false;
		const auto wait = Data::FloodWaitSeconds(error.type());
		if (wait > 0) {
			noteFloodWait(wait);
			_floodTimer.setCallback([=] {
				dispatchResumed(_resumeGen);
			});
			_floodTimer.callOnce(wait * crl::time(1000));
			return;
		}
		LOG(("ChatScan: recovery batch failed: %1").arg(error.type()));
		if (++_recoveryErrorStreak >= kRecoveryBatchErrorCeil) {
			fail(error.type());
			return;
		}
		advance();
		_scheduler->pump();
		_recoveryTimer.setCallback([=] {
			maybeRequestRecovery();
		});
		_recoveryTimer.callOnce(RecoveryDelay(_recoveryAbove, _recoveryPos));
	};
	const auto takeoutId = needsTakeout()
		? _session->api().takeoutId()
		: std::nullopt;
	if (const auto channel = _peer->asChannel()) {
		auto request = MTPchannels_GetMessages(
			channel->inputChannel(),
			MTP_vector<MTPInputMessage>(ids));
		if (takeoutId) {
			_api.request(MTPInvokeWithTakeout<MTPchannels_GetMessages>(
				MTP_long(*takeoutId),
				std::move(request)
			)).done(std::move(done)).fail(std::move(failed)).send();
			return;
		}
		_api.request(std::move(request))
			.done(std::move(done))
			.fail(std::move(failed))
			.send();
		return;
	}
	auto request = MTPmessages_GetMessages(MTP_vector<MTPInputMessage>(ids));
	if (takeoutId) {
		_api.request(MTPInvokeWithTakeout<MTPmessages_GetMessages>(
			MTP_long(*takeoutId),
			std::move(request)
		)).done(std::move(done)).fail(std::move(failed)).send();
		return;
	}
	_api.request(std::move(request))
		.done(std::move(done))
		.fail(std::move(failed))
		.send();
}

void Api::handleRecoveryMessages(const QVector<MTPMessage> &messages) {
	for (const auto &message : messages) {
		const auto id = message.match([](const MTPDmessage &data) {
			return data.vid().v;
		}, [](const MTPDmessageService &data) {
			return data.vid().v;
		}, [](const MTPDmessageEmpty &data) {
			return data.vid().v;
		}, [](const auto &) {
			return 0;
		});
		if (id <= 0) {
			continue;
		}
		message.match([&](const MTPDmessage &data) {
			handleRecoveryMessage(data);
		}, [&](const auto &) {
		});
	}
	_scheduler->pump();
}

void Api::handleRecoveryMessage(const MTPDmessage &message) {
	const auto media = message.vmedia();
	if (!media) {
		return;
	}
	media->match([&](const MTPDmessageMediaDocument &data) {
		if (const auto document = data.vdocument()) {
			const auto doc = _session->data().processDocument(*document);
			_documents[doc->id] = doc.get();
			_seenDocs.insert(doc->id);
			_staleMedia.erase(doc->id);
		}
	}, [&](const MTPDmessageMediaPhoto &data) {
		if (const auto photo = data.vphoto()) {
			const auto ph = _session->data().processPhoto(*photo);
			_photos[ph->id] = ph.get();
			_seenPhotos.insert(ph->id);
			_staleMedia.erase(ph->id);
		}
	}, [&](const auto &) {
	});
}

void Api::noteFloodWait(int seconds) {
	_floodWaitUntilMs = crl::now() + seconds * crl::time(1000);
	_lastPublishMs = 0;
	publish();
}

void Api::publish() {
	const auto now = crl::now();
	auto state = _state.current();
	if (state.phase == _lastPhase && now - _lastPublishMs < 250) {
		return;
	}
	_lastPhase = state.phase;
	_lastPublishMs = now;
	const auto chunkUntil = _session->data().chunkFetcher().unblockTime();
	state.floodUntilMs = std::max(_floodWaitUntilMs, int64(chunkUntil));
	state.scannedCount = _scanned;
	state.categories = _totals;
	state.hashDone = _hashDone;
	state.mediaTotal = _mediaTotal;
	state.hashTotal = _hashDone
		+ _queueOutstanding
		+ _failedHashes;
	state.failedHashes = _failedHashes;
	_state = std::move(state);
}

void Api::finish() {
	if (_walkDone || _paused) {
		return;
	}
	_walkDone = true;
	publish();
	auto state = _state.current();
	state.phase = State::Phase::Resolving;
	_state = std::move(state);
	_scheduler->pump();
	maybeFinalize();
}

void Api::fail(const QString &error) {
	_failed = true;
	stopScheduler();
	_recoveryTimer.cancel();
	_pageTimer.cancel();
	auto state = _state.current();
	state.phase = State::Phase::Failed;
	state.error = error;
	_state = std::move(state);
}

void Api::schedulerUpdate() {
	if (_finished || _cancelled) {
		return;
	}
	if (!_queueOutstanding
		&& !_scheduler->active()
		&& !_scheduler->pumping()) {
		_scheduler->flush();
	}
	publish();
	if (!_walkDone && !_paused && !_failed) {
		maybeRequestNext();
	}
	maybeRequestRecovery();
	logSchedulerStarvation();
	maybeFinalize();
}

void Api::logSchedulerStarvation() {
	if (_paused
		|| _finished
		|| _cancelled
		|| _failed
		|| _walkDone
		|| _recovering
		|| _recoveryInFlight
		|| _pageInFlight) {
		_starvationSince = 0;
		return;
	}
	const auto active = _scheduler->active();
	if (active >= _scheduler->target()
		|| _queueOutstanding >= _scheduler->credit()) {
		_starvationSince = 0;
		return;
	}
	const auto now = crl::now();
	if (!_starvationSince) {
		_starvationSince = now;
		return;
	}
	if (now - _starvationSince < kStarvationDelayMs
		|| now - _lastStarvationLogMs < kStarvationLogMs) {
		return;
	}
	_lastStarvationLogMs = now;
	const auto rate = _session->data().chunkFetcher().achievedRate();
	LOG(("ChatScan: scheduler starved, active %1/%2, pending %3, "
		"leased %4, outstanding %5, rate %6/s")
		.arg(active)
		.arg(_scheduler->target())
		.arg(_queue->pending())
		.arg(_queue->leased())
		.arg(_queueOutstanding)
		.arg(rate, 0, 'f', 1));
}

void Api::maybeFinalize() {
	if (_finished
		|| _cancelled
		|| _failed
		|| _paused
		|| !_walkDone
		|| _recovering) {
		return;
	}
	if (!_scheduler->active() && !_scheduler->pumping()) {
		_scheduler->flush();
		_queueOutstanding = _queue->pending() + _queue->leased();
	}
	if (!_scheduler->idle() || _queueOutstanding) {
		return;
	}
	finalizeAndWrite();
}

void Api::onHashed(
		const Data::ChatScanItem &item,
		const QByteArray &hash) {
	if (_cancelled || _failed || hash.isEmpty()) {
		return;
	}
	_queueOutstanding = std::max(0, _queueOutstanding - 1);
	using Table = Data::DedupDb::Table;
	const auto tierOn = GetEnhancedBool("prevent_upload_duplicates");
	const auto fresh = _seenHashes.insert(hash).second;
	if ((tierOn
			&& db().containsHash(Table::Uploads, hash)
			&& db().containsFinishedHash(Table::Uploads, hash))
		|| !fresh
		|| db().chatScanContainsHash(_peer->id, hash)) {
		auto &totals = _totals[item.category];
		totals.uniqueCount--;
		totals.uniqueBytes -= item.size;
		totals.dupCount++;
		totals.dupBytes += item.size;
		bumpProgressDelta(
			item.category,
			0,
			-1,
			-item.size,
			1,
			item.size);
		pushDupRow(
			item.msgId,
			item.category,
			item.size,
			item.filename);
	}
	publish();
}

void Api::onHashFailed(const Data::ChatScanItem &) {
	if (_cancelled || _failed) {
		return;
	}
	_queueOutstanding = std::max(0, _queueOutstanding - 1);
	++_failedHashes;
	publish();
}

void Api::onHashCommitted(int resolved) {
	_hashDone += resolved;
	flushHashUpdates();
	publish();
}

bool Api::onHashReferenceExpired(const Data::ChatScanItem &item) {
	if (_cancelled || _failed || _finished || _paused) {
		return false;
	}
	const auto id = int32(item.msgId.bare);
	const auto retry = (item.attempts <= 2) && (id != 0);
	if (!retry) {
		return false;
	}
	_staleMedia.insert(item.mediaId);
	_recoveryAbove = std::max(_recoveryAbove, id + 1);
	_recoveryWindowDone = true;
	_recoveryDone = false;
	maybeRequestRecovery();
	return true;
}

void Api::finalizeAndWrite() {
	if (_finished || _cancelled || _aggregating) {
		return;
	}
	_aggregating = true;
	if (_rescan && !_walkedNew) {
		_aggregating = false;
		stopScheduler();
		finishBorrowedTakeout();
		_finished = true;
		auto state = _state.current();
		state.phase = State::Phase::Finished;
		_state = std::move(state);
		return;
	}
	stopScheduler();
	_scheduler->flush();
	flushHashUpdates();
	finishBorrowedTakeout();
	writeAggregateRows();
}

void Api::writeAggregateRows() {
	auto &database = db();
	const auto peerId = _peer->id;

	flushHashUpdates();
	auto dupTotal = 0;
	auto dupBytes = int64(0);
	for (const auto &[category, t] : _totals) {
		dupTotal += t.dupCount;
		dupBytes += t.dupBytes;
	}
	database.beginTransaction();
	for (auto &[category, t] : _totals) {
		auto row = Data::ChatScanRow();
		row.peerId = peerId;
		row.category = category;
		row.uniqueCount = t.uniqueCount;
		row.uniqueBytes = t.uniqueBytes;
		row.dupCount = t.dupCount;
		row.dupBytes = t.dupBytes;
		row.scannedUpto = MsgId(_highMark);
		row.scanFinished = true;
		row.deleteFinished = (dupTotal == 0);
		database.writeChatScanRow(row);
	}
	database.commitTransaction();
	_aggregateAbove = MsgId(std::numeric_limits<int64>::max());
	aggregateStep();
}

void Api::aggregateStep() {
	constexpr auto kBatch = 2000;
	using Table = Data::DedupDb::Table;
	auto &database = db();
	const auto peerId = _peer->id;
	if (_cancelled) {
		_aggregating = false;
		return;
	}
	if (_aggregateAbove.bare > 1) {
		const auto batch = database.chatScanHashedItemsWindow(
			peerId,
			_aggregateAbove,
			kBatch);
		for (const auto &item : batch) {
			_aggregateAbove = item.msgId;
		}
		if (!batch.empty()) {
			database.beginTransaction();
			for (const auto &item : batch) {
				if (!item.mediaId || item.hash.isEmpty()) {
					continue;
				}
				if (database.containsDocId(Table::Uploads, item.mediaId)) {
					continue;
				}
				if (database.containsHash(Table::Uploads, item.hash)
					&& !database.containsFinishedHash(
						Table::Uploads,
						item.hash)) {
					continue;
				}
				database.insert(
					Table::Uploads,
					{ item.hash, item.mediaId, u"f"_q });
			}
			database.commitTransaction();
			if (int(batch.size()) == kBatch) {
				_aggregateTimer.setCallback([=] {
					aggregateStep();
				});
				_aggregateTimer.callOnce(0);
				return;
			}
		}
	}
	database.beginTransaction();
	database.writeScanCursor({
		peerId,
		MsgId(_highMark),
		int(State::Phase::Finished),
	});
	database.commitTransaction();

	stopScheduler();
	_documents.clear();
	_photos.clear();
	_seenDocs.clear();
	_seenPhotos.clear();
	_seenHashes.clear();
	_staleMedia.clear();
	_pageDups.clear();
	_pageDups.shrink_to_fit();
	_pendingDupRows.clear();
	_pendingDupRows.shrink_to_fit();
	_pageItems.clear();
	_pageItems.shrink_to_fit();
	_recoveryIds.clear();
	_recoveryIds.shrink_to_fit();
	_recovering = false;
	_recoveryDone = true;
	_recoveryWindowDone = true;
	_recoveryInFlight = false;
	_recoveryTimer.cancel();
	_queueOutstanding = 0;

	_aggregating = false;
	_finished = true;
	auto dupTotal = 0;
	auto dupBytes = int64(0);
	for (const auto &[category, t] : _totals) {
		dupTotal += t.dupCount;
		dupBytes += t.dupBytes;
	}
	auto state = _state.current();
	state.phase = State::Phase::Finished;
	state.categories = _totals;
	state.dupTotal = dupTotal;
	state.dupBytes = dupBytes;
	state.hashDone = _hashDone;
	state.hashTotal = _hashDone + _failedHashes;
	_state = std::move(state);
	ChatScan::ForgetScannedPeer(peerId);
	ChatScan::RememberScannedPeer(peerId);
}

Data::DedupDb &Api::db() const {
	return Core::App().downloadManager().ensureDedupDb();
}

} // namespace ChatScan
