/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "data/data_forward_seen.h"
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

// One duplicate decision for every forward path. The caller owns its own
// accepted set for the current batch and passes only whether that set already
// has the media; every check that talks to the dedup database goes through
// this decision.
enum class Verdict : uchar {
	Allowed,
	InBatch,
	DestinationDuplicate,
	GlobalDuplicate,
};

// `run` is the content this multi-destination run already accepted for
// another destination: its uploads row must not make this destination skip
// the same content. `checkFwTmp` adds the per-run rows the copy pipeline
// writes before it sends; the manual copy path has none.
struct Context {
	PeerId destination;
	bool globalOn = false;
	bool destinationOnly = false;
	const ForwardRunSeen *run = nullptr;
	bool checkFwTmp = false;
	uint64 sessionId = 0;
};

[[nodiscard]] bool IsGlobalOn(const Context &context);

[[nodiscard]] Verdict CheckMedia(
	const Context &context,
	uint64 mediaId,
	bool isPhoto,
	bool seenInBatch);

[[nodiscard]] Verdict CheckHash(
	const Context &context,
	const QByteArray &hash,
	bool seenInBatch);

void AddPending(uint64 mediaId, bool isPhoto, const QByteArray &hash);
void RemovePending(uint64 mediaId, bool isPhoto);

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
