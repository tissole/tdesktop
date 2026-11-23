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
#include "rpl/producer.h"

#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace BulkForward {

// One destination of a running bulk forward, for the shared progress surface.
// `scanned` and `total` describe the source scan; `matched` is how many
// messages will be forwarded, `sent` and `skipped` how they are being handled.
// `skipped` counts duplicates only: they are removed from the displayed totals
// and reported on their own line. `failed` stays in the totals, so the bar
// ends below 100% when something could not be forwarded.
struct Counters {
	Main::Session *session = nullptr;
	PeerId source;
	PeerId destination;
	// Real first and last matched ids of the whole run, so the Forwards
	// tab shows the selected range instead of one batch.
	FullMsgId firstSource;
	FullMsgId lastSource;
	int scanned = 0;
	int total = 0;
	int matched = 0;
	int sent = 0;
	int skipped = 0;
	int failed = 0;
	// Settled protected copies already included in `sent`. A live copy
	// pipeline reports the same items item by item, so the bar subtracts
	// this before adding the pipeline's own counter.
	int copied = 0;
	// Bytes of the messages routed to the copy path, and bytes already
	// uploaded to a destination. The first grows as protected batches are
	// queued, the second with each finished upload.
	qint64 knownBytes = 0;
	qint64 doneBytes = 0;
	// Bytes of the duplicates in `skipped`; removed from the byte total and
	// shown on the duplicates line.
	qint64 skippedBytes = 0;
	// Unixtime when this run started, so the Forwards tab can date the row
	// while it is still running.
	int64 startedAt = 0;
	bool paused = false;
	bool active = false;
	bool scanDone = false;
};

// Replaces the counters of the given destinations and notifies once.
void UpdateCounters(std::vector<Counters> counters);

// Drops the counters of one destination, after a finish or a cancel.
void RemoveCounters(Main::Session *session, const PeerId &destination);

// Drops every counter of a session, after a cancel or a logout.
void RemoveAllCounters(not_null<Main::Session*> session);

[[nodiscard]] std::vector<Counters> AllCounters(
	not_null<Main::Session*> session);

[[nodiscard]] bool HasCounters(
	not_null<Main::Session*> session,
	const PeerId &destination);

[[nodiscard]] rpl::producer<> countersChanged();

} // namespace BulkForward
