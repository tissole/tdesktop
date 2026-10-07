/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/downloads/info_downloads_provider.h"

#include "info/media/info_media_widget.h"
#include "info/media/info_media_list_section.h"
#include "info/info_controller.h"
#include "ui/text/format_song_document_name.h"
#include "ui/ui_utility.h"
#include "data/data_download_manager.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer.h"
#include "overview/overview_layout.h"
#include "core/click_handler_types.h"
#include "apiwrap.h"
#include "data/data_session.h"
#include "main/main_account.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "main/main_domain.h"
#include "history/history_item.h"
#include "history/history_item_helpers.h"
#include "history/history.h"
#include "enhanced_forward.h"
#include "logs.h"
#include "core/application.h"
#include "lang/lang_keys.h"
#include "settings.h"
#include "storage/file_upload.h"
#include "storage/storage_shared_media.h"
#include "layout/layout_selection.h"
#include "styles/style_overview.h"
#include "data/data_msg_id.h"
#include <tuple>

namespace Info::Downloads {
namespace {

using namespace Media;

[[nodiscard]] Overview::Layout::ForwardSummaryData FwAggregateFor(
		not_null<Main::Session*> session,
		const PeerId &dst) {
	auto result = Overview::Layout::ForwardSummaryData();
	auto sent = 0;
	auto total = 0;
	auto skipped = 0;
	auto flood = 0;
	auto anyActive = false;
	auto anyPaused = false;
	auto haveLive = false;
	auto srcPeer = PeerId();
	for (const auto &job : EnhancedForward::AllJobs(session)) {
		if (job.peer != dst
			|| job.progress.state == EnhancedForward::State::Cancelled) {
			continue;
		}
		const auto live = job.active && !job.finished;
		if (!live && !job.resumable && !job.finished) {
			continue; // queued, not started yet
		}
		if (live || job.resumable) {
			haveLive = true;
		} else {
			continue; // kept finished runs come from the registry below
		}
		if (srcPeer == PeerId()) {
			srcPeer = job.srcPeer;
		}
		sent += job.progress.sent;
		total += job.progress.total;
		skipped += job.progress.skipped;
		if (job.resumable) {
			anyPaused = true;
		} else if (job.progress.state == EnhancedForward::State::Paused) {
			anyPaused = true;
		} else {
			anyActive = true;
		}
	}
	for (const auto &counters : NormalForward::AllCounters(session)) {
		if (counters.dst != dst || !counters.active) {
			continue;
		}
		haveLive = true;
		sent += counters.done;
		total += counters.total;
		skipped += counters.skipped;
		flood = std::max(flood, counters.floodSeconds);
		if (srcPeer == PeerId() && counters.firstSource) {
			srcPeer = counters.firstSource.peer;
		}
		if (counters.paused) {
			anyPaused = true;
		} else {
			anyActive = true;
		}
	}
	if (!haveLive) {
		return result;
	}
	if (const auto action = EnhancedForward::CurrentForwardActionId(
		session,
		dst)) {
		auto absorbing = false;
		for (const auto &batch : EnhancedForward::ForwardBatches(session)) {
			if (batch.dst == dst && !batch.finished) {
				absorbing = true;
				break;
			}
		}
		if (absorbing) {
			for (const auto &record
				: EnhancedForward::FinishedFwRuns(session)) {
				if (record.dst != dst || record.actionId != action) {
					continue;
				}
				sent += record.done;
				total += record.total;
				skipped += record.skipped;
				if (srcPeer == PeerId() && record.src != PeerId()) {
					srcPeer = record.src;
				}
			}
		}
	}
	const auto peer = session->data().peer(dst);
	result.title = (srcPeer != PeerId() && srcPeer != dst)
		? u"%1 → %2"_q.arg(
			session->data().peer(srcPeer)->name(),
			peer->name())
		: peer->name();
	if (srcPeer != PeerId() && srcPeer != dst) {
		result.srcName = session->data().peer(srcPeer)->name();
		result.dstName = peer->name();
	}
	result.status = tr::lng_tm_files_progress(
		tr::now,
		lt_done, QString::number(sent),
		lt_total, QString::number(total));
	if (skipped > 0) {
		result.status += u" (+%1)"_q.arg(
			tr::lng_tm_fw_duplicates_skipped(tr::now, lt_count, skipped));
	}
	if (flood > 0) {
		result.status += u"  FLOOD_WAIT %1s"_q.arg(flood);
	}
	result.progress = total ? (float64(sent) / total) : 0.;
	result.paused = !anyActive && anyPaused;
	result.finished = !anyActive && !anyPaused;
	return result;
}

[[nodiscard]] Overview::Layout::ForwardSummaryData FwFinishedGroupData(
		not_null<Main::Session*> session,
		const std::vector<EnhancedForward::FinishedFwRun> &runs) {
	auto result = Overview::Layout::ForwardSummaryData();
	if (runs.empty()) {
		return result;
	}
	auto sent = 0;
	auto total = 0;
	auto skipped = 0;
	auto finishedAt = int64(0);
	auto srcPeer = PeerId();
	auto dstPeer = PeerId();
	for (const auto &run : runs) {
		sent += run.done;
		total += run.total;
		skipped += run.skipped;
		finishedAt = std::max(finishedAt, run.finishedAt);
		dstPeer = run.dst;
		if (srcPeer == PeerId() && run.src != PeerId()) {
			srcPeer = run.src;
		}
	}
	const auto peer = session->data().peer(dstPeer);
	result.title = (srcPeer != PeerId() && srcPeer != dstPeer)
		? u"%1 → %2"_q.arg(
			session->data().peer(srcPeer)->name(),
			peer->name())
		: peer->name();
	if (srcPeer != PeerId() && srcPeer != dstPeer) {
		result.srcName = session->data().peer(srcPeer)->name();
		result.dstName = peer->name();
	}
	result.status = tr::lng_tm_files_progress(
		tr::now,
		lt_done, QString::number(sent),
		lt_total, QString::number(total));
	if (skipped > 0) {
		result.status += u" (+%1)"_q.arg(
			tr::lng_tm_fw_duplicates_skipped(tr::now, lt_count, skipped));
	}
	if (finishedAt > 0) {
		result.statusDate = langDateTime(
			QDateTime::fromSecsSinceEpoch(finishedAt));
	}
	result.progress = total ? (float64(sent) / total) : 0.;
	result.finished = true;
	return result;
}

[[nodiscard]] Overview::Layout::ForwardSummaryData DlRunData(
		const Data::DownloadManager::DlRunSnapshot &run) {
	auto result = Overview::Layout::ForwardSummaryData();
	result.title = !run.srcName.isEmpty()
		? run.srcName
		: !run.destDir.isEmpty()
		? run.destDir
		: tr::lng_tm_dl_section(tr::now);
	const auto done = std::min(run.done, run.total);
	result.status = tr::lng_tm_files_progress(
		tr::now,
		lt_done, QString::number(done),
		lt_total, QString::number(run.total));
	if (run.skipped > 0) {
		result.status += u" (+%1)"_q.arg(
			tr::lng_tm_dl_duplicates_skipped(
				tr::now,
				lt_count,
				run.skipped));
	}
	if (run.finished && run.finishedAt > 0) {
		result.status += u" · %1"_q.arg(
			langDateTime(QDateTime::fromSecsSinceEpoch(run.finishedAt)));
	}
	result.progress = run.total ? (float64(done) / run.total) : 0.;
	result.finished = run.finished;
	return result;
}

[[nodiscard]] Overview::Layout::ForwardSummaryData UlRunData(
		const Data::DownloadManager::UlRunSnapshot &run,
		const QString &dstName) {
	auto result = Overview::Layout::ForwardSummaryData();
	result.title = dstName.isEmpty()
		? tr::lng_tm_ul_section(tr::now)
		: dstName;
	const auto done = std::min(run.done, run.total);
	result.status = tr::lng_tm_files_progress(
		tr::now,
		lt_done, QString::number(done),
		lt_total, QString::number(run.total));
	if (run.skipped > 0) {
		result.status += u" (+%1)"_q.arg(
			tr::lng_tm_ul_duplicates_skipped(
				tr::now,
				lt_count,
				run.skipped));
	}
	if (run.finished && run.finishedAt > 0) {
		result.status += u" · %1"_q.arg(
			langDateTime(QDateTime::fromSecsSinceEpoch(run.finishedAt)));
	}
	result.progress = run.total ? (float64(done) / run.total) : 0.;
	result.finished = run.finished;
	return result;
}

void ToggleFwRunPause(
		not_null<Main::Session*> session,
		const PeerId &dst) {
	auto anyActive = false;
	for (const auto &job : EnhancedForward::AllJobs(session)) {
		if (job.peer == dst
			&& job.active
			&& !job.finished
			&& job.progress.state != EnhancedForward::State::Cancelled
			&& job.progress.state != EnhancedForward::State::Paused) {
			anyActive = true;
		}
	}
	for (const auto &counters : NormalForward::AllCounters(session)) {
		if (counters.dst == dst && counters.active && !counters.paused) {
			anyActive = true;
		}
	}
	if (anyActive) {
		for (const auto &job : EnhancedForward::AllJobs(session)) {
			if (job.peer == dst
				&& job.active
				&& !job.finished
				&& job.progress.state != EnhancedForward::State::Cancelled
				&& job.progress.state != EnhancedForward::State::Paused) {
				EnhancedForward::pauseForward(job.peer, session);
			}
		}
		NormalForward::PauseDst(session, dst);
		return;
	}
	for (const auto &job : EnhancedForward::AllJobs(session)) {
		if (job.peer != dst) {
			continue;
		}
		if (job.active
			&& !job.finished
			&& job.progress.state == EnhancedForward::State::Paused) {
			EnhancedForward::resumeForward(job.peer, session);
		} else if (job.resumable) {
			session->api().startResumeForward(job.srcPeer, job.peer, session);
		}
	}
	NormalForward::ResumeDst(session, dst);
}

void ClearFwRun(
		not_null<Main::Session*> session,
		const PeerId &dst) {
	for (const auto &job : EnhancedForward::AllJobs(session)) {
		if (job.peer == dst && job.finished && !job.resumable) {
			for (const auto &srcId : job.progress.sourceIds) {
				EnhancedForward::ClearFinishedItems(session, srcId);
			}
		}
	}
	for (const auto &batch : EnhancedForward::ForwardBatches(session)) {
		if (batch.dst == dst && batch.finished) {
			EnhancedForward::DropForwardBatch(session, dst);
		}
	}
}

[[nodiscard]] std::optional<PeerId> FwCurrentDst(
		not_null<Main::Session*> session,
		FullMsgId srcId) {
	for (const auto &job : EnhancedForward::AllJobs(session)) {
		if (!job.active
			|| job.finished
			|| job.progress.state == EnhancedForward::State::Cancelled) {
			continue;
		}
		const auto &ids = job.progress.sourceIds;
		for (const auto idx : {
			job.progress.currentDownload,
			job.progress.currentUpload }) {
			if (idx >= 0
				&& idx < int(ids.size())
				&& ids[idx] == srcId) {
				return job.peer;
			}
		}
	}
	return std::nullopt;
}

[[nodiscard]] uint64 FwTickDigest(
		not_null<Main::Session*> session,
		const std::vector<EnhancedForward::JobSnapshot> &ordered,
		const std::vector<std::pair<PeerId, HistoryItem*>> &liveCarriers,
		const std::vector<std::pair<EnhancedForward::FinishedFwRun, HistoryItem*>> &keptRuns) {
	auto result = uint64(0);
	auto unit = uint64(0);
	const auto unitBegin = [&](uint64 salt) {
		unit = salt ^ uint64(1469598103934665603ULL);
	};
	const auto unitFeed = [&](uint64 value) {
		unit ^= value;
		unit *= uint64(1099511628211ULL);
	};
	const auto unitEnd = [&] {
		result ^= unit;
	};
	for (const auto &job : ordered) {
		unitBegin(uint64(1));
		unitFeed(uint64(job.peer.value));
		unitFeed(uint64(job.srcPeer.value));
		unitFeed(job.active);
		unitFeed(job.finished);
		unitFeed(job.resumable);
		unitFeed(uint64(static_cast<int>(job.progress.state)));
		unitFeed(uint64(job.progress.sent));
		unitFeed(uint64(job.progress.total));
		unitFeed(uint64(job.progress.skipped));
		for (const auto &srcId : job.progress.sourceIds) {
			unitFeed(uint64(srcId.peer.value));
			unitFeed(uint64(srcId.msg.bare));
		}
		for (const auto &item : job.progress.items) {
			unitFeed(item.cancelled);
			unitFeed(item.dedupSkipped);
			unitFeed(uint64(static_cast<int>(item.state)));
		}
		unitEnd();
	}
	for (const auto &counters : NormalForward::AllCounters(session)) {
		unitBegin(uint64(2));
		unitFeed(uint64(counters.dst.value));
		unitFeed(counters.active);
		unitFeed(counters.paused);
		unitFeed(uint64(counters.done));
		unitFeed(uint64(counters.total));
		unitFeed(uint64(counters.skipped));
		unitFeed(uint64(counters.floodSeconds));
		unitFeed(uint64(counters.firstSource.peer.value));
		unitFeed(uint64(counters.firstSource.msg.bare));
		unitEnd();
	}
	for (const auto &batch : EnhancedForward::ForwardBatches(session)) {
		unitBegin(uint64(3));
		unitFeed(uint64(batch.dst.value));
		unitFeed(uint64(batch.total));
		unitFeed(uint64(batch.sent));
		unitFeed(uint64(batch.skipped));
		unitFeed(batch.finished);
		unitEnd();
	}
	for (const auto &entry : keptRuns) {
		const auto &run = entry.first;
		unitBegin(uint64(4));
		unitFeed(uint64(run.dst.value));
		unitFeed(uint64(run.src.value));
		unitFeed(uint64(run.total));
		unitFeed(uint64(run.done));
		unitFeed(uint64(run.skipped));
		unitFeed(run.runId);
		unitFeed(run.actionId);
		unitFeed(uint64(run.finishedAt));
		unitEnd();
	}
	for (const auto &entry : liveCarriers) {
		unitBegin(uint64(5));
		unitFeed(uint64(entry.first.value));
		unitEnd();
	}
	return result;
}

} // namespace

Provider::Provider(not_null<AbstractController*> controller)
: _controller(controller)
, _storiesAddToAlbumId(_controller->storiesAddToAlbumId()) {
	style::PaletteChanged(
	) | rpl::on_next([=] {
		for (auto &layout : _layouts) {
			layout.second.item->invalidateCache();
		}
	}, _lifetime);
}

Type Provider::type() {
	return Type::File;
}

bool Provider::hasSelectRestriction() {
	return false;
}

rpl::producer<bool> Provider::hasSelectRestrictionChanges() {
	return rpl::never<bool>();
}

bool Provider::sectionHasFloatingHeader() {
	return _showGroupHeaders;
}

QString Provider::sectionTitle(not_null<const BaseLayout*> item) {
	if (!_showGroupHeaders) {
		return QString();
	} else if (dynamic_cast<const Overview::Layout::ForwardSummary*>(
		item.get())) {
		if (isDlHeaderLayout(item)) {
			return tr::lng_tm_dl_section(tr::now);
		}
		return isUlHeaderLayout(item)
			? tr::lng_tm_ul_section(tr::now)
			: tr::lng_tm_fw_section(tr::now);
	}
	return isUploadItem(item->getItem())
		? tr::lng_tm_ul_section(tr::now)
		: tr::lng_tm_dl_section(tr::now);
}

bool Provider::sectionItemBelongsHere(
		not_null<const BaseLayout*> item,
		not_null<const BaseLayout*> previous) {
	if (!_showGroupHeaders) {
		return true;
	}
	const auto category = [&](not_null<const BaseLayout*> layout) {
		if (dynamic_cast<const Overview::Layout::ForwardSummary*>(
			layout.get())) {
			if (isDlHeaderLayout(layout)) {
				return 0;
			}
			return isUlHeaderLayout(layout) ? 1 : 2;
		}
		const auto element = ranges::find_if(
			_elements,
			[&](const Element &element) {
				return element.item == layout->getItem();
			});
		if (element != end(_elements)) {
			if (element->dlBatch != 0) {
				return 0;
			} else if (element->ulBatch != 0) {
				return 1;
			}
		}
		return isUploadItem(layout->getItem()) ? 1 : 0;
	};
	return category(item) == category(previous);
}

void Provider::setFilter(Filter filter) {
	if (_filter == filter) {
		return;
	}
	_filter = filter;
	updateCounter();
	_refreshed.fire({});
}

rpl::producer<QString> Provider::counterValue() const {
	return _counterText.value();
}

rpl::producer<bool> Provider::hasDownloadsValue() const {
	return _hasDownloads.value();
}

rpl::producer<bool> Provider::hasUploadsValue() const {
	return _hasUploads.value();
}

void Provider::updateAvailability() {
	_hasDownloads = !_downloading.empty()
		|| !_downloaded.empty()
		|| ((_filter == Filter::Forwards || _filter == Filter::All)
			&& !_enhancedForward.empty());
	_hasUploads = !_uploading.empty() || !_uploaded.empty();
}

void Provider::updateCounter() {
	if (_filter == Filter::Downloads || _filter == Filter::Uploads) {
		// Each batch carries its own counter header, the tab total would
		// only duplicate them.
		_counterText = QString();
		return;
	}
	const auto wantDownloads = (_filter == Filter::Downloads
		|| _filter == Filter::All);
	const auto wantUploads = (_filter == Filter::Uploads
		|| _filter == Filter::All);
	const auto wantForwards = (_filter == Filter::Forwards
		|| _filter == Filter::All);
	auto done = 0;
	auto total = 0;
	const auto eachSession = [&](auto &&callback) {
		for (const auto &account : Core::App().domain().orderedAccounts()) {
			if (const auto session = account->maybeSession()) {
				callback(session);
			}
		}
	};
	auto accounts = 0;
	auto fwFlood = 0;
	auto fwDone = 0;
	auto fwTotal = 0;
	auto fwJobs = 0;
	if (wantForwards && _filter != Filter::Forwards) {
		// Count every pending forward item. Forward jobs are keyed globally
		// by peer and reported by every session that has that peer loaded, so
		// deduplicate by peer to avoid counting one forward per account.
		auto seen = base::flat_set<PeerId>();
		auto seenBatches = base::flat_set<PeerId>();
		eachSession([&](not_null<Main::Session*> session) {
			accounts++;
			for (const auto &batch : EnhancedForward::ForwardBatches(session)) {
				if (!seenBatches.emplace(batch.dst).second) {
					continue;
				}
				fwJobs++;
				fwTotal += batch.total;
				fwDone += batch.sent;
			}
			for (const auto &job : EnhancedForward::MemoryJobs(session)) {
				if (job.finished
					|| job.progress.state
						== EnhancedForward::State::Cancelled) {
					continue;
				}
				if (seenBatches.contains(job.peer)) {
					continue;
				}
				if (!seen.emplace(job.peer).second) {
					continue;
				}
				fwJobs++;
				fwTotal += job.progress.total;
				fwDone += job.progress.sent;
			}
		});
		eachSession([&](not_null<Main::Session*> session) {
			for (const auto &counters : NormalForward::AllCounters(session)) {
				if (!counters.active) {
					continue;
				}
				fwFlood = std::max(fwFlood, counters.floodSeconds);
				if (seenBatches.contains(counters.dst)) {
					continue;
				}
				fwTotal += counters.total;
				fwDone += counters.done;
			}
		});
		total += fwTotal;
		done += fwDone;
	}
	auto dlDone = 0;
	auto dlTotal = 0;
	if (wantDownloads) {
		auto &manager = Core::App().downloadManager();
		eachSession([&](not_null<Main::Session*> session) {
			dlTotal += manager.jobTotal(session);
			dlDone += manager.jobDone(session);
		});
		total += dlTotal;
		done += dlDone;
	}
	auto ulDone = 0;
	auto ulTotal = 0;
	if (wantUploads) {
		eachSession([&](not_null<Main::Session*> session) {
			ulTotal += session->uploader().jobTotal();
			ulDone += session->uploader().jobDone();
		});
		total += ulTotal;
		done += ulDone;
	}
	const auto floodText = (fwFlood > 0)
		? u"  FLOOD_WAIT %1s"_q.arg(fwFlood)
		: QString();
	if (total == 0) {
		_counterText = floodText;
		return;
	}
	_counterText = tr::lng_tm_counter(
		tr::now,
		lt_done, QString::number(done),
		lt_total, QString::number(total)) + floodText;
}

bool Provider::isPossiblyMyItem(not_null<const HistoryItem*> item) {
	return true;
}

std::optional<int> Provider::fullCount() {
	return _queryWords.empty()
		? _fullCount
		: (_foundCount || _fullCount.has_value())
		? _foundCount
		: std::optional<int>();
}

void Provider::restart() {
}

void Provider::checkPreload(
	QSize viewport,
	not_null<BaseLayout*> topLayout,
	not_null<BaseLayout*> bottomLayout,
	bool preloadTop,
	bool preloadBottom) {
}

void Provider::setSearchQuery(QString query) {
	if (_query == query) {
		return;
	}
	_query = query;
	auto words = TextUtilities::PrepareSearchWords(_query);
	if (!_started || _queryWords == words) {
		return;
	}
	_queryWords = std::move(words);
	if (searchMode()) {
		_foundCount = 0;
		for (auto &element : _elements) {
			if ((element.found = computeIsFound(element))) {
				++_foundCount;
			}
		}
	}
	_refreshed.fire({});
}

void Provider::jumpToMessage(MsgId messageId, Fn<void(FullMsgId)>) {
}

void Provider::refreshViewer() {
	if (_started) {
		return;
	}
	_started = true;
	auto &manager = Core::App().downloadManager();
	rpl::single(rpl::empty) | rpl::then(
		manager.loadingListChanges() | rpl::to_empty
	) | rpl::on_next([=, &manager] {
		auto copy = _downloading;
		auto efCopy = _enhancedForward;
		for (const auto id : manager.loadingList()) {
			if (!id->done) {
				const auto item = id->object.item;
				if (id->enhancedForward) {
					if (!efCopy.remove(item) && !_downloaded.contains(item)) {
						const auto wasUploading = _uploading.contains(item->fullId());
						const auto wasUploaded = _uploaded.contains(item->fullId());
						_enhancedForward.emplace(item);
						if (!wasUploading && !wasUploaded) {
							addElementNow({
								.item = item,
								.started = int64(item->date()) * 1000,
								.path = id->path,
							});
						}
						trackItemSession(item);
						refreshPostponed(true);
					}
				} else if (!copy.remove(item) && !_downloaded.contains(item)) {
					const auto wasUploading = _uploading.remove(item->fullId());
					const auto wasUploaded = _uploaded.remove(item->fullId());
					_downloading.emplace(item);
					if (!wasUploading && !wasUploaded) {
						addElementNow({
							.item = item,
							.started = id->started,
							.path = id->path,
							.dlBatch = id->batchId,
						});
					}
					trackItemSession(item);
					refreshPostponed(true);
				}
		} else if (id->enhancedForward) {
			// EF item download is done, but the forward may still upload.
			// Keep the item in _enhancedForward until it's fully removed
			// from loadingList() (when removeLoading is called after upload).
		} else {
			copy.remove(id->object.item);
		}
	}
		for (const auto &item : copy) {
			const auto inPostponed = ranges::contains(
				_addPostponed,
				item,
				&Element::item);
			_downloading.remove(item);
			if (!_downloaded.contains(item) && !inPostponed) {
				if (manager.isDone(item)) {
					// The item finished during the provider's setup window and
					// its loadedAdded was missed (we subscribed afterwards).
					// Keep it listed instead of dropping it from the tab.
					_downloaded.emplace(item);
				} else {
					remove(item);
				}
			}
		}
		for (const auto &item : efCopy) {
			const auto inPostponed = ranges::contains(
				_addPostponed,
				item,
				&Element::item);
			if (!inPostponed) {
				// Keep EF items in _enhancedForward even after they're
				// fully done (download + upload complete) so they stay
				// visible in the Forwards tab until user clears them.
				// The item will be removed from loadingList() when
				// removeLoading() is called after upload completion,
				// but we keep it here for display.
			}
		}
		if (!_fullCount.has_value()) {
			refreshPostponed(false);
		}
		_efTransferring.clear();
		for (const auto id : manager.loadingList()) {
			if (id->enhancedForward
				&& !id->done
				&& id->object.item != nullptr) {
				const auto fullId = id->object.item->fullId();
				_efTransferring.emplace(fullId);
				const auto it = ranges::find_if(
					_layouts,
					[&](const auto &pair) {
						return pair.first->fullId() == fullId;
					});
				if (it != end(_layouts)) {
					it->second.item->itemDataChanged();
				}
			}
		}
		touchDlHeaders();
	}, _lifetime);

	for (const auto id : manager.loadedList()) {
		addPostponed(id);
	}

	manager.loadedAdded(
	) | rpl::on_next([=](not_null<const Data::DownloadedId*> entry) {
		if (const auto object = entry->object.get()) {
			_downloading.remove(not_null(object->item));
		}
		addPostponed(entry);
		touchDlHeaders();
	}, _lifetime);

	manager.loadedRemoved(
	) | rpl::on_next([=](not_null<const HistoryItem*> item) {
		if (_enhancedForward.contains(item)) {
			return;
		}
		if (!_downloading.contains(item)) {
			remove(item);
		} else {
			_downloaded.remove(item);
			_addPostponed.erase(
				ranges::remove(_addPostponed, item, &Element::item),
				end(_addPostponed));
		}
	}, _lifetime);

	manager.loadedResolveDone(
	) | rpl::on_next([=] {
		if (!_fullCount.has_value()) {
			_fullCount = 0;
		}
	}, _lifetime);

	for (const auto &account : Core::App().domain().orderedAccounts()) {
		const auto session = account->maybeSession();
		if (!session) continue;

		auto pull = [=] {
			auto copy = _uploading;
			auto activeIds = base::flat_set<FullMsgId>();
			for (const auto &account : Core::App().domain().orderedAccounts()) {
				if (const auto s = account->maybeSession()) {
					for (const auto &info : s->uploader().activeUploads()) {
						activeIds.emplace(info.itemId);
					}
				}
			}
			for (const auto &info : session->uploader().activeUploads()) {
				const auto item = session->data().message(info.itemId);
				if (!item) continue;
				if (EnhancedForward::isEnhancedUpload(info.itemId)
					|| EnhancedForward::isEnhancedTempUpload(
						session,
						info.filename)) {
					continue;
				}
				// An entry still present in the upload queue is never a
				// finished upload: clear-finished must not drop it from the
				// tab together with the genuinely finished items.
				_uploaded.remove(info.itemId);
				if (!copy.remove(info.itemId)
					&& !_uploading.contains(info.itemId)
					&& !_downloading.contains(item)
					&& !_enhancedForward.contains(item)
					&& !_downloaded.contains(item)) {
					_uploading.emplace(info.itemId);
					addElementNow({
						.item = item,
						.started = int64(item->date()) * 1000,
						.path = info.filename,
						.ulBatch = info.batchId,
					});
					refreshPostponed(true);
				}
			}
			for (const auto &left : copy) {
				if (activeIds.contains(left)) {
					// This upload is still queued in some session (not this
					// one). Keeping it classified as finished would let the
					// clear-finished action remove it from the tab together
					// with the genuinely finished items.
					if (!_uploading.contains(left)) {
						_uploading.emplace(left);
					}
					continue;
				}
				_uploading.remove(left);
				_uploaded.emplace(left);
			}
			if (!copy.empty()) {
				refreshPostponed(false);
			}
			touchUlHeaders();
		};
		rpl::single(rpl::empty) | rpl::then(
			session->uploader().loadingListChanges() | rpl::to_empty
		) | rpl::on_next(pull, _lifetime);

		session->uploader().documentFailed(
		) | rpl::on_next([=](FullMsgId itemId) {
			_uploading.remove(itemId);
			_uploaded.remove(itemId);
			if (const auto item = session->data().message(itemId)) {
				remove(item);
				item->destroy();
			}
		}, _lifetime);
		session->uploader().photoFailed(
		) | rpl::on_next([=](FullMsgId itemId) {
			_uploading.remove(itemId);
			_uploaded.remove(itemId);
			if (const auto item = session->data().message(itemId)) {
				remove(item);
				item->destroy();
			}
		}, _lifetime);

		for (const auto &info : session->uploader().finishedUploadList()) {
			const auto item = session->data().message(info.itemId);
			if (!item) {
				_pendingFinishedUploads.emplace(
					info.itemId,
					PendingFinishedUpload{
						.session = session,
						.started = info.started,
						.path = info.filename,
						.batchId = info.batchId,
					});
				continue;
			}
			addFinishedUpload(session, info.itemId, {
				.session = session,
				.started = info.started,
				.path = info.filename,
				.batchId = info.batchId,
			});
		}
		resolvePendingFinishedUploads(session);

	session->uploader().finishedUploadAdded(
	) | rpl::on_next([=](FullMsgId itemId) {
		const auto item = session->data().message(itemId);
		if (!item) return;
		if (EnhancedForward::isEnhancedUpload(itemId)) {
			return;
		}
		if (ranges::any_of(_elements, [&](const Element &element) {
			return element.item.get() == item;
		})) {
			return;
		}
		if (!_uploaded.contains(itemId)
			&& !_uploading.contains(itemId)
			&& !_downloading.contains(item)
			&& !_enhancedForward.contains(item)
			&& !_downloaded.contains(item)) {
				_uploaded.emplace(itemId);
				auto batchId = uint64(0);
				for (const auto &info
					: session->uploader().finishedUploadList()) {
					if (info.itemId == itemId) {
						batchId = info.batchId;
						break;
					}
				}
				addElementNow({
					.item = item,
					.started = int64(item->date()) * 1000,
					.path = QString(),
					.ulBatch = batchId,
				});
				refreshPostponed(true);
				touchUlHeaders();
			}
		}, _lifetime);

		session->uploader().finishedUploadsCleared(
		) | rpl::on_next([=] {
			auto activeIds = base::flat_set<FullMsgId>();
			for (const auto &account : Core::App().domain().orderedAccounts()) {
				if (const auto s = account->maybeSession()) {
					for (const auto &info : s->uploader().activeUploads()) {
						activeIds.emplace(info.itemId);
					}
				}
			}
			const auto uploadedCopy = _uploaded;
			for (const auto &itemId : uploadedCopy) {
				_uploaded.remove(itemId);
				if (activeIds.contains(itemId)) {
					// Still queued in some uploader: keep it as an active
					// upload, not as a finished one that we are clearing.
					_uploading.emplace(itemId);
				}
			}
			// Only the finished items are cleared - active and paused uploads
			// must stay visible in the Uploads tab.
			for (auto i = _elements.begin(); i != _elements.end();) {
				const auto id = i->item->fullId();
				if (uploadedCopy.contains(id) && !activeIds.contains(id)) {
					i = _elements.erase(i);
				} else {
					++i;
				}
			}
			for (auto it = _layouts.begin(); it != _layouts.end();) {
				const auto id = it->first->fullId();
				if (uploadedCopy.contains(id) && !activeIds.contains(id)) {
					_layoutRemoved.fire(it->second.item.get());
					it = _layouts.erase(it);
				} else {
					++it;
				}
			}
			refreshPostponed(false);
		}, _lifetime);

		session->uploader().finishedUploadRemoved(
		) | rpl::on_next([=](FullMsgId itemId) {
			if (_uploaded.remove(itemId)) {
				if (const auto item = session->data().message(itemId)) {
					remove(item);
				}
			}
		}, _lifetime);

		session->data().itemIdChanged(
		) | rpl::on_next([=](const Data::Session::IdChange &change) {
			const auto oldFullId = FullMsgId(change.newId.peer, change.oldId);
			if (_uploaded.remove(oldFullId)) {
				_uploaded.emplace(change.newId);
			}
			if (_uploading.remove(oldFullId)) {
				_uploading.emplace(change.newId);
			}
			if (const auto i = _pendingFinishedUploads.find(oldFullId);
				i != _pendingFinishedUploads.end()) {
				const auto upload = i->second;
				_pendingFinishedUploads.erase(i);
				_pendingFinishedUploads.emplace(change.newId, upload);
				resolvePendingFinishedUploads(session);
			}
			if (const auto i = _pendingFinishedUploadsFetching.find(oldFullId);
				i != _pendingFinishedUploadsFetching.end()) {
				_pendingFinishedUploadsFetching.erase(i);
				_pendingFinishedUploadsFetching.emplace(change.newId);
			}
			if (const auto item = session->data().message(change.newId)) {
				if (const auto i = _layouts.find(item); i != _layouts.end()) {
					_layoutRemoved.fire(i->second.item.get());
					_layouts.erase(i);
					refreshPostponed(false);
				}
			}
		}, _lifetime);

		// The persisted resume rows are merged into the in-memory job states
		// once (peers may still be loading right after restart); afterwards
		// the Forwards list is driven purely by the push-based jobsValue
		// stream - no polling timer and no database reads on the hot path.
		EnhancedForward::EnsureResumeStatesSeeded(session);
		EnhancedForward::EnsureFinishedFwRunsSeeded(session);
		EnhancedForward::jobsValue(session) | rpl::on_next([=](const auto &jobs) {
			refreshEF(session, jobs);
		}, _lifetime);
		// Mirror the download/upload jobCounterChanged signals: refresh the
		// counter text only when the forward counts actually changed.
		EnhancedForward::counterChanges() | rpl::on_next([=] {
			updateCounter();
		}, _lifetime);
		NormalForward::countersChanged() | rpl::on_next([=] {
			updateCounter();
			refreshForwards();
		}, _lifetime);

		Core::App().downloadManager().jobCounterChanged(
		) | rpl::on_next([=] {
			updateCounter();
		}, _lifetime);
		session->uploader().jobCounterChanged(
		) | rpl::on_next([=] {
			updateCounter();
		}, _lifetime);
	}

	performAdd();
	performRefresh();
}

void Provider::refreshForwards() {
	auto sessions = std::vector<not_null<Main::Session*>>();
	sessions.push_back(&_controller->session());
	for (const auto &[tracked, lifetime] : _trackedSessions) {
		if (tracked != &_controller->session()) {
			sessions.push_back(tracked);
		}
	}
	for (const auto session : sessions) {
		refreshEF(session, EnhancedForward::MemoryJobs(session));
	}
}

void Provider::refreshEF(
		not_null<Main::Session*> session,
		const std::vector<EnhancedForward::JobSnapshot> &) {
	auto wantedItems = base::flat_set<not_null<const HistoryItem*>>();
	auto wantedDsts = base::flat_set<PeerId>();
	auto unresolved = std::vector<FullMsgId>();
	// Finished/resumable batches come first so that when a new job runs for
	// the same peer the merged Forwards list keeps a stable source order.
	auto ordered = EnhancedForward::AllJobs(session);
	ranges::stable_sort(
		ordered,
		ranges::less(),
		[](const EnhancedForward::JobSnapshot &job) {
			return !(job.finished || job.resumable);
		});
	for (const auto &job : ordered) {
		if (job.progress.state == EnhancedForward::State::Cancelled) {
			continue;
		}
		const auto active = job.active && !job.finished;
		for (auto i = 0; i < int(job.progress.sourceIds.size()); i++) {
			if (int(job.progress.items.size()) > i
				&& (job.progress.items[i].cancelled
					|| job.progress.items[i].dedupSkipped)) {
				continue;
			}
			const auto srcId = job.progress.sourceIds[i];
			const auto pending = (active || job.resumable)
				&& int(job.progress.items.size()) > i
				&& job.progress.items[i].state
					== EnhancedForward::ItemState::Pending;
			if (pending) {
				_efPending.emplace(srcId);
			} else {
				_efPending.remove(srcId);
			}
		}
		if (!active) {
			continue;
		}
		for (const auto idx : {
			job.progress.currentDownload,
			job.progress.currentUpload }) {
			if (idx < 0 || idx >= int(job.progress.sourceIds.size())) {
				continue;
			}
			const auto srcId = job.progress.sourceIds[idx];
			const auto message = session->data().message(srcId);
			if (!message) {
				if (IsServerMsgId(srcId.msg)
					&& !_failedEFResolve.contains(srcId)) {
					unresolved.push_back(srcId);
				}
				continue;
			}
			const auto item = not_null<HistoryItem*>(message);
			wantedItems.emplace(item);
			if (!_enhancedForward.contains(item)) {
				_enhancedForward.emplace(item);
				trackItemSession(item);
			}
		}
	}
	// Forward rows: one live aggregate per destination with live or
	// resumable work, plus one row per kept finished run. Live and
	// resumable runs provide their own carriers; kept finished runs
	// resolve the carrier from their persisted first source.
	auto liveCarriers = std::vector<std::pair<PeerId, HistoryItem*>>();
	for (const auto &job : ordered) {
		if (job.progress.state == EnhancedForward::State::Cancelled
			|| wantedDsts.contains(job.peer)) {
			continue;
		}
		const auto live = job.active && !job.finished;
		if (!live && !job.resumable) {
			continue;
		}
		auto carrier = (HistoryItem*)nullptr;
		for (const auto &srcId : job.progress.sourceIds) {
			if (const auto message = session->data().message(srcId)) {
				carrier = message;
				break;
			} else if (IsServerMsgId(srcId.msg)
				&& !_failedEFResolve.contains(srcId)) {
				unresolved.push_back(srcId);
			}
		}
		if (!carrier) {
			continue;
		}
		wantedDsts.emplace(job.peer);
		liveCarriers.emplace_back(job.peer, carrier);
	}
	for (const auto &counters : NormalForward::AllCounters(session)) {
		if (!counters.active
			|| !counters.firstSource
			|| wantedDsts.contains(counters.dst)) {
			continue;
		}
		const auto message = session->data().message(counters.firstSource);
		if (!message) {
			continue;
		}
		wantedDsts.emplace(counters.dst);
		liveCarriers.emplace_back(counters.dst, message);
	}
	auto keptRuns = std::vector<
		std::pair<EnhancedForward::FinishedFwRun, HistoryItem*>>();
	for (const auto &record : EnhancedForward::FinishedFwRuns(session)) {
		if (!record.firstSource) {
			continue;
		}
		if (const auto message = session->data().message(record.firstSource)) {
			keptRuns.emplace_back(record, message);
		} else if (IsServerMsgId(record.firstSource.msg)
			&& !_failedEFResolve.contains(record.firstSource)) {
			unresolved.push_back(record.firstSource);
		}
	}
	ranges::stable_sort(
		keptRuns,
		ranges::less(),
		[](const auto &entry) { return entry.first.finishedAt; });
	const auto digest = FwTickDigest(session, ordered, liveCarriers, keptRuns);
	const auto digestIt = _fwTickDigest.find(session);
	if (digestIt != end(_fwTickDigest) && digestIt->second == digest) {
		updateAvailability();
		return;
	}
	if (digestIt != end(_fwTickDigest)) {
		digestIt->second = digest;
	} else {
		_fwTickDigest.emplace(session, digest);
	}
	auto absorbingDsts = base::flat_set<PeerId>();
	for (const auto &batch : EnhancedForward::ForwardBatches(session)) {
		if (!batch.finished) {
			absorbingDsts.emplace(batch.dst);
		}
	}
	struct KeptGroup {
		PeerId dst;
		uint64 action = 0;
		std::vector<EnhancedForward::FinishedFwRun> runs;
		HistoryItem *carrier = nullptr;
	};
	auto keptGroups = std::vector<KeptGroup>();
	for (const auto &[run, message] : keptRuns) {
		auto found = false;
		for (auto &group : keptGroups) {
			if (group.dst == run.dst && group.action == run.actionId) {
				group.runs.push_back(run);
				found = true;
				break;
			}
		}
		if (!found) {
			auto group = KeptGroup();
			group.dst = run.dst;
			group.action = run.actionId;
			group.runs.push_back(run);
			group.carrier = message;
			keptGroups.push_back(std::move(group));
		}
	}
	auto wantedGroups = base::flat_set<uint64>();
	auto added = false;
	for (const auto &[dst, carrier] : liveCarriers) {
		added |= ensureFwAggregate(
			session,
			dst,
			not_null<HistoryItem*>(carrier));
		for (const auto &group : keptGroups) {
			if (group.dst != dst) {
				continue;
			}
			if (absorbingDsts.contains(dst)
				&& group.action
					== EnhancedForward::CurrentForwardActionId(
						session,
						dst)) {
				continue;
			}
			wantedGroups.emplace(group.action);
			added |= ensureFwFinishedGroup(
				session,
				group.dst,
				group.action,
				not_null<HistoryItem*>(group.carrier));
		}
	}
	for (const auto &group : keptGroups) {
		if (wantedDsts.contains(group.dst)
			|| wantedGroups.contains(group.action)) {
			continue;
		}
		if (absorbingDsts.contains(group.dst)
			&& group.action
				== EnhancedForward::CurrentForwardActionId(
					session,
					group.dst)) {
			continue;
		}
		wantedGroups.emplace(group.action);
		added |= ensureFwFinishedGroup(
			session,
			group.dst,
			group.action,
			not_null<HistoryItem*>(group.carrier));
	}
	if (added) {
		refreshPostponed(true);
	}
	auto toRemove = std::vector<not_null<const HistoryItem*>>();
	for (const auto &item : _enhancedForward) {
		if (&item->history()->session() != session.get()
			|| wantedItems.contains(item)) {
			continue;
		}
		const auto stillLoading = ranges::any_of(
			Core::App().downloadManager().loadingList(),
			[&](const auto &id) {
				return id->enhancedForward
					&& (id->object.item == item);
			});
		if (!stillLoading) {
			toRemove.push_back(item);
		}
	}
	for (const auto &item : toRemove) {
		_enhancedForward.remove(item);
		if (isForwardAggregate(item)) {
			continue;
		}
		remove(item);
	}
	auto toPrune = std::vector<std::pair<PeerId, uint64>>();
	for (const auto &[key, cached] : _fwLayouts) {
		if (std::get<0>(key) != session.get()) {
			continue;
		}
		const auto dst = std::get<1>(key);
		const auto group = std::get<2>(key);
		const auto wanted = (group == 0)
			? wantedDsts.contains(dst)
			: wantedGroups.contains(group);
		if (!wanted) {
			toPrune.emplace_back(dst, group);
		}
	}
	for (const auto &[dst, group] : toPrune) {
		if (group == 0) {
			removeFwAggregate(session, dst);
		} else {
			removeFwFinishedGroup(session, group);
		}
	}
	if (!unresolved.empty()) {
		auto toFetch = std::vector<FullMsgId>();
		for (const auto &id : unresolved) {
			if (_fetchingEFResolve.contains(id)) {
				continue;
			}
			_fetchingEFResolve.emplace(id);
			toFetch.push_back(id);
		}		if (!toFetch.empty()) {
			const auto weak = base::make_weak(this);
			EnhancedForward::EnsureForwardSourceMessages(
				session,
				toFetch,
				[weak, session, toFetch](bool ok) {
					if (!weak) {
						return;
					}
					for (const auto &id : toFetch) {
						weak->_fetchingEFResolve.remove(id);
						if (ok) {
							weak->_failedEFResolve.remove(id);
						} else {
							weak->_failedEFResolve.emplace(id);
						}
					}
					weak->refreshEF(
						session,
						EnhancedForward::MemoryJobs(session));
					EnhancedForward::notifyTransfersUpdated();
				});
		}
	}
	for (auto &pair : _layouts) {
		if (wantedItems.contains(pair.first)) {
			pair.second.item->itemDataChanged();
		}
	}
	updateAvailability();
}

bool Provider::ensureFwAggregate(
		not_null<Main::Session*> session,
		const PeerId &dst,
		not_null<HistoryItem*> carrier) {
	if (const auto found = ranges::find_if(
		_elements,
		[&](const Element &element) {
			return element.fwRow == 1
				&& element.fwGroup == 0
				&& element.fwDst == dst
				&& &element.item->history()->session() == session.get();
		}); found == end(_elements)) {
		addElementNow(Element{
			carrier,
			int64(carrier->date()) * 1000,
			QString(),
			0,
			dst,
			1,
			uint64(0),
		});
		trackItemSession(carrier);
		if (const auto it = _fwLayouts.find({ session.get(), dst, uint64(0) });
			it != end(_fwLayouts)) {
			it->second.item->itemDataChanged();
		}
		return true;
	}
	if (const auto it = _fwLayouts.find({ session.get(), dst, uint64(0) });
		it != end(_fwLayouts)) {
		it->second.item->itemDataChanged();
	}
	return false;
}

bool Provider::ensureFwFinishedGroup(
		not_null<Main::Session*> session,
		const PeerId &dst,
		uint64 actionId,
		not_null<HistoryItem*> carrier) {
	if (const auto found = ranges::find_if(
		_elements,
		[&](const Element &element) {
			return element.fwRow == 1
				&& element.fwGroup == actionId
				&& &element.item->history()->session() == session.get();
		}); found == end(_elements)) {
		addElementNow(Element{
			carrier,
			int64(carrier->date()) * 1000,
			QString(),
			0,
			dst,
			1,
			actionId,
		});
		trackItemSession(carrier);
		if (const auto it = _fwLayouts.find(
			{ session.get(), dst, actionId });
			it != end(_fwLayouts)) {
			it->second.item->itemDataChanged();
		}
		return true;
	}
	if (const auto it = _fwLayouts.find(
		{ session.get(), dst, actionId });
		it != end(_fwLayouts)) {
		it->second.item->itemDataChanged();
	}
	return false;
}

void Provider::removeFwAggregate(
		not_null<Main::Session*> session,
		const PeerId &dst) {
	if (const auto it = _fwLayouts.find({ session.get(), dst, uint64(0) });
		it != end(_fwLayouts)) {
		_layoutRemoved.fire(it->second.item.get());
		_fwLayouts.erase(it);
	}
	auto carriers = std::vector<not_null<const HistoryItem*>>();
	for (auto i = _elements.begin(); i != _elements.end();) {
		if (i->fwRow == 1
			&& i->fwGroup == 0
			&& i->fwDst == dst
			&& &i->item->history()->session() == session.get()) {
			if (i->found && searchMode()) {
				--_foundCount;
			}
			carriers.push_back(i->item);
			i = _elements.erase(i);
		} else {
			++i;
		}
	}
	for (const auto &carrier : carriers) {
		const auto stillUsed = ranges::any_of(
			_elements,
			[&](const Element &element) { return element.item == carrier; });
		if (!stillUsed) {
			_enhancedForward.remove(carrier);
		}
	}
	refreshPostponed(false);
}

void Provider::removeFwFinishedGroup(
		not_null<Main::Session*> session,
		uint64 actionId) {
	for (auto i = _fwLayouts.begin(); i != end(_fwLayouts);) {
		if (std::get<0>(i->first) == session.get()
			&& std::get<2>(i->first) == actionId) {
			_layoutRemoved.fire(i->second.item.get());
			i = _fwLayouts.erase(i);
		} else {
			++i;
		}
	}
	auto carriers = std::vector<not_null<const HistoryItem*>>();
	for (auto i = _elements.begin(); i != _elements.end();) {
		if (i->fwRow == 1
			&& i->fwGroup == actionId
			&& &i->item->history()->session() == session.get()) {
			if (i->found && searchMode()) {
				--_foundCount;
			}
			carriers.push_back(i->item);
			i = _elements.erase(i);
		} else {
			++i;
		}
	}
	for (const auto &carrier : carriers) {
		const auto stillUsed = ranges::any_of(
			_elements,
			[&](const Element &element) { return element.item == carrier; });
		if (!stillUsed) {
			_enhancedForward.remove(carrier);
		}
	}
	refreshPostponed(false);
}

void Provider::addFinishedUpload(
		not_null<Main::Session*> session,
		FullMsgId itemId,
		const PendingFinishedUpload &upload) {
	const auto item = session->data().message(itemId);
	if (!item) return;
	if (EnhancedForward::isEnhancedUpload(itemId)
		|| EnhancedForward::isEnhancedTempUpload(session, upload.path)) {
		return;
	}
if (ranges::any_of(_elements, [&](const Element &element) {
			return element.item.get() == item;
		})) {
			return;
		}
if (!_uploaded.contains(itemId)
		&& !_uploading.contains(itemId)
		&& !_downloading.contains(item)
		&& !_enhancedForward.contains(item)
		&& !_downloaded.contains(item)) {
		_uploaded.emplace(itemId);
		addElementNow({
			.item = item,
			.started = (upload.started
				? upload.started
				: int64(item->date()) * 1000),
			.path = upload.path,
			.ulBatch = upload.batchId,
		});
		refreshPostponed(true);
		touchUlHeaders();
	}
}

void Provider::resolvePendingFinishedUploads(
		not_null<Main::Session*> session) {
	auto unresolved = std::vector<FullMsgId>();
	for (auto i = _pendingFinishedUploads.begin();
			i != _pendingFinishedUploads.end();) {
		const auto &upload = i->second;
		if (upload.session != session) {
			++i;
			continue;
		}
		const auto itemId = i->first;
		if (session->data().message(itemId)) {
			addFinishedUpload(session, itemId, upload);
			i = _pendingFinishedUploads.erase(i);
			continue;
		}
		if (IsServerMsgId(itemId.msg)
			&& !_pendingFinishedUploadsFetching.contains(itemId)) {
			unresolved.push_back(itemId);
			_pendingFinishedUploadsFetching.emplace(itemId);
		}
		++i;
	}
	if (!unresolved.empty()) {
		const auto weak = base::make_weak(this);
		EnhancedForward::EnsureForwardSourceMessages(
			session,
			unresolved,
			[this, weak, session, unresolved](bool ok) {
				if (!weak) {
					return;
				}
				for (const auto &itemId : unresolved) {
					_pendingFinishedUploadsFetching.remove(itemId);
				}
				if (ok) {
					for (const auto &itemId : unresolved) {
						if (session->data().message(itemId)) {
							continue;
						}
						_pendingFinishedUploads.erase(itemId);
						session->uploader().removeFinishedUpload(itemId);
					}
				}
				resolvePendingFinishedUploads(session);
			});
	}
}

void Provider::addPostponed(not_null<const Data::DownloadedId*> entry) {
	Expects(entry->object != nullptr);

	const auto item = entry->object->item;
	// Enhanced-forward temp downloads should never appear in the regular
	// Downloads tab: the DownloadManager records every finished document
	// (including the ones enhanced forward downloaded to ForwardTemp/), but
	// those are only meaningful in the Forwards tab.
	if (EnhancedForward::isEnhancedTempUpload(
			&item->history()->session(),
			entry->path)) {
		return;
	}
	trackItemSession(item);
	const auto i = ranges::find(_addPostponed, item, &Element::item);
	if (i != end(_addPostponed)) {
		i->path = entry->path;
		i->started = entry->started;
		i->dlBatch = entry->batchId;
	} else {
		_addPostponed.push_back({
			.item = item,
			.started = entry->started,
			.path = entry->path,
			.dlBatch = entry->batchId,
		});
		if (_addPostponed.size() == 1) {
			Ui::PostponeCall(this, [=] {
				performAdd();
			});
		}
	}
}

void Provider::performAdd() {
	if (_addPostponed.empty()) {
		return;
	}
	for (auto &element : base::take(_addPostponed)) {
		_downloaded.emplace(element.item);
		const auto already = ranges::contains(
			_elements,
			element.item,
			&Element::item);
		if (!already) {
			addElementNow(std::move(element));
		}
	}
	refreshPostponed(true);
}

void Provider::addElementNow(Element &&element) {
	const auto itemId = element.item->fullId();
	const auto already = ranges::find_if(
		_elements,
		[&](const Element &existing) {
			if (existing.dlRow != element.dlRow) {
				return false;
			}
			if (element.dlRow == 1) {
				return existing.dlBatch == element.dlBatch
					&& &existing.item->history()->session()
						== &element.item->history()->session();
			}
			if (existing.ulRow != element.ulRow) {
				return false;
			}
			if (element.ulRow == 1) {
				return existing.ulBatch == element.ulBatch
					&& &existing.item->history()->session()
						== &element.item->history()->session();
			}
			if (existing.fwRow != element.fwRow) {
				return false;
			}
			if (element.fwRow == 1) {
				return existing.fwDst == element.fwDst
					&& existing.fwGroup == element.fwGroup
					&& &existing.item->history()->session()
						== &element.item->history()->session();
			}
			return existing.item->fullId() == itemId;
		});
	if (already != end(_elements)) {
		// The same item may be reported by several sources at once (the
		// loading list, the EF job list, uploader events): keep one row.
		return;
	}
	element.order = _nextElementOrder++;
	_elements.push_back(std::move(element));
	auto &added = _elements.back();
	fillSearchIndex(added);
	added.found = searchMode() && computeIsFound(added);
	if (added.found) {
		++_foundCount;
	}
}

void Provider::remove(not_null<const HistoryItem*> item) {
	_addPostponed.erase(
		ranges::remove(_addPostponed, item, &Element::item),
		end(_addPostponed));
	_downloading.remove(item);
	_enhancedForward.remove(item);
	_downloaded.remove(item);
	_efPending.remove(item->fullId());
	const auto proj = [&](const Element &element) {
		if (element.item != item) {
			return false;
		} else if (element.found && searchMode()) {
			--_foundCount;
		}
		return true;
	};
	_elements.erase(ranges::remove_if(_elements, proj), end(_elements));
	if (const auto i = _layouts.find(item); i != end(_layouts)) {
		_layoutRemoved.fire(i->second.item.get());
		// The list widget handles layoutRemoved() synchronously and may
		// refresh its height from there, which can reach refreshViewer()
		// -> refreshRows() -> fillSections() -> clearStaleLayouts() before
		// we get back here, erasing this very entry, so look it up again.
		if (const auto j = _layouts.find(item); j != end(_layouts)) {
			_layouts.erase(j);
		}
	}
	for (auto i = _fwLayouts.begin(); i != _fwLayouts.end();) {
		if (i->second.item->getItem() == item) {
			_layoutRemoved.fire(i->second.item.get());
			i = _fwLayouts.erase(i);
		} else {
			++i;
		}
	}
	for (auto i = _dlLayouts.begin(); i != _dlLayouts.end();) {
		if (i->second.item->getItem() == item) {
			_layoutRemoved.fire(i->second.item.get());
			i = _dlLayouts.erase(i);
		} else {
			++i;
		}
	}
	for (auto i = _ulLayouts.begin(); i != _ulLayouts.end();) {
		if (i->second.item->getItem() == item) {
			_layoutRemoved.fire(i->second.item.get());
			i = _ulLayouts.erase(i);
		} else {
			++i;
		}
	}
	refreshPostponed(false);
}

void Provider::refreshPostponed(bool added) {
	if (added) {
		_postponedRefreshSort = true;
	}
	if (!_postponedRefresh) {
		_postponedRefresh = true;
		Ui::PostponeCall(this, [=] {
			performRefresh();
		});
	}
}

void Provider::performRefresh() {
	if (!_postponedRefresh) {
		return;
	}
	_postponedRefresh = false;
	if (!_elements.empty() || _fullCount.has_value()) {
		_fullCount = _elements.size();
	}
	if (base::take(_postponedRefreshSort)) {
		// Forwards keep the job's source order; Downloads/Uploads sort by
		// their start time so a rebuilt provider (reopened tab) still shows
		// source-chat order instead of completion order.
		if (_filter == Filter::Forwards) {
			ranges::stable_sort(_elements, ranges::less(), &Element::order);
		} else {
			ranges::stable_sort(_elements, ranges::less(), &Element::started);
		}
	}
	syncDlRuns();
	syncUlRuns();
	_refreshed.fire({});
	updateAvailability();
	updateCounter();
}

void Provider::trackItemSession(not_null<const HistoryItem*> item) {
	const auto session = &item->history()->session();
	if (_trackedSessions.contains(session)) {
		return;
	}
	auto &lifetime = _trackedSessions.emplace(session).first->second;

	session->data().itemRemoved(
	) | rpl::on_next([this](auto item) {
		itemRemoved(item);
	}, lifetime);

	session->account().sessionChanges(
	) | rpl::take(1) | rpl::on_next([=] {
		_trackedSessions.remove(session);
		_fwTickDigest.remove(session);
	}, lifetime);
}

rpl::producer<> Provider::refreshed() {
	return _refreshed.events();
}

void Provider::syncDlRuns() {
	auto &manager = Core::App().downloadManager();
	const auto runs = manager.dlRuns();
	const auto sessionOf = [](const Element &element) {
		return element.item->history()->session().uniqueId();
	};
	auto changed = false;
	auto runsToDrop = std::vector<uint64>();
	for (const auto &run : runs) {
		auto carrier = (HistoryItem*)nullptr;
		for (const auto &element : _elements) {
			if (element.dlRow != 0
				|| element.dlBatch != run.batchId
				|| sessionOf(element) != run.sessionId) {
				continue;
			}
			carrier = element.item;
			break;
		}
		if (!carrier) {
			continue;
		}
		const auto exists = ranges::any_of(
			_elements,
			[&](const Element &element) {
				return element.dlRow == 1
					&& element.dlBatch == run.batchId
					&& sessionOf(element) == run.sessionId;
			});
		if (!exists) {
			addElementNow(Element{
				.item = not_null<HistoryItem*>(carrier),
				.started = int64(run.startedAt) * 1000,
				.dlBatch = run.batchId,
				.dlRow = 1,
			});
			changed = true;
		}
	}
	for (auto i = _elements.begin(); i != _elements.end();) {
		if (i->dlRow != 1) {
			++i;
			continue;
		}
		const auto batchId = i->dlBatch;
		const auto sessionId = sessionOf(*i);
		const auto runIt = ranges::find_if(
			runs,
			[&](const Data::DownloadManager::DlRunSnapshot &run) {
				return run.batchId == batchId
					&& run.sessionId == sessionId;
			});
		const auto hasMembers = ranges::any_of(
			_elements,
			[&](const Element &element) {
				return element.dlRow == 0
					&& element.dlBatch == batchId
					&& sessionOf(element) == sessionId;
			});
		if (runIt != end(runs) && hasMembers) {
			++i;
			continue;
		}
		if (runIt != end(runs) && runIt->finished && !hasMembers) {
			runsToDrop.push_back(batchId);
		}
		if (i->found && searchMode()) {
			--_foundCount;
		}
		const auto key = std::make_tuple(
			&i->item->history()->session(),
			batchId);
		i = _elements.erase(i);
		if (const auto lit = _dlLayouts.find(key);
			lit != end(_dlLayouts)) {
			_layoutRemoved.fire(lit->second.item.get());
			_dlLayouts.erase(lit);
		}
		if (runIt == end(runs) && batchId) {
			for (auto &element : _elements) {
				if (element.dlRow == 0 && element.dlBatch == batchId) {
					element.dlBatch = 0;
				}
			}
		}
		changed = true;
	}
	// dropDlRun fires loadingListChanges synchronously, which re-enters the
	// provider pull that can mutate _elements, so it runs after the loops.
	for (const auto batchId : runsToDrop) {
		manager.dropDlRun(batchId);
	}
	if (changed) {
		_postponedRefreshSort = true;
		refreshPostponed(true);
	}
}

void Provider::touchDlHeaders() {
	for (auto &pair : _dlLayouts) {
		pair.second.item->itemDataChanged();
	}
}

bool Provider::isDlHeaderLayout(
		not_null<const Media::BaseLayout*> layout) const {
	for (const auto &pair : _dlLayouts) {
		if (pair.second.item.get() == layout.get()) {
			return true;
		}
	}
	return false;
}

void Provider::removeDlHeader(uint64 batchId) {
	if (!batchId) {
		return;
	}
	for (auto i = _elements.begin(); i != _elements.end();) {
		if (i->dlRow == 1 && i->dlBatch == batchId) {
			if (i->found && searchMode()) {
				--_foundCount;
			}
			i = _elements.erase(i);
		} else {
			if (i->dlRow == 0 && i->dlBatch == batchId) {
				i->dlBatch = 0;
			}
			++i;
		}
	}
	for (auto i = _dlLayouts.begin(); i != end(_dlLayouts);) {
		if (std::get<1>(i->first) == batchId) {
			_layoutRemoved.fire(i->second.item.get());
			i = _dlLayouts.erase(i);
		} else {
			++i;
		}
	}
	refreshPostponed(false);
}

void Provider::syncUlRuns() {
	auto &manager = Core::App().downloadManager();
	const auto runs = manager.ulRuns();
	const auto sessionOf = [](const Element &element) {
		return element.item->history()->session().uniqueId();
	};
	auto changed = false;
	auto runsToDrop = std::vector<uint64>();
	for (const auto &run : runs) {
		auto carrier = (HistoryItem*)nullptr;
		for (const auto &element : _elements) {
			if (element.ulRow != 0
				|| element.ulBatch != run.batchId
				|| sessionOf(element) != run.sessionId) {
				continue;
			}
			carrier = element.item;
			break;
		}
		if (!carrier) {
			continue;
		}
		const auto exists = ranges::any_of(
			_elements,
			[&](const Element &element) {
				return element.ulRow == 1
					&& element.ulBatch == run.batchId
					&& sessionOf(element) == run.sessionId;
			});
		if (!exists) {
			addElementNow(Element{
				.item = not_null<HistoryItem*>(carrier),
				.started = int64(run.startedAt) * 1000,
				.ulBatch = run.batchId,
				.ulRow = 1,
			});
			changed = true;
		}
	}
	for (auto i = _elements.begin(); i != _elements.end();) {
		if (i->ulRow != 1) {
			++i;
			continue;
		}
		const auto batchId = i->ulBatch;
		const auto sessionId = sessionOf(*i);
		const auto runIt = ranges::find_if(
			runs,
			[&](const Data::DownloadManager::UlRunSnapshot &run) {
				return run.batchId == batchId
					&& run.sessionId == sessionId;
			});
		const auto hasMembers = ranges::any_of(
			_elements,
			[&](const Element &element) {
				return element.ulRow == 0
					&& element.ulBatch == batchId
					&& sessionOf(element) == sessionId;
			});
		if (runIt != end(runs) && hasMembers) {
			++i;
			continue;
		}
		if (runIt != end(runs) && runIt->finished && !hasMembers) {
			runsToDrop.push_back(batchId);
		}
		if (i->found && searchMode()) {
			--_foundCount;
		}
		const auto key = std::make_tuple(
			&i->item->history()->session(),
			batchId);
		i = _elements.erase(i);
		if (const auto lit = _ulLayouts.find(key);
			lit != end(_ulLayouts)) {
			_layoutRemoved.fire(lit->second.item.get());
			_ulLayouts.erase(lit);
		}
		if (runIt == end(runs) && batchId) {
			for (auto &element : _elements) {
				if (element.ulRow == 0 && element.ulBatch == batchId) {
					element.ulBatch = 0;
				}
			}
		}
		changed = true;
	}
	// dropUlRun fires loadingListChanges synchronously, which re-enters the
	// provider pull that can mutate _elements, so it runs after the loops.
	for (const auto batchId : runsToDrop) {
		manager.dropUlRun(batchId);
	}
	if (changed) {
		_postponedRefreshSort = true;
		refreshPostponed(true);
	}
}

void Provider::touchUlHeaders() {
	for (auto &pair : _ulLayouts) {
		pair.second.item->itemDataChanged();
	}
}

bool Provider::isUlHeaderLayout(
		not_null<const Media::BaseLayout*> layout) const {
	for (const auto &pair : _ulLayouts) {
		if (pair.second.item.get() == layout.get()) {
			return true;
		}
	}
	return false;
}

void Provider::removeUlHeader(uint64 batchId) {
	if (!batchId) {
		return;
	}
	for (auto i = _elements.begin(); i != _elements.end();) {
		if (i->ulRow == 1 && i->ulBatch == batchId) {
			if (i->found && searchMode()) {
				--_foundCount;
			}
			i = _elements.erase(i);
		} else {
			if (i->ulRow == 0 && i->ulBatch == batchId) {
				i->ulBatch = 0;
			}
			++i;
		}
	}
	for (auto i = _ulLayouts.begin(); i != end(_ulLayouts);) {
		if (std::get<1>(i->first) == batchId) {
			_layoutRemoved.fire(i->second.item.get());
			i = _ulLayouts.erase(i);
		} else {
			++i;
		}
	}
	refreshPostponed(false);
}

void Provider::appendGroupedUploads(
		Media::ListSection &section,
		not_null<Overview::Layout::Delegate*> delegate,
		bool search,
		Fn<bool(const Element&)> accept) {
	auto &manager = Core::App().downloadManager();
	const auto runs = manager.ulRuns();
	const auto sessionOf = [](const Element &element) {
		return element.item->history()->session().uniqueId();
	};
	auto emitted = base::flat_set<uint64>();
	for (const auto &run : runs) {
		auto header = (const Element*)nullptr;
		for (const auto &element : _elements) {
			if (element.ulRow == 1
				&& element.ulBatch == run.batchId
				&& sessionOf(element) == run.sessionId
				&& accept(element)) {
				header = &element;
				break;
			}
		}
		if (!header) {
			continue;
		}
		auto members = std::vector<const Element*>();
		for (const auto &element : _elements) {
			if (element.ulRow == 0
				&& element.ulBatch == run.batchId
				&& sessionOf(element) == run.sessionId
				&& accept(element)) {
				members.push_back(&element);
			}
		}
		const auto headerShown = !search
			|| header->found
			|| ranges::any_of(members, [](const Element *member) {
				return member->found;
			});
		if (!headerShown) {
			for (const auto member : members) {
				if (member->found) {
					if (auto layout = getLayout(*member, delegate)) {
						section.addItem(layout);
					}
				}
			}
			emitted.emplace(run.batchId);
			continue;
		}
		if (auto layout = getLayout(*header, delegate)) {
			section.addItem(layout);
		}
		for (const auto member : members) {
			if (search && !member->found && !header->found) {
				continue;
			}
			if (auto layout = getLayout(*member, delegate)) {
				section.addItem(layout);
			}
		}
		emitted.emplace(run.batchId);
	}
	auto rest = std::vector<const Element*>();
	for (const auto &element : _elements) {
		if (element.ulRow == 1) {
			continue;
		} else if (element.ulBatch != 0
			&& emitted.contains(element.ulBatch)) {
			continue;
		} else if (!accept(element)) {
			continue;
		} else if (search && !element.found) {
			continue;
		}
		rest.push_back(&element);
	}
	ranges::stable_sort(rest, ranges::less(), &Element::started);
	for (const auto element : rest) {
		if (auto layout = getLayout(*element, delegate)) {
			section.addItem(layout);
		}
	}
}

bool Provider::isUploadAggregate(
		not_null<const HistoryItem*> item) const {
	return ranges::any_of(_elements, [&](const Element &element) {
		return element.ulRow == 1 && element.item == item;
	});
}

uint64 Provider::uploadAggregateBatchId(
		not_null<const HistoryItem*> item) const {
	for (const auto &element : _elements) {
		if (element.ulRow == 1 && element.item == item) {
			return element.ulBatch;
		}
	}
	return 0;
}

bool Provider::isUploadAggregateFinished(
		not_null<const HistoryItem*> item) const {
	const auto batchId = uploadAggregateBatchId(item);
	if (!batchId) {
		return true;
	}
	for (const auto &run : Core::App().downloadManager().ulRuns()) {
		if (run.batchId == batchId) {
			return run.finished;
		}
	}
	return true;
}

void Provider::clearUploadRun(uint64 batchId) {
	if (!batchId) {
		return;
	}
	auto &manager = Core::App().downloadManager();
	auto members = std::vector<not_null<HistoryItem*>>();
	for (const auto &element : _elements) {
		if (element.ulRow == 0 && element.ulBatch == batchId) {
			members.push_back(element.item);
		}
	}
	// cancel()/removeFinishedUpload() synchronously re-enter remove()
	// through uploader signals, so the members are collected first.
	for (const auto item : members) {
		const auto session = &item->history()->session();
		if (_uploading.contains(item->fullId())) {
			session->uploader().cancel(item->fullId());
		} else {
			session->uploader().removeFinishedUpload(item->fullId());
		}
	}
	manager.dropUlBatch(batchId);
	removeUlHeader(batchId);
	refreshPostponed(false);
}

void Provider::appendGroupedDownloads(
		Media::ListSection &section,
		not_null<Overview::Layout::Delegate*> delegate,
		bool search,
		Fn<bool(const Element&)> accept) {
	auto &manager = Core::App().downloadManager();
	const auto runs = manager.dlRuns();
	const auto sessionOf = [](const Element &element) {
		return element.item->history()->session().uniqueId();
	};
	auto emitted = base::flat_set<uint64>();
	for (const auto &run : runs) {
		auto header = (const Element*)nullptr;
		for (const auto &element : _elements) {
			if (element.dlRow == 1
				&& element.dlBatch == run.batchId
				&& sessionOf(element) == run.sessionId
				&& accept(element)) {
				header = &element;
				break;
			}
		}
		if (!header) {
			continue;
		}
		auto members = std::vector<const Element*>();
		for (const auto &element : _elements) {
			if (element.dlRow == 0
				&& element.dlBatch == run.batchId
				&& sessionOf(element) == run.sessionId
				&& accept(element)) {
				members.push_back(&element);
			}
		}
		const auto headerShown = !search
			|| header->found
			|| ranges::any_of(members, [](const Element *member) {
				return member->found;
			});
		if (!headerShown) {
			for (const auto member : members) {
				if (member->found) {
					if (auto layout = getLayout(*member, delegate)) {
						section.addItem(layout);
					}
				}
			}
			emitted.emplace(run.batchId);
			continue;
		}
		if (auto layout = getLayout(*header, delegate)) {
			section.addItem(layout);
		}
		for (const auto member : members) {
			if (search && !member->found && !header->found) {
				continue;
			}
			if (auto layout = getLayout(*member, delegate)) {
				section.addItem(layout);
			}
		}
		emitted.emplace(run.batchId);
	}
	auto rest = std::vector<const Element*>();
	for (const auto &element : _elements) {
		if (element.dlRow == 1) {
			continue;
		} else if (element.dlBatch != 0
			&& emitted.contains(element.dlBatch)) {
			continue;
		} else if (!accept(element)) {
			continue;
		} else if (search && !element.found) {
			continue;
		}
		rest.push_back(&element);
	}
	ranges::stable_sort(rest, ranges::less(), &Element::started);
	for (const auto element : rest) {
		if (auto layout = getLayout(*element, delegate)) {
			section.addItem(layout);
		}
	}
}

std::vector<ListSection> Provider::fillSections(
		not_null<Overview::Layout::Delegate*> delegate) {
	const auto search = searchMode();

	if (!search) {
		markLayoutsStale();
	}
	const auto guard = gsl::finally([&] { clearStaleLayouts(); });

	_showGroupHeaders = (_filter == Filter::All);

	if (_elements.empty() || (search && !_foundCount)) {
		return {};
	}

	const auto isEf = [this](not_null<const HistoryItem*> item) {
		return _enhancedForward.contains(item);
	};
	const auto matches = [&](const Element &element) {
		const auto item = element.item;
		const auto ef = isEf(item);
		const auto nf = isForwardAggregate(item);
		if (_filter == Filter::Forwards) {
			return element.fwRow == 1;
		}
		if (element.dlRow == 1 || element.dlBatch != 0) {
			return _filter == Filter::Downloads
				|| _filter == Filter::All;
		}
		if (element.ulRow == 1 || element.ulBatch != 0) {
			return _filter == Filter::Uploads
				|| _filter == Filter::All;
		}
		if (nf) {
			return _filter == Filter::All;
		}
		if (ef) {
			const auto genuine = _uploading.contains(item->fullId())
				|| _uploaded.contains(item->fullId())
				|| _downloading.contains(item);
			if (!genuine) {
				return false;
			}
		}
		if (_filter == Filter::All) {
			return true;
		}
		const auto upload = isUploadItem(item);
		return (_filter == Filter::Uploads) ? upload : !upload;
	};

	auto result = std::vector<ListSection>();
	if (_showGroupHeaders) {
		auto downloads = ListSection(Type::File, sectionDelegate());
		auto uploads = ListSection(Type::File, sectionDelegate());
		auto forwards = ListSection(Type::File, sectionDelegate());
		const auto acceptDownload = [&](const Element &element) {
			if (element.fwRow == 1) {
				return false;
			}
			if (element.ulRow == 1 || element.ulBatch != 0) {
				return false;
			}
			if (element.dlRow == 1 || element.dlBatch != 0) {
				return true;
			}
			if (element.fwRow == 0
				&& _enhancedForward.contains(element.item)
				&& !_uploading.contains(element.item->fullId())
				&& !_uploaded.contains(element.item->fullId())
				&& !_downloading.contains(element.item)) {
				return false;
			}
			return !isUploadItem(element.item);
		};
		appendGroupedDownloads(
			downloads,
			delegate,
			search,
			acceptDownload);
		const auto acceptUpload = [&](const Element &element) {
			if (element.fwRow == 1
				|| element.dlRow == 1
				|| element.dlBatch != 0) {
				return false;
			}
			if (element.ulRow == 1 || element.ulBatch != 0) {
				return true;
			}
			if (element.fwRow == 0
				&& _enhancedForward.contains(element.item)
				&& !_uploading.contains(element.item->fullId())
				&& !_uploaded.contains(element.item->fullId())
				&& !_downloading.contains(element.item)) {
				return false;
			}
			return isUploadItem(element.item);
		};
		appendGroupedUploads(
			uploads,
			delegate,
			search,
			acceptUpload);
		for (const auto &element : _elements) {
			if (search && !element.found) {
				continue;
			}
			if (element.fwRow == 0
				&& _enhancedForward.contains(element.item)
				&& !_uploading.contains(element.item->fullId())
				&& !_uploaded.contains(element.item->fullId())
				&& !_downloading.contains(element.item)) {
				continue;
			}
			if (element.dlRow == 1 || element.dlBatch != 0) {
				continue;
			}
			if (element.ulRow == 1 || element.ulBatch != 0) {
				continue;
			}
			// Ungrouped downloads and uploads were already emitted by the
			// grouped helpers above; only forward rows remain here.
			if (element.fwRow != 1) {
				continue;
			}
			const auto layout = getLayout(element, delegate);
			if (!layout) {
				continue;
			}
			forwards.addItem(layout);
		}
		downloads.finishSection();
		uploads.finishSection();
		forwards.finishSection();
		if (!downloads.empty()) {
			result.push_back(std::move(downloads));
		}
		if (!uploads.empty()) {
			result.push_back(std::move(uploads));
		}
		if (!forwards.empty()) {
			result.push_back(std::move(forwards));
		}
	} else {
		auto section = ListSection(Type::File, sectionDelegate());
		if (_filter == Filter::Forwards
			|| _filter == Filter::Uploads
			|| _filter == Filter::Downloads) {
			// Forwards/Uploads/Downloads keep source-chat order. Forwards use
			// the job's source order (Element::order); Downloads use the time
			// the download started and Uploads the message date - both are
			// the order the files were picked in the source chat and they
			// survive a provider rebuild (entry order alone becomes
			// completion order and is not stable).
			std::vector<const Element*> eligible;
			eligible.reserve(_elements.size());
			for (const auto &element : _elements) {
				if (!search || element.found) {
					eligible.push_back(&element);
				}
			}
			if (_filter == Filter::Forwards) {
				ranges::stable_sort(
					eligible,
					ranges::less(),
					[](const Element *element) { return element->order; });
			} else {
				ranges::stable_sort(
					eligible,
					ranges::less(),
					&Element::started);
			}
			if (_filter == Filter::Downloads) {
				appendGroupedDownloads(
					section,
					delegate,
					search,
					[&](const Element &element) {
						return matches(element);
					});
			} else if (_filter == Filter::Uploads) {
				appendGroupedUploads(
					section,
					delegate,
					search,
					[&](const Element &element) {
						return matches(element);
					});
			} else for (const auto *element : eligible) {
				if (!matches(*element)) {
					continue;
				} else if (auto layout = getLayout(*element, delegate)) {
					section.addItem(layout);
				}
			}
		} else {
			for (auto i = 0; i != int(_elements.size()); ++i) {
				const auto &element = _elements[_elements.size() - 1 - i];
				if (search && !element.found) {
					continue;
				} else if (!matches(element)) {
					continue;
				} else if (auto layout = getLayout(element, delegate)) {
					section.addItem(layout);
				}
			}
		}
		section.finishSection();
		if (!section.empty()) {
			result.push_back(std::move(section));
		}
	}
	return result;
}

void Provider::markLayoutsStale() {
	for (auto &layout : _layouts) {
		layout.second.stale = true;
	}
	for (auto &layout : _fwLayouts) {
		layout.second.stale = true;
	}
	for (auto &layout : _dlLayouts) {
		layout.second.stale = true;
	}
	for (auto &layout : _ulLayouts) {
		layout.second.stale = true;
	}
}

void Provider::clearStaleLayouts() {
	for (auto i = _layouts.begin(); i != _layouts.end();) {
		if (i->second.stale) {
			_layoutRemoved.fire(i->second.item.get());
			i = _layouts.erase(i);
		} else {
			++i;
		}
	}
	for (auto i = _fwLayouts.begin(); i != _fwLayouts.end();) {
		if (i->second.stale) {
			_layoutRemoved.fire(i->second.item.get());
			i = _fwLayouts.erase(i);
		} else {
			++i;
		}
	}
	for (auto i = _dlLayouts.begin(); i != _dlLayouts.end();) {
		if (i->second.stale) {
			_layoutRemoved.fire(i->second.item.get());
			i = _dlLayouts.erase(i);
		} else {
			++i;
		}
	}
	for (auto i = _ulLayouts.begin(); i != _ulLayouts.end();) {
		if (i->second.stale) {
			_layoutRemoved.fire(i->second.item.get());
			i = _ulLayouts.erase(i);
		} else {
			++i;
		}
	}
}

rpl::producer<not_null<BaseLayout*>> Provider::layoutRemoved() {
	return _layoutRemoved.events();
}

BaseLayout *Provider::lookupLayout(const HistoryItem *item) {
	return nullptr;
}

bool Provider::isMyItem(not_null<const HistoryItem*> item) {
	return _downloading.contains(item)
		|| _enhancedForward.contains(item)
		|| _downloaded.contains(item);
}

bool Provider::isForwardAggregate(not_null<const HistoryItem*> item) const {
	return ranges::any_of(_elements, [&](const Element &element) {
		return element.fwRow == 1 && element.item == item;
	});
}

std::optional<PeerId> Provider::forwardAggregateDst(
		not_null<const HistoryItem*> item) const {
	for (const auto &element : _elements) {
		if (element.fwRow == 1 && element.item == item) {
			return element.fwDst;
		}
	}
	return std::nullopt;
}

uint64 Provider::forwardAggregateGroupId(
		not_null<const HistoryItem*> item) const {
	for (const auto &element : _elements) {
		if (element.fwRow == 1 && element.item == item) {
			return element.fwGroup;
		}
	}
	return 0;
}

void Provider::toggleForwardRun(not_null<const HistoryItem*> item) {
	const auto dst = forwardAggregateDst(item);
	if (!dst) {
		return;
	}
	ToggleFwRunPause(&item->history()->session(), *dst);
}

bool Provider::isForwardAggregatePaused(
		not_null<const HistoryItem*> item) const {
	const auto dst = forwardAggregateDst(item);
	if (!dst) {
		return false;
	}
	const auto data = FwAggregateFor(&item->history()->session(), *dst);
	return data.paused && !data.finished;
}

bool Provider::isForwardAggregateFinished(
		not_null<const HistoryItem*> item) const {
	if (forwardAggregateGroupId(item)) {
		return true;
	}
	const auto dst = forwardAggregateDst(item);
	if (!dst) {
		return false;
	}
	return FwAggregateFor(&item->history()->session(), *dst).finished;
}

void Provider::cancelForwardRun(not_null<const HistoryItem*> item) {
	const auto dst = forwardAggregateDst(item);
	if (!dst) {
		return;
	}
	const auto session = &item->history()->session();
	EnhancedForward::cancelForward(*dst, session);
	NormalForward::CancelDst(session, *dst);
}

void Provider::clearForwardRun(not_null<const HistoryItem*> item) {
	const auto session = &item->history()->session();
	if (const auto group = forwardAggregateGroupId(item)) {
		EnhancedForward::DropFinishedFwRunGroup(session, group);
		removeFwFinishedGroup(session, group);
		return;
	}
	const auto dst = forwardAggregateDst(item);
	if (!dst) {
		return;
	}
	ClearFwRun(session, *dst);
}

bool Provider::isDownloadAggregate(
		not_null<const HistoryItem*> item) const {
	return ranges::any_of(_elements, [&](const Element &element) {
		return element.dlRow == 1 && element.item == item;
	});
}

uint64 Provider::downloadAggregateBatchId(
		not_null<const HistoryItem*> item) const {
	for (const auto &element : _elements) {
		if (element.dlRow == 1 && element.item == item) {
			return element.dlBatch;
		}
	}
	return 0;
}

bool Provider::isDownloadAggregateFinished(
		not_null<const HistoryItem*> item) const {
	const auto batchId = downloadAggregateBatchId(item);
	if (!batchId) {
		return true;
	}
	for (const auto &run : Core::App().downloadManager().dlRuns()) {
		if (run.batchId == batchId) {
			return run.finished;
		}
	}
	return true;
}

void Provider::clearDownloadRun(uint64 batchId) {
	if (!batchId) {
		return;
	}
	auto &manager = Core::App().downloadManager();
	auto members = std::vector<not_null<HistoryItem*>>();
	for (const auto &element : _elements) {
		if (element.dlRow == 0 && element.dlBatch == batchId) {
			members.push_back(element.item);
		}
	}
	// clearFinishedItem()/cancel() synchronously re-enter remove() through
	// loadedRemoved/loadingListChanges, so the elements are collected first.
	for (const auto item : members) {
		if (_downloading.contains(item)) {
			manager.cancel(item);
		} else {
			manager.clearFinishedItem(item);
		}
	}
	manager.dropDlBatch(batchId);
	removeDlHeader(batchId);
	refreshPostponed(false);
}

void Provider::clearTransferRow(not_null<const HistoryItem*> item) {
	auto &manager = Core::App().downloadManager();
	manager.removeLoading(item);
	manager.clearFinishedItem(item);
	_controller->session().uploader().removeFinishedUpload(item->fullId());
	_downloading.remove(item);
	_downloaded.remove(item);
	_uploading.remove(item->fullId());
	_uploaded.remove(item->fullId());
	remove(item);
}

bool Provider::isAfter(
		not_null<const HistoryItem*> a,
		not_null<const HistoryItem*> b) {
	if (a != b) {
		for (const auto &element : _elements) {
			if (element.item == a) {
				return false;
			} else if (element.item == b) {
				return true;
			}
		}
	}
	return false;
}

bool Provider::searchMode() const {
	return !_queryWords.empty();
}

void Provider::fillSearchIndex(Element &element) {
	if (element.ulRow == 1) {
		element.words = TextUtilities::PrepareSearchWords(
			element.item->history()->peer->name());
		element.letters.clear();
		for (const auto &word : element.words) {
			element.letters.emplace(word.front());
		}
		return;
	}
	if (element.dlRow == 1) {
		auto strings = QStringList();
		for (const auto &run
			: Core::App().downloadManager().dlRuns()) {
			if (run.batchId == element.dlBatch) {
				strings.append(run.srcName);
				strings.append(run.destDir);
				break;
			}
		}
		element.words = TextUtilities::PrepareSearchWords(
			strings.join(' '));
		element.letters.clear();
		for (const auto &word : element.words) {
			element.letters.emplace(word.front());
		}
		return;
	}
	if (element.fwRow == 1) {
		const auto session = &element.item->history()->session();
		element.words = TextUtilities::PrepareSearchWords(
			session->data().peer(element.fwDst)->name());
		element.letters.clear();
		for (const auto &word : element.words) {
			element.letters.emplace(word.front());
		}
		return;
	}
	auto strings = QStringList(QFileInfo(element.path).fileName());
	if (const auto media = element.item->media()) {
		if (const auto document = media->document()) {
			strings.append(document->filename());
			strings.append(Ui::Text::FormatDownloadsName(document).text);
		}
	}
	element.words = TextUtilities::PrepareSearchWords(strings.join(' '));
	element.letters.clear();
	for (const auto &word : element.words) {
		element.letters.emplace(word.front());
	}
}

bool Provider::computeIsFound(const Element &element) const {
	Expects(!_queryWords.empty());

	const auto has = [&](const QString &queryWord) {
		if (!element.letters.contains(queryWord.front())) {
			return false;
		}
		for (const auto &word : element.words) {
			if (word.startsWith(queryWord)) {
				return true;
			}
		}
		return false;
	};
	for (const auto &queryWord : _queryWords) {
		if (!has(queryWord)) {
			return false;
		}
	}
	return true;
}

void Provider::itemRemoved(not_null<const HistoryItem*> item) {
	remove(item);
}

BaseLayout *Provider::getLayout(
		Element element,
		not_null<Overview::Layout::Delegate*> delegate) {
	if (element.dlRow == 1) {
		const auto key = std::make_tuple(
			&element.item->history()->session(),
			element.dlBatch);
		auto it = _dlLayouts.find(key);
		if (it == _dlLayouts.end()) {
			if (auto layout = createLayout(element, delegate)) {
				layout->initDimensions();
				it = _dlLayouts.emplace(key, std::move(layout)).first;
			} else {
				return nullptr;
			}
		}
		it->second.stale = false;
		return it->second.item.get();
	}
	if (element.ulRow == 1) {
		const auto key = std::make_tuple(
			&element.item->history()->session(),
			element.ulBatch);
		auto it = _ulLayouts.find(key);
		if (it == _ulLayouts.end()) {
			if (auto layout = createLayout(element, delegate)) {
				layout->initDimensions();
				it = _ulLayouts.emplace(key, std::move(layout)).first;
			} else {
				return nullptr;
			}
		}
		it->second.stale = false;
		return it->second.item.get();
	}
	if (element.fwRow == 1) {
		const auto key = std::make_tuple(
			&element.item->history()->session(),
			element.fwDst,
			element.fwGroup);
		auto it = _fwLayouts.find(key);
		if (it == _fwLayouts.end()) {
			if (auto layout = createLayout(element, delegate)) {
				layout->initDimensions();
				it = _fwLayouts.emplace(key, std::move(layout)).first;
			} else {
				return nullptr;
			}
		}
		it->second.stale = false;
		return it->second.item.get();
	}
	auto it = _layouts.find(element.item);
	if (it == _layouts.end()) {
		if (auto layout = createLayout(element, delegate)) {
			layout->initDimensions();
			it = _layouts.emplace(element.item, std::move(layout)).first;
		} else {
			return nullptr;
		}
	}
	it->second.stale = false;
	return it->second.item.get();
}

std::unique_ptr<BaseLayout> Provider::createLayout(
		Element element,
		not_null<Overview::Layout::Delegate*> delegate) {
	if (element.dlRow == 1) {
		const auto session = &element.item->history()->session();
		const auto batchId = element.dlBatch;
		auto fields = Overview::Layout::ForwardSummaryFields{
			.snapshot = [=] {
				for (const auto &run
					: Core::App().downloadManager().dlRuns()) {
					if (run.batchId == batchId
						&& run.sessionId == session->uniqueId()) {
						return DlRunData(run);
					}
				}
				return Overview::Layout::ForwardSummaryData();
			},
			.action = std::make_shared<LambdaClickHandler>(
				crl::guard(this, [=] {
				})),
			.noThumb = true,
		};
		return std::make_unique<Overview::Layout::ForwardSummary>(
			delegate,
			element.item,
			std::move(fields),
			st::overviewFileLayout);
	}
	if (element.ulRow == 1) {
		const auto session = &element.item->history()->session();
		const auto batchId = element.ulBatch;
		const auto carrier = element.item.get();
		auto fields = Overview::Layout::ForwardSummaryFields{
			.snapshot = [=] {
				for (const auto &run
					: Core::App().downloadManager().ulRuns()) {
					if (run.batchId == batchId
						&& run.sessionId == session->uniqueId()) {
						return UlRunData(
							run,
							carrier->history()->peer->name());
					}
				}
				return Overview::Layout::ForwardSummaryData();
			},
			.action = std::make_shared<LambdaClickHandler>(
				crl::guard(this, [=] {
				})),
			.noThumb = true,
		};
		return std::make_unique<Overview::Layout::ForwardSummary>(
			delegate,
			element.item,
			std::move(fields),
			st::overviewFileLayout);
	}
	if (element.fwRow == 1) {
		const auto session = &element.item->history()->session();
		const auto dst = element.fwDst;
		if (element.fwGroup) {
			auto runs = std::vector<EnhancedForward::FinishedFwRun>();
			for (const auto &record
				: EnhancedForward::FinishedFwRuns(session)) {
				if (record.actionId == element.fwGroup) {
					runs.push_back(record);
				}
			}
			if (!runs.empty()) {
				const auto groupId = element.fwGroup;
				auto fields = Overview::Layout::ForwardSummaryFields{
					.snapshot = [=] {
						auto current
							= std::vector<EnhancedForward::FinishedFwRun>();
						for (const auto &record
							: EnhancedForward::FinishedFwRuns(session)) {
							if (record.actionId == groupId) {
								current.push_back(record);
							}
						}
						return FwFinishedGroupData(session, current);
					},
					.action = std::make_shared<LambdaClickHandler>(
						crl::guard(this, [=] {
						})),
				};
				return std::make_unique<Overview::Layout::ForwardSummary>(
					delegate,
					element.item,
					std::move(fields),
					st::overviewFileLayout);
			}
		}
		auto fields = Overview::Layout::ForwardSummaryFields{
			.snapshot = [=] {
				return FwAggregateFor(session, dst);
			},
			.action = std::make_shared<LambdaClickHandler>(
				crl::guard(this, [=] {
					const auto data = FwAggregateFor(session, dst);
					if (data.finished) {
						ClearFwRun(session, dst);
					} else {
						ToggleFwRunPause(session, dst);
					}
				})),
		};
		return std::make_unique<Overview::Layout::ForwardSummary>(
			delegate,
			element.item,
			std::move(fields),
			st::overviewFileLayout);
	}
	const auto getFile = [&]() -> DocumentData* {
		if (auto media = element.item->media()) {
			return media->document();
		}
		return nullptr;
	};

	using namespace Overview::Layout;
	const auto media = element.item->media();
	if (const auto photo = media ? media->photo() : nullptr) {
		return std::make_unique<Photo>(
			delegate,
			element.item,
			photo,
			MediaOptions{ .spoiler = media && media->hasSpoiler() });
	}
	const auto &songSt = st::overviewFileLayout;
	if (const auto file = getFile()) {
		const auto srcId = element.item->fullId();
		const auto session = &_controller->session();
		auto fields = DocumentFields{
			.document = file,
			.dateOverride = Data::DateFromDownloadDate(element.started),
			.forceFileLayout = true,
			.forceCancelCheck = [this, id = srcId] {
				if (_efPending.contains(id)
					|| _efTransferring.contains(id)) {
					return true;
				}
				const auto item = _controller->session().data().message(id);
				if (!item) {
					return false;
				}
				const auto media = item->media();
				const auto doc = media ? media->document() : nullptr;
				return doc && (doc->uploadingData != nullptr);
			},
			.savedProgress = [session, srcId](qint64 *r, qint64 *t) {
				const auto bytes = EnhancedForward::persistedItemBytes(
					session,
					srcId);
				const auto total = EnhancedForward::persistedItemFileSize(
					session,
					srcId);
				if (!bytes || !total || *total <= 0) {
					return false;
				}
				*r = std::min(*bytes, *total);
				*t = *total;
				return true;
			},
		};
		const auto item = element.item;
		auto &manager = Core::App().downloadManager();
		if (manager.loadingExternalState(item).has_value()) {
			fields.externalLoading = [item]()
			-> std::optional<DocumentExternalLoading> {
				auto &manager = Core::App().downloadManager();
				const auto state = manager.loadingExternalState(item);
				if (!state || state->done) {
					return std::nullopt;
				}
				return DocumentExternalLoading{
					.ready = state->ready,
					.total = state->total,
				};
			};
			fields.externalCancel = [item] {
				Core::App().downloadManager().cancelLoadingExternal(item);
			};
		}
		if (const auto dst = FwCurrentDst(
			&element.item->history()->session(),
			srcId)) {
			const auto itemSession = &element.item->history()->session();
			fields.cancellOverride = std::make_shared<LambdaClickHandler>(
				crl::guard(this, [=] {
					ToggleFwRunPause(itemSession, *dst);
				}));
			fields.forceCancelCheck = [] {
				return true;
			};
		}
		return std::make_unique<Document>(
			delegate,
			element.item,
			std::move(fields),
			songSt);
	}
	return nullptr;
}

ListItemSelectionData Provider::computeSelectionData(
		not_null<const HistoryItem*> item,
		TextSelection selection) {
	auto result = ListItemSelectionData(selection);
	const auto ef = isEnhancedForward(item);
	result.canDelete = !(ef && isEnhancedForwardFinished(item));
	result.canForward = item->allowsForward()
		&& (&item->history()->session() == &_controller->session());
	return result;
}

void Provider::applyDragSelection(
		ListSelectedMap &selected,
		not_null<const HistoryItem*> fromItem,
		bool skipFrom,
		not_null<const HistoryItem*> tillItem,
		bool skipTill) {
	auto from = ranges::find(_elements, fromItem, &Element::item);
	auto till = ranges::find(_elements, tillItem, &Element::item);
	if (from == end(_elements) || till == end(_elements)) {
		return;
	}
	if (skipFrom) {
		++from;
	}
	if (!skipTill) {
		++till;
	}
	if (from >= till) {
		selected.clear();
		return;
	}
	const auto search = !_queryWords.isEmpty();
	const auto selectLimit = _storiesAddToAlbumId
		? _controller->session().appConfig().storiesAlbumLimit()
		: MaxSelectedItems;
	auto chosen = base::flat_set<not_null<const HistoryItem*>>();
	chosen.reserve(till - from);
	for (auto i = from; i != till; ++i) {
		if (search && !i->found) {
			continue;
		}
		const auto item = i->item;
		chosen.emplace(item);
		ChangeItemSelection(
			selected,
			item,
			computeSelectionData(item, FullSelection),
			selectLimit);
	}
	if (selected.size() != chosen.size()) {
		for (auto i = begin(selected); i != end(selected);) {
			if (selected.contains(i->first)) {
				++i;
			} else {
				i = selected.erase(i);
			}
		}
	}
}

bool Provider::allowSaveFileAs(
		not_null<const HistoryItem*> item,
		not_null<DocumentData*> document) {
	return false;
}

bool Provider::isUploadItem(not_null<const HistoryItem*> item) const {
	return _uploading.contains(item->fullId())
		|| _uploaded.contains(item->fullId());
}

bool Provider::isEnhancedForward(not_null<const HistoryItem*> item) const {
	return _enhancedForward.contains(item);
}

bool Provider::isDownloading(not_null<const HistoryItem*> item) const {
	return _downloading.contains(item);
}

bool Provider::isDownloaded(not_null<const HistoryItem*> item) const {
	return _downloaded.contains(item);
}

bool Provider::isUploading(not_null<const HistoryItem*> item) const {
	return _uploading.contains(item->fullId());
}

bool Provider::isUploaded(not_null<const HistoryItem*> item) const {
	return _uploaded.contains(item->fullId());
}

bool Provider::isEnhancedForwardFinished(
		not_null<const HistoryItem*> item) const {
	if (!_enhancedForward.contains(item)) {
		return false;
	}
	const auto itemId = item->globalId().itemId;
	for (const auto &job : EnhancedForward::AllJobs(&_controller->session())) {
		for (const auto &srcId : job.progress.sourceIds) {
			if (srcId.peer == itemId.peer && srcId.msg == itemId.msg) {
				return job.finished;
			}
		}
	}
	return false;
}

QString Provider::showInFolderPath(
		not_null<const HistoryItem*> item,
		not_null<DocumentData*> document) {
	const auto i = ranges::find(_elements, item, &Element::item);
	return (i != end(_elements)) ? i->path : QString();
}

int64 Provider::scrollTopStatePosition(not_null<HistoryItem*> item) {
	const auto i = ranges::find(_elements, item, &Element::item);
	return (i != end(_elements)) ? i->started : 0;
}

HistoryItem *Provider::scrollTopStateItem(ListScrollTopState state) {
	if (!state.position) {
		return _elements.empty() ? nullptr : _elements.back().item.get();
	}
	const auto i = ranges::lower_bound(
		_elements,
		state.position,
		ranges::less(),
		&Element::started);
	return (i != end(_elements))
		? i->item.get()
		: _elements.empty()
		? nullptr
		: _elements.back().item.get();
}

void Provider::saveState(
		not_null<Media::Memento*> memento,
		ListScrollTopState scrollState) {
	if (!_elements.empty() && scrollState.item) {
		memento->setAroundId({ PeerId(), 1 });
		memento->setScrollTopItem(scrollState.item->globalId());
		memento->setScrollTopItemPosition(scrollState.position);
		memento->setScrollTopShift(scrollState.shift);
	}
}

void Provider::restoreState(
		not_null<Media::Memento*> memento,
		Fn<void(ListScrollTopState)> restoreScrollState) {
	if (memento->aroundId() == FullMsgId(PeerId(), 1)) {
		restoreScrollState({
			.position = memento->scrollTopItemPosition(),
			.item = MessageByGlobalId(memento->scrollTopItem()),
			.shift = memento->scrollTopShift(),
		});
		refreshViewer();
	}
}

} // namespace Info::Downloads
