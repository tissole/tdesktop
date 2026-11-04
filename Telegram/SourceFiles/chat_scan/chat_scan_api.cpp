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
#include "storage/storage_facade.h"
#include "storage/storage_shared_media.h"

#include <QDateTime>

#include <algorithm>
#include <limits>
#include <optional>

#include <crl/crl.h>

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
// How long one loop turn may spend applying finished hashes before the rest
// goes to the next turn. The throttle covers about 35 ms of silence with its
// burst, so a turn has to stay well under that on any machine.
constexpr auto kApplyTurnBudgetMs = 20;
[[nodiscard]] crl::time RecoveryDelay(int32 above, int pos) {
	return crl::time(kRecoverySpacingMs)
		+ crl::time((uint64(above) * 13 + uint64(pos) * 7) % kPageJitterMs);
}

[[nodiscard]] int ScanMediaCategory(
		not_null<DocumentData*> document) {
	return int(DocumentMediaCategory(document));
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

bool Api::clearing() const {
	return _clearing;
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
	const auto mode = db().chatScanMode(_peer->id);
	const auto current = GetEnhancedBool("prevent_upload_duplicates") ? 1 : 0;
	const auto cursor = db().readScanCursor(_peer->id);
	if (cursor) {
		const auto finished = (cursor->phase == int(State::Phase::Finished));
		const auto cancelled = (cursor->phase
			== int(State::Phase::Cancelled));
		if (!finished && !cancelled && (mode == -1 || mode == current)) {
			requestMediaTotals();
			recoverFromCursor(*cursor);
			return;
		}
		if (finished && mode == current) {
			return;
		}
	}
	if (mode != current) {
		if (mode == -1 && !cursor) {
			requestMediaTotals();
			beginFreshScan();
		} else {
			startFullRescan();
		}
		return;
	}
	requestMediaTotals();
	beginFreshScan();
}

void Api::beginFreshScan() {
	if (_finished || _cancelled || _paused || _walkDone || _recovering) {
		return;
	}
	const auto begin = [=] {
		if (_finished || _cancelled || _paused || _walkDone || _recovering) {
			return;
		}
		db().writeChatScanMode(_peer->id, -1);
		_elapsedMs = 0;
		startElapsed();
		loadKnownHashes([=] {
			if (_finished
				|| _cancelled
				|| _paused
				|| _walkDone
				|| _recovering) {
				return;
			}
			startScheduler();
			requestPage();
		});
	};
	if (needsTakeout()) {
		ensureScanTakeout(begin);
	} else {
		begin();
	}
}

void Api::startFullRescan() {
	if (_clearing || _finished || _cancelled) {
		return;
	}
	_clearing = true;
	_incremental = false;
	finishBorrowedTakeout();
	stopScheduler();
	auto state = _state.current();
	state.phase = State::Phase::Cancelling;
	state.elapsedMs = 0;
	_state = std::move(state);
	const auto weak = base::weak_ptr<Api>(this);
	db().clearChatScanAsync(
		_peer->id,
		[weak](
				bool success,
				std::vector<Data::ChatScanOwnerChange> changed) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			strong->_clearing = false;
			if (!success) {
				LOG(("ChatScan: failed to clear before full rescan."));
				auto state = strong->_state.current();
				state.phase = State::Phase::Failed;
				strong->_state = std::move(state);
				return;
			}
			const auto stickerTypes = Storage::SharedMediaTypesMask{}.added(
				Storage::SharedMediaType::Sticker);
			strong->_session->storage().remove(Storage::SharedMediaRemoveAll(
				strong->_peer->id,
				stickerTypes));
			ChatScan::ForgetScannedPeer(strong->_peer->id);
			for (const auto &[peerId, hash] : changed) {
				ChatScan::MarkUniqueHashDirty(peerId, hash);
				strong->db().invalidateChatTable(peerId);
			}
			strong->db().invalidateChatTable(strong->_peer->id);
			strong->db().invalidateAllPeersChatTable();
			if (strong->_cancelled) {
				auto state = strong->_state.current();
				state.phase = State::Phase::Cancelled;
				state.elapsedMs = 0;
				strong->_state = std::move(state);
				return;
			}
			auto state = strong->_state.current();
			state.phase = State::Phase::Scanning;
			strong->_state = std::move(state);
			strong->requestMediaTotals();
			strong->beginFreshScan();
		});
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
	// The paused cursor has to describe stored rows, so everything already
	// finished is applied before the cursor is written.
	if (!_pendingHashed.empty()) {
		drainHashed(std::numeric_limits<int>::max());
	}
	finishBorrowedTakeout();
	_paused = true;
	++_resumeGen;
	_pageInFlight = false;
	stopScheduler();
	_floodTimer.cancel();
	_recoveryTimer.cancel();
	_pageTimer.cancel();
	_aggregating = false;
	freezeElapsed();
	db().writeScanCursor({
		.peerId = _peer->id,
		.cursor = MsgId(_offsetId),
		.phase = int(State::Phase::Paused),
		.elapsedMs = _elapsedMs,
	});
	auto state = _state.current();
	state.phase = State::Phase::Paused;
	state.elapsedMs = _elapsedMs;
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
		startElapsed();
		_postFinishRetryUsed = false;
		const auto retried = _queue->retryFailed();
		if (retried > 0) {
			LOG(("ChatScan: retrying %1 failed items.").arg(retried));
		}
		auto state = _state.current();
		state.phase = _walkDone
			? State::Phase::Resolving
			: State::Phase::Scanning;
		state.error = QString();
		state.elapsedMs = elapsedMs();
		db().writeScanCursor({
			.peerId = _peer->id,
			.cursor = MsgId(_offsetId),
			.phase = int(state.phase),
			.elapsedMs = state.elapsedMs,
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
	if (_finished
		|| _cancelled
		|| _rebuilding
		|| _recomputingProgress
		|| _loadingResumeSets
		|| gen != _resumeGen) {
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
	if (_finished || _cancelled) {
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
	_aggregating = false;
	_rebuildTimer.cancel();
	_rebuilding = false;
	_recomputingProgress = false;
	_loadingResumeSets = false;
	_resumeSetsPending = 0;
	_resumeSetsDone = nullptr;
	_documents.clear();
	_photos.clear();
	_seenDocs.clear();
	_seenPhotos.clear();
	clearHashedBatch();
	_uniqueHashes.clear();
	_queue->clearKnownHashes();
	db().clearChatTable();
	db().invalidateChatTable(_peer->id);
	db().invalidateAllPeersChatTable();
	_uploadTableUse.reset();
	_pageStickers.clear();
	_stickerTotal = 0;
	_staleMedia.clear();
	_recoveryIds.clear();
	_recovering = false;
	_recoveryDone = true;
	_recoveryWindowDone = true;
	_queueOutstanding = 0;
	_elapsedMs = 0;
	_elapsedStartedAt = 0;
	const auto stickerTypes = Storage::SharedMediaTypesMask{}.added(
		Storage::SharedMediaType::Sticker);
	const auto session = db().chatScanSession(_peer->id);
	if (session) {
		const auto stickerIds = db().chatScanSessionItemIds(
			_peer->id,
			int(Export::MediaSettings::Type::Sticker));
		for (const auto id : stickerIds) {
			_session->storage().remove(Storage::SharedMediaRemoveOne(
				_peer->id,
				MsgId(0),
				PeerId(0),
				stickerTypes,
				id));
		}
		if (db().cancelChatScanSession(_peer->id)) {
			ChatScan::ForgetScannedPeer(_peer->id);
			ChatScan::RememberScannedPeer(_peer->id);
		} else {
			LOG(("ChatScan: failed to cancel incremental session."));
		}
	} else if (_incremental) {
		LOG(("ChatScan: canceled before incremental session started."));
	} else if (_clearing) {
		LOG(("ChatScan: canceled while a full rescan clear is running."));
	} else {
		_clearing = true;
		const auto weak = base::weak_ptr<Api>(this);
		db().clearChatScanAsync(
			_peer->id,
			[weak](
					bool success,
					std::vector<Data::ChatScanOwnerChange> changed) {
				const auto strong = weak.get();
				if (!strong) {
					return;
				}
				if (!success) {
					LOG(("ChatScan: failed to clear full scan."));
				} else {
					const auto stickerTypes =
						Storage::SharedMediaTypesMask{}.added(
							Storage::SharedMediaType::Sticker);
					strong->_session->storage().remove(
						Storage::SharedMediaRemoveAll(
							strong->_peer->id,
							stickerTypes));
					ChatScan::ForgetScannedPeer(strong->_peer->id);
					for (const auto &[peerId, hash] : changed) {
						ChatScan::MarkUniqueHashDirty(peerId, hash);
						strong->db().invalidateChatTable(peerId);
					}
					strong->db().invalidateChatTable(
						strong->_peer->id);
					strong->db().invalidateAllPeersChatTable();
				}
				strong->_clearing = false;
				strong->db().clearChatTable();
				auto state = strong->_state.current();
				state.phase = State::Phase::Cancelled;
				state.elapsedMs = 0;
				strong->_state = std::move(state);
			});
	}
	_sessionId = 0;
	_incremental = false;
	auto state = _state.current();
	state.phase = _clearing
		? State::Phase::Cancelling
		: State::Phase::Cancelled;
	state.elapsedMs = 0;
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
	_pageStickers.clear();
	if (messages.isEmpty()) {
		finish();
		return;
	}
	auto minId = std::numeric_limits<int32>::max();
	auto maxId = int32(0);
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
		maxId = std::max(maxId, id);
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
	_session->storage().add(Storage::SharedMediaAddSlice(
		_peer->id,
		MsgId(0),
		PeerId(0),
		Storage::SharedMediaType::Sticker,
		std::move(_pageStickers),
		MsgRange(MsgId(minId), MsgId(maxId)),
		_stickerTotal));
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
				id);
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
		MsgId msgId) {
	const auto type = ScanMediaCategory(document);
	if (type == int(Export::MediaSettings::Type::Sticker)) {
		_pageStickers.push_back(msgId);
		++_stickerTotal;
	}
	const auto size = std::max(document->size, int64(0));
	const auto skipped = (size <= 0);
	auto item = Data::ChatScanItem();
	item.peerId = _peer->id;
	item.msgId = msgId;
	item.category = type;
	item.size = size;
	item.mediaId = document->id;
	item.isPhoto = false;
	item.filename = document->filename();
	item.sessionId = _sessionId;
	_documents[document->id] = document.get();
	if (!_seenDocs.insert(document->id).second) {
		// A second occurrence is never hashed on its own: one hash per media
		// id is enough, and staying out of the queue is what makes the
		// walker's outstanding counter match one lease per media id. The
		// representative's commit fills the rows that exist at that moment,
		// but the walk goes newest to oldest, so an older occurrence is
		// usually inserted long after that commit has run. Carrying the
		// already known hash over keeps this row resolvable on its own.
		item.state = skipped ? 4 : 2;
		item.isDup = true;
		if (!skipped) {
			item.hash = knownHashForMedia(item.mediaId, item.isPhoto);
		}
		_pageItems.push_back(item);
		auto &totals = _totals[type];
		totals.dupCount++;
		totals.dupBytes += size;
		return;
	}
	item.state = skipped ? 4 : 0;
	_pageItems.push_back(item);
	auto &totals = _totals[type];
	totals.uniqueCount++;
	totals.uniqueBytes += size;
	if (!skipped) {
		++_queueOutstanding;
	}
}

void Api::addPhotoOccurrence(
		not_null<PhotoData*> photo,
		MsgId msgId) {
	using Type = Export::MediaSettings::Type;
	auto size = int64(photo->imageByteSize(Data::PhotoSize::Large));
	if (!size) {
		size = photo->videoByteSize(Data::PhotoSize::Large);
	}
	size = std::max(size, int64(0));
	const auto skipped = (size <= 0);
	auto item = Data::ChatScanItem();
	item.peerId = _peer->id;
	item.msgId = msgId;
	item.category = int(Type::Photo);
	item.size = size;
	item.mediaId = photo->id;
	item.isPhoto = true;
	item.sessionId = _sessionId;
	_photos[photo->id] = photo.get();
	if (!_seenPhotos.insert(photo->id).second) {
		item.state = skipped ? 4 : 2;
		item.isDup = true;
		if (!skipped) {
			item.hash = knownHashForMedia(item.mediaId, item.isPhoto);
		}
		_pageItems.push_back(item);
		auto &totals = _totals[int(Type::Photo)];
		totals.dupCount++;
		totals.dupBytes += size;
		return;
	}
	item.state = skipped ? 4 : 0;
	_pageItems.push_back(item);
	auto &totals = _totals[int(Type::Photo)];
	totals.uniqueCount++;
	totals.uniqueBytes += size;
	if (!skipped) {
		++_queueOutstanding;
	}
}

void Api::commitPage() {
	auto &database = db();
	_queue->insert(_pageItems);
	_pageItems.clear();
	database.beginTransaction();
	database.writeScanCursor({
		.peerId = _peer->id,
		.cursor = MsgId(_offsetId),
		.phase = int(State::Phase::Scanning),
		.elapsedMs = elapsedMs(),
	});
	database.commitTransaction();
}

void Api::rescan() {
	if (_finished || _cancelled || _walkDone || _recovering || _rescan) {
		return;
	}
	const auto mode = db().chatScanMode(_peer->id);
	const auto current = GetEnhancedBool("prevent_upload_duplicates") ? 1 : 0;
	if (mode != current) {
		_incremental = false;
		start();
		return;
	}
	_incremental = true;
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
	const auto mark = db().chatScanScannedUpto(peerId).bare;
	if (!mark) {
		return;
	}
	const auto sessionId = uint64(
		QDateTime::currentMSecsSinceEpoch()) | 1;
	if (!db().beginChatScanSession(peerId, sessionId, MsgId(mark))) {
		LOG(("ChatScan: failed to begin incremental session."));
		finishBorrowedTakeout();
		return;
	}
	_sessionId = sessionId;
	_rescan = true;
	_stopMark = mark;
	_elapsedMs = 0;
	_elapsedStartedAt = 0;
	startElapsed();
	_postFinishRetryUsed = false;
	_queueOutstanding = _queue->pending() + _queue->leased();
	_failedHashes = _queue->failed();
	loadStickerIndexAsync();
	loadKnownHashes(nullptr);
	// Results that finished under the previous session still describe the same
	// rows, so they are stored before the unique-hash map is rebuilt.
	drainHashed(std::numeric_limits<int>::max());
	_uniqueHashes.clear();
	_rebuildAboveBare = 0;
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
	const auto batch = database.chatScanItemsWindow(
		peerId,
		MsgId(_rebuildAboveBare),
		kBatch);
	for (const auto &item : batch) {
		_rebuildAboveBare = int64(item.msgId.bare);
		restoreRebuiltItem(item);
	}
	if (int(batch.size()) == kBatch) {
		continueRebuildStep();
		return;
	}
	_highMark = 0;
	const auto weak = base::weak_ptr<Api>(this);
	_recomputingProgress = true;
	database.recomputeChatScanProgressAsync(
		peerId,
		[weak](
				std::vector<Data::ChatScanProgressRow> progress,
				bool success) {
			if (const auto strong = weak.get()) {
				strong->applyRebuildProgress(
					std::move(progress),
					success);
			}
		});
}

void Api::applyRebuildProgress(
		std::vector<Data::ChatScanProgressRow> progress,
		bool success) {
	_rebuilding = false;
	_recomputingProgress = false;
	if (_cancelled || _finished) {
		return;
	}
	if (!success) {
		if (_paused) {
			publish(true);
			return;
		}
		if (_failed) {
			return;
		}
		requestPage();
		maybeRequestRecovery();
		if (_recoveryDone && _recoveryWindowDone) {
			startScheduler();
		}
		return;
	}
	for (const auto &row : progress) {
		auto &totals = _totals[row.category];
		totals.uniqueCount = int(row.uniqueCount);
		totals.uniqueBytes = row.uniqueBytes;
		totals.dupCount = int(row.dupCount);
		totals.dupBytes = row.dupBytes;
	}
	if (_paused) {
		publish(true);
		return;
	}
	if (_failed) {
		return;
	}
	if (_queueOutstanding > 0) {
		_recoveryWindowDone = true;
		_recoveryDone = false;
		_recoveryAbove = int32(db().chatScanMaxMsgId(_peer->id).bare) + 1;
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
	const auto isDup = item.isDup;
	if (!item.hash.isEmpty()) {
		auto i = _uniqueHashes.find(item.hash);
		if (i == _uniqueHashes.end()) {
			_uniqueHashes.insert(
				item.hash,
				isDup
					? UniqueHash{ MsgId(0), 0, 0 }
					: UniqueHash{
						item.msgId,
						item.category,
						item.size });
		} else if (!isDup && i.value().msgId < item.msgId) {
			i.value() = UniqueHash{
				item.msgId,
				item.category,
				item.size,
			};
		}
	}
	auto &totals = _totals[item.category];
	if (isDup) {
		totals.dupCount++;
		totals.dupBytes += item.size;
	} else {
		totals.uniqueCount++;
		totals.uniqueBytes += item.size;
	}
	if (!item.mediaId) {
		return;
	}
	_queue->rememberHash(item.mediaId, item.isPhoto, item.hash);
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
	const auto mode = db().chatScanMode(_peer->id);
	const auto current = GetEnhancedBool("prevent_upload_duplicates") ? 1 : 0;
	if (mode != -1 && mode != current) {
		return false;
	}
	_paused = true;
	if (const auto session = db().chatScanSession(_peer->id)) {
		_sessionId = session->first;
		_stopMark = session->second.bare;
		_incremental = true;
		_rescan = true;
	}
	_elapsedMs = cursor.elapsedMs;
	_elapsedStartedAt = 0;
	++_resumeGen;
	const auto retried = _queue->retryFailed();
	if (retried > 0) {
		LOG(("ChatScan: retrying %1 failed items.").arg(retried));
	}
	requestMediaTotals();
	_floodTimer.cancel();
	_offsetId = int32(cursor.cursor.bare);
	_totals.clear();
	_stickerTotal = 0;
	_scanned = 0;
	_highMark = 0;
	auto &database = db();
	if (!database.isOpen()) {
		return false;
	}
	const auto weak = base::weak_ptr<Api>(this);
	_recomputingProgress = true;
	database.recomputeChatScanProgressAsync(
		_peer->id,
		[weak](
				std::vector<Data::ChatScanProgressRow> progress,
				bool success) {
			if (const auto strong = weak.get()) {
				strong->applyRecoveredProgress(
					std::move(progress),
					success);
			}
		});
	auto state = _state.current();
	state.phase = State::Phase::Paused;
	_state = std::move(state);
	publish(true);
	return true;
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
	const auto sticker = _totals.find(
		int(Export::MediaSettings::Type::Sticker));
	_stickerTotal = (sticker != _totals.end())
		? (sticker->second.uniqueCount + sticker->second.dupCount)
		: 0;
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

void Api::loadKnownHashes(Fn<void()> done) {
	const auto dedupOn = GetEnhancedBool("prevent_upload_duplicates");
	if (dedupOn) {
		_uploadTableUse = db().useTable(Data::DedupDb::Table::Uploads);
	} else {
		_uploadTableUse.reset();
	}
	const auto weak = base::weak_ptr<Api>(this);
	db().chatScanMediaHashesAsync(
		_peer->id,
		dedupOn,
		[
			weak,
			dedupOn,
			done = std::move(done)
		](std::vector<Data::ChatScanMediaHash> rows, bool success) mutable {
			const auto strong = weak.get();
			if (!strong || strong->_cancelled || strong->_finished) {
				return;
			}
			strong->_queue->setKnownHashes(
				std::move(rows),
				success,
				dedupOn && success);
			if (strong->_paused
				|| strong->_walkDone
				|| strong->_recovering) {
				return;
			}
			if (done) {
				done();
			}
		});
}

void Api::loadResumeSets(Fn<void()> done) {
	if (_cancelled || _finished || _loadingResumeSets) {
		return;
	}
	_loadingResumeSets = true;
	_resumeSetsPending = 3;
	_resumeSetsDone = std::move(done);
	_uniqueHashes.clear();
	_queue->clearKnownHashes();
	const auto dedupOn = GetEnhancedBool("prevent_upload_duplicates");
	if (dedupOn) {
		_uploadTableUse = db().useTable(Data::DedupDb::Table::Uploads);
	} else {
		_uploadTableUse.reset();
	}
	const auto weak = base::weak_ptr<Api>(this);
	db().chatScanUniqueHashesAsync(
		_peer->id,
		[weak](
				std::vector<std::pair<QByteArray, MsgId>> rows,
				bool success) {
			if (const auto strong = weak.get()) {
				strong->uniqueHashesLoaded(std::move(rows), success);
			}
		});
	db().chatScanMediaHashesAsync(
		_peer->id,
		dedupOn,
		[weak, dedupOn](
				std::vector<Data::ChatScanMediaHash> rows,
				bool success) {
			if (const auto strong = weak.get()) {
				if (strong->_cancelled || strong->_finished) {
					return;
				}
				strong->_queue->setKnownHashes(
					std::move(rows),
					success,
					dedupOn && success);
				strong->resumeSetLoaded();
			}
		});
	db().chatScanItemIdsAsync(
		_peer->id,
		int(Export::MediaSettings::Type::Sticker),
		[weak](std::vector<MsgId> ids, bool success) {
			if (const auto strong = weak.get()) {
				strong->stickerIndexLoaded(std::move(ids), success);
			}
		});
}

void Api::loadStickerIndexAsync() {
	const auto weak = base::weak_ptr<Api>(this);
	db().chatScanItemIdsAsync(
		_peer->id,
		int(Export::MediaSettings::Type::Sticker),
		[weak](std::vector<MsgId> ids, bool success) {
			if (const auto strong = weak.get()) {
				if (!strong->_cancelled
					&& !strong->_finished
					&& success) {
					strong->applyStickerIndex(std::move(ids));
				}
			}
		});
}

void Api::uniqueHashesLoaded(
		std::vector<std::pair<QByteArray, MsgId>> rows,
		bool success) {
	if (_cancelled || _finished) {
		return;
	}
	if (success) {
		_uniqueHashes.clear();
		for (const auto &[hash, msgId] : rows) {
			_uniqueHashes.insert(
				hash,
				UniqueHash{ msgId, 0, 0 });
		}
	} else {
		LOG(("ChatScan: failed to load unique hashes."));
	}
	resumeSetLoaded();
}

void Api::stickerIndexLoaded(
		std::vector<MsgId> ids,
		bool success) {
	if (_cancelled || _finished) {
		return;
	}
	if (success) {
		applyStickerIndex(std::move(ids));
	}
	resumeSetLoaded();
}

void Api::applyStickerIndex(std::vector<MsgId> ids) {
	_stickerTotal = int(ids.size());
	if (ids.empty()) {
		return;
	}
	auto minId = ids.front();
	auto maxId = ids.front();
	for (const auto id : ids) {
		minId = std::min(minId, id);
		maxId = std::max(maxId, id);
	}
	const auto count = int(ids.size());
	_session->storage().add(Storage::SharedMediaAddSlice(
		_peer->id,
		MsgId(0),
		PeerId(0),
		Storage::SharedMediaType::Sticker,
		std::move(ids),
		MsgRange(minId, maxId),
		count));
}

void Api::resumeSetLoaded() {
	if (_resumeSetsPending <= 0) {
		return;
	}
	if (--_resumeSetsPending > 0) {
		return;
	}
	_loadingResumeSets = false;
	auto done = std::move(_resumeSetsDone);
	_resumeSetsDone = nullptr;
	if (done) {
		done();
	}
}

void Api::finishResumeSets(bool seeded) {
	if (_cancelled || _finished) {
		return;
	}
	if (_paused) {
		auto state = _state.current();
		state.phase = State::Phase::Paused;
		_state = std::move(state);
		publish(true);
		return;
	}
	if (_failed) {
		return;
	}
	if (!seeded) {
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

void Api::rememberRestoredMedia(const Data::ChatScanItem &item) {
	if (!item.mediaId) {
		return;
	}
	_queue->rememberHash(item.mediaId, item.isPhoto, item.hash);
	if (item.isPhoto) {
		_seenPhotos.insert(item.mediaId);
	} else {
		_seenDocs.insert(item.mediaId);
	}
}

void Api::recoverFromCursor(const Data::ChatScanState &cursor) {
	stopScheduler();
	if (const auto session = db().chatScanSession(_peer->id)) {
		_sessionId = session->first;
		_stopMark = session->second.bare;
		_incremental = true;
		_rescan = true;
	}
	_elapsedMs = cursor.elapsedMs;
	_elapsedStartedAt = 0;
	startElapsed();
	_postFinishRetryUsed = false;
	const auto retried = _queue->retryFailed();
	if (retried > 0) {
		LOG(("ChatScan: retrying %1 failed items.").arg(retried));
	}
	_documents.clear();
	_photos.clear();
	_seenDocs.clear();
	_seenPhotos.clear();
	clearHashedBatch();
	_uniqueHashes.clear();
	_stickerTotal = 0;
	_staleMedia.clear();
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
	requestRecoveryProgress();
}

void Api::requestRecoveryProgress() {
	const auto weak = base::weak_ptr<Api>(this);
	_recomputingProgress = true;
	db().recomputeChatScanProgressAsync(
		_peer->id,
		[weak](
				std::vector<Data::ChatScanProgressRow> progress,
				bool success) {
			if (const auto strong = weak.get()) {
				strong->applyRecoveredProgress(
					std::move(progress),
					success);
			}
		});
}

void Api::applyRecoveredProgress(
		std::vector<Data::ChatScanProgressRow> progress,
		bool success) {
	_recomputingProgress = false;
	if (_cancelled || _finished) {
		return;
	}
	if (!success) {
		if (_paused) {
			publish(true);
			return;
		}
		if (_failed) {
			return;
		}
		requestPage();
		maybeRequestRecovery();
		if (_recoveryDone && _recoveryWindowDone) {
			startScheduler();
		}
		return;
	}
	const auto seeded = seedFromProgress(progress);
	if (_failed) {
		return;
	}
	if (!seeded) {
		finishResumeSets(false);
		return;
	}
	const auto weak = base::weak_ptr<Api>(this);
	loadResumeSets([weak] {
		if (const auto strong = weak.get()) {
			strong->finishResumeSets(true);
		}
	});
	if (_paused) {
		auto state = _state.current();
		state.phase = State::Phase::Paused;
		_state = std::move(state);
		publish(true);
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

int64 Api::elapsedMs() const {
	return _elapsedMs + (_elapsedStartedAt
		? (crl::now() - _elapsedStartedAt)
		: 0);
}

void Api::startElapsed() {
	if (!_elapsedStartedAt) {
		_elapsedStartedAt = crl::now();
	}
}

void Api::freezeElapsed() {
	if (_elapsedStartedAt) {
		_elapsedMs += crl::now() - _elapsedStartedAt;
		_elapsedStartedAt = 0;
	}
}

void Api::publish(bool force) {
	const auto now = crl::now();
	auto state = _state.current();
	if (!force && state.phase == _lastPhase && now - _lastPublishMs < 250) {
		return;
	}
	_lastPhase = state.phase;
	_lastPublishMs = now;
	const auto chunkUntil = _session->data().chunkFetcher().unblockTime();
	state.floodUntilMs = std::max(_floodWaitUntilMs, int64(chunkUntil));
	state.scannedCount = _scanned;
	state.categories = _totals;
	state.hashDone = _hashDone;
	state.hashPending = _queueOutstanding;
	state.mediaTotal = _mediaTotal;
	state.elapsedMs = elapsedMs();
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
	if (!_postFinishRetryUsed && _queue->failed() > 0) {
		_postFinishRetryUsed = true;
		if (_queue->retryFailed() > 0) {
			_failedHashes = 0;
			_queueOutstanding = _queue->pending() + _queue->leased();
			_scheduler->pump();
			publish();
			if (_queueOutstanding > 0) {
				return;
			}
		}
	}
	finalizeAndWrite();
}

void Api::demoteToDuplicate(const Data::ChatScanItem &item) {
	auto &totals = _totals[item.category];
	if (totals.uniqueCount > 0) {
		--totals.uniqueCount;
	}
	totals.uniqueBytes = std::max(int64(0), totals.uniqueBytes - item.size);
	++totals.dupCount;
	totals.dupBytes += item.size;
	db().markChatScanItemDup(
		_peer->id,
		item.msgId,
		true,
		item.filename);
}

QString Api::filenameForMedia(uint64 mediaId, bool isPhoto) const {
	if (isPhoto || !mediaId) {
		return QString();
	}
	const auto i = _documents.find(mediaId);
	if (i != _documents.end() && i->second) {
		return i->second->filename();
	}
	return _session->data().document(mediaId)->filename();
}

QByteArray Api::knownHashForMedia(uint64 mediaId, bool isPhoto) const {
	return _queue->knownHash(
		mediaId,
		isPhoto,
		GetEnhancedBool("prevent_upload_duplicates"));
}

void Api::applyOwnerDecision(
		const Data::ChatScanItem &item,
		const QByteArray &hash,
		const Data::ChatScanOwnerDecision &decision) {
	const auto demote = [&](const Data::ChatScanItem &row) {
		auto &totals = _totals[row.category];
		if (totals.uniqueCount > 0) {
			--totals.uniqueCount;
		}
		totals.uniqueBytes = std::max(
			int64(0),
			totals.uniqueBytes - row.size);
		++totals.dupCount;
		totals.dupBytes += row.size;
	};
	const auto promote = [&](const Data::ChatScanItem &row) {
		auto &totals = _totals[row.category];
		if (totals.dupCount > 0) {
			--totals.dupCount;
		}
		totals.dupBytes = std::max(
			int64(0),
			totals.dupBytes - row.size);
		++totals.uniqueCount;
		totals.uniqueBytes += row.size;
	};
	if (decision.demoted
		&& decision.demoted->peerId == _peer->id
		&& decision.demoted->msgId != item.msgId
		&& decision.demoted->tracked
		&& !decision.demoted->isDup) {
		demote(*decision.demoted);
	} else if (decision.demoted) {
		MarkUniqueHashDirty(decision.demoted->peerId, hash);
	}
	if (decision.wasDup != decision.isDup) {
		if (decision.isDup) {
			demote(item);
		} else {
			promote(item);
		}
	}
	if (decision.owner && decision.owner->peerId == _peer->id) {
		if (decision.owner->msgId == item.msgId) {
			_uniqueHashes[hash] = UniqueHash{
				item.msgId,
				item.category,
				item.size,
			};
		} else if (const auto owner = db().chatScanItem(
				_peer->id,
				decision.owner->msgId)) {
			_uniqueHashes[hash] = UniqueHash{
				owner->msgId,
				owner->category,
				owner->size,
			};
		} else {
			_uniqueHashes.remove(hash);
		}
	} else {
		_uniqueHashes.remove(hash);
	}
}

void Api::onHashed(
		const Data::ChatScanItem &item,
		const QByteArray &hash) {
	if (_cancelled || _failed || hash.isEmpty()) {
		return;
	}
	_queueOutstanding = std::max(0, _queueOutstanding - 1);
	_queue->rememberHash(item.mediaId, item.isPhoto, hash);
	// Owner election is a database session per finished file; it is deferred
	// to the commit batch so a response callback only appends here.
	_pendingHashed.push_back({ item, hash });
}

void Api::applyHashed(const PendingHash &entry) {
	const auto &item = entry.item;
	const auto &hash = entry.hash;
	const auto tierOn = GetEnhancedBool("prevent_upload_duplicates");
	auto i = _uniqueHashes.find(hash);
	const auto uniqueMsgId = (i != _uniqueHashes.end())
		? i.value().msgId
		: MsgId(0);
	if (TakeUniqueHashDirty(_peer->id, hash)
		|| (uniqueMsgId
			&& TakeUniqueHashDeleted(_peer->id, uniqueMsgId))) {
		const auto actual = db().chatScanUniqueHash(_peer->id, hash);
		if (!actual) {
			if (i != _uniqueHashes.end()) {
				_uniqueHashes.erase(i);
			}
			i = _uniqueHashes.end();
		} else if (i == _uniqueHashes.end()) {
			i = _uniqueHashes.insert(
				hash,
				UniqueHash{ actual, 0, 0 });
		} else {
			i.value() = UniqueHash{ actual, 0, 0 };
		}
	}
	if (!tierOn && item.mediaId) {
		db().insert(Data::DedupDb::Table::Uploads, {
			.hash = hash,
			.documentId = item.mediaId,
			.isPhoto = item.isPhoto,
			.status = u"f"_q,
		});
	}
	if (tierOn) {
		const auto decision = db().chatScanApplyHashOwner(
			_peer->id,
			item.msgId,
			item.mediaId,
			item.isPhoto,
			hash,
			item.isDup,
			filenameForMedia(item.mediaId, item.isPhoto));
		if (decision) {
			applyOwnerDecision(item, hash, *decision);
		} else {
			LOG(("ChatScan: failed to apply hash owner for message %1.")
				.arg(item.msgId.bare));
		}
	} else if (i == _uniqueHashes.end()) {
		_uniqueHashes.insert(
			hash,
			UniqueHash{ item.msgId, item.category, item.size });
	} else if (item.msgId == i.value().msgId) {
		// Already the stored unique hash owner.
	} else if (!i.value().msgId || item.msgId < i.value().msgId) {
		demoteToDuplicate(item);
	} else {
		auto previous = db().chatScanItem(_peer->id, i.value().msgId);
		if (previous && !previous->isDup) {
			previous->filename = filenameForMedia(
				previous->mediaId,
				previous->isPhoto);
			demoteToDuplicate(*previous);
		}
		i.value() = UniqueHash{
			item.msgId,
			item.category,
			item.size,
		};
	}
}

void Api::applyHashedBatch() {
	if (_pendingHashedPos >= int(_pendingHashed.size())) {
		return;
	}
	if (_cancelled || _failed) {
		clearHashedBatch();
		return;
	}
	if (_drainScheduled) {
		return;
	}
	_drainScheduled = true;
	drainHashed(kApplyTurnBudgetMs);
}

void Api::drainHashed(int budgetMs) {
	if (_cancelled || _failed) {
		clearHashedBatch();
		return;
	}
	if (_pendingHashedPos >= int(_pendingHashed.size())) {
		clearHashedBatch();
		return;
	}
	const auto started = crl::now();
	const auto appliedFrom = _pendingHashedPos;
	// One transaction per turn: every applyHashed() session nests into it
	// instead of committing on its own, so the write-ahead log does not grow
	// once per file and its checkpoints stay small and rare.
	db().beginTransaction();
	while (_pendingHashedPos < int(_pendingHashed.size())) {
		applyHashed(_pendingHashed[_pendingHashedPos++]);
		if (crl::now() - started >= budgetMs) {
			break;
		}
	}
	db().commitTransaction();
	const auto elapsed = crl::now() - started;
	if (elapsed > 40) {
		LOG(("ChatScan: applying %1 hashes took %2 ms."
			).arg(_pendingHashedPos - appliedFrom).arg(elapsed));
	}
	publish();
	if (_pendingHashedPos < int(_pendingHashed.size())) {
		// The rest waits for the next loop turn: a turn longer than the
		// throttle's burst costs the rate the whole scan is tuned for.
		const auto weak = base::weak_ptr<Api>(this);
		crl::on_main([weak] {
			if (const auto strong = weak.get()) {
				strong->drainHashed(kApplyTurnBudgetMs);
			}
		});
		return;
	}
	clearHashedBatch();
}

void Api::clearHashedBatch() {
	_drainScheduled = false;
	_pendingHashed.clear();
	_pendingHashedPos = 0;
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
	applyHashedBatch();
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
	freezeElapsed();
	{
		auto state = _state.current();
		state.hashPending = 0;
		state.elapsedMs = _elapsedMs;
		_state = std::move(state);
	}
	if (_rescan && !_walkedNew) {
		_aggregating = false;
		stopScheduler();
		finishBorrowedTakeout();
		db().writeChatScanMode(
			_peer->id,
			GetEnhancedBool("prevent_upload_duplicates") ? 1 : 0);
		db().finishChatScanSession(_peer->id);
		_sessionId = 0;
		_finished = true;
		auto state = _state.current();
		state.phase = State::Phase::Finished;
		_state = std::move(state);
		return;
	}
	stopScheduler();
	_scheduler->flush();
	// Everything finished must be applied before the aggregates are written,
	// so this turn ignores the budget the normal drain uses.
	if (!_pendingHashed.empty()) {
		drainHashed(std::numeric_limits<int>::max());
	}
	finishBorrowedTakeout();
	writeAggregateRows();
}

void Api::writeAggregateRows() {
	auto &database = db();
	const auto peerId = _peer->id;

	auto dupTotal = 0;
	auto dupBytes = int64(0);
	for (const auto &[category, t] : _totals) {
		dupTotal += t.dupCount;
		dupBytes += t.dupBytes;
	}
	database.beginTransaction();
	database.finishChatScan(
		peerId,
		MsgId(_highMark),
		GetEnhancedBool("prevent_upload_duplicates") ? 1 : 0,
		(dupTotal == 0),
		_elapsedMs);
	database.commitTransaction();
	database.finishChatScanSession(peerId);
	_sessionId = 0;

	stopScheduler();
	_documents.clear();
	_photos.clear();
	_seenDocs.clear();
	_seenPhotos.clear();
	_uniqueHashes.clear();
	_queue->clearKnownHashes();
	db().clearChatTable();
	db().invalidateChatTable(_peer->id);
	db().invalidateAllPeersChatTable();
	_uploadTableUse.reset();
	_stickerTotal = 0;
	_staleMedia.clear();
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
	auto state = _state.current();
	state.phase = State::Phase::Finished;
	state.categories = _totals;
	state.dupTotal = dupTotal;
	state.dupBytes = dupBytes;
	state.hashDone = _hashDone;
	state.hashPending = 0;
	state.hashTotal = _hashDone + _failedHashes;
	state.elapsedMs = _elapsedMs;
	_state = std::move(state);
	ChatScan::ForgetScannedPeer(peerId);
	ChatScan::RememberScannedPeer(peerId);
}

Data::DedupDb &Api::db() const {
	return Core::App().downloadManager().ensureDedupDb();
}

} // namespace ChatScan
