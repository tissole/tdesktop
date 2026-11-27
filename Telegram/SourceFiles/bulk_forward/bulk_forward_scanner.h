/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "bulk_forward/bulk_forward_classifier.h"
#include "bulk_forward/bulk_forward_filter.h"
#include "base/basic_types.h"
#include "base/flat_map.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_msg_id.h"
#include "data/data_types.h"
#include "mtproto/core_types.h"
#include "scheme.h"

#include <optional>
#include <utility>
#include <vector>

namespace Main {
class Session;
} // namespace Main

class PeerData;

namespace BulkForward {

struct ScanCallbacks {
	// Ascending ids, at most 100 per call.
	Fn<void(std::vector<SourceMessage>)> batch;
	// `total` is the chat size for the all-messages scope and the expected
	// number of matches for a search walk; it is zero when a history walk of
	// a date or id range has no size known up front.
	Fn<void(int scanned, int total, int matched)> progress;
	Fn<void()> done;
	Fn<void(QString)> fail;
};

struct ScanRequest {
	not_null<PeerData*> peer;
	MsgId topicRootId;
	Filter filter;
	ScanMethod method = ScanMethod::History;
	// Saved position per source peer, from resumeCursors(). A zero or missing
	// cursor starts the peer from the beginning of its scope.
	base::flat_map<PeerId, MsgId> cursors;
	ScanCallbacks callbacks;
};

// Reports the ids that pass the filter, oldest first.
//
// Every scope is walked from the oldest wanted message towards the newest one,
// so the first matching batch is reported right away instead of after the whole
// range is read. Telegram pages backwards, so the first page is reached in one
// request by asking for the 100 messages above a cursor id:
//
//   offset_id = cursor + 1, add_offset = -100, limit = 100
//
// The first page needs the id of the oldest wanted message, which is known per
// scope: `1` for all messages, `fromId` for a message id range, and a single
// `offset_date` jump for a date range. The cursor is that id minus one.
//
// A migrated chat is walked in both of its peers, the old one first. Forum
// topics are read through messages.getReplies, so only the selected topic is
// scanned.
//
// When the filter maps onto one server media index and that index covers at
// most half of the chat, the same cursor walk reads messages.search with that
// filter instead of the whole history. Only the current peer is scanned then,
// because migrated chats always use history.
class HistoryScanner final : public base::has_weak_ptr {
public:
	explicit HistoryScanner(not_null<Main::Session*> session);
	~HistoryScanner();

	void start(ScanRequest &&request);
	void pause();
	void resume();
	void cancel();

	[[nodiscard]] bool paused() const {
		return _paused;
	}
	[[nodiscard]] int scanned() const {
		return _scanned;
	}
	[[nodiscard]] int matched() const {
		return _matchedCount;
	}

	// Positions of the sources, newest reported id per peer. Passing them back
	// through ScanRequest::cursors continues the walk after the saved page.
	[[nodiscard]] std::vector<std::pair<PeerId, MsgId>> resumeCursors() const;
	[[nodiscard]] ScanMethod method() const;

private:
	struct Source {
		not_null<PeerData*> peer;

		// Highest id already reported; the next page starts right above it.
		MsgId cursor;

		int total = 0;
		int scanned = 0;
		bool jumping = false;
		bool finished = false;
	};

	void requestNextPage();
	void handlePage(
		const MTPmessages_Messages &result,
		int sourceIndex);
	void handleJumpPage(
		Source &source,
		const QVector<MTPMessage> *messages);
	void handleWalkPage(
		Source &source,
		const QVector<MTPMessage> &messages);
	[[nodiscard]] std::optional<SourceMessage> checkMessage(
		const Source &source,
		const MTPMessage &message) const;
	void emitBatch(std::vector<SourceMessage> items);
	void finishSource(Source &source);
	void updateTotal();
	void reportProgress();
	void releaseTakeout();
	void finish();
	void fail(const QString &error);

	const not_null<Main::Session*> _session;
	ScanCallbacks _callbacks;
	Filter _filter;
	ScanMethod _method = ScanMethod::History;
	std::optional<MTPMessagesFilter> _searchFilter;
	std::vector<Source> _sources;
	base::Timer _timer;
	// When the next page may be requested after a rate-limit countdown or the
	// fallback pacing delay. Kept across pause(), so holding the scan and
	// releasing it does not turn a countdown into an immediate request.
	crl::time _resumeAt = 0;
	MsgId _topicRootId;
	int _sourceIndex = 0;
	int _scanned = 0;
	int _matchedCount = 0;
	int _total = 0;
	mtpRequestId _requestId = 0;
	uint64 _takeoutId = 0;
	bool _paused = false;
	bool _waiting = false;
	bool _cancelled = false;
	bool _takeoutBorrowed = false;
	bool _takeoutFallbackTried = false;

};

} // namespace BulkForward
