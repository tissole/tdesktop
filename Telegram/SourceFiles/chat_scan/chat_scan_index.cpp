/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_scan/chat_scan_index.h"

#include "core/application.h"
#include "data/data_dedup_db.h"
#include "data/data_document.h"
#include "data/data_download_manager.h"
#include "data/data_media_category.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "export/export_settings.h"
#include "history/history.h"
#include "history/history_item.h"

#include <set>

namespace ChatScan {
namespace {

using Type = Export::MediaSettings::Type;

std::set<uint64> &ScannedPeers() {
	static auto peers = std::set<uint64>();
	return peers;
}

void EnsureScanned() {
	static auto loaded = false;
	if (loaded) {
		return;
	}
	loaded = true;
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	auto &set = ScannedPeers();
	for (const auto peer : db.scannedPeerIds()) {
		set.insert(peer.value);
	}
}

bool IsScanned(PeerId peerId) {
	EnsureScanned();
	return ScannedPeers().contains(peerId.value);
}

struct SizedMedia {
	int category = -1;
	int64 size = 0;
};

SizedMedia Classify(not_null<HistoryItem*> item) {
	if (const auto media = item->media()) {
		if (const auto document = media->document()) {
		 auto size = document->size;
			if (!size && document->uploadingData) {
				size = document->uploadingData->size;
			}
			return {
				int(::DocumentMediaCategory(document)),
				size,
			};
		}
		if (const auto photo = media->photo()) {
			auto size = int64(photo->imageByteSize(Data::PhotoSize::Large));
			if (!size) {
				size = photo->videoByteSize(Data::PhotoSize::Large);
			}
			if (!size && photo->uploadingData) {
				size = photo->uploadingData->size;
			}
			return { int(Type::Photo), size };
		}
	}
	return {};
}

QString ItemFilename(not_null<HistoryItem*> item) {
	if (const auto media = item->media()) {
		if (const auto document = media->document()) {
			return document->filename();
		}
	}
	return QString();
}

uint64 ItemMediaId(not_null<HistoryItem*> item, bool &isPhoto) {
	if (const auto media = item->media()) {
		if (const auto document = media->document()) {
			isPhoto = false;
			return document->id;
		}
		if (const auto photo = media->photo()) {
			isPhoto = true;
			return photo->id;
		}
	}
	return 0;
}

void WriteLiveItem(not_null<HistoryItem*> item, const SizedMedia &sized) {
	auto itemRow = Data::ChatScanItem();
	const auto history = item->history();
	itemRow.peerId = history->peer->id;
	itemRow.msgId = item->id;
	itemRow.category = sized.category;
	itemRow.size = sized.size;
	itemRow.mediaId = ItemMediaId(item, itemRow.isPhoto);
	itemRow.filename = ItemFilename(item);
	auto &db = Core::App().downloadManager().ensureDedupDb();
	db.writeChatScanItems({ itemRow });
}

} // namespace

rpl::variable<int> &ScanVersion() {
	static auto version = rpl::variable<int>(0);
	return version;
}

void BumpScanVersion() {
	ScanVersion() = ScanVersion().current() + 1;
}

void ForgetScannedPeer(PeerId peerId) {
	ScannedPeers().erase(peerId.value);
	BumpScanVersion();
}

void RememberScannedPeer(PeerId peerId) {
	ScannedPeers().insert(peerId.value);
}

rpl::producer<int> ScanSizeVersion() {
	return ScanVersion().value();
}

int64 ScanSizeBytes(PeerId peerId, int category) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return 0;
	}
	for (const auto &row : db.readChatScan(peerId)) {
		if (row.category == category) {
			return row.uniqueBytes;
		}
	}
	return 0;
}

ScanTotalMedia TotalScannedMedia(PeerId peerId) {
	auto result = ScanTotalMedia();
	if (!IsScanned(peerId)) {
		return result;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return result;
	}
	for (const auto &row : db.readChatScan(peerId)) {
		result.files += row.uniqueCount;
		result.bytes += row.uniqueBytes;
	}
	result.files += db.chatScanDupCount(peerId, -1);
	result.bytes += db.chatScanDupBytes(peerId);
	return result;
}

void PromoteSuccessor(
		Data::DedupDb &db,
		PeerId peerId,
		MsgId deletedId,
		const std::optional<Data::ChatScanItem> &stored,
		int category) {
	if (!stored
		|| (stored->hash.isEmpty() && !stored->mediaId)) {
		return;
	}
	auto best = int64(0);
	for (const auto &other : db.readChatScanItems(peerId)) {
		if (other.msgId.bare == deletedId.bare) {
			continue;
		}
		const auto same = stored->hash.isEmpty()
			? (other.mediaId && other.mediaId == stored->mediaId)
			: (other.hash == stored->hash);
		if (same && other.msgId.bare > best) {
			best = other.msgId.bare;
		}
	}
	if (!best) {
		return;
	}
	const auto survivor = db.takeChatScanDup(peerId, MsgId(best));
	if (!survivor) {
		return;
	}
	const auto rows = db.readChatScan(peerId);
	for (const auto &row : rows) {
		if (row.category != category) {
			continue;
		}
		auto updated = row;
		if (updated.dupCount) {
			updated.dupCount--;
		}
		updated.dupBytes = std::max(int64(0), updated.dupBytes - survivor->size);
		updated.uniqueCount++;
		updated.uniqueBytes += survivor->size;
		db.writeChatScanRow(updated);
		BumpScanVersion();
		return;
	}
}

void NoteItemAdded(not_null<HistoryItem*> item) {
	const auto history = item->history();
	const auto peerId = history->peer->id;
	if (!IsScanned(peerId)) {
		return;
	}
	const auto sized = Classify(item);
	if (sized.category < 0) {
		return;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	const auto rows = db.readChatScan(peerId);
	auto maxUpto = int64(0);
	for (const auto &row : rows) {
		maxUpto = std::max(maxUpto, row.scannedUpto.bare);
	}
	if (db.hasChatScanItem(peerId, item->id)) {
		return;
	}
	for (const auto &row : rows) {
		if (row.category != sized.category) {
			continue;
		}
		if (item->id.bare <= row.scannedUpto.bare) {
			return;
		}
		WriteLiveItem(item, sized);
		auto updated = row;
		updated.uniqueCount++;
		updated.uniqueBytes += sized.size;
		db.writeChatScanRow(updated);
		BumpScanVersion();
		return;
	}
	WriteLiveItem(item, sized);
	auto fresh = Data::ChatScanRow();
	fresh.peerId = peerId;
	fresh.category = sized.category;
	fresh.uniqueCount = 1;
	fresh.uniqueBytes = sized.size;
	fresh.scannedUpto = MsgId(maxUpto);
	fresh.scanFinished = true;
	db.writeChatScanRow(fresh);
	BumpScanVersion();
}

void NoteItemRemoved(not_null<HistoryItem*> item) {
	if (!item->isHistoryEntry()) {
		return;
	}
	const auto history = item->history();
	const auto peerId = history->peer->id;
	if (!IsScanned(peerId)) {
		return;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	const auto stored = db.chatScanItem(peerId, item->id);
	db.removeChatScanItem(peerId, item->id);
	// The shared items row is the one-shot token: the first window or
	// flow that sees the removal consumed it and owns the accounting,
	// so a second window (the same chat viewed from another account)
	// must not adjust the row again.
	if (!stored) {
		return;
	}
	const auto sized = Classify(item);
	if (sized.category < 0) {
		return;
	}
	const auto adjust = [&](bool dups, int64 size, int category) {
		const auto rows = db.readChatScan(peerId);
		for (const auto &row : rows) {
			if (row.category != category) {
				continue;
			}
			auto updated = row;
			if (dups) {
				if (!updated.dupCount) {
					return;
				}
				updated.dupCount--;
				updated.dupBytes = std::max(
					int64(0),
					updated.dupBytes - size);
			} else {
				if (!updated.uniqueCount) {
					return;
				}
				updated.uniqueCount--;
				updated.uniqueBytes = std::max(
					int64(0),
					updated.uniqueBytes - size);
			}
			db.writeChatScanRow(updated);
			BumpScanVersion();
			return;
		}
	};
	if (const auto dup = db.takeChatScanDup(peerId, item->id)) {
		adjust(true, dup->size, dup->category);
		return;
	}
	adjust(false, sized.size, sized.category);
	PromoteSuccessor(db, peerId, item->id, stored, sized.category);
}

void NoteItemMediaUpdated(not_null<HistoryItem*> item) {
	if (!item->isHistoryEntry()) {
		return;
	}
	const auto history = item->history();
	const auto peerId = history->peer->id;
	if (!IsScanned(peerId)) {
		return;
	}
	const auto sized = Classify(item);
	if (sized.category < 0) {
		return;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	const auto stored = db.chatScanItem(peerId, item->id);
	if (!stored) {
		WriteLiveItem(item, sized);
		const auto rows = db.readChatScan(peerId);
		for (const auto &row : rows) {
			if (row.category != sized.category) {
				continue;
			}
			auto updated = row;
			updated.uniqueCount++;
			updated.uniqueBytes += sized.size;
			db.writeChatScanRow(updated);
			BumpScanVersion();
			return;
		}
		return;
	}
	if (stored->category == sized.category && stored->size == sized.size) {
		return;
	}
	auto fresh = *stored;
	fresh.category = sized.category;
	fresh.size = sized.size;
	fresh.mediaId = ItemMediaId(item, fresh.isPhoto);
	fresh.filename = ItemFilename(item);
	db.writeChatScanItems({ fresh });
	const auto rows = db.readChatScan(peerId);
	for (const auto &row : rows) {
		if (row.category != stored->category
			&& row.category != sized.category) {
			continue;
		}
		auto updated = row;
		if (row.category == stored->category) {
			if (updated.uniqueCount) {
				updated.uniqueCount--;
			}
			updated.uniqueBytes = std::max(
				int64(0),
				updated.uniqueBytes - stored->size);
		}
		if (row.category == sized.category) {
			updated.uniqueCount++;
			updated.uniqueBytes += sized.size;
		}
		db.writeChatScanRow(updated);
	}
	BumpScanVersion();
}

} // namespace ChatScan
