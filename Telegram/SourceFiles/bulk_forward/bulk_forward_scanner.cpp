/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk_forward/bulk_forward_scanner.h"

#include "apiwrap.h"
#include "base/algorithm.h"
#include "base/debug_log.h"
#include "bulk_forward/bulk_forward_search.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "enhanced_forward.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"

#include <crl/crl.h>

#include <algorithm>
#include <iterator>
#include <limits>

namespace BulkForward {
namespace {

constexpr auto kPageSize = 100;
// Without takeout the server sustains roughly one history page per second.
// The takeout path is not paced.
constexpr auto kFallbackPageInterval = crl::time(1000);
// Right after a restart the source peer may still be missing from memory;
// its input then has no access hash and the history read fails.
constexpr auto kPeerWaitInterval = crl::time(500);

[[nodiscard]] int MessageIdOf(const MTPMessage &message) {
	return message.match(
		[](const MTPDmessage &data) {
			return data.vid().v;
		}, [](const MTPDmessageService &data) {
			return data.vid().v;
		}, [](const MTPDmessageEmpty &data) {
			return data.vid().v;
		}, [](const auto &) {
			return 0;
		});
}

[[nodiscard]] int MessageDateOf(const MTPMessage &message) {
	return message.match(
		[](const MTPDmessage &data) {
			return data.vdate().v;
		}, [](const MTPDmessageService &data) {
			return data.vdate().v;
		}, [](const auto &) {
			return 0;
		});
}

[[nodiscard]] int MtpId(int64 value) {
	return int(std::clamp<int64>(
		value,
		int64(0),
		int64(std::numeric_limits<int>::max())));
}

} // namespace

HistoryScanner::HistoryScanner(not_null<Main::Session*> session)
: _session(session) {
}

HistoryScanner::~HistoryScanner() {
	cancel();
}

void HistoryScanner::start(ScanRequest &&request) {
	cancel();

	if (UsesSelection(request.filter)) {
		if (request.callbacks.fail) {
			request.callbacks.fail(u"selection scope needs the selection scanner"_q);
		}
		return;
	}

	_cancelled = false;
	_paused = false;
	_waiting = false;
	_resumeAt = 0;
	_takeoutFallbackTried = false;
	_sourceIndex = 0;
	_scanned = 0;
	_matchedCount = 0;
	_total = 0;
	_topicRootId = request.topicRootId;
	_filter = std::move(request.filter);
	_callbacks = std::move(request.callbacks);
	_method = request.method;
	_searchFilter.reset();
	if (_method == ScanMethod::Search) {
		if (const auto piece = SingleSearchPiece(_filter.types)) {
			if (_filter.scope != Scope::DateRange
				&& !request.peer->migrateFrom()) {
				_searchFilter = SearchFilterFor(*piece);
			} else {
				_method = ScanMethod::History;
			}
		} else {
			LOG(("BulkForward: search scan needs one media index, "
				"falling back to history."));
			_method = ScanMethod::History;
		}
	}
	LOG(("BulkForward: scan method is %1."
		).arg((_method == ScanMethod::Search) ? u"search"_q : u"history"_q));
	_sources.clear();

	const auto push = [&](PeerData *peer) {
		auto source = Source{ not_null<PeerData*>(peer) };
		switch (_filter.scope) {
		case Scope::All:
			break;
		case Scope::DateRange:
			source.jumping = (_filter.fromDate > 0);
			break;
		case Scope::IdRange:
			source.cursor = _filter.fromId
				? (_filter.fromId - 1)
				: MsgId(0);
			break;
		case Scope::Selection:
			break;
		}
		if (const auto i = request.cursors.find(peer->id);
			_filter.scope != Scope::Selection
			&& i != end(request.cursors)
			&& i->second) {
			source.cursor = i->second;
			source.jumping = false;
		}
		_sources.push_back(std::move(source));
	};
	if (const auto migrated = request.peer->migrateFrom()) {
		push(migrated);
	}
	push(request.peer);

	_timer.setCallback([=] {
		requestNextPage();
	});
	_session->api().ensureTakeout(
		_sources.back().peer,
		crl::guard(this, [=](bool ok) {
			if (ok && !_takeoutBorrowed) {
				_takeoutBorrowed = true;
				_session->api().setTakeoutBorrowed(true);
				if (const auto id = _session->api().takeoutId()) {
					_takeoutId = *id;
				}
			}
			if (_cancelled) {
				releaseTakeout();
				return;
			}
			requestNextPage();
		}));
}

void HistoryScanner::pause() {
	_paused = true;
	_timer.cancel();
}

void HistoryScanner::resume() {
	if (!_paused) {
		return;
	}
	_paused = false;
	const auto now = crl::now();
	if (_resumeAt > now) {
		_timer.callOnce(_resumeAt - now);
	} else {
		requestNextPage();
	}
}

void HistoryScanner::cancel() {
	_timer.cancel();
	if (_requestId) {
		_session->api().request(base::take(_requestId)).cancel();
	}
	_cancelled = true;
	_waiting = false;
	_callbacks = ScanCallbacks();
	releaseTakeout();
}

	std::vector<std::pair<PeerId, MsgId>> HistoryScanner::resumeCursors() const {
	auto result = std::vector<std::pair<PeerId, MsgId>>();
	result.reserve(_sources.size());
	for (const auto &source : _sources) {
		result.push_back({ source.peer->id, source.cursor });
	}
	return result;
}

ScanMethod HistoryScanner::method() const {
	return _method;
}

void HistoryScanner::requestNextPage() {
	if (_cancelled || _paused || _waiting) {
		return;
	}
	while (_sourceIndex < int(_sources.size())
		&& _sources[_sourceIndex].finished) {
		++_sourceIndex;
	}
	if (_sourceIndex >= int(_sources.size())) {
		finish();
		return;
	}

	const auto sourceIndex = _sourceIndex;
	const auto &source = _sources[sourceIndex];
	const auto needsAccessHash = source.peer->isUser()
		|| source.peer->isChannel();
	if (needsAccessHash && !source.peer->isLoaded()) {
		_resumeAt = crl::now() + kPeerWaitInterval;
		_timer.callOnce(kPeerWaitInterval);
		return;
	}
	const auto jumping = source.jumping;
	const auto offsetId = jumping ? 0 : MtpId(source.cursor.bare + 1);
	const auto offsetDate = jumping ? (int(_filter.fromDate) - 1) : 0;
	const auto addOffset = jumping ? -1 : -kPageSize;
	const auto limit = jumping ? 1 : kPageSize;

	const auto weak = base::make_weak(this);
	const auto done = [=](const MTPmessages_Messages &result) {
		if (!weak) {
			return;
		}
		weak->_requestId = 0;
		weak->_waiting = false;
		weak->handlePage(result, sourceIndex);
	};
	const auto failed = [=](const MTP::Error &error) {
		if (!weak) {
			return;
		}
		weak->_requestId = 0;
		weak->_waiting = false;
		if (weak->_takeoutId && !weak->_takeoutFallbackTried) {
			weak->_takeoutFallbackTried = true;
			LOG(("BulkForward: takeout scan read failed (%1), retrying."
				).arg(error.type()));
			weak->releaseTakeout();
			weak->requestNextPage();
			return;
		}
		weak->fail(error.type());
	};

	const auto send = [&](auto request) {
		using Request = decltype(request);
		if (_takeoutId) {
			_requestId = _session->api().request(
				MTPInvokeWithTakeout<Request>(
					MTP_long(_takeoutId),
					std::move(request))
			).done(done).fail(failed).send();
		} else {
			_requestId = _session->api().request(std::move(request))
				.done(done)
				.fail(failed)
				.send();
		}
	};
	_waiting = true;
	_resumeAt = 0;
	if (_method == ScanMethod::Search) {
		// No min_id/max_id bounds and no date bounds: they change what the
		// offset is counted over and break the ascending walk. The cursor
		// starts at fromId - 1 for an id range and the walk stops locally.
		using Flag = MTPmessages_Search::Flag;
		send(MTPmessages_Search(
			MTP_flags(_topicRootId ? Flag::f_top_msg_id : Flag()),
			source.peer->input(),
			MTP_string(),
			MTPInputPeer(),
			MTPInputPeer(),
			MTPVector<MTPReaction>(),
			MTP_int(MtpId(_topicRootId.bare)),
			*_searchFilter,
			MTP_int(0),
			MTP_int(0),
			MTP_int(offsetId),
			MTP_int(addOffset),
			MTP_int(limit),
			MTP_int(0),
			MTP_int(0),
			MTP_long(0)
		));
	} else if (_topicRootId) {
		send(MTPmessages_GetReplies(
			source.peer->input(),
			MTP_int(MtpId(_topicRootId.bare)),
			MTP_int(offsetId),
			MTP_int(offsetDate),
			MTP_int(addOffset),
			MTP_int(limit),
			MTP_int(0),
			MTP_int(0),
			MTP_long(0)
		));
	} else {
		send(MTPmessages_GetHistory(
			source.peer->input(),
			MTP_int(offsetId),
			MTP_int(offsetDate),
			MTP_int(addOffset),
			MTP_int(limit),
			MTP_int(0),
			MTP_int(0),
			MTP_long(0)
		));
	}
}

void HistoryScanner::handlePage(
		const MTPmessages_Messages &result,
		int sourceIndex) {
	if (_cancelled || (sourceIndex != _sourceIndex)) {
		return;
	}
	auto &source = _sources[sourceIndex];

	const QVector<MTPMessage> *messages = nullptr;
	auto count = 0;
	auto nextRate = 0;
	auto complete = false;
	result.match([&](const MTPDmessages_messages &data) {
		messages = &data.vmessages().v;
		count = int(data.vmessages().v.size());
		complete = true;
	}, [&](const MTPDmessages_messagesSlice &data) {
		messages = &data.vmessages().v;
		count = data.vcount().v;
		if (const auto rate = data.vnext_rate()) {
			nextRate = rate->v;
		}
	}, [&](const MTPDmessages_channelMessages &data) {
		messages = &data.vmessages().v;
		count = data.vcount().v;
	}, [&](const auto &) {
	});

	if (source.jumping) {
		handleJumpPage(source, messages);
		nextRate = 0;
	} else {
		if (count > 0
			&& (_filter.scope == Scope::All || _method == ScanMethod::Search)
			&& source.total != count) {
			source.total = count;
			updateTotal();
		}
		if (!messages || messages->isEmpty()) {
			finishSource(source);
		} else {
			const auto size = int(messages->size());
			source.scanned += size;
			_scanned += size;
			handleWalkPage(source, *messages);
			if (_cancelled) {
				return;
			} else if (complete) {
				finishSource(source);
			}
		}
	}
	reportProgress();
	if (_cancelled) {
		return;
	} else if (_paused) {
		return;
	} else if (nextRate > 0) {
		const auto delay = crl::time(nextRate) * 1000;
		_resumeAt = crl::now() + delay;
		_timer.callOnce(delay);
	} else if (!_takeoutId) {
		_resumeAt = crl::now() + kFallbackPageInterval;
		_timer.callOnce(kFallbackPageInterval);
	} else {
		_resumeAt = 0;
		requestNextPage();
	}
}

void HistoryScanner::handleJumpPage(
		Source &source,
		const QVector<MTPMessage> *messages) {
	source.jumping = false;
	const auto first = (messages && !messages->isEmpty())
		? MessageIdOf(messages->front())
		: 0;

	// No message older than the range start means the range begins at the
	// chat start, so the walk simply starts from the oldest message.
	source.cursor = (first > 0) ? MsgId(first - 1) : MsgId(0);
}

void HistoryScanner::handleWalkPage(
		Source &source,
		const QVector<MTPMessage> &messages) {
	const auto from = source.cursor.bare;
	const auto size = int(messages.size());
	auto highest = int64(0);
	auto newestDate = 0;
	auto batch = std::vector<SourceMessage>();
	batch.reserve(size);
	for (auto i = size; i > 0;) {
		const auto &message = messages[--i];
		const auto id = int64(MessageIdOf(message));
		if (id <= 0 || id <= from) {
			continue;
		}
		if (id > highest) {
			highest = id;
		}
		const auto date = MessageDateOf(message);
		if (date > newestDate) {
			newestDate = date;
		}
		const auto checked = checkMessage(source, message);
		if (checked) {
			batch.push_back(*checked);
			++_matchedCount;
		}
	}
	if (highest <= 0) {
		finishSource(source);
		return;
	}
	source.cursor = MsgId(highest);
	emitBatch(std::move(batch));
	if (_cancelled) {
		return;
	}
	const auto dateDone = (_filter.scope == Scope::DateRange)
		&& (_filter.tillDate > 0)
		&& (newestDate > int(_filter.tillDate));
	const auto idDone = (_filter.scope == Scope::IdRange)
		&& (_filter.tillId > 0)
		&& (MsgId(highest) > _filter.tillId);
	// A search count may be capped, so only an exhausted page ends it; the
	// history total is exact and saves the page after the last one.
	const auto totalDone = (_method != ScanMethod::Search)
		&& (source.total > 0)
		&& (source.scanned >= source.total);
	if (size < kPageSize || totalDone || dateDone || idDone) {
		finishSource(source);
	}
}

std::optional<SourceMessage> HistoryScanner::checkMessage(
		const Source &source,
		const MTPMessage &message) const {
	const auto classified = Classify(
		message,
		source.peer->id,
		EnhancedForward::PeerNeedsEnhancedForward(source.peer));
	if (!classified) {
		return std::nullopt;
	} else if (!MatchesRange(_filter, *classified)
		|| !MatchesContent(_filter, *classified)) {
		return std::nullopt;
	}
	return classified;
}

void HistoryScanner::emitBatch(std::vector<SourceMessage> items) {
	if (items.empty() || !_callbacks.batch) {
		return;
	}
	_callbacks.batch(std::move(items));
}

void HistoryScanner::finishSource(Source &source) {
	source.finished = true;
}

void HistoryScanner::updateTotal() {
	_total = 0;
	for (const auto &source : _sources) {
		_total += source.total;
	}
}

void HistoryScanner::reportProgress() {
	if (_callbacks.progress) {
		_callbacks.progress(_scanned, _total, _matchedCount);
	}
}

void HistoryScanner::releaseTakeout() {
	if (!_takeoutBorrowed) {
		return;
	}
	_takeoutBorrowed = false;
	_takeoutId = 0;
	_session->api().setTakeoutBorrowed(false);
	// The shared session stays open for the rest of the app run. Finishing it
	// here would trigger TAKEOUT_INIT_DELAY_ on the next scan and force the
	// slow path. It is finished on exit or by another shared-session user.
}

void HistoryScanner::finish() {
	if (_cancelled) {
		return;
	}
	reportProgress();
	_cancelled = true;
	releaseTakeout();
	const auto done = std::move(_callbacks.done);
	_callbacks = ScanCallbacks();
	if (done) {
		done();
	}
}

void HistoryScanner::fail(const QString &error) {
	if (_cancelled) {
		return;
	}
	_cancelled = true;
	releaseTakeout();
	const auto fail = std::move(_callbacks.fail);
	_callbacks = ScanCallbacks();
	if (fail) {
		fail(error);
	}
}

} // namespace BulkForward
