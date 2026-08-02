/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_download_manager.h"

#include "data/data_file_hash.h"
#include "logs.h"
#include "settings.h"
#include "core/enhanced_settings.h"
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
#include "history/history.h"
#include "history/history_item.h"
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
#include "styles/style_layers.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>

namespace Data {
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

} // namespace

struct DownloadManager::DeleteFilesDescriptor {
	base::flat_set<not_null<Main::Session*>> sessions;
	base::flat_map<QString, DocumentDescriptor> files;
};

DownloadManager::DownloadManager()
: _clearLoadingTimer([=] { clearLoading(); }) {
}

DownloadManager::~DownloadManager() {
	_sessions.clear();
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
	data.downloaded = deserialize(session);
	data.resolveNeeded = data.downloaded.size();

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

void DownloadManager::addLoading(DownloadObject object) {
	Expects(object.item != nullptr);
	Expects(object.document != nullptr);

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
		data.downloading.push_back({
			.object = object,
			.started = computeNextStartDate(),
			.path = path,
			.ready = ready,
			.total = size,
			.hiddenByView = false,
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
			if (IsServerMsgId(item->id)) {
				const auto peer = item->history()->peer;
				ensureDedupDb().insertResumeDl({
					.peerId = peer->id.value,
					.peerAccessHash = PeerAccessHash(peer),
					.msgId = item->id.bare,
					.documentId = document->id,
					.size = size,
					.path = path,
				});
			}
		}

		if (GetEnhancedBool("prevent_download_duplicates")) {
			saveFileHash(
				&item->history()->session(),
				object.document,
				size);
		}

		check(item);
	};

	start();
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

	if (!ItemContainsMedia(entry.object)) {
		cancel(data, i);
		return;
	}
	const auto document = entry.object.document;

	// Load with progress only documents for now.
	Assert(document != nullptr);

	const auto path = document->filepath(true);
	if (!path.isEmpty()) {
		if (_loading.contains(entry.object.item)) {
			addLoaded(entry.object, path, entry.started);
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
		DownloadDate started) {
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
	const auto docId = object.document
		? object.document->id
		: (object.photo ? object.photo->id : 0);
	if (GetEnhancedBool("prevent_download_duplicates")) {
		loadFileHashes();
		const auto known = docId ? documentIds.find(docId) : documentIds.end();
		if (known != documentIds.end()) {
			// saveFileHash() already fetched and stored this exact
			// fingerprint from the server when the download started.
			// Re-reading the same 2 sample chunks off the just-finished
			// file here would just recompute the same hash a second time.
			dedupHash = known.value();
		} else {
			// No start-time record: dedup was off when this started, the
			// remote fetch failed, or the file was too small to sample
			// remotely (see kDedupMinPartialHashSize). FileFingerprint()
			// covers that last case with a full-file local hash, so it's
			// still worth trying here as a fallback.
			dedupHash = Data::FileFingerprint(path, size);
		}
	}
	// A hash match against our *own* unfinished record (the one saveFileHash
	// wrote for this same docId when the download started) is not a
	// duplicate, it's just this download recognizing itself - excluding it
	// here is required, otherwise every dedup-tracked download would delete
	// the file it just finished the moment it completed.
	const auto isDuplicateOfOther = [&] {
		if (dedupHash.isEmpty()) {
			return false;
		}
		if (const auto it = fileHashes.find(dedupHash); it != fileHashes.end()) {
			return (it.value().size == size) && (it.value().documentId != docId);
		}
		if (const auto it = pendingFileHashes.find(dedupHash); it != pendingFileHashes.end()) {
			return (it.value().size == size) && (it.value().documentId != docId);
		}
		return false;
	}();
	const auto isDuplicatePhotoById = [&] {
		if (!object.photo || !docId) {
			return false;
		}
		// The same photo id already has a dedup record (finished or still
		// unfinished), so the content is already known to be downloaded.
		return documentIds.contains(docId) || pendingDocumentIds.contains(docId);
	}();
	if (isDuplicateOfOther || isDuplicatePhotoById) {
		LOG(("DEDUP: addLoaded dup size=%1 path=%2").arg(
			size).arg(path));
		QFile::remove(path);
		if (const auto document = object.document) {
			removePendingDocument(document, size);
			ensureDedupDb().removeResumeDlByDocumentId(document->id);
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

	data.downloaded.push_back({
		.download = id,
		.started = started,
		.path = path,
		.size = size,
		.itemId = item->fullId(),
		.peerAccessHash = PeerAccessHash(item->history()->peer),
		.object = std::make_unique<DownloadObject>(object),
	});
	_loaded.emplace(item);
	_loadedAdded.fire(&data.downloaded.back());

	if (!dedupHash.isEmpty()) {
		loadFileHashes();
		if (!fileHashes.contains(dedupHash)) {
			fileHashes.insert(dedupHash, DedupEntry{ docId, size, DedupStatus::Finished });
			documentIds.insert(docId, dedupHash);
			fileSizes.insert(size);
			ensureDedupDb().insert(DedupDb::Table::Downloads, {
				.hash = dedupHash,
				.size = size,
				.documentId = docId,
				.status = u"f"_q,
			});
		} else if (fileHashes[dedupHash].status == DedupStatus::Unfinished) {
			fileHashes[dedupHash].status = DedupStatus::Finished;
			ensureDedupDb().insert(DedupDb::Table::Downloads, {
				.hash = dedupHash,
				.size = size,
				.documentId = docId,
				.status = u"f"_q,
			});
		}
		if (const auto document = object.document) {
			removePendingDocument(document, size);
		}
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
		saveIfIdle();
		if (const auto document = entry.object.document) {
			ensureDedupDb().removeResumeDlByDocumentId(document->id);
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

			const auto k = ranges::find(data.downloaded, item, ByItem);
			if (k != end(data.downloaded)) {
				const auto document = k->object->document;
				// The dedup record for this document is intentionally kept:
				// it only ever gets dropped for downloads that are cancelled
				// or removed before finishing (see remove()). Deleting the
				// already-downloaded file here doesn't undo the fact that we
				// downloaded this content once already.
				descriptor.files.emplace(k->path, DocumentDescriptor{
					.sessionUniqueId = id.sessionUniqueId,
					.documentId = document ? document->id : DocumentId(),
					.itemId = id.itemId,
				});
				_loaded.remove(item);
				_generated.remove(item);
				if (document) {
					_generatedDocuments.remove(document);
				}
				data.downloaded.erase(k);
				_loadedRemoved.fire_copy(item);

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
			Platform::File::MoveToTrash(file.first);
			crl::on_main([descriptor = file.second] {
				if (const auto session = SessionByUniqueId(
						descriptor.sessionUniqueId)) {
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

bool DownloadManager::loadedHasNonCloudFile() const {
	for (const auto &[session, data] : _sessions) {
		for (const auto &id : data.downloaded) {
			if (const auto object = id.object.get()) {
				if (!object->item->isHistoryEntry()) {
					return true;
				}
			}
		}
	}
	return false;
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

void DownloadManager::loadingStopWithConfirmation(
		Fn<void()> callback,
		Main::Session *onlyInSession) {
	const auto item = lookupLoadingItem(onlyInSession);
	if (!item) {
		return;
	}
	const auto window = Core::App().windowFor(
		not_null(&item->history()->session().account()));
	if (!window) {
		return;
	}
	const auto weak = base::make_weak(&item->history()->session());
	const auto id = item->fullId();
	auto box = Box([=](not_null<Ui::GenericBox*> box) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box.get(),
				tr::lng_download_sure_stop(),
				st::boxLabel),
			st::boxPadding + QMargins(0, 0, 0, st::boxPadding.bottom()));
		box->setStyle(st::defaultBox);
		box->addButton(tr::lng_selected_upload_stop(), [=] {
			box->closeBox();

			if (!onlyInSession || weak.get()) {
				loadingStop(onlyInSession);
			}
			if (callback) {
				callback();
			}
		}, st::attentionBoxButton);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		box->addLeftButton(tr::lng_upload_show_file(), [=] {
			box->closeBox();

			if (const auto strong = weak.get()) {
				if (const auto item = strong->data().message(id)) {
					if (const auto window = strong->tryResolveWindow()) {
						window->showMessage(item);
					}
				}
			}
		});
	});
	window->show(std::move(box));
	window->activate();
}

void DownloadManager::quitWithConfirmation(Fn<void()> quit) {
	const auto item = lookupLoadingItem(nullptr);
	if (!item) {
		if (quit) {
			quit();
		}
		return;
	}
	const auto window = Core::App().windowFor(
		not_null(&item->history()->session().account()));
	if (!window) {
		if (quit) {
			quit();
		}
		return;
	}
	auto loadingCount = 0;
	for ([[maybe_unused]] const auto &id : loadingList()) {
		++loadingCount;
	}
	auto box = Box([=](not_null<Ui::GenericBox*> box) {
		box->setCloseByOutsideClick(false);
		box->setCloseByEscape(false);
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box.get(),
				tr::lng_downloads_quit_confirm(
					tr::now,
					lt_count,
					loadingCount),
				st::boxLabel),
			st::boxPadding + QMargins(0, 0, 0, st::boxPadding.bottom()));
		box->setStyle(st::defaultBox);
		box->addButton(tr::lng_downloads_quit_cancel(), [=] {
			box->closeBox();
			auto confirmBox = Box([=](not_null<Ui::GenericBox*> confirmBox) {
				confirmBox->setCloseByOutsideClick(false);
				confirmBox->setCloseByEscape(false);
				confirmBox->addRow(object_ptr<Ui::FlatLabel>(
					confirmBox.get(),
					tr::lng_download_cancel_confirm(tr::now),
					st::boxLabel));
				confirmBox->addButton(tr::lng_download_cancel_yes(), [=] {
					confirmBox->closeBox();
					loadingStop(nullptr);
					if (quit) {
						quit();
					}
				});
				confirmBox->addButton(tr::lng_download_cancel_no(), [=] {
					confirmBox->closeBox();
				});
			});
			window->show(std::move(confirmBox));
		}, st::attentionBoxButton);
		box->addButton(tr::lng_downloads_quit_pause(), [=] {
			box->closeBox();
			pauseAll();
			if (quit) {
				quit();
			}
		});
		box->addButton(tr::lng_downloads_quit_continue(), [=] {
			box->closeBox();
		});
	});
	window->show(std::move(box));
	window->activate();
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
	auto &owner = session->data();
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
	auto &owner = session->data();
	for (; data.resolveSentTotal > 0; --data.resolveSentTotal) {
		const auto i = begin(data.downloaded) + (--data.resolveNeeded);
		if (i->path.isEmpty()) {
			data.downloaded.erase(i);
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
			removePendingDocument(document, i->total);
		}
	}
	data.downloading.erase(i);
	_loadingListChanges.fire({});
	_loadingProgress = now;
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

	remove(data, i);
	if (!item->isAdminLogEntry()) {
		if (document) {
			document->cancel();
		} else if (const auto photo = object.photo) {
			photo->cancel();
		}
	}
	if (document) {
		ensureDedupDb().removeResumeDlByDocumentId(document->id);
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
			tr::lng_download_cancel_confirm(tr::now),
			st::boxLabel));
		box->setStyle(st::defaultBox);
		box->addButton(tr::lng_download_cancel_yes(), [=] {
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
		box->addButton(tr::lng_download_cancel_no(), [=] {
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
		const auto i = ranges::find(data.downloaded, item.get(), ByItem);
		Assert(i != end(data.downloaded));

		const auto media = item->media();
		const auto photo = media ? media->photo() : nullptr;
		const auto document = media ? media->document() : nullptr;
		if (i->object->photo != photo || i->object->document != document) {
			detach(*i);
		}
	}
	if (_loading.contains(item) || _loadingDone.contains(item)) {
		check(item);
	}
}

void DownloadManager::removed(not_null<const HistoryItem*> item) {
	if (_loaded.contains(item)) {
		auto &data = sessionData(item);
		const auto i = ranges::find(data.downloaded, item.get(), ByItem);
		Assert(i != end(data.downloaded));
		detach(*i);
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

void DownloadManager::detach(DownloadedId &id) {
	Expects(id.object != nullptr);
	Expects(_loaded.contains(id.object->item));
	Expects(!_generated.contains(id.object->item));

	// Maybe generate new document?
	const auto was = id.object->item;
	const auto now = regenerateItem(*id.object);
	_loaded.remove(was);
	_loaded.emplace(now);
	id.object->item = now;

	_loadedRemoved.fire_copy(was);
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
		const auto count = data.downloaded.size();
		const auto constant = sizeof(quint64) // download.objectId
			+ sizeof(qint32) // download.type
			+ sizeof(qint64) // started
			+ sizeof(quint32) // size
			+ sizeof(quint64) // itemId.peer
			+ sizeof(qint64) // itemId.msg
			+ sizeof(quint64); // peerAccessHash
		auto size = sizeof(qint32) // count
			+ count * constant;
		for (const auto &id : data.downloaded) {
			size += Serialize::stringSize(id.path);
		}
		result.reserve(size);

		auto stream = QDataStream(&result, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		stream << qint32(count);
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
				<< id.path;
		}
		stream.device()->close();

		return result;
	};
}

std::vector<DownloadedId> DownloadManager::deserialize(
		not_null<Main::Session*> session) const {
	const auto serialized = session->account().local().downloadsSerialized();
	if (serialized.isEmpty()) {
		return {};
	}

	QDataStream stream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);

	auto count = qint32();
	stream >> count;
	if (stream.status() != QDataStream::Ok || count <= 0 || count > 99'999) {
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
		});
	}
	return result;
}

bool DownloadManager::hasUnfinishedResume(
		not_null<Main::Session*> session) const {
	return ensureDedupDb().isOpen()
		&& !ensureDedupDb().loadAllResumeDl().empty();
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

void DownloadManager::loadFileHashes() {
	if (_hasLoadedHashes) {
		return;
	}
	_hasLoadedHashes = true;
	auto &db = ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	auto records = db.loadAll(DedupDb::Table::Downloads);
	for (const auto &record : records) {
		if (record.hash.isEmpty()) {
			continue;
		}
		if (!fileHashes.contains(record.hash)) {
			fileHashes.insert(record.hash, DedupEntry{
				.documentId = record.documentId,
				.size = record.size,
				.status = (record.status == u"u"_q)
					? DedupStatus::Unfinished
					: DedupStatus::Finished,
			});
			documentIds.insert(record.documentId, record.hash);
			fileSizes.insert(record.size);
		}
	}
	LOG(("DEDUP: loadFileHashes loaded entries=%1").arg(fileHashes.size()));
}

void DownloadManager::saveToDisk() {
}

void DownloadManager::saveIfIdle() {
}

bool DownloadManager::hasFileSize(int64 size) const {
	return fileSizes.contains(size) || pendingFileSizes.contains(size);
}

bool DownloadManager::findDuplicateByDocumentId(uint64 documentId, int64 size) const {
	if (!documentId) {
		return false;
	}
	return documentIds.contains(documentId) || pendingDocumentIds.contains(documentId);
}

bool DownloadManager::findDuplicateByHash(int64 size, const QByteArray &hash) const {
	if (fileHashes.contains(hash)) {
		return fileHashes[hash].size == size;
	}
	if (pendingFileHashes.contains(hash)) {
		return pendingFileHashes[hash].size == size;
	}
	return false;
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
	loadFileHashes();
	const auto docId = document->id;
		if (documentIds.contains(docId)) {
			// Already has a record (unfinished or finished), most likely
			// restored from disk on startup together with a resumed download.
			// Don't kick off another remote fingerprint fetch for it.
			return;
		}
		fetchFingerprint(session, document, [=](QByteArray hash) {
			if (hash.isEmpty()) {
				return;
			}
			// The download could've been cancelled, finished or removed while
			// we were waiting for the server to answer with the sampled chunks.
			if (!_loadingDocuments.contains(document)) {
				return;
			}
			loadFileHashes();
			if (fileHashes.contains(hash)) {
				// Another download already registered this content (either as
				// an unfinished or a finished record), don't overwrite it.
				return;
			}
			fileHashes.insert(hash, DedupEntry{ docId, size, DedupStatus::Unfinished });
			documentIds.insert(docId, hash);
			fileSizes.insert(size);
			ensureDedupDb().insert(DedupDb::Table::Downloads, {
				.hash = hash,
				.size = size,
				.documentId = docId,
				.status = u"u"_q,
			});
	});
}

void DownloadManager::removeFileHash(uint64 documentId, int64 size) {
	if (!documentId) {
		return;
	}
	const auto it = documentIds.find(documentId);
	if (it == documentIds.end()) {
		return;
	}
	const auto hash = it.value();
	documentIds.erase(it);
	fileHashes.remove(hash);
	ensureDedupDb().remove(DedupDb::Table::Downloads, hash, size);

	auto sizeStillUsed = false;
	for (auto i = fileHashes.constBegin(); i != fileHashes.constEnd(); ++i) {
		if (i.value().size == size) {
			sizeStillUsed = true;
			break;
		}
	}
	if (!sizeStillUsed) {
		fileSizes.remove(size);
	}
}

void DownloadManager::addPendingDocument(
		not_null<DocumentData*> document,
		int64 size) {
	const auto docId = document->id;
	pendingDocumentIds.insert(docId, QByteArray());
	pendingFileSizes.insert(size);
	pendingDocuments.insert(docId, document);
	documentsAwaitingHash[size].push_back(docId);
}

void DownloadManager::removePendingDocument(
		not_null<DocumentData*> document,
		int64 size) {
	const auto docId = document->id;
	const auto hashIt = pendingDocumentIds.find(docId);
	if (hashIt != pendingDocumentIds.end() && !hashIt.value().isEmpty()) {
		pendingFileHashes.remove(hashIt.value());
	}
	pendingDocumentIds.remove(docId);
	pendingDocuments.remove(docId);
	_fingerprintCache.remove(docId);
	auto it = documentsAwaitingHash.find(size);
	if (it != documentsAwaitingHash.end()) {
		auto &vec = it.value();
		vec.erase(
			ranges::remove(vec, docId),
			vec.end());
		if (vec.empty()) {
			documentsAwaitingHash.erase(it);
		}
	}
	auto sizeStillUsed = documentsAwaitingHash.contains(size);
	if (!sizeStillUsed) {
		for (auto i = pendingFileHashes.constBegin();
				i != pendingFileHashes.constEnd();
				++i) {
			if (i.value().size == size) {
				sizeStillUsed = true;
				break;
			}
		}
	}
	if (!sizeStillUsed) {
		pendingFileSizes.remove(size);
	}
}

DocumentData* DownloadManager::findDocumentAwaitingHash(int64 size) {
	const auto it = documentsAwaitingHash.find(size);
	if (it == documentsAwaitingHash.end() || it.value().empty()) {
		return nullptr;
	}
	const auto docId = it.value().front();
	const auto docIt = pendingDocuments.find(docId);
	if (docIt == pendingDocuments.end()) {
		return nullptr;
	}
	return docIt.value().get();
}

void DownloadManager::updatePendingHash(
		not_null<DocumentData*> document,
		int64 size,
		const QByteArray &hash) {
	const auto docId = document->id;
	auto it = documentsAwaitingHash.find(size);
	if (it != documentsAwaitingHash.end()) {
		auto &vec = it.value();
		vec.erase(
			ranges::remove(vec, docId),
			vec.end());
		if (vec.empty()) {
			documentsAwaitingHash.erase(it);
		}
	}
	if (pendingDocumentIds.contains(docId)) {
		const auto oldHash = pendingDocumentIds[docId];
		if (!oldHash.isEmpty()) {
			pendingFileHashes.remove(oldHash);
		}
		pendingDocumentIds[docId] = hash;
	}
	if (!hash.isEmpty()) {
		pendingFileHashes.insert(hash, DedupEntry{ docId, size });
	}
}

void DownloadManager::checkDuplicate(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document,
		Fn<void(bool)> done) {
	if (!GetEnhancedBool("prevent_download_duplicates")) {
		done(false);
		return;
	}
	_checksInProgress++;
	const auto size = document->size;
	auto wrappedDone = [=, this](bool skip) {
		_checksInProgress--;
		done(skip);
		saveIfIdle();
	};
	loadFileHashes();

	// Step 1: Check doc ID (fastest - O(1), catches same doc re-download)
	if (findDuplicateByDocumentId(document->id, size)) {
		wrappedDone(true);
		return;
	}

	// Step 2: Check if size exists in DB or pending simultaneously
	if (!hasFileSize(size)) {
		addPendingDocument(document, size);
		wrappedDone(false);
		return;
	}

	// Step 3: Fetch chunks, compute hash, check hash in DB and pending simultaneously
	fetchFingerprint(
		session,
		document,
		[=](QByteArray hash) {
			loadFileHashes();
			const auto skip = !hash.isEmpty()
				&& findDuplicateByHash(document->size, hash);
			LOG(("DEDUP: checkDuplicate doc=%1 size=%2 hash=%3 skip=%4").arg(
				document->id
			).arg(
				document->size
			).arg(
				QString::fromLatin1(hash.toHex())
			).arg(
				skip));
			if (skip) {
				wrappedDone(true);
				return;
			}

			// Step 4: Cross-check with pending docs of same size without hash
			auto otherDoc = findDocumentAwaitingHash(document->size);
			if (otherDoc) {
				if (!hash.isEmpty()) {
					auto otherSession = &otherDoc->session();
					auto otherHash = std::make_shared<QByteArray>();
					fetchFingerprint(
						otherSession,
						otherDoc,
						[=](QByteArray h) mutable {
							*otherHash = h;
							if (!h.isEmpty()) {
								updatePendingHash(
									otherDoc,
									document->size,
									h);
							}
							if (h.isEmpty()
								|| hash.isEmpty()
								|| *otherHash != hash) {
								addPendingDocument(document, size);
								wrappedDone(false);
							} else {
								wrappedDone(true);
							}
						});
					return;
				}
			}

			addPendingDocument(document, size);
			wrappedDone(false);
		});
}

void DownloadManager::showResumeUnfinished(
		not_null<Main::Session*> session) {
	const auto records = ensureDedupDb().loadAllResumeDl();
	if (records.empty()) {
		return;
	}
	struct FileJob {
		PeerId peerId;
		uint64 peerAccessHash = 0;
		std::vector<ResumeEntry> entries;
	};
	auto jobs = std::make_shared<std::vector<FileJob>>();
	auto totalItems = 0;
	for (const auto &record : records) {
		if (!record.peerId || !record.msgId || !record.documentId) {
			continue;
		}
		const auto peerId = PeerId(record.peerId);
		auto jobIt = ranges::find(*jobs, peerId, &FileJob::peerId);
		if (jobIt == end(*jobs)) {
			jobs->push_back({
				.peerId = peerId,
				.peerAccessHash = record.peerAccessHash,
			});
			jobIt = end(*jobs) - 1;
		}
		jobIt->entries.push_back({
			.msgId = MsgId(record.msgId),
			.documentId = record.documentId,
			.size = record.size,
			.path = record.path,
		});
	}
	for (const auto &job : *jobs) {
		totalItems += int(job.entries.size());
	}
	if (jobs->empty()) {
		return;
	}

	const auto weak = base::make_weak(session);
	const auto resumeAllJobs = [=](bool startPaused) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		for (const auto &job : *jobs) {
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
					document->save(
						item->fullId(),
						entry.path);
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
				strong->api().request(MTPchannels_GetMessages(
					MTP_inputChannel(
						MTP_long(peerToChannel(job.peerId).bare),
						MTP_long(job.peerAccessHash)),
					MTP_vector<MTPInputMessage>(ids))
				).done([=](const MTPmessages_Messages &result) {
					if (const auto strong = weak.get()) {
						strong->data().processExistingMessages(
							strong->data().channelLoaded(
								peerToChannel(job.peerId)),
							result);
						startDownloads(strong);
					}
				}).send();
			} else {
				strong->api().request(MTPmessages_GetMessages(
					MTP_vector<MTPInputMessage>(ids))
				).done([=](const MTPmessages_Messages &result) {
					if (const auto strong = weak.get()) {
						strong->data().processExistingMessages(
							nullptr,
							result);
						startDownloads(strong);
					}
				}).send();
			}
		}
	};
	const auto cancelAllJobs = [=] {
		for (const auto &job : *jobs) {
			for (const auto &entry : job.entries) {
				if (!entry.path.isEmpty()) {
					QFile(entry.path).remove();
					if (const auto strong = weak.get()) {
						PruneEmptyDownloadFolders(strong, entry.path);
					}
				}
				ensureDedupDb().removeResumeDlByDocumentId(
					entry.documentId);
			}
		}
	};

	const auto window = Core::App().windowFor(
		not_null(&session->account()));
	if (!window) {
		return;
	}
	const auto openPanel = [=] {
		crl::on_main(crl::guard(weak, [=] {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			const auto window = Core::App().windowFor(
				not_null(&strong->account()));
			if (!window) {
				return;
			}
			if (const auto controller = window->sessionController()) {
				controller->showSection(
					Info::Downloads::Make(strong->user()));
			}
		}));
	};
	auto box = Box([=](not_null<Ui::GenericBox*> box) {
		box->setCloseByOutsideClick(false);
		box->setCloseByEscape(false);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box.get(),
			tr::lng_downloads_resume_unfinished(
				tr::now,
				lt_count,
				totalItems),
			st::boxLabel));
		box->addButton(tr::lng_downloads_resume_cancel(), [=] {
			box->closeBox();
			auto confirmBox = Box([=](not_null<Ui::GenericBox*> confirmBox) {
				confirmBox->setCloseByOutsideClick(false);
				confirmBox->setCloseByEscape(false);
				confirmBox->addRow(object_ptr<Ui::FlatLabel>(
					confirmBox.get(),
					tr::lng_download_cancel_confirm(tr::now),
					st::boxLabel));
				confirmBox->addButton(tr::lng_download_cancel_yes(), [=] {
					confirmBox->closeBox();
					crl::on_main(crl::guard(weak, [=] {
						cancelAllJobs();
					}));
				});
				confirmBox->addButton(tr::lng_download_cancel_no(), [=] {
					confirmBox->closeBox();
				});
			});
			window->show(std::move(confirmBox));
		}, st::attentionBoxButton);
		box->addButton(tr::lng_downloads_resume_later(), [=] {
			box->closeBox();
			crl::on_main(crl::guard(weak, [=] {
				resumeAllJobs(true);
			}));
		});
		box->addButton(tr::lng_downloads_resume_yes(), [=] {
			box->closeBox();
			crl::on_main(crl::guard(weak, [=] {
				resumeAllJobs(false);
				openPanel();
			}));
		});
	});
	window->show(std::move(box));
	window->activate();
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
			const auto downloadProgress = Core::App().downloadManager().loadingProgress();
			consumer.put_next(Ui::DownloadBarProgress{
				.ready = downloadProgress.ready,
				.total = downloadProgress.total,
				.uploadReady = 0,
				.uploadTotal = 0,
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
		};

		const auto state = lifetime.make_state<State>();
		auto &manager = Core::App().downloadManager();

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
				if (!single) {
					single = &id->object;
				}
				++content.count;
				if (id->done) {
					++content.done;
				}
			}
			if (content.count == 1) {
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
				) | rpl::on_next([=](const Storage::UploadProgress &) {
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
				) | rpl::on_next([state](const Storage::UploadProgress &) {
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
				const auto qSize = session->uploader().queueSize();
				totalQueue += qSize;
				if (qSize > 0) {
					if (!firstActiveFound) {
						firstActiveFound = true;
						firstActiveName = session->uploader().firstUploadName();
					}
					const auto uploads = session->uploader().activeUploads();
					for (const auto &u : uploads) {
						totalReady += u.offset;
						totalSize += u.total;
					}
					if (uploads.size() == 1 && !firstActiveTotal) {
						firstActiveReady = uploads[0].offset;
						firstActiveTotal = uploads[0].total;
					}
				} else {
					const auto pending = session->uploader().pendingUploads();
					const auto pSize = int(pending.size());
					totalPending += pSize;
					if (pSize > 0) {
						if (!firstPendingFound) {
							firstPendingFound = true;
							firstPendingName = pending[0].filename;
						}
						for (const auto &p : pending) {
							totalReady += p.sent;
							totalSize += p.total;
						}
						if (pending.size() == 1 && !firstPendingTotal) {
							firstPendingReady = pending[0].sent;
							firstPendingTotal = pending[0].total;
						}
					}
				}
			}
			content.uploadCount = totalQueue;
			if (!content.uploadCount) {
				content.uploadCount = totalPending;
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
				) | rpl::on_next([=](const Storage::UploadProgress &) {
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
				) | rpl::on_next([state](const Storage::UploadProgress &) {
					state->push();
				}, state->uploadSubscriptions);
			}
		}, lifetime);

		notify();
		return lifetime;
	};
}

} // namespace Data
