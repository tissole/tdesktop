/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "data/data_msg_id.h"
#include "data/data_peer_id.h"

#include <QByteArray>

#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

class DocumentData;
class HistoryItem;
class PhotoData;

namespace Main {
class Session;
} // namespace Main

namespace Data::ForwardDedup {

// Copy and server-side forward requests reuse the file on the server, so
// transfer dedup works on document ids plus the remote content fingerprints.
// Text and id-less items always go.
struct Media {
	uint64 mediaId = 0;
	DocumentData *doc = nullptr;
	PhotoData *photo = nullptr;
	MsgId sentId;
	QByteArray hash;
	bool pending = false;
};

[[nodiscard]] Media MediaOf(HistoryItem *item);

// Samples are only fetched for items that no id check can rule out, so a
// repeat copy of files already sent does not touch the network at all. The
// id checks here are read-only: the batch itself is still filtered in order
// by Skip, which owns the run-wide seen sets and the pending rows.
void ResolveHashes(
	PeerId dstPeer,
	const std::set<std::pair<uint64, bool>> &seen,
	const std::vector<not_null<HistoryItem*>> &items,
	std::shared_ptr<std::map<const HistoryItem*, QByteArray>> known,
	Fn<void()> done);

[[nodiscard]] bool Skip(
	HistoryItem *item,
	PeerId dstPeer,
	std::set<std::pair<uint64, bool>> &seenInBatch,
	std::set<QByteArray> &seenHashes,
	const QByteArray &hash,
	bool &pendingAdded);

void RecordHash(
	uint64 mediaId,
	bool isPhoto,
	const QByteArray &hash,
	PeerId dstPeer,
	MsgId sentId);

void Done(
	Main::Session *session,
	uint64 mediaId,
	DocumentData *doc,
	PhotoData *photo,
	const QByteArray &hash,
	PeerId dstPeer,
	MsgId sentId,
	bool pending);

void Fail(uint64 mediaId, bool isPhoto, bool pending);

} // namespace Data::ForwardDedup
