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

namespace ChatScan {

void NoteItemAdded(not_null<HistoryItem*> item);
void NoteItemRemoved(not_null<HistoryItem*> item);
void NoteItemMediaUpdated(not_null<HistoryItem*> item);
// Drops one peer from the in-RAM scanned-peers cache (after its record
// was cleared or rewritten in the database).
void ForgetScannedPeer(PeerId peerId);
// Marks the peer's walk as live-tracked once its scan wrote results.
void RememberScannedPeer(PeerId peerId);

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
