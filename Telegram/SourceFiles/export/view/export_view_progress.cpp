/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/view/export_view_progress.h"

#include "ui/effects/animations.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/fade_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "lang/lang_keys.h"
#include "styles/style_boxes.h"
#include "styles/style_export.h"

namespace Export {
namespace View {
namespace {

constexpr auto kShowSkipFileTimeout = 5 * crl::time(1000);

} // namespace

class ProgressWidget::Row : public Ui::RpWidget {
public:
	Row(QWidget *parent, Content::Row &&data);

	void updateData(Content::Row &&data);

protected:
	int resizeGetHeight(int newWidth) override;

	void paintEvent(QPaintEvent *e) override;

private:
	struct Instance {
		base::unique_qptr<Ui::FadeWrap<Ui::FlatLabel>> label;
		base::unique_qptr<Ui::FadeWrap<Ui::FlatLabel>> info;

		float64 value = 0.;
		Ui::Animations::Simple progress;

		bool hiding = true;
		Ui::Animations::Simple opacity;
	};

	void fillCurrentInstance();
	void hideCurrentInstance();
	void setInstanceProgress(Instance &instance, float64 progress);
	void toggleInstance(Instance &data, bool shown);
	void instanceOpacityCallback(base::weak_qptr<Ui::FlatLabel> label);
	void removeOldInstance(base::weak_qptr<Ui::FlatLabel> label);
	void paintInstance(QPainter &p, const Instance &data);

	void updateControlsGeometry(int newWidth);
	void updateInstanceGeometry(const Instance &instance, int newWidth);

	Content::Row _data;
	Instance _current;
	std::vector<Instance> _old;

};

ProgressWidget::Row::Row(QWidget *parent, Content::Row &&data)
: RpWidget(parent)
, _data(std::move(data)) {
	fillCurrentInstance();
}

void ProgressWidget::Row::updateData(Content::Row &&data) {
	const auto wasId = _data.id;
	const auto nowId = data.id;
	_data = std::move(data);
	if (nowId.isEmpty()) {
		hideCurrentInstance();
	} else if (wasId.isEmpty()) {
		fillCurrentInstance();
	} else {
		_current.label->entity()->setText(_data.label);
		_current.info->entity()->setText(_data.info);
		setInstanceProgress(_current, _data.progress);
		if (nowId != wasId) {
			_current.progress.stop();
		}
	}
	updateControlsGeometry(width());
	// Notify the parent VerticalLayout that our height may have changed
	// (e.g. info text switches between one-line and two-line mode).
	// Without this the Row stays at its previously allocated height and
	// clips the info label that moved below the filename.
	updateGeometry();
	update();
}

void ProgressWidget::Row::fillCurrentInstance() {
	_current.label = base::make_unique_q<Ui::FadeWrap<Ui::FlatLabel>>(
		this,
		object_ptr<Ui::FlatLabel>(
			this,
			_data.label,
			st::exportProgressLabel));
	_current.info = base::make_unique_q<Ui::FadeWrap<Ui::FlatLabel>>(
		this,
		object_ptr<Ui::FlatLabel>(
			this,
			_data.info,
			st::exportProgressInfoLabel));
	_current.label->hide(anim::type::instant);
	_current.info->hide(anim::type::instant);

	setInstanceProgress(_current, _data.progress);
	toggleInstance(_current, true);
	if (_data.id == "main") {
		_current.opacity.stop();
		_current.label->finishAnimating();
		_current.info->finishAnimating();
	}
}

void ProgressWidget::Row::hideCurrentInstance() {
	if (!_current.label) {
		return;
	}
	setInstanceProgress(_current, 1.);
	toggleInstance(_current, false);
	_old.push_back(std::move(_current));
}

void ProgressWidget::Row::setInstanceProgress(
		Instance &instance,
		float64 progress) {
	if (_current.value < progress) {
		_current.progress.start(
			[=] { update(); },
			_current.value,
			progress,
			st::exportProgressDuration,
			anim::sineInOut);
	} else if (_current.value > progress) {
		_current.progress.stop();
	}
	_current.value = progress;
}

void ProgressWidget::Row::toggleInstance(Instance &instance, bool shown) {
	Expects(instance.label != nullptr);

	if (instance.hiding != shown) {
		return;
	}
	const auto label = base::make_weak(instance.label->entity());
	instance.opacity.start(
		[=] { instanceOpacityCallback(label); },
		shown ? 0. : 1.,
		shown ? 1. : 0.,
		st::exportProgressDuration);
	instance.hiding = !shown;
	_current.label->toggle(shown, anim::type::normal);
	_current.info->toggle(shown, anim::type::normal);
}

void ProgressWidget::Row::instanceOpacityCallback(
		base::weak_qptr<Ui::FlatLabel> label) {
	update();
	const auto i = ranges::find(_old, label, [](const Instance &instance) {
		return base::make_weak(instance.label->entity());
	});
	if (i != end(_old) && i->hiding && !i->opacity.animating()) {
		crl::on_main(this, [=] {
			removeOldInstance(label);
		});
	}
}

void ProgressWidget::Row::removeOldInstance(
		base::weak_qptr<Ui::FlatLabel> label) {
	const auto i = ranges::find(_old, label, [](const Instance &instance) {
		return base::make_weak(instance.label->entity());
	});
	if (i != end(_old)) {
		_old.erase(i);
	}
}

int ProgressWidget::Row::resizeGetHeight(int newWidth) {
	updateControlsGeometry(newWidth);
	if (_data.id.isEmpty() || _data.id == Content::kDoneId) {
		return 0;
	}
	// If info was placed below label (two-line mode), return combined height.
	if (_current.label && _current.info && _current.info->y() > 0) {
		return _current.label->height() + _current.info->height() + st::exportProgressWidth;
	}
	return st::exportProgressRowHeight;
}

void ProgressWidget::Row::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);

	// Draw the separator line only for active-download rows (main, chat, file_).
	// Stat summary rows ("stat_*", "header_*") and the invisible sentinel done
	// row ("done") do not need a separator — they are visually separated by the
	// FixedHeightWidget spacers inserted between them.
	const bool isStat = _data.id.startsWith(QStringLiteral("stat_"))
		|| _data.id.startsWith(QStringLiteral("header_"))
		|| _data.id == Content::kDoneId
		|| _data.id.isEmpty();
	if (!isStat) {
		const auto thickness = st::exportProgressWidth;
		const auto top = height() - thickness;
		p.fillRect(0, top, width(), thickness, st::shadowFg);
	}

	for (const auto &instance : _old) {
		paintInstance(p, instance);
	}
	paintInstance(p, _current);
}

void ProgressWidget::Row::paintInstance(QPainter &p, const Instance &data) {
	const auto opacity = data.opacity.value(data.hiding ? 0. : 1.);

	if (!opacity) {
		return;
	}
	// Don't paint progress bars for invisible/sentinel rows (empty id or kDoneId).
	// These have height=0 and painting at height()-thickness bleeds into surrounding widgets.
	if (_data.id.isEmpty() || _data.id == Content::kDoneId) {
		return;
	}
	p.setOpacity(opacity);

	const auto thickness = st::exportProgressWidth;
	const auto top = height() - thickness;
	const auto till = qRound(data.progress.value(data.value) * width());
	if (till > 0) {
		p.fillRect(0, top, till, thickness, st::exportProgressFg);
	}
	if (till < width()) {
		const auto left = width() - till;
		p.fillRect(till, top, left, thickness, st::exportProgressBg);
	}
}

void ProgressWidget::Row::updateControlsGeometry(int newWidth) {
	updateInstanceGeometry(_current, newWidth);
	for (const auto &instance : _old) {
		updateInstanceGeometry(instance, newWidth);
	}
}

void ProgressWidget::Row::updateInstanceGeometry(
		const Instance &instance,
		int newWidth) {
	if (!instance.label) {
		return;
	}
	// Let the info label resize first, then give remaining width to label.
	// If overall text is too wide, allow the label to take full width
	// and place info on a second line below.
	instance.info->resizeToNaturalWidth(newWidth);
	const auto infoWidth = instance.info->width();
	const auto labelNatural = instance.label->naturalWidth();
	const auto labelAvailable = newWidth - infoWidth;
	const auto labelWidth = std::max(labelAvailable, newWidth / 3);
	instance.label->resizeToWidth(labelWidth);
	instance.info->moveToRight(0, 0, newWidth);
	instance.label->moveToLeft(0, 0, newWidth);
}

ProgressWidget::ProgressWidget(
	QWidget *parent,
	rpl::producer<Content> content)
: RpWidget(parent)
, _scroll(Ui::CreateChild<Ui::ScrollArea>(this, st::defaultScrollArea))
, _body(static_cast<Ui::VerticalLayout*>(_scroll->setOwnedWidget(
	object_ptr<Ui::OverrideMargins>(
		_scroll,
		object_ptr<Ui::VerticalLayout>(_scroll)))->entity()))
, _fileShowSkipTimer([=] { _skipFile->show(anim::type::normal); }) {
	_about = _body->add(
		object_ptr<Ui::FlatLabel>(
			this,
			tr::lng_export_progress(tr::now),
			st::exportAboutLabel),
		st::exportAboutPadding);
	widthValue(
	) | rpl::on_next([=](int width) {
		_body->resizeToWidth(width);
		_body->moveToLeft(0, 0);
	}, _body->lifetime());

	auto skipFileWrap = _body->add(object_ptr<Ui::FixedHeightWidget>(
		_body,
		st::defaultLinkButton.font->height + st::exportProgressRowSkip));
	_skipFileWrap = skipFileWrap;
	_skipFile = base::make_unique_q<Ui::FadeWrap<Ui::LinkButton>>(
		skipFileWrap,
		object_ptr<Ui::LinkButton>(
			this,
			tr::lng_export_skip_file(tr::now),
			st::defaultLinkButton));
	_skipFile->hide(anim::type::instant);
	_skipFile->moveToLeft(st::exportProgressRowPadding.left(), 0);

	widthValue(
	) | rpl::on_next([=](int width) {
		if (const auto widget = static_cast<Ui::RpWidget*>(_scroll->widget())) {
			widget->resizeToWidth(width);
		}
		_body->resizeToWidth(width);
	}, lifetime());

	std::move(
		content
	) | rpl::on_next([=](Content &&content) {
		updateState(std::move(content));
	}, lifetime());

	_cancel = base::make_unique_q<Ui::RoundButton>(
		this,
		tr::lng_export_stop(),
		st::exportCancelButton);
	setupBottomButton(_cancel.get());

	sizeValue()
		| rpl::on_next([=](QSize size) {
			const auto buttonHeight = _cancel ? _cancel->height() : (_done ? _done->height() : 0);
			const auto bottomGap = buttonHeight + st::exportCancelBottom + 10;
			_scroll->resize(size.width(), size.height() - bottomGap);
			_scroll->moveToLeft(0, 0);
		}, lifetime());
}

rpl::producer<uint64> ProgressWidget::skipFileClicks() const {
	return _skipFile->entity()->clicks(
	) | rpl::map([=] { return _fileRandomId; });
}

rpl::producer<> ProgressWidget::cancelClicks() const {
	return _cancel
		? (_cancel->clicks() | rpl::to_empty)
		: (rpl::never<>() | rpl::type_erased);
}

rpl::producer<> ProgressWidget::doneClicks() const {
	return _doneClicks.events();
}

void ProgressWidget::setupBottomButton(not_null<Ui::RoundButton*> button) {
	button->setTextTransform(Ui::RoundButton::TextTransform::NoTransform);
	button->show();

	sizeValue(
	) | rpl::on_next([=](QSize size) {
		button->move(
			(size.width() - button->width()) / 2,
			(size.height() - st::exportCancelBottom - button->height()));
	}, button->lifetime());
}

void ProgressWidget::updateState(Content &&content) {
	const auto doneId = Content::kDoneId;
	// Ignore scan_complete sentinel (clears top bar scanning label only).
	if (ranges::any_of(content.rows, [](const auto &r) {
		return r.id == QStringLiteral("scan_complete");
	})) {
		return;
	}
	const auto done = ranges::any_of(content.rows, [&](const auto &row) {
		return row.id == doneId;
	});
	if (done) {
		showDone();
	}

	const auto wasCount = _rows.size();
	const auto headerCount = 2; // _about and _skipFileWrap
	auto index = 0;
	for (auto &row : content.rows) {
		if (index < _rows.size()) {
			_rows[index]->updateData(std::move(row));
		} else {
			if (index > 0) {
				_body->insert(
					headerCount + index * 2 - 1,
					object_ptr<Ui::FixedHeightWidget>(
						this,
						st::exportProgressRowSkip));
			}
			_rows.push_back(_body->insert(
				headerCount + index * 2,
				object_ptr<Row>(this, std::move(row)),
				st::exportProgressRowPadding));
			_rows.back()->show();
		}
		++index;
	}
	const auto fileRandomId = !content.rows.empty()
		? content.rows.back().randomId
		: uint64(0);
	if (_fileRandomId != fileRandomId) {
		_fileShowSkipTimer.cancel();
		_skipFile->hide(anim::type::normal);
		_fileRandomId = fileRandomId;
		if (_fileRandomId) {
			_fileShowSkipTimer.callOnce(kShowSkipFileTimeout);
		}
	}
	for (const auto count = _rows.size(); index != count; ++index) {
		_rows[index]->updateData(Content::Row());
	}
	if (_rows.size() != wasCount) {
		_body->resizeToWidth(width());
	}
}

void ProgressWidget::showDone() {
	_cancel = nullptr;
	if (_skipFileWrap) {
		_skipFileWrap->hide();
	}
	_skipFile->hide(anim::type::instant);
	_fileShowSkipTimer.cancel();

	_about->setText(tr::lng_export_about_done(tr::now));

	_body->resizeToWidth(width());

	_done = base::make_unique_q<Ui::RoundButton>(
		this,
		tr::lng_export_done(),
		st::exportDoneButton);
	const auto desired = std::min(
		st::exportDoneButton.style.font->width(tr::lng_export_done(tr::now))
		+ st::exportDoneButton.height
		- st::exportDoneButton.style.font->height,
		st::exportPanelSize.width() - 2 * st::exportCancelBottom);
	if (_done->width() < desired) {
		_done->setFullWidth(desired);
	}
	_done->clicks(
	) | rpl::to_empty
	| rpl::start_to_stream(_doneClicks, _done->lifetime());
	setupBottomButton(_done.get());
}

ProgressWidget::~ProgressWidget() = default;

} // namespace View
} // namespace Export
