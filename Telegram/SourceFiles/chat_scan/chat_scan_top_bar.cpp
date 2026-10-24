/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_scan/chat_scan_top_bar.h"

#include "base/basic_types.h"
#include "chat_scan/view/chat_scan_box.h"
#include "data/data_peer.h"
#include "export/view/export_view_content.h"
#include "lang/lang_keys.h"
#include "window/window_session_controller.h"

#include <algorithm>

namespace ChatScan {

TextWithEntities ScanTopBar::leftTitle() const {
	return tr::lng_chat_scan_title(tr::now, tr::bold);
}

rpl::event_stream<ScanTopBarEvent> &TopBarEvents() {
	static auto events = rpl::event_stream<ScanTopBarEvent>();
	return events;
}

std::optional<ScanTopBarEvent> &TopBarEventCurrent() {
	static auto current = std::optional<ScanTopBarEvent>();
	return current;
}

void FireScanTopBarEvent(ScanTopBarEvent &&event) {
	TopBarEventCurrent() = event;
	TopBarEvents().fire(std::move(event));
}

void ClearScanTopBarEvent() {
	TopBarEventCurrent().reset();
}

std::optional<ScanTopBarEvent> CurrentScanTopBarEvent() {
	return TopBarEventCurrent();
}

void ActivateScanTopBar(
		not_null<Window::SessionController*> clickController,
		const ScanTopBarEvent &event) {
	if (!event.peer || !event.api) {
		return;
	}
	clickController->show(
		Box<ChatScanBox>(event.peer, false, true));
}

Export::View::Content TopBarContent(
		const State &state,
		const QString &peerName) {
	using Phase = State::Phase;
	using Row = Export::View::Content::Row;

	auto content = Export::View::Content();
	content.rows.push_back(Row{ u"scan"_q });
	auto &row = content.rows.back();
	row.label = peerName
		+ u" — "_q
		+ [&]() -> QString {
			switch (state.phase) {
			case Phase::Scanning:
				return tr::lng_chat_scan_scanning(tr::now);
			case Phase::Paused:
				return tr::lng_chat_scan_paused(tr::now);
			case Phase::Resolving:
				return tr::lng_chat_scan_resolving(tr::now);
			case Phase::Failed:
				return tr::lng_chat_scan_failed(tr::now);
			default:
				return QString();
			}
		}();
	const auto done = state.hashDone;
	const auto total = std::max(state.hashTotal, state.mediaTotal);
	if (total > 0) {
		row.info = QString::number(done)
			+ u" / "_q
			+ QString::number(total);
		row.progress = std::clamp(
			float64(done) / float64(total),
			0.,
			1.);
	}
	return content;
}

} // namespace ChatScan
