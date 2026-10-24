/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "chat_scan/chat_scan_api.h"
#include "export/view/export_view_top_bar.h"
#include "rpl/event_stream.h"

#include <memory>
#include <optional>

namespace Window {
class SessionController;
} // namespace Window

class PeerData;

namespace ChatScan {

class ScanTopBar final : public Export::View::TopBar {
public:
	using TopBar::TopBar;

protected:
	[[nodiscard]] TextWithEntities leftTitle() const override;

};

struct ScanTopBarEvent {
	PeerData *peer = nullptr;
	std::shared_ptr<Api> api;
};

[[nodiscard]] rpl::event_stream<ScanTopBarEvent> &TopBarEvents();

void FireScanTopBarEvent(ScanTopBarEvent &&event);

void ClearScanTopBarEvent();

[[nodiscard]] std::optional<ScanTopBarEvent> CurrentScanTopBarEvent();

void ActivateScanTopBar(
	not_null<Window::SessionController*> clickController,
	const ScanTopBarEvent &event);

[[nodiscard]] Export::View::Content TopBarContent(
	const State &state,
	const QString &peerName);

} // namespace ChatScan
