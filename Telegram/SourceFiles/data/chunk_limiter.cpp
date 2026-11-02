/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/chunk_limiter.h"

#include "logs.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Data {
namespace {

constexpr auto kCwndMin = 24;
constexpr auto kCwndMax = 96;
constexpr auto kCwndSteady = 80;
// Only small sampled reads feed the congestion signal. Whole-file reads are
// throughput work, not RTT, and used to keep p95 high long enough to collapse
// the window. The 1600 ms/240 experiment let the server queue up to 37 s, so
// a 3000 ms hard cut now reacts before queueing can run away.
//
// Growth is gated by the distance between the smoothed latency and the
// smallest latency seen in the current epoch, not by an absolute target: a
// link whose healthy RTT is already close to a fixed target could never grow
// the window back after a single spike.
constexpr auto kQueueBudgetMs = 1000.;
constexpr auto kQueueCollapseMs = 3000.;
constexpr auto kLatencyHardLimitMs = 3000.;
constexpr auto kLatencySampleMaxBytes = 32 * 1024;
// A shorter latency window is not trusted for growth or collapse decisions:
// its samples may still describe the regime that was just corrected.
constexpr auto kLatencySamplesForTrust = 8;
// Per-completion growth stops here; above it the once-a-second tick decides,
// so the ramp cannot fill the server queue in a few seconds the way the old
// 17 -> 85 in 30 s did.
constexpr auto kSlowStartCeiling = 32;
constexpr auto kCwndGrowthPerTick = 2;
constexpr auto kCollapseCooldown = crl::time(2000);

constexpr auto kStartRateFactor = 0.4;
constexpr auto kWarmGrowth = 1.15;
constexpr auto kSteadyGrowthPerSecond = 10.;
constexpr auto kGrowInterval = crl::time(1000);
constexpr auto kRefillClamp = crl::time(1000);
// The pump runs on every response and the bucket refills between pumps. A cap
// of one token clipped that credit and the rate settled at ~80% of the ceiling;
// a few tokens of credit let the next pump spend the difference.
constexpr auto kTokenBurst = 4.;
constexpr auto kRetryIn = crl::time(20);

constexpr auto kFloodRateFactor = 0.6;
constexpr auto kProofFloor = crl::time(5000);
constexpr auto kCutSilence = crl::time(3000);
constexpr auto kCutProofWindow = crl::time(2000);
constexpr auto kCutMergeWindow = crl::time(5000);
constexpr auto kLifetimeProof = crl::time(150000);
constexpr auto kThirdCutProofWindow = crl::time(30000);
constexpr auto kThirdCutInterval = crl::time(240000);
constexpr auto kStrikeInterval = crl::time(5000);
constexpr auto kMaxStrikes = 6;
constexpr auto kStrikeDecay = crl::time(60000);
// Current lane experiment: all DCs are capped at 115/s and fall back to 110/s
// after a cut. All lanes share one limiter/window per bare DC; repeated cuts
// keep the fallback instead of halving it.
constexpr auto kCutCeilingFactor = 1.;

constexpr auto kMaxJitter = 1000;

// The scan comparison needs both chunk plans to run with the same window, so
// the limiter's own controller must not be the variable. Setting this pins the
// window and keeps only the rate ceiling and the flood/cut handling active.
[[nodiscard]] int FixedWindowFromEnvironment() {
	static const auto result = []() -> int {
		const auto value = std::getenv("TDESKTOP_DEDUP_CWND");
		const auto parsed = value ? std::atoi(value) : 0;
		if (parsed <= 0) {
			return 0;
		}
		const auto window = std::clamp(parsed, kCwndMin, kChunkWindowLimit);
		LOG(("ChunkLimiter: pinned window %1 by TDESKTOP_DEDUP_CWND."
			).arg(window));
		return window;
	}();
	return result;
}

} // namespace

ChunkLimits chunkLimitsForDc(int) {
	// Current settings: dc:1 through dc:5 are 115 / 80, post-cut 110.
	return { 115, 80, 110 };
}

ChunkLimiter::ChunkLimiter(
	int dcId,
	ChunkWindow *window)
: _dcId(dcId)
, _limits(chunkLimitsForDc(dcId))
, _window(window)
, _fixedCwnd(FixedWindowFromEnvironment())
, _rate(_limits.ceiling * kStartRateFactor)
, _tokens(kTokenBurst)
, _cwnd(_fixedCwnd ? _fixedCwnd : kCwndMin)
, _seed(0x9E3779B97F4A7C15ULL + uint64(dcId)) {
}

ChunkAdmission ChunkLimiter::acquire(crl::time now) {
	if (!_healthySince) {
		_healthySince = now;
	}
	refill(now);
	if (now < _quietUntil) {
		return { _quietUntil - now, 0 };
	}
	if (_window && _window->used >= _window->limit) {
		return { kRetryIn, 0 };
	}
	if (_inFlight >= _cwnd) {
		return { kRetryIn, 0 };
	}
	if (_tokens < 1.) {
		return { msUntilToken(), 0 };
	}
	_tokens -= 1.;
	++_inFlight;
	if (_window) {
		++_window->used;
	}
	return { 0, _generation };
}

void ChunkLimiter::done(
		crl::time took,
		int bytes,
		uint64 generation,
		bool sampleLatency) {
	if (generation != _generation) {
		return;
	}
	release();
	if (sampleLatency && bytes > 0 && bytes <= kLatencySampleMaxBytes) {
		pushLatency(took);
	}
	if (_fixedCwnd) {
		return;
	}
	// Slow start: after a collapse the window climbs back through completions
	// instead of waiting a full second per step.
	if (_state != ChunkState::Backoff
		&& _cwnd < kSlowStartCeiling
		&& delayOkay()) {
		++_cwnd;
	}
}

void ChunkLimiter::flood(crl::time now, int seconds, uint64 generation) {
	if (generation != _generation) {
		return;
	}
	release();
	const auto value = crl::time(std::max(seconds, 0));
	_state = ChunkState::Backoff;
	_strikes = std::min(_strikes + 1, kMaxStrikes);
	_lastStrike = now;
	_healthySince = now;
	_slowTicks = 0;
	_rate = std::max(double(_limits.floor), _rate * kFloodRateFactor);
	if (!_fixedCwnd) {
		_cwnd = std::max(kCwndMin, _cwnd / 2);
	}
	_quietUntil = now + value * 1000 + jitter();
	// Recover five seconds after the flood wait ends; multiplying the wait
	// kept the limiter slow for minutes after a long FLOOD_WAIT.
	_nextGrow = _quietUntil
		+ kProofFloor
		+ crl::time(_strikes - 1) * kStrikeInterval;
}

void ChunkLimiter::cut(crl::time now) {
	// One incident usually kills the connection more than once while it is
	// reconnecting; those repeats are the same cut and must not escalate.
	if (_lastCut && now - _lastCut < kCutMergeWindow) {
		return;
	}
	const auto previousCut = _lastCut;
	// The DC ends a connection after its ~5 minute lifetime with a -429 and
	// keeps a short ban. That is not a rate punishment, so keep the learned
	// rate and window and resume at full speed once the ban is over.
	const auto lifetime = !_strikes
		&& _ceilingFactor >= 1.
		&& _healthySince
		&& now - _healthySince >= kLifetimeProof;

	++_generation;
	if (_window) {
		_window->used = std::max(0, _window->used - _inFlight);
	}
	_inFlight = 0;
	_quietUntil = now + kCutSilence + jitter();
	_lastCut = now;
	_healthySince = now;
	_slowTicks = 0;
	if (_limits.ceiling > _limits.cutFallback) {
		_fallbackCeiling = _limits.cutFallback;
		_rate = std::min(_rate, double(_limits.cutFallback));
	}

	if (lifetime) {
		return;
	}

	if (!_fixedCwnd) {
		_cwnd = kCwndMin;
	}
	resetLatency();
	_state = ChunkState::Backoff;
	_strikes = std::min(_strikes + 1, kMaxStrikes);
	_lastStrike = now;
	_nextGrow = now
		+ kCutProofWindow
		+ crl::time(_strikes - 1) * kStrikeInterval;
	_recentCuts = (previousCut && now - previousCut < kThirdCutInterval)
		? (_recentCuts + 1)
		: 1;
	if (_recentCuts >= 3) {
		_ceilingFactor = kCutCeilingFactor;
		_nextGrow = now + kThirdCutProofWindow;
	}
}

void ChunkLimiter::tick(crl::time now) {
	if (_strikes && now - _lastStrike >= kStrikeDecay) {
		_strikes = 0;
	}
	if (now - _lastCut >= kThirdCutInterval) {
		_ceilingFactor = 1.;
		_recentCuts = 0;
	}
	if (_state == ChunkState::Backoff) {
		if (now < _nextGrow) {
			return;
		}
		_state = ChunkState::Warm;
	}
	if (now < _nextGrow) {
		return;
	}
	_nextGrow = now + kGrowInterval;
	if (_fixedCwnd) {
		// The comparison run keeps the window pinned, but the rate still has
		// to open up to the ceiling or the pin would never be reached.
		_slowTicks = 0;
		_rate = std::min(_rate + kSteadyGrowthPerSecond, effectiveCeiling());
		_state = ChunkState::Steady;
		return;
	}
	if ((_latencyP95 > kLatencyHardLimitMs
			|| _latencyAvg > (_minRtt + kQueueCollapseMs))
		&& _latencyCount >= kLatencySamplesForTrust) {
		collapse(now);
		return;
	}
	if (!delayOkay()) {
		if (++_slowTicks >= 2) {
			_slowTicks = 0;
			_cwnd = std::max<int>(kCwndMin, int(_cwnd * 0.95));
			if (_state == ChunkState::Steady) {
				_state = ChunkState::Warm;
			}
		}
		return;
	}
	_slowTicks = 0;
	const auto ceiling = effectiveCeiling();
	if (_state == ChunkState::Warm) {
		_rate = std::min(_rate * kWarmGrowth, ceiling);
	} else {
		_rate = std::min(_rate + kSteadyGrowthPerSecond, ceiling);
	}
	_cwnd = std::min(_cwnd + kCwndGrowthPerTick, kCwndMax);
	if (_rate >= ceiling && _cwnd >= kCwndSteady) {
		_state = ChunkState::Steady;
	}
}

bool ChunkLimiter::delayOkay() const {
	if (_fixedCwnd || _latencyCount < kLatencySamplesForTrust) {
		return true;
	}
	const auto floor = (_minRtt > 0.) ? _minRtt : _latencyAvg;
	return (_latencyAvg <= floor + kQueueBudgetMs);
}

void ChunkLimiter::collapse(crl::time now) {
	// The requests admitted under the old window are still outstanding; the
	// fetcher cancels and re-queues them by watching this counter. Dropping
	// the in-flight count here keeps the window and the real concurrency equal
	// instead of letting the old batch drain for a minute.
	++_generation;
	++_collapses;
	if (_window) {
		_window->used = std::max(0, _window->used - _inFlight);
	}
	_inFlight = 0;
	if (!_fixedCwnd) {
		_cwnd = kCwndMin;
	}
	_state = ChunkState::Warm;
	_slowTicks = 0;
	_nextGrow = now + kCollapseCooldown;
	// The samples that caused the collapse describe the old window; keeping
	// them would hold p95 high and block the recovery for minutes.
	resetLatency();
}

void ChunkLimiter::resetLatency() {
	_latencies = {};
	_latencyCount = 0;
	_latencyNext = 0;
	_latencyAvg = 0.;
	_latencyP95 = 0.;
	_minRtt = 0.;
}

double ChunkLimiter::effectiveCeiling() const {
	auto base = double(_limits.ceiling);
	if (_fallbackCeiling > 0) {
		base = std::min(base, double(_fallbackCeiling));
	}
	return base * _ceilingFactor;
}

crl::time ChunkLimiter::msUntilToken() const {
	if (_rate <= 0.) {
		return kRetryIn;
	}
	const auto ms = (1. - _tokens) / _rate * 1000.;
	return crl::time(std::max(1., std::ceil(ms)));
}

crl::time ChunkLimiter::jitter() {
	_seed = _seed * 6364136223846793005ULL + 1442695040888963407ULL;
	return crl::time((_seed >> 33) % uint64(kMaxJitter));
}

void ChunkLimiter::refill(crl::time now) {
	if (_lastRefill && now > _lastRefill) {
		const auto delta = std::min(now - _lastRefill, kRefillClamp);
		_tokens = std::min(kTokenBurst, _tokens + double(delta) * _rate / 1000.);
	}
	_lastRefill = now;
}

void ChunkLimiter::release() {
	if (_inFlight > 0) {
		--_inFlight;
	}
	if (_window && _window->used > 0) {
		--_window->used;
	}
}

void ChunkLimiter::pushLatency(crl::time took) {
	_latencies[_latencyNext] = took;
	_latencyNext = (_latencyNext + 1) % kLatencyWindow;
	// The first sample after a reset seeds the average; starting from zero
	// would make the queue estimate read low for the next twenty samples.
	_latencyAvg = (_latencyCount > 0)
		? (_latencyAvg * 0.9 + double(took) * 0.1)
		: double(took);
	_latencyCount = std::min(_latencyCount + 1, kLatencyWindow);
	const auto value = double(took);
	if (_minRtt <= 0. || value < _minRtt) {
		_minRtt = value;
	}
	auto sorted = _latencies;
	std::sort(sorted.begin(), sorted.begin() + _latencyCount);
	const auto index = std::min(
		_latencyCount - 1,
		(_latencyCount * 95) / 100);
	_latencyP95 = double(sorted[index]);
}

} // namespace Data
