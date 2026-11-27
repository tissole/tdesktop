/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk_forward/bulk_forward_forwarder.h"

#include "apiwrap.h"
#include "base/algorithm.h"
#include "base/debug_log.h"
#include "base/random.h"
#include "core/application.h"
#include "data/data_dedup_db.h"
#include "data/data_download_manager.h"
#include "data/data_file_hash.h"
#include "data/data_forward_dedup.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "enhanced_forward.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"
#include "scheme.h"
#include "settings.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace BulkForward {
namespace {

constexpr auto kMaxSampleRequests = 4;

constexpr auto kPending = uchar(0);
constexpr auto kApproved = uchar(1);
constexpr auto kSkipped = uchar(2);

[[nodiscard]] int IntId(MsgId id) {
	return int(std::clamp<int64>(
		id.bare,
		int64(0),
		int64(std::numeric_limits<int>::max())));
}

[[nodiscard]] base::flat_map<uint64, MsgId> CreatedIdsFromUpdates(
		const MTPUpdates &updates) {
	auto result = base::flat_map<uint64, MsgId>();
	const auto scan = [&](const MTPVector<MTPUpdate> &list) {
		for (const auto &update : list.v) {
			update.match([&](const MTPDupdateMessageID &data) {
				result.emplace(
					data.vrandom_id().v,
					MsgId(data.vid().v));
			}, [](const auto &) {
			});
		}
	};
	updates.match([&](const MTPDupdates &data) {
		scan(data.vupdates());
	}, [&](const MTPDupdatesCombined &data) {
		scan(data.vupdates());
	}, [](const auto &) {
	});
	return result;
}

} // namespace

Forwarder::Forwarder(
	not_null<Main::Session*> session,
	not_null<PeerData*> destination,
	MsgId topicRootId,
	std::shared_ptr<RunSeen> runSeen,
	int sent,
	int skipped)
: _session(session)
, _destination(destination)
, _topicRootId(topicRootId)
, _runSeen(std::move(runSeen))
, _sent(sent)
, _skipped(skipped) {
}

Forwarder::~Forwarder() {
	cancel();
}

PeerId Forwarder::destinationId() const {
	return _destination->id;
}

Forwarder::PendingInfo Forwarder::pendingInfo() const {
	auto result = PendingInfo();
	const auto add = [&](const SourceMessage &item) {
		const auto i = result.from.find(item.id.peer);
		if (i == end(result.from) || i->second > item.id.msg) {
			result.from[item.id.peer] = item.id.msg;
		}
		++result.count;
	};
	for (const auto &batch : _queue) {
		for (const auto &item : batch.items) {
			add(item);
		}
	}
	for (auto i = 0; i != int(_current.items.size()); ++i) {
		if (i < int(_decisions.size()) && _decisions[i] == kSkipped) {
			continue;
		}
		add(_current.items[i]);
	}
	return result;
}

void Forwarder::enqueue(Batch batch) {
	if (_cancelled || batch.items.empty()) {
		return;
	}
	_busy = true;
	_queue.push_back(std::move(batch));
	pump();
}

void Forwarder::cancel() {
	_cancelled = true;
	if (_requestId) {
		_session->api().request(base::take(_requestId)).cancel();
	}
	releasePending();
	clearPrepared();
	_seenMedia.clear();
	_seenHashes.clear();
	_sending = false;
	_busy = false;
	_paused = false;
	_inflight = 0;
	_queue.clear();
	progress = nullptr;
	drained = nullptr;
	unhandled = nullptr;
	failed = nullptr;
}

void Forwarder::pause() {
	_paused = true;
}

void Forwarder::resume() {
	if (!_paused) {
		return;
	}
	_paused = false;
	pump();
}

void Forwarder::pump() {
	if (_cancelled || _paused || _sending) {
		return;
	}
	while (true) {
		if (!_preparing) {
			if (_queue.empty()) {
				if (std::exchange(_busy, false) && drained) {
					drained();
				}
				return;
			}
			beginPrepare();
			if (!_preparing) {
				continue;
			}
		}
		if (!scanPrepared()) {
			return;
		}
		if (flushPrepared()) {
			return;
		}
		_preparing = false;
	}
}

void Forwarder::beginPrepare() {
	_current = std::move(_queue.front());
	_queue.pop_front();
	if (_current.items.empty()) {
		_current = Batch();
		return;
	}
	const auto source = _session->data().peerLoaded(_current.sourcePeer);
	const auto protectedSource = source
		&& EnhancedForward::PeerNeedsEnhancedForward(source);
	if (_current.isProtected || protectedSource) {
		if (unhandled) {
			unhandled(std::move(_current));
		}
		_current = Batch();
		return;
	}
	_decisions.assign(_current.items.size(), kPending);
	_hashes.assign(_current.items.size(), QByteArray());
	_scanIndex = 0;
	_inflight = 0;
	_preparing = true;
}

bool Forwarder::scanPrepared() {
	const auto context = Data::ForwardDedup::Context{
		.destination = _destination->id,
		.globalOn = GetEnhancedBool("prevent_forward_duplicates"),
		.run = _runSeen.get(),
		.checkFwTmp = true,
		.sessionId = _session->uniqueId(),
	};
	const auto count = int(_current.items.size());
	while (_scanIndex < count) {
		const auto index = _scanIndex;
		const auto &item = _current.items[index];
		const auto mediaId = item.mediaId;
		const auto isPhoto = item.isPhoto;
		if (!mediaId) {
			approve(index, QByteArray());
			++_scanIndex;
			continue;
		}
		const auto key = std::pair<uint64, bool>(mediaId, isPhoto);
		const auto duplicate = _seenMedia.contains(key)
			|| (Data::ForwardDedup::CheckMedia(
				context,
				mediaId,
				isPhoto,
				false) != Data::ForwardDedup::Verdict::Allowed);
		if (duplicate) {
			skip(index);
			++_scanIndex;
			continue;
		}
		if (item.sample.size <= 0) {
			approve(index, QByteArray());
			++_scanIndex;
			continue;
		} else if (_inflight >= kMaxSampleRequests) {
			return false;
		}
		startSample(index);
		++_scanIndex;
	}
	return (_inflight == 0);
}

void Forwarder::startSample(int index) {
	const auto item = _current.items[index];
	if (item.mediaId) {
		_seenMedia.insert({ item.mediaId, item.isPhoto });
	}
	++_inflight;
	const auto weak = base::make_weak(this);
	Data::RemoteSampleFingerprint(
		_session,
		item.sample,
		[weak, index](QByteArray hash) {
			if (const auto strong = weak.get()) {
				strong->sampleResolved(index, std::move(hash));
			}
		});
}

void Forwarder::sampleResolved(int index, QByteArray hash) {
	if (_inflight > 0) {
		--_inflight;
	}
	if (_cancelled || !_preparing || index >= int(_current.items.size())) {
		return;
	}
	const auto context = Data::ForwardDedup::Context{
		.destination = _destination->id,
		.globalOn = GetEnhancedBool("prevent_forward_duplicates"),
		.run = _runSeen.get(),
		.checkFwTmp = true,
		.sessionId = _session->uniqueId(),
	};
	const auto duplicate = !hash.isEmpty()
		&& (Data::ForwardDedup::CheckHash(
			context,
			hash,
			_seenHashes.contains(hash))
			!= Data::ForwardDedup::Verdict::Allowed);
	if (duplicate) {
		skip(index);
	} else {
		approve(index, std::move(hash));
	}
	pump();
}

void Forwarder::approve(int index, QByteArray hash) {
	_decisions[index] = kApproved;
	auto &stored = _hashes[index] = std::move(hash);
	const auto &item = _current.items[index];
	if (item.mediaId) {
		const auto key = std::pair<uint64, bool>(item.mediaId, item.isPhoto);
		_seenMedia.insert(key);
		if (_runSeen) {
			_runSeen->media.insert(key);
		}
	}
	if (stored.isEmpty() || !item.mediaId) {
		return;
	}
	_seenHashes.insert(stored);
	if (_runSeen) {
		_runSeen->hashes.insert(stored);
	}
	Data::ForwardDedup::AddPending(item.mediaId, item.isPhoto, stored);
	_pendingKeys.emplace_back(item.mediaId, item.isPhoto);
}

void Forwarder::skip(int index) {
	_decisions[index] = kSkipped;
	++_skipped;
}

void Forwarder::forget(const SourceMessage &item, const QByteArray &hash) {
	if (item.mediaId) {
		const auto key = std::pair<uint64, bool>(item.mediaId, item.isPhoto);
		_seenMedia.erase(key);
		if (_runSeen) {
			_runSeen->media.erase(key);
		}
	}
	if (!hash.isEmpty()) {
		_seenHashes.erase(hash);
		if (_runSeen) {
			_runSeen->hashes.erase(hash);
		}
	}
}

void Forwarder::handoffRefused(
		PeerId sourcePeer,
		std::vector<SourceMessage> items) {
	if (items.empty()) {
		return;
	}
	const auto source = _session->data().peerLoaded(sourcePeer);
	if (source) {
		EnhancedForward::MarkPeerProtectedByError(source);
	}
	auto batch = Batch();
	batch.sourcePeer = sourcePeer;
	batch.isProtected = true;
	batch.items = std::move(items);
	if (unhandled) {
		unhandled(std::move(batch));
	}
}

bool Forwarder::flushPrepared() {
	const auto count = int(_decisions.size());
	_approved.clear();
	_approvedHashes.clear();
	_approved.reserve(count);
	_approvedHashes.reserve(count);
	for (auto i = 0; i != count; ++i) {
		if (_decisions[i] != kApproved) {
			continue;
		}
		_approved.push_back(_current.items[i]);
		_approvedHashes.push_back(_hashes[i]);
	}
	_preparing = false;
	if (_approved.empty()) {
		return false;
	}
	return sendApproved();
}

bool Forwarder::sendApproved() {
	if (_approved.empty()) {
		return false;
	}
	const auto source = _session->data().peerLoaded(_current.sourcePeer);
	if (!source) {
		releasePending();
		_approved.clear();
		_approvedHashes.clear();
		return false;
	}
	auto ids = QVector<MTPint>();
	auto randomIds = QVector<MTPlong>();
	ids.reserve(int(_approved.size()));
	randomIds.reserve(int(_approved.size()));
	_approvedRandomIds.clear();
	_approvedRandomIds.reserve(_approved.size());
	for (const auto &item : _approved) {
		const auto randomId = base::RandomValue<uint64>();
		ids.push_back(MTP_int(IntId(item.id.msg)));
		randomIds.push_back(MTP_long(randomId));
		_approvedRandomIds.push_back(randomId);
	}

	_sending = true;
	const auto weak = base::make_weak(this);
	_requestId = _session->api().request(MTPmessages_ForwardMessages(
		MTP_flags(0),
		source->input(),
		MTP_vector<MTPint>(ids),
		MTP_vector<MTPlong>(randomIds),
		_destination->input(),
		MTP_int(IntId(_topicRootId)),
		MTPInputReplyTo(),
		MTP_int(0),
		MTP_int(0),
		MTP_inputPeerEmpty(),
		MTPInputQuickReplyShortcut(),
		MTP_long(0),
		MTPint(),
		MTP_long(0),
		MTPSuggestedPost()
	)).done([weak](const MTPUpdates &result) {
		if (const auto strong = weak.get()) {
			strong->handleSuccess(result);
		}
	}).fail([weak](const MTP::Error &error) {
		if (const auto strong = weak.get()) {
			strong->handleFailure(error.type());
		}
	}).handleFloodErrors().send();
	return true;
}

bool Forwarder::dropMissingApproved() {
	auto kept = std::vector<SourceMessage>();
	auto keptHashes = std::vector<QByteArray>();
	kept.reserve(_approved.size());
	keptHashes.reserve(_approvedHashes.size());
	auto dropped = 0;
	for (auto i = size_t(0); i != _approved.size(); ++i) {
		if (_session->data().message(_approved[i].id)) {
			kept.push_back(std::move(_approved[i]));
			keptHashes.push_back(std::move(_approvedHashes[i]));
		} else {
			++dropped;
		}
	}
	if (!dropped) {
		return false;
	}
	_failed += dropped;
	_approved = std::move(kept);
	_approvedHashes = std::move(keptHashes);
	return true;
}

void Forwarder::handleSuccess(const MTPUpdates &result) {
	_requestId = 0;
	_sending = false;
	const auto tooLong = (result.type() == mtpc_updatesTooLong);
	const auto createdIds = tooLong
		? base::flat_map<uint64, MsgId>()
		: CreatedIdsFromUpdates(result);
	auto refused = std::vector<SourceMessage>();
	auto createdCount = 0;
	auto &db = Core::App().downloadManager().ensureDedupDb();
	for (auto i = size_t(0); i != _approved.size(); ++i) {
		const auto &item = _approved[i];
		const auto &hash = _approvedHashes[i];
		const auto randomId = (i < _approvedRandomIds.size())
			? _approvedRandomIds[i]
			: uint64(0);
		const auto found = createdIds.find(randomId);
		const auto created = tooLong
			|| (found != end(createdIds) && found->second);
		if (!created) {
			forget(item, hash);
			refused.push_back(item);
			continue;
		}
		++createdCount;
		if (hash.isEmpty() || !item.mediaId || !db.isOpen()) {
			continue;
		}
		if (found != end(createdIds) && found->second) {
			// The created id proves the message exists in this destination,
			// so the enhanced path can trust its destination record even
			// when the global uploads row came from a refused forward.
			Data::ForwardDedup::RecordHash(
				item.mediaId,
				item.isPhoto,
				hash,
				_destination->id,
				found->second);
		} else {
			db.insert(Data::DedupDb::Table::Uploads, {
				.hash = hash,
				.documentId = item.mediaId,
				.isPhoto = item.isPhoto,
				.status = u"f"_q,
			});
		}
	}
	LOG(("BulkForward: Forward result, destination %1, approved %2, "
		"created %3, refused %4")
		.arg(_destination->id.value)
		.arg(int(_approved.size()))
		.arg(createdCount)
		.arg(int(refused.size())));
	const auto sourcePeer = _current.sourcePeer;
	releasePending();
	_sent += createdCount;
	const auto notify = progress;
	clearPrepared();
	handoffRefused(sourcePeer, std::move(refused));
	if (notify) {
		notify(_sent);
	}
	_session->api().applyUpdates(result);
	pump();
}

void Forwarder::handleFailure(const QString &error) {
	_requestId = 0;
	_sending = false;
	if (error == u"CHAT_FORWARDS_RESTRICTED"_q) {
		for (auto i = size_t(0); i != _approved.size(); ++i) {
			forget(_approved[i], _approvedHashes[i]);
		}
		releasePending();
		LOG(("BulkForward: Forward restricted, destination %1, items %2")
			.arg(_destination->id.value)
			.arg(int(_approved.size())));
		auto refused = std::move(_approved);
		const auto sourcePeer = _current.sourcePeer;
		clearPrepared();
		handoffRefused(sourcePeer, std::move(refused));
		pump();
		return;
	}
	releasePending();
	// A message can disappear between the scan and the send. The server
	// rejects the whole request without saying which id is gone, so the
	// batch is sent again without the ids that no longer exist locally.
	if ((error == u"MESSAGE_ID_INVALID"_q || error == u"MESSAGE_IDS_EMPTY"_q)
		&& dropMissingApproved()) {
		if (sendApproved()) {
			return;
		}
		const auto notify = progress;
		clearPrepared();
		if (notify) {
			notify(_sent);
		}
		pump();
		return;
	}
	clearPrepared();
	const auto callback = failed;
	if (callback) {
		callback(error);
	}
}

void Forwarder::clearPrepared() {
	_preparing = false;
	_current = Batch();
	_decisions.clear();
	_hashes.clear();
	_approved.clear();
	_approvedHashes.clear();
	_approvedRandomIds.clear();
	_scanIndex = 0;
}

void Forwarder::releasePending() {
	if (_pendingKeys.empty()) {
		return;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		for (const auto &[mediaId, isPhoto] : _pendingKeys) {
			db.removePending(
				Data::DedupDb::Table::Uploads,
				mediaId,
				isPhoto);
		}
	}
	_pendingKeys.clear();
}

} // namespace BulkForward
