/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <crl/crl_time.h>

#include <array>

namespace Data {

enum class ChunkState {
	Warm,
	Steady,
	Backoff,
};

// One shared window per bare DC. All download lanes share it, so the total
// in-flight stays bounded while each lane carries only a few requests. The
// 16-slot layout with 128/96/1000 is the proven high-rate configuration.
constexpr auto kChunkWindowLimit = 128;

struct ChunkWindow {
	int limit = kChunkWindowLimit;
	int used = 0;
};

struct ChunkLimits {
	int ceiling = 130;
	// Lower bound for the multiplicative RPC FLOOD_WAIT backoff.
	int floor = 80;
	// Temporary ceiling after a transport -429 cut.
	int cutFallback = 125;
};

// |wait| is 0 when the request may be sent right now, and then |generation|
// tags it: a completion that arrives after a cut is recognised as stale.
struct ChunkAdmission {
	crl::time wait = 0;
	uint64 generation = 0;
};

[[nodiscard]] ChunkLimits chunkLimitsForDc(int dcId);

class ChunkLimiter final {
public:
	explicit ChunkLimiter(
		int dcId,
		ChunkWindow *window = nullptr);

	[[nodiscard]] ChunkAdmission acquire(crl::time now);
	void done(
		crl::time took,
		int bytes,
		uint64 generation,
		bool sampleLatency = true);
	void flood(crl::time now, int seconds, uint64 generation);
	void cut(crl::time now);
	void tick(crl::time now);

	[[nodiscard]] int dcId() const {
		return _dcId;
	}
	[[nodiscard]] int ceiling() const {
		return _limits.ceiling;
	}
	[[nodiscard]] double rate() const {
		return _rate;
	}
	[[nodiscard]] int window() const {
		return _cwnd;
	}
	[[nodiscard]] int inFlight() const {
		return _inFlight;
	}
	[[nodiscard]] ChunkState currentState() const {
		return _state;
	}
	[[nodiscard]] crl::time quietUntil() const {
		return _quietUntil;
	}
	[[nodiscard]] uint64 generation() const {
		return _generation;
	}
	[[nodiscard]] double latencyP95() const {
		return _latencyP95;
	}
	[[nodiscard]] double latencyAvg() const {
		return _latencyAvg;
	}

private:
	[[nodiscard]] double effectiveCeiling() const;
	[[nodiscard]] crl::time msUntilToken() const;
	crl::time jitter();
	void refill(crl::time now);
	void release();
	void pushLatency(crl::time took);

	const int _dcId;
	const ChunkLimits _limits;
	ChunkWindow *const _window = nullptr;

	double _rate = 0.;
	double _tokens = 1.;
	crl::time _lastRefill = 0;
	int _cwnd = 16;
	int _inFlight = 0;

	ChunkState _state = ChunkState::Warm;
	crl::time _quietUntil = 0;
	crl::time _nextGrow = 0;
	crl::time _lastStrike = 0;
	crl::time _lastCut = 0;
	crl::time _healthySince = 0;
	int _strikes = 0;
	int _recentCuts = 0;
	int _slowTicks = 0;
	int _fallbackCeiling = 0;
	double _ceilingFactor = 1.;
	uint64 _generation = 0;

	double _latencyAvg = 0.;
	double _latencyP95 = 0.;
	static constexpr auto kLatencyWindow = 64;
	std::array<crl::time, kLatencyWindow> _latencies = {};
	int _latencyCount = 0;
	int _latencyNext = 0;

	uint64 _seed = 0;

};

} // namespace Data
