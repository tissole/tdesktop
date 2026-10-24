/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/facade.h"

#include "base/flat_map.h"
#include "storage/localstorage.h"
#include "core/application.h"
#include "main/main_account.h"

namespace MTP {
namespace details {
namespace {

int PauseLevel = 0;
rpl::event_stream<> Unpaused;

// The server kills the connection of a flooded DC without a duration, so the
// pause is chosen here: long enough for the reconnect to settle, short enough
// to keep the run moving, and the DC keeps its slower pacing after that.
constexpr auto kTransportFloodSilence = crl::time(1000);

base::flat_map<ShiftedDcId, TransportFlood> TransportFloods;

} // namespace

bool paused() {
	return PauseLevel > 0;
}

void pause() {
	++PauseLevel;
}

void unpause() {
	--PauseLevel;
	if (!PauseLevel) {
		Unpaused.fire({});
	}
}

rpl::producer<> unpaused() {
	return Unpaused.events();
}

void NoteTransportFlood(ShiftedDcId shiftedDcId) {
	auto &flood = TransportFloods[shiftedDcId];
	flood.flooded = true;
	flood.silenceUntil = crl::now() + kTransportFloodSilence;
	++flood.cuts;
}

void NoteTransportFloodRecovered(ShiftedDcId shiftedDcId) {
	const auto i = TransportFloods.find(shiftedDcId);
	if (i == TransportFloods.end() || !i->second.flooded) {
		return;
	}
	i->second.silenceUntil = crl::now() + kTransportFloodSilence;
}

TransportFlood TransportFloodState(ShiftedDcId shiftedDcId) {
	const auto i = TransportFloods.find(shiftedDcId);
	return (i != TransportFloods.end()) ? i->second : TransportFlood();
}

} // namespace details
} // namespace MTP
