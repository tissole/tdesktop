/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/flat_set.h"

#include <QByteArray>

#include <utility>

namespace Data {

// Content already accepted by at least one destination of the current
// multi-destination forward. A fan-out sends the same file to every selected
// chat, so the global uploads rows created by the run itself must not make the
// other destinations skip that file. Rows left by earlier runs still count as
// duplicates. Shared by the plain bulk forwarder and the copy pipeline, so
// both halves of a mixed run agree on what this run is already sending.
struct ForwardRunSeen {
	base::flat_set<std::pair<uint64, bool>> media;
	base::flat_set<QByteArray> hashes;
};

} // namespace Data
