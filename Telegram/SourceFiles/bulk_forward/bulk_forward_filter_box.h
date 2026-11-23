/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "bulk_forward/bulk_forward_filter.h"
#include "data/data_msg_id.h"
#include "ui/layers/generic_box.h"

class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace BulkForward {

struct FilterBoxArgs {
	Filter filter;
	bool selectionAvailable = false;
	Main::Session *session = nullptr;
	PeerData *peer = nullptr;
	MsgId topicRootId = 0;
	Fn<void(Filter)> submit;
};

[[nodiscard]] object_ptr<Ui::GenericBox> FilterBox(FilterBoxArgs &&args);

} // namespace BulkForward
