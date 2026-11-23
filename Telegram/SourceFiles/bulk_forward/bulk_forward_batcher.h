/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "bulk_forward/bulk_forward_classifier.h"
#include "base/basic_types.h"
#include "data/data_types.h"

#include <vector>

namespace BulkForward {

// A run that can be sent by one request: one source chat, one routing
// decision, at most 100 ids, and never a cut in the middle of an album.
struct Batch {
	PeerId sourcePeer;
	std::vector<SourceMessage> items;
	bool isProtected = false;
};

// Turns the ascending stream reported by a scanner into send-ready batches.
//
// An album is a run of consecutive messages sharing a group id, so a batch that
// would end inside such a run is shortened to the start of the run and the rest
// waits for the next chunk. A run at the very end of a chunk is held back too,
// because its remaining members may arrive on the next page; the caller reports
// the end of the stream with flush().
class Batcher final {
public:
	void push(std::vector<SourceMessage> items);
	void flush();
	void reset();

	// Items pushed but not yet emitted as batches.
	[[nodiscard]] const std::vector<SourceMessage> &pending() const {
		return _pending;
	}

	// Called in order for every finished batch.
	Fn<void(Batch)> ready;

private:
	void drain(bool final);

	std::vector<SourceMessage> _pending;

};

} // namespace BulkForward
