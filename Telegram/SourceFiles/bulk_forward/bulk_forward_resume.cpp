/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk_forward/bulk_forward_resume.h"

#include "bulk_forward/bulk_forward_counters.h"
#include "base/debug_log.h"
#include "core/application.h"
#include "data/data_dedup_db.h"
#include "data/data_download_manager.h"
#include "main/main_session.h"

#include <QDataStream>

#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>

namespace BulkForward {
namespace {

constexpr auto kSerializationVersion = quint32(7);
constexpr auto kMaxDestinations = quint32(1000);
constexpr auto kMaxCursors = quint32(1000);
constexpr auto kMaxCarriers = quint32(1000);
constexpr auto kMaxCarrierDone = quint32(1000);
constexpr auto kMaxHashSize = 1024;

struct Tracked {
	uint64 jobId = 0;
	std::optional<ResumeState> restored;
};

base::flat_map<uint64, Tracked> &Registry() {
	static auto result = base::flat_map<uint64, Tracked>();
	return result;
}

[[nodiscard]] QByteArray SerializeState(const ResumeState &state) {
	auto data = QByteArray();
	auto stream = QDataStream(&data, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_6_0);
	stream << kSerializationVersion;
	stream << quint64(state.source.value);
	stream << qint64(state.sourceTopicRootId.bare);
	stream << Serialize(state.filter);
	stream << quint32(state.destinations.size());
	for (const auto &destination : state.destinations) {
		stream << quint64(destination.peer.value);
		stream << qint64(destination.topicRootId.bare);
		stream << qint32(destination.sent);
		stream << qint32(destination.skipped);
		stream << qint32(destination.failed);
	}
	stream << quint32(state.cursors.size());
	for (const auto &[peer, cursor] : state.cursors) {
		stream << quint64(peer.value);
		stream << qint64(cursor.bare);
	}
	stream << qint32(state.scanned);
	stream << qint32(state.total);
	stream << qint32(state.matched);
	stream << qint64(state.knownBytes);
	stream << qint64(state.doneBytes);
	stream << qint64(state.skippedBytes);
	stream << qint64(state.startedAt);
	stream << quint32(state.carriers.size());
	for (const auto &carrier : state.carriers) {
		stream << quint64(carrier.source.peer.value);
		stream << qint64(carrier.source.msg.bare);
		stream << quint64(carrier.carrierPeer.value);
		stream << qint64(carrier.carrierId.bare);
		stream << quint64(carrier.mediaId);
		stream << carrier.isPhoto;
		stream << carrier.hash;
		stream << quint32(carrier.done.size());
		for (const auto &peer : carrier.done) {
			stream << quint64(peer.value);
		}
	}
	stream << qint32(state.previewTotal);
	stream << quint8(state.method);
	return data;
}

[[nodiscard]] std::optional<ResumeState> DeserializeState(
		const QByteArray &data) {
	if (data.isEmpty()) {
		return std::nullopt;
	}
	auto stream = QDataStream(data);
	stream.setVersion(QDataStream::Qt_6_0);

	auto version = quint32(0);
	stream >> version;
	if (version < 2 || version > kSerializationVersion) {
		return std::nullopt;
	}

	auto result = ResumeState();
	result.version = int(version);
	auto source = quint64(0);
	auto sourceTopic = qint64(0);
	stream >> source;
	stream >> sourceTopic;
	if (!source || sourceTopic < 0) {
		return std::nullopt;
	}
	result.source = PeerId(source);
	result.sourceTopicRootId = MsgId(sourceTopic);

	auto filterData = QByteArray();
	stream >> filterData;
	const auto filter = Deserialize(filterData);
	if (!filter) {
		return std::nullopt;
	}
	result.filter = *filter;

	auto destinations = quint32(0);
	stream >> destinations;
	if (destinations > kMaxDestinations) {
		return std::nullopt;
	}
	for (auto i = quint32(0); i != destinations; ++i) {
		auto peer = quint64(0);
		auto topic = qint64(0);
		auto sent = qint32(0);
		auto skipped = qint32(0);
		auto failed = qint32(0);
		stream >> peer;
		stream >> topic;
		stream >> sent;
		stream >> skipped;
		if (version >= 5) {
			stream >> failed;
		}
		if (!peer || topic < 0 || sent < 0 || skipped < 0 || failed < 0) {
			return std::nullopt;
		}
		result.destinations.push_back({
			.peer = PeerId(peer),
			.topicRootId = MsgId(topic),
			.sent = sent,
			.skipped = skipped,
			.failed = failed,
		});
	}
	if (result.destinations.empty()) {
		return std::nullopt;
	}

	auto cursors = quint32(0);
	stream >> cursors;
	if (cursors > kMaxCursors) {
		return std::nullopt;
	}
	for (auto i = quint32(0); i != cursors; ++i) {
		auto peer = quint64(0);
		auto cursor = qint64(0);
		stream >> peer;
		stream >> cursor;
		if (!peer || cursor < 0) {
			return std::nullopt;
		}
		result.cursors[PeerId(peer)] = MsgId(cursor);
	}

	auto scanned = qint32(0);
	auto total = qint32(0);
	auto matched = qint32(0);
	stream >> scanned;
	stream >> total;
	stream >> matched;
	if (scanned < 0 || total < 0 || matched < 0) {
		return std::nullopt;
	}
	result.scanned = scanned;
	result.total = total;
	result.matched = matched;
	if (version >= 3) {
		auto knownBytes = qint64(0);
		auto doneBytes = qint64(0);
		stream >> knownBytes;
		stream >> doneBytes;
		if (knownBytes < 0 || doneBytes < 0) {
			return std::nullopt;
		}
		result.knownBytes = knownBytes;
		result.doneBytes = doneBytes;
		if (version >= 5) {
			auto skippedBytes = qint64(0);
			stream >> skippedBytes;
			if (skippedBytes < 0) {
				return std::nullopt;
			}
			result.skippedBytes = skippedBytes;
		}
	}
	if (version >= 4) {
		auto startedAt = qint64(0);
		stream >> startedAt;
		if (startedAt < 0) {
			return std::nullopt;
		}
		result.startedAt = startedAt;
	}

	auto carriers = quint32(0);
	stream >> carriers;
	if (carriers > kMaxCarriers) {
		return std::nullopt;
	}
	for (auto i = quint32(0); i != carriers; ++i) {
		auto sourcePeer = quint64(0);
		auto sourceMsg = qint64(0);
		auto carrierPeer = quint64(0);
		auto carrierMsg = qint64(0);
		auto mediaId = quint64(0);
		auto isPhoto = bool(false);
		auto hash = QByteArray();
		auto doneCount = quint32(0);
		stream >> sourcePeer;
		stream >> sourceMsg;
		stream >> carrierPeer;
		stream >> carrierMsg;
		stream >> mediaId;
		stream >> isPhoto;
		stream >> hash;
		stream >> doneCount;
		if (!sourcePeer
			|| sourceMsg < 0
			|| !carrierPeer
			|| carrierMsg <= 0
			|| hash.size() > kMaxHashSize
			|| doneCount > kMaxCarrierDone) {
			return std::nullopt;
		}
		auto item = ResumeCarrierItem{
			.source = FullMsgId(PeerId(sourcePeer), MsgId(sourceMsg)),
			.carrierPeer = PeerId(carrierPeer),
			.carrierId = MsgId(carrierMsg),
			.mediaId = mediaId,
			.isPhoto = isPhoto,
			.hash = std::move(hash),
		};
		for (auto j = quint32(0); j != doneCount; ++j) {
			auto peer = quint64(0);
			stream >> peer;
			if (!peer) {
				return std::nullopt;
			}
			item.done.insert(PeerId(peer));
		}
		result.carriers.push_back(std::move(item));
	}
	if (version >= 6) {
		auto previewTotal = qint32(0);
		stream >> previewTotal;
		if (previewTotal < 0) {
			return std::nullopt;
		}
		result.previewTotal = previewTotal;
	}
	if (version >= 7) {
		auto method = quint8(0);
		stream >> method;
		if (method > quint8(ScanMethod::Search)) {
			return std::nullopt;
		}
		result.method = ScanMethod(method);
	}
	if (stream.status() != QDataStream::Ok) {
		return std::nullopt;
	}
	if (!IsValid(result.filter)) {
		return std::nullopt;
	}
	return result;
}

} // namespace

void EnsureResumeSeeded(not_null<Main::Session*> session) {
	const auto key = session->uniqueId();
	auto &registry = Registry();
	if (registry.contains(key)) {
		return;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}

	auto &entry = registry[key];
	session->lifetime().add([key, session] {
		Registry().erase(key);
		RemoveAllCounters(session);
	});

	for (const auto &job : db.findActiveJobs(
			Data::JobKind::BulkForward,
			key)) {
		if (auto parsed = DeserializeState(job.options)) {
			if (!parsed->startedAt && job.createdAt > 0) {
				parsed->startedAt = job.createdAt;
			}
			entry.jobId = job.jobId;
			entry.restored = std::move(parsed);
			break;
		}
		db.cancelJob(job.jobId);
	}
	if (!entry.restored) {
		return;
	}

	const auto &state = *entry.restored;
	LOG(("BulkForward: Restored paused run, source %1, %2 destination(s), "
		"scanned %3, matched %4, %5 carrier record(s)")
		.arg(state.source.value)
		.arg(int(state.destinations.size()))
		.arg(state.scanned)
		.arg(state.matched)
		.arg(int(state.carriers.size())));
	auto counters = std::vector<Counters>();
	counters.reserve(state.destinations.size());
	const auto selection = UsesSelection(state.filter);
	for (const auto &destination : state.destinations) {
		const auto sent = destination.sent;
		const auto skipped = destination.skipped;
		const auto failed = destination.failed;
		counters.push_back({
			.session = session.get(),
			.source = state.source,
			.destination = destination.peer,
			.scanned = state.scanned,
			.total = state.total,
			.matched = std::max({
				state.matched,
				sent + skipped + failed,
				state.previewTotal }),
			.sent = sent,
			.skipped = skipped,
			.failed = failed,
			// The saved `sent` already includes every copy that settled
			// before the restart, so the copy rows must not add them again
			// while the run waits for its resume.
			.copied = sent,
			.knownBytes = selection ? 0 : state.knownBytes,
			.doneBytes = selection ? 0 : state.doneBytes,
			.skippedBytes = selection ? 0 : state.skippedBytes,
			.startedAt = state.startedAt,
			.paused = true,
		});
	}
	UpdateCounters(std::move(counters));
}

std::optional<ResumeState> TakeSeededResume(
		not_null<Main::Session*> session) {
	const auto it = Registry().find(session->uniqueId());
	if (it == end(Registry())) {
		return std::nullopt;
	}
	return std::exchange(it->second.restored, std::nullopt);
}

void SaveResume(
		not_null<Main::Session*> session,
		const ResumeState &state,
		bool paused) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen() || state.destinations.empty()) {
		return;
	}
	const auto key = session->uniqueId();
	auto &entry = Registry()[key];

	auto record = Data::JobRecord();
	record.kind = Data::JobKind::BulkForward;
	record.sessionId = key;
	record.scopeId = state.source.value;
	record.options = SerializeState(state);
	record.state = paused
		? Data::JobState::Pending
		: Data::JobState::InFlight;
	record.total = state.matched;
	for (const auto &destination : state.destinations) {
		record.done = std::max(record.done, destination.sent);
		record.skipped = std::max(record.skipped, destination.skipped);
	}
	if (entry.jobId) {
		db.setJobState(entry.jobId, record.state);
		db.setJobProgress(
			entry.jobId,
			record.total,
			record.done,
			record.skipped);
		db.updateJobOptions(entry.jobId, record.options);
	} else {
		entry.jobId = db.createJob(record);
	}
}

void ClearResume(not_null<Main::Session*> session) {
	const auto key = session->uniqueId();
	auto &entry = Registry()[key];
	const auto jobId = entry.jobId;
	entry.jobId = 0;
	entry.restored = std::nullopt;

	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	if (jobId) {
		db.cancelJob(jobId);
	}
	for (const auto &job : db.findActiveJobs(
			Data::JobKind::BulkForward,
			key)) {
		db.cancelJob(job.jobId);
	}
}

} // namespace BulkForward
