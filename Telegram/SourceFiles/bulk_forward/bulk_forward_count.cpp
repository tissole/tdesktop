/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk_forward/bulk_forward_count.h"

#include "apiwrap.h"
#include "bulk_forward/bulk_forward_search.h"
#include "data/data_peer.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"
#include "scheme.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace BulkForward {
namespace {

constexpr auto kCountDelay = crl::time(400);
constexpr auto kPeerWaitInterval = crl::time(500);

[[nodiscard]] int MtpId(int64 value) {
	return int(std::clamp<int64>(
		value,
		int64(0),
		int64(std::numeric_limits<int>::max())));
}

struct CountResult {
	int count = 0;
	MsgId newestId;
};

[[nodiscard]] MsgId NewestIdFromMessages(const QVector<MTPMessage> &messages) {
	for (const auto &message : messages) {
		const auto id = message.match(
			[](const MTPDmessage &data) {
				return data.vid().v;
			}, [](const MTPDmessageService &data) {
				return data.vid().v;
			}, [](const MTPDmessageEmpty &data) {
				return data.vid().v;
			}, [](const auto &) {
				return 0;
			});
		if (id > 0) {
			return MsgId(id);
		}
	}
	return MsgId(0);
}

[[nodiscard]] CountResult CountFromResult(const MTPmessages_Messages &result) {
	return result.match(
		[](const MTPDmessages_messages &data) {
			return CountResult{
				.count = int(data.vmessages().v.size()),
				.newestId = NewestIdFromMessages(data.vmessages().v),
			};
		}, [](const MTPDmessages_messagesSlice &data) {
			return CountResult{
				.count = data.vcount().v,
				.newestId = NewestIdFromMessages(data.vmessages().v),
			};
		}, [](const MTPDmessages_channelMessages &data) {
			return CountResult{
				.count = data.vcount().v,
				.newestId = NewestIdFromMessages(data.vmessages().v),
			};
		}, [](const MTPDmessages_messagesNotModified &) {
			return CountResult{ .count = -1 };
		});
}

void RequestHistoryTotal(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		MsgId topicRootId,
		int minId,
		int maxId,
		Fn<void(CountResult)> done) {
	const auto received = [=](const MTPmessages_Messages &result) {
		done(CountFromResult(result));
	};
	const auto failed = [=](const MTP::Error &) {
		done(CountResult{ .count = -1 });
	};
	if (topicRootId) {
		session->api().request(MTPmessages_GetReplies(
			peer->input(),
			MTP_int(MtpId(topicRootId.bare)),
			MTP_int(0),
			MTP_int(0),
			MTP_int(0),
			MTP_int(1),
			MTP_int(maxId),
			MTP_int(minId),
			MTP_long(0)
		)).done(received).fail(failed).send();
	} else {
		session->api().request(MTPmessages_GetHistory(
			peer->input(),
			MTP_int(0),
			MTP_int(0),
			MTP_int(0),
			MTP_int(1),
			MTP_int(maxId),
			MTP_int(minId),
			MTP_long(0)
		)).done(received).fail(failed).send();
	}
}

void RequestCount(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		MsgId topicRootId,
		int minDate,
		int maxDate,
		int minId,
		int maxId,
		const MTPMessagesFilter &filter,
		Fn<void(CountResult)> done) {
	using Flag = MTPmessages_Search::Flag;
	session->api().request(MTPmessages_Search(
		MTP_flags(topicRootId ? Flag::f_top_msg_id : Flag()),
		peer->input(),
		MTP_string(),
		MTPInputPeer(),
		MTPInputPeer(),
		MTPVector<MTPReaction>(),
		MTP_int(MtpId(topicRootId.bare)),
		filter,
		MTP_int(minDate),
		MTP_int(maxDate),
		MTP_int(0),
		MTP_int(0),
		MTP_int(1),
		MTP_int(maxId),
		MTP_int(minId),
		MTP_long(0)
	)).done([=](const MTPmessages_Messages &result) {
		done(CountFromResult(result));
	}).fail([=](const MTP::Error &) {
		done(CountResult{ .count = -1 });
	}).send();
}

} // namespace

CountProbe::CountProbe(
	not_null<Main::Session*> session,
	not_null<PeerData*> peer,
	MsgId topicRootId)
: _session(session)
, _peer(peer)
, _topicRootId(topicRootId) {
	_timer.setCallback([this] {
		start();
	});
}

CountProbe::~CountProbe() = default;

rpl::producer<QString> CountProbe::text() const {
	return _text.value();
}

rpl::producer<int> CountProbe::countValue() const {
	return _countValue.value();
}

rpl::producer<MsgId> CountProbe::lastId() const {
	return _lastId.value();
}

rpl::producer<ScanMethod> CountProbe::method() const {
	return _method.value();
}

void CountProbe::request(Filter filter) {
	_filter = std::move(filter);
	_timer.cancel();
	_lastId = 0;
	_method = ScanMethod::Unknown;
	if (_filter.scope == Scope::Selection || !IsValid(_filter)) {
		_text = QString();
		_countValue = 0;
		_lastId = 0;
		_method = ScanMethod::History;
		return;
	}
	_timer.callOnce(kCountDelay);
	if (!boundsChanged(_filter)) {
		// Cached counts are valid for the same bounds, so a filter that can
		// never use search need not wait for the debounced round-trips.
		publishMethod();
	}
}

bool CountProbe::boundsChanged(const Filter &filter) const {
	return (_boundsScope != filter.scope)
		|| (_boundsFromDate != filter.fromDate)
		|| (_boundsTillDate != filter.tillDate)
		|| (_boundsFromId != filter.fromId)
		|| (_boundsTillId != filter.tillId);
}

int CountProbe::minDate() const {
	if (_filter.scope != Scope::DateRange || !_filter.fromDate) {
		return 0;
	}
	// The server treats both date bounds as exclusive, while the scan
	// keeps the chosen days themselves, so each bound moves one step out.
	return int(_filter.fromDate) - 1;
}

int CountProbe::maxDate() const {
	if (_filter.scope != Scope::DateRange || !_filter.tillDate) {
		return 0;
	}
	return int(_filter.tillDate) + 1;
}

int CountProbe::minId() const {
	if (_filter.scope != Scope::IdRange || !_filter.fromId) {
		return 0;
	}
	return MtpId(_filter.fromId.bare - 1);
}

int CountProbe::maxId() const {
	if (_filter.scope != Scope::IdRange || !_filter.tillId) {
		return 0;
	}
	return MtpId(_filter.tillId.bare + 1);
}

void CountProbe::start() {
	if (_filter.scope == Scope::Selection || !IsValid(_filter)) {
		_text = QString();
		_countValue = 0;
		_lastId = 0;
		_method = ScanMethod::History;
		return;
	}
	const auto needsAccessHash = _peer->isUser() || _peer->isChannel();
	if (needsAccessHash && !_peer->isLoaded()) {
		_timer.callOnce(kPeerWaitInterval);
		return;
	}
	if (boundsChanged(_filter)) {
		_boundsScope = _filter.scope;
		_boundsFromDate = _filter.fromDate;
		_boundsTillDate = _filter.tillDate;
		_boundsFromId = _filter.fromId;
		_boundsTillId = _filter.tillId;
		_counts.clear();
		_total = -1;
		_totalNewestId = 0;
		_totalFailed = false;
		_countValue = 0;
		_lastId = 0;
		_method = ScanMethod::Unknown;
	}
	const auto generation = ++_generation;
	const auto needsText = (_filter.types & MediaType::Text).value() != 0;
	// The history total is always read: the preview needs it for text and the
	// scan method needs it to weigh the indexed matches against the chat.
	if (_total < 0 && !_totalFailed) {
		requestTotal(generation);
	}
	auto pieces = SelectedSearchPieces(_filter.types);
	if (needsText) {
		for (const auto piece : AllSearchPieces()) {
			if (std::find(pieces.begin(), pieces.end(), piece)
				== pieces.end()) {
				pieces.push_back(piece);
			}
		}
	}
	for (const auto piece : pieces) {
		if (!_counts.contains(int(piece))) {
			requestPiece(generation, int(piece));
		}
	}
	tryPublish(generation);
}

void CountProbe::requestPiece(int generation, int piece) {
	const auto weak = base::make_weak(this);
	RequestCount(
		_session,
		_peer,
		_topicRootId,
		minDate(),
		maxDate(),
		minId(),
		maxId(),
		SearchFilterFor(SearchPiece(piece)),
		[=](CountResult result) {
			if (const auto strong = weak.get()) {
				if (result.count < 0) {
					strong->failed(generation, piece);
				} else {
					strong->arrived(
						generation,
						piece,
						result.count,
						result.newestId);
				}
			}
		});
}

void CountProbe::requestTotal(int generation) {
	const auto weak = base::make_weak(this);
	const auto done = [=](CountResult result) {
		if (const auto strong = weak.get()) {
			if (result.count < 0) {
				strong->failed(generation, -1);
			} else {
				strong->arrived(
					generation,
					-1,
					result.count,
					result.newestId);
			}
		}
	};
	if (_filter.scope == Scope::DateRange) {
		// History has no date bounds, so a date range needs a search total.
		RequestCount(
			_session,
			_peer,
			_topicRootId,
			minDate(),
			maxDate(),
			minId(),
			maxId(),
			MTPMessagesFilter(MTP_inputMessagesFilterEmpty()),
			done);
	} else {
		RequestHistoryTotal(
			_session,
			_peer,
			_topicRootId,
			minId(),
			maxId(),
			done);
	}
}

void CountProbe::arrived(
		int generation,
		int piece,
		int count,
		MsgId newestId) {
	if (generation != _generation) {
		return;
	}
	if (piece < 0) {
		_total = count;
		_totalNewestId = newestId;
	} else {
		_counts[piece] = Cached{
			.count = count,
			.newestId = newestId,
		};
	}
	tryPublish(generation);
}

void CountProbe::failed(int generation, int piece) {
	if (generation != _generation) {
		return;
	}
	if (piece < 0) {
		_totalFailed = true;
	} else {
		_counts[piece] = Cached{ .failed = true };
	}
	tryPublish(generation);
}

void CountProbe::publishMethod() {
	if (_method.current() != ScanMethod::Unknown) {
		return;
	}
	const auto piece = SingleSearchPiece(_filter.types);
	if (!piece
		|| _filter.scope == Scope::DateRange
		|| _peer->migrateFrom()) {
		_method = ScanMethod::History;
		return;
	}
	const auto i = _counts.find(int(*piece));
	if (i == end(_counts)) {
		return;
	} else if (i->second.failed) {
		_method = ScanMethod::History;
		return;
	}
	if (_total < 0) {
		if (_totalFailed) {
			_method = ScanMethod::History;
		}
		return;
	}
	const auto covered = i->second.count;
	_method = (_total > 0 && covered * 2 <= _total)
		? ScanMethod::Search
		: ScanMethod::History;
}

void CountProbe::tryPublish(int generation) {
	if (generation != _generation
		|| _filter.scope == Scope::Selection
		|| !IsValid(_filter)) {
		return;
	}
	publishMethod();
	const auto types = _filter.types;
	auto count = 0;
	for (const auto piece : SelectedSearchPieces(types)) {
		const auto i = _counts.find(int(piece));
		if (i == end(_counts)) {
			return;
		} else if (i->second.failed) {
			_text = QString();
			_countValue = 0;
			_lastId = 0;
			return;
		}
		count += i->second.count;
	}
	auto exact = true;
	const auto needsText = (types & MediaType::Text).value() != 0;
	if (needsText) {
		exact = false;
		if (_total < 0) {
			if (_totalFailed) {
				_text = QString();
				_countValue = 0;
				_lastId = 0;
			}
			return;
		}
		auto all = 0;
		for (const auto piece : AllSearchPieces()) {
			const auto i = _counts.find(int(piece));
			if (i == end(_counts)) {
				return;
			} else if (i->second.failed) {
				_text = QString();
				_countValue = 0;
				_lastId = 0;
				return;
			}
			all += i->second.count;
		}
		count += std::max(0, _total - all);
	}
	auto lastId = MsgId(0);
	for (const auto piece : SelectedSearchPieces(types)) {
		const auto i = _counts.find(int(piece));
		if (i != end(_counts) && i->second.newestId > lastId) {
			lastId = i->second.newestId;
		}
	}
	if (needsText && _totalNewestId > lastId) {
		lastId = _totalNewestId;
	}
	if (_lastId.current() != lastId) {
		_lastId = lastId;
	}
	if ((types & MediaType::Sticker).value()
		|| (types & MediaType::Other).value()
		|| _filter.extensionMode != ExtensionMode::None
		|| _filter.minSize > 0
		|| _filter.maxSize > 0
		|| _peer->migrateFrom()) {
		exact = false;
	}
	if (_countValue.current() != count) {
		_countValue = count;
	}
	_text = exact
		? tr::lng_bulk_forward_count(tr::now, lt_count, count)
		: tr::lng_bulk_forward_count_about(tr::now, lt_count, count);
}

} // namespace BulkForward
