/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "enhanced_forward.h"

#include "base/timer.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"

namespace EnhancedForward {
namespace {

struct SharedState {
	int total = 0;
	int sent = 0;
	bool cancelled = false;
	bool finished = false;
	std::unique_ptr<base::Timer> finishTimer;
};

using StateMap = std::unordered_map<PeerId, SharedState>;

StateMap &ActiveStates() {
	static StateMap map;
	return map;
}

void fireUpdate(not_null<Main::Session*> session, const PeerId &peer) {
	session->changes().peerUpdated(
		session->data().peer(peer),
		Data::PeerUpdate::Flag::Slowmode);
}

} // namespace

bool isForwardNeeded(not_null<HistoryItem*> item) {
	if (item->isNoForwards()) {
		LOG(("Enhanced Forward: item %1 blocked by per-message NoForwards"
			).arg(item->id.bare));
		return true;
	}
	const auto sourcePeer = item->history()->peer;
	const auto media = item->media();
	const auto hasMedia = (media != nullptr);
	LOG(("Enhanced Forward: checking item %1, peer=%2, hasMedia=%3"
		).arg(item->id.bare
		).arg(sourcePeer->name()
		).arg(hasMedia ? 1 : 0));
	if (const auto channel = sourcePeer->asChannel()) {
		if (channel->flags() & ChannelData::Flag::NoForwards) {
			LOG(("Enhanced Forward: item %1 blocked by channel NoForwards"
				).arg(item->id.bare));
			return true;
		}
	}
	if (const auto user = sourcePeer->asUser()) {
		if (user->flags() & UserDataFlag::NoForwardsPeerEnabled) {
			LOG(("Enhanced Forward: item %1 blocked by user NoForwards"
				).arg(item->id.bare));
			return true;
		}
	}
	if (const auto chat = sourcePeer->asChat()) {
		if (chat->flags() & ChatData::Flag::NoForwards) {
			LOG(("Enhanced Forward: item %1 blocked by chat NoForwards"
				).arg(item->id.bare));
			return true;
		}
	}
	LOG(("Enhanced Forward: item %1 NOT restricted, using normal forward"
		).arg(item->id.bare));
	return false;
}

bool anyItemNeedsForward(
		const std::vector<not_null<HistoryItem*>> &items) {
	for (const auto &item : items) {
		const auto needed = isForwardNeeded(item);
		LOG(("Enhanced Forward: anyItemNeedsForward item %1 => %2"
			).arg(item->id.bare).arg(needed));
		if (needed) return true;
	}
	return false;
}

void startForwardSession(
		not_null<Main::Session*> session,
		const PeerId &peerId,
		int totalItems) {
	auto &states = ActiveStates();
	states.erase(peerId);

	auto &state = states[peerId];
	state.total = totalItems;
	state.sent = 0;
	state.cancelled = false;
	state.finished = false;

	fireUpdate(session, peerId);
}

void markItemSent(
		not_null<Main::Session*> session,
		const PeerId &peerId) {
	auto &states = ActiveStates();
	const auto it = states.find(peerId);
	if (it == states.end()) return;

	auto &state = it->second;
	if (state.cancelled || state.finished) return;

	state.sent++;
	fireUpdate(session, peerId);

	if (state.sent >= state.total) {
		state.finished = true;
		state.finishTimer = std::make_unique<base::Timer>([=] {
			auto &states = ActiveStates();
			states.erase(peerId);
			fireUpdate(session, peerId);
		});
		state.finishTimer->callOnce(3000);
	}
}

void cancelForward(
		const PeerId &id,
		not_null<Main::Session*> session) {
	auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return;

	it->second.cancelled = true;
	fireUpdate(session, id);
	states.erase(it);
}

bool isForwarding(const PeerId &id) {
	const auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return false;
	return !it->second.cancelled && !it->second.finished;
}

ForwardProgress currentProgress(const PeerId &id) {
	ForwardProgress result;
	const auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) {
		result.state = State::Idle;
		return result;
	}
	const auto &state = it->second;
	result.sent = state.sent;
	result.total = state.total;

	if (state.cancelled) {
		result.state = State::Cancelled;
	} else if (state.finished) {
		result.state = State::Finished;
	} else {
		result.state = State::Sending;
	}

	return result;
}

} // namespace EnhancedForward
