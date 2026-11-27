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
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_msg_id.h"
#include "rpl/variable.h"

class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace BulkForward {

// Debounced preview of how many messages the filter matches. Media types have
// server-side indexes and are counted exactly, while text and stickers have
// none, so their whole-history remainder is added as an estimate. Extension
// and size limits are applied later, during the scan, so they make the number
// rough as well.
class CountProbe final : public base::has_weak_ptr {
public:
	CountProbe(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		MsgId topicRootId);
	~CountProbe();

	[[nodiscard]] rpl::producer<QString> text() const;
	// The numeric value behind text(), or zero while it is still unknown.
	[[nodiscard]] rpl::producer<int> countValue() const;
	// The newest matching message id, or zero while it is still unknown.
	[[nodiscard]] rpl::producer<MsgId> lastId() const;
	// How the scan should read the matches. Stays Unknown until the indexed
	// count and the history total are known; falls back to History whenever
	// the selection cannot be walked with one search filter.
	[[nodiscard]] rpl::producer<ScanMethod> method() const;

	// Subscriptions made by the owner live as long as the probe.
	[[nodiscard]] rpl::lifetime &lifetime() {
		return _lifetime;
	}

	void request(Filter filter);

private:
	struct Cached {
		int count = 0;
		MsgId newestId;
		bool failed = false;
	};

	void start();
	void requestPiece(int generation, int piece);
	void requestTotal(int generation);
	void arrived(int generation, int piece, int count, MsgId newestId);
	void failed(int generation, int piece);
	void publishMethod();
	void tryPublish(int generation);

	[[nodiscard]] bool boundsChanged(const Filter &filter) const;
	[[nodiscard]] int minDate() const;
	[[nodiscard]] int maxDate() const;
	[[nodiscard]] int minId() const;
	[[nodiscard]] int maxId() const;

	const not_null<Main::Session*> _session;
	const not_null<PeerData*> _peer;
	const MsgId _topicRootId;
	base::Timer _timer;
	rpl::lifetime _lifetime;
	rpl::variable<QString> _text;
	rpl::variable<int> _countValue;
	rpl::variable<MsgId> _lastId;
	rpl::variable<ScanMethod> _method;
	Filter _filter;
	Scope _boundsScope = Scope::All;
	TimeId _boundsFromDate = 0;
	TimeId _boundsTillDate = 0;
	MsgId _boundsFromId = 0;
	MsgId _boundsTillId = 0;
	base::flat_map<int, Cached> _counts;
	int _total = -1;
	MsgId _totalNewestId;
	bool _totalFailed = false;
	int _generation = 0;

};

} // namespace BulkForward
