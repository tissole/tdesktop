/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_scan/chat_scan_index.h"

#include "base/weak_ptr.h"
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
#include "main/main_session.h"
#include "storage/storage_facade.h"
#include "storage/storage_shared_media.h"

#include <QByteArray>
#include <algorithm>
#include <set>
#include <utility>

namespace ChatScan {
namespace {

using Type = Export::MediaSettings::Type;

// The stored per-category totals are adjusted here, where the row before and
// after a change is already known, so the scan never has to recount and the
// live paths never have to read anything extra.
void AdjustTotals(
		Data::DedupDb &db,
		PeerId peerId,
		int category,
		int64 size,
		bool isDup,
		int sign) {
	db.adjustChatScanTotals(
		peerId,
		category,
		isDup ? 0 : sign,
		isDup ? 0 : sign * size,
		isDup ? sign : 0,
		isDup ? sign * size : 0);
}

void AdjustTotals(
		Data::DedupDb &db,
		const Data::ChatScanItem &row,
		bool isDup,
		int sign) {
	AdjustTotals(db, row.peerId, row.category, row.size, isDup, sign);
}

std::set<uint64> &ScannedPeers() {
	static auto peers = std::set<uint64>();
	return peers;
}

std::set<std::pair<uint64, QByteArray>> &DirtyUniqueHashes() {
	static auto hashes = std::set<std::pair<uint64, QByteArray>>();
	return hashes;
}

std::set<std::pair<uint64, int64>> &DeletedUniqueHashes() {
	static auto ids = std::set<std::pair<uint64, int64>>();
	return ids;
}

std::set<uint64> &LoadedStickerIndexPeers() {
	static auto peers = std::set<uint64>();
	return peers;
}

template <typename Set>
void ErasePeerEntries(Set &set, PeerId peerId) {
	for (auto i = set.begin(); i != set.end();) {
		if (i->first == peerId.value) {
			i = set.erase(i);
		} else {
			++i;
		}
	}
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
				std::max(size, int64(0)),
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
			return { int(Type::Photo), std::max(size, int64(0)) };
		}
	}
	return {};
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

void WriteLiveItem(
		not_null<HistoryItem*> item,
		const SizedMedia &sized,
		bool isDup,
		const QByteArray &hash) {
	auto itemRow = Data::ChatScanItem();
	const auto history = item->history();
	itemRow.peerId = history->peer->id;
	itemRow.msgId = item->id;
	itemRow.category = sized.category;
	itemRow.size = sized.size;
	itemRow.mediaId = ItemMediaId(item, itemRow.isPhoto);
	itemRow.state = (sized.size <= 0) ? 4 : 0;
	itemRow.isDup = isDup;
	itemRow.tracked = true;
	itemRow.hash = hash;
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (const auto session = db.chatScanSession(itemRow.peerId)) {
		itemRow.sessionId = session->first;
	}
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
	LoadedStickerIndexPeers().erase(peerId.value);
	ErasePeerEntries(DirtyUniqueHashes(), peerId);
	ErasePeerEntries(DeletedUniqueHashes(), peerId);
	BumpScanVersion();
}

void RememberScannedPeer(PeerId peerId) {
	ScannedPeers().insert(peerId.value);
}

void MarkUniqueHashDirty(PeerId peerId, const QByteArray &hash) {
	if (!hash.isEmpty()) {
		DirtyUniqueHashes().insert({ peerId.value, hash });
	}
}

bool TakeUniqueHashDirty(PeerId peerId, const QByteArray &hash) {
	return DirtyUniqueHashes().erase({ peerId.value, hash }) > 0;
}

void MarkUniqueHashDeleted(PeerId peerId, MsgId msgId) {
	if (msgId) {
		DeletedUniqueHashes().insert({ peerId.value, msgId.bare });
	}
}

bool TakeUniqueHashDeleted(PeerId peerId, MsgId msgId) {
	return DeletedUniqueHashes().erase({ peerId.value, msgId.bare }) > 0;
}

void LoadStickerIndex(not_null<Main::Session*> session, PeerId peerId) {
	if (!LoadedStickerIndexPeers().insert(peerId.value).second) {
		return;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		LoadedStickerIndexPeers().erase(peerId.value);
		return;
	}
	const auto weak = base::weak_ptr<Main::Session>(session);
	db.chatScanItemIdsAsync(
		peerId,
		int(Type::Sticker),
		[weak, peerId](std::vector<MsgId> ids, bool success) {
			if (!success) {
				LoadedStickerIndexPeers().erase(peerId.value);
				return;
			}
			if (ids.empty()) {
				return;
			}
			if (LoadedStickerIndexPeers().find(peerId.value)
				== LoadedStickerIndexPeers().end()) {
				return;
			}
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			auto minId = ids.front();
			auto maxId = ids.front();
			for (const auto id : ids) {
				minId = std::min(minId, id);
				maxId = std::max(maxId, id);
			}
			const auto count = int(ids.size());
			strong->storage().add(Storage::SharedMediaAddSlice(
				peerId,
				MsgId(0),
				PeerId(0),
				Storage::SharedMediaType::Sticker,
				std::move(ids),
				MsgRange(minId, maxId),
				count));
		});
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
		result.files += row.uniqueCount + row.dupCount;
		result.bytes += row.uniqueBytes + row.dupBytes;
	}
	return result;
}

void PromoteSuccessor(
		Data::DedupDb &db,
		PeerId peerId,
		MsgId deletedId,
		const std::optional<Data::ChatScanItem> &stored) {
	if (!stored
		|| (stored->hash.isEmpty() && !stored->mediaId)) {
		return;
	}
	auto survivor = db.chatScanDuplicateSuccessor(
		peerId,
		deletedId,
		stored->mediaId,
		stored->isPhoto,
		stored->hash);
	if (!survivor) {
		return;
	}
	db.markChatScanItemDup(peerId, survivor->msgId, false);
	AdjustTotals(db, *survivor, false, +1);
	AdjustTotals(db, *survivor, true, -1);
	if (!stored->hash.isEmpty() && survivor->hash.isEmpty()) {
		db.updateChatScanItemHash(
			peerId,
			survivor->msgId,
			survivor->mediaId,
			survivor->isPhoto,
			stored->hash);
	}
	BumpScanVersion();
}

void NoteItemAdded(not_null<HistoryItem*> item) {
	if (!IsServerMsgId(item->id)) {
		return;
	}
	const auto history = item->history();
	const auto peerId = history->peer->id;
	const auto sized = Classify(item);
	if (sized.category < 0) {
		return;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	const auto tracked = IsScanned(peerId);
	if (tracked && db.hasChatScanItem(peerId, item->id)) {
		return;
	}
	if (tracked && item->id.bare <= db.chatScanScannedUpto(peerId).bare
		&& db.chatScanCategoryHasItems(peerId, sized.category)) {
		return;
	}
	auto isPhoto = false;
	const auto mediaId = ItemMediaId(item, isPhoto);
	auto hash = QByteArray();
	auto isDup = false;
	if (mediaId && (tracked || item->out())) {
		hash = db.hashForDocId(
			Data::DedupDb::Table::Uploads,
			mediaId,
			isPhoto);
		if (!hash.isEmpty()) {
			isDup = db.recordUploadDestination(
				hash,
				mediaId,
				isPhoto,
				peerId,
				item->id,
				tracked);
		}
	}
	if (!tracked) {
		return;
	}
	WriteLiveItem(item, sized, isDup, hash);
	AdjustTotals(db, peerId, sized.category, sized.size, isDup, +1);
	BumpScanVersion();
}

void NoteItemRemoved(not_null<HistoryItem*> item) {
	if (!item->isHistoryEntry()) {
		return;
	}
	const auto history = item->history();
	const auto peerId = history->peer->id;
	const auto tracked = IsScanned(peerId);
	if (!tracked && !item->out()) {
		return;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	const auto stored = db.chatScanItem(peerId, item->id);
	// The shared items row is the one-shot token: the first window or
	// flow that sees the removal consumed it and owns the accounting,
	// so a second window (the same chat viewed from another account)
	// must not adjust the row again.
	if (!stored) {
		return;
	}
	auto hashId = uint64(0);
	auto wasOwner = false;
	if (!stored->hash.isEmpty()) {
		hashId = db.findHashId(stored->hash);
		if (hashId) {
			const auto owner = db.hashOwner(hashId);
			wasOwner = owner
				&& owner->exists
				&& owner->peerId == peerId
				&& owner->msgId == item->id;
		}
	}
	auto replacement = std::optional<Data::HashOwner>();
	auto replacementRow = std::optional<Data::ChatScanItem>();
	if (tracked) {
		if (!stored->isDup) {
			if (!hashId) {
				PromoteSuccessor(db, peerId, item->id, stored);
				MarkUniqueHashDeleted(peerId, item->id);
			} else {
				MarkUniqueHashDirty(peerId, stored->hash);
				if (wasOwner) {
					replacement = db.refreshOwner(
						hashId,
						peerId,
						item->id);
					if (replacement) {
						replacementRow = db.chatScanItem(
							replacement->peerId,
							replacement->msgId);
					}
				}
			}
		}
	} else if (wasOwner) {
		MarkUniqueHashDirty(peerId, stored->hash);
		replacement = db.refreshOwner(hashId, peerId, item->id);
		if (replacement) {
			replacementRow = db.chatScanItem(
				replacement->peerId,
				replacement->msgId);
		}
	}
	if (wasOwner && !stored->hash.isEmpty()) {
		db.removeChatTableUnique(
			peerId,
			stored->mediaId,
			stored->isPhoto,
			stored->hash);
		if (replacement && replacementRow) {
			db.addChatTableUnique(
				replacement->peerId,
				replacementRow->mediaId,
				replacementRow->isPhoto,
				stored->hash);
		}
	} else if (!stored->isDup && !stored->hash.isEmpty()) {
		db.invalidateChatTable(peerId);
		db.invalidateAllPeersChatTable();
	}
	if (replacement) {
		MarkUniqueHashDirty(replacement->peerId, stored->hash);
	}
	if (replacement && replacementRow) {
		AdjustTotals(db, *replacementRow, false, +1);
		AdjustTotals(db, *replacementRow, true, -1);
	}
	AdjustTotals(db, *stored, replacement ? true : stored->isDup, -1);
	db.removeChatScanItem(peerId, item->id);
	if (tracked) {
		BumpScanVersion();
	}
}

void NoteChatDeleted(PeerId peerId) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		const auto changed = db.removeChatScan(peerId);
		db.invalidateChatTable(peerId);
		for (const auto &[replacementPeer, hash] : changed) {
			MarkUniqueHashDirty(replacementPeer, hash);
			db.invalidateChatTable(replacementPeer);
			if (const auto promoted = db.chatScanUniqueHash(
					replacementPeer,
					hash)) {
				if (const auto row = db.chatScanItem(
						replacementPeer,
						promoted)) {
					AdjustTotals(db, *row, false, +1);
					AdjustTotals(db, *row, true, -1);
				}
			}
		}
		db.invalidateAllPeersChatTable();
	}
	ForgetScannedPeer(peerId);
}

void NoteItemMediaUpdated(not_null<HistoryItem*> item) {
	if (!item->isHistoryEntry() || !IsServerMsgId(item->id)) {
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
		auto isPhoto = false;
		const auto mediaId = ItemMediaId(item, isPhoto);
		const auto hash = db.hashForDocId(
			Data::DedupDb::Table::Uploads,
			mediaId,
			isPhoto);
		WriteLiveItem(item, sized, false, hash);
		AdjustTotals(db, peerId, sized.category, sized.size, false, +1);
		BumpScanVersion();
		return;
	}
	if (stored->category == sized.category && stored->size == sized.size) {
		return;
	}
	auto fresh = *stored;
	fresh.category = sized.category;
	fresh.size = sized.size;
	fresh.mediaId = ItemMediaId(item, fresh.isPhoto);
	const auto mediaChanged = (fresh.mediaId != stored->mediaId)
		|| (fresh.isPhoto != stored->isPhoto);
	if (mediaChanged) {
		fresh.hash = db.hashForDocId(
			Data::DedupDb::Table::Uploads,
			fresh.mediaId,
			fresh.isPhoto);
	}
	if (sized.size <= 0) {
		fresh.state = 4;
	}
	db.writeChatScanItems({ fresh });
	AdjustTotals(db, *stored, stored->isDup, -1);
	AdjustTotals(db, fresh, fresh.isDup, +1);
	if (mediaChanged && !fresh.hash.isEmpty()) {
		db.updateChatScanItemHash(
			peerId,
			fresh.msgId,
			fresh.mediaId,
			fresh.isPhoto,
			fresh.hash);
	}
	if (mediaChanged) {
		db.invalidateChatTable(peerId);
		db.invalidateAllPeersChatTable();
	}
	BumpScanVersion();
}

} // namespace ChatScan
