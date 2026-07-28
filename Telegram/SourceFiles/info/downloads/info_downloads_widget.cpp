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
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/scroll_area.h"
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
	memento->setScrollTop(scrollTopSave());
	_inner->saveState(memento);
}

void Widget::restoreState(not_null<Memento*> memento) {
	_inner->restoreState(memento);
	scrollTopRestore(memento->scrollTop());
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

	if (loadingManager.anyResumable()) {
		addAction(
			tr::lng_downloads_pause_all(tr::now),
			[=] {
				Ui::PostponeCall(this, [] {
					Core::App().downloadManager().pauseAll();
				});
			},
			&st::menuIconSchedule);
	}
	if (loadingManager.anyPaused()) {
		addAction(
			tr::lng_downloads_resume_all(tr::now),
			[=] {
				Ui::PostponeCall(this, [] {
					Core::App().downloadManager().resumeAll();
				});
			},
			&st::menuIconDownload);
	}
	if (const auto session = Core::App().maybePrimarySession()) {
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
		auto allFinished = true;
		for (const auto &account : Core::App().domain().orderedAccounts()) {
			if (const auto s = account->maybeSession()) {
				if (s->uploader().anyFinishedUploads() > 0) {
					hasFinishedUploads = true;
				}
				if (!s->uploader().allFinished()) {
					allFinished = false;
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
		if (allFinished && (uploadsActive || uploadsPaused || onlyOnDisk || hasFinishedUploads)) {
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
	if (loadingManager.anyFinishedLoading()) {
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
	if (loadingManager.anyResumable()
		|| loadingManager.anyFinishedLoading()) {
		addAction(
			tr::lng_downloads_delete_all_downloads(tr::now),
			deleteAll,
			&st::menuIconDelete);
	}
}

rpl::producer<QString> Widget::title() {
	const auto computeTitle = [=]() -> QString {
		auto &manager = Core::App().downloadManager();
		auto hasDl = false;
		for ([[maybe_unused]] const auto id : manager.loadingList()) {
			hasDl = true;
			break;
		}
		if (!hasDl) {
			for ([[maybe_unused]] const auto id : manager.loadedList()) {
				hasDl = true;
				break;
			}
		}
		auto hasUl = false;
		for (const auto &account : Core::App().domain().orderedAccounts()) {
			if (const auto s = account->maybeSession()) {
				if (s->uploader().anyUploads()
					|| s->uploader().isPaused()
					|| s->uploader().pendingResumeCount() > 0) {
					hasUl = true;
					break;
				}
			}
		}
		if (hasDl && hasUl) {
			return tr::lng_downloads_section(tr::now)
				+ u" & "_q
				+ tr::lng_uploads_section(tr::now);
		} else if (hasUl) {
			return tr::lng_uploads_section(tr::now);
		}
		return tr::lng_downloads_section(tr::now);
	};
	return rpl::merge(
		rpl::single(computeTitle()),
		controller()->session().uploader().loadingListChanges()
			| rpl::map([=] { return computeTitle(); })
	);
}

std::shared_ptr<Info::Memento> Make(not_null<UserData*> self) {
	return std::make_shared<Info::Memento>(
		std::vector<std::shared_ptr<ContentMemento>>(
			1,
			std::make_shared<Memento>(self)));
}

} // namespace Info::Downloads

