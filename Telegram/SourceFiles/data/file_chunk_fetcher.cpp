/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/file_chunk_fetcher.h"

#include "base/flat_set.h"
#include "base/random.h"
#include "data/data_file_hash.h"
#include "logs.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/facade.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_response.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace Data {
namespace {

constexpr auto kMaxAttempts = 4;
constexpr auto kRequestTimeout = crl::time(60000);
constexpr auto kMinPoll = crl::time(1);
constexpr auto kMaxPoll = crl::time(250);
constexpr auto kRetryBase = crl::time(500);
constexpr auto kRetryCap = crl::time(8000);
constexpr auto kStatusInterval = crl::time(15000);
constexpr auto kIdleSessionClose = crl::time(30000);
constexpr auto kIdleCheckInterval = crl::time(5000);

[[nodiscard]] bool IsDedupSession(MTP::ShiftedDcId dcId) {
	const auto shift = MTP::GetDcIdShift(dcId);
	return shift >= kDedupChunkDcShift
		&& shift < kDedupChunkDcShift + kDedupChunkLaneCount;
}

[[nodiscard]] crl::time DedupLaneStartupDelay(int lane) {
	const auto group = lane / kDedupChunkLaneGroup;
	return crl::time(group * kDedupChunkLaneStartupStep)
		+ crl::time(base::RandomIndex(kDedupChunkLaneJitter + 1));
}

[[nodiscard]] crl::time FloodJitter(ChunkRequestId id, int64 offset) {
	return crl::time((id * 13 + uint64(offset)) % 2100);
}

[[nodiscard]] crl::time RetryJitter(ChunkRequestId id, int attempts) {
	return crl::time((id * 37 + uint64(attempts) * 13) % 500);
}

[[nodiscard]] crl::time RetryBackoff(int attempts) {
	const auto shift = std::min(attempts - 1, 4);
	return std::min(kRetryCap, kRetryBase * (crl::time(1) << shift));
}

[[nodiscard]] QByteArray BytesFromResponse(const MTP::Response &response) {
	auto data = response.reply.constData();
	if (!data || response.reply.isEmpty()) {
		return QByteArray();
	}
	auto result = MTPupload_File();
	if (!result.read(data, data + response.reply.size())
		|| result.type() != mtpc_upload_file) {
		return QByteArray();
	}
	return result.c_upload_file().vbytes().v;
}

} // namespace

FileChunkFetcher::FileChunkFetcher(not_null<Main::Session*> session)
: _session(session)
, _timer([=] { pump(); })
, _idleTimer([=] { closeIdleSessions(); }) {
}

FileChunkFetcher::~FileChunkFetcher() {
	for (const auto &[id, job] : _jobs) {
		if (job.sent && job.requestId) {
			_session->account().mtp().cancel(job.requestId);
		}
	}
}

ChunkRequestId FileChunkFetcher::fetch(
		ChunkRequest request,
		Fn<void(ChunkResult)> done) {
	const auto id = _nextId++;
	auto &job = _jobs[id];
	job.request = std::move(request);
	job.done = std::move(done);
	job.limiter = &limiterFor(job.request.dcId);

	const auto lane = DedupChunkLaneIndex(job.request.dcId);
	if (lane >= 0
		&& _laneReadyAt.find(job.request.dcId) == _laneReadyAt.end()) {
		const auto now = crl::now();
		if (!_startupAt) {
			_startupAt = now;
		}
		_laneReadyAt.emplace(
			job.request.dcId,
			_startupAt + DedupLaneStartupDelay(lane));
	}
	pump();
	return id;
}

void FileChunkFetcher::cancel(ChunkRequestId id) {
	const auto i = _jobs.find(id);
	if (i == _jobs.end()) {
		return;
	}
	if (i->second.sent && i->second.requestId) {
		_session->account().mtp().cancel(i->second.requestId);
	}
	noteActivity(i->second.request.dcId);
	_jobs.erase(i);
	pump();
}

double FileChunkFetcher::achievedRate() const {
	auto result = 0.;
	for (const auto &entry : _limiters) {
		result += entry.second->rate();
	}
	return result;
}

crl::time FileChunkFetcher::unblockTime() const {
	auto result = crl::time(0);
	for (const auto &entry : _limiters) {
		result = std::max(result, entry.second->quietUntil());
	}
	return result;
}

void FileChunkFetcher::pump() {
	if (_pumping) {
		return;
	}
	if (_jobs.empty()) {
		_laneReadyAt.clear();
		_startupAt = 0;
		_timer.cancel();
		return;
	}
	_pumping = true;
	const auto now = crl::now();
	checkTransport(now);
	for (auto &entry : _limiters) {
		entry.second->tick(now);
	}
	logStatus(now);

	auto expired = std::vector<ChunkRequestId>();
	auto ready = std::vector<ChunkRequestId>();
	auto wait = kMaxPoll;
	for (auto &[id, job] : _jobs) {
		if (job.sent) {
			if (now - job.sentAt >= kRequestTimeout) {
				expired.push_back(id);
			}
		} else if (job.retryAt > now) {
			wait = std::min(wait, job.retryAt - now);
		} else {
			const auto readyAt = _laneReadyAt.find(job.request.dcId);
			if (readyAt != _laneReadyAt.end() && now < readyAt->second) {
				wait = std::min(wait, readyAt->second - now);
			} else {
				const auto admission = job.limiter->acquire(now);
				if (!admission.wait) {
					job.generation = admission.generation;
					ready.push_back(id);
				} else {
					wait = std::min(wait, admission.wait);
				}
			}
		}
	}
	_pumping = false;
	for (const auto id : expired) {
		timeout(id, crl::now());
	}
	for (const auto id : ready) {
		send(id);
	}
	schedule(wait);
}

void FileChunkFetcher::send(ChunkRequestId id) {
	const auto i = _jobs.find(id);
	if (i == _jobs.end()) {
		return;
	}
	auto &job = i->second;
	if (job.sent) {
		return;
	}
	const auto request = job.request;
	job.sent = true;
	job.sentAt = crl::now();

	auto &load = _loads[request.dcId];
	if (!load.started) {
		load.started = job.sentAt;
		_seenCuts[request.dcId] =
			MTP::details::TransportFloodState(request.dcId).cuts;
	}
	// The first request on a lane pays for the session initConnection and is
	// not a server queueing signal; every later request is sampled normally.
	job.sampleLatency = (load.requests > 0);
	load.lastActivity = job.sentAt;
	++load.requests;
	scheduleIdleCheck();

	const auto inner = MTPupload_GetFile(
		MTP_flags(MTPupload_GetFile::Flag::f_precise),
		request.location,
		MTP_long(request.offset),
		MTP_int(request.limit));
	auto onDone = [=](const MTP::Response &response) {
		handleResponse(id, response);
		return true;
	};
	auto onFail = [=](const MTP::Error &error, const MTP::Response &) {
		handleError(id, error);
		return true;
	};
	auto &mtp = _session->account().mtp();
	auto mtpId = mtpRequestId(0);
	if (request.takeoutId) {
		mtpId = mtp.send(
			MTPInvokeWithTakeout<MTPupload_GetFile>(
				MTP_long(request.takeoutId),
				inner),
			std::move(onDone),
			std::move(onFail),
			request.dcId);
	} else {
		mtpId = mtp.send(
			inner,
			std::move(onDone),
			std::move(onFail),
			request.dcId);
	}
	if (const auto j = _jobs.find(id); j != _jobs.end()) {
		j->second.requestId = mtpId;
	}
}

void FileChunkFetcher::timeout(ChunkRequestId id, crl::time now) {
	const auto i = _jobs.find(id);
	if (i == _jobs.end()) {
		return;
	}
	auto &job = i->second;
	if (!job.sent) {
		return;
	}
	_session->account().mtp().cancel(job.requestId);
	noteActivity(job.request.dcId);
	job.sent = false;
	job.requestId = 0;
	job.limiter->done(
		now - job.sentAt,
		0,
		job.generation,
		job.sampleLatency);
	job.retryAt = now;
	if (++job.attempts > kMaxAttempts) {
		const auto done = std::move(job.done);
		_jobs.erase(i);
		if (done) {
			done(ChunkResult{ ChunkResult::Kind::Failed, QByteArray() });
		}
	}
}

void FileChunkFetcher::handleResponse(
		ChunkRequestId id,
		const MTP::Response &response) {
	const auto i = _jobs.find(id);
	if (i == _jobs.end()) {
		return;
	}
	auto &job = i->second;
	if (!job.sent) {
		return;
	}
	job.sent = false;
	job.requestId = 0;
	noteActivity(job.request.dcId);
	const auto bytes = BytesFromResponse(response);
	job.limiter->done(
		crl::now() - job.sentAt,
		int(bytes.size()),
		job.generation,
		job.sampleLatency);
	if (bytes.isEmpty()) {
		finish(id, ChunkResult{ ChunkResult::Kind::Failed, QByteArray() });
	} else {
		finish(id, ChunkResult{ ChunkResult::Kind::Done, bytes });
	}
	pump();
}

void FileChunkFetcher::handleError(
		ChunkRequestId id,
		const MTP::Error &error) {
	const auto i = _jobs.find(id);
	if (i == _jobs.end()) {
		return;
	}
	auto &job = i->second;
	if (!job.sent) {
		return;
	}
	const auto now = crl::now();
	const auto took = now - job.sentAt;
	job.sent = false;
	job.requestId = 0;
	noteActivity(job.request.dcId);

	const auto type = error.type();
	if (const auto seconds = FloodWaitSeconds(type)) {
		job.limiter->flood(now, seconds, job.generation);
		if (++job.attempts > kMaxAttempts) {
			finish(id, ChunkResult{ ChunkResult::Kind::Failed, QByteArray() });
		} else {
			job.retryAt = now
				+ crl::time(seconds) * 1000
				+ FloodJitter(id, job.request.offset);
		}
		pump();
		return;
	}

	job.limiter->done(took, 0, job.generation, job.sampleLatency);
	if (type.startsWith(u"FILE_REFERENCE_"_q)) {
		finish(id, ChunkResult{ ChunkResult::Kind::Reference, QByteArray() });
		pump();
		return;
	}
	if (++job.attempts > kMaxAttempts) {
		finish(id, ChunkResult{ ChunkResult::Kind::Failed, QByteArray() });
	} else {
		job.retryAt = now
			+ RetryBackoff(job.attempts)
			+ RetryJitter(id, job.attempts);
	}
	pump();
}

void FileChunkFetcher::checkTransport(crl::time now) {
	// A cut on any download lane means the whole bare DC has to back off, so
	// all lanes share one limiter and one generation.
	auto cutDcs = base::flat_set<MTP::DcId>();
	for (const auto &[shiftedDcId, load] : _loads) {
		const auto state = MTP::details::TransportFloodState(shiftedDcId);
		auto &seen = _seenCuts[shiftedDcId];
		if (state.cuts == seen) {
			continue;
		}
		seen = state.cuts;
		cutDcs.insert(MTP::BareDcId(shiftedDcId));
	}
	for (const auto bareDc : cutDcs) {
		const auto i = _limiters.find(bareDc);
		if (i == end(_limiters)) {
			continue;
		}
		const auto generation = i->second->generation();
		i->second->cut(now);
		if (i->second->generation() == generation) {
			continue;
		}
		reissueAfterCut(bareDc, now);
	}
}

void FileChunkFetcher::reissueAfterCut(MTP::DcId bareDc, crl::time now) {
	for (auto &[shiftedDcId, load] : _loads) {
		if (MTP::BareDcId(shiftedDcId) == bareDc) {
			load = Load();
			load.started = now;
			load.lastActivity = now;
		}
	}
	auto ids = std::vector<ChunkRequestId>();
	for (const auto &[id, job] : _jobs) {
		if (job.sent && MTP::BareDcId(job.request.dcId) == bareDc) {
			ids.push_back(id);
		}
	}
	for (const auto id : ids) {
		const auto i = _jobs.find(id);
		if (i == _jobs.end()) {
			continue;
		}
		auto &job = i->second;
		_session->account().mtp().cancel(job.requestId);
		job.sent = false;
		job.requestId = 0;
		job.retryAt = now;
		if (++job.attempts > kMaxAttempts) {
			const auto done = std::move(job.done);
			_jobs.erase(i);
			if (done) {
				done(ChunkResult{ ChunkResult::Kind::Failed, QByteArray() });
			}
		}
	}
}

void FileChunkFetcher::finish(ChunkRequestId id, ChunkResult result) {
	const auto i = _jobs.find(id);
	if (i == _jobs.end()) {
		return;
	}
	auto done = std::move(i->second.done);
	_jobs.erase(i);
	if (done) {
		done(std::move(result));
	}
}

void FileChunkFetcher::schedule(crl::time wait) {
	if (_jobs.empty()) {
		_timer.cancel();
		return;
	}
	_timer.callOnce(std::clamp(wait, kMinPoll, kMaxPoll));
}

void FileChunkFetcher::noteActivity(MTP::ShiftedDcId dcId) {
	const auto i = _loads.find(dcId);
	if (i == end(_loads)) {
		return;
	}
	i->second.lastActivity = crl::now();
	scheduleIdleCheck();
}

void FileChunkFetcher::closeIdleSessions() {
	const auto now = crl::now();
	for (auto i = begin(_loads); i != end(_loads);) {
		const auto dcId = i->first;
		if (!IsDedupSession(dcId) || hasJobsFor(dcId)) {
			++i;
			continue;
		}
		if (now - i->second.lastActivity < kIdleSessionClose) {
			++i;
			continue;
		}
		_session->account().mtp().stopSession(dcId);
		_seenCuts.erase(dcId);
		i = _loads.erase(i);
	}
	scheduleIdleCheck();
}

void FileChunkFetcher::scheduleIdleCheck() {
	auto hasDedupSessions = false;
	for (const auto &entry : _loads) {
		if (IsDedupSession(entry.first)) {
			hasDedupSessions = true;
			break;
		}
	}
	if (!hasDedupSessions) {
		_idleTimer.cancel();
		return;
	}
	_idleTimer.callOnce(kIdleCheckInterval);
}

bool FileChunkFetcher::hasJobsFor(MTP::ShiftedDcId dcId) const {
	for (const auto &entry : _jobs) {
		if (entry.second.request.dcId == dcId) {
			return true;
		}
	}
	return false;
}

void FileChunkFetcher::logStatus(crl::time now) {
	if (now - _lastStatus < kStatusInterval) {
		return;
	}
	_lastStatus = now;
	auto loads = base::flat_map<MTP::DcId, Load>();
	for (const auto &[shiftedDcId, load] : _loads) {
		auto &dest = loads[MTP::BareDcId(shiftedDcId)];
		if (!dest.started
			|| (load.started && load.started < dest.started)) {
			dest.started = load.started;
		}
		dest.requests += load.requests;
	}
	for (const auto &entry : _limiters) {
		const auto &limiter = *entry.second;
		auto &load = loads[entry.first];
		LOG(("Chunk dc:%1 state:%2 rate:%3 cwnd:%4 inflight:%5 p95:%6ms avg:%7ms age:%8s req:%9"
			).arg(entry.first
			).arg(int(limiter.currentState())
			).arg(int(limiter.rate())
			).arg(limiter.window()
			).arg(limiter.inFlight()
			).arg(int(limiter.latencyP95())
			).arg(int(limiter.latencyAvg())
			).arg(load.started ? int((now - load.started) / 1000) : 0
			).arg(load.requests));
	}
}

ChunkLimiter &FileChunkFetcher::limiterFor(MTP::ShiftedDcId dcId) {
	const auto bareDc = MTP::BareDcId(dcId);
	auto &slot = _limiters[bareDc];
	if (!slot) {
		slot = std::make_unique<ChunkLimiter>(
			bareDc,
			&_windows[bareDc]);
	}
	return *slot;
}

} // namespace Data
