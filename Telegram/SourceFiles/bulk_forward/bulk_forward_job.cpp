/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk_forward/bulk_forward_job.h"

#include "api/api_common.h"
#include "base/debug_log.h"
#include "base/unixtime.h"
#include "bulk_forward/bulk_forward_counters.h"
#include "core/application.h"
#include "data/data_dedup_db.h"
#include "data/data_download_manager.h"
#include "data/data_forward_dedup.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "enhanced_forward.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "settings.h"

#include <crl/crl.h>

#include <algorithm>
#include <utility>

namespace BulkForward {
namespace {

constexpr auto kCheckpointInterval = crl::time(3000);

// The scan must not run far ahead of the sends. Telegram accepts about two
// thousand forwards per hour, so a larger backlog only costs requests and
// memory: the scan is held once this many matched messages are still waiting
// anywhere in the pipeline and released again when it drains below the low
// mark. The gap between the marks keeps the scanner from starting and stopping
// on every page.
constexpr auto kScanAheadHigh = 2000;
constexpr auto kScanAheadLow = 1000;

[[nodiscard]] QString CopyReportReason(
		EnhancedForward::SettleResult result) {
	switch (result) {
	case EnhancedForward::SettleResult::Created:
		return u"created"_q;
	case EnhancedForward::SettleResult::DestinationDuplicate:
		return u"destination duplicate"_q;
	case EnhancedForward::SettleResult::GlobalDuplicate:
		return u"global duplicate"_q;
	case EnhancedForward::SettleResult::CoveredByRun:
		return u"covered by run"_q;
	case EnhancedForward::SettleResult::Cancelled:
		break;
	}
	return u"cancelled"_q;
}

[[nodiscard]] bool DestinationOnlyDedup() {
	// A protected copy follows the same dedup rules as a normal server-side
	// forward: while forward dedup is on it consults the global uploads
	// table, and when dedup is off only the destination records matter.
	return !GetEnhancedBool("prevent_forward_duplicates");
}

struct ResumeRowsInfo {
	std::vector<Data::EfResumeItem> rows;
	bool valid = false;
};

[[nodiscard]] ResumeRowsInfo LoadResumeRows(
		not_null<Main::Session*> session,
		PeerId source,
		PeerId destination) {
	auto result = ResumeRowsInfo();
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return result;
	}
	const auto records = db.loadEfResumeItemsForPeer(
		session->uniqueId(),
		destination);
	for (const auto &record : records) {
		if (record.sourceId.peer == source) {
			result.rows.push_back(record);
		}
	}
	std::sort(
		result.rows.begin(),
		result.rows.end(),
		[](const Data::EfResumeItem &a, const Data::EfResumeItem &b) {
			return a.sourceId.msg < b.sourceId.msg;
		});
	result.valid = !result.rows.empty();
	return result;
}

} // namespace

Job::Job(not_null<Main::Session*> session)
: _session(session)
, _scanner(session)
, _selection(session) {
	_batcher.ready = [=](Batch batch) {
		enqueue(std::move(batch));
	};
}

Job::~Job() {
	cancel();
}

void Job::start(JobRequest &&request) {
	cancel();

	_finished = false;
	_failed = false;
	_scanStarted = false;
	_scanDone = false;
	_scanHeld = false;
	_scanMethod = ScanMethod::Unknown;
	_scanCursors.clear();
	_paused = false;
	_scanned = 0;
	_total = 0;
	_matched = 0;
	_firstMatchedId = 0;
	_lastMatchedId = 0;
	_scannedBase = 0;
	_matchedBase = 0;
	_knownBytes = 0;
	_doneBytes = 0;
	_skippedBytes = 0;
	_byteCounted.clear();
	_protectedBusyBytes = 0;
	_startedAt = base::unixtime::now();
	_previewTotal = 0;
	_previewLastId = 0;
	_lastCheckpoint = 0;
	_filter = request.filter;
	_source = request.source->id;
	_sourceTopicRootId = request.sourceTopicRootId;
	_batcher.reset();
	_forwarders.clear();
	_runSeen = (request.destinations.size() > 1)
		? std::make_shared<RunSeen>()
		: std::shared_ptr<RunSeen>();
	_inFlush = false;
	_donePending = false;
	_protectedBusy = false;
	_protectedBusyPeer = PeerId();
	_protectedBusyFrom = 0;
	_protectedBusyCount = 0;
	_protectedBusyItems.clear();
	_protectedSettled.clear();
	_protectedBusyDirectCopy = false;
	_protectedBusyDestination = PeerId();
	_protectedCounts.clear();
	_carrierResume = request.resume
		? request.resume->carriers
		: std::vector<ResumeCarrierItem>();
	_failures.clear();

	auto resume = request.resume ? &*request.resume : nullptr;
	if (resume && resume->startedAt > 0) {
		_startedAt = resume->startedAt;
	}
	if (resume && !UsesSelection(_filter)) {
		_scanMethod = resume->method;
	}
	auto resumeRows = ResumeRowsInfo();
	auto resumeSent = 0;
	auto resumeSentBytes = qint64(0);
	auto resumeAdvanced = false;
	auto resumeOverlapSent = 0;
	auto resumeOverlapBytes = qint64(0);
	if (resume
		&& !UsesSelection(_filter)
		&& request.destinations.size() == 1) {
		const auto destination = request.destinations.front().peer->id;
		resumeRows = LoadResumeRows(_session, _source, destination);
		if (resumeRows.valid) {
			const auto savedCursor = request.resume->cursors[_source];
			auto rows = std::vector<Data::EfResumeItem>();
			rows.reserve(resumeRows.rows.size());
			for (const auto &row : resumeRows.rows) {
				// Rows at or below the saved cursor belong to earlier runs of
				// the same copy job; only the current window can be resumed.
				if (row.sourceId.msg > savedCursor) {
					rows.push_back(row);
				}
			}
			auto sent = 0;
			auto sentBytes = qint64(0);
			for (const auto &row : rows) {
				if (row.state.toInt() >= 3) {
					++sent;
					sentBytes += std::max<qint64>(row.fileSize, 0);
				}
			}
			for (const auto &saved : resume->destinations) {
				if (saved.peer == destination) {
					sent = std::max(saved.sent, sent);
					break;
				}
			}
			resumeSent = sent;
			resumeSentBytes = sentBytes;
			auto firstUnsettled = MsgId();
			for (const auto &row : rows) {
				if (row.state.toInt() < 3) {
					firstUnsettled = row.sourceId.msg;
					break;
				}
			}
			if (firstUnsettled && !rows.empty()) {
				auto &cursor = request.resume->cursors[_source];
				const auto firstRowId = rows.front().sourceId.msg;
				if (cursor.bare >= firstRowId.bare - 1
					&& cursor.bare < firstUnsettled.bare - 1) {
					cursor = MsgId(firstUnsettled.bare - 1);
					resumeAdvanced = true;
				}
			}
			const auto cursor = request.resume->cursors[_source];
			const auto behindCursor = !firstUnsettled
				|| (cursor.bare >= firstUnsettled.bare - 1);
			if (!behindCursor) {
				resumeSent = 0;
				resumeSentBytes = 0;
				// The scan starts before these rows, so they are read and
				// counted again; remove them from the checkpoint base first.
				resumeOverlapSent = sent;
				resumeOverlapBytes = std::max(sentBytes, resume->doneBytes);
			}
			LOG(("BulkForward: Resume rows, sent %1, bytes %2, cursor %3")
				.arg(resumeSent)
				.arg(resumeSentBytes)
				.arg(cursor.bare));
		}
	}
	if (!resume) {
		ClearResume(_session);
		RemoveAllCounters(_session);
	} else if (!UsesSelection(_filter)) {
		_scannedBase = resume->scanned;
		_matchedBase = resume->matched;
		_total = resume->total;
		_knownBytes = resume->knownBytes;
		_doneBytes = resume->doneBytes;
		_skippedBytes = resume->skippedBytes;
		_previewTotal = resume->previewTotal;
		if (resumeSent > 0) {
			_knownBytes = std::max(_knownBytes, resumeSentBytes);
			_doneBytes = std::max(_doneBytes, resumeSentBytes);
		}
		if (resumeOverlapSent > 0) {
			_matchedBase = std::max(0, _matchedBase - resumeOverlapSent);
		}
		if (resumeOverlapBytes > 0) {
			_knownBytes = std::max<qint64>(
				0,
				_knownBytes - resumeOverlapBytes);
			_doneBytes = std::max<qint64>(
				0,
				_doneBytes - resumeOverlapBytes);
		}
		if (resumeAdvanced) {
			_matchedBase = std::max(_matchedBase, resumeSent);
		}
	}
	_scanned = _scannedBase;
	_matched = _matchedBase;

	if (request.destinations.empty()) {
		scanFailed(u"no destinations"_q);
		return;
	} else if (!IsValid(_filter)) {
		scanFailed(u"invalid filter"_q);
		return;
	}

	for (const auto &destination : request.destinations) {
		auto sent = 0;
		auto skipped = 0;
		auto failed = 0;
		if (resume && !UsesSelection(_filter)) {
			for (const auto &saved : resume->destinations) {
				if (saved.peer == destination.peer->id) {
					sent = saved.sent;
					skipped = saved.skipped;
					failed = saved.failed;
					break;
				}
			}
			sent = std::max(0, sent - resumeOverlapSent);
			sent = std::max(sent, resumeSent);
		}
		auto forwarder = std::make_unique<Forwarder>(
			_session,
			destination.peer,
			destination.topicRootId,
			_runSeen,
			sent,
			skipped);
		if (failed > 0) {
			_protectedCounts[destination.peer->id].failed = failed;
		}
		forwarder->progress = [=](int) {
			notifyUpdated();
		};
		forwarder->drained = [=] {
			updateScanFlow();
			checkDone();
		};
		forwarder->unhandled = [=](Batch batch) {
			enqueueProtected(std::move(batch));
		};
		const auto destinationId = forwarder->destinationId();
		forwarder->failed = [=](QString error) {
			destinationFailed(destinationId, std::move(error));
		};
		_forwarders.push_back(std::move(forwarder));
	}

	if (!UsesSelection(_filter)) {
		// The bar reports the whole matching set from the start, the same
		// number the filter box previews, instead of a page or a batch.
		_previewProbe = std::make_unique<CountProbe>(
			_session,
			request.source,
			_sourceTopicRootId);
		_previewProbe->countValue(
		) | rpl::on_next([=](int total) {
			if (_previewTotal != total) {
				_previewTotal = total;
				notifyUpdated();
			}
		}, _previewProbe->lifetime());
		_previewProbe->lastId(
		) | rpl::on_next([=](MsgId last) {
			if (_previewLastId != last) {
				_previewLastId = last;
				notifyUpdated();
			}
		}, _previewProbe->lifetime());
		_previewProbe->request(_filter);
	}

	if (UsesSelection(_filter)) {
		_selection.start(
			_filter.selection,
			_filter,
			[=](std::vector<SourceMessage> items) {
				_matched += int(items.size());
				scanBatch(std::move(items));
				updateScanFlow();
			},
			[=] {
				_scanDone = true;
				_scanHeld = false;
				flushBatcher();
				checkDone();
			},
			[=](QString error) {
				scanFailed(error);
			});
		notifyUpdated();
		return;
	}

	_scanCursors = resume
		? resume->cursors
		: base::flat_map<PeerId, MsgId>();
	if (_scanMethod != ScanMethod::Unknown) {
		startScan(_scanMethod);
	} else {
		_previewProbe->method(
		) | rpl::on_next([=](ScanMethod method) {
			applyScanMethod(method);
		}, _previewProbe->lifetime());
	}
	notifyUpdated();
}

void Job::pause() {
	if (_paused || _finished) {
		return;
	}
	_paused = true;
	updateScanFlow();
	for (const auto &forwarder : _forwarders) {
		forwarder->pause();
	}
	notifyUpdated();
}

void Job::resume() {
	if (!_paused || _finished) {
		return;
	}
	_paused = false;
	_lastCheckpoint = 0;
	applyScanMethod(_scanMethod);
	updateScanFlow();
	for (const auto &forwarder : _forwarders) {
		forwarder->resume();
	}
	processProtected();
	notifyUpdated();
}

void Job::cancel() {
	_scanner.cancel();
	_selection.cancel();
	_batcher.reset();
	_protectedQueue.clear();
	_protectedBusy = false;
	_protectedBusyPeer = PeerId();
	_protectedBusyFrom = 0;
	_protectedBusyCount = 0;
	_protectedBusyBytes = 0;
	_protectedBusyItems.clear();
	_protectedSettled.clear();
	_protectedBusyDirectCopy = false;
	_protectedBusyDestination = PeerId();
	++_protectedCopyToken;
	_fanout.reset();
	_carrierResume.clear();
	++_carrierToken;
	_previewProbe.reset();
	_previewTotal = 0;
	_previewLastId = 0;
	_knownBytes = 0;
	_doneBytes = 0;
	_skippedBytes = 0;
	_byteCounted.clear();
	finishCounters();
	for (const auto &forwarder : _forwarders) {
		forwarder->cancel();
	}
	_forwarders.clear();
	_scanStarted = false;
	_scanDone = false;
	_scanHeld = false;
	_scanMethod = ScanMethod::Unknown;
	_scanCursors.clear();
	_paused = false;
}

int Job::sent() const {
	auto result = 0;
	for (const auto &forwarder : _forwarders) {
		result += forwarder->sent();
	}
	for (const auto &entry : _protectedCounts) {
		result += entry.second.handed;
	}
	return result;
}

int Job::skipped() const {
	auto result = 0;
	for (const auto &forwarder : _forwarders) {
		result += forwarder->skipped();
	}
	for (const auto &entry : _protectedCounts) {
		result += entry.second.duplicates;
	}
	return result;
}

void Job::scanBatch(std::vector<SourceMessage> items) {
	if (items.empty()) {
		return;
	}
	for (const auto &item : items) {
		if (!_firstMatchedId || item.id.msg < _firstMatchedId) {
			_firstMatchedId = item.id.msg;
		}
		if (!_lastMatchedId || item.id.msg > _lastMatchedId) {
			_lastMatchedId = item.id.msg;
		}
	}
	_inFlush = true;
	_batcher.push(std::move(items));
	_inFlush = false;
	if (std::exchange(_donePending, false)) {
		checkDone();
	}
}

void Job::scanProgress(int scanned, int total, int matched) {
	_scanned = _scannedBase + scanned;
	_total = total;
	_matched = _matchedBase + matched;
	notifyUpdated();
}

void Job::scanFinished() {
	if (_finished || _failed) {
		return;
	}
	_scanDone = true;
	_scanHeld = false;
	flushBatcher();
	checkDone();
}

void Job::scanFailed(const QString &error) {
	if (_finished || _failed) {
		return;
	}
	_failed = true;
	_finished = true;
	_scanner.cancel();
	_selection.cancel();
	_fanout.reset();
	_carrierResume.clear();
	++_carrierToken;
	for (const auto &forwarder : _forwarders) {
		forwarder->cancel();
	}
	finishCounters();
	ClearResume(_session);
	if (failed) {
		failed(error);
	}
}

void Job::applyScanMethod(ScanMethod method) {
	if (method == ScanMethod::Unknown
		|| _scanStarted
		|| _scanDone
		|| _finished
		|| _failed) {
		return;
	}
	_scanMethod = method;
	if (!_paused) {
		startScan(method);
		notifyUpdated();
	}
}

void Job::startScan(ScanMethod method) {
	if (_scanStarted || !_source || _finished || _failed) {
		return;
	}
	_scanStarted = true;
	_scanMethod = method;
	auto callbacks = ScanCallbacks();
	callbacks.batch = [=](std::vector<SourceMessage> items) {
		scanBatch(std::move(items));
	};
	callbacks.progress = [=](int scanned, int total, int matched) {
		scanProgress(scanned, total, matched);
	};
	callbacks.done = [=] {
		scanFinished();
	};
	callbacks.fail = [=](QString error) {
		scanFailed(error);
	};
	_scanner.start(ScanRequest{
		.peer = _session->data().peer(_source),
		.topicRootId = _sourceTopicRootId,
		.filter = _filter,
		.method = method,
		.cursors = _scanCursors,
		.callbacks = std::move(callbacks),
	});
	_scanMethod = _scanner.method();
	if (_scanHeld) {
		_scanner.pause();
	}
}

void Job::destinationFailed(const PeerId &destination, QString error) {
	if (_finished || _failed) {
		return;
	}
	for (const auto &forwarder : _forwarders) {
		if (forwarder->destinationId() == destination) {
			forwarder->cancel();
			break;
		}
	}
	_failures.push_back(
		_session->data().peer(destination)->name()
		+ u" ("_q
		+ error
		+ u")"_q);
	auto alive = false;
	for (const auto &forwarder : _forwarders) {
		if (!forwarder->cancelled()) {
			alive = true;
			break;
		}
	}
	if (!alive) {
		_scanner.cancel();
		_selection.cancel();
		_scanDone = true;
	}
	RemoveCounters(_session.get(), destination);
	notifyUpdated();
	checkDone();
}

void Job::enqueue(Batch batch) {
	if (batch.isProtected) {
		enqueueProtected(std::move(batch));
		return;
	}
	for (const auto &forwarder : _forwarders) {
		forwarder->enqueue(batch);
	}
	updateScanFlow();
}

void Job::updateScanFlow() {
	if (_scanDone || _finished || _failed) {
		return;
	}
	const auto ahead = pendingAhead();
	const auto shouldHold = _paused
		|| (_scanHeld
			? (ahead > kScanAheadLow)
			: (ahead >= kScanAheadHigh));
	if (shouldHold == _scanHeld) {
		return;
	}
	_scanHeld = shouldHold;
	holdScan(shouldHold);
}

void Job::holdScan(bool held) {
	if (UsesSelection(_filter)) {
		if (held) {
			_selection.pause();
		} else {
			_selection.resume();
		}
	} else if (!_scanStarted) {
		// Not running yet; startScan applies the held state after starting.
		return;
	} else if (held) {
		_scanner.pause();
	} else {
		_scanner.resume();
	}
}

int Job::pendingAhead() const {
	auto result = int(_batcher.pending().size());
	for (const auto &batch : _protectedQueue) {
		result += int(batch.items.size());
	}
	if (_protectedBusy) {
		if (_protectedBusyDirectCopy) {
			for (const auto &item : _protectedBusyItems) {
				if (!_protectedSettled.contains(item.id)) {
					++result;
				}
			}
		} else {
			result += _protectedBusyCount;
		}
	}
	if (_fanout) {
		result += int(_fanout->resolved.size());
	}
	auto deepest = 0;
	for (const auto &forwarder : _forwarders) {
		if (!forwarder->cancelled()) {
			deepest = std::max(deepest, forwarder->pendingInfo().count);
		}
	}
	return result + deepest;
}

qint64 Job::pendingBytes() const {
	auto result = qint64(0);
	for (const auto &batch : _protectedQueue) {
		for (const auto &item : batch.items) {
			result += std::max<qint64>(item.size, 0);
		}
	}
	if (_protectedBusy) {
		if (_protectedBusyDirectCopy) {
			for (const auto &item : _protectedBusyItems) {
				if (!_protectedSettled.contains(item.id)) {
					result += std::max<qint64>(item.size, 0);
				}
			}
		} else {
			result += _protectedBusyBytes;
		}
	}
	if (_fanout) {
		for (const auto &id : _fanout->resolved) {
			const auto i = _fanout->sizes.find(id);
			if (i != end(_fanout->sizes)) {
				result += i->second;
			}
		}
	}
	return result;
}

void Job::enqueueProtected(Batch batch) {
	if (_finished || _failed || batch.items.empty()) {
		return;
	}
	// Only the copy path moves bytes, so only its items form the byte line.
	// Server-side forwards are moved by the server and have no byte progress.
	for (const auto &item : batch.items) {
		_knownBytes += std::max<qint64>(item.size, 0);
	}
	_protectedQueue.push_back(std::move(batch));
	updateScanFlow();
	processProtected();
}

void Job::processProtected() {
	if (_protectedBusy
		|| _fanout
		|| _paused
		|| _finished
		|| _failed
		|| _protectedQueue.empty()) {
		return;
	}
	auto alive = false;
	for (const auto &forwarder : _forwarders) {
		if (!forwarder->cancelled()) {
			alive = true;
			break;
		}
	}
	if (!alive) {
		_protectedQueue.clear();
		return;
	}
	_protectedBusy = true;
	auto batch = std::move(_protectedQueue.front());
	_protectedQueue.pop_front();
	_protectedBusyItems = batch.items;
	_protectedSettled.clear();
	_protectedBusyDirectCopy = false;
	_protectedBusyDestination = PeerId();
	_protectedBusyPeer = batch.sourcePeer;
	_protectedBusyCount = int(batch.items.size());
	_protectedBusyFrom = 0;
	_protectedBusyBytes = 0;
	for (const auto &item : batch.items) {
		if (!_protectedBusyFrom || item.id.msg < _protectedBusyFrom) {
			_protectedBusyFrom = item.id.msg;
		}
		_protectedBusyBytes += std::max<qint64>(item.size, 0);
	}
	auto ids = std::vector<FullMsgId>();
	ids.reserve(batch.items.size());
	for (const auto &item : batch.items) {
		ids.push_back(item.id);
	}
	const auto weak = base::make_weak(this);
	EnhancedForward::EnsureForwardSourceMessages(
		_session,
		ids,
		[weak, batch = std::move(batch)](bool) mutable {
			crl::on_main(weak, [weak, batch = std::move(batch)]() mutable {
				if (const auto strong = weak.get()) {
					strong->protectedResolved(std::move(batch));
				}
			});
		});
}

void Job::protectedResolved(Batch batch) {
	_protectedBusy = false;
	_protectedBusyPeer = PeerId();
	_protectedBusyFrom = 0;
	_protectedBusyCount = 0;
	_protectedBusyBytes = 0;
	if (_finished || _failed) {
		return;
	}
	auto items = std::vector<not_null<HistoryItem*>>();
	items.reserve(batch.items.size());
	for (const auto &item : batch.items) {
		if (const auto message = _session->data().message(item.id)) {
			items.push_back(not_null<HistoryItem*>(message));
		}
	}
	const auto handed = int(items.size());
	const auto lost = int(batch.items.size()) - handed;

	auto alive = std::vector<PeerId>();
	for (const auto &forwarder : _forwarders) {
		if (!forwarder->cancelled()) {
			alive.push_back(forwarder->destinationId());
		}
	}
	auto batchIds = base::flat_set<FullMsgId>();
	for (const auto &item : batch.items) {
		batchIds.emplace(item.id);
	}
	// A run that lost all but one destination used to fall back to a plain
	// copy, which ignored the carrier plan and copied the batch again. While
	// the plan knows any source of this batch, it is followed even with a
	// single destination left: saved carriers are forwarded, the rest copied.
	auto hasSavedPlan = false;
	for (const auto &record : _carrierResume) {
		if (batchIds.contains(record.source)) {
			hasSavedPlan = true;
			break;
		}
	}
	if (alive.size() <= 1 && !hasSavedPlan) {
		if (handed > 0) {
			for (const auto &forwarder : _forwarders) {
				if (forwarder->cancelled()) {
					continue;
				}
				const auto destination = forwarder->destinationId();
				auto action = Api::SendAction(
					_session->data().history(destination));
				if (const auto topicRootId = forwarder->topicRootId()) {
					action.replyTo.messageId = FullMsgId(
						destination,
						topicRootId);
					action.replyTo.topicRootId = topicRootId;
				}
				auto busyFrom = MsgId(0);
				auto busyBytes = qint64(0);
				for (const auto &item : batch.items) {
					if (!busyFrom || item.id.msg < busyFrom) {
						busyFrom = item.id.msg;
					}
					busyBytes += std::max<qint64>(item.size, 0);
				}
				_protectedBusy = true;
				_protectedBusyPeer = batch.sourcePeer;
				_protectedBusyFrom = busyFrom;
				_protectedBusyCount = int(batch.items.size());
				_protectedBusyBytes = busyBytes;
				_protectedBusyItems = batch.items;
				_protectedSettled.clear();
				_protectedBusyDirectCopy = true;
				_protectedBusyDestination = destination;
				const auto token = ++_protectedCopyToken;
				const auto weak = base::make_weak(this);
				auto copy = items;
				EnhancedForward::Pipeline::Start(
					&_session->api(),
					std::move(copy),
					action,
					Data::ForwardOptions::PreserveInfo,
					Data::GroupingOptions::GroupAsIs,
					nullptr,
					_runSeen,
					[weak, token, destination, lost](
							std::vector<EnhancedForward::SettledItem> &&results) {
						if (const auto strong = weak.get()) {
							strong->protectedCopySettled(
								token,
								destination,
								lost,
								std::move(results));
						}
					},
					DestinationOnlyDedup(),
					[weak](const EnhancedForward::SettledItem &item) {
						if (const auto strong = weak.get()) {
							strong->protectedItemSettled(item);
						}
					},
					true,
					true);
			}
		} else {
			for (const auto destination : alive) {
				_protectedCounts[destination].failed += lost;
			}
		}
		notifyUpdated();
		processProtected();
		checkDone();
		return;
	}

	auto fanout = std::make_unique<CarrierBatch>();
	fanout->candidates = std::move(alive);
	fanout->resolved.reserve(batch.items.size());
	for (const auto &item : batch.items) {
		fanout->resolved.push_back(item.id);
		fanout->sizes[item.id] = std::max<qint64>(item.size, 0);
	}

	// Copies that the previous process already made are taken from the
	// checkpoint, so they are forwarded instead of copied again. Records of
	// sources outside this batch stay for a later batch of the same run.
	auto resumed = base::flat_set<FullMsgId>();
	if (!_carrierResume.empty()) {
		auto rest = std::vector<ResumeCarrierItem>();
		for (auto &record : _carrierResume) {
			if (!batchIds.contains(record.source)) {
				rest.push_back(std::move(record));
				continue;
			}
			resumed.emplace(record.source);
			fanout->created.push_back({
				.item = {
					.source = record.source,
					.carrierPeer = record.carrierPeer,
					.carrierId = record.carrierId,
					.mediaId = record.mediaId,
					.isPhoto = record.isPhoto,
					.hash = std::move(record.hash),
				},
				.alreadyHave = {},
				.done = std::move(record.done),
			});
		}
		_carrierResume = std::move(rest);
	}
	if (!resumed.empty()) {
		LOG(("BulkForward: Reusing carrier plan, batch %1 source(s), "
			"reused %2")
			.arg(int(batch.items.size()))
			.arg(int(resumed.size())));
	}
	// Losses of a resumed batch were already counted in the saved
	// counters, so only a fresh batch adds them again.
	fanout->runLost = resumed.empty() ? lost : 0;
	for (const auto &item : items) {
		const auto id = item->fullId();
		if (!resumed.contains(id)) {
			fanout->retry.push_back(id);
		}
	}
	_fanout = std::move(fanout);
	if (_fanout->retry.empty()) {
		startCarrierFanout();
	} else {
		startCarrierAttempt();
	}
	notifyUpdated();
}

void Job::protectedCopySettled(
		int token,
		PeerId destination,
		int lostBefore,
		std::vector<EnhancedForward::SettledItem> &&results) {
	if (token != _protectedCopyToken || _finished || _failed) {
		return;
	}
	_protectedBusy = false;
	_protectedBusyPeer = PeerId();
	_protectedBusyFrom = 0;
	_protectedBusyCount = 0;
	_protectedBusyBytes = 0;
	_protectedBusyItems.clear();
	_protectedSettled.clear();
	_protectedBusyDirectCopy = false;
	_protectedBusyDestination = PeerId();
	for (const auto &result : results) {
		LOG(("BulkForward: Copy report, destination %1, source %2/%3, "
			"message %4, reason %5")
			.arg(destination.value)
			.arg(result.source.peer.value)
			.arg(result.source.msg.bare)
			.arg(result.destination.msg.bare)
			.arg(CopyReportReason(result.result)));
	}
	_protectedCounts[destination].failed += lostBefore;
	notifyUpdated();
	processProtected();
	checkDone();
}

void Job::protectedItemSettled(const EnhancedForward::SettledItem &item) {
	if (_finished || _failed) {
		return;
	}
	const auto destination = _protectedBusyDirectCopy
		? _protectedBusyDestination
		: PeerId();
	if (_protectedBusyDirectCopy) {
		_protectedSettled.emplace(item.source);
	}
	const auto duplicate = (item.result
		== EnhancedForward::SettleResult::DestinationDuplicate)
		|| (item.result == EnhancedForward::SettleResult::GlobalDuplicate)
		|| (item.result == EnhancedForward::SettleResult::CoveredByRun);
	if (duplicate) {
		if (destination) {
			++_protectedCounts[destination].duplicates;
			_skippedBytes += std::max<qint64>(item.size, 0);
		}
		return;
	} else if (item.result == EnhancedForward::SettleResult::Cancelled) {
		if (destination) {
			++_protectedCounts[destination].failed;
		}
		return;
	} else if (item.result != EnhancedForward::SettleResult::Created) {
		return;
	}
	if (destination) {
		++_protectedCounts[destination].handed;
		Data::ForwardDedup::RecordHash(
			item.mediaId,
			item.isPhoto,
			item.hash,
			destination,
			item.destination.msg);
	}
	// Uploaded bytes are counted per item, not per batch: a batch report
	// arrives only after its last item, which can be many minutes after the
	// first uploads and would leave the byte line at zero for the whole run.
	// Only the first upload of a source item counts, so a carrier copy that
	// later has to be copied again for one destination does not inflate the
	// numerator past the denominator that was counted once per item.
	if (!_byteCounted.emplace(item.source).second) {
		return;
	}
	_doneBytes += std::max<qint64>(item.size, 0);
	notifyUpdated();
}

void Job::startCarrierAttempt() {
	if (!_fanout || _finished || _failed) {
		return;
	}
	auto &fanout = *_fanout;
	while (fanout.candidateIndex < int(fanout.candidates.size())
		&& !forwarderAlive(fanout.candidates[fanout.candidateIndex])) {
		++fanout.candidateIndex;
	}
	if (fanout.retry.empty()
		|| fanout.candidateIndex >= int(fanout.candidates.size())) {
		// Items left here were already counted as skipped at every
		// destination that was asked about them above.
		fanout.retry.clear();
		startCarrierFanout();
		return;
	}
	const auto candidate = fanout.candidates[fanout.candidateIndex++];
	auto items = std::vector<not_null<HistoryItem*>>();
	items.reserve(fanout.retry.size());
	for (const auto &source : fanout.retry) {
		if (const auto message = _session->data().message(source)) {
			items.push_back(not_null<HistoryItem*>(message));
		} else {
			++_protectedCounts[candidate].failed;
		}
	}
	fanout.retry.clear();
	if (items.empty()) {
		startCarrierAttempt();
		return;
	}
	auto action = Api::SendAction(_session->data().history(candidate));
	if (const auto topicRootId = forwarderTopicRoot(candidate)) {
		action.replyTo.messageId = FullMsgId(candidate, topicRootId);
		action.replyTo.topicRootId = topicRootId;
	}
	const auto token = ++_carrierToken;
	const auto weak = base::make_weak(this);
	fanout.attemptBefore = _runSeen
		? std::make_shared<RunSeen>(*_runSeen)
		: nullptr;
	EnhancedForward::Pipeline::Start(
		&_session->api(),
		std::move(items),
		action,
		Data::ForwardOptions::PreserveInfo,
		Data::GroupingOptions::GroupAsIs,
		nullptr,
		_runSeen,
		[weak, token, candidate](
				std::vector<EnhancedForward::SettledItem> &&results) {
			if (const auto strong = weak.get()) {
				strong->carrierReported(
					token,
					candidate,
					std::move(results));
			}
		},
		DestinationOnlyDedup(),
		[weak](const EnhancedForward::SettledItem &item) {
			if (const auto strong = weak.get()) {
				strong->protectedItemSettled(item);
			}
		},
		true,
		true);
}

void Job::carrierReported(
		int token,
		PeerId candidate,
		std::vector<EnhancedForward::SettledItem> &&results) {
	if (!_fanout || token != _carrierToken) {
		return;
	}
	auto &fanout = *_fanout;
	auto createdMedia = base::flat_set<std::pair<uint64, bool>>();
	auto createdHashes = base::flat_set<QByteArray>();
	for (const auto &result : results) {
		if (result.result != EnhancedForward::SettleResult::Created) {
			continue;
		}
		if (result.mediaId) {
			createdMedia.emplace(result.mediaId, result.isPhoto);
		}
		if (!result.hash.isEmpty()) {
			createdHashes.emplace(result.hash);
		}
	}
	const auto contentCreated = [&](const EnhancedForward::SettledItem &r) {
		return (r.mediaId && createdMedia.contains({ r.mediaId, r.isPhoto }))
			|| (!r.hash.isEmpty() && createdHashes.contains(r.hash));
	};
	// The run set also contains content this very attempt marked while
	// preparing an item that the destination then refused as a duplicate.
	// Such a mark does not mean the content was delivered, so only a match
	// from before the attempt counts as covered by the run.
	const auto knownBefore = [&](const EnhancedForward::SettledItem &r) {
		return fanout.attemptBefore
			&& ((r.mediaId
					&& fanout.attemptBefore->media.contains(
						{ r.mediaId, r.isPhoto }))
				|| (!r.hash.isEmpty()
					&& fanout.attemptBefore->hashes.contains(r.hash)));
	};
	const auto retryCovered = [&](const EnhancedForward::SettledItem &r) {
		fanout.skippedAt[r.source].insert(candidate);
		fanout.retry.push_back(r.source);
		if (_runSeen) {
			if (r.mediaId) {
				_runSeen->media.erase({ r.mediaId, r.isPhoto });
			}
			if (!r.hash.isEmpty()) {
				_runSeen->hashes.erase(r.hash);
			}
		}
	};
	auto created = 0;
	auto duplicates = 0;
	auto failed = 0;
	for (const auto &result : results) {
		switch (result.result) {
		case EnhancedForward::SettleResult::Created:
			fanout.created.push_back({
				.item = {
					.source = result.source,
					.carrierPeer = candidate,
					.carrierId = result.destination.msg,
					.mediaId = result.mediaId,
					.isPhoto = result.isPhoto,
					.hash = result.hash,
				},
				.alreadyHave = fanout.skippedAt[result.source],
			});
			++created;
			LOG(("BulkForward: Carrier copy, destination %1, "
				"source %2/%3, message %4")
				.arg(candidate.value)
				.arg(result.source.peer.value)
				.arg(result.source.msg.bare)
				.arg(result.destination.msg.bare));
			break;
		case EnhancedForward::SettleResult::DestinationDuplicate:
			++duplicates;
			retryCovered(result);
			if (contentCreated(result) || knownBefore(result)) {
				_skippedBytes += std::max<qint64>(result.size, 0);
			}
			break;
		case EnhancedForward::SettleResult::CoveredByRun:
			++duplicates;
			if (!contentCreated(result) && !knownBefore(result)) {
				retryCovered(result);
			} else {
				_skippedBytes += std::max<qint64>(result.size, 0);
			}
			break;
		case EnhancedForward::SettleResult::GlobalDuplicate:
			++duplicates;
			_skippedBytes += std::max<qint64>(result.size, 0);
			break;
		case EnhancedForward::SettleResult::Cancelled:
			++failed;
			break;
		}
	}
	if (created) {
		_protectedCounts[candidate].handed += created;
	}
	if (duplicates) {
		_protectedCounts[candidate].duplicates += duplicates;
	}
	if (failed) {
		_protectedCounts[candidate].failed += failed;
	}
	if (!fanout.retry.empty()) {
		startCarrierAttempt();
	} else {
		startCarrierFanout();
	}
	// The created ids are the only way a resumed run can forward instead of
	// copying again, so they are written as soon as they are known.
	saveCheckpoint(_paused);
	notifyUpdated();
}

void Job::startCarrierFanout() {
	if (!_fanout || _finished || _failed) {
		return;
	}
	auto &fanout = *_fanout;
	fanout.attemptBefore.reset();
	fanout.dispatching = true;

	auto carriers = std::vector<PeerId>();
	for (const auto &created : fanout.created) {
		if (std::find(
				carriers.begin(),
				carriers.end(),
				created.item.carrierPeer) == carriers.end()) {
			carriers.push_back(created.item.carrierPeer);
		}
	}
	auto alive = std::vector<PeerId>();
	for (const auto &forwarder : _forwarders) {
		if (!forwarder->cancelled()) {
			alive.push_back(forwarder->destinationId());
			if (fanout.runLost) {
				_protectedCounts[forwarder->destinationId()].failed
					+= fanout.runLost;
			}
		}
	}
	const auto weak = base::make_weak(this);
	for (const auto destination : alive) {
		for (const auto carrier : carriers) {
			if (carrier == destination) {
				continue;
			}
			auto filtered = std::vector<CarrierItem>();
			for (const auto &created : fanout.created) {
				if (created.item.carrierPeer != carrier
					|| created.alreadyHave.contains(destination)
					|| created.done.contains(destination)) {
					continue;
				}
				filtered.push_back(created.item);
			}
			if (filtered.empty()) {
				continue;
			}
			auto forwarder = std::make_unique<CarrierForwarder>(
				_session,
				_session->data().peer(destination),
				forwarderTopicRoot(destination));
			forwarder->done = [weak, destination](
					CarrierForwarder::Result &&result) {
				if (const auto strong = weak.get()) {
					strong->carrierForwarded(
						destination,
						std::move(result));
				}
			};
			++fanout.pendingForwarders;
			fanout.forwarders.push_back(std::move(forwarder));
			fanout.forwarders.back()->send(
				_session->data().peer(carrier),
				std::move(filtered));
		}
	}
	fanout.dispatching = false;
	if (fanout.pendingForwarders == 0) {
		scheduleFinishCarrierFanout();
	}
}

void Job::carrierForwarded(
		PeerId destination,
		CarrierForwarder::Result &&result) {
	if (!_fanout) {
		return;
	}
	auto &fanout = *_fanout;
	if (fanout.pendingForwarders > 0) {
		--fanout.pendingForwarders;
	}
	_protectedCounts[destination].handed += result.forwarded;
	_protectedCounts[destination].duplicates += result.skipped;
	LOG(("BulkForward: Carrier forward done, destination %1, "
		"forwarded %2, skipped %3")
		.arg(destination.value)
		.arg(result.forwarded)
		.arg(result.skipped));
	auto progress = markCarrierDone(destination, result.sent);
	if (!result.error.isEmpty()) {
		LOG(("BulkForward: Carrier forward failed, destination %1, %2")
			.arg(destination.value)
			.arg(result.error));
		progress |= markCarrierDone(destination, result.fallback);
		startCarrierFallback(destination, std::move(result.fallback));
	}
	if (progress) {
		saveCheckpoint(_paused);
	}
	if (fanout.pendingForwarders == 0 && !fanout.dispatching) {
		scheduleFinishCarrierFanout();
	}
	notifyUpdated();
}

bool Job::markCarrierDone(
		PeerId destination,
		const std::vector<CarrierItem> &items) {
	if (!_fanout || items.empty()) {
		return false;
	}
	auto changed = false;
	for (const auto &item : items) {
		for (auto &created : _fanout->created) {
			if (created.item.source == item.source) {
				changed |= created.done.insert(destination).second;
				break;
			}
		}
	}
	return changed;
}

void Job::startCarrierFallback(
		PeerId destination,
		std::vector<CarrierItem> items) {
	if (items.empty() || _finished || _failed) {
		return;
	}
	auto resolved = std::vector<not_null<HistoryItem*>>();
	resolved.reserve(items.size());
	for (const auto &item : items) {
		if (const auto message = _session->data().message(item.source)) {
			resolved.push_back(not_null<HistoryItem*>(message));
		}
	}
	if (resolved.empty()) {
		return;
	}
	const auto resolvedCount = int(resolved.size());
	// The carrier already approved this content for the run, so the shared
	// run set exempts it from the global check, the same way the plain
	// forwarder exempts its other destinations.
	auto runSeen = _runSeen;
	if (!runSeen) {
		runSeen = std::make_shared<RunSeen>();
	}
	for (const auto &item : items) {
		if (item.mediaId) {
			runSeen->media.insert({ item.mediaId, item.isPhoto });
		}
		if (!item.hash.isEmpty()) {
			runSeen->hashes.insert(item.hash);
		}
	}
	auto action = Api::SendAction(_session->data().history(destination));
	if (const auto topicRootId = forwarderTopicRoot(destination)) {
		action.replyTo.messageId = FullMsgId(destination, topicRootId);
		action.replyTo.topicRootId = topicRootId;
	}
	const auto weak = base::make_weak(this);
	EnhancedForward::Pipeline::Start(
		&_session->api(),
		std::move(resolved),
		action,
		Data::ForwardOptions::PreserveInfo,
		Data::GroupingOptions::GroupAsIs,
		nullptr,
		std::move(runSeen),
		[](std::vector<EnhancedForward::SettledItem> &&) {},
		DestinationOnlyDedup(),
		[weak](const EnhancedForward::SettledItem &item) {
			if (const auto strong = weak.get()) {
				strong->protectedItemSettled(item);
			}
		},
		true,
		true);
	_protectedCounts[destination].handed += resolvedCount;
	LOG(("BulkForward: Carrier fallback copy, destination %1, items %2")
		.arg(destination.value)
		.arg(resolvedCount));
}

void Job::scheduleFinishCarrierFanout() {
	if (!_fanout
		|| _fanout->finishScheduled
		|| _fanout->pendingForwarders > 0) {
		return;
	}
	_fanout->finishScheduled = true;
	const auto weak = base::make_weak(this);
	crl::on_main([weak] {
		if (const auto strong = weak.get()) {
			strong->finishCarrierFanout();
		}
	});
}

void Job::finishCarrierFanout() {
	if (!_fanout || _fanout->pendingForwarders > 0) {
		return;
	}
	_fanout.reset();
	++_carrierToken;
	saveCheckpoint(_paused);
	notifyUpdated();
	processProtected();
	checkDone();
}

MsgId Job::forwarderTopicRoot(PeerId destination) const {
	for (const auto &forwarder : _forwarders) {
		if (forwarder->destinationId() == destination) {
			return forwarder->topicRootId();
		}
	}
	return MsgId(0);
}

bool Job::forwarderAlive(PeerId destination) const {
	for (const auto &forwarder : _forwarders) {
		if (forwarder->destinationId() == destination) {
			return !forwarder->cancelled();
		}
	}
	return false;
}

void Job::flushBatcher() {
	_inFlush = true;
	_batcher.flush();
	_inFlush = false;
}

void Job::checkDone() {
	if (_finished || _failed || !_scanDone) {
		return;
	} else if (_inFlush) {
		_donePending = true;
		return;
	} else if (_protectedBusy || !_protectedQueue.empty() || _fanout) {
		return;
	}
	for (const auto &forwarder : _forwarders) {
		if (!forwarder->idle()) {
			return;
		}
	}
	_finished = true;
	finishCounters();
	ClearResume(_session);
	if (done) {
		done();
	}
}

void Job::notifyUpdated() {
	updateCounters();
	updateScanFlow();
	if (!_finished && !_failed) {
		const auto now = crl::now();
		const auto due = _paused
			|| !_lastCheckpoint
			|| (now - _lastCheckpoint >= kCheckpointInterval);
		if (due) {
			saveCheckpoint(_paused);
		}
	}
	if (updated) {
		updated();
	}
}

void Job::saveCheckpoint(bool paused) {
	if (_finished || _failed || !_source) {
		return;
	}
	auto state = ResumeState();
	state.source = _source;
	state.sourceTopicRootId = _sourceTopicRootId;
	state.filter = _filter;
	state.method = _scanMethod;
	state.scanned = _scanned;
	state.total = _total;
	state.matched = _matched;
	state.skippedBytes = _skippedBytes;
	state.startedAt = _startedAt;
	state.previewTotal = _previewTotal;
	for (const auto &forwarder : _forwarders) {
		if (forwarder->cancelled()) {
			continue;
		}
		const auto it = _protectedCounts.find(forwarder->destinationId());
		const auto extra = (it != end(_protectedCounts))
			? it->second
			: ProtectedCount();
		state.destinations.push_back({
			.peer = forwarder->destinationId(),
			.topicRootId = forwarder->topicRootId(),
			.sent = forwarder->sent() + extra.handed,
			.skipped = forwarder->skipped() + extra.duplicates,
			.failed = forwarder->failedCount() + extra.failed,
		});
	}
	if (state.destinations.empty()) {
		return;
	}

	if (UsesSelection(_filter)) {
		// A selection is a fixed list, so the resumed run simply reads it
		// again and the destinations skip what is already there.
		state.scanned = 0;
		state.total = 0;
		state.matched = 0;
		state.knownBytes = 0;
		state.doneBytes = 0;
		state.skippedBytes = 0;
		for (auto &destination : state.destinations) {
			destination.sent = 0;
			destination.skipped = 0;
			destination.failed = 0;
		}
	} else {
		if (_scanStarted) {
			for (const auto &[peer, cursor] : _scanner.resumeCursors()) {
				state.cursors[peer] = cursor;
			}
		} else {
			state.cursors = _scanCursors;
		}

		// Everything that is queued but not settled will be read again after
		// a restart, so it is subtracted from the matched baseline and the
		// saved cursors are pulled back below it.
		auto pendingFrom = base::flat_map<PeerId, MsgId>();
		const auto mergeFrom = [&](PeerId peer, MsgId id) {
			const auto i = pendingFrom.find(peer);
			if (i == end(pendingFrom) || i->second > id) {
				pendingFrom[peer] = id;
			}
		};
		auto pendingCount = 0;
		const auto addItem = [&](const SourceMessage &item) {
			mergeFrom(item.id.peer, item.id.msg);
			++pendingCount;
		};
		for (const auto &item : _batcher.pending()) {
			addItem(item);
		}
		auto forwarderPending = 0;
		for (const auto &forwarder : _forwarders) {
			if (forwarder->cancelled()) {
				continue;
			}
			const auto info = forwarder->pendingInfo();
			forwarderPending = std::max(forwarderPending, info.count);
			for (const auto &[peer, id] : info.from) {
				mergeFrom(peer, id);
			}
		}
		pendingCount += forwarderPending;
		for (const auto &batch : _protectedQueue) {
			for (const auto &item : batch.items) {
				addItem(item);
			}
		}
		if (_protectedBusy) {
			if (_protectedBusyDirectCopy) {
				for (const auto &item : _protectedBusyItems) {
					if (!_protectedSettled.contains(item.id)) {
						addItem(item);
					}
				}
			} else if (_protectedBusyFrom) {
				mergeFrom(_protectedBusyPeer, _protectedBusyFrom);
				pendingCount += _protectedBusyCount;
			}
		}
		if (_fanout) {
			for (const auto &id : _fanout->resolved) {
				mergeFrom(id.peer, id.msg);
				++pendingCount;
			}
		}
		state.matched = std::max(0, _matched - pendingCount);
		state.knownBytes = std::max<qint64>(0, _knownBytes - pendingBytes());
		state.doneBytes = _doneBytes;
		for (const auto &[peer, id] : pendingFrom) {
			const auto i = state.cursors.find(peer);
			if (i == end(state.cursors) || i->second >= id) {
				state.cursors[peer] = MsgId(id.bare - 1);
			}
		}
	}
	state.carriers = _carrierResume;
	if (_fanout) {
		for (const auto &created : _fanout->created) {
			state.carriers.push_back({
				.source = created.item.source,
				.carrierPeer = created.item.carrierPeer,
				.carrierId = created.item.carrierId,
				.mediaId = created.item.mediaId,
				.isPhoto = created.item.isPhoto,
				.hash = created.item.hash,
				.done = created.done,
			});
		}
	}
	SaveResume(_session, state, paused);
	_lastCheckpoint = crl::now();
}

void Job::updateCounters() {
	if (_finished) {
		return;
	}
	auto counters = std::vector<Counters>();
	counters.reserve(_forwarders.size());
	const auto knownBytes = std::max<qint64>(_knownBytes, 0);
	const auto doneBytes = (knownBytes > 0)
		? std::min(std::max<qint64>(_doneBytes, 0), knownBytes)
		: std::max<qint64>(_doneBytes, 0);
	for (const auto &forwarder : _forwarders) {
		if (forwarder->cancelled()) {
			continue;
		}
		const auto it = _protectedCounts.find(forwarder->destinationId());
		const auto extra = (it != end(_protectedCounts))
			? it->second
			: ProtectedCount();
		const auto sent = forwarder->sent() + extra.handed;
		const auto skipped = forwarder->skipped() + extra.duplicates;
		const auto failed = forwarder->failedCount() + extra.failed;
		const auto lastMatched = (_previewLastId > _lastMatchedId)
			? _previewLastId
			: _lastMatchedId;
		counters.push_back({
			.session = _session.get(),
			.source = _source,
			.destination = forwarder->destinationId(),
			.firstSource = _firstMatchedId
				? FullMsgId(_source, _firstMatchedId)
				: FullMsgId(),
			.lastSource = lastMatched
				? FullMsgId(_source, lastMatched)
				: FullMsgId(),
			.scanned = _scanned,
			.total = _total,
			.matched = std::max({
				_matched,
				sent + skipped + failed,
				_previewTotal }),
			.sent = sent,
			.skipped = skipped,
			.failed = failed,
			.copied = extra.handed,
			.knownBytes = knownBytes,
			.doneBytes = doneBytes,
			.skippedBytes = std::min(
				std::max<qint64>(_skippedBytes, 0),
				knownBytes),
			.startedAt = _startedAt,
			.paused = _paused,
			// A scope with no known chat size - a date or id range, or a
			// selection - has nothing to walk towards, so it reports the
			// send phase from the start.
			.scanDone = _scanDone || (_total <= 0),
		});
	}
	UpdateCounters(std::move(counters));
}

void Job::finishCounters() {
	for (const auto &forwarder : _forwarders) {
		RemoveCounters(_session.get(), forwarder->destinationId());
	}
}

} // namespace BulkForward
