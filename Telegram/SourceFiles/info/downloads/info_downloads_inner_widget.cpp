/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/downloads/info_downloads_inner_widget.h"

#include "info/downloads/info_downloads_widget.h"
#include "info/downloads/info_downloads_provider.h"
#include "info/media/info_media_list_widget.h"
#include "info/info_controller.h"
#include "ui/widgets/labels.h"
#include "ui/search_field_controller.h"
#include "lang/lang_keys.h"
#include "styles/style_info.h"

namespace Info::Downloads {

class EmptyWidget : public Ui::RpWidget {
public:
	EmptyWidget(QWidget *parent);

	void setFullHeight(rpl::producer<int> fullHeightValue);
	void setSearchQuery(const QString &query);

protected:
	int resizeGetHeight(int newWidth) override;

	void paintEvent(QPaintEvent *e) override;

private:
	object_ptr<Ui::FlatLabel> _text;
	int _height = 0;

};

EmptyWidget::EmptyWidget(QWidget *parent)
: RpWidget(parent)
, _text(this, st::infoEmptyLabel) {
}

void EmptyWidget::setFullHeight(rpl::producer<int> fullHeightValue) {
	std::move(
		fullHeightValue
	) | rpl::on_next([this](int fullHeight) {
		// Make icon center be on 1/3 height.
		auto iconCenter = fullHeight / 3;
		auto iconHeight = st::infoEmptyFile.height();
		auto iconTop = iconCenter - iconHeight / 2;
		_height = iconTop + st::infoEmptyIconTop;
		resizeToWidth(width());
	}, lifetime());
}

void EmptyWidget::setSearchQuery(const QString &query) {
	_text->setText(query.isEmpty()
		? tr::lng_media_file_empty(tr::now)
		: tr::lng_media_file_empty_search(tr::now));
	resizeToWidth(width());
}

int EmptyWidget::resizeGetHeight(int newWidth) {
	auto labelTop = _height - st::infoEmptyLabelTop;
	auto labelWidth = newWidth - 2 * st::infoEmptyLabelSkip;
	_text->resizeToNaturalWidth(labelWidth);

	auto labelLeft = (newWidth - _text->width()) / 2;
	_text->moveToLeft(labelLeft, labelTop, newWidth);

	update();
	return _height;
}

void EmptyWidget::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);

	const auto iconLeft = (width() - st::infoEmptyFile.width()) / 2;
	const auto iconTop = height() - st::infoEmptyIconTop;
	st::infoEmptyFile.paint(p, iconLeft, iconTop, width());
}

InnerWidget::InnerWidget(
	QWidget *parent,
	not_null<Controller*> controller)
: RpWidget(parent)
, _controller(controller)
, _list(this, _controller)
, _empty(this) {
	_counter.create(this, st::downloadsCounter);
	const auto heightLifetime = std::make_shared<rpl::lifetime>();
	_list->heightValue(
	) | rpl::on_next(
		[this] { refreshHeight(); },
		*heightLifetime);
	using namespace rpl::mappers;
	_list->scrollToRequests(
	) | rpl::map([widget = _list.data()](int to) {
		return Ui::ScrollToRequest {
			widget->y() + to,
			-1
		};
	}) | rpl::start_to_stream(
		_scrollToRequests,
		*heightLifetime);
	_selectedLists.fire(_list->selectedListValue());
	_listTops.fire(_list->topValue());
	_empty->heightValue(
	) | rpl::on_next(
		[this] { refreshHeight(); },
		_empty->lifetime());

	_counter->heightValue(
	) | rpl::on_next(
		[this] { refreshHeight(); },
		_counter->lifetime());
	provider()->counterValue(
	) | rpl::on_next([=](const QString &text) {
		_counter->setText(text);
		_counter->setVisible(!text.isEmpty());
		refreshHeight();
	}, _counter->lifetime());
}

void InnerWidget::visibleTopBottomUpdated(
		int visibleTop,
		int visibleBottom) {
	if (_list) {
		setChildVisibleTopBottom(_list, visibleTop, visibleBottom);
	}
}

bool InnerWidget::showInternal(not_null<Memento*> memento) {
	if (memento->section().type() == Section::Type::Downloads) {
		restoreState(memento);
		return true;
	}
	return false;
}



void InnerWidget::saveState(not_null<Memento*> memento) {
	_list->saveState(&memento->media());
}

void InnerWidget::restoreState(not_null<Memento*> memento) {
	_list->restoreState(&memento->media());
}

Provider *InnerWidget::provider() const {
	return static_cast<Provider*>(_list->provider().get());
}

void InnerWidget::setFilter(Tab tab) {
	const auto filter = (tab == Tab::Downloads)
		? Provider::Filter::Downloads
		: (tab == Tab::Uploads)
		? Provider::Filter::Uploads
		: (tab == Tab::Forwards)
		? Provider::Filter::Forwards
		: Provider::Filter::All;
	provider()->setFilter(filter);
	_selectedLists.fire(_list->selectedListValue());
	refreshHeight();
}

rpl::producer<bool> InnerWidget::hasDownloadsValue() const {
	return provider()->hasDownloadsValue();
}

rpl::producer<bool> InnerWidget::hasUploadsValue() const {
	return provider()->hasUploadsValue();
}

rpl::producer<SelectedItems> InnerWidget::selectedListValue() const {
	return _selectedLists.events_starting_with(
		_list->selectedListValue()
	) | rpl::flatten_latest();
}

void InnerWidget::selectionAction(SelectionAction action) {
	_list->selectionAction(action);
}

InnerWidget::~InnerWidget() = default;

int InnerWidget::resizeGetHeight(int newWidth) {
	_inResize = true;
	auto guard = gsl::finally([this] { _inResize = false; });

	_list->resizeToWidth(newWidth);
	_empty->resizeToWidth(newWidth);
	_counter->resizeToWidth(newWidth);
	return recountHeight();
}

void InnerWidget::refreshHeight() {
	if (_inResize) {
		return;
	}
	resize(width(), recountHeight());
}

int InnerWidget::recountHeight() {
	auto top = 0;
	const auto counterShown = !_counter->isHidden();
	if (counterShown) {
		top += st::downloadsCounterSkip;
		_counter->moveToLeft(0, top);
		top += _counter->heightNoMargins();
		top += st::downloadsCounterSkip;
	}
	if (_list->heightNoMargins() > 0) {
		_list->moveToLeft(0, top);
		top += _list->heightNoMargins();
		_empty->hide();
	} else {
		_empty->show();
		_empty->moveToLeft(0, top);
		top += _empty->heightNoMargins();
	}
	return top;
}

void InnerWidget::setScrollHeightValue(rpl::producer<int> value) {
	using namespace rpl::mappers;
	_empty->setFullHeight(rpl::combine(
		std::move(value),
		_listTops.events_starting_with(
			_list->topValue()
		) | rpl::flatten_latest(),
		_1 - _2));
}

rpl::producer<Ui::ScrollToRequest> InnerWidget::scrollToRequests() const {
	return _scrollToRequests.events();
}

} // namespace Info::Downloads
