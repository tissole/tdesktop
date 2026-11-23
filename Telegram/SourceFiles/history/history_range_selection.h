/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_msg_id.h"
#include "mtproto/sender.h"
#include "scheme.h"

#include <vector>

class History;
class HistoryItem;
class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace MTP {
class Error;
} // namespace MTP

namespace HistoryView {

// Fetches a bounded range of messages from the server so that "Select up to
// this message" works without scrolling the range into memory first.
//
// Pages walk older from just above `tillId` towards `fromId` with
// messages.getHistory, newest first. The loader stops at `limit` selectable
// items and never materializes more than MaxSelectedItems raw messages, so the
// memory cost of a selection stays bounded. The end of the range is detected
// by seeing an id below `fromId`; a short page is not treated as the end,
// because the server may omit an endpoint or return fewer messages than
// requested in the middle of a range. The caller owns the loader, cancels it
// by destroying it, and may poll `stopped` between pages to stop early, for
// example after the selection was cleared by the user.
class RangeSelectionLoader final : public base::has_weak_ptr {
public:
	struct Result {
		int accepted = 0;
		bool truncated = false;
		bool failed = false;
	};
	using Page = std::vector<not_null<HistoryItem*>>;

	RangeSelectionLoader(
		not_null<Main::Session*> session,
		not_null<History*> history,
		MsgId fromId,
		MsgId tillId,
		int limit,
		Fn<bool(not_null<HistoryItem*>)> selectable,
		Fn<bool()> stopped);
	~RangeSelectionLoader();

	void start(
		Fn<void(Page)> onPage,
		Fn<void(int seconds)> onWait,
		Fn<void(Result)> done);

private:
	void requestPage();
	void handlePage(const MTPmessages_Messages &result);
	void handleFailure(const MTP::Error &error);
	void releaseTakeout();
	void finish();

	const not_null<Main::Session*> _session;
	const not_null<PeerData*> _peer;
	const MsgId _fromId;
	const MsgId _tillId;
	const int _limit = 0;
	Fn<bool(not_null<HistoryItem*>)> _selectable;
	Fn<bool()> _stopped;
	Fn<void(Page)> _onPage;
	Fn<void(int seconds)> _onWait;
	Fn<void(Result)> _done;
	MTP::Sender _api;
	base::Timer _retryTimer;
	base::Timer _paceTimer;
	uint64 _takeoutId = 0;
	MsgId _offsetId;
	int _rawFetched = 0;
	int _accepted = 0;
	int _retries = 0;
	bool _takeoutBorrowed = false;
	bool _takeoutFallbackTried = false;
	bool _finished = false;
	bool _truncated = false;
	bool _failed = false;

};

} // namespace HistoryView
