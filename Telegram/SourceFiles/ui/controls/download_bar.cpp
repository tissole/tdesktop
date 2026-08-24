/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/controls/download_bar.h"

#include "ui/widgets/buttons.h"
#include "ui/text/format_values.h"
#include "ui/text/text_utilities.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"
#include "lang/lang_keys.h"
#include "styles/style_dialogs.h"
#include <rpl/never.h>

namespace Ui {
namespace {

[[nodiscard]] QImage Make(const QImage &image, int size) {
	if (image.isNull()) {
		return QImage();
	}
	auto result = image.scaledToWidth(
		size * style::DevicePixelRatio(),
		Qt::SmoothTransformation);
	result.setDevicePixelRatio(style::DevicePixelRatio());
	return result;
}

} // namespace

DownloadBar::DownloadBar(
	not_null<QWidget*> parent,
	rpl::producer<DownloadBarProgress> progress)
: _button(
	parent,
	object_ptr<RippleButton>(parent, st::dialogsMenuToggle.ripple))
, _shadow(parent)
, _progress(std::move(progress))
, _radial([=](crl::time now) { radialAnimationCallback(now); }) {
	_button.hide(anim::type::instant);
	_shadow.showOn(_button.shownValue());
	_button.setDirectionUp(false);
	_button.entity()->resize(0, st::downloadBarHeight);
	_button.entity()->paintRequest(
	) | rpl::on_next([=](QRect clip) {
		auto p = Painter(_button.entity());
		paint(p, clip);
	}, lifetime());

	style::PaletteChanged(
	) | rpl::on_next([=] {
		refreshIcon();
	}, lifetime());
	refreshIcon();

	_progress.value(
	) | rpl::on_next([=](const DownloadBarProgress &progress) {
		refreshInfo(progress);
	}, lifetime());
}

DownloadBar::~DownloadBar() = default;

void DownloadBar::show(DownloadBarContent &&content) {
	const auto allFinished = (content.done >= content.count)
		&& (content.uploadDone >= content.uploadCount)
		&& (content.efDone >= content.efCount)
		&& (content.nfCount == 0 || content.nfDone >= content.nfCount);
	_button.toggle(!allFinished, anim::type::normal);
	if (allFinished) {
		return;
	}
	if (!_radial.animating()) {
		_radial.start(computeProgress());
	}
	_content = content;
	const auto finished = (_content.done == _content.count)
		&& (_content.uploadDone == _content.uploadCount)
		&& (_content.efDone == _content.efCount)
		&& (_content.nfCount == 0 || _content.nfDone >= _content.nfCount);
	if (_finished != finished) {
		_finished = finished;
		_finishedAnimation.start(
			[=] { _button.update(); },
			_finished ? 0. : 1.,
			_finished ? 1. : 0.,
			st::widgetFadeDuration);
	}
	refreshThumbnail();
	const auto dlPrefix = u"DL "_q;
	const auto ulPrefix = u"UL "_q;
	const auto efPrefix = u"EF "_q;
	const auto nfPrefix = u"FWD "_q;
	_title.setMarkedText(
		st::defaultTextStyle,
		(content.count > 1
			? tr::bold(dlPrefix + tr::lng_profile_files(
				tr::now,
				lt_count, content.count))
			: content.count == 1
			? tr::bold(tr::lng_tm_dl_prefix(
				tr::now,
				lt_name, content.singleName.text))
			: (content.efCount > 1
				? tr::bold(efPrefix + tr::lng_tm_files_progress(
					tr::now,
					lt_done, QString::number(content.efDone),
					lt_total, QString::number(content.efCount)))
				: content.efCount == 1
				? tr::bold(tr::lng_tm_fw_prefix(
					tr::now,
					lt_name, content.singleName.text))
				: (content.nfCount > 0
					? tr::bold(nfPrefix + tr::lng_tm_files_progress(
						tr::now,
						lt_done, QString::number(content.nfDone),
						lt_total, QString::number(content.nfCount)))
					: (content.uploadCount > 1
						? tr::bold(ulPrefix + tr::lng_tm_files_progress(
							tr::now,
							lt_done, QString::number(content.uploadDone),
							lt_total, QString::number(content.uploadCount)))
						: tr::bold(tr::lng_tm_ul_prefix(
							tr::now,
							lt_name, content.singleUploadName.text)))))));
	refreshInfo(_progress.current());
}

void DownloadBar::refreshThumbnail() {
	if (_content.singleThumbnail.isNull()) {
		_thumbnail = _thumbnailDone = QImage();
		_thumbnailCacheKey = 0;
		return;
	}
	const auto cacheKey = _content.singleThumbnail.cacheKey();
	if (_thumbnailCacheKey == cacheKey) {
		return;
	}
	_thumbnailCacheKey = cacheKey;
	_thumbnailLarge = _content.singleThumbnail;
	_thumbnailLarge.detach();
	const auto width = _thumbnailLarge.width();
	const auto height = _thumbnailLarge.height();
	if (width != height) {
		const auto size = std::min(width, height);
		_thumbnailLarge = _thumbnailLarge.copy(
			(width - size) / 2,
			(height - size) / 2,
			size,
			size);
	}
	const auto size = st::downloadLoadingSize;
	const auto added = 3 * st::downloadLoadingLine;
	const auto loadingsize = size;
	const auto donesize = size + (added - st::downloadLoadingLine) * 2;
	const auto make = [&](int size) {
		return Images::Circle(Make(_thumbnailLarge, size));
	};
	_thumbnail = make(loadingsize);
	_thumbnailDone = make(donesize);
	_thumbnailLarge = Images::Circle(std::move(_thumbnailLarge));
}

void DownloadBar::refreshIcon() {
	_documentIconLarge = st::downloadIconDocument.instance(
		st::windowFgActive->c,
		style::kScaleMax / style::DevicePixelRatio());
	_documentIcon = Make(_documentIconLarge, st::downloadIconSize);
	_documentIconDone = Make(_documentIconLarge, st::downloadIconSizeDone);
}

void DownloadBar::refreshInfo(const DownloadBarProgress &progress) {
	auto text = TextWithEntities();
	const auto effectiveUploadReady = progress.uploadReady
		? progress.uploadReady
		: (_content.uploadCount > 1
			? _content.uploadReady
			: _content.uploadSingleReady);
	const auto effectiveUploadTotal = progress.uploadTotal
		? progress.uploadTotal
		: (_content.uploadCount > 1
			? _content.uploadTotal
			: _content.uploadSingleTotal);
	const auto effectiveReady = progress.ready;
	const auto effectiveTotal = progress.total;
	const auto efReady = progress.efReady;
	const auto efTotal = progress.efTotal;
	if (_content.nfCount > 0
		&& _content.nfDone < _content.nfCount
		&& progress.nfFloodSeconds > 0) {
		text = tr::marked(
			u"FLOOD_WAIT %1s"_q.arg(progress.nfFloodSeconds));
	} else if (efReady < efTotal && efTotal > 0) {
		text = tr::marked(
			FormatDownloadText(efReady, efTotal));
	} else if (_content.efCount > 0 && efReady > 0) {
		// Persisted-only state: total is unknown offline, show ready bytes.
		text = tr::marked(
			FormatDownloadText(efReady, efReady));
	} else if (effectiveReady < effectiveTotal) {
		text = tr::marked(
			FormatDownloadText(effectiveReady, effectiveTotal));
	} else if (effectiveUploadReady < effectiveUploadTotal) {
		text = tr::marked(
			FormatDownloadText(effectiveUploadReady, effectiveUploadTotal));
	} else if (_content.uploadCount == 1 && _content.uploadReady < _content.uploadTotal) {
		text = tr::marked(
			FormatDownloadText(_content.uploadReady, _content.uploadTotal));
	} else {
		text = TextWithEntities();
	}
	_info.setMarkedText(
		st::downloadInfoStyle,
		std::move(text));
	_button.entity()->update();
}

bool DownloadBar::isHidden() const {
	return _button.isHidden();
}

int DownloadBar::height() const {
	return _button.height();
}

rpl::producer<int> DownloadBar::heightValue() const {
	return _button.heightValue();
}

rpl::producer<bool> DownloadBar::shownValue() const {
	return _button.shownValue();
}

void DownloadBar::setGeometry(int left, int top, int width, int height) {
	_button.resizeToWidth(width);
	_button.moveToLeft(left, top);
	_shadow.setGeometry(left, top - st::lineWidth, width, st::lineWidth);
}

rpl::producer<> DownloadBar::clicks() const {
	const auto entity = _button.entity();
	if (!entity) {
		return rpl::never<>();
	}
	return entity->clicks() | rpl::to_empty;
}

rpl::lifetime &DownloadBar::lifetime() {
	return _button.lifetime();
}

void DownloadBar::paint(Painter &p, QRect clip) {
	const auto button = _button.entity();
	const auto outerw = button->width();
	const auto over = button->isOver() || button->isDown();
	const auto &icon = over ? st::downloadArrowOver : st::downloadArrow;
	p.fillRect(clip, st::windowBg);
	button->paintRipple(p, 0, 0);

	const auto finished = _finishedAnimation.value(_finished ? 1. : 0.);
	const auto size = st::downloadLoadingSize;
	const auto added = 3 * st::downloadLoadingLine;
	const auto skipx = st::downloadLoadingLeft;
	const auto skipy = (button->height() - size) / 2;
	const auto full = QRect(
		skipx - added,
		skipy - added,
		size + added * 2,
		size + added * 2);
	if (full.intersects(clip)) {
		const auto done = (finished == 1.);
		const auto loading = _radial.computeState();
		if (loading.shown > 0) {
			auto hq = PainterHighQualityEnabler(p);
			p.setOpacity(loading.shown);
			auto pen = st::windowBgActive->p;
			pen.setWidth(st::downloadLoadingLine);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			const auto m = added / 2.;
			auto rect = QRectF(full).marginsRemoved({ m, m, m, m });
			if (loading.arcLength < arc::kFullLength) {
				p.drawArc(rect, loading.arcFrom, loading.arcLength);
			} else {
				p.drawEllipse(rect);
			}
			p.setOpacity(1.);
		}
		const auto shift = st::downloadLoadingLine
			+ (1. - finished) * (added - st::downloadLoadingLine);
		const auto ellipse = QRectF(full).marginsRemoved(
			{ shift, shift, shift, shift });
		if (_thumbnail.isNull()) {
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgActive);
			p.drawEllipse(ellipse);
			const auto sizeLoading = st::downloadIconSize;
			if (finished == 0. || done) {
				const auto size = done
					? st::downloadIconSizeDone
					: sizeLoading;
				const auto image = done ? _documentIconDone : _documentIcon;
				p.drawImage(
					full.x() + (full.width() - size) / 2,
					full.y() + (full.height() - size) / 2,
					image);
			} else {
				auto hq = PainterHighQualityEnabler(p);
				const auto size = sizeLoading
					+ (st::downloadIconSizeDone - sizeLoading) * finished;
				p.drawImage(
					QRectF(
						full.x() + (full.width() - size) / 2.,
						full.y() + (full.height() - size) / 2.,
						size,
						size),
					_documentIconLarge);
			}
		} else if (finished == 0. || done) {
			p.drawImage(
				base::SafeRound(ellipse.x()),
				base::SafeRound(ellipse.y()),
				done ? _thumbnailDone : _thumbnail);
		} else {
			auto hq = PainterHighQualityEnabler(p);
			p.drawImage(ellipse, _thumbnailLarge);
		}
	}

	const auto minleft = std::min(
		st::downloadTitleLeft,
		st::downloadInfoLeft);
	const auto maxwidth = outerw - minleft;
	if (!clip.intersects({ minleft, 0, maxwidth, st::downloadBarHeight })) {
		return;
	}
	const auto right = st::downloadArrowRight + icon.width();
	const auto available = button->width() - st::downloadTitleLeft - right;
	p.setPen(st::windowBoldFg);
	_title.drawLeftElided(
		p,
		st::downloadTitleLeft,
		st::downloadTitleTop,
		available,
		outerw);

	p.setPen(st::windowSubTextFg);
	p.setTextPalette(st::defaultTextPalette);
	_info.drawLeftElided(
		p,
		st::downloadInfoLeft,
		st::downloadInfoTop,
		available,
		outerw);

	const auto iconTop = (st::downloadBarHeight - icon.height()) / 2;
	icon.paint(p, outerw - right, iconTop, outerw);
}

float64 DownloadBar::computeProgress() const {
	const auto now = _progress.current();
	if (now.efTotal > 0) {
		return now.efReady / float64(now.efTotal);
	} else if (now.total) {
		return now.ready / float64(now.total);
	} else if (_content.uploadSingleReady < _content.uploadSingleTotal
		&& _content.uploadSingleTotal > 0) {
		return _content.uploadSingleReady
			/ float64(_content.uploadSingleTotal);
	} else if (_content.uploadReady < _content.uploadTotal
		&& _content.uploadTotal > 0) {
		return _content.uploadReady / float64(_content.uploadTotal);
	}
	return 0.;
}

void DownloadBar::radialAnimationCallback(crl::time now) {
	const auto finished = (_content.done == _content.count)
		&& (_content.efDone == _content.efCount);
	const auto updated = _radial.update(computeProgress(), finished, now);
	if (!anim::Disabled() || updated) {
		const auto button = _button.entity();
		const auto size = st::downloadLoadingSize;
		const auto added = 3 * st::downloadLoadingLine;
		const auto skipx = st::downloadLoadingLeft;
		const auto skipy = (button->height() - size) / 2;
		const auto full = QRect(
			skipx - added,
			skipy - added,
			size + added * 2,
			size + added * 2);
		button->update(full);
	}
}

} // namespace Ui
