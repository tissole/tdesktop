/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_peer_id.h"
#include "main/main_session.h"

namespace EnhancedForward {

enum class State : uint8_t {
	Idle,
	Sending,
	Finished,
	Cancelled,
};

struct ForwardProgress {
	State state = State::Idle;
	int sent = 0;
	int total = 0;
};

// Check if a single item needs enhanced forward
[[nodiscard]] bool isForwardNeeded(not_null<HistoryItem*> item);

// Check if the full (without attribution) forward is needed
[[nodiscard]] bool isFullForwardNeeded(not_null<HistoryItem*> item);

// Check if any item in the list needs enhanced forward
[[nodiscard]] bool anyItemNeedsForward(
	const std::vector<not_null<HistoryItem*>> &items);

// Start a session to track progress for a forward operation
void startForwardSession(
	not_null<Main::Session*> session,
	const PeerId &peerId,
	int totalItems);

// Called each time one item has been dispatched (sent via
// sendMessage / SendExistingDocument / SendExistingPhoto / FileLoadTask)
void markItemSent(
	not_null<Main::Session*> session,
	const PeerId &peerId);

// Cancel an active forward operation
void cancelForward(
	const PeerId &id,
	not_null<Main::Session*> session);

// Check if a forward is currently in progress
[[nodiscard]] bool isForwarding(const PeerId &id);

// Get the current progress of an active forward
[[nodiscard]] ForwardProgress currentProgress(const PeerId &id);

} // namespace EnhancedForward