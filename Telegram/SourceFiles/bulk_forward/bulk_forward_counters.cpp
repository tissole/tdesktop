/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk_forward/bulk_forward_counters.h"

#include "base/flat_map.h"
#include "rpl/event_stream.h"
#include "rpl/producer.h"

#include <iterator>
#include <utility>

namespace BulkForward {
namespace {

using Key = std::pair<Main::Session*, PeerId>;

base::flat_map<Key, Counters> &Registry() {
	static auto result = base::flat_map<Key, Counters>();
	return result;
}

rpl::event_stream<> &Changes() {
	static auto result = rpl::event_stream<>();
	return result;
}

} // namespace

void UpdateCounters(std::vector<Counters> counters) {
	if (counters.empty()) {
		return;
	}
	for (auto &entry : counters) {
		entry.active = true;
		Registry()[{ entry.session, entry.destination }] = std::move(entry);
	}
	Changes().fire({});
}

void RemoveCounters(Main::Session *session, const PeerId &destination) {
	if (Registry().erase({ session, destination })) {
		Changes().fire({});
	}
}

void RemoveAllCounters(not_null<Main::Session*> session) {
	auto &registry = Registry();
	auto removed = false;
	for (auto it = begin(registry); it != end(registry);) {
		if (it->first.first == session.get()) {
			it = registry.erase(it);
			removed = true;
		} else {
			++it;
		}
	}
	if (removed) {
		Changes().fire({});
	}
}

std::vector<Counters> AllCounters(not_null<Main::Session*> session) {
	auto result = std::vector<Counters>();
	for (const auto &entry : Registry()) {
		if (entry.second.session == session.get()) {
			result.push_back(entry.second);
		}
	}
	return result;
}

bool HasCounters(
		not_null<Main::Session*> session,
		const PeerId &destination) {
	return Registry().find({ session.get(), destination }) != end(Registry());
}

rpl::producer<> countersChanged() {
	return Changes().events();
}

} // namespace BulkForward
