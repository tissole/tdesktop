/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_msg_id.h"
#include "rpl/variable.h"

class HistoryItem;
class PeerData;
class QByteArray;

namespace Main {
class Session;
} // namespace Main

namespace ChatScan {

void NoteItemAdded(not_null<HistoryItem*> item);
void NoteItemRemoved(not_null<HistoryItem*> item);
void NoteItemMediaUpdated(not_null<HistoryItem*> item);
// Drops a really deleted chat from the scan database, promoting or clearing
// the owners it held and cleaning up unreferenced content rows.
void NoteChatDeleted(PeerId peerId);
// Drops one peer from the in-RAM scanned-peers cache (after its record
// was cleared or rewritten in the database).
void ForgetScannedPeer(PeerId peerId);
// Marks the peer's walk as live-tracked once its scan wrote results.
void RememberScannedPeer(PeerId peerId);
// Marks the stored unique hash stale after a live deletion; the
// running scan re-reads it from the database on the next hash completion.
void MarkUniqueHashDirty(PeerId peerId, const QByteArray &hash);
[[nodiscard]] bool TakeUniqueHashDirty(
	PeerId peerId,
	const QByteArray &hash);
// Same for a unique hash owner deleted before its hash reached the database.
void MarkUniqueHashDeleted(PeerId peerId, MsgId msgId);
[[nodiscard]] bool TakeUniqueHashDeleted(
	PeerId peerId,
	MsgId msgId);
// Seeds the local sticker media list from the last scan of this peer.
void LoadStickerIndex(not_null<Main::Session*> session, PeerId peerId);

// Bytes recorded for one scan category (0 when the peer was never
// scanned or the category is absent). Backs the profile size labels.
[[nodiscard]] int64 ScanSizeBytes(PeerId peerId, int category);

// Everything one peer's scan record holds at the moment: the unique
// files plus the duplicates still on the list. Zero until the first
// walk wrote results. Backs the profile total row.
struct ScanTotalMedia {
	int files = 0;
	int64 bytes = 0;
};
[[nodiscard]] ScanTotalMedia TotalScannedMedia(PeerId peerId);
// Fires on every index mutation; size labels re-read through it.
[[nodiscard]] rpl::producer<int> ScanSizeVersion();

} // namespace ChatScan
