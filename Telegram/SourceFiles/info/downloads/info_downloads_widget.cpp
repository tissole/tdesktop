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
	_tabs->hide();
	_tabsShadow->hide();

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
			: (tab == Tab::Both)
			? tr::lng_downloads_tab_all(tr::now)
			: tr::lng_downloads_section(tr::now));
	}
	_tabs->setSections(labels);
}

void Widget::refreshTabs() {
	auto tabs = std::vector<Tab>();
	if (_hasDownloads && _hasUploads) {
		tabs = { Tab::Downloads, Tab::Uploads, Tab::Both };
	} else if (_hasDownloads) {
		tabs = { Tab::Downloads };
	} else if (_hasUploads) {
		tabs = { Tab::Uploads };
	}
	const auto show = !tabs.empty();
	if (tabs != _tabList) {
		_tabList = std::move(tabs);
		rebuildTabSections();
	}
	if (show != _tabsShown) {
		_tabsShown = show;
		_tabs->setVisible(show);
		_tabsShadow->setVisible(show);
		setScrollTopSkip(show ? (_tabs->st().height + st::lineWidth) : 0);
	}
	if (show) {
		auto tab = _currentTab;
		if (std::find(_tabList.begin(), _tabList.end(), tab)
			== _tabList.end()) {
			tab = _tabList.front();
		}
		applyTab(tab, true);
	} else {
		_inner->setFilter(Tab::Both);
	}
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
	const auto &loadingManager = Core::App().downloadManager();
	const auto both = _hasDownloads && _hasUploads;
	const auto showDownloads = both
		? (_currentTab != Tab::Uploads)
		: _hasDownloads;
	const auto showUploads = both
		? (_currentTab != Tab::Downloads)
		: _hasUploads;

	if (showDownloads && loadingManager.anyResumable()) {
		addAction(
			tr::lng_downloads_pause_all(tr::now),
			[=] {
				Ui::PostponeCall(this, [] {
					Core::App().downloadManager().pauseAll();
				});
			},
			&st::menuIconSchedule);
	}
	if (showDownloads && loadingManager.anyPaused()) {
		addAction(
			tr::lng_downloads_resume_all(tr::now),
			[=] {
				Ui::PostponeCall(this, [] {
					Core::App().downloadManager().resumeAll();
				});
			},
			&st::menuIconDownload);
	}
	if (showUploads && Core::App().maybePrimarySession()) {
		auto uploadsActive = false;
		auto uploadsPaused = false;
		auto uploadsOnDisk = false;
		for (const auto &account : Core::App().domain().orderedAccounts()) {
			if (const auto s = account->maybeSession()) {
				const auto active = s->uploader().anyUploads();
				const auto paused = s->uploader().isPaused();
				const auto onDisk = !active
					&& s->uploader().pendingResumeCount() > 0;
				uploadsActive |= active;
				uploadsPaused |= paused;
				uploadsOnDisk |= onDisk;
			}
		}
		const auto onlyOnDisk = !uploadsActive && uploadsOnDisk;
		if (uploadsActive && !uploadsPaused) {
			addAction(
				tr::lng_uploads_pause_all(tr::now),
				[=] {
					Ui::PostponeCall(this, [] {
						for (const auto &account : Core::App().domain().orderedAccounts()) {
							if (const auto s = account->maybeSession()) {
								s->uploader().pauseAllUploads();
							}
						}
					});
				},
				&st::menuIconSchedule);
		} else if (uploadsPaused || onlyOnDisk) {
			addAction(
				tr::lng_uploads_resume_all(tr::now),
				[=] {
					Ui::PostponeCall(this, [] {
						for (const auto &account : Core::App().domain().orderedAccounts()) {
							if (const auto s = account->maybeSession()) {
								s->uploader().resumeAllUploads();
							}
						}
					});
				},
				&st::menuIconDownload);
		}
		if (uploadsActive || uploadsPaused || onlyOnDisk) {
			addAction(
				tr::lng_uploads_cancel_all(tr::now),
				[=] {
					window->show(Ui::MakeConfirmBox({
						.text = tr::lng_uploads_delete_sure_all(tr::now),
						.confirmed = [=](Fn<void()> close) {
							close();
							for (const auto &account : Core::App().domain().orderedAccounts()) {
								if (const auto s = account->maybeSession()) {
									s->uploader().cancelAll();
								}
							}
						},
						.confirmText = tr::lng_upload_cancel_yes(tr::now),
						.cancelText = tr::lng_upload_cancel_no(tr::now),
						.confirmStyle = &st::attentionBoxButton,
					}));
				},
				&st::menuIconDelete);
		}
		auto hasFinishedUploads = false;
		for (const auto &account : Core::App().domain().orderedAccounts()) {
			if (const auto s = account->maybeSession()) {
				if (s->uploader().anyFinishedUploads() > 0) {
					hasFinishedUploads = true;
				}
			}
		}
		if (hasFinishedUploads) {
			addAction(
				tr::lng_uploads_clear_list(tr::now),
				[=] {
					for (const auto &account : Core::App().domain().orderedAccounts()) {
						if (const auto s = account->maybeSession()) {
							s->uploader().clearFinishedUploads();
						}
					}
				},
				&st::menuIconClear);
		}
		if (hasFinishedUploads) {
			addAction(
				tr::lng_uploads_delete_all(tr::now),
				[=] {
					window->show(Ui::MakeConfirmBox({
						.text = tr::lng_uploads_delete_all_sure(tr::now),
						.confirmed = [=](Fn<void()> close) {
							close();
							for (const auto &account : Core::App().domain().orderedAccounts()) {
								if (const auto s = account->maybeSession()) {
									s->uploader().deleteAllFinishedUploads();
								}
							}
						},
						.confirmText = tr::lng_uploads_delete_all_confirm(tr::now),
						.cancelText = tr::lng_cancel(tr::now),
						.confirmStyle = &st::attentionBoxButton,
					}));
				},
				&st::menuIconDelete);
		}
	}
	if (showDownloads && loadingManager.anyFinishedLoading()) {
		addAction(
			tr::lng_downloads_clear_list(tr::now),
			[=] {
				Ui::PostponeCall(this, [] {
					Core::App().downloadManager().clearFinishedLoading();
				});
			},
			&st::menuIconClear);
	}

	const auto deleteAll = [=] {
		auto &manager = Core::App().downloadManager();
		const auto phrase = tr::lng_downloads_delete_sure_all(tr::now);
		const auto added = manager.loadedHasNonCloudFile()
			? QString()
			: tr::lng_downloads_delete_in_cloud(tr::now);
		const auto deleteSure = [=, &manager](Fn<void()> close) {
			Ui::PostponeCall(this, close);
			manager.deleteAll();
		};
		window->show(Ui::MakeConfirmBox({
			.text = phrase + (added.isEmpty() ? QString() : "\n\n" + added),
			.confirmed = deleteSure,
			.confirmText = tr::lng_box_delete(tr::now),
			.confirmStyle = &st::attentionBoxButton,
		}));
	};
	if (showDownloads
		&& (loadingManager.anyResumable()
			|| loadingManager.anyFinishedLoading())) {
		addAction(
			tr::lng_downloads_delete_all_downloads(tr::now),
			deleteAll,
			&st::menuIconDelete);
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

