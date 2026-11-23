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
#include "base/weak_ptr.h"
#include "data/data_msg_id.h"
#include "mtproto/core_types.h"
#include "scheme.h"

#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace BulkForward {

// Reads the details of a frozen selection in batches, classifies every message
// and reports the ids that pass the content filter. It never touches the chat
// selection and never resolves items through the materialized session cache.
class SelectionScanner final : public base::has_weak_ptr {
public:
	explicit SelectionScanner(not_null<Main::Session*> session);
	~SelectionScanner();

	void start(
		MessageIdsList ids,
		Filter filter,
		Fn<void(std::vector<SourceMessage>)> batch,
		Fn<void()> finished,
		Fn<void(QString)> fail);
	void pause();
	void resume();
	void cancel();

	[[nodiscard]] bool paused() const {
		return _paused;
	}

private:
	struct Batch {
		PeerId peerId;
		MessageIdsList ids;
	};

	void requestNextBatch();
	void acquireTakeout(not_null<PeerData*> peer);
	void handleBatch(
		const MTPmessages_Messages &result,
		PeerId peerId);
	void handleMessages(
		const QVector<MTPMessage> &messages,
		PeerId peerId);
	void releaseTakeout();
	void finish();
	void fail(const QString &error);

	const not_null<Main::Session*> _session;
	Fn<void(std::vector<SourceMessage>)> _done;
	Fn<void()> _finished;
	Fn<void(QString)> _fail;
	std::vector<Batch> _batches;
	std::vector<SourceMessage> _matched;
	Filter _filter;
	int _batchIndex = 0;
	mtpRequestId _requestId = 0;
	uint64 _takeoutId = 0;
	bool _paused = false;
	bool _cancelled = false;
	bool _takeoutRequested = false;
	bool _takeoutBorrowed = false;
	bool _takeoutFallbackTried = false;

};

} // namespace BulkForward
