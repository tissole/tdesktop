/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/flat_map.h"
#include "base/timer.h"
#include "data/chunk_limiter.h"
#include "mtproto/core_types.h"
#include "scheme.h"

#include <QByteArray>

#include <map>
#include <memory>

namespace MTP {
class Error;
struct Response;
} // namespace MTP

namespace Main {
class Session;
} // namespace Main

namespace Data {

struct ChunkRequest {
	MTPInputFileLocation location;
	int64 offset = 0;
	int limit = 0;
	uint64 takeoutId = 0;
	MTP::ShiftedDcId dcId = 0;
};

struct ChunkResult {
	enum class Kind {
		Done,
		Reference,
		Failed,
	};

	Kind kind = Kind::Failed;
	QByteArray bytes;
};

using ChunkRequestId = uint64;

// The only sender of upload.getFile in the app. It admits every request
// through the per-DC ChunkLimiter, owns the retry decision, cancels what a
// transport cut would otherwise resend, and delivers each request exactly
// once. All download lanes of one bare DC share the limiter and window.
class FileChunkFetcher final {
public:
	explicit FileChunkFetcher(not_null<Main::Session*> session);
	~FileChunkFetcher();

	ChunkRequestId fetch(
		ChunkRequest request,
		Fn<void(ChunkResult)> done);
	void cancel(ChunkRequestId id);

	// Sum of the per-DC token rates, requests per second. The scan walker
	// derives its durable run-ahead credit from the shared limiter, so it
	// follows the achieved pace instead of an arbitrary item buffer.
	[[nodiscard]] double achievedRate() const;

	// Latest crl::now()-based time any DC is silent after a flood or cut,
	// so the scan UI can show the wait without a separate flood register.
	[[nodiscard]] crl::time unblockTime() const;

private:
	struct Job {
		ChunkRequest request;
		Fn<void(ChunkResult)> done;
		ChunkLimiter *limiter = nullptr;
		uint64 generation = 0;
		mtpRequestId requestId = 0;
		crl::time sentAt = 0;
		crl::time retryAt = 0;
		int attempts = 0;
		bool sent = false;
		bool sampleLatency = true;
	};

	struct Load {
		crl::time started = 0;
		crl::time lastActivity = 0;
		int requests = 0;
	};

	void pump();
	void send(ChunkRequestId id);
	void timeout(ChunkRequestId id, crl::time now);
	void handleResponse(ChunkRequestId id, const MTP::Response &response);
	void handleError(ChunkRequestId id, const MTP::Error &error);
	void checkTransport(crl::time now);
	void reissueAfterCut(MTP::DcId bareDc, crl::time now);
	void finish(ChunkRequestId id, ChunkResult result);
	void schedule(crl::time wait);
	void noteActivity(MTP::ShiftedDcId dcId);
	void closeIdleSessions();
	void scheduleIdleCheck();
	[[nodiscard]] bool hasJobsFor(MTP::ShiftedDcId dcId) const;
	void logStatus(crl::time now);
	[[nodiscard]] ChunkLimiter &limiterFor(MTP::ShiftedDcId dcId);

	const not_null<Main::Session*> _session;
	std::map<MTP::DcId, ChunkWindow> _windows;
	base::flat_map<
		MTP::DcId,
		std::unique_ptr<ChunkLimiter>> _limiters;
	base::flat_map<MTP::ShiftedDcId, uint64> _seenCuts;
	base::flat_map<MTP::ShiftedDcId, Load> _loads;
	std::map<ChunkRequestId, Job> _jobs;
	base::Timer _timer;
	base::Timer _idleTimer;
	ChunkRequestId _nextId = 1;
	crl::time _lastStatus = 0;
	bool _pumping = false;

};

} // namespace Data
