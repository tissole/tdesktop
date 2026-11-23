/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "bulk_forward/bulk_forward_filter.h"
#include "base/basic_types.h"
#include "data/data_msg_id.h"
#include "data/data_types.h"

class PeerData;

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionNavigation;
} // namespace Window

namespace BulkForward {

// Opens the filter box for a chat, then the destination chooser, then runs the
// job. Only one bulk forward runs at a time.
void ShowBox(
	not_null<Window::SessionNavigation*> navigation,
	not_null<PeerData*> peer,
	MsgId topicRootId,
	MessageIdsList selection);

void CancelActive(not_null<Main::Session*> session);

// Pauses or resumes the single active job of the session. A run restored
// after a restart is rebuilt from its durable record on resume.
void PauseActive(not_null<Main::Session*> session);
void ResumeActive(not_null<Main::Session*> session);

} // namespace BulkForward
