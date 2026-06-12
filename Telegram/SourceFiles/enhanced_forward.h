/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_peer_id.h"

class HistoryItem;
class PhotoData;
class DocumentData;

namespace Main {
class Session;
} // namespace Main

namespace EnhancedForward {

struct ItemFlags {
	bool msg = false;
	bool peer = false;
	bool restricted = false; // msg || peer
};

struct Split {
	std::vector<not_null<HistoryItem*>> restricted;
	std::vector<not_null<HistoryItem*>> normal;
};

[[nodiscard]] ItemFlags checkItem(not_null<HistoryItem*> item);
[[nodiscard]] Split classifyItems(
	const std::vector<not_null<HistoryItem*>> &items);
[[nodiscard]] bool checkMsgRestriction(not_null<HistoryItem*> item);

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

void startForwardSession(
	not_null<Main::Session*> session,
	const PeerId &peerId,
	int totalItems);

void markItemSent(
	not_null<Main::Session*> session,
	const PeerId &peerId);

void cancelForward(
	const PeerId &id,
	not_null<Main::Session*> session);

[[nodiscard]] bool isForwarding(const PeerId &id);

[[nodiscard]] ForwardProgress currentProgress(const PeerId &id);

} // namespace EnhancedForward
