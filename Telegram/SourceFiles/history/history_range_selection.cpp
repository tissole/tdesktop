/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/history_range_selection.h"

#include "config.h"
#include "apiwrap.h"
#include "base/debug_log.h"
#include "data/data_channel.h"
#include "data/data_file_hash.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"

#include <crl/crl.h>

#include <algorithm>
#include <limits>

namespace HistoryView {
namespace {

constexpr auto kPageSize = 100;
constexpr auto kMaxFloodRetries = 3;
// The normal path allows roughly 31 history pages per 30 seconds; a burst is
// no faster and forces a visible 27-second wait. The takeout path is not paced.
constexpr auto kPageInterval = crl::time(1000);

[[nodiscard]] const QVector<MTPMessage> *MessagesOf(
		const MTPmessages_Messages &result) {
	return result.match(
		[](const MTPDmessages_messages &data) {
			return &data.vmessages().v;
		}, [](const MTPDmessages_messagesSlice &data) {
			return &data.vmessages().v;
		}, [](const MTPDmessages_channelMessages &data) {
			return &data.vmessages().v;
		}, [](const MTPDmessages_messagesNotModified &) {
			return (const QVector<MTPMessage>*)nullptr;
		});
}

[[nodiscard]] MsgId IdOf(const MTPMessage &message) {
	return MsgId(message.match([](const auto &data) {
		return data.vid().v;
	}));
}

[[nodiscard]] int ClampId(MsgId id) {
	return int(std::clamp<int64>(
		id.bare,
		0,
		std::numeric_limits<int32>::max()));
}

} // namespace

RangeSelectionLoader::RangeSelectionLoader(
	not_null<Main::Session*> session,
	not_null<History*> history,
	MsgId fromId,
	MsgId tillId,
	int limit,
	Fn<bool(not_null<HistoryItem*>)> selectable,
	Fn<bool()> stopped)
: _session(session)
, _peer(history->peer)
, _fromId(fromId)
, _tillId(tillId)
, _limit(std::max(limit, 0))
, _selectable(std::move(selectable))
, _stopped(std::move(stopped))
, _api(&session->mtp())
, _offsetId(tillId + 1) {
}

RangeSelectionLoader::~RangeSelectionLoader() {
	releaseTakeout();
}

void RangeSelectionLoader::start(
		Fn<void(Page)> onPage,
		Fn<void(int seconds)> onWait,
		Fn<void(Result)> done) {
	_onPage = std::move(onPage);
	_onWait = std::move(onWait);
	_done = std::move(done);
	_paceTimer.setCallback([=] {
		requestPage();
	});
	_session->api().ensureTakeout(_peer, crl::guard(this, [=](bool ok) {
		if (ok && !_takeoutBorrowed) {
			_takeoutBorrowed = true;
			_session->api().setTakeoutBorrowed(true);
			if (const auto id = _session->api().takeoutId()) {
				_takeoutId = *id;
			}
		}
		if (_finished) {
			releaseTakeout();
			return;
		}
		requestPage();
	}));
}

void RangeSelectionLoader::requestPage() {
	if (_finished || _limit <= 0) {
		finish();
		return;
	} else if (_stopped && _stopped()) {
		finish();
		return;
	}
	const auto pageLimit = std::min(
		kPageSize,
		MaxSelectedItems - _rawFetched);
	if (pageLimit <= 0) {
		_truncated = true;
		finish();
		return;
	}
	auto request = MTPmessages_GetHistory(
		_peer->input(),
		MTP_int(ClampId(_offsetId)),
		MTP_int(0), // offset_date
		MTP_int(0), // add_offset
		MTP_int(pageLimit),
		MTP_int(0), // max_id
		MTP_int(0), // min_id
		MTP_long(0) // hash
	);
	const auto done = [=](const MTPmessages_Messages &result) {
		handlePage(result);
	};
	const auto failed = [=](const MTP::Error &error) {
		handleFailure(error);
	};
	if (_takeoutId) {
		_api.request(MTPInvokeWithTakeout<MTPmessages_GetHistory>(
			MTP_long(_takeoutId),
			std::move(request)
		)).done(done).fail(failed).handleFloodErrors().send();
	} else {
		_api.request(std::move(request))
			.done(done)
			.fail(failed)
			.handleFloodErrors()
			.send();
	}
}

void RangeSelectionLoader::handlePage(const MTPmessages_Messages &result) {
	if (_finished) {
		return;
	} else if (_stopped && _stopped()) {
		finish();
		return;
	}
	_session->data().processExistingMessages(_peer->asChannel(), result);

	const auto messages = MessagesOf(result);
	if (!messages || messages->isEmpty()) {
		finish();
		return;
	}
	auto page = Page();
	page.reserve(kPageSize);
	auto lowest = MsgId();
	auto reachedStart = false;
	auto hitLimit = false;
	for (const auto &message : *messages) {
		const auto id = IdOf(message);
		if (id.bare <= 0) {
			continue;
		} else if (id >= _offsetId) {
			// Some servers treat offset_id as inclusive, so the id already
			// handled by the previous page may repeat here.
			continue;
		}
		lowest = id;
		if (id < _fromId) {
			reachedStart = true;
			break;
		} else if (id > _tillId) {
			continue;
		} else if (_accepted >= _limit) {
			hitLimit = true;
			break;
		}
		++_rawFetched;
		const auto item = _session->data().message(FullMsgId(_peer->id, id));
		if (!item || (_selectable && !_selectable(not_null{ item }))) {
			continue;
		}
		page.push_back(not_null{ item });
		++_accepted;
		if (_accepted >= _limit) {
			hitLimit = true;
			break;
		}
	}
	if (!page.empty() && _onPage) {
		_onPage(std::move(page));
	}
	if (reachedStart || hitLimit || _rawFetched >= MaxSelectedItems) {
		_truncated = !reachedStart && (lowest > _fromId);
		finish();
		return;
	} else if (!lowest) {
		finish();
		return;
	}
	_retries = 0;
	_offsetId = lowest;
	if (_takeoutId) {
		requestPage();
	} else {
		_paceTimer.callOnce(kPageInterval);
	}
}

void RangeSelectionLoader::handleFailure(const MTP::Error &error) {
	if (_finished) {
		return;
	} else if (_stopped && _stopped()) {
		finish();
		return;
	}
	const auto wait = Data::FloodWaitSeconds(error.type());
	if (wait > 0 && _retries < kMaxFloodRetries) {
		++_retries;
		if (_onWait) {
			_onWait(wait);
		}
		_retryTimer.setCallback([=] {
			if (_finished) {
				return;
			} else if (_onWait) {
				_onWait(0);
			}
			requestPage();
		});
		_retryTimer.callOnce(wait * crl::time(1000));
		return;
	}

	// Reading a protected chat may reject the takeout wrapper even though the
	// normal request is allowed, so drop takeout once before failing.
	if (_takeoutId && !_takeoutFallbackTried) {
		_takeoutFallbackTried = true;
		_retries = 0;
		LOG(("RangeSelection: takeout read failed (%1), retrying without it."
			).arg(error.type()));
		releaseTakeout();
		requestPage();
		return;
	}
	_failed = true;
	finish();
}

void RangeSelectionLoader::releaseTakeout() {
	if (!_takeoutBorrowed) {
		return;
	}
	_takeoutBorrowed = false;
	_takeoutId = 0;
	_session->api().setTakeoutBorrowed(false);
	// The shared session stays open for the rest of the app run. Finishing it
	// here would trigger TAKEOUT_INIT_DELAY_ on the next range and force the
	// slow path. It is finished on exit or by another shared-session user.
}

void RangeSelectionLoader::finish() {
	if (_finished) {
		return;
	}
	_finished = true;
	_retryTimer.cancel();
	_paceTimer.cancel();
	releaseTakeout();
	if (const auto done = std::move(_done)) {
		done(Result{ _accepted, _truncated, _failed });
	}
}

} // namespace HistoryView
