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

namespace Info::Downloads {
namespace {

using namespace Media;

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
	}
	return isUploadItem(item->getItem())
		? tr::lng_uploads_section(tr::now)
		: tr::lng_downloads_section(tr::now);
}

bool Provider::sectionItemBelongsHere(
		not_null<const BaseLayout*> item,
		not_null<const BaseLayout*> previous) {
	if (!_showGroupHeaders) {
		return true;
	}
	return isUploadItem(item->getItem())
		== isUploadItem(previous->getItem());
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
	_hasDownloads = !_downloading.empty() || !_downloaded.empty();
	_hasUploads = !_uploading.empty() || !_uploaded.empty();
}

void Provider::updateCounter() {
	const auto wantDownloads = (_filter == Filter::Downloads
		|| _filter == Filter::All);
	const auto wantUploads = (_filter == Filter::Uploads
		|| _filter == Filter::All);
	const auto wantForwards = (_filter == Filter::Forwards);
	auto done = 0;
	auto total = 0;
	if (wantForwards) {
		const auto session = &_controller->session();
		using ForwardJob = EnhancedForward::JobSnapshot;
		const auto jobs = EnhancedForward::AllJobs(session);
		const ForwardJob *current = nullptr;
		for (const auto &job : jobs) {
			if (job.active) {
				current = &job;
				break;
			}
		}
		if (!current) {
			for (const auto &job : jobs) {
				if (job.finished || job.resumable) {
					current = &job;
					break;
				}
			}
		}
		if (current) {
			for (const auto &item : current->progress.items) {
				if (item.cancelled) {
					continue;
				}
				++total;
				if (item.sent
					|| (item.state == EnhancedForward::ItemState::Done)) {
					++done;
				}
			}
		}
	}
	if (wantDownloads) {
		const auto session = &_controller->session();
		auto &manager = Core::App().downloadManager();
		total += manager.jobTotal(session);
		done += manager.jobDone(session);
	}
	if (wantUploads) {
		const auto session = &_controller->session();
		total += session->uploader().jobTotal();
		done += session->uploader().jobDone();
	}
	if (total == 0) {
		_counterText = QString();
		return;
	}
	_counterText = tr::lng_tm_counter(
		tr::now,
		lt_done, QString::number(done),
		lt_total, QString::number(total));
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
				remove(item);
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
			for (const auto &info : session->uploader().activeUploads()) {
				const auto item = session->data().message(info.itemId);
				if (!item) continue;
				if (EnhancedForward::isEnhancedUpload(info.itemId)
					|| EnhancedForward::isEnhancedTempUpload(
						session,
						info.filename)) {
					continue;
				}
			if (!copy.remove(info.itemId)
				&& !_uploaded.contains(info.itemId)
				&& !_downloading.contains(item)
				&& !_enhancedForward.contains(item)
				&& !_downloaded.contains(item)) {
					_uploading.emplace(info.itemId);
					addElementNow({
						.item = item,
						.started = int64(item->date()) * 1000,
						.path = info.filename,
					});
					refreshPostponed(true);
				}
			}
			for (const auto &left : copy) {
				_uploading.remove(left);
				_uploaded.emplace(left);
			}
			if (!copy.empty()) {
				refreshPostponed(false);
			}
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
					});
				continue;
			}
			addFinishedUpload(session, info.itemId, {
				.session = session,
				.started = info.started,
				.path = info.filename,
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
				addElementNow({
					.item = item,
					.started = int64(item->date()) * 1000,
					.path = QString(),
				});
				refreshPostponed(true);
			}
		}, _lifetime);

		session->uploader().finishedUploadsCleared(
		) | rpl::on_next([=] {
			const auto uploadedCopy = _uploaded;
			const auto uploadingCopy = _uploading;
			for (const auto &itemId : uploadedCopy) {
				_uploaded.remove(itemId);
			}
			for (const auto &itemId : uploadingCopy) {
				_uploading.remove(itemId);
			}
			for (auto i = _elements.begin(); i != _elements.end();) {
				const auto id = i->item->fullId();
				if (uploadedCopy.contains(id)
					|| uploadingCopy.contains(id)) {
					i = _elements.erase(i);
				} else {
					++i;
				}
			}
			for (auto it = _layouts.begin(); it != _layouts.end();) {
				const auto id = it->first->fullId();
				if (uploadedCopy.contains(id)
					|| uploadingCopy.contains(id)) {
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

		const auto refreshEF = std::make_shared<Fn<void()>>();
		const auto failedResolve = std::make_shared<base::flat_set<FullMsgId>>();
		*refreshEF = [=, refreshEF = refreshEF.get()] {
			auto jobItems = base::flat_set<not_null<const HistoryItem*>>();
			auto unresolved = std::vector<FullMsgId>();
			for (const auto &job : EnhancedForward::AllJobs(session)) {
				if (job.progress.state == EnhancedForward::State::Cancelled) {
					continue;
				}
				for (auto i = 0; i < int(job.progress.sourceIds.size()); i++) {
					if (int(job.progress.items.size()) > i
						&& job.progress.items[i].cancelled) {
						continue;
					}
					const auto srcId = job.progress.sourceIds[i];
					const auto message = session->data().message(srcId);
					if (!message) {
						if (!failedResolve->contains(srcId)) {
							unresolved.push_back(srcId);
						}
						continue;
					}
					const auto item = not_null<HistoryItem*>(message);
					jobItems.emplace(item);
					if (_enhancedForward.contains(item)) {
						continue;
					}
					_enhancedForward.emplace(item);
					const auto alreadyInElements = ranges::any_of(
						_elements,
						[&](const Element &element) {
							return element.item == item;
						});
					if (!alreadyInElements) {
						addElementNow(Element{
							item,
							int64(item->date()) * 1000,
							QString(),
						});
					}
					trackItemSession(item);
					refreshPostponed(true);
				}
			}
			auto toRemove = std::vector<not_null<const HistoryItem*>>();
			auto &downloadManager = Core::App().downloadManager();
			for (const auto &item : _enhancedForward) {
				if (jobItems.contains(item)) {
					continue;
				}
				const auto stillLoading = ranges::any_of(
					downloadManager.loadingList(),
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
				remove(item);
			}
			if (!unresolved.empty()) {
				const auto weak = base::make_weak(this);
				EnhancedForward::EnsureForwardSourceMessages(
					session,
					unresolved,
					[weak, refreshEF, failedResolve, unresolved](bool ok) {
						if (ok) {
							for (const auto &id : unresolved) {
								failedResolve->erase(id);
							}
						} else {
							for (const auto &id : unresolved) {
								failedResolve->emplace(id);
							}
						}
						if (const auto alive = weak.get()) {
							(*refreshEF)();
						}
					});
			}
		};
		rpl::single(rpl::empty) | rpl::then(
			EnhancedForward::stateChanges() | rpl::to_empty
		) | rpl::on_next([refreshEF] {
			(*refreshEF)();
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
		});
		refreshPostponed(true);
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
	trackItemSession(item);
	const auto i = ranges::find(_addPostponed, item, &Element::item);
	if (i != end(_addPostponed)) {
		i->path = entry->path;
		i->started = entry->started;
	} else {
		_addPostponed.push_back({
			.item = item,
			.started = entry->started,
			.path = entry->path,
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
		_layouts.erase(i);
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
		ranges::stable_sort(_elements, ranges::less(), &Element::started);
	}
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
	}, lifetime);
}

rpl::producer<> Provider::refreshed() {
	return _refreshed.events();
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
	const auto matches = [&](not_null<const HistoryItem*> item) {
		const auto ef = isEf(item);
		if (_filter == Filter::Forwards) {
			return ef;
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
		for (const auto &element : ranges::views::reverse(_elements)) {
			if (search && !element.found) {
				continue;
			}
			const auto layout = getLayout(element, delegate);
			if (!layout) {
				continue;
			}
			if (isUploadItem(element.item)) {
				uploads.addItem(layout);
			} else {
				downloads.addItem(layout);
			}
		}
		downloads.finishSection();
		uploads.finishSection();
		if (!downloads.empty()) {
			result.push_back(std::move(downloads));
		}
		if (!uploads.empty()) {
			result.push_back(std::move(uploads));
		}
	} else {
		auto section = ListSection(Type::File, sectionDelegate());
		const auto forward = (_filter == Filter::Forwards);
		for (auto i = 0; i != int(_elements.size()); ++i) {
			const auto &element = _elements[forward
				? i
				: int(_elements.size() - 1 - i)];
			if (search && !element.found) {
				continue;
			} else if (!matches(element.item)) {
				continue;
			} else if (auto layout = getLayout(element, delegate)) {
				section.addItem(layout);
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
	const auto getFile = [&]() -> DocumentData* {
		if (auto media = element.item->media()) {
			return media->document();
		}
		return nullptr;
	};

	using namespace Overview::Layout;
	auto &songSt = st::overviewFileLayout;
	if (const auto file = getFile()) {
		return std::make_unique<Document>(
			delegate,
			element.item,
			DocumentFields{
				.document = file,
				.dateOverride = Data::DateFromDownloadDate(element.started),
				.forceFileLayout = true,
			},
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
