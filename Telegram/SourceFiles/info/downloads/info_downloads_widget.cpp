/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/downloads/info_downloads_widget.h"

#include "info/downloads/info_downloads_inner_widget.h"
#include "info/info_controller.h"
#include "info/info_memento.h"
#include "ui/boxes/confirm_box.h"
#include "ui/search_field_controller.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/shadow.h"
#include "ui/ui_utility.h"
#include "data/data_download_manager.h"
#include "data/data_user.h"
#include "enhanced_forward.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "storage/file_upload.h"
#include "core/application.h"
#include "lang/lang_keys.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"

namespace Info::Downloads {

Memento::Memento(not_null<Controller*> controller)
: ContentMemento(Tag{})
, _media(controller) {
}

Memento::Memento(not_null<UserData*> self)
: ContentMemento(Tag{})
, _media(self, 0, Media::Type::File) {
}

Memento::~Memento() = default;

Section Memento::section() const {
	return Section(Section::Type::Downloads);
}

object_ptr<ContentWidget> Memento::createWidget(
		QWidget *parent,
		not_null<Controller*> controller,
		const QRect &geometry) {
	auto result = object_ptr<Widget>(parent, controller);
	result->setInternalState(geometry, this);
	return result;
}

Widget::Widget(
	QWidget *parent,
	not_null<Controller*> controller)
: ContentWidget(parent, controller) {
	_inner = setInnerWidget(object_ptr<InnerWidget>(
		this,
		controller));
	_inner->setScrollHeightValue(scrollHeightValue());
	_inner->scrollToRequests(
	) | rpl::on_next([this](Ui::ScrollToRequest request) {
		scrollTo(request);
	}, _inner->lifetime());
	setupTabs();
}

void Widget::setupTabs() {
	_tabs = object_ptr<Ui::SettingsSlider>(this, st::downloadsTabsSlider);
	_tabsShadow = object_ptr<Ui::PlainShadow>(this);

	const auto tabsHeight = _tabs->st().height;
	widthValue(
	) | rpl::on_next([=](int width) {
		_tabs->resizeToWidth(width);
		_tabs->moveToLeft(0, 0);
		_tabsShadow->setGeometry(0, tabsHeight, width, st::lineWidth);
	}, lifetime());

	_tabs->sectionActivated(
	) | rpl::on_next([=](int index) {
		if (index >= 0 && index < int(_tabList.size())) {
			applyTab(_tabList[index], false);
		}
	}, _tabs->lifetime());

	_tabList = { Tab::Downloads, Tab::Uploads, Tab::Forwards, Tab::Both };
	rebuildTabSections();
	_tabsShown = true;
	setScrollTopSkip(_tabs->st().height + st::lineWidth);
	applyTab(_currentTab, true);

	rpl::combine(
		_inner->hasDownloadsValue(),
		_inner->hasUploadsValue()
	) | rpl::on_next([=](bool hasDownloads, bool hasUploads) {
		_hasDownloads = hasDownloads;
		_hasUploads = hasUploads;
		refreshTabs();
	}, lifetime());
}

void Widget::rebuildTabSections() {
	if (_tabList.empty()) {
		return;
	}
	auto labels = std::vector<QString>();
	labels.reserve(_tabList.size());
	for (const auto tab : _tabList) {
		labels.push_back((tab == Tab::Uploads)
			? tr::lng_uploads_section(tr::now)
			: (tab == Tab::Forwards)
			? tr::lng_forwards_section(tr::now)
			: (tab == Tab::Both)
			? tr::lng_downloads_tab_all(tr::now)
			: tr::lng_downloads_section(tr::now));
	}
	_tabs->setSections(labels);
}

void Widget::refreshTabs() {
	const auto tab = _currentTab;
	if (std::find(_tabList.begin(), _tabList.end(), tab)
		== _tabList.end()) {
		_currentTab = _tabList.front();
	}
	applyTab(_currentTab, true);
}

void Widget::applyTab(Tab tab, bool updateSlider) {
	_currentTab = tab;
	_inner->setFilter(tab);
	if (updateSlider) {
		const auto i = std::find(_tabList.begin(), _tabList.end(), tab);
		if (i != _tabList.end()) {
			const auto index = int(i - _tabList.begin());
			if (_tabs->activeSection() != index) {
				_tabs->setActiveSectionFast(index);
			}
		}
	}
}

bool Widget::showInternal(not_null<ContentMemento*> memento) {
	if (auto downloadsMemento = dynamic_cast<Memento*>(memento.get())) {
		restoreState(downloadsMemento);
		return true;
	}
	return false;
}

void Widget::setInternalState(
		const QRect &geometry,
		not_null<Memento*> memento) {
	setGeometry(geometry);
	Ui::SendPendingMoveResizeEvents(this);
	restoreState(memento);
}

std::shared_ptr<ContentMemento> Widget::doCreateMemento() {
	auto result = std::make_shared<Memento>(controller());
	saveState(result.get());
	return result;
}

void Widget::saveState(not_null<Memento*> memento) {
	memento->setTab(_currentTab);
	memento->setScrollTop(scrollTopSave());
	_inner->saveState(memento);
}

void Widget::restoreState(not_null<Memento*> memento) {
	_currentTab = memento->tab();
	_inner->restoreState(memento);
	scrollTopRestore(memento->scrollTop());
	refreshTabs();
}

rpl::producer<SelectedItems> Widget::selectedListValue() const {
	return _inner->selectedListValue();
}

void Widget::selectionAction(SelectionAction action) {
	_inner->selectionAction(action);
}

void Widget::fillTopBarMenu(const Ui::Menu::MenuCallback &addAction) {
	const auto window = controller()->parentController();
	auto &loadingManager = Core::App().downloadManager();
	const auto showDownloads = (_currentTab == Tab::Downloads) && _hasDownloads;
	const auto showUploads = (_currentTab == Tab::Uploads) && _hasUploads;

	const auto downloadsRunning = loadingManager.anyResumable();
	const auto downloadsPaused = loadingManager.anyPaused();
	const auto downloadsFinished = loadingManager.anyFinishedLoading();
	const auto uploadsActive = Core::App().uploaderAny();
	const auto uploadsPaused = Core::App().uploaderAnyPaused();
	const auto uploadsOnlyOnDisk = !uploadsActive
		&& (Core::App().uploaderPendingResumeCount() > 0);
	const auto uploadsFinished = Core::App().uploaderAnyFinished();

	auto forwardsActive = false;
	auto forwardsPaused = false;
	auto forwardsFinished = false;
	if (_currentTab == Tab::Forwards || _currentTab == Tab::Both) {
		const auto session = &window->session();
		for (const auto &job : EnhancedForward::AllJobs(session)) {
			if (job.active) {
				forwardsActive = true;
				if (job.progress.state == EnhancedForward::State::Paused) {
					forwardsPaused = true;
				}
			} else if (job.finished) {
				forwardsFinished = true;
			}
		}
	}

	const auto finishedDownloadIds = [] {
		auto result = std::vector<GlobalMsgId>();
		auto &manager = Core::App().downloadManager();
		for (const auto &id : manager.loadedList()) {
			if (id->object) {
				result.push_back({
					.itemId = id->itemId,
					.sessionUniqueId = id->object->item->history()
						->session().uniqueId(),
				});
			}
		}
		return result;
	};

	if (_currentTab == Tab::Forwards) {
		if (forwardsPaused) {
			addAction(
				tr::lng_tm_fw_resume(tr::now),
				[=] {
					Ui::PostponeCall(this, [=] {
						const auto session = &window->session();
						for (const auto &job : EnhancedForward::AllJobs(session)) {
							if (job.active
								&& job.progress.state
									== EnhancedForward::State::Paused) {
								EnhancedForward::resumeForward(job.peer, session);
							}
						}
					});
				},
				&st::menuIconDownload);
		} else if (forwardsActive) {
			addAction(
				tr::lng_tm_fw_pause(tr::now),
				[=] {
					Ui::PostponeCall(this, [=] {
						const auto session = &window->session();
						for (const auto &job : EnhancedForward::AllJobs(session)) {
							if (job.active) {
								EnhancedForward::pauseForward(job.peer, session);
							}
						}
					});
				},
				&st::menuIconSchedule);
		}
		if (forwardsActive || forwardsPaused) {
			addAction(
				tr::lng_tm_fw_cancel(tr::now),
				[=] {
					window->show(Ui::MakeConfirmBox({
						.text = tr::lng_enhanced_forward_cancel_all_confirm(tr::now),
						.confirmed = [=](Fn<void()> close) {
							close();
							Ui::PostponeCall(this, [=] {
								EnhancedForward::CancelAll(&window->session());
							});
						},
						.confirmText = tr::lng_box_yes(tr::now),
						.cancelText = tr::lng_box_no(tr::now),
						.confirmStyle = &st::attentionBoxButton,
					}));
				},
				&st::menuIconCancel);
		}
		if (forwardsFinished) {
			addAction(
				tr::lng_tm_fw_clear(tr::now),
				[=] {
					Ui::PostponeCall(this, [=] {
						const auto session = &window->session();
						for (const auto &job : EnhancedForward::AllJobs(session)) {
							if (job.finished) {
								EnhancedForward::ClearFinished(session, job.peer);
							}
						}
					});
				},
				&st::menuIconClear);
		}
		return;
	}

	if (_currentTab == Tab::Both) {
		if (!_hasDownloads && !_hasUploads
			&& !forwardsActive && !forwardsPaused && !forwardsFinished) {
			return;
		}
		const auto hasAnyRunning = downloadsRunning
			|| uploadsActive
			|| forwardsActive;
		const auto hasAnyPaused = downloadsPaused
			|| uploadsPaused
			|| uploadsOnlyOnDisk
			|| forwardsPaused;
		const auto hasAnyFinished = downloadsFinished
			|| uploadsFinished
			|| forwardsFinished;
		if (hasAnyRunning && !hasAnyPaused) {
			addAction(
				tr::lng_tm_all_pause(tr::now),
				[=] {
					Ui::PostponeCall(this, [=] {
						Core::App().downloadManager().pauseAll();
						Core::App().uploaderPauseAll();
						const auto session = &window->session();
						for (const auto &job : EnhancedForward::AllJobs(session)) {
							if (job.active) {
								EnhancedForward::pauseForward(job.peer, session);
							}
						}
					});
				},
				&st::menuIconSchedule);
		} else if (hasAnyPaused) {
			addAction(
				tr::lng_tm_all_resume(tr::now),
				[=] {
					Ui::PostponeCall(this, [=] {
						Core::App().downloadManager().resumeAll();
						Core::App().uploaderResumeAll();
						const auto session = &window->session();
						for (const auto &job : EnhancedForward::AllJobs(session)) {
							if (job.active
								&& job.progress.state
									== EnhancedForward::State::Paused) {
								EnhancedForward::resumeForward(job.peer, session);
							}
						}
					});
				},
				&st::menuIconDownload);
		}
		if (hasAnyRunning || hasAnyPaused) {
			addAction(
				tr::lng_tm_all_cancel(tr::now),
				[=] {
					window->show(Ui::MakeConfirmBox({
						.text = tr::lng_tm_all_cancel_confirm(tr::now),
						.confirmed = [=](Fn<void()> close) {
							close();
							Ui::PostponeCall(this, [=] {
								Core::App().downloadManager().cancelAll();
								Core::App().uploaderCancelAll();
								EnhancedForward::CancelAll(&window->session());
							});
						},
						.confirmText = tr::lng_box_yes(tr::now),
						.cancelText = tr::lng_box_no(tr::now),
						.confirmStyle = &st::attentionBoxButton,
					}));
				},
				&st::menuIconCancel);
		}
		if (downloadsFinished || uploadsFinished) {
			addAction(
				tr::lng_tm_all_delete(tr::now),
				[=] {
					window->show(Ui::MakeConfirmBox({
						.text = tr::lng_tm_all_delete_confirm(tr::now),
						.confirmed = [=](Fn<void()> close) {
							close();
							Ui::PostponeCall(this, [=] {
								auto ids = finishedDownloadIds();
								if (!ids.empty()) {
									Core::App().downloadManager().deleteFiles(
										std::move(ids));
								}
								Core::App().uploaderDeleteAllFinished();
							});
						},
						.confirmText = tr::lng_box_yes(tr::now),
						.cancelText = tr::lng_box_no(tr::now),
						.confirmStyle = &st::attentionBoxButton,
					}));
				},
				&st::menuIconDelete);
		}
		if (hasAnyFinished) {
			addAction(
				tr::lng_tm_all_clear(tr::now),
				[=] {
					Ui::PostponeCall(this, [=] {
						Core::App().downloadManager().clearFinishedLoading();
						Core::App().uploaderClearFinished();
						const auto session = &window->session();
						for (const auto &job : EnhancedForward::AllJobs(session)) {
							if (job.finished) {
								EnhancedForward::ClearFinished(session, job.peer);
							}
						}
					});
				},
				&st::menuIconClear);
		}
		return;
	}

	if (showDownloads) {
		if (downloadsRunning && !downloadsPaused) {
			addAction(
				tr::lng_downloads_pause_all(tr::now),
				[=] {
					Ui::PostponeCall(this, [] {
						Core::App().downloadManager().pauseAll();
					});
				},
				&st::menuIconSchedule);
		} else if (downloadsPaused) {
			addAction(
				tr::lng_downloads_resume_all(tr::now),
				[=] {
					Ui::PostponeCall(this, [] {
						Core::App().downloadManager().resumeAll();
					});
				},
				&st::menuIconDownload);
		}
		if (downloadsRunning || downloadsPaused) {
			addAction(
				tr::lng_downloads_cancel_all(tr::now),
				[=] {
					window->show(Ui::MakeConfirmBox({
						.text = tr::lng_downloads_delete_sure_all(tr::now),
						.confirmed = [=](Fn<void()> close) {
							close();
							Ui::PostponeCall(this, [] {
								Core::App().downloadManager().cancelAll();
							});
						},
						.confirmText = tr::lng_box_yes(tr::now),
						.cancelText = tr::lng_box_no(tr::now),
						.confirmStyle = &st::attentionBoxButton,
					}));
				},
				&st::menuIconCancel);
		}
		if (downloadsFinished) {
			addAction(
				tr::lng_downloads_delete_all_downloads(tr::now),
				[=] {
					const auto phrase = tr::lng_downloads_delete_sure_all(tr::now);
					const auto added = Core::App().downloadManager()
						.loadedHasNonCloudFile()
						? QString()
						: tr::lng_downloads_delete_in_cloud(tr::now);
					const auto deleteSure = [=](Fn<void()> close) {
						Ui::PostponeCall(this, close);
						auto ids = finishedDownloadIds();
						if (!ids.empty()) {
							Core::App().downloadManager().deleteFiles(
								std::move(ids));
						}
					};
					window->show(Ui::MakeConfirmBox({
						.text = phrase
							+ (added.isEmpty() ? QString() : "\n\n" + added),
						.confirmed = deleteSure,
						.confirmText = tr::lng_box_yes(tr::now),
						.cancelText = tr::lng_box_no(tr::now),
						.confirmStyle = &st::attentionBoxButton,
					}));
				},
				&st::menuIconDelete);
			addAction(
				tr::lng_downloads_clear_list(tr::now),
				[=] {
					Ui::PostponeCall(this, [] {
						Core::App().downloadManager().clearFinishedLoading();
					});
				},
				&st::menuIconClear);
		}
	}
	if (showUploads) {
		if (uploadsActive && !uploadsPaused) {
			addAction(
				tr::lng_uploads_pause_all(tr::now),
				[=] {
					Ui::PostponeCall(this, [] {
						Core::App().uploaderPauseAll();
					});
				},
				&st::menuIconSchedule);
		} else if (uploadsPaused || uploadsOnlyOnDisk) {
			addAction(
				tr::lng_uploads_resume_all(tr::now),
				[=] {
					Ui::PostponeCall(this, [] {
						Core::App().uploaderResumeAll();
					});
				},
				&st::menuIconDownload);
		}
		if (uploadsActive || uploadsPaused || uploadsOnlyOnDisk) {
			addAction(
				tr::lng_uploads_cancel_all(tr::now),
				[=] {
					window->show(Ui::MakeConfirmBox({
						.text = tr::lng_uploads_cancel_sure_all(tr::now),
						.confirmed = [=](Fn<void()> close) {
							close();
							Ui::PostponeCall(this, [] {
								Core::App().uploaderCancelAll();
							});
						},
						.confirmText = tr::lng_upload_cancel_yes(tr::now),
						.cancelText = tr::lng_upload_cancel_no(tr::now),
						.confirmStyle = &st::attentionBoxButton,
					}));
				},
				&st::menuIconCancel);
		}
		if (uploadsFinished) {
			addAction(
				tr::lng_uploads_delete_all(tr::now),
				[=] {
					window->show(Ui::MakeConfirmBox({
						.text = tr::lng_uploads_delete_all_sure(tr::now),
						.confirmed = [=](Fn<void()> close) {
							close();
							Ui::PostponeCall(this, [] {
								Core::App().uploaderDeleteAllFinished();
							});
						},
						.confirmText = tr::lng_box_yes(tr::now),
						.cancelText = tr::lng_box_no(tr::now),
						.confirmStyle = &st::attentionBoxButton,
					}));
				},
				&st::menuIconDelete);
			addAction(
				tr::lng_uploads_clear_list(tr::now),
				[=] {
					Core::App().uploaderClearFinished();
				},
				&st::menuIconClear);
		}
	}
}

rpl::producer<QString> Widget::title() {
	return tr::lng_transfer_manager_title();
}

std::shared_ptr<Info::Memento> Make(not_null<UserData*> self, Tab tab) {
	auto memento = std::make_shared<Memento>(self);
	memento->setTab(tab);
	return std::make_shared<Info::Memento>(
		std::vector<std::shared_ptr<ContentMemento>>(
			1,
			std::move(memento)));
}

} // namespace Info::Downloads

