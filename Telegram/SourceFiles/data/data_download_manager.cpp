/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_download_manager.h"

#include "base/weak_ptr.h"
#include "data/data_file_hash.h"
#include "logs.h"
#include "settings.h"
#include "core/core_settings.h"
#include "core/enhanced_settings.h"
#include "enhanced_forward.h"
#include "storage/localimageloader.h"
#include "data/data_session.h"
#include "data/data_photo.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_click_handler.h"
#include "data/data_peer.h"
#include "data/data_web_page.h"
#include "data/data_changes.h"
#include "data/data_user.h"
#include "data/data_channel.h"
#include "data/data_file_origin.h"
#include "storage/file_upload.h"
#include "base/unixtime.h"
#include "base/random.h"
#include "main/main_session.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "lang/lang_keys.h"
#include "storage/storage_account.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "iv/iv_rich_page.h"

#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_helpers.h"
#include "core/application.h"
#include "core/mime_type.h"
#include "platform/platform_file_utilities.h"
#include "ui/controls/download_bar.h"
#include "info/downloads/info_downloads_widget.h"
#include "info/info_memento.h"
#include "ui/text/format_song_document_name.h"
#include "ui/layers/generic_box.h"
#include "ui/ui_utility.h"
#include "storage/serialize_common.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "apiwrap.h"
#include "ui/boxes/confirm_box.h"
#include "ui/toast/toast.h"
#include "styles/style_layers.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace Data {
namespace {
int g_copyAlbumDone = 0;
int g_copyAlbumTotal = 0;
rpl::event_stream<> g_copyAlbumChanges;
} // namespace

void SetCopyAlbumProgress(int done, int total) {
	g_copyAlbumDone = done;
	g_copyAlbumTotal = total;
	g_copyAlbumChanges.fire({});
}
std::pair<int,int> CopyAlbumProgress() {
	return { g_copyAlbumDone, g_copyAlbumTotal };
}

void FilterCopyAlbumDuplicates(
		not_null<Main::Session*> session,
		std::vector<not_null<HistoryItem*>> items,
		Fn<void(std::vector<not_null<HistoryItem*>>)> done) {
	const auto dedupOff = !GetEnhancedBool("prevent_forward_duplicates");
	auto &db = Core::App().downloadManager().ensureDedupDb();
	QSet<uint64> seenIds;
	auto filteredIds = std::vector<not_null<HistoryItem*>>();
	filteredIds.reserve(items.size());
	int skippedIds = 0;
	for (const auto item : items) {
		const auto media = item->media();
		const auto doc = media ? media->document() : nullptr;
		const auto photo = media ? media->photo() : nullptr;
		uint64 mediaId = 0;
		if (doc) mediaId = uint64(doc->id);
		else if (photo) mediaId = uint64(photo->id);
		else { filteredIds.push_back(item); continue; }
		if (seenIds.contains(mediaId)
			|| (!dedupOff
				&& db.containsDocId(Data::DedupDb::Table::Uploads, mediaId))) {
			skippedIds++;
			continue;
		}
		seenIds.insert(mediaId);
		filteredIds.push_back(item);
	}
	QSet<QByteArray> seenHashes;
	auto finalFiltered = std::vector<not_null<HistoryItem*>>();
	finalFiltered.reserve(filteredIds.size());
	auto hashCandidates = std::vector<not_null<HistoryItem*>>();
	for (const auto item : filteredIds) {
		if (const auto doc = item->media()->document()) {
			if (doc->size > 0) { hashCandidates.push_back(item); continue; }
		} else if (item->media()->photo()) {
			hashCandidates.push_back(item); continue;
		}
		finalFiltered.push_back(item);
	}
	if (hashCandidates.empty()) {
		done(std::move(finalFiltered));
		return;
	}
	auto remaining = std::make_shared<int>(int(hashCandidates.size()));
	auto hashes = std::make_shared<QMap<MsgId, QByteArray>>();
	auto seenHashesPtr = std::make_shared<QSet<QByteArray>>(std::move(seenHashes));
	auto finalFilteredPtr = std::make_shared<std::vector<not_null<HistoryItem*>>>(std::move(finalFiltered));
	auto skippedHashesPtr = std::make_shared<int>(0);
	auto allDone = std::make_shared<Fn<void()>>();
	*allDone = [=]() mutable {
		if (--(*remaining) > 0) return;
		auto &db2 = Core::App().downloadManager().ensureDedupDb();
		for (const auto item : hashCandidates) {
			const auto h = hashes->value(item->id);
			bool isDup = !h.isEmpty() && (seenHashesPtr->contains(h)
				|| (!dedupOff
					&& db2.containsFinishedHash(
						Data::DedupDb::Table::Uploads,
						h)));
			if (isDup) {
				(*skippedHashesPtr)++;
				if (!dedupOff
					&& !h.isEmpty()
					&& !db2.containsFinishedHash(
						Data::DedupDb::Table::Uploads,
						h)) {
					uint64 mid = 0;
					if (const auto doc = item->media()->document()) mid = uint64(doc->id);
					else if (const auto photo = item->media()->photo()) mid = uint64(photo->id);
					db2.insert(Data::DedupDb::Table::Uploads, { .hash = h, .documentId = mid, .status = u"f"_q });
				}
				continue;
			}
			if (!h.isEmpty()) seenHashesPtr->insert(h);
			finalFilteredPtr->push_back(item);
		}
		done(std::move(*finalFilteredPtr));
	};
	for (const auto item : hashCandidates) {
		if (const auto doc = item->media()->document()) {
			Core::App().downloadManager().fetchFingerprint(session, doc, [=](QByteArray h) {
				(*hashes)[item->id] = h;
				(*allDone)();
			});
		} else if (const auto photo = item->media()->photo()) {
			Core::App().downloadManager().fetchFingerprint(session, photo, [=](QByteArray h) {
				(*hashes)[item->id] = h;
				(*allDone)();
			});
		} else {
			(*hashes)[item->id] = QByteArray();
			(*allDone)();
		}
	}
}

namespace {

constexpr auto kClearLoadingTimeout = 5 * crl::time(1000);
constexpr auto kMaxFileSize = 4000 * int64(1024 * 1024);
constexpr auto kMaxResolvePerAttempt = 100;


constexpr auto ByItem = [](const auto &entry) {
	if constexpr (std::is_same_v<decltype(entry), const DownloadingId&>) {
		return entry.object.item;
	} else {
		const auto resolved = entry.object.get();
		return resolved ? resolved->item.get() : nullptr;
	}
};

constexpr auto ByDocument = [](const auto &entry) {
	return entry.object.document;
};

[[nodiscard]] uint64 PeerAccessHash(not_null<PeerData*> peer) {
	if (const auto user = peer->asUser()) {
		return user->accessHash();
	} else if (const auto channel = peer->asChannel()) {
		return channel->accessHash();
	}
	return 0;
}

[[nodiscard]] bool ItemContainsMedia(const DownloadObject &object) {
	if (const auto iv = object.item->Get<HistoryMessageMediaForInstantView>()) {
		if (object.document && iv->documents.contains(object.document)) {
			return true;
		} else if (object.photo && iv->photos.contains(object.photo)) {
			return true;
		}
	}
	if (const auto photo = object.photo) {
		if (const auto media = object.item->media()) {
			if (const auto page = media->webpage()) {
				if (page->photo == photo) {
					return true;
				}
				for (const auto &item : page->collage.items) {
					if (const auto v = std::get_if<PhotoData*>(&item)) {
						if ((*v) == photo) {
							return true;
						}
					}
				}
			} else {
				return (media->photo() == photo);
			}
		}
	} else if (const auto document = object.document) {
		if (const auto media = object.item->media()) {
			if (const auto page = media->webpage()) {
				if (page->document == document) {
					return true;
				}
				for (const auto &item : page->collage.items) {
					if (const auto v = std::get_if<DocumentData*>(&item)) {
						if ((*v) == document) {
							return true;
						}
					}
				}
			} else {
				return (media->document() == document);
			}
		}
	}
	return false;
}

struct DocumentDescriptor {
	uint64 sessionUniqueId = 0;
	DocumentId documentId = 0;
	FullMsgId itemId;
};

[[nodiscard]] QString RichExportFolderToRemove(const QString &filePath) {
	if (!filePath.endsWith(u".html"_q, Qt::CaseInsensitive)) {
		return QString();
	}
	const auto info = QFileInfo(filePath);
	const auto folder = info.absolutePath();
	if (QDir(folder).dirName() != info.completeBaseName()) {
		return QString();
	}
	auto file = QFile(filePath);
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	const auto head = QString::fromUtf8(file.read(2048));
	if (!head.contains(Iv::RichExportGeneratorMarker())) {
		return QString();
	}
	const auto entries = QDir(folder).entryList(
		QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
	for (const auto &entry : entries) {
		if ((entry != info.fileName()) && (entry != u"media"_q)) {
			return QString();
		}
	}
	return folder;
}

} // namespace

struct DownloadManager::DeleteFilesDescriptor {
	base::flat_set<not_null<Main::Session*>> sessions;
	base::flat_map<QString, DocumentDescriptor> files;
};

DownloadManager::DownloadManager()
: _clearLoadingTimer([=] { clearLoading(); })
, _duplicatesToastTimer([=] { notifyDuplicateSkips(); })
, _holdExpiryTimer([=] { _loadingListChanges.fire({}); }) {
}

DownloadManager::~DownloadManager() {
	_sessions.clear();
}

void DownloadManager::reportDuplicateSkipped(
		DedupDb::Table table,
		int count) {
	const auto type = int(table);
	if (type < 0 || type >= 2 || count <= 0) {
		return;
	}
	_duplicatesSkipped[type] += count;
	_duplicatesToastTimer.callOnce(600);
}

void DownloadManager::addBatch(std::weak_ptr<DownloadBatch> batch) {
	_batches.push_back(std::move(batch));
	_heldDlBatches.clear();
	_loadingListChanges.fire({});
}

void DownloadManager::holdFinishedDlBatch(
		std::shared_ptr<DownloadBatch> batch) {
	if (!batch || !batch->done) {
		return;
	}
	_heldDlBatches.push_back({
		batch->total,
		batch->downloaded + batch->failed,
		batch->duplicates,
		crl::now() + crl::time(2000),
	});
	_holdExpiryTimer.callOnce(crl::time(2000));
	_loadingListChanges.fire({});
}

void DownloadManager::dropHeldDlBatches() {
	if (_heldDlBatches.empty()) {
		return;
	}
	_heldDlBatches.clear();
	_loadingListChanges.fire({});
}

bool DownloadManager::hasHeldDlBatches() const {
	return !_heldDlBatches.empty();
}

void DownloadManager::pokeDownloadBar() {
	_loadingListChanges.fire({});
}

std::tuple<int, int, int> DownloadManager::batchTotals() {
	auto total = 0;
	auto done = 0;
	auto skipped = 0;
	const auto now = crl::now();
	for (auto i = 0; i != int(_heldDlBatches.size());) {
		if (_heldDlBatches[i].until <= now) {
			_heldDlBatches.erase(_heldDlBatches.begin() + i);
		} else {
			const auto &held = _heldDlBatches[i];
			total += held.total;
			done += held.done;
			skipped += held.skipped;
			++i;
		}
	}
	for (auto i = 0; i != int(_batches.size());) {
		const auto batch = _batches[i].lock();
		if (!batch || batch->done) {
			_batches.erase(_batches.begin() + i);
		} else if (batch->id
			&& ranges::contains(_droppedDlBatches, batch->id)) {
			_batches.erase(_batches.begin() + i);
		} else {
			total += batch->total;
			done += batch->downloaded + batch->failed;
			skipped += batch->duplicates;
			++i;
		}
	}
	return { total, done, skipped };
}

void DownloadManager::noteDlBatchFinished(
		std::shared_ptr<DownloadBatch> batch) {
	if (!batch
		|| !batch->done
		|| batch->total <= 1
		|| !batch->id) {
		return;
	}
	const auto dropped = ranges::contains(_droppedDlBatches, batch->id);
	_droppedDlBatches.erase(
		ranges::remove(_droppedDlBatches, batch->id),
		end(_droppedDlBatches));
	if (dropped) {
		return;
	}
	for (const auto &run : _finishedDlRuns) {
		if (run.batchId == batch->id) {
			return;
		}
	}
	if (!batch->finishedAt) {
		batch->finishedAt = base::unixtime::now();
	}
	_finishedDlRuns.push_back({
		.batchId = batch->id,
		.sessionId = batch->sessionId,
		.srcName = batch->srcName,
		.destDir = batch->destDir,
		.total = batch->total,
		.done = batch->downloaded + batch->failed,
		.skipped = batch->duplicates,
		.startedAt = batch->startedAt,
		.finishedAt = batch->finishedAt,
	});
	while (_finishedDlRuns.size() > 200) {
		_finishedDlRuns.erase(_finishedDlRuns.begin());
	}
	for (const auto &account : Core::App().domain().orderedAccounts()) {
		if (const auto session = account->maybeSession()) {
			if (session->uniqueId() == batch->sessionId) {
				writePostponed(not_null<Main::Session*>(session));
				break;
			}
		}
	}
	_loadingListChanges.fire({});
}

void DownloadManager::dropDlBatch(uint64 batchId) {
	if (!batchId) {
		return;
	}
	if (!ranges::contains(_droppedDlBatches, batchId)) {
		_droppedDlBatches.push_back(batchId);
		while (_droppedDlBatches.size() > 512) {
			_droppedDlBatches.erase(_droppedDlBatches.begin());
		}
	}
	_batches.erase(
		ranges::remove_if(
			_batches,
			[&](const std::weak_ptr<DownloadBatch> &weak) {
				const auto batch = weak.lock();
				return !batch || batch->id == batchId;
			}),
		end(_batches));
	dropDlRun(batchId);
	_loadingListChanges.fire({});
}

void DownloadManager::dropFinishedDlRuns() {
	if (_finishedDlRuns.empty()) {
		return;
	}
	_finishedDlRuns.clear();
	_loadingListChanges.fire({});
}

void DownloadManager::dropDlRun(uint64 batchId) {
	if (!batchId) {
		return;
	}
	auto sessionId = uint64(0);
	_finishedDlRuns.erase(
		ranges::remove_if(
			_finishedDlRuns,
			[&](const FinishedDlRun &run) {
				if (run.batchId != batchId) {
					return false;
				}
				sessionId = run.sessionId;
				return true;
			}),
		end(_finishedDlRuns));
	if (sessionId) {
		for (const auto &account
			: Core::App().domain().orderedAccounts()) {
			if (const auto session = account->maybeSession()) {
				if (session->uniqueId() == sessionId) {
					writePostponed(not_null<Main::Session*>(session));
					break;
				}
			}
		}
	}
	_loadingListChanges.fire({});
}

std::vector<DownloadManager::DlRunSnapshot> DownloadManager::dlRuns() {
	auto result = std::vector<DlRunSnapshot>();
	for (auto i = 0; i != int(_batches.size());) {
		const auto batch = _batches[i].lock();
		if (!batch || batch->done) {
			_batches.erase(_batches.begin() + i);
		} else if (batch->total <= 1
			|| !batch->id
			|| ranges::contains(_droppedDlBatches, batch->id)) {
			++i;
		} else {
			result.push_back({
				.batchId = batch->id,
				.sessionId = batch->sessionId,
				.srcName = batch->srcName,
				.destDir = batch->destDir,
				.total = batch->total,
				.done = batch->downloaded + batch->failed,
				.skipped = batch->duplicates,
				.startedAt = batch->startedAt,
				.finished = false,
			});
			++i;
		}
	}
	for (const auto &run : _finishedDlRuns) {
		result.push_back({
			.batchId = run.batchId,
			.sessionId = run.sessionId,
			.srcName = run.srcName,
			.destDir = run.destDir,
			.total = run.total,
			.done = run.done,
			.skipped = run.skipped,
			.startedAt = run.startedAt,
			.finishedAt = run.finishedAt,
			.finished = true,
		});
	}
	ranges::stable_sort(
		result,
		ranges::less(),
		[](const DlRunSnapshot &run) { return run.startedAt; });
	return result;
}

void DownloadManager::addUploadBatch(std::weak_ptr<UploadBatch> batch) {
	_uploadBatches.push_back(std::move(batch));
	_heldUlBatches.clear();
	_loadingListChanges.fire({});
}

void DownloadManager::holdFinishedUlBatch(
		std::shared_ptr<UploadBatch> batch) {
	if (!batch || !batch->done) {
		return;
	}
	_heldUlBatches.push_back({
		batch->total,
		batch->uploaded + batch->failed,
		batch->duplicates,
		crl::now() + crl::time(2000),
	});
	_holdExpiryTimer.callOnce(crl::time(2000));
	_loadingListChanges.fire({});
}

void DownloadManager::dropHeldUlBatches() {
	if (_heldUlBatches.empty()) {
		return;
	}
	_heldUlBatches.clear();
	_loadingListChanges.fire({});
}

bool DownloadManager::hasHeldUlBatches() const {
	return !_heldUlBatches.empty();
}

std::tuple<int, int, int> DownloadManager::uploadBatchTotals() {
	auto total = 0;
	auto done = 0;
	auto skipped = 0;
	const auto now = crl::now();
	for (auto i = 0; i != int(_heldUlBatches.size());) {
		if (_heldUlBatches[i].until <= now) {
			_heldUlBatches.erase(_heldUlBatches.begin() + i);
		} else {
			const auto &held = _heldUlBatches[i];
			total += held.total;
			done += held.done;
			skipped += held.skipped;
			++i;
		}
	}
	for (auto i = 0; i != int(_uploadBatches.size());) {
		const auto batch = _uploadBatches[i].lock();
		if (!batch || batch->done) {
			_uploadBatches.erase(_uploadBatches.begin() + i);
		} else if (batch->id
			&& ranges::contains(_droppedUlBatches, batch->id)) {
			_uploadBatches.erase(_uploadBatches.begin() + i);
		} else {
			total += batch->total;
			done += batch->uploaded + batch->failed;
			skipped += batch->duplicates;
			++i;
		}
	}
	return { total, done, skipped };
}

void DownloadManager::noteUlBatchFinished(
		std::shared_ptr<UploadBatch> batch) {
	if (!batch
		|| !batch->done
		|| batch->total <= 1
		|| !batch->id) {
		return;
	}
	const auto dropped = ranges::contains(_droppedUlBatches, batch->id);
	_droppedUlBatches.erase(
		ranges::remove(_droppedUlBatches, batch->id),
		end(_droppedUlBatches));
	if (dropped) {
		return;
	}
	for (const auto &run : _finishedUlRuns) {
		if (run.batchId == batch->id) {
			return;
		}
	}
	if (!batch->finishedAt) {
		batch->finishedAt = base::unixtime::now();
	}
	_finishedUlRuns.push_back({
		.batchId = batch->id,
		.sessionId = batch->sessionId,
		.total = batch->total,
		.done = batch->uploaded + batch->failed,
		.skipped = batch->duplicates,
		.startedAt = batch->startedAt,
		.finishedAt = batch->finishedAt,
	});
	while (_finishedUlRuns.size() > 200) {
		_finishedUlRuns.erase(_finishedUlRuns.begin());
	}
	for (const auto &account : Core::App().domain().orderedAccounts()) {
		if (const auto session = account->maybeSession()) {
			if (session->uniqueId() == batch->sessionId) {
				writePostponed(not_null<Main::Session*>(session));
				break;
			}
		}
	}
	_loadingListChanges.fire({});
}

void DownloadManager::dropUlBatch(uint64 batchId) {
	if (!batchId) {
		return;
	}
	if (!ranges::contains(_droppedUlBatches, batchId)) {
		_droppedUlBatches.push_back(batchId);
		while (_droppedUlBatches.size() > 512) {
			_droppedUlBatches.erase(_droppedUlBatches.begin());
		}
	}
	_uploadBatches.erase(
		ranges::remove_if(
			_uploadBatches,
			[&](const std::weak_ptr<UploadBatch> &weak) {
				const auto batch = weak.lock();
				return !batch || batch->id == batchId;
			}),
		end(_uploadBatches));
	dropUlRun(batchId);
	_loadingListChanges.fire({});
}

void DownloadManager::dropFinishedUlRuns() {
	if (_finishedUlRuns.empty()) {
		return;
	}
	_finishedUlRuns.clear();
	for (auto &[session, data] : _sessions) {
		writePostponed(session);
	}
	_loadingListChanges.fire({});
}

void DownloadManager::dropUlRun(uint64 batchId) {
	if (!batchId) {
		return;
	}
	auto sessionId = uint64(0);
	_finishedUlRuns.erase(
		ranges::remove_if(
			_finishedUlRuns,
			[&](const FinishedUlRun &run) {
				if (run.batchId != batchId) {
					return false;
				}
				sessionId = run.sessionId;
				return true;
			}),
		end(_finishedUlRuns));
	if (sessionId) {
		for (const auto &account
			: Core::App().domain().orderedAccounts()) {
			if (const auto session = account->maybeSession()) {
				if (session->uniqueId() == sessionId) {
					writePostponed(not_null<Main::Session*>(session));
					break;
				}
			}
		}
	}
	_loadingListChanges.fire({});
}

std::vector<DownloadManager::UlRunSnapshot> DownloadManager::ulRuns() {
	auto result = std::vector<UlRunSnapshot>();
	for (auto i = 0; i != int(_uploadBatches.size());) {
		const auto batch = _uploadBatches[i].lock();
		if (!batch || batch->done) {
			_uploadBatches.erase(_uploadBatches.begin() + i);
		} else if (batch->total <= 1
			|| !batch->id
			|| ranges::contains(_droppedUlBatches, batch->id)) {
			++i;
		} else {
			result.push_back({
				.batchId = batch->id,
				.sessionId = batch->sessionId,
				.total = batch->total,
				.done = batch->uploaded + batch->failed,
				.skipped = batch->duplicates,
				.startedAt = batch->startedAt,
				.finished = false,
			});
			++i;
		}
	}
	for (const auto &run : _finishedUlRuns) {
		result.push_back({
			.batchId = run.batchId,
			.sessionId = run.sessionId,
			.total = run.total,
			.done = run.done,
			.skipped = run.skipped,
			.startedAt = run.startedAt,
			.finishedAt = run.finishedAt,
			.finished = true,
		});
	}
	ranges::stable_sort(
		result,
		ranges::less(),
		[](const UlRunSnapshot &run) { return run.startedAt; });
	return result;
}

void DownloadManager::notifyDuplicateSkips() {
	const auto downloads = _duplicatesSkipped[int(DedupDb::Table::Downloads)];
	const auto uploads = _duplicatesSkipped[int(DedupDb::Table::Uploads)];
	_duplicatesSkipped[int(DedupDb::Table::Downloads)] = 0;
	_duplicatesSkipped[int(DedupDb::Table::Uploads)] = 0;
	if (downloads > 0) {
		Ui::Toast::Show(tr::lng_tm_dl_duplicates_skipped(
			tr::now,
			lt_count,
			downloads));
	}
	if (uploads > 0) {
		Ui::Toast::Show(tr::lng_tm_ul_duplicates_skipped(
			tr::now,
			lt_count,
			uploads));
	}
}

bool DownloadManager::empty() const {
	for (const auto &[session, data] : _sessions) {
		if (!data.downloading.empty() || !data.downloaded.empty()) {
			return false;
		}
	}
	return true;
}

void DownloadManager::trackSession(not_null<Main::Session*> session) {
	auto &data = _sessions.emplace(session, SessionData()).first->second;
	auto runs = std::vector<FinishedDlRun>();
	auto ulRuns = std::vector<FinishedUlRun>();
	data.downloaded = deserialize(session, &data.jobId, &runs, &ulRuns);
	data.resolveNeeded = data.downloaded.size();
	for (const auto &run : runs) {
		if (run.sessionId != session->uniqueId()
			|| ranges::any_of(
				_finishedDlRuns,
				[&](const FinishedDlRun &existing) {
					return existing.batchId == run.batchId;
				})) {
			continue;
		}
		const auto hasMembers = ranges::any_of(
			data.downloaded,
			[&](const DownloadedId &id) {
				return id.batchId == run.batchId;
			});
		if (hasMembers) {
			_finishedDlRuns.push_back(run);
		}
	}
	for (const auto &run : ulRuns) {
		if (run.sessionId != session->uniqueId()
			|| ranges::any_of(
				_finishedUlRuns,
				[&](const FinishedUlRun &existing) {
					return existing.batchId == run.batchId;
				})) {
			continue;
		}
		_finishedUlRuns.push_back(run);
	}

	session->data().documentLoadProgress(
	) | rpl::filter([=](not_null<DocumentData*> document) {
		return _loadingDocuments.contains(document);
	}) | rpl::on_next([=](not_null<DocumentData*> document) {
		check(document);
	}, data.lifetime);

	session->data().itemLayoutChanged(
	) | rpl::filter([=](not_null<const HistoryItem*> item) {
		return _loading.contains(item);
	}) | rpl::on_next([=](not_null<const HistoryItem*> item) {
		check(item);
	}, data.lifetime);

	session->data().itemViewRefreshRequest(
	) | rpl::on_next([=](not_null<const HistoryItem*> item) {
		changed(item);
	}, data.lifetime);

	session->changes().messageUpdates(
		MessageUpdate::Flag::Destroyed
	) | rpl::on_next([=](const MessageUpdate &update) {
		removed(update.item);
	}, data.lifetime);

	session->account().sessionChanges(
	) | rpl::filter(
		rpl::mappers::_1 != session
	) | rpl::take(1) | rpl::on_next([=] {
		untrack(session);
	}, data.lifetime);
}

void DownloadManager::itemVisibilitiesUpdated(
		not_null<Main::Session*> session) {
	const auto i = _sessions.find(session);
	if (i == end(_sessions)
		|| i->second.downloading.empty()
		|| !i->second.downloading.front().hiddenByView) {
		return;
	}
	for (const auto &id : i->second.downloading) {
		if (!id.done
			&& !session->data().queryItemVisibility(id.object.item)) {
			for (auto &id : i->second.downloading) {
				id.hiddenByView = false;
			}
			_loadingListChanges.fire({});
			return;
		}
	}
}

int64 DownloadManager::computeNextStartDate() {
	const auto now = base::unixtime::now();
	if (_lastStartedBase != now) {
		_lastStartedBase = now;
		_lastStartedAdded = 0;
	} else {
		++_lastStartedAdded;
	}
	return int64(_lastStartedBase) * 1000 + _lastStartedAdded;
}

void DownloadManager::addLoading(
	DownloadObject object,
	bool enhancedForward,
	uint64 batchId) {
	Expects(object.item != nullptr);
	Expects(object.document != nullptr);

	Info::Downloads::SetLastActivityTab(enhancedForward
		? Info::Downloads::Tab::Forwards
		: Info::Downloads::Tab::Downloads);

	const auto item = object.item;
	auto &data = sessionData(item);

	const auto already = ranges::find(data.downloading, item, ByItem);
	if (already != end(data.downloading)) {
		const auto document = already->object.document;
		const auto photo = already->object.photo;
		if (document == object.document && photo == object.photo) {
			check(item);
			return;
		}
		remove(data, already);
	}

	const auto size = object.document->size;
	const auto path = object.document->loadingFilePath();
	if (path.isEmpty()) {
		return;
	}

	const auto start = [=, &data] {
	    const auto ready = QFileInfo(path).exists()
		    ? QFileInfo(path).size()
		    : 0;
		if (!enhancedForward) {
			const auto active = ranges::any_of(
				data.downloading,
				[](const DownloadingId &entry) {
					return !entry.done;
				});
			if (!active) {
				++data.jobId;
			}
			_jobCounterChanged.fire({});
		}
		data.downloading.push_back({
			.object = object,
			.started = computeNextStartDate(),
			.path = path,
			.ready = ready,
			.total = size,
			.hiddenByView = false,
			.enhancedForward = enhancedForward,
			.jobIndex = data.jobId,
			.batchId = enhancedForward ? uint64(0) : batchId,
		});
		_loading.emplace(item);
		_loadingDocuments.emplace(object.document);
		_loadingProgress = DownloadProgress{
			.ready = _loadingProgress.current().ready + ready,
			.total = _loadingProgress.current().total + size,
		};
		_loadingListChanges.fire({});
		_clearLoadingTimer.cancel();

		if (const auto document = object.document) {
			if (!enhancedForward && IsServerMsgId(item->id)) {
				const auto peer = item->history()->peer;
				ensureDedupDb().insertDlResume({
					.sessionId = item->history()->session().uniqueId(),
					.peerId = peer->id.value,
					.msgId = item->id.bare,
					.path = path,
					.fileSize = size,
					.docId = uint64(document->id),
				});
			}
		}

		if (GetEnhancedBool("prevent_download_duplicates") && !enhancedForward) {
			saveFileHash(
				&item->history()->session(),
				object.document,
				size);
		}

		check(item);
	};

	start();
}

not_null<HistoryItem*> DownloadManager::generateExternalItem(
		not_null<DocumentData*> document) {
	_generatedDocuments.emplace(document);
	return generateFakeItem(document);
}

void DownloadManager::addLoadingExternal(
		DownloadObject object,
		const QString &path,
		int64 total,
		Fn<void()> cancel) {
	Expects(object.item != nullptr);
	Expects(object.document != nullptr);
	Expects(cancel != nullptr);

	const auto item = object.item;
	auto &data = sessionData(item);

	data.downloading.push_back({
		.object = object,
		.started = computeNextStartDate(),
		.path = path,
		.externalCancel = std::move(cancel),
		.total = total,
	});
	_loading.emplace(item);
	_loadingProgress = DownloadProgress{
		.ready = _loadingProgress.current().ready,
		.total = _loadingProgress.current().total + total,
	};
	_loadingListChanges.fire({});
	_clearLoadingTimer.cancel();
}

auto DownloadManager::lookupExternal(not_null<const HistoryItem*> item)
-> std::pair<SessionData*, std::vector<DownloadingId>::iterator> {
	const auto session = &item->history()->session();
	const auto i = _sessions.find(session);
	if (i == end(_sessions)) {
		return {};
	}
	const auto j = ranges::find(i->second.downloading, item, ByItem);
	if (j == end(i->second.downloading) || !j->externalCancel) {
		return {};
	}
	return { &i->second, j };
}

void DownloadManager::updateLoadingExternal(
		not_null<const HistoryItem*> item,
		int64 ready,
		int64 total) {
	const auto [data, i] = lookupExternal(item);
	if (!data || i->done) {
		return;
	}
	const auto readyChange = ready - i->ready;
	const auto totalChange = total - i->total;
	if (!readyChange && !totalChange) {
		return;
	}
	i->ready += readyChange;
	i->total += totalChange;
	_loadingProgress = DownloadProgress{
		.ready = _loadingProgress.current().ready + readyChange,
		.total = _loadingProgress.current().total + totalChange,
	};
	item->history()->owner().requestItemRepaint(item);
}

auto DownloadManager::loadingExternalState(
	not_null<const HistoryItem*> item)
-> std::optional<ExternalLoadingState> {
	const auto [data, i] = lookupExternal(item);
	if (!data) {
		return std::nullopt;
	}
	return ExternalLoadingState{
		.ready = i->ready,
		.total = i->total,
		.done = i->done,
	};
}

void DownloadManager::cancelLoadingExternal(
		not_null<const HistoryItem*> item) {
	const auto [data, i] = lookupExternal(item);
	if (data) {
		cancel(*data, i);
	}
}

void DownloadManager::finishLoadingExternal(
		not_null<const HistoryItem*> item,
		const QString &path) {
	const auto [data, i] = lookupExternal(item);
	if (!data) {
		return;
	}
	const auto object = i->object;
	const auto started = i->started;
	if (!i->done) {
		const auto readyChange = i->total - i->ready;
		i->ready += readyChange;
		i->done = true;
		_loading.remove(object.item);
		_loadingDone.emplace(object.item);
		_loadingProgress = DownloadProgress{
			.ready = _loadingProgress.current().ready + readyChange,
			.total = _loadingProgress.current().total,
		};
		_loadingListChanges.fire({});
		if (_loading.empty()) {
			_clearLoadingTimer.callOnce(kClearLoadingTimeout);
		}
		item->history()->owner().requestItemRepaint(item);
	}
	addLoaded(object, path, started);
}

void DownloadManager::removeLoadingExternal(
		not_null<const HistoryItem*> item) {
	const auto [data, i] = lookupExternal(item);
	if (!data) {
		return;
	}
	i->externalCancel = nullptr;
	remove(*data, i);
}

void DownloadManager::check(not_null<const HistoryItem*> item) {
	auto &data = sessionData(item);
	const auto i = ranges::find(data.downloading, item, ByItem);
	Assert(i != end(data.downloading));
	check(data, i);
}

void DownloadManager::check(not_null<DocumentData*> document) {
	auto &data = sessionData(document);
	const auto i = ranges::find(
		data.downloading,
		document.get(),
		ByDocument);
	Assert(i != end(data.downloading));
	check(data, i);
}

void DownloadManager::check(
		SessionData &data,
		std::vector<DownloadingId>::iterator i) {
	auto &entry = *i;

	if (entry.externalCancel) {
		return;
	} else if (!ItemContainsMedia(entry.object)) {
		cancel(data, i);
		return;
	}
	const auto document = entry.object.document;

	// Load with progress only documents for now.
	Assert(document != nullptr);

	const auto path = document->filepath(true);
	if (!path.isEmpty()) {
		if (entry.enhancedForward) {
			if (_loading.contains(entry.object.item)) {
				const auto totalChange = document->size - entry.total;
				const auto readyChange = document->size - entry.ready;
				entry.ready += readyChange;
				entry.total += totalChange;
				entry.done = true;
				_loadingDocuments.remove(document);
				_loading.remove(entry.object.item);
				_loadingDone.emplace(entry.object.item);
				saveIfIdle();
				_loadingProgress = DownloadProgress{
					.ready = _loadingProgress.current().ready + readyChange,
					.total = _loadingProgress.current().total + totalChange,
				};
				_loadingListChanges.fire({});
			}
		} else if (_loading.contains(entry.object.item)) {
			addLoaded(entry.object, path, entry.started, entry.batchId);
		}
	} else if (!document->loading()) {
		remove(data, i);
	} else {
		const auto nowPaused = document->downloadPaused();
		const auto pausedChanged = (entry.paused != nowPaused);
		entry.paused = nowPaused;
		const auto totalChange = document->size - entry.total;
        const auto readyChange = document->loadOffset() - entry.ready;
		if (!readyChange && !totalChange) {
			if (pausedChanged) {
				_loadingListChanges.fire({});
			}
			return;
		}
		entry.ready += readyChange;
		entry.total += totalChange;
		_loadingProgress = DownloadProgress{
			.ready = _loadingProgress.current().ready + readyChange,
			.total = _loadingProgress.current().total + totalChange,
		};
	}
}

void DownloadManager::addLoaded(
	DownloadObject object,
	const QString &path,
	DownloadDate started,
	uint64 batchId) {
	Expects(object.item != nullptr);
	Expects(object.document || object.photo);

	const auto size = QFileInfo(path).size();
	if (size <= 0 || size > kMaxFileSize) {
		return;
	}

	const auto item = object.item;
	auto &data = sessionData(item);

	const auto id = object.document
		? DownloadId{ object.document->id, DownloadType::Document }
		: DownloadId{ object.photo->id, DownloadType::Photo };

	auto dedupHash = QByteArray();
	auto isBigFile = false;
	const auto docId = object.document
		? object.document->id
		: (object.photo ? object.photo->id : 0);
	auto &dedupDb = ensureDedupDb();
	const auto dedupOff = !GetEnhancedBool("prevent_download_duplicates");
	if ((!dedupOff || batchId) && dedupDb.isOpen()) {
		if (docId) {
			// saveFileHash() may have stored the remote fingerprint for this
			// doc id when the download started. Re-reading the same 2 sample
			// chunks off the just-finished file would recompute it, so reuse.
			dedupHash = dedupDb.hashForDocId(DedupDb::Table::Downloads, docId);
		}
		// Only big files get a 2-chunk remote hash at download start, which
		// means their duplicate decision was already made before download, in
		// checkDuplicate. So isBigFile here really means "decided earlier".
		isBigFile = !dedupHash.isEmpty();
		if (dedupHash.isEmpty()) {
			// No start-time record: dedup was off when this started, the
			// remote fetch failed, or the file was too small to sample
			// remotely (see kDedupMinPartialHashSize). FileFingerprint()
			// covers that last case with a full-file local hash, so it's
			// still worth trying here as a fallback.
			dedupHash = Data::FileFingerprint(path, size);
		}
	}
	// A hash match against our *own* record (the one saveFileHash wrote for
	// this same docId when the download started) is not a duplicate - it's
	// just this download recognizing itself. Excluding it here is required,
	// otherwise every dedup-tracked download would delete the file it just
	// finished the moment it completed.
	const auto isDuplicateByHash = [&] {
		if (dedupHash.isEmpty() || !dedupDb.isOpen()) {
			return false;
		}
		return (dedupDb.seekDocumentId(
			DedupDb::Table::Downloads,
			dedupHash,
			docId) != 0);
	}();
	const auto isDuplicatePhotoById = [&] {
		if (!object.photo || !docId) {
			return false;
		}
		// The same photo id already has a dedup record (finished or still
		// unfinished), so the content is already known to be downloaded.
		return dedupDb.containsDocId(DedupDb::Table::Downloads, docId);
	}();
	const auto sessionId = item->history()->session().uniqueId();
	const auto isDuplicateTmp = batchId
		&& dedupOff
		&& docId
		&& dedupDb.isOpen()
		&& (dedupDb.containsDlTmpDocId(sessionId, batchId, docId)
			|| (!dedupHash.isEmpty()
				&& dedupDb.containsDlTmpHash(sessionId, batchId, dedupHash)));
	if ((isDuplicateByHash || isDuplicatePhotoById || isDuplicateTmp)
		&& !isBigFile) {
		// A 2-chunk remote hash (big files only) means the duplicate decision
		// was already made in checkDuplicate - the duplicate was skipped and
		// never written to disk, so re-checking here would delete the real
		// download. Only files without one (small files, photos, failed
		// remote samples) are decided here.
		QFile::remove(path);
		if (!dedupHash.isEmpty() && docId && dedupDb.isOpen()) {
			// Remember this doc id -> same content so future dedup of this
			// exact id is O(1) without re-fetching a fingerprint.
			if (dedupOff) {
				if (batchId) {
					dedupDb.insertDlTmp(
						sessionId,
						batchId,
						docId,
						dedupHash);
				}
			} else {
				dedupDb.insert(DedupDb::Table::Downloads, {
					.hash = dedupHash,
					.documentId = docId,
					.status = u"f"_q,
				});
			}
		}
		if (const auto document = object.document) {
			_fingerprintCache.remove(document->id);
			ensureDedupDb().removeDlResume(
				item->history()->session().uniqueId(),
				item->history()->peer->id.value,
				item->id.bare);
		}
		const auto i = ranges::find(
			data.downloading,
			item,
			ByItem);
		if (i != end(data.downloading)) {
			const auto entry = *i;
			if (entry.object.document) {
				_loadingDocuments.remove(
					entry.object.document);
			}
			ensureDedupDb().removeDlResume(
				entry.object.item->history()->session().uniqueId(),
				entry.object.item->history()->peer->id.value,
				entry.object.item->id.bare);
			data.downloading.erase(i);
			const auto j = _loading.find(item);
			if (j != end(_loading)) {
				_loading.erase(j);
			}
			saveIfIdle();
			_loadingProgress = DownloadProgress{
				.ready = std::max(
					int64(0),
					_loadingProgress
						.current()
						.ready
						- entry.ready),
				.total = std::max(
					int64(0),
					_loadingProgress
						.current()
						.total
						- entry.total),
			};
			_loadingListChanges.fire({});
		}
		return;
	}

	const auto completedJobIndex = [&] {
		const auto i = ranges::find(data.downloading, item, ByItem);
		return (i != end(data.downloading)) ? i->jobIndex : data.jobId;
	}();

	data.downloaded.push_back({
		.download = id,
		.started = started,
		.path = path,
		.size = size,
		.itemId = item->fullId(),
		.peerAccessHash = PeerAccessHash(item->history()->peer),
		.jobIndex = completedJobIndex,
		.batchId = batchId,
		.object = std::make_unique<DownloadObject>(object),
	});
	_loaded.emplace(item);
	_loadedAdded.fire(&data.downloaded.back());

	if (!dedupOff && !dedupHash.isEmpty() && docId && dedupDb.isOpen()) {
		dedupDb.insert(DedupDb::Table::Downloads, {
			.hash = dedupHash,
			.documentId = docId,
			.status = u"f"_q,
		});
		if (const auto document = object.document) {
			_fingerprintCache.remove(document->id);
		}
	}
	if (batchId
		&& dedupOff
		&& !dedupHash.isEmpty()
		&& docId
		&& dedupDb.isOpen()) {
		dedupDb.insertDlTmp(sessionId, batchId, docId, dedupHash);
	}

	writePostponed(&item->history()->session());

	const auto i = ranges::find(data.downloading, item, ByItem);
	if (i != end(data.downloading)) {
		auto &entry = *i;
		const auto document = entry.object.document;
		if (document) {
			_loadingDocuments.remove(document);
		}
		const auto j = _loading.find(entry.object.item);
		if (j == end(_loading)) {
			return;
		}
		const auto totalChange = document->size - entry.total;
		const auto readyChange = document->size - entry.ready;
		entry.ready += readyChange;
		entry.total += totalChange;
		entry.done = true;
		_loading.erase(j);
		_loadingDone.emplace(entry.object.item);
		_jobCounterChanged.fire({});
		saveIfIdle();
		if (const auto document = entry.object.document) {
			ensureDedupDb().removeDlResume(
				entry.object.item->history()->session().uniqueId(),
				entry.object.item->history()->peer->id.value,
				entry.object.item->id.bare);
		}
		_loadingProgress = DownloadProgress{
			.ready = _loadingProgress.current().ready + readyChange,
			.total = _loadingProgress.current().total + totalChange,
		};
		_loadingListChanges.fire({});
		// Previously this scheduled clearLoading() after kClearLoadingTimeout,
		// which silently dropped the finished entry (and any other finished
		// entries) from the downloads list a few seconds after completion.
		// Finished downloads must stay listed until the user explicitly
		// cancels them or clears the list (see clearIfFinished()).
	}
}

void DownloadManager::clearIfFinished() {
	if (_clearLoadingTimer.isActive()) {
		_clearLoadingTimer.cancel();
		clearLoading();
	}
}

void DownloadManager::deleteFiles(const std::vector<GlobalMsgId> &ids) {
	auto descriptor = DeleteFilesDescriptor();
	for (const auto &id : ids) {
		if (const auto item = MessageByGlobalId(id)) {
			const auto session = &item->history()->session();
			const auto i = _sessions.find(session);
			if (i == end(_sessions)) {
				continue;
			}
			auto &data = i->second;
			const auto j = ranges::find(
				data.downloading,
				not_null{ item },
				ByItem);
			if (j != end(data.downloading)) {
				cancel(data, j);
			}

			// Addressed by item, and one item may own several downloaded
			// files, so every one of them goes. Removing just the first left
			// the rest on disk while loadedRemoved already hid the row.
			auto erased = false;
			for (auto k = 0; k != int(data.downloaded.size());) {
				if (ByItem(data.downloaded[k]) != item) {
					++k;
					continue;
				}
				const auto &entry = data.downloaded[k];
				const auto document = entry.object->document;
				descriptor.files.emplace(entry.path, DocumentDescriptor{
					.sessionUniqueId = id.sessionUniqueId,
					.documentId = document ? document->id : DocumentId(),
					.itemId = id.itemId,
				});
				if (document) {
					_generatedDocuments.remove(document);
				}
				data.downloaded.erase(begin(data.downloaded) + k);
				erased = true;
			}
			if (erased) {
				_generated.remove(item);
				_loaded.remove(item);
				_loadedRemoved.fire_copy(item);
				_jobCounterChanged.fire({});

				descriptor.sessions.emplace(session);
			}
		}
	}
	finishFilesDelete(std::move(descriptor));
}

void DownloadManager::deleteAll() {
	auto descriptor = DeleteFilesDescriptor();
	for (auto &[session, data] : _sessions) {
		if (!data.downloaded.empty()) {
			descriptor.sessions.emplace(session);
		} else if (data.downloading.empty()) {
			continue;
		}
		const auto sessionUniqueId = session->uniqueId();
		while (!data.downloading.empty()) {
			cancel(data, data.downloading.end() - 1);
		}
		for (auto &id : base::take(data.downloaded)) {
			const auto object = id.object.get();
			const auto document = object ? object->document : nullptr;
			// Same as deleteFiles(): completed dedup records are kept even
			// when the downloaded file itself is deleted here.
			descriptor.files.emplace(id.path, DocumentDescriptor{
				.sessionUniqueId = sessionUniqueId,
				.documentId = document ? document->id : DocumentId(),
				.itemId = id.itemId,
			});
			if (document) {
				_generatedDocuments.remove(document);
			}
			if (const auto item = object ? object->item.get() : nullptr) {
				_loaded.remove(item);
				_generated.remove(item);
				_loadedRemoved.fire_copy(item);
			}
		}
		_jobCounterChanged.fire({});
	}
	for (const auto &session : descriptor.sessions) {
		writePostponed(session);
	}
	finishFilesDelete(std::move(descriptor));
}

void DownloadManager::finishFilesDelete(DeleteFilesDescriptor &&descriptor) {
	for (const auto &session : descriptor.sessions) {
		writePostponed(session);
	}
	crl::async([files = std::move(descriptor.files)]{
		for (const auto &file : files) {
			const auto folder = RichExportFolderToRemove(file.first);
			if (folder.isEmpty()) {
				Platform::File::MoveToTrash(file.first);
			} else if (!Platform::File::MoveToTrash(folder)) {
				QDir(folder).removeRecursively();
			}
			crl::on_main([descriptor = file.second, path = file.first] {
				if (const auto session = SessionByUniqueId(
						descriptor.sessionUniqueId)) {
					PruneEmptyDownloadFolders(session, path);
					if (const auto id = descriptor.documentId) {
						[[maybe_unused]] const auto location
							= session->data().document(id)->location(true);
					}
					const auto itemId = descriptor.itemId;
					if (const auto item = session->data().message(itemId)) {
						session->data().requestItemRepaint(item);
					}
				}
			});
		}
	});
}

auto DownloadManager::loadingList() const
-> ranges::any_view<const DownloadingId*, ranges::category::input> {
	return ranges::views::all(
		_sessions
	) | ranges::views::transform([=](const auto &pair) {
		return ranges::views::all(
			pair.second.downloading
		) | ranges::views::transform([](const DownloadingId &id) {
			return &id;
		});
	}) | ranges::views::join;
}

DownloadProgress DownloadManager::loadingProgress() const {
	return _loadingProgress.current();
}

rpl::producer<> DownloadManager::loadingListChanges() const {
	return _loadingListChanges.events();
}

auto DownloadManager::loadingProgressValue() const
-> rpl::producer<DownloadProgress> {
	return _loadingProgress.value();
}

bool DownloadManager::loadingInProgress(Main::Session *onlyInSession) const {
	return lookupLoadingItem(onlyInSession) != nullptr;
}

HistoryItem *DownloadManager::lookupLoadingItem(
		Main::Session *onlyInSession) const {
	constexpr auto find = [](const SessionData &data) {
		constexpr auto proj = &DownloadingId::done;
		const auto i = ranges::find(data.downloading, false, proj);
		return (i != end(data.downloading)) ? i->object.item.get() : nullptr;
	};
	if (onlyInSession) {
		const auto i = _sessions.find(onlyInSession);
		return (i != end(_sessions)) ? find(i->second) : nullptr;
	} else {
		for (const auto &[session, data] : _sessions) {
			if (const auto result = find(data)) {
				return result;
			}
		}
	}
	return nullptr;
}

void DownloadManager::loadingStop(Main::Session *onlyInSession) {
	const auto stopInSession = [&](SessionData &data) {
		while (!data.downloading.empty()) {
			cancel(data, data.downloading.end() - 1);
		}
	};
	if (onlyInSession) {
		const auto i = _sessions.find(onlyInSession);
		if (i != end(_sessions)) {
			stopInSession(i->second);
		}
	} else {
		for (auto &[session, data] : _sessions) {
			stopInSession(data);
		}
	}
}

void DownloadManager::clearLoading() {
	Expects(_loading.empty());

	for (auto &[session, data] : _sessions) {
		while (!data.downloading.empty()) {
			remove(data, data.downloading.end() - 1);
		}
	}
}

void DownloadManager::clearFinishedLoading() {
	auto sessions = base::flat_set<not_null<Main::Session*>>();
	dropFinishedDlRuns();
	for (auto &[session, data] : _sessions) {
		if (_clearLoadingTimer.isActive()) {
			_clearLoadingTimer.cancel();
			clearLoading();
		}
		if (data.downloaded.empty()) {
			continue;
		}
		for (auto &id : base::take(data.downloaded)) {
			const auto object = id.object.get();
			const auto document = object ? object->document : nullptr;
			if (document) {
				_generatedDocuments.remove(document);
			}
			if (const auto item = object ? object->item.get() : nullptr) {
				_loaded.remove(item);
				_generated.remove(item);
				_loadedRemoved.fire_copy(item);
			}
		}
		sessions.emplace(session);
	}
	for (const auto &session : sessions) {
		writePostponed(session);
	}
	// Finished runs are dropped for every session, so persist the wipe
	// even for sessions that had no downloaded files to clear.
	for (auto &[session, data] : _sessions) {
		if (!sessions.contains(session)) {
			writePostponed(session);
		}
	}
	dropHeldDlBatches();
}

void DownloadManager::clearFinishedItem(not_null<const HistoryItem*> item) {
	auto &data = sessionData(item);
	const auto i = ranges::find(data.downloaded, item, ByItem);
	if (i == end(data.downloaded)) {
		return;
	}
	const auto object = i->object.get();
	const auto document = object ? object->document : nullptr;
	if (document) {
		_generatedDocuments.remove(document);
	}
	_loaded.remove(item);
	_generated.remove(item);
	data.downloaded.erase(i);
	_loadedRemoved.fire_copy(item);
	writePostponed(&item->history()->session());
}

void DownloadManager::removeLoading(not_null<const HistoryItem*> item) {
	auto &data = sessionData(item);
	const auto i = ranges::find(data.downloading, item, ByItem);
	if (i == end(data.downloading)) {
		return;
	}
	remove(data, i);
}

auto DownloadManager::loadedList()
-> ranges::any_view<const DownloadedId*, ranges::category::input> {
	for (auto &[session, data] : _sessions) {
		resolve(session, data);
	}
	return ranges::views::all(
		_sessions
	) | ranges::views::transform([=](const auto &pair) {
		return ranges::views::all(
			pair.second.downloaded
		) | ranges::views::filter([](const DownloadedId &id) {
			return (id.object != nullptr);
		}) | ranges::views::transform([](const DownloadedId &id) {
			return &id;
		});
	}) | ranges::views::join;
}

rpl::producer<> DownloadManager::loadedResolveDone() const {
	using namespace rpl::mappers;
	return _loadedResolveDone.value() | rpl::filter(_1) | rpl::to_empty;
}

void DownloadManager::resolve(
		not_null<Main::Session*> session,
		SessionData &data) {
	const auto guard = gsl::finally([&] {
		checkFullResolveDone();
	});
	if (data.resolveSentTotal >= data.resolveNeeded
		|| data.resolveSentTotal >= kMaxResolvePerAttempt) {
		return;
	}
	struct Prepared {
		uint64 peerAccessHash = 0;
		QVector<MTPInputMessage> ids;
	};
	const auto &owner = session->data();
	auto prepared = base::flat_map<PeerId, Prepared>();
	auto last = begin(data.downloaded);
	auto from = last + (data.resolveNeeded - data.resolveSentTotal);
	for (auto i = from; i != last;) {
		auto &id = *--i;
		const auto msgId = id.itemId.msg;
		const auto info = QFileInfo(id.path);
		if (!info.exists() || info.size() != id.size) {
			// Mark as deleted.
			id.path = QString();
		} else if (!owner.message(id.itemId) && IsServerMsgId(msgId)) {
			const auto groupByPeer = peerIsChannel(id.itemId.peer)
				? id.itemId.peer
				: session->userPeerId();
			auto &perPeer = prepared[groupByPeer];
			if (peerIsChannel(id.itemId.peer) && !perPeer.peerAccessHash) {
				perPeer.peerAccessHash = id.peerAccessHash;
			}
			perPeer.ids.push_back(MTP_inputMessageID(MTP_int(msgId.bare)));
		}
		if (++data.resolveSentTotal >= kMaxResolvePerAttempt) {
			break;
		}
	}
	const auto check = [=] {
		auto &data = sessionData(session);
		if (!data.resolveSentRequests) {
			resolveRequestsFinished(session, data);
		}
	};
	const auto requestFinished = [=] {
		--sessionData(session).resolveSentRequests;
		check();
	};
	for (auto &[peer, perPeer] : prepared) {
		if (const auto channelId = peerToChannel(peer)) {
			session->api().request(MTPchannels_GetMessages(
				MTP_inputChannel(
					MTP_long(channelId.bare),
					MTP_long(perPeer.peerAccessHash)),
				MTP_vector<MTPInputMessage>(perPeer.ids)
			)).done([=](const MTPmessages_Messages &result) {
				session->data().processExistingMessages(
					session->data().channelLoaded(channelId),
					result);
				requestFinished();
			}).fail(requestFinished).send();
		} else {
			session->api().request(MTPmessages_GetMessages(
				MTP_vector<MTPInputMessage>(perPeer.ids)
			)).done([=](const MTPmessages_Messages &result) {
				session->data().processExistingMessages(nullptr, result);
				requestFinished();
			}).fail(requestFinished).send();
		}
	}
	data.resolveSentRequests += prepared.size();
	check();
}

void DownloadManager::resolveRequestsFinished(
		not_null<Main::Session*> session,
		SessionData &data) {
	const auto &owner = session->data();
	for (; data.resolveSentTotal > 0; --data.resolveSentTotal) {
		const auto i = begin(data.downloaded) + (--data.resolveNeeded);
		if (i->path.isEmpty()) {
			data.downloaded.erase(i);
			_jobCounterChanged.fire({});
			continue;
		}
		const auto item = owner.message(i->itemId);
		const auto media = item ? item->media() : nullptr;
		const auto document = media ? media->document() : nullptr;
		const auto photo = media ? media->photo() : nullptr;
		if (i->download.type == DownloadType::Document
			&& (!document || document->id != i->download.objectId)) {
			generateEntry(session, *i);
		} else if (i->download.type == DownloadType::Photo
			&& (!photo || photo->id != i->download.objectId)) {
			generateEntry(session, *i);
		} else {
			i->object = std::make_unique<DownloadObject>(DownloadObject{
				.item = item,
				.document = document,
				.photo = photo,
			});
			_loaded.emplace(item);
		}
		_loadedAdded.fire(&*i);
	}
	crl::on_main(session, [=] {
		resolve(session, sessionData(session));
	});
}

void DownloadManager::checkFullResolveDone() {
	if (_loadedResolveDone.current()) {
		return;
	}
	for (const auto &[session, data] : _sessions) {
		if (data.resolveSentTotal < data.resolveNeeded
			|| data.resolveSentRequests > 0) {
			return;
		}
	}
	_loadedResolveDone = true;
}

void DownloadManager::generateEntry(
		not_null<Main::Session*> session,
		DownloadedId &id) {
	Expects(!id.object);

	const auto info = QFileInfo(id.path);
	const auto document = session->data().document(
		base::RandomValue<DocumentId>(),
		0, // accessHash
		QByteArray(), // fileReference
		TimeId(id.started / 1000),
		QVector<MTPDocumentAttribute>(
			1,
			MTP_documentAttributeFilename(
				MTP_string(info.fileName()))),
		Core::MimeTypeForFile(info).name(),
		InlineImageLocation(), // inlineThumbnail
		ImageWithLocation(), // thumbnail
		ImageWithLocation(), // videoThumbnail
		false, // isPremiumSticker
		0, // dc
		id.size);
	document->setLocation(Core::FileLocation(info));
	_generatedDocuments.emplace(document);

	id.object = std::make_unique<DownloadObject>(DownloadObject{
		.item = generateFakeItem(document),
		.document = document,
	});
	_loaded.emplace(id.object->item);
}

auto DownloadManager::loadedAdded() const
-> rpl::producer<not_null<const DownloadedId*>> {
	return _loadedAdded.events();
}

auto DownloadManager::loadedRemoved() const
-> rpl::producer<not_null<const HistoryItem*>> {
	return _loadedRemoved.events();
}

void DownloadManager::remove(
		SessionData &data,
		std::vector<DownloadingId>::iterator i) {
	const auto now = DownloadProgress{
		.ready = _loadingProgress.current().ready - i->ready,
		.total = _loadingProgress.current().total - i->total,
	};
	_loading.remove(i->object.item);
	_loadingDone.remove(i->object.item);
	if (const auto document = i->object.document) {
		_loadingDocuments.remove(document);
		if (GetEnhancedBool("prevent_download_duplicates")) {
			_fingerprintCache.remove(document->id);
			ensureDedupDb().removeByDocumentId(
				DedupDb::Table::Downloads,
				document->id,
				u"u"_q);
		}
		if (i->externalCancel && !_loaded.contains(i->object.item)) {
			_generated.remove(i->object.item);
			_generatedDocuments.remove(document);
		}
	}
	data.downloading.erase(i);
	_loadingListChanges.fire({});
	_loadingProgress = now;
	_jobCounterChanged.fire({});
	// No auto-clear scheduling here either: entries only leave the list via
	// remove() itself, called from an explicit cancel or clearLoading()/
	// clearIfFinished() triggered by the user.
}

void DownloadManager::cancel(
		SessionData &data,
		std::vector<DownloadingId>::iterator i) {
	const auto object = i->object;
	const auto item = object.item;
	const auto path = i->path;
	const auto peer = item->history()->peer;
	const auto session = &item->history()->session();
	const auto document = object.document;
	const auto total = i->total;

	if (document && GetEnhancedBool("prevent_download_duplicates")) {
		removeFileHash(document->id, total);
		clearFingerprintCache(document->id);
	}

	if (auto external = base::take(i->externalCancel)) {
		remove(data, i);
		external();
		return;
	}

	remove(data, i);
	if (!item->isAdminLogEntry()) {
		if (document) {
			document->cancel();
		} else if (const auto photo = object.photo) {
			photo->cancel();
		}
	}
	if (document) {
		ensureDedupDb().removeDlResume(
			item->history()->session().uniqueId(),
			item->history()->peer->id.value,
			item->id.bare);
	}
	PruneEmptyDownloadFolders(session, path);
}

void DownloadManager::pause(not_null<const HistoryItem*> item) {
	auto &data = sessionData(item);
	const auto i = ranges::find(data.downloading, item, ByItem);
	if (i == end(data.downloading)) {
		return;
	}
	const auto document = i->object.document;
	if (document && document->loading() && !document->downloadPaused()) {
		document->pause();
		i->paused = true;
		_loadingListChanges.fire({});
	}
}

void DownloadManager::resume(not_null<const HistoryItem*> item) {
	auto &data = sessionData(item);
	const auto i = ranges::find(data.downloading, item, ByItem);
	if (i == end(data.downloading)) {
		return;
	}
	const auto document = i->object.document;
	if (document && document->downloadPaused()) {
		document->resume();
		i->paused = false;
		_loadingListChanges.fire({});
	}
}

void DownloadManager::cancel(not_null<const HistoryItem*> item) {
	auto &data = sessionData(item);
	const auto i = ranges::find(data.downloading, item, ByItem);
	if (i == end(data.downloading)) {
		return;
	}
	cancel(data, i);
}

void DownloadManager::cancelWithConfirmation(not_null<const HistoryItem*> item) {
	auto &data = sessionData(item);
	const auto i = ranges::find(data.downloading, item, ByItem);
	const auto tracked = (i != end(data.downloading));
	if (tracked && i->done) {
		// Nothing to cancel: it already finished. Removing a finished entry
		// from the list is not "cancel a download" and doesn't need this
		// confirmation - that's clearFinishedLoading()/clearIfFinished().
		return;
	}
	const auto window = Core::App().windowFor(
		not_null(&item->history()->session().account()));
	if (!window) {
		// No window to show the confirmation in (e.g. headless/background
		// call) - fall back to cancelling directly rather than silently
		// doing nothing.
		cancel(item);
		return;
	}
	const auto weak = base::make_weak(&item->history()->session());
	const auto id = item->fullId();
	auto box = Box([=](not_null<Ui::GenericBox*> box) {
		box->addRow(object_ptr<Ui::FlatLabel>(
			box.get(),
			tr::lng_tm_dl_cancel_confirm(tr::now),
			st::boxLabel));
		box->setStyle(st::defaultBox);
		box->addButton(tr::lng_box_yes(), [=] {
			box->closeBox();
			if (const auto strong = weak.get()) {
				if (const auto item = strong->data().message(id)) {
					// The document may not be tracked as a download at all
					// (e.g. it was only ever auto-loaded, never registered
					// via addLoading()) - cancel() only acts on tracked
					// entries, so fall back to cancelling the document's own
					// loader directly for the untracked case.
				    auto &data = sessionData(item);
				    const auto itemId = not_null<HistoryItem*>(item);
				    const auto i = ranges::find(data.downloading, itemId, ByItem);
					if (i != end(data.downloading)) {
						cancel(item);
					} else if (const auto media = item->media()) {
						if (const auto document = media->document()) {
							document->cancel();
						} else if (const auto photo = media->photo()) {
							photo->cancel();
						}
					}
				}
			}
		}, st::attentionBoxButton);
		box->addButton(tr::lng_box_no(), [=] {
			box->closeBox();
		});
	});
	window->show(std::move(box));
}

void DownloadManager::pauseAll() {
	auto changed = false;
	for (auto &[session, data] : _sessions) {
		for (auto &entry : data.downloading) {
			const auto document = entry.object.document;
			if (document
				&& document->loading()
				&& !document->downloadPaused()) {
				document->pause();
				entry.paused = true;
				changed = true;
			}
		}
	}
	if (changed) {
		_loadingListChanges.fire({});
		clearFingerprintCache();
	}
}

void DownloadManager::pauseAll(not_null<Main::Session*> session) {
	const auto i = _sessions.find(session);
	if (i == end(_sessions)) {
		return;
	}
	auto changed = false;
	for (auto &entry : i->second.downloading) {
		const auto document = entry.object.document;
		if (document
			&& document->loading()
			&& !document->downloadPaused()) {
			document->pause();
			entry.paused = true;
			changed = true;
		}
	}
	if (changed) {
		_loadingListChanges.fire({});
		clearFingerprintCache();
	}
}

void DownloadManager::resumeAll() {
	auto changed = false;
	for (auto &[session, data] : _sessions) {
		for (auto &entry : data.downloading) {
			const auto document = entry.object.document;
			if (document && document->downloadPaused()) {
				document->resume();
				entry.paused = false;
				changed = true;
			}
		}
	}
	if (changed) {
		_loadingListChanges.fire({});
	}
}

void DownloadManager::cancelAll() {
	for (auto &[session, data] : _sessions) {
		auto active = std::vector<not_null<const HistoryItem*>>();
		for (const auto &entry : data.downloading) {
			if (_loading.contains(entry.object.item)) {
				active.push_back(entry.object.item);
			}
		}
		for (const auto item : active) {
			const auto i = ranges::find(data.downloading, item, ByItem);
			if (i != end(data.downloading)) {
				cancel(data, i);
			}
		}
	}
}

bool DownloadManager::anyFinishedLoading() const {
	for (const auto &[session, data] : _sessions) {
		if (!data.downloaded.empty()) {
			return true;
		}
		for (const auto &entry : data.downloading) {
			if (entry.enhancedForward) {
				// Enhanced-Forward downloads are tracked by the forward
				// pipeline; they must not count as finished downloads.
				continue;
			}
			if (!_loading.contains(entry.object.item)) {
				return true;
			}
		}
	}
	return false;
}

bool DownloadManager::anyPaused() const {
	for (const auto &[session, data] : _sessions) {
		for (const auto &entry : data.downloading) {
			if (entry.paused) {
				return true;
			}
		}
	}
	return false;
}

bool DownloadManager::anyResumable() const {
	for (const auto &[session, data] : _sessions) {
		for (const auto &entry : data.downloading) {
			if (!entry.paused && !entry.done) {
				return true;
			}
		}
	}
	return false;
}

void DownloadManager::changed(not_null<const HistoryItem*> item) {
	if (_loaded.contains(item)) {
		auto &data = sessionData(item);

		// One item may be referenced by several downloaded entries, each
		// with its own object, so all of them have to be re-checked.
		// Indexed, because detach() emits and the list can move under us.
		auto found = false;
		for (auto i = 0; i != int(data.downloaded.size()); ++i) {
			if (ByItem(data.downloaded[i]) != item.get()) {
				continue;
			}
			found = true;
			if (!ItemContainsMedia(*data.downloaded[i].object)) {
				detach(data, data.downloaded[i]);
			}
		}
		Assert(found);
	}
	if (_loading.contains(item) || _loadingDone.contains(item)) {
		check(item);
	}
}

void DownloadManager::removed(not_null<const HistoryItem*> item) {
	if (_loaded.contains(item)) {
		auto &data = sessionData(item);
		auto i = ranges::find(data.downloaded, item.get(), ByItem);
		Assert(i != end(data.downloaded));

		// One item may be referenced by several downloaded entries, and
		// every one of them must be detached, or the rest keep pointing
		// at the destroyed HistoryItem.
		do {
			detach(data, *i);
			i = ranges::find(data.downloaded, item.get(), ByItem);
		} while (i != end(data.downloaded));
	}
	if (_loading.contains(item) || _loadingDone.contains(item)) {
		auto &data = sessionData(item);
		const auto i = ranges::find(data.downloading, item, ByItem);
		Assert(i != end(data.downloading));

		// We don't want to download files without messages.
		// For example, there is no way to refresh a file reference for them.
		//entry.object.item = nullptr;
		cancel(data, i);
	}
}

not_null<HistoryItem*> DownloadManager::regenerateItem(
		const DownloadObject &previous) {
	return generateItem(previous.item, previous.document, previous.photo);
}

not_null<HistoryItem*> DownloadManager::generateFakeItem(
		not_null<DocumentData*> document) {
	return generateItem(nullptr, document, nullptr);
}

not_null<HistoryItem*> DownloadManager::generateItem(
		HistoryItem *previousItem,
		DocumentData *document,
		PhotoData *photo) {
	Expects(document || photo);

	const auto session = document
		? &document->session()
		: &photo->session();
	const auto history = previousItem
		? previousItem->history()
		: session->data().history(session->user());
	;
	const auto caption = TextWithEntities();
	const auto make = [&](const auto media) {
		return history->makeMessage({
			.id = history->nextNonHistoryEntryId(),
			.flags = MessageFlag::FakeHistoryItem,
			.from = (previousItem
				? previousItem->from()->id
				: session->userPeerId()),
			.date = base::unixtime::now(),
		}, media, caption);
	};
	const auto result = document ? make(document) : make(photo);
	_generated.emplace(result);
	return result;
}

void DownloadManager::detach(SessionData &data, DownloadedId &id) {
	Expects(id.object != nullptr);
	Expects(!_generated.contains(id.object->item));

	// Maybe generate new document?
	const auto was = id.object->item;
	const auto now = regenerateItem(*id.object);
	id.object->item = now;
	_loaded.emplace(now);

	// One item may have several downloaded entries, so it leaves _loaded -
	// and the downloads list - only when the last of them was detached.
	// Reporting it removed while another entry still holds it would drop a
	// row that is still backed by a file.
	if (!ranges::contains(data.downloaded, was.get(), ByItem)) {
		_loaded.remove(was);
		_loadedRemoved.fire_copy(was);
	}

	_loadedAdded.fire_copy(&id);
}

DownloadManager::SessionData &DownloadManager::sessionData(
		not_null<Main::Session*> session) {
	const auto i = _sessions.find(session);
	Assert(i != end(_sessions));
	return i->second;
}

const DownloadManager::SessionData &DownloadManager::sessionData(
		not_null<Main::Session*> session) const {
	const auto i = _sessions.find(session);
	Assert(i != end(_sessions));
	return i->second;
}

DownloadManager::SessionData &DownloadManager::sessionData(
		not_null<const HistoryItem*> item) {
	return sessionData(&item->history()->session());
}

DownloadManager::SessionData &DownloadManager::sessionData(
		not_null<DocumentData*> document) {
	return sessionData(&document->session());
}

void DownloadManager::writePostponed(not_null<Main::Session*> session) {
	session->account().local().updateDownloads(serializator(session));
}

Fn<std::optional<QByteArray>()> DownloadManager::serializator(
		not_null<Main::Session*> session) const {
	return [this, weak = base::make_weak(session)]()
		-> std::optional<QByteArray> {
		const auto strong = weak.get();
		if (!strong) {
			return std::nullopt;
		} else if (!_sessions.contains(strong)) {
			return QByteArray();
		}
		auto result = QByteArray();
		const auto &data = sessionData(strong);
		const auto sessionId = strong->uniqueId();
		auto runs = std::vector<const FinishedDlRun*>();
		for (const auto &run : _finishedDlRuns) {
			if (run.sessionId == sessionId && run.total > 1) {
				runs.push_back(&run);
			}
		}
		auto ulRuns = std::vector<const FinishedUlRun*>();
		for (const auto &run : _finishedUlRuns) {
			if (run.sessionId == sessionId && run.total > 1) {
				ulRuns.push_back(&run);
			}
		}
		const auto count = data.downloaded.size();
		const auto constant = sizeof(quint64) // download.objectId
			+ sizeof(qint32) // download.type
			+ sizeof(qint64) // started
			+ sizeof(quint32) // size
			+ sizeof(quint64) // itemId.peer
			+ sizeof(qint64) // itemId.msg
			+ sizeof(quint64) // peerAccessHash
			+ sizeof(qint32) // jobIndex
			+ sizeof(quint64); // batchId
		const auto runConstant = sizeof(quint64) // batchId
			+ sizeof(quint64) // sessionId
			+ sizeof(qint32) // total
			+ sizeof(qint32) // done
			+ sizeof(qint32) // skipped
			+ sizeof(qint64) // startedAt
			+ sizeof(qint64); // finishedAt
		auto size = sizeof(qint32) // version
			+ sizeof(qint32) // count
			+ count * constant
			+ sizeof(qint32) // jobId
			+ sizeof(qint32) // run count
			+ runs.size() * runConstant
			+ sizeof(qint32) // upload run count
			+ ulRuns.size() * runConstant;
		for (const auto &id : data.downloaded) {
			size += Serialize::stringSize(id.path);
		}
		for (const auto run : runs) {
			size += Serialize::stringSize(run->srcName);
			size += Serialize::stringSize(run->destDir);
		}
		result.reserve(size);

		auto stream = QDataStream(&result, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		stream << qint32(-3) << qint32(int(count));
		for (const auto &id : data.downloaded) {
			stream
				<< quint64(id.download.objectId)
				<< qint32(id.download.type)
				<< qint64(id.started)
				// FileSize: Right now any file size fits 32 bit.
				<< quint32(id.size)
				<< quint64(id.itemId.peer.value)
				<< qint64(id.itemId.msg.bare)
				<< quint64(id.peerAccessHash)
				<< id.path
				<< qint32(id.jobIndex)
				<< quint64(id.batchId);
		}
		stream << qint32(data.jobId);
		stream << qint32(int(runs.size()));
		for (const auto run : runs) {
			stream
				<< quint64(run->batchId)
				<< quint64(run->sessionId)
				<< run->srcName
				<< run->destDir
				<< qint32(run->total)
				<< qint32(run->done)
				<< qint32(run->skipped)
				<< qint64(run->startedAt)
				<< qint64(run->finishedAt);
		}
		stream << qint32(int(ulRuns.size()));
		for (const auto run : ulRuns) {
			stream
				<< quint64(run->batchId)
				<< quint64(run->sessionId)
				<< run->srcName
				<< run->destDir
				<< qint32(run->total)
				<< qint32(run->done)
				<< qint32(run->skipped)
				<< qint64(run->startedAt)
				<< qint64(run->finishedAt);
		}
		stream.device()->close();

		return result;
	};
}

std::vector<DownloadedId> DownloadManager::deserialize(
		not_null<Main::Session*> session,
		int *jobId,
		std::vector<FinishedDlRun> *runs,
		std::vector<FinishedUlRun> *ulRuns) const {
	const auto serialized = session->account().local().downloadsSerialized();
	if (serialized.isEmpty()) {
		return {};
	}

	QDataStream stream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);

	auto count = qint32();
	stream >> count;
	if (stream.status() != QDataStream::Ok) {
		return {};
	}
	const auto legacy = (count >= 0);
	auto v2 = false;
	auto v3 = false;
	if (!legacy) {
		if (count == -3) {
			v2 = true;
			v3 = true;
		} else if (count == -2) {
			v2 = true;
		} else if (count != -1) {
			return {};
		}
		stream >> count;
	}
	if (stream.status() != QDataStream::Ok
		|| count <= 0
		|| count > 99'999) {
		return {};
	}
	auto result = std::vector<DownloadedId>();
	result.reserve(count);
	for (auto i = 0; i != count; ++i) {
		auto downloadObjectId = quint64();
		auto uncheckedDownloadType = qint32();
		auto started = qint64();
		// FileSize: Right now any file size fits 32 bit.
		auto size = quint32();
		auto itemIdPeer = quint64();
		auto itemIdMsg = qint64();
		auto peerAccessHash = quint64();
		auto path = QString();
		stream
			>> downloadObjectId
			>> uncheckedDownloadType
			>> started
			>> size
			>> itemIdPeer
			>> itemIdMsg
			>> peerAccessHash
			>> path;
		auto jobIndex = qint32(0);
		auto batchId = quint64(0);
		if (!legacy) {
			stream >> jobIndex;
			if (v2) {
				stream >> batchId;
			}
		}
		const auto downloadType = DownloadType(uncheckedDownloadType);
		if (stream.status() != QDataStream::Ok
			|| path.isEmpty()
			|| size <= 0
			|| size > kMaxFileSize
			|| (downloadType != DownloadType::Document
				&& downloadType != DownloadType::Photo)) {
			return {};
		}
		result.push_back({
			.download = {
				.objectId = downloadObjectId,
				.type = downloadType,
			},
			.started = started,
			.path = path,
			.size = int64(size),
			.itemId = { PeerId(itemIdPeer), MsgId(itemIdMsg) },
			.peerAccessHash = peerAccessHash,
			.jobIndex = legacy ? 0 : int(jobIndex),
			.batchId = v2 ? batchId : uint64(0),
		});
	}
	if (legacy) {
		if (jobId) {
			*jobId = 1;
		}
	} else {
		if (jobId && !stream.atEnd()) {
			auto value = qint32();
			stream >> value;
			*jobId = value;
		}
		if (v2 && runs && !stream.atEnd()) {
			auto runCount = qint32(0);
			stream >> runCount;
			if (stream.status() == QDataStream::Ok
				&& runCount >= 0
				&& runCount <= 999) {
				for (auto i = 0; i != runCount; ++i) {
					auto run = FinishedDlRun();
					auto runBatchId = quint64(0);
					auto runSessionId = quint64(0);
					auto total = qint32(0);
					auto done = qint32(0);
					auto skipped = qint32(0);
					auto startedAt = qint64(0);
					auto finishedAt = qint64(0);
					stream
						>> runBatchId
						>> runSessionId
						>> run.srcName
						>> run.destDir
						>> total
						>> done
						>> skipped
						>> startedAt
						>> finishedAt;
					if (stream.status() != QDataStream::Ok
						|| !runBatchId
						|| total <= 0
						|| total > 99'999
						|| done < 0
						|| done > total
						|| skipped < 0
						|| skipped > total
						|| startedAt < 0
						|| finishedAt < 0) {
						break;
					}
					run.batchId = runBatchId;
					run.sessionId = runSessionId;
					run.total = int(total);
					run.done = int(done);
					run.skipped = int(skipped);
					run.startedAt = TimeId(startedAt);
					run.finishedAt = TimeId(finishedAt);
					runs->push_back(std::move(run));
				}
			}
		}
		if (v3 && ulRuns && !stream.atEnd()) {
			auto runCount = qint32(0);
			stream >> runCount;
			if (stream.status() == QDataStream::Ok
				&& runCount >= 0
				&& runCount <= 999) {
				for (auto i = 0; i != runCount; ++i) {
					auto run = FinishedUlRun();
					auto runBatchId = quint64(0);
					auto runSessionId = quint64(0);
					auto total = qint32(0);
					auto done = qint32(0);
					auto skipped = qint32(0);
					auto startedAt = qint64(0);
					auto finishedAt = qint64(0);
					stream
						>> runBatchId
						>> runSessionId
						>> run.srcName
						>> run.destDir
						>> total
						>> done
						>> skipped
						>> startedAt
						>> finishedAt;
					if (stream.status() != QDataStream::Ok
						|| !runBatchId
						|| total <= 0
						|| total > 99'999
						|| done < 0
						|| done > total
						|| skipped < 0
						|| skipped > total
						|| startedAt < 0
						|| finishedAt < 0) {
						break;
					}
					run.batchId = runBatchId;
					run.sessionId = runSessionId;
					run.total = int(total);
					run.done = int(done);
					run.skipped = int(skipped);
					run.startedAt = TimeId(startedAt);
					run.finishedAt = TimeId(finishedAt);
					ulRuns->push_back(std::move(run));
				}
			}
		}
	}
	return result;
}

int DownloadManager::jobTotal(not_null<Main::Session*> session) const {
	const auto i = _sessions.find(session);
	if (i == end(_sessions)) {
		return 0;
	}
	const auto &data = i->second;
	auto result = 0;
	for (const auto &entry : data.downloading) {
		if (entry.jobIndex == data.jobId
			&& !entry.done
			&& !entry.enhancedForward) {
			++result;
		}
	}
	for (const auto &id : data.downloaded) {
		if (id.jobIndex == data.jobId) {
			++result;
		}
	}
	return result;
}

int DownloadManager::jobDone(not_null<Main::Session*> session) const {
	const auto i = _sessions.find(session);
	if (i == end(_sessions)) {
		return 0;
	}
	const auto &data = i->second;
	auto result = 0;
	for (const auto &id : data.downloaded) {
		if (id.jobIndex == data.jobId) {
			++result;
		}
	}
	return result;
}

int DownloadManager::jobId(not_null<Main::Session*> session) const {
	const auto i = _sessions.find(session);
	return (i == end(_sessions)) ? 0 : i->second.jobId;
}

bool DownloadManager::isDone(not_null<const HistoryItem*> item) const {
	return _loaded.contains(item) || _loadingDone.contains(item);
}

rpl::producer<> DownloadManager::jobCounterChanged() const {
	return _jobCounterChanged.events();
}

int DownloadManager::dlResumeCount() const {
	return ensureDedupDb().isOpen()
		? int(ensureDedupDb().loadAllDlResume().size())
		: 0;
}

QString DownloadManager::dedupDbPath() const {
	return cWorkingDir() + u"tdata/dedup.db"_q;
}

DedupDb &DownloadManager::dedupDb() const {
	return ensureDedupDb();
}

DedupDb &DownloadManager::ensureDedupDb() const {
	if (!_dedupDb) {
		const auto path = dedupDbPath();
		if (!path.isEmpty()) {
			_dedupDb = std::make_unique<DedupDb>(path);
		}
	}
	return *_dedupDb;
}

void DownloadManager::saveToDisk() {
}

void DownloadManager::saveIfIdle() {
}

void DownloadManager::fetchFingerprint(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document,
		Fn<void(QByteArray)> done) {
	const auto docId = document->id;
	const auto cached = _fingerprintCache.find(docId);
	if (cached != _fingerprintCache.end()) {
		done(cached.value());
		return;
	}
	Data::RemoteFileFingerprint(session, document, [=](QByteArray hash) {
		_fingerprintCache[docId] = hash;
		done(hash);
	});
}

void DownloadManager::fetchFingerprint(
		not_null<Main::Session*> session,
		not_null<PhotoData*> photo,
		Fn<void(QByteArray)> done) {
	Data::RemotePhotoFingerprint(session, photo, std::move(done));
}

void DownloadManager::clearFingerprintCache(uint64 documentId) {
	_fingerprintCache.remove(documentId);
}

void DownloadManager::clearFingerprintCache() {
	_fingerprintCache.clear();
}

void DownloadManager::saveFileHash(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document,
		int64 size) {
	const auto docId = document->id;
	auto &db = ensureDedupDb();
	if (!db.isOpen() || db.containsDocId(DedupDb::Table::Downloads, docId)) {
		return;
	}
	fetchFingerprint(session, document, [=, &db](QByteArray hash) {
		if (hash.isEmpty()) {
			return;
		}
		if (!_loadingDocuments.contains(document)) {
			return;
		}
		// Register this document's content so a concurrent download of the
		// same content (different doc id) can be skipped while we download.
		ensureDedupDb().insert(DedupDb::Table::Downloads, {
			.hash = hash,
			.documentId = docId,
			.status = u"u"_q,
		});
	});
}

void DownloadManager::removeFileHash(uint64 documentId, int64 size) {
	if (!documentId) {
		return;
	}
	_fingerprintCache.remove(documentId);
	ensureDedupDb().removeByDocumentId(
		DedupDb::Table::Downloads,
		documentId,
		u"u"_q);
}

void DownloadManager::checkDuplicate(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document,
		Fn<void(bool)> done,
		uint64 batchId) {
	if (!GetEnhancedBool("prevent_download_duplicates")) {
		if (!batchId) {
			done(false);
			return;
		}
		const auto sessionId = session->uniqueId();
		auto &tmpDb = ensureDedupDb();
		if (tmpDb.isOpen()
			&& tmpDb.containsDlTmpDocId(
				sessionId,
				batchId,
				document->id)) {
			done(true);
			return;
		}
		if (document->size < Data::kDedupMinPartialHashSize) {
			done(false);
			return;
		}
		_checksInProgress++;
		fetchFingerprint(session, document, [=, this](QByteArray hash) {
			_checksInProgress--;
			if (hash.isEmpty()) {
				done(false);
			} else {
				auto &db = ensureDedupDb();
				const auto skip = db.isOpen()
					&& db.containsDlTmpHash(sessionId, batchId, hash);
				if (db.isOpen()) {
					db.insertDlTmp(
						sessionId,
						batchId,
						document->id,
						hash);
				}
				done(skip);
			}
			saveIfIdle();
		});
		return;
	}
	_checksInProgress++;
	const auto size = document->size;
	auto wrappedDone = [=, this](bool skip) {
		_checksInProgress--;
		done(skip);
		saveIfIdle();
	};
	auto &db = ensureDedupDb();

	// Step 1: Check doc ID (O(1), catches same doc re-download).
	if (db.containsDocId(DedupDb::Table::Downloads, document->id)) {
		wrappedDone(true);
		return;
	}

	// Step 2: Small files / photos can't be sampled remotely: they are
	// downloaded first and deduplicated by full local hash after download
	// (in addLoaded). Just proceed.
	if (size < Data::kDedupMinPartialHashSize) {
		wrappedDone(false);
		return;
	}

	// Step 3: Fetch 2 sample chunks, compute the hash, compare with known
	// content (both finished and in-flight 'u' rows).
	fetchFingerprint(
		session,
		document,
		[=, &db](QByteArray hash) {
			if (hash.isEmpty()) {
				// Remote sampling failed (small file, no access, etc.) - the
				// full local hash after download will decide then.
				wrappedDone(false);
				return;
			}
			if (!db.isOpen() || !db.containsHash(DedupDb::Table::Downloads, hash)) {
				// No known content with this hash -> not a duplicate.
				wrappedDone(false);
				return;
			}
			// Same content already known. Remember the alias id -> hash so
			// future dedup of this exact doc id is O(1) without fetching.
			// Aliases must reference finished content only: one based on an
			// in-flight row would survive its cancel and block this content.
			if (db.containsFinishedHash(DedupDb::Table::Downloads, hash)) {
				ensureDedupDb().insert(DedupDb::Table::Downloads, {
					.hash = hash,
					.documentId = document->id,
					.status = u"f"_q,
				});
			}
			wrappedDone(true);
			return;
		});
}

void DownloadManager::commitDlTmpFile(
		uint64 sessionId,
		uint64 batchId,
		uint64 documentId,
		const QString &path) {
	if (!batchId
		|| !documentId
		|| GetEnhancedBool("prevent_download_duplicates")) {
		return;
	}
	const auto size = QFileInfo(path).size();
	if (size <= 0 || size >= Data::kDedupMinPartialHashSize) {
		return;
	}
	const auto hash = Data::FileFingerprint(path, size);
	if (hash.isEmpty()) {
		return;
	}
	auto &db = ensureDedupDb();
	if (db.isOpen()) {
		db.insertDlTmp(sessionId, batchId, documentId, hash);
	}
}

void DownloadManager::startAllResumeDownloads(bool startPaused) {
	auto &db = ensureDedupDb();
	const auto records = db.loadAllDlResume();
	if (records.empty()) {
		return;
	}
	struct FileJob {
		uint64 sessionId = 0;
		PeerId peerId;
		std::vector<ResumeEntry> entries;
	};
	auto jobs = std::make_shared<std::vector<FileJob>>();
	for (const auto &record : records) {
		if (!record.sessionId || !record.peerId || !record.msgId) {
			continue;
		}
		const auto peerId = PeerId(record.peerId);
		auto jobIt = ranges::find_if(*jobs, [&](const FileJob &job) {
			return job.sessionId == record.sessionId && job.peerId == peerId;
		});
		if (jobIt == end(*jobs)) {
			jobs->push_back({
				.sessionId = record.sessionId,
				.peerId = peerId,
			});
			jobIt = end(*jobs) - 1;
		}
		jobIt->entries.push_back({
			.msgId = MsgId(record.msgId),
			.path = record.path,
			.docId = record.docId,
		});
	}
	const auto findSession = [](uint64 sessionId) -> Main::Session* {
		for (const auto &account : Core::App().domain().orderedAccounts()) {
			const auto session = account->maybeSession();
			if (session && session->uniqueId() == sessionId) {
				return session;
			}
		}
		return nullptr;
	};
	for (const auto &job : *jobs) {
		const auto strong = findSession(job.sessionId);
		if (!strong) {
			continue;
		}
		auto ids = QVector<MTPInputMessage>();
		for (const auto &entry : job.entries) {
			ids.push_back(
				MTP_inputMessageID(MTP_int(entry.msgId.bare)));
		}
		const auto startDownloads = [=](
				not_null<Main::Session*> strong) {
			for (const auto &entry : job.entries) {
				const auto item = strong->data().message(
					job.peerId,
					entry.msgId);
				if (!item) {
					continue;
				}
			const auto media = item->media();
			const auto document = media
				? media->document()
				: nullptr;
			if (!document) {
				continue;
			}
			auto path = entry.path;
			if (entry.docId && entry.docId != uint64(document->id)) {
				// Message edited to a different file: never
				// resume into the stale prefix, renumber fresh.
				// Legacy rows (docId 0) keep the old trust.
				const auto slash = path.lastIndexOf('/');
				const auto dot = path.lastIndexOf('.');
				const auto base = (dot > slash) ? path.mid(0, dot) : path;
				const auto ext = (dot > slash) ? path.mid(dot) : QString();
				for (int i = 0; QFileInfo::exists(path); ++i) {
					path = base + u" (%1)"_q.arg(i + 2) + ext;
				}
				ensureDedupDb().insertDlResume({
					.sessionId = uint64(job.sessionId),
					.peerId = uint64(job.peerId.value),
					.msgId = entry.msgId.bare,
					.path = path,
					.fileSize = document->size,
					.docId = uint64(document->id),
				});
			}
			// Use a plain file loader so a paused-then-resumed
			// download restarts reliably - the streaming-reader
			// downloader can't be resumed after being paused before
			// its first part request.
			document->save(
				item->fullId(),
				path,
				LoadFromCloudOrLocal,
					false,
					true);
				if (document->loading()
					&& !document->loadingFilePath().isEmpty()) {
					addLoading({
						.item = item,
						.document = document,
					});
					if (startPaused) {
						pause(item);
					}
				}
			}
		};
		if (peerIsChannel(job.peerId)) {
			const auto channelId = peerToChannel(job.peerId);
			const auto live = strong->data().channelLoaded(channelId);
			const auto primaryHash = (live && live->accessHash() != 0)
				? uint64(live->accessHash())
				: 0;
			auto attempt = std::make_shared<Fn<void(uint64)>>();
			*attempt = [=](uint64 hash) {
				strong->api().request(MTPchannels_GetMessages(
					MTP_inputChannel(
						MTP_long(channelId.bare),
						MTP_long(hash)),
					MTP_vector<MTPInputMessage>(ids))
				).done([=](const MTPmessages_Messages &result) {
					if (const auto session = findSession(job.sessionId)) {
						const auto strong =
							not_null<Main::Session*>(session);
						strong->data().processExistingMessages(
							strong->data().channelLoaded(channelId),
							result);
						startDownloads(strong);
					}
				}).fail([=](const MTP::Error &error) {
					if (hash != 0
						&& error.type() == u"CHANNEL_INVALID"_q) {
						(*attempt)(0);
					}
				}).send();
			};
			(*attempt)(primaryHash);
		} else {
			strong->api().request(MTPmessages_GetMessages(
				MTP_vector<MTPInputMessage>(ids))
			).done([=](const MTPmessages_Messages &result) {
				if (const auto session = findSession(job.sessionId)) {
					const auto strong = not_null<Main::Session*>(session);
					strong->data().processExistingMessages(
						nullptr,
						result);
					startDownloads(strong);
				}
			}).send();
		}
	}
}

void DownloadManager::cancelAllResumeDownloads() {
	auto &db = ensureDedupDb();
	const auto records = db.loadAllDlResume();
	const auto findSession = [](uint64 sessionId) -> Main::Session* {
		for (const auto &account : Core::App().domain().orderedAccounts()) {
			const auto session = account->maybeSession();
			if (session && session->uniqueId() == sessionId) {
				return session;
			}
		}
		return nullptr;
	};
	for (const auto &record : records) {
		if (!record.sessionId || !record.peerId || !record.msgId) {
			continue;
		}
		const auto session = findSession(record.sessionId);
		if (!record.path.isEmpty()) {
			QFile(record.path).remove();
			if (session) {
				PruneEmptyDownloadFolders(session, record.path);
			}
		}
		ensureDedupDb().removeDlResume(
			record.sessionId,
			record.peerId,
			record.msgId);
	}
	_loadingListChanges.fire({});
}

void DownloadManager::untrack(not_null<Main::Session*> session) {
	const auto i = _sessions.find(session);
	Assert(i != end(_sessions));

	for (const auto &entry : i->second.downloaded) {
		if (const auto resolved = entry.object.get()) {
			const auto item = resolved->item;
			_loaded.remove(item);
			_generated.remove(item);
			if (const auto document = resolved->document) {
				_generatedDocuments.remove(document);
			}
		}
	}
	while (!i->second.downloading.empty()) {
		remove(i->second, i->second.downloading.end() - 1);
	}
	_sessions.erase(i);
}

rpl::producer<Ui::DownloadBarProgress> MakeDownloadBarProgress() {
	return [](auto consumer) {
		auto lifetime = rpl::lifetime();

		struct State {
			base::has_weak_ptr guard;
			bool scheduled = false;
			Fn<void()> push;
		};
		const auto state = lifetime.make_state<State>();

	const auto notify = [=] {
			DownloadProgress dlProgress;
			auto efProgress = DownloadProgress();
			auto &manager = Core::App().downloadManager();
			for (const auto id : manager.loadingList()) {
				if (id->jobIndex != manager.jobId(
					&id->object.item->history()->session())) {
					continue;
				}
				if (id->enhancedForward) {
					efProgress.ready += id->ready;
					efProgress.total += id->total;
					continue;
				}
				dlProgress.ready += id->ready;
				dlProgress.total += id->total;
			}
			if (dlProgress.total == 0) {
				for (const auto &account : Core::App().domain().orderedAccounts()) {
					if (const auto session = account->maybeSession()) {
						auto &db = Core::App().downloadManager().ensureDedupDb();
						if (!db.isOpen()) {
							continue;
						}
						for (const auto &record : db.loadAllDlResume()) {
							if (record.sessionId != session->uniqueId()) {
								continue;
							}
							qint64 total = record.fileSize;
							if (total <= 0) {
								const auto peerId = PeerId(record.peerId);
								const auto msgId = MsgId(record.msgId);
								if (const auto item = session->data().message(peerId, msgId)) {
									if (const auto media = item->media()) {
										if (const auto doc = media->document()) {
											total = doc->size;
										}
									}
								}
							}
							if (total <= 0) {
								continue;
							}
							const auto ready = QFileInfo::exists(record.path)
								? qint64(QFileInfo(record.path).size())
								: qint64(0);
							dlProgress.total += total;
							dlProgress.ready += std::min(ready, total);
						}
					}
				}
			}
			qint64 persistedReady = 0, persistedTotal = 0;
			for (const auto &account :
					Core::App().domain().orderedAccounts()) {
				if (const auto session = account->maybeSession()) {
					qint64 total = 0;
					persistedReady += EnhancedForward::PersistedForwardBytes(
						session,
						&total);
					persistedTotal += total;
				}
			}
			auto efReady = efProgress.ready;
			auto efTotalBytes = efProgress.total;
			if (efTotalBytes == 0) {
				efReady = persistedReady;
				efTotalBytes = persistedTotal;
			} else if (persistedTotal > efTotalBytes) {
				efTotalBytes = persistedTotal;
				efReady = std::max(efReady, persistedReady);
			}
			auto nfDone = 0;
			auto nfTotal = 0;
			auto nfSkipped = 0;
			auto nfFloodSeconds = 0;
			for (const auto &account :
					Core::App().domain().orderedAccounts()) {
				if (const auto session = account->maybeSession()) {
					for (const auto &counters
						: NormalForward::AllCounters(session)) {
						if (!counters.active) {
							continue;
						}
						nfDone += counters.done;
						nfTotal += counters.total;
						nfSkipped += counters.skipped;
						nfFloodSeconds = std::max(
							nfFloodSeconds,
							counters.floodSeconds);
					}
				}
			}
			{
				const auto copy = CopyAlbumProgress();
				nfDone = std::max(nfDone, copy.first);
				nfTotal = std::max(nfTotal, copy.second);
			}
			consumer.put_next(Ui::DownloadBarProgress{
				.ready = dlProgress.ready,
				.total = dlProgress.total,
				.efReady = efReady,
				.efTotal = efTotalBytes,
				.nfDone = nfDone,
				.nfTotal = nfTotal,
				.nfSkipped = nfSkipped,
				.nfFloodSeconds = nfFloodSeconds,
			});
		};

		state->push = [=] {
			if (state->scheduled) return;
			state->scheduled = true;
			Ui::PostponeCall(&state->guard, [=] {
				state->scheduled = false;
				notify();
			});
		};

		Core::App().downloadManager().loadingProgressValue(
		) | rpl::on_next([=](const DownloadProgress &) {
			state->push();
		}, lifetime);

		EnhancedForward::counterChanges(
		) | rpl::on_next([=] {
			state->push();
		}, lifetime);

		NormalForward::countersChanged(
		) | rpl::on_next([=] {
			state->push();
		}, lifetime);

		g_copyAlbumChanges.events(
		) | rpl::on_next([=] {
			state->push();
		}, lifetime);

		notify();
		return lifetime;
	};
}

rpl::producer<Ui::DownloadBarContent> MakeDownloadBarContent() {
	return [](auto consumer) {
		auto lifetime = rpl::lifetime();

		struct State {
			DocumentData *document = nullptr;
			std::shared_ptr<Data::DocumentMedia> media;
			rpl::lifetime downloadTaskLifetime;
			QImage thumbnail;

			base::has_weak_ptr guard;
			bool scheduled = false;
			Fn<void()> push;
			int efTotal = 0;
			int efDone = 0;
			int efSkipped = 0;
		};

		const auto state = lifetime.make_state<State>();
		auto &manager = Core::App().downloadManager();

		const auto computeEfPending = [=] {
			auto totalF = 0;
			auto doneF = 0;
			auto skippedF = 0;
			for (const auto &account :
					Core::App().domain().orderedAccounts()) {
				if (const auto session = account->maybeSession()) {
					for (const auto &job :
							EnhancedForward::GetUnfinishedJobs(session)) {
						totalF += job.total;
						doneF += job.sent;
					}
					for (const auto &job : EnhancedForward::AllJobs(session)) {
						skippedF += job.progress.skipped;
					}
				}
			}
			state->efTotal = totalF;
			state->efDone = doneF;
			state->efSkipped = skippedF;
		};
		computeEfPending();
		EnhancedForward::counterChanges(
		) | rpl::on_next([=] {
			computeEfPending();
			state->push();
		}, lifetime);

		const auto resolveThumbnailRecursive = [=](auto &&self) -> bool {
			if (state->document && !state->document->hasThumbnail()) {
				state->media = nullptr;
			}
			if (!state->media) {
				state->downloadTaskLifetime.destroy();
				if (!state->thumbnail.isNull()) {
					return false;
				}
				state->thumbnail = QImage();
				return true;
			}
			if (const auto image = state->media->thumbnail()) {
				state->thumbnail = image->original();
				state->downloadTaskLifetime.destroy();
				state->media = nullptr;
				return true;
			} else if (const auto embed = state->media->thumbnailInline()) {
				if (!state->thumbnail.isNull()) {
					return false;
				}
				state->thumbnail = Images::Prepare(embed->original(), 0, {
					.options = Images::Option::Blur,
				});
			} else if (!state->downloadTaskLifetime) {
				state->document->session().downloaderTaskFinished(
				) | rpl::filter([=] {
					return self(self);
				}) | rpl::on_next(
					state->push,
					state->downloadTaskLifetime);
			}
			return !state->thumbnail.isNull();
		};
		const auto resolveThumbnail = [=] {
			return resolveThumbnailRecursive(resolveThumbnailRecursive);
		};

		const auto notify = [=, &manager] {
			auto content = Ui::DownloadBarContent();
			auto single = (const Data::DownloadObject*) nullptr;
			for (const auto id : manager.loadingList()) {
				if (id->hiddenByView) {
					break;
				}
				if (id->enhancedForward) {
					continue;
				}
				if (id->jobIndex != manager.jobId(
					&id->object.item->history()->session())) {
					continue;
				}
				if (!single) {
					single = &id->object;
				}
				++content.count;
				if (id->done) {
					++content.done;
				}
			}
			if (content.count == 0) {
				int pausedCount = 0;
				for (const auto &account : Core::App().domain().orderedAccounts()) {
					if (const auto session = account->maybeSession()) {
						auto &db = Core::App().downloadManager().ensureDedupDb();
						if (!db.isOpen()) {
							continue;
						}
						for (const auto &record : db.loadAllDlResume()) {
							if (record.sessionId != session->uniqueId()) {
								continue;
							}
							++pausedCount;
						}
					}
				}
			if (pausedCount > 0) {
				content.count = pausedCount;
				content.done = 0;
			}
		}
		const auto [batchTotal, batchDone, batchSkipped]
			= manager.batchTotals();
		if (batchTotal > 0) {
			content.count = batchTotal;
			content.done = batchDone;
			content.dlTotal = batchTotal;
			content.dlSkipped = batchSkipped;
		}
		content.keepVisible = manager.hasHeldDlBatches();
			content.efCount = state->efTotal;
			content.efDone = state->efDone;
			content.efSkipped = state->efSkipped;
			for (const auto &account : Core::App().domain().orderedAccounts()) {
				if (const auto session = account->maybeSession()) {
					for (const auto &counters
						: NormalForward::AllCounters(session)) {
						if (!counters.active) {
							continue;
						}
						content.nfCount += counters.total;
						content.nfDone += counters.done;
						if (content.nfLastName.isEmpty()
							&& !counters.lastFileName.isEmpty()) {
							content.nfLastName = counters.lastFileName;
						}
						content.efSkipped += counters.skipped;
					}
				}
			}
			{
				const auto copy = CopyAlbumProgress();
				if (copy.second > content.nfCount) {
					content.nfCount = copy.second;
					content.nfDone = copy.first;
				}
			}
			if (content.count == 1 && single) {
				const auto document = single->document;
				const auto thumbnailed = (single->item
					&& document->hasThumbnail())
					? document
					: nullptr;
				if (state->document != thumbnailed) {
					state->document = thumbnailed;
					state->media = thumbnailed
						? thumbnailed->createMediaView()
						: nullptr;
					if (const auto raw = state->media.get()) {
						raw->thumbnailWanted(single->item->fullId());
					}
					state->thumbnail = QImage();
					resolveThumbnail();
				}
				content.singleName = Ui::Text::FormatDownloadsName(
					document);
				content.singleThumbnail = state->thumbnail;
			}
			consumer.put_next(std::move(content));
		};
		state->push = [=] {
			if (state->scheduled) {
				return;
			}
			state->scheduled = true;
			Ui::PostponeCall(&state->guard, [=] {
				state->scheduled = false;
				notify();
			});
		};

		manager.loadingListChanges(
		) | rpl::filter([=] {
			return !state->scheduled;
		}) | rpl::on_next(state->push, lifetime);

		NormalForward::countersChanged(
		) | rpl::on_next([=] {
			state->push();
		}, lifetime);

		g_copyAlbumChanges.events(
		) | rpl::on_next([=] {
			state->push();
		}, lifetime);

		notify();
		return lifetime;
	};
}

rpl::producer<Ui::DownloadBarProgress> MakeUploadBarProgress() {
	return [](auto consumer) {
		auto lifetime = rpl::lifetime();

		struct State {
			base::has_weak_ptr guard;
			bool scheduled = false;
			Fn<void()> push;
			rpl::lifetime uploadSubscriptions;
		};
		const auto state = lifetime.make_state<State>();

		const auto notify = [=] {
			auto ready = int64(0);
			auto total = int64(0);
			for (const auto &account : Core::App().domain().orderedAccounts()) {
				if (const auto session = account->maybeSession()) {
					for (const auto &u : session->uploader().activeUploads()) {
						if (EnhancedForward::isEnhancedUpload(u.itemId)) {
							continue;
						}
						ready += u.offset;
						total += u.total;
					}
				}
			}
			consumer.put_next(Ui::DownloadBarProgress{
				.ready = 0,
				.total = 0,
				.uploadReady = ready,
				.uploadTotal = total,
			});
		};

		state->push = [=] {
			if (state->scheduled) return;
			state->scheduled = true;
			Ui::PostponeCall(&state->guard, [=] {
				state->scheduled = false;
				notify();
			});
		};

		for (const auto &account : Core::App().domain().orderedAccounts()) {
			if (const auto session = account->maybeSession()) {
				session->uploader().loadingListChanges(
				) | rpl::on_next(state->push, lifetime);
				session->uploader().uploadProgressValue(
				) | rpl::on_next([=](const Storage::UploadFileProgress &) {
					state->push();
				}, lifetime);
			}
		}
		Core::App().domain().activeSessionChanges(
		) | rpl::on_next([state](Main::Session *session) {
			if (session) {
				session->uploader().loadingListChanges(
				) | rpl::on_next(state->push, state->uploadSubscriptions);
				session->uploader().uploadProgressValue(
				) | rpl::on_next([state](const Storage::UploadFileProgress &) {
					state->push();
				}, state->uploadSubscriptions);
			}
		}, lifetime);

		notify();
		return lifetime;
	};
}

rpl::producer<Ui::DownloadBarContent> MakeUploadBarContent() {
	return [](auto consumer) {
		auto lifetime = rpl::lifetime();
		struct State {
			base::has_weak_ptr guard;
			bool scheduled = false;
			Fn<void()> push;
			rpl::lifetime uploadSubscriptions;
		};
		const auto state = lifetime.make_state<State>();

		const auto notify = [=] {
			auto content = Ui::DownloadBarContent();
			auto totalQueue = int(0);
			auto totalPending = int(0);
			auto totalReady = int64(0);
			auto totalSize = int64(0);
			auto firstActiveName = QString();
			auto firstPendingName = QString();
			auto firstActiveReady = int64(0);
			auto firstActiveTotal = int64(0);
			auto firstPendingReady = int64(0);
			auto firstPendingTotal = int64(0);
			auto firstActiveFound = false;
			auto firstPendingFound = false;
			for (const auto &account : Core::App().domain().orderedAccounts()) {
				const auto session = account->maybeSession();
				if (!session) continue;
				const auto uploads = session->uploader().activeUploads();
				auto efActive = 0;
				for (const auto &u : uploads) {
					if (EnhancedForward::isEnhancedUpload(u.itemId)) {
						efActive++;
					}
				}
				const auto qSize = session->uploader().queueSize();
				const auto realQueue = qSize - efActive;
				totalQueue += realQueue;
				if (realQueue > 0) {
					if (!firstActiveFound) {
						firstActiveFound = true;
						firstActiveName = session->uploader().firstUploadName();
					}
					for (const auto &u : uploads) {
						if (EnhancedForward::isEnhancedUpload(u.itemId)) {
							continue;
						}
						totalReady += u.offset;
						totalSize += u.total;
					}
					if (realQueue == 1 && !firstActiveTotal) {
						for (const auto &u : uploads) {
							if (!EnhancedForward::isEnhancedUpload(u.itemId)) {
								firstActiveReady = u.offset;
								firstActiveTotal = u.total;
								break;
							}
						}
					}
				} else {
					const auto pending = session->uploader().pendingUploads();
					auto pSize = int(pending.size());
					for (const auto &p : pending) {
						if (EnhancedForward::isEnhancedUpload(p.itemId)
							|| p.path.contains(u"ForwardTemp"_q)) {
							pSize--;
						}
					}
					totalPending += pSize;
					if (pSize > 0) {
						auto addedPendingName = false;
						for (const auto &p : pending) {
							if (EnhancedForward::isEnhancedUpload(p.itemId)
								|| p.path.contains(u"ForwardTemp"_q)) {
								continue;
							}
							if (!firstPendingFound) {
								firstPendingFound = true;
								firstPendingName = p.filename;
							}
							totalReady += p.sent;
							totalSize += p.total;
							if (!addedPendingName) {
								addedPendingName = true;
								firstPendingReady = p.sent;
								firstPendingTotal = p.total;
							}
						}
					}
				}
			}
			content.uploadCount = totalQueue;
			if (!content.uploadCount) {
				content.uploadCount = totalPending;
			}
		auto jobTotal = 0;
		auto jobDone = 0;
		for (const auto &account : Core::App().domain().orderedAccounts()) {
			const auto session = account->maybeSession();
			if (!session) continue;
			jobTotal += session->uploader().jobTotal();
			jobDone += session->uploader().jobDone();
		}
		const auto [ulBatchTotal, ulBatchDone, ulBatchSkipped]
			= Core::App().downloadManager().uploadBatchTotals();
		if (jobTotal > 0
			&& (totalQueue > 0 || totalPending > 0 || ulBatchTotal > 0)) {
			content.uploadCount = jobTotal;
			content.uploadDone = jobDone;
		}
			if (content.uploadCount == 1) {
				const auto name = totalQueue
					? firstActiveName
					: firstPendingName;
				content.singleUploadName = name.isEmpty()
					? TextWithEntities()
					: tr::marked(name);
			}
		content.uploadReady = totalReady;
		content.uploadTotal = totalSize;
		if (ulBatchTotal > 0) {
				content.uploadCount = ulBatchTotal;
				content.uploadDone = ulBatchDone;
				content.ulTotal = ulBatchTotal;
				content.ulSkipped = ulBatchSkipped;
			}
			content.keepVisible
				= Core::App().downloadManager().hasHeldUlBatches();
			if (totalQueue == 0 && totalPending == 1) {
				content.uploadSingleReady = firstPendingReady;
				content.uploadSingleTotal = firstPendingTotal;
			} else if (totalQueue == 1 && totalPending == 0) {
				content.uploadSingleReady = firstActiveReady;
				content.uploadSingleTotal = firstActiveTotal;
			}
			consumer.put_next(std::move(content));
		};

		state->push = [=] {
			if (state->scheduled) return;
			state->scheduled = true;
			Ui::PostponeCall(&state->guard, [=] {
				state->scheduled = false;
				notify();
			});
		};

		for (const auto &account : Core::App().domain().orderedAccounts()) {
			if (const auto session = account->maybeSession()) {
				session->uploader().loadingListChanges(
				) | rpl::on_next(state->push, lifetime);
				session->uploader().uploadProgressValue(
				) | rpl::on_next([=](const Storage::UploadFileProgress &) {
					state->push();
				}, lifetime);
			}
		}
		Core::App().domain().activeSessionChanges(
		) | rpl::on_next([state](Main::Session *session) {
			if (session) {
				session->uploader().loadingListChanges(
				) | rpl::on_next(state->push, state->uploadSubscriptions);
				session->uploader().uploadProgressValue(
				) | rpl::on_next([state](const Storage::UploadFileProgress &) {
					state->push();
				}, state->uploadSubscriptions);
			}
		}, lifetime);
		Core::App().downloadManager().loadingListChanges(
		) | rpl::on_next(state->push, lifetime);

		notify();
		return lifetime;
	};
}

QStringList FilterUploadDuplicates(
		QStringList paths,
		std::optional<bool> sendImagesAsPhotos) {
	const auto dedupOff = !GetEnhancedBool(u"prevent_upload_duplicates"_q);
	static const auto imageSuffixes = {
		u"jpg"_q, u"jpeg"_q, u"png"_q, u"gif"_q, u"webp"_q,
		u"bmp"_q, u"tif"_q, u"tiff"_q, u"heic"_q, u"heif"_q, u"jxl"_q,
		u"avif"_q,
	};
	auto &db = Core::App().downloadManager().dedupDb();
	if (!dedupOff && !db.isOpen()) {
		return paths;
	}
	const auto sendLargePhotos = Core::App().settings()
		.sendFilesWay().sendLargePhotos();
	auto seen = base::flat_set<QByteArray>();
	auto duplicates = 0;
	auto result = QStringList();
	result.reserve(paths.size());
	const auto inSeenOrDb = [&](const QByteArray &hash) {
		return !hash.isEmpty()
			&& (seen.contains(hash)
				|| (!dedupOff
					&& db.containsHash(Data::DedupDb::Table::Uploads, hash)));
	};
	for (const auto &path : paths) {
		const auto size = QFileInfo(path).size();
		if (size <= 0) {
			result.push_back(path);
			continue;
		}
		if (ranges::contains(
			imageSuffixes,
			QFileInfo(path).suffix().toLower())) {
			// Photos are re-encoded before upload, so as photos they dedup by
			// the encoded bytes, as documents by the source bytes. Only drop
			// the file when the chosen mode matches a previous upload - when
			// the mode isn't known yet, require both modes to have seen it.
			const auto raw = Data::FileFingerprint(path, size);
			const auto encoded = (sendImagesAsPhotos == false)
				? QByteArray()
				: Data::ContentFingerprint(
					PreparePhotoUploadBytes(path, sendLargePhotos));
			const auto photo = inSeenOrDb(encoded);
			const auto document = inSeenOrDb(raw);
			auto duplicate = false;
			if (sendImagesAsPhotos == true) {
				duplicate = photo;
			} else if (sendImagesAsPhotos == false) {
				duplicate = document;
			} else {
				duplicate = photo && document;
			}
			if (!encoded.isEmpty()) {
				seen.emplace(encoded);
			}
			if (!raw.isEmpty()) {
				seen.emplace(raw);
			}
			if (duplicate) {
				duplicates++;
			} else {
				result.push_back(path);
			}
			continue;
		}
		const auto hash = Data::FileFingerprint(path, size);
		if (hash.isEmpty()) {
			result.push_back(path);
			continue;
		}
		const auto seenBefore = seen.contains(hash);
		seen.emplace(hash);
		if ((!dedupOff
			&& db.containsHash(Data::DedupDb::Table::Uploads, hash))
			|| seenBefore) {
			duplicates++;
		} else {
			result.push_back(path);
		}
	}
	if (duplicates > 0) {
		Core::App().downloadManager().reportDuplicateSkipped(
			Data::DedupDb::Table::Uploads,
			duplicates);
	}
	return result;
}

} // namespace Data
