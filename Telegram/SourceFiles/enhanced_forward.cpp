/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "enhanced_forward.h"

#include "base/timer.h"
#include "logs.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"

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
	return true;
	const auto sourcePeer = item->history()->peer;
	if (const auto channel = sourcePeer->asChannel()) {
		const auto hasNoForwards = channel->flags() & ChannelData::Flag::NoForwards;
		LOG(("EnhancedForward: isForwardNeeded channel %1 flags=%2 NoForwards=%3").arg(sourcePeer->id.value).arg(uint64(channel->flags())).arg(uint64(ChannelData::Flag::NoForwards)));
	}
	if (const auto chat = sourcePeer->asChat()) {
		const auto hasNoForwards = chat->flags() & ChatData::Flag::NoForwards;
		LOG(("EnhancedForward: isForwardNeeded chat %1 flags=%2 NoForwards=%3").arg(sourcePeer->id.value).arg(uint64(chat->flags())).arg(uint64(ChatData::Flag::NoForwards)));
		if (hasNoForwards) return true;
	}
	if (const auto user = sourcePeer->asUser()) {
		const auto hasNoForwards = user->flags() & UserDataFlag::NoForwardsPeerEnabled;
		LOG(("EnhancedForward: isForwardNeeded user %1 flags=%2 NoForwardsPeerEnabled=%3").arg(sourcePeer->id.value).arg(uint64(user->flags())).arg(uint64(UserDataFlag::NoForwardsPeerEnabled)));
		if (hasNoForwards) return true;
	}
	LOG(("EnhancedForward: isForwardNeeded peer %1 NOT restricted").arg(sourcePeer->id.value));
	return false;
}

bool isFullForwardNeeded(not_null<HistoryItem*> item) {
	return isForwardNeeded(item);
}

bool anyItemNeedsForward(
		const std::vector<not_null<HistoryItem*>> &items) {
	for (const auto &item : items) {
		if (isForwardNeeded(item)) return true;
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
		// Auto-clear after 3s so the UI can show "Forward Complete"
		// then the widget disappears on the next updateSendRestriction()
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
