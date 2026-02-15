/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "export/export_controller.h"

namespace Export {
struct Settings;
} // namespace Export

namespace Export {
namespace View {

struct Content {
	struct Row {
		QString id;
		QString label;
		QString info;
		float64 progress = 0.;
		uint64 randomId = 0;
	};

	std::vector<Row> rows;
	bool isScanning = false;

	static const QString kDoneId;

};

[[nodiscard]] Content ContentFromState(
	const ProcessingState &state); // <-- REMOVE 'settings' argument
[[nodiscard]] Content ContentFromState(const FinishedState &state);

[[nodiscard]] inline auto ContentFromState(
		rpl::producer<State> state) {
	return std::move(
		state
	) | rpl::filter([](const State &state) {
		return v::is<ProcessingState>(state) || v::is<FinishedState>(state);
	}) | rpl::map([=](const State &state) {
		if (const auto process = std::get_if<ProcessingState>(&state)) {
			return ContentFromState(*process);
		} else if (v::is<FinishedState>(state)) {
			return ContentFromState(std::get<FinishedState>(state));
		} else {
			auto result = Content();
			result.rows.push_back({ Content::kDoneId });
			return result;
		}
	});
}

} // namespace View
} // namespace Export
