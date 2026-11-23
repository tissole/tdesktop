/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk_forward/bulk_forward_settings.h"

#include "core/application.h"
#include "core/core_settings.h"

#include <QtCore/QByteArray>

#include <string_view>

namespace BulkForward {
namespace {

constexpr auto kKey = std::string_view("bulk_forward_filter");

[[nodiscard]] Filter WithoutSelection(Filter filter) {
	filter.selection.clear();
	if (filter.scope == Scope::Selection) {
		filter.scope = Scope::All;
	}
	return filter;
}

} // namespace

Filter LoadLastFilter() {
	const auto stored = Core::App().settings().readPref<QByteArray>(kKey);
	const auto parsed = Deserialize(stored);
	return parsed ? WithoutSelection(*parsed) : Filter();
}

void SaveLastFilter(const Filter &filter) {
	Core::App().settings().writePref<QByteArray>(
		kKey,
		Serialize(WithoutSelection(filter)));
}

} // namespace BulkForward
