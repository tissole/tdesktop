/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "info/info_content_widget.h"
#include "info/media/info_media_widget.h"
#include "base/object_ptr.h"

namespace Ui {
class SettingsSlider;
class PlainShadow;
} // namespace Ui

namespace Ui {
class SearchFieldController;
} // namespace Ui

namespace Info::Downloads {

class InnerWidget;

enum class Tab {
	Downloads,
	Uploads,
	Forwards,
	Both,
};

class Memento final : public ContentMemento {
public:
	Memento(not_null<Controller*> controller);
	Memento(not_null<UserData*> self);
	~Memento();

	object_ptr<ContentWidget> createWidget(
		QWidget *parent,
		not_null<Controller*> controller,
		const QRect &geometry) override;

	Section section() const override;

	void setTab(Tab tab) {
		_tab = tab;
	}
	[[nodiscard]] Tab tab() const {
		return _tab;
	}

	[[nodiscard]] Media::Memento &media() {
		return _media;
	}
	[[nodiscard]] const Media::Memento &media() const {
		return _media;
	}

private:
	Media::Memento _media;
	Tab _tab = Tab::Downloads;

};

class Widget final : public ContentWidget {
public:
	Widget(QWidget *parent, not_null<Controller*> controller);

	bool showInternal(
		not_null<ContentMemento*> memento) override;

	void setInternalState(
		const QRect &geometry,
		not_null<Memento*> memento);

	rpl::producer<SelectedItems> selectedListValue() const override;
	void selectionAction(SelectionAction action) override;

	void fillTopBarMenu(const Ui::Menu::MenuCallback &addAction) override;

	rpl::producer<QString> title() override;

private:
	void saveState(not_null<Memento*> memento);
	void restoreState(not_null<Memento*> memento);
	void setupTabs();
	void refreshTabs();
	void rebuildTabSections();
	void applyTab(Tab tab, bool updateSlider);

	std::shared_ptr<ContentMemento> doCreateMemento() override;

	InnerWidget *_inner = nullptr;
	object_ptr<Ui::SettingsSlider> _tabs = { nullptr };
	object_ptr<Ui::PlainShadow> _tabsShadow = { nullptr };
	std::vector<Tab> _tabList;
	Tab _currentTab = Tab::Downloads;
	bool _hasDownloads = false;
	bool _hasUploads = false;
	bool _tabsShown = false;

};

[[nodiscard]] std::shared_ptr<Info::Memento> Make(
	not_null<UserData*> self,
	Tab tab = Tab::Downloads);

void SetLastActivityTab(Tab tab);
[[nodiscard]] Tab LastActivityTab();

} // namespace Info::Downloads
