/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "bulk_forward/bulk_forward_filter.h"
#include "base/basic_types.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "data/data_msg_id.h"
#include "data/data_peer_id.h"

#include <QByteArray>

#include <optional>
#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace BulkForward {

struct ResumeDestination {
	PeerId peer;
	MsgId topicRootId;
	int sent = 0;
	int skipped = 0;
	int failed = 0;
};

// One carrier copy that produced a message in this run. `done` lists the
// destinations that already received this exact source message, so a resumed
// fan-out only forwards the missing part and never copies the batch again at
// the carrier.
struct ResumeCarrierItem {
	FullMsgId source;
	PeerId carrierPeer;
	MsgId carrierId;
	uint64 mediaId = 0;
	bool isPhoto = false;
	QByteArray hash;
	base::flat_set<PeerId> done;
};

// Everything needed to continue a bulk forward after a restart. The scan
// position is a cursor per source peer, so a migrated chat keeps its own
// position. `matched` already excludes the messages that were still queued
// when the record was written; they are reported again by the resumed scan.
struct ResumeState {
	PeerId source;
	MsgId sourceTopicRootId;
	Filter filter;
	ScanMethod method = ScanMethod::Unknown;
	std::vector<ResumeDestination> destinations;
	std::vector<ResumeCarrierItem> carriers;
	base::flat_map<PeerId, MsgId> cursors;
	int scanned = 0;
	int total = 0;
	int matched = 0;
	// Bytes of the messages routed to the copy path, and bytes already
	// uploaded. Pending copy messages are subtracted before the save,
	// because they are read and measured again after a resume.
	qint64 knownBytes = 0;
	qint64 doneBytes = 0;
	qint64 skippedBytes = 0;
	// The count preview at save time, so a resumed bar keeps the whole
	// matching set as its denominator instead of the current batch.
	int previewTotal = 0;
	// Unixtime of the original start, so a resumed run keeps its date.
	int64 startedAt = 0;
	// Serialization version of the record this state came from. Records
	// written before the per-item copy counters need the resume fallback.
	int version = 0;
};

// Loads the unfinished bulk forward of the session once, publishes its
// counters as paused and remembers it for ResumeActive().
void EnsureResumeSeeded(not_null<Main::Session*> session);

// Returns the seeded run and forgets that it is waiting for a resume, so a
// second resume click does not start it twice. The database row stays.
[[nodiscard]] std::optional<ResumeState> TakeSeededResume(
	not_null<Main::Session*> session);

// Creates or updates the durable record of a running or paused run.
void SaveResume(
	not_null<Main::Session*> session,
	const ResumeState &state,
	bool paused);

// Removes the durable record and the seeded state of the session.
void ClearResume(not_null<Main::Session*> session);

} // namespace BulkForward
