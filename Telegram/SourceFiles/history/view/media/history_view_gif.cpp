/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/media/history_view_gif.h"

#include "apiwrap.h"
#include "api/api_transcribes.h"
#include "lang/lang_keys.h"
#include "mainwindow.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "media/audio/media_audio.h"
#include "media/clip/media_clip_reader.h"
#include "media/player/media_player_instance.h"
#include "media/streaming/media_streaming_instance.h"
#include "media/streaming/media_streaming_player.h"
#include "media/streaming/media_streaming_utility.h"
#include "media/view/media_view_open_common.h"
#include "media/view/media_view_playback_progress.h"
#include "ui/boxes/confirm_box.h"
#include "ui/painter.h"
#include "ui/rect.h"
#include "history/history_item_components.h"
#include "history/history_item.h"
#include "history/history.h"
#include "history/view/history_view_element.h"
#include "history/history_item_helpers.h"
#include "base/unixtime.h"
#include "history/view/history_view_cursor_state.h"
#include "history/view/history_view_reply.h"
#include "history/view/history_view_transcribe_button.h"
#include "history/view/media/history_view_document.h" // TTLVoiceStops
#include "history/view/media/history_view_media_common.h"
#include "history/view/media/history_view_media_spoiler.h"
#include "window/window_session_controller.h"
#include "core/application.h" // Application::showDocument.
#include "core/core_settings.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/chat/chat_style.h"
#include "ui/image/image.h"
#include "ui/text/format_values.h"
#include "ui/grouped_layout.h"
#include "ui/cached_round_corners.h"
#include "ui/power_saving.h"
#include "ui/ui_utility.h"
#include "ui/effects/path_shift_gradient.h"
#include "ui/effects/spoiler_mess.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_streaming.h"
#include "data/data_document.h"
#include "data/data_file_click_handler.h"
#include "data/data_file_origin.h"
#include "data/data_document_media.h"
#include "data/data_web_page.h"
#include "storage/storage_account.h"
#include "styles/style_chat.h"

#include <QSvgRenderer>

namespace HistoryView {
namespace {

constexpr auto kMaxGifForwardedBarLines = 4;
constexpr auto kUseNonBlurredThreshold = 240;
constexpr auto kMaxInlineArea = 1920 * 1080;

[[nodiscard]] int GifMaxStatusWidth(not_null<DocumentData*> document) {
	auto result = st::normalFont->width(
		Ui::FormatDownloadText(document->size, document->size));
	accumulate_max(
		result,
		st::normalFont->width(Ui::FormatGifAndSizeText(document->size)));
	return result;
}

[[nodiscard]] HistoryView::TtlRoundPaintCallback CreateTtlPaintCallback(
		Fn<void()> update) {
	const auto centerMargins = Margins(st::historyFileInPause.width() * 3);

	const auto renderer = std::make_shared<QSvgRenderer>(
		u":/gui/ttl/video_message_icon.svg"_q);

	return [=](QPainter &p, QRect r, const PaintContext &context) {
		const auto centerRect = r - centerMargins;
		const auto &icon = context.imageStyle()->historyVideoMessageTtlIcon;
		const auto iconRect = QRect(
			rect::right(centerRect) - icon.width() * 0.75,
			rect::bottom(centerRect) - icon.height() * 0.75,
			icon.width(),
			icon.height());
		{
			auto hq = PainterHighQualityEnabler(p);
			auto path = QPainterPath();
			path.setFillRule(Qt::WindingFill);
			path.addEllipse(centerRect);
			path.addEllipse(iconRect);
			p.fillPath(path, st::shadowFg);
			p.fillPath(path, st::shadowFg);
			p.fillPath(path, st::shadowFg);
		}

		renderer->render(&p, centerRect - Margins(centerRect.width() / 4));

		icon.paint(p, iconRect.topLeft(), centerRect.width());
	};
}

} // namespace

struct Gif::Streamed {
	Streamed(
		not_null<DocumentData*> chosen,
		std::shared_ptr<::Media::Streaming::Document> shared,
		Fn<void()> waitingCallback);
	const not_null<DocumentData*> chosen;
	::Media::Streaming::Instance instance;
	::Media::Streaming::FrameRequest frozenRequest;
	QImage frozenFrame;
	QString frozenStatusText;
};

Gif::Streamed::Streamed(
	not_null<DocumentData*> chosen,
	std::shared_ptr<::Media::Streaming::Document> shared,
	Fn<void()> waitingCallback)
: chosen(chosen)
, instance(std::move(shared), std::move(waitingCallback)) {
}

[[nodiscard]] bool IsHiddenRoundMessage(not_null<Element*> parent) {
	return parent->delegate()->elementContext() != Context::TTLViewer
		&& parent->data()->media()
		&& parent->data()->media()->ttlSeconds();
}

Gif::Gif(
	not_null<Element*> parent,
	not_null<HistoryItem*> realParent,
	not_null<DocumentData*> document,
	bool spoiler)
: File(parent, realParent)
, _data(document)
, _videoCover(LookupVideoCover(document, realParent))
, _storyId(realParent->media()
	? realParent->media()->storyId()
	: FullStoryId())
, _spoiler((spoiler
	|| IsHiddenRoundMessage(_parent)
	|| realParent->isMediaSensitive())
	? std::make_unique<MediaSpoiler>()
	: nullptr)
, _downloadSize(Ui::FormatSizeText(_data->size))
, _videoTimestamp(::Media::View::ExtractVideoTimestamp(realParent))
, _sensitiveSpoiler(realParent->isMediaSensitive())
, _hasVideoCover(realParent->media() && realParent->media()->videoCover()) {
	if (_data->isVideoMessage() && _parent->data()->media()->ttlSeconds()) {
		if (_spoiler) {
			_drawTtl = CreateTtlPaintCallback([=] { repaint(); });
		}
		const auto fullId = _realParent->fullId();
		const auto &data = &_parent->data()->history()->owner();
		const auto isOut = _parent->data()->out();
		_parent->data()->removeFromSharedMediaIndex();
		setDocumentLinks(_data, realParent, [=] {
			auto lifetime = std::make_shared<rpl::lifetime>();
			TTLVoiceStops(fullId) | rpl::on_next([=]() mutable {
				if (lifetime) {
					base::take(lifetime)->destroy();
				}
				if (!isOut) {
					if (const auto item = data->message(fullId)) {
						// Destroys this.
						item->clearMediaAsExpired();
					}
				}
			}, *lifetime);

			return false;
		});
	} else {
		setDocumentLinks(_data, realParent, [=] {
			if (!_data->createMediaView()->canBePlayed()
				|| !_data->isAnimation()
				|| _data->isVideoMessage()
				|| !CanPlayInline(_data)) {
				return false;
			}
			playAnimation(false);
			return true;
		});
	}

	setStatusSize(Ui::FileStatusSizeReady);

	if (_spoiler) {
		createSpoilerLink(_spoiler.get());
	}

	if ((_dataMedia = _data->activeMediaView())) {
		dataMediaCreated();
	} else if (_videoCover) {
		if (_videoCover->inlineThumbnailBytes().isEmpty()
			&& (_videoCover->hasExact(Data::PhotoSize::Small)
				|| _videoCover->hasExact(Data::PhotoSize::Thumbnail))) {
			_videoCover->load(Data::PhotoSize::Small, realParent->fullId());
		}
	} else {
		_data->loadThumbnail(realParent->fullId());
		if (!autoplayEnabled()) {
			_data->loadVideoThumbnail(realParent->fullId());
		}
	}
	ensureTranscribeButton();

	_purchasedPriceTag = hasPurchasedTag();
}

Gif::~Gif() {
	if (_streamed || _dataMedia) {
		if (_streamed) {
			_data->owner().streaming().keepAlive(_data);
			setStreamed(nullptr);
		}
		if (_dataMedia) {
			_data->owner().keepAlive(base::take(_dataMedia));
			_parent->checkHeavyPart();
		}
	}
	togglePollingStory(false);
}

bool Gif::CanPlayInline(not_null<DocumentData*> document) {
	const auto dimensions = document->dimensions;
	return dimensions.width() * dimensions.height() <= kMaxInlineArea;
}

QSize Gif::sizeForAspectRatio() const {
	if (!_data->dimensions.isEmpty()) {
		return _data->dimensions;
	}
	if (_videoCover) {
		return { _videoCover->width(), _videoCover->height() };
	} else if (_data->hasThumbnail()) {
		const auto &location = _data->thumbnailLocation();
		return { location.width(), location.height() };
	}
	return { 1, 1 };
}

QSize Gif::countThumbSize(int &inOutWidthMax) const {
	const auto maxSize = _data->isVideoFile()
		? st::maxMediaSize
		: _data->isVideoMessage()
		? st::maxVideoMessageSize
		: st::maxGifSize;
	const auto size = style::ConvertScale(videoSize());
	accumulate_min(inOutWidthMax, maxSize);
	auto result = DownscaledSize(size, { inOutWidthMax, maxSize });
	if (_data->isVideoMessage()) {
		result.setHeight(result.height() + st::msgDateFont->height + 2 * st::msgDateImgPadding.y() + 2);
	}
	return result;
}

QSize Gif::countOptimalSize() {
	if (_data->isVideoMessage() && _transcribe) {
		const auto &entry = _data->session().api().transcribes().entry(
			_realParent);
		_transcribe->setLoading(
			entry.shown && (entry.requestId || entry.pending));
	}

	const auto minWidth = std::clamp(
		_parent->minWidthForMedia(),
		(_parent->hasBubble()
			? st::historyPhotoBubbleMinWidth
			: st::minPhotoSize),
		st::maxMediaSize);
	auto thumbMaxWidth = st::msgMaxWidth;
	const auto scaled = countThumbSize(thumbMaxWidth);
	auto maxWidth = std::min(
		std::max(scaled.width(), minWidth),
		thumbMaxWidth);
	auto minHeight = qMax(scaled.height(), st::minPhotoSize);
	if (!activeCurrentStreamed()) {
		accumulate_max(
			maxWidth,
			GifMaxStatusWidth(_data)
				+ 2 * (st::msgDateImgDelta + st::msgDateImgPadding.x()));
	}
	if (_parent->hasBubble()) {
		maxWidth = qMax(maxWidth, _parent->textualMaxWidth());
		minHeight = adjustHeightForLessCrop(
			scaled,
			{ maxWidth, minHeight });
	} else if (isUnwrapped()) {
		const auto item = _parent->data();
		auto via = item->Get<HistoryMessageVia>();
		auto reply = _parent->Get<Reply>();
		auto forwarded = item->Get<HistoryMessageForwarded>();
		if (forwarded) {
			forwarded->create(via, item);
		}
		maxWidth += additionalWidth(reply, via, forwarded);
		accumulate_max(maxWidth, _parent->reactionsOptimalWidth());
	}
	return { maxWidth, minHeight };
}

QSize Gif::countCurrentSize(int newWidth) {
	auto availableWidth = newWidth;

	auto thumbMaxWidth = newWidth;
	const auto scaled = countThumbSize(thumbMaxWidth);
	const auto minWidthByInfo = _parent->infoWidth()
		+ 2 * (st::msgDateImgDelta + st::msgDateImgPadding.x());
	newWidth = std::clamp(
		std::max(scaled.width(), minWidthByInfo),
		st::minPhotoSize,
		thumbMaxWidth);
	auto newHeight = qMax(scaled.height(), st::minPhotoSize);
	if (!activeCurrentStreamed()) {
		accumulate_max(
			newWidth,
			GifMaxStatusWidth(_data)
				+ 2 * (st::msgDateImgDelta + st::msgDateImgPadding.x()));
	}
	if (_parent->hasBubble()) {
		accumulate_max(newWidth, _parent->minWidthForMedia());
		auto captionMaxWidth = _parent->textualMaxWidth();
		const auto botTop = _parent->Get<FakeBotAboutTop>();
		if (botTop) {
			accumulate_max(captionMaxWidth, botTop->maxWidth);
		}
		const auto maxWithCaption = qMin(st::msgMaxWidth, captionMaxWidth);
		newWidth = qMin(qMax(newWidth, maxWithCaption), thumbMaxWidth);
		newHeight = adjustHeightForLessCrop(
			scaled,
			{ newWidth, newHeight });
	} else if (isUnwrapped()) {
		accumulate_max(newWidth, _parent->reactionsOptimalWidth());

		const auto item = _parent->data();
		auto via = item->Get<HistoryMessageVia>();
		auto reply = _parent->Get<Reply>();
		auto forwarded = item->Get<HistoryMessageForwarded>();
		if (via || reply || forwarded) {
			auto additional = additionalWidth(reply, via, forwarded);
			newWidth += additional;
			accumulate_min(newWidth, availableWidth);
			auto usew = maxWidth() - additional;
			auto availw = newWidth - usew - st::msgReplyPadding.left() - st::msgReplyPadding.left() - st::msgReplyPadding.left();
			if (!forwarded && via) {
				via->resize(availw);
			}
			if (reply) {
				[[maybe_unused]] int height = reply->resizeToWidth(availw);
			}
		}
	}

	return { newWidth, newHeight };
}

int Gif::adjustHeightForLessCrop(QSize dimensions, QSize current) const {
	if (dimensions.isEmpty()) {
		return current.height();
	}
	// Allow some more vertical space for less cropping,
	// but not more than 1.33 * existing height.
	return qMax(
		current.height(),
		qMin(
			current.width() * dimensions.height() / dimensions.width(),
			current.height() * 4 / 3));
}

QSize Gif::videoSize() const {
	if (const auto streamed = activeCurrentStreamed()) {
		return streamed->player().videoSize();
	} else if (_videoCover) {
		return QSize(_videoCover->width(), _videoCover->height());
	} else if (!_data->dimensions.isEmpty()) {
		return _data->dimensions;
	} else if (_data->hasThumbnail()) {
		const auto &location = _data->thumbnailLocation();
		return QSize(location.width(), location.height());
	} else {
		return QSize(1, 1);
	}
}

void Gif::validateRoundingMask(QSize size) const {
	if (_roundingMask.size() != size) {
		const auto ratio = style::DevicePixelRatio();
		_roundingMask = Images::EllipseMask(size / ratio);
	}
}

bool Gif::downloadInCorner() const {
	return _data->isVideoFile()
		&& _data->canBeStreamed()
		&& !_data->inappPlaybackFailed();
}

bool Gif::autoplayUnderCursor() const {
	return (_videoTimestamp || _hasVideoCover);
}

bool Gif::underCursor() const {
	return ClickHandler::getActive() == currentVideoLink();
}

bool Gif::autoplayEnabled() const {
	auto peerId = _parent->data()->from() ? _parent->data()->from()->id : PeerId(0);
	auto user = history()->session().data().peerLoaded(_parent->data()->from() ? _parent->data()->from()->id : PeerId(0));
	if ((GetEnhancedBool("blocked_user_spoiler_mode") && blockExist(peerId.value)) || (GetEnhancedBool("blocked_user_spoiler_mode") && user && user->isBlocked())) {
		return false;
	}
	return Data::AutoDownload::ShouldAutoPlay(
		_data->session().settings().autoDownload(),
		_realParent->history()->peer,
		_data);
}

bool Gif::hideMessageText() const {
	return _data->isVideoMessage();
}

void Gif::draw(Painter &p, const PaintContext &context) const {
	if (width() < st::msgPadding.left() + st::msgPadding.right() + 1) return;

	_smallGroupPart = false;

	// Calculate showInfo for hover effect
	const bool showInfo = _parent->isUnderCursor() || context.selected();

	ensureDataMediaCreated();
	const auto item = _parent->data();
	const auto loaded = dataLoaded();
	const auto displayLoading = (item->isSending() || _data->displayLoading());
	const auto st = context.st;
	const auto sti = context.imageStyle();
	const auto cornerDownload = downloadInCorner();
	const auto canBePlayed = _dataMedia->canBePlayed();
	const auto autoplay = autoplayEnabled()
		&& canBePlayed
		&& CanPlayInline(_data);
	const auto activeRoundPlaying = activeRoundStreamed();

	auto paintx = 0, painty = 0, paintw = width(), painth = height();
	const bool bubble = _parent->hasBubble();
	const auto rightLayout = _parent->hasRightLayout();
	const auto inWebPage = (_parent->media() != this);
	const auto isRound = _data->isVideoMessage();

	const auto rounding = (inWebPage
			// Dangerous change.
			&& bubbleRounding() == Ui::BubbleRounding())
		? std::optional<Ui::BubbleRounding>()
		: adjustedBubbleRounding();

	auto usex = 0, usew = paintw;
	const auto unwrapped = isUnwrapped();
	const auto via = unwrapped ? item->Get<HistoryMessageVia>() : nullptr;
	const auto reply = unwrapped ? _parent->Get<Reply>() : nullptr;
	const auto forwarded = unwrapped ? item->Get<HistoryMessageForwarded>() : nullptr;
	const auto rightAligned = unwrapped && rightLayout;
	if (via || reply || forwarded) {
		usew = maxWidth() - additionalWidth(reply, via, forwarded);
		if (rightAligned) {
			usex = width() - usew;
		}
	}
	if (isRound) {
		accumulate_min(usew, painth);
	}
	if (rtl()) usex = width() - usex - usew;

	QRect rthumb(style::rtlrect(usex + paintx, painty, usew, isRound ? usew : painth, width()));

	const auto inTTLViewer = _parent->delegate()->elementContext()
		== Context::TTLViewer;
	const auto revealed = (isRound
			&& item->media()->ttlSeconds()
			&& !inTTLViewer)
		? 0
		: (!isRound && _spoiler)
		? _spoiler->revealAnimation.value(_spoiler->revealed ? 1. : 0.)
		: 1.;
	const auto fullHiddenBySpoiler = (revealed == 0.);
	if (revealed < 1.) {
		validateSpoilerImageCache(rthumb.size(), rounding);
	}

	const auto canStartPlay = autoplay
		&& !_streamed
		&& !activeRoundPlaying
		&& !fullHiddenBySpoiler;
	const auto shouldBePlaying = !autoplayUnderCursor() || underCursor();
	if (!shouldBePlaying && _videoTimestamp != 0) {
		const_cast<Gif*>(this)->stopAnimation();
	} else if (canStartPlay) {
		const_cast<Gif*>(this)->playAnimation(true);
	} else {
		checkStreamedIsStarted();
	}
	const auto streamingMode = _streamed || activeRoundPlaying || autoplay;
	const auto activeOwnPlaying = activeOwnStreamed();

	auto displayMute = false;
	const auto streamed = activeRoundPlaying
		? activeRoundPlaying
		: activeOwnPlaying
		? &activeOwnPlaying->instance
		: nullptr;
	const auto streamedForWaiting = activeRoundPlaying
		? activeRoundPlaying
		: _streamed
		? &_streamed->instance
		: nullptr;

	if (displayLoading
		&& (!streamedForWaiting
			|| item->isSending()
			|| _data->uploading()
			|| (cornerDownload && _data->loading()))) {
		ensureAnimation();
		if (!_animation->radial.animating()) {
			_animation->radial.start(dataProgress());
		}
	}
	updateStatusText();
	const auto radial = isRadialAnimation()
		|| (streamedForWaiting && streamedForWaiting->waitingShown());

	if (!bubble && !unwrapped) {
		Assert(rounding.has_value());
		fillImageShadow(p, rthumb, *rounding, context);
	}

	const auto skipDrawingContent = context.skipDrawingParts
		== PaintContext::SkipDrawingParts::Content;
	const auto drawStreamed = streamed && (shouldBePlaying || !_videoCover);
	if (drawStreamed && !skipDrawingContent && !fullHiddenBySpoiler) {
		auto paused = context.paused || !shouldBePlaying;
		auto request = ::Media::Streaming::FrameRequest{
			.outer = QSize(usew, painth) * style::DevicePixelRatio(),
			.blurredBackground = true,
		};
		if (isRound) {
			if (activeRoundStreamed()) {
				paused = false;
			} else {
				displayMute = true;
			}
			validateRoundingMask(request.outer);
			request.mask = _roundingMask;
		} else {
			request.rounding = MediaRoundingMask(rounding);
		}
		if (!activeRoundPlaying && activeOwnPlaying->instance.playerLocked()) {
			if (activeOwnPlaying->frozenFrame.isNull()) {
				activeOwnPlaying->frozenRequest = request;
				activeOwnPlaying->frozenFrame = streamed->frame(request);
				activeOwnPlaying->frozenStatusText = _statusText;
			} else if (activeOwnPlaying->frozenRequest != request) {
				activeOwnPlaying->frozenRequest = request;
				activeOwnPlaying->frozenFrame = streamed->frame(request);
			}
			p.drawImage(rthumb, activeOwnPlaying->frozenFrame);
		} else {
			if (activeOwnPlaying
				&& !activeOwnPlaying->frozenFrame.isNull()) {
				activeOwnPlaying->frozenFrame = QImage();
				activeOwnPlaying->frozenStatusText = QString();
			}

			const auto frame = streamed->frameWithInfo(request);
			p.drawImage(rthumb, frame.image);
			if (!paused) {
				streamed->markFrameShown();
			}
		}

		if (const auto playback = videoPlayback()) {
			const auto value = playback->value();
			if (value > 0.) {
				auto pen = st->historyVideoMessageProgressFg()->p;
				const auto was = p.pen();
				pen.setWidth(st::radialLine);
				pen.setCapStyle(Qt::RoundCap);
				p.setPen(pen);
				p.setOpacity(st::historyVideoMessageProgressOpacity);

				const auto from = arc::kQuarterLength;
				const auto len = std::round(arc::kFullLength
					* (inTTLViewer ? (1. - value) : -value));
				const auto stepInside = st::radialLine / 2;
				{
					auto hq = PainterHighQualityEnabler(p);
					p.drawArc(rthumb - Margins(stepInside), from, len);
				}

				p.setPen(was);
				p.setOpacity(1.);
			}
		}
	} else if (!skipDrawingContent && !fullHiddenBySpoiler) {
		ensureDataMediaCreated();
		validateThumbCache({ usew, painth }, isRound, rounding);
		p.drawImage(rthumb, _thumbCache);
	}
	if (!isRound) {
		paintTimestampMark(p, rthumb, rounding);
	}

	if (revealed < 1.) {
		p.setOpacity(1. - revealed);
		if (!isRound) {
			p.drawImage(rthumb.topLeft(), _spoiler->background);
			fillImageSpoiler(p, _spoiler.get(), rthumb, context);
		} else {
			auto frame = _spoiler->background;
			{
				auto q = QPainter(&frame);
				fillImageSpoiler(q, _spoiler.get(), rthumb, context);
			}
			p.drawImage(rthumb.topLeft(), Images::Circle(std::move(frame)));
		}
		p.setOpacity(1.);
	}
	if (context.selected()) {
		if (isRound) {
			Ui::FillComplexEllipse(p, st, rthumb);
		} else {
			fillImageOverlay(p, rthumb, rounding, context);
		}
	}

	const auto paintInCenter = !_sensitiveSpoiler
		&& (radial
			|| (!streamingMode
				&& ((!loaded && !_data->loading()) || !autoplay)));
	if (paintInCenter) {
		const auto radialRevealed = 1.;
		const auto opacity = (item->isSending() || _data->uploading())
			? 1.
			: streamedForWaiting
			? streamedForWaiting->waitingOpacity()
			: (radial && loaded)
			? _animation->radial.opacity()
			: 1.;
		const auto radialOpacity = opacity * radialRevealed;
		const auto innerSize = st::msgFileLayout.thumbSize;
		auto inner = QRect(rthumb.x() + (rthumb.width() - innerSize) / 2, rthumb.y() + (rthumb.height() - innerSize) / 2, innerSize, innerSize);
		p.setPen(Qt::NoPen);
		if (context.selected()) {
			p.setBrush(st->msgDateImgBgSelected());
		} else if (isThumbAnimation()) {
			auto over = _animation->a_thumbOver.value(1.);
			p.setBrush(anim::brush(st->msgDateImgBg(), st->msgDateImgBgOver(), over));
		} else {
			const auto over = ClickHandler::showAsActive(
				(_data->loading() || _data->uploading()) ? _cancell : _savel);
			p.setBrush(over ? st->msgDateImgBgOver() : st->msgDateImgBg());
		}
		p.setOpacity(radialOpacity * p.opacity());

		{
			PainterHighQualityEnabler hq(p);
			p.drawEllipse(inner);
		}

		p.setOpacity(radialOpacity);
		const auto icon = [&]() -> const style::icon * {
			if (streamingMode && !_data->uploading()) {
				return nullptr;
			} else if ((loaded || canBePlayed) && (!radial || cornerDownload)) {
				return &sti->historyFileThumbPlay;
			} else if (radial || _data->loading()) {
				if (!item->isSending() || _data->uploading()) {
					return &sti->historyFileThumbCancel;
				}
				return nullptr;
			}
			return &sti->historyFileThumbDownload;
		}();
		const auto previous = _data->waitingForAlbum()
			? &sti->historyFileThumbCancel
			: nullptr;
		if (icon) {
			if (previous && radialOpacity > 0. && radialOpacity < 1.) {
				PaintInterpolatedIcon(p, *icon, *previous, radialOpacity, inner);
			} else {
				icon->paintInCenter(p, inner);
			}
		}
		p.setOpacity(radialRevealed);
		if (radial) {
			const auto line = st::historyGroupRadialLine;
			const auto rinner = inner.marginsRemoved({ line, line, line, line });
			if (streamedForWaiting && !_data->uploading()) {
				Ui::InfiniteRadialAnimation::Draw(
					p,
					streamedForWaiting->waitingState(),
					rinner.topLeft(),
					rinner.size(),
					width(),
					sti->historyFileThumbRadialFg,
					st::msgFileRadialLine);
			} else if (!cornerDownload) {
				_animation->radial.draw(
					p,
					rinner,
					st::msgFileRadialLine,
					sti->historyFileThumbRadialFg);
			}
		}
		p.setOpacity(1.);
	} else if (_sensitiveSpoiler) {
		drawSpoilerTag(p, rthumb, context, [&] {
			return spoilerTagBackground();
		});
	}
	if (displayMute) {
		auto muteRect = style::rtlrect(rthumb.x() + (rthumb.width() - st::historyVideoMessageMuteSize) / 2, rthumb.y() + st::msgDateImgDelta, st::historyVideoMessageMuteSize, st::historyVideoMessageMuteSize, width());
		p.setPen(Qt::NoPen);
		p.setBrush(sti->msgDateImgBg);
		PainterHighQualityEnabler hq(p);
		p.drawEllipse(muteRect);
		sti->historyVideoMessageMute.paintInCenter(p, muteRect);
	}

	const auto skipDrawingSurrounding = context.skipDrawingParts
		== PaintContext::SkipDrawingParts::Surrounding;

	if (!skipDrawingSurrounding && _purchasedPriceTag) {
		drawPurchasedTag(p, rthumb, context);
	}

	if (!unwrapped && !skipDrawingSurrounding) {
		const auto sponsoredSkip = !_data->isVideoFile()
			&& _realParent->isSponsored();
		if ((!isRound || !inWebPage) && !sponsoredSkip) {
			drawCornerStatus(p, context, QPoint());

			// --- NEW: Bottom-Right Info Bubble for Single Video ---
			// Only show if media is loaded (cover/thumbnail/video) and it's not a round message
			// and ONLY if we should show info (hover/selected).
			if (!isRound && !sponsoredSkip && showInfo && (loaded || _data->hasThumbnail() || _data->duration() > 0 || _data->size > 0)) {
				qint64 durSeconds = std::max<qint64>(0, _data->duration() / 1000);
				qint64 sizeBytes = _data->size;

				if (durSeconds >= 0 && sizeBytes > 0) {
					const auto font = st::msgDateFont;
					const auto sti = context.imageStyle();
					const auto text = Ui::FormatDurationText(durSeconds) + QChar(' ') + Ui::FormatSizeText(sizeBytes);
					const auto textWidth = font->width(text);
					const auto textHeight = font->height;
					const auto hPadding = 2;
					const auto vPadding = st::msgDateImgPadding.y();
					const auto bubbleW = textWidth + 2 * hPadding;
					const auto bubbleH = textHeight + 2 * vPadding;

					const auto bubbleX = width() - bubbleW - st::msgDateImgDelta;
					const auto bubbleY = height() - bubbleH - st::msgDateImgDelta;

					p.save();
					p.setOpacity(0.95);
					Ui::FillRoundRect(
						p,
						bubbleX,
						bubbleY,
						bubbleW,
						bubbleH,
						sti->msgDateImgBg,
						sti->msgDateImgBgCorners);
					p.restore();

					p.setPen(st->msgDateImgFg());
					p.setFont(font->bold());
					const auto baseY = bubbleY + (bubbleH - textHeight) / 2 + font->ascent;
					p.drawText(bubbleX + hPadding, baseY, text);
				}
			}
			// -----------------------------------------------------
		}
	} else if (!skipDrawingSurrounding) {
		if (isRound) {
			const auto mediaUnread = item->hasUnreadMediaFlag();
			auto statusW = st::normalFont->width(_statusText) + 2 * st::msgDateImgPadding.x();
			auto statusH = st::normalFont->height + 2 * st::msgDateImgPadding.y();
			auto statusX = usex + paintx + st::msgDateImgDelta + st::msgDateImgPadding.x();
			auto statusY = painty + painth - st::msgDateImgDelta - statusH + st::msgDateImgPadding.y();
			if (mediaUnread) {
				statusW += st::mediaUnreadSkip + st::mediaUnreadSize;
			}
			Ui::FillRoundRect(p, style::rtlrect(statusX - st::msgDateImgPadding.x(), statusY - st::msgDateImgPadding.y(), statusW, statusH, width()), sti->msgServiceBg, sti->msgServiceBgCornersSmall);
			p.setFont(st::normalFont);
			// Use consistent edited glyph and color for status; rely on this item's edited state
			const auto editedGlyph = (item->Get<HistoryMessageEdited>() && !item->hideEditedBadge())
				? (QString::fromUtf8("✏️") + " ")
				: QString();
			const auto statusText = editedGlyph + _statusText;
			p.setPen(st->msgDateImgFg());
			p.drawTextLeft(statusX, statusY, width(), statusText, statusW - 2 * st::msgDateImgPadding.x());
			if (mediaUnread) {
				p.setPen(Qt::NoPen);
				p.setBrush(st->msgServiceFg());

				{
					PainterHighQualityEnabler hq(p);
					p.drawEllipse(style::rtlrect(statusX - st::msgDateImgPadding.x() + statusW - st::msgDateImgPadding.x() - st::mediaUnreadSize, statusY + st::mediaUnreadTop, st::mediaUnreadSize, st::mediaUnreadSize, width()));
				}
			}
			ensureTranscribeButton();
		}
		if (via || reply || forwarded) {
			auto rectw = width() - usew - st::msgReplyPadding.left();
			auto innerw = rectw - (st::msgReplyPadding.left() + st::msgReplyPadding.right());
			auto recth = 0;
			auto forwardedHeightReal = forwarded ? forwarded->text.countHeight(innerw) : 0;
			auto forwardedHeight = qMin(forwardedHeightReal, kMaxGifForwardedBarLines * st::msgServiceNameFont->height);
			if (forwarded) {
				recth += st::msgReplyPadding.top() + forwardedHeight;
			} else if (via) {
				recth += st::msgReplyPadding.top() + st::msgServiceNameFont->height + (reply ? st::msgReplyPadding.top() : 0);
			}
			if (reply) {
				const auto replyMargins = reply->margins();
				recth += reply->height()
					- ((forwarded || via) ? 0 : replyMargins.top())
					- replyMargins.bottom();
			} else {
				recth += st::msgReplyPadding.bottom();
			}
			int rectx = rightAligned ? 0 : (usew + st::msgReplyPadding.left());
			int recty = painty;
			if (rtl()) rectx = width() - rectx - rectw;

			Ui::FillRoundRect(p, rectx, recty, rectw, recth, sti->msgServiceBg, sti->msgServiceBgCornersSmall);
			p.setPen(st->msgServiceFg());
			const auto textx = rectx + st::msgReplyPadding.left();
			const auto textw = rectw - st::msgReplyPadding.left() - st::msgReplyPadding.right();
			if (forwarded) {
				p.setTextPalette(st->serviceTextPalette());
				auto breakEverywhere = (forwardedHeightReal > forwardedHeight);
				forwarded->text.drawElided(p, textx, recty + st::msgReplyPadding.top(), textw, kMaxGifForwardedBarLines, style::al_left, 0, -1, 0, breakEverywhere);
				p.restoreTextPalette();

				const auto skip = std::min(
					forwarded->text.countHeight(textw),
					kMaxGifForwardedBarLines * st::msgServiceNameFont->height);
				recty += skip;
			} else if (via) {
				p.setFont(st::msgServiceNameFont);
				p.drawTextLeft(textx, recty + st::msgReplyPadding.top(), 2 * textx + textw, via->text);
				int skip = st::msgServiceNameFont->height + (reply ? st::msgReplyPadding.top() : 0);
				recty += skip;
			}
			if (reply) {
				if (forwarded || via) {
					recty += st::msgReplyPadding.top();
					recth -= st::msgReplyPadding.top();
				} else {
					recty -= reply->margins().top();
				}
				reply->paint(p, _parent, context, rectx, recty, rectw, false);
			}
		}
	}
	if (!inWebPage && !skipDrawingSurrounding) {
		auto fullRight = paintx + usex + usew;
		auto fullBottom = painty + painth;
		auto maxRight = _parent->width() - st::msgMargin.left();
		if (_parent->hasFromPhoto()) {
			maxRight -= st::msgMargin.right();
		} else {
			maxRight -= st::msgMargin.left();
		}
		if (unwrapped && !rightAligned) {
			auto infoWidth = _parent->infoWidth();
			fullRight += infoWidth - st::normalFont->height;
			if (fullRight > maxRight) {
				fullRight = maxRight;
			}
		}
		
		if (isRound) {
			// Helper to get formatted local date/time

			const auto font = st::msgDateFont;
			const auto sti = context.imageStyle();
			const auto views = item->Get<HistoryMessageViews>();
			const auto viewsText = (views && views->views.count >= 0)
				? Lang::FormatCountToShort(std::max(views->views.count, 1)).string
				: QString();
			
			const auto authorName = [&] {
				if (const auto msgsigned = item->Get<HistoryMessageSigned>()) {
					return msgsigned->author;
				}
				return item->from()->name();
			}();
			const auto timeText = QLocale().toString(
				ItemDateTime(item).time(),
				GetEnhancedBool("show_seconds")
					? QLocale::system().timeFormat(QLocale::LongFormat).remove("t")
					: QLocale::system().timeFormat(QLocale::ShortFormat));
			const auto msgIdText = (GetEnhancedBool("show_messages_id") && item->fullId().msg > 0)
				? QString(" %1").arg(item->fullId().msg.bare)
				: QString();

			// Construct display string: "[ViewsIcon] [Count] [AuthorName] [Time] [ID]"
			// Note: We'll draw the icon manually on the far left.
			QString displayText = viewsText;
			if (!authorName.isEmpty()) displayText += " " + authorName;
			displayText += " " + timeText + msgIdText;

			const int textWidth = font->width(displayText);
			const int textHeight = font->height;
			const int iconGap = 3;
			const int iconW = st::historyViewsWidth;
			const int hPadding = st::msgDateImgPadding.x();
			const int vPadding = st::msgDateImgPadding.y();
			
			int totalW = hPadding + (viewsText.isEmpty() ? 0 : (iconW + iconGap)) + textWidth + hPadding;
			int totalH = textHeight + 2 * vPadding;

			int bubbleX = fullRight - totalW - st::msgDateImgDelta;
			int bubbleY = fullBottom - totalH - st::msgDateImgDelta;

			// Draw Bubble Background
			p.save();
			p.setOpacity(0.95);
			Ui::FillRoundRect(
				p,
				bubbleX,
				bubbleY,
				totalW,
				totalH,
				(unwrapped ? sti->msgServiceBg : sti->msgDateImgBg),
				(unwrapped ? sti->msgServiceBgCornersSmall : sti->msgDateImgBgCorners));
			p.restore();

			// Draw Content
			p.setPen(unwrapped ? st->msgServiceFg() : st->msgDateImgFg());
			p.setFont(font->bold());
			int currentX = bubbleX + hPadding;
			const int textBaseY = bubbleY + vPadding + font->ascent;

			if (!viewsText.isEmpty()) {
				const auto &icon = st->historyViewsInvertedIcon();
				const int baseIconW = std::max(1, icon.width());
				const int baseIconH = icon.height();
				const int scaledIconH = (baseIconH * iconW) / baseIconW;
				icon.paint(p, currentX, bubbleY + (totalH - scaledIconH) / 2 + 1, iconW);
				currentX += iconW + iconGap;
			}
			p.drawText(currentX, textBaseY, displayText);
		}

		if (const auto size = bubble ? std::nullopt : _parent->rightActionSize()
			; size || (_transcribe && !rightAligned)) {
			const auto rightActionWidth = size
				? size->width()
				: _transcribe->size().width();
			auto fastShareLeft = rightLayout
				? (paintx + usex - size->width() - st::historyFastShareLeft)
				: (fullRight + st::historyFastShareLeft);
			auto fastShareTop = fullBottom
				- st::historyFastShareBottom
				- (size ? size->height() : 0);
			if (fastShareLeft + rightActionWidth > maxRight) {
				fastShareLeft = fullRight
					- rightActionWidth
					- st::msgDateImgDelta;
				fastShareTop -= st::msgDateImgDelta
					+ st::msgDateImgPadding.y()
					+ st::msgDateFont->height
					+ st::msgDateImgPadding.y();
			}
			if (size) {
				_parent->drawRightAction(p, context, fastShareLeft, fastShareTop, 2 * paintx + paintw);
			}
			if (_transcribe) {
				paintTranscribe(p, fastShareLeft, fastShareTop, true, context);
			}
		} else if (rightAligned && _transcribe) {
			paintTranscribe(p, usex, fullBottom, false, context);
		}
	}
	if (_drawTtl) {
		_drawTtl(p, rthumb, context);
	}

	// --- CUSTOM TOP-RIGHT BUBBLE (For Standard Videos) ---
	// Always draw top-right for videos even if caption exists.
	// Only draw on hover/select.
	if (!isRound && !inWebPage && !_parent->data()->isFakeAboutView() && showInfo) {
		// Local definition to fix compilation error

		const auto font = st::msgDateFont;
		p.setFont(font);

		const auto edited = item->Get<HistoryMessageEdited>() && !item->hideEditedBadge();
		const auto dateText = QLocale().toString(
			ItemDateTime(item).time(),
			GetEnhancedBool("show_seconds")
				? QLocale::system().timeFormat(QLocale::LongFormat).remove("t")
				: QLocale::system().timeFormat(QLocale::ShortFormat));

		const auto msgIdText = (GetEnhancedBool("show_messages_id") && item->fullId().msg > 0)
			? QString(" %1").arg(item->fullId().msg.bare)
			: QString();

		const auto views = item->Get<HistoryMessageViews>();
		const auto viewsText = (views && views->views.count >= 0)
			? Lang::FormatCountToShort(std::max(views->views.count, 1)).string
			: QString();

		int totalWidth = 0;
		const int textPadding = font->width(' ');
		totalWidth += font->width(dateText + msgIdText);

		if (edited) {
			totalWidth += textPadding + font->width(QString::fromUtf8("✏️"));
		}

		int viewsWidth = 0;
		if (!viewsText.isEmpty()) {
			viewsWidth = st::historyViewsWidth + 1 + font->width(viewsText);
			totalWidth += (2 * textPadding) + viewsWidth;
		}

		const auto textHeight = font->height;
		const auto hPadding = 2;
		const auto vPadding = st::msgDateImgPadding.y();
		const auto bubbleW = totalWidth + 2 * hPadding;
		const auto bubbleH = textHeight + 2 * vPadding;

		// Position: Top-Right
		const auto bubbleX = width() - bubbleW - st::msgDateImgDelta;
		const auto bubbleY = st::msgDateImgDelta;

		p.save();
		p.setOpacity(0.95);
		Ui::FillRoundRect(p, bubbleX, bubbleY, bubbleW, bubbleH, sti->msgDateImgBg, sti->msgDateImgBgCorners);
		p.restore();

		p.setPen(st->msgDateImgFg());
		p.setFont(font->bold());
		const int textBaseY = bubbleY + (bubbleH - textHeight) / 2 + font->ascent;
		int currentLeft = bubbleX + hPadding;

		if (!viewsText.isEmpty()) {
			const auto &icon = st->historyViewsInvertedIcon();
			const int baseIconW = std::max(1, icon.width());
			const int baseIconH = icon.height();
			const int scaledIconH = (baseIconH * st::historyViewsWidth) / baseIconW;
			icon.paint(p, currentLeft, bubbleY + (bubbleH - scaledIconH) / 2 + 1, st::historyViewsWidth);
			p.drawText(currentLeft + st::historyViewsWidth + 1, textBaseY, viewsText);
			currentLeft += viewsWidth + textPadding;
		}
		if (edited) {
			const auto editedText = QString::fromUtf8("✏️");
			p.setFont(font);
			p.drawText(currentLeft, textBaseY, editedText);
			currentLeft += font->width(editedText) + textPadding;
			p.setFont(font->bold());
		}
		p.drawText(currentLeft, textBaseY, dateText + msgIdText);
	}
}

void Gif::paintTranscribe(
		Painter &p,
		int x,
		int y,
		bool right,
		const PaintContext &context) const {
	if (!_transcribe) {
		return;
	}
	const auto s = _transcribe->size();
	_transcribe->paint(
		p,
		x - (right ? 0 : s.width()),
		y - s.height() - st::msgDateImgDelta,
		context);
}

void Gif::paintTimestampMark(
		Painter &p,
		QRect rthumb,
		std::optional<Ui::BubbleRounding> rounding) const {
	if (_videoTimestamp <= 0 && _videoPosition < crl::time(200)) {
		return;
	}
	const auto convert = [](Ui::BubbleCornerRounding rounding) {
		return (rounding == Ui::BubbleCornerRounding::Small)
			? Ui::BubbleRadiusSmall()
			: (rounding == Ui::BubbleCornerRounding::Large)
			? Ui::BubbleRadiusLarge()
			: 0;
	};
	const auto radiusl = rounding
		? convert(rounding->bottomLeft)
		: st::roundRadiusSmall;
	const auto radiusr = rounding
		? convert(rounding->bottomRight)
		: st::roundRadiusSmall;
	const auto line = st::historyVideoTimestampProgressLine;
	const auto duration = _data->duration();
	const auto position = (_videoPosition > 0)
		? _videoPosition
		: (_videoTimestamp * crl::time(1000));
	if (rthumb.height() <= line
		|| rthumb.width() <= radiusl + radiusr
		|| position > duration) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);
	const auto used = rthumb.width() - radiusl - radiusr;
	const auto progress = position / float64(duration);
	const auto edge = radiusl + int(base::SafeRound(used * progress));
	const auto top = rthumb.y() + rthumb.height() - line;
	p.save();
	p.setPen(Qt::NoPen);
	if (edge > 0) {
		p.setBrush(st::windowBgActive);

		p.setClipRect(rthumb.x(), top, edge, line);
		p.drawRoundedRect(
			rthumb.x(),
			top - 2 * radiusl,
			edge + radiusl,
			line + 2 * radiusl,
			radiusl,
			radiusl);
	}
	if (const auto width = rthumb.width() - edge; width > 0) {
		const auto left = rthumb.x() + edge;
		p.setBrush(st::mediaviewPlaybackProgressFg);
		p.setClipRect(left, top, width, line);
		p.drawRoundedRect(
			left - radiusr,
			top - 2 * radiusr,
			width + radiusr,
			line + 2 * radiusr,
			radiusr,
			radiusr);
	}
	p.restore();
}

void Gif::drawSpoilerTag(
		Painter &p,
		QRect rthumb,
		const PaintContext &context,
		Fn<QImage()> generateBackground) const {
	Media::drawSpoilerTag(
		p,
		_spoiler.get(),
		_spoilerTag,
		rthumb,
		context,
		std::move(generateBackground));
}

ClickHandlerPtr Gif::spoilerTagLink() const {
	return Media::spoilerTagLink(_spoiler.get(), _spoilerTag);
}

QImage Gif::spoilerTagBackground() const {
	return _spoiler ? _spoiler->background : QImage();
}

void Gif::validateVideoThumbnail() const {
	Expects(!_videoCover);

	const auto content = _dataMedia->videoThumbnailContent();
	if (_videoThumbnailFrame || content.isEmpty()) {
		return;
	}
	auto info = v::get<Ui::PreparedFileInformation::Video>(
		::Media::Clip::PrepareForSending(QString(), content).media);
	_videoThumbnailFrame = std::make_unique<Image>(info.thumbnail.isNull()
		? Image::BlankMedia()->original()
		: info.thumbnail);
}

void Gif::validateThumbCache(
		QSize outer,
		bool isEllipse,
		std::optional<Ui::BubbleRounding> rounding) const {
	const auto good = _videoCoverMedia
		? _videoCoverMedia->image(Data::PhotoSize::Large)
		: _dataMedia->goodThumbnail();
	const auto normal = good
		? good
		: _videoCoverMedia
		? nullptr
		: _dataMedia->thumbnail();
	if (!normal) {
		if (_videoCoverMedia) {
			_videoCover->load(Data::PhotoSize::Small, _realParent->fullId());
		} else {
			_data->loadThumbnail(_realParent->fullId());
			validateVideoThumbnail();
		}
	}
	const auto videothumb = (normal || _videoCoverMedia)
		? nullptr
		: _videoThumbnailFrame.get();
	const auto blurred = normal
		? (!good
			&& (normal->width() < kUseNonBlurredThreshold)
			&& (normal->height() < kUseNonBlurredThreshold)
			&& !_data->isVideoFile())
		: !videothumb;
	const auto ratio = style::DevicePixelRatio();
	if (_thumbCache.size() == (outer * ratio)
		&& _thumbCacheRounding == rounding
		&& _thumbCacheBlurred == blurred
		&& _thumbIsEllipse == isEllipse) {
		return;
	}
	auto cache = prepareThumbCache(outer);
	_thumbCache = isEllipse
		? Images::Circle(std::move(cache))
		: Images::Round(std::move(cache), MediaRoundingMask(rounding));
	_thumbCacheRounding = rounding;
	_thumbCacheBlurred = blurred;
}

QImage Gif::prepareThumbCache(QSize outer) const {
	const auto good = _videoCoverMedia
		? _videoCoverMedia->image(Data::PhotoSize::Large)
		: _dataMedia->goodThumbnail();
	const auto normal = good
		? good
		: _videoCoverMedia
		? nullptr
		: _dataMedia->thumbnail();
	const auto videothumb = (normal || _videoCoverMedia)
		? nullptr
		: _videoThumbnailFrame.get();
	auto blurred = (!good
		&& normal
		&& (normal->width() < kUseNonBlurredThreshold)
		&& (normal->height() < kUseNonBlurredThreshold)
		&& !_data->isVideoFile())
		? normal
		: nullptr;
	const auto blurFromLarge = good || (normal && !blurred);
	const auto large = blurFromLarge ? normal : videothumb;
	if (videothumb) {
	} else if (_videoCoverMedia) {
		if (const auto embedded = _videoCoverMedia->thumbnailInline()) {
			blurred = embedded;
		}
	} else if (const auto embedded = _dataMedia->thumbnailInline()) {
		blurred = embedded;
	}
	if (!large && !blurred && !_data->hasThumbnail() && _data->isVideoFile()) {
		static const auto placeholder = QImage(u":/icons/video_placeholder.png"_q);
		if (!placeholder.isNull()) {
			return PrepareWithBlurredBackground(
				outer,
				::Media::Streaming::DecideVideoFrameResize(
					outer,
					placeholder.size()),
				placeholder,
				QImage());
		}
	}
	const auto resize = large
		? ::Media::Streaming::DecideVideoFrameResize(
			outer,
			large->size())
		: ::Media::Streaming::ExpandDecision();
	return PrepareWithBlurredBackground(
		outer,
		resize,
		large,
		blurFromLarge ? large : blurred);
}

void Gif::validateSpoilerImageCache(
		QSize outer,
		std::optional<Ui::BubbleRounding> rounding) const {
	Expects(_spoiler != nullptr);

	const auto ratio = style::DevicePixelRatio();
	if (_spoiler->background.size() == (outer * ratio)
		&& _spoiler->backgroundRounding == rounding) {
		return;
	}
	const auto normal = _videoCoverMedia
		? _videoCoverMedia->image(Data::PhotoSize::Small)
		: _dataMedia->thumbnail();
	auto container = std::optional<Image>();
	const auto downscale = [&](Image *image) {
		if (!image || (image->width() <= 40 && image->height() <= 40)) {
			return image;
		}
		container.emplace(image->original().scaled(
			{ 40, 40 },
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation));
		return &*container;
	};
	const auto embedded = _videoCoverMedia
		? _videoCoverMedia->thumbnailInline()
		: _dataMedia->thumbnailInline();
	const auto blurred = embedded ? embedded : downscale(normal);
	_spoiler->background = Images::Round(
		PrepareWithBlurredBackground(
			outer,
			::Media::Streaming::ExpandDecision(),
			nullptr,
			blurred),
		MediaRoundingMask(rounding));
	_spoiler->backgroundRounding = rounding;
}

void Gif::drawCornerStatus(
		Painter &p,
		const PaintContext &context,
		QPoint position) const {
	// Only draw if NOT loaded (i.e. if we need to show download/cancel)
	// and if we are downloading in corner (isVideoFile).
	if (!downloadInCorner() || dataLoaded()) {
		return;
	}

	const auto st = context.st;
	const auto sti = context.imageStyle();
	const auto padding = st::msgDateImgPadding;
	const auto radial = _animation && _animation->radial.animating(); // Check animation state

	// Position for Top-Left
	// Use padding.y() for both to have equal gaps.
	const auto statusX = position.x() + st::msgDateImgDelta + padding.y();
	const auto statusY = position.y() + st::msgDateImgDelta + padding.y();
	
	// Create a square rect for the button
	const auto buttonSize = 31; // 70% of 44px
	const auto inner = QRect(statusX, statusY, buttonSize, buttonSize);

	// Draw Circular Background (Semi-transparent black)
	{
		PainterHighQualityEnabler hq(p);
		p.setPen(Qt::NoPen);
		p.setBrush(sti->msgDateImgBg);
		p.drawEllipse(inner);
	}

	// Draw Icon (Download or Cancel)
	const auto &icon = _data->loading()
		? sti->historyVideoCancel
		: sti->historyVideoDownload;
	
	{
		p.save();
		const auto ratio = float64(buttonSize) / st::historyVideoDownloadSize;
		p.translate(inner.center());
		p.scale(ratio, ratio);
		icon.paintInCenter(p, QRect(-st::historyVideoDownloadSize / 2, -st::historyVideoDownloadSize / 2, st::historyVideoDownloadSize, st::historyVideoDownloadSize));
		p.restore();
	}

	// Draw Radial Progress if animating
	if (radial) {
		const auto radialLine = int(st::historyVideoRadialLine * (float64(buttonSize) / st::historyVideoDownloadSize));
		QRect rinner(inner.marginsRemoved(QMargins(radialLine, radialLine, radialLine, radialLine)));
		_animation->radial.draw(p, rinner, radialLine, sti->historyFileThumbRadialFg);
	}
}

TextState Gif::cornerStatusTextState(
		QPoint point,
		StateRequest request,
		QPoint position) const {
	auto result = TextState(_parent);
	if (!needCornerStatusDisplay() || !downloadInCorner() || dataLoaded()) {
		return result;
	}
	const auto padding = st::msgDateImgPadding;
	const auto statusX = position.x() + st::msgDateImgDelta + padding.y();
	const auto statusY = position.y() + st::msgDateImgDelta + padding.y();
	const auto buttonSize = 31;
	const auto inner = QRect(statusX, statusY, buttonSize, buttonSize);
	if (inner.contains(point)) {
		result.link = _data->loading() ? _cancell : _savel;
	}
	return result;
}

TextState Gif::textState(QPoint point, StateRequest request) const {
	auto result = TextState(_parent);

	if (width() < st::msgPadding.left() + st::msgPadding.right() + 1) {
		return result;
	}
	auto paintx = 0, painty = 0, paintw = width(), painth = height();
	auto bubble = _parent->hasBubble();

	const auto rightLayout = _parent->hasRightLayout();
	const auto inWebPage = (_parent->media() != this);
	const auto isRound = _data->isVideoMessage();
	const auto unwrapped = isUnwrapped();
	const auto item = _parent->data();
	auto usew = paintw, usex = 0;
	const auto via = unwrapped ? item->Get<HistoryMessageVia>() : nullptr;
	const auto reply = unwrapped ? _parent->Get<Reply>() : nullptr;
	const auto forwarded = unwrapped ? item->Get<HistoryMessageForwarded>() : nullptr;
	const auto rightAligned = unwrapped && rightLayout;
	if (via || reply || forwarded) {
		usew = maxWidth() - additionalWidth(reply, via, forwarded);
		if (rightAligned) {
			usex = width() - usew;
		}
	}
	if (isRound) {
		accumulate_min(usew, painth);
	}
	if (rtl()) usex = width() - usex - usew;

	// --- CUSTOM TOOLTIP LOGIC (Top-Right Bubble for Standard Videos) ---
	// FIX Issue 3: Implemented 2-zone tooltip logic here for Top-Right bubble
	if (!isRound && !inWebPage && !_parent->data()->isFakeAboutView()) {

		const auto font = st::msgDateFont;
		
		const auto edited = item->Get<HistoryMessageEdited>() && !item->hideEditedBadge();
		const auto dateText = QLocale().toString(
			ItemDateTime(item).time(),
			GetEnhancedBool("show_seconds")
				? QLocale::system().timeFormat(QLocale::LongFormat).remove("t")
				: QLocale::system().timeFormat(QLocale::ShortFormat));

		const auto msgIdText = (GetEnhancedBool("show_messages_id") && item->fullId().msg > 0)
			? QString(" %1").arg(item->fullId().msg.bare)
			: QString();

		const auto views = item->Get<HistoryMessageViews>();
		const auto viewsText = (views && views->views.count >= 0)
			? Lang::FormatCountToShort(std::max(views->views.count, 1)).string
			: QString();

		int totalWidth = 0;
		const int textPadding = font->width(' ');
		const int dateWidth = font->width(dateText + msgIdText);
		totalWidth += dateWidth;

		int editedWidth = 0;
		if (edited) {
			editedWidth = font->width(QString::fromUtf8("✏️"));
			totalWidth += textPadding + editedWidth;
		}

		int viewsWidth = 0;
		if (!viewsText.isEmpty()) {
			viewsWidth = st::historyViewsWidth + 1 + font->width(viewsText);
			totalWidth += (2 * textPadding) + viewsWidth;
		}

		const auto textHeight = font->height;
		const auto hPadding = 2;
		const auto vPadding = st::msgDateImgPadding.y();
		const auto bubbleW = totalWidth + 2 * hPadding;
		const auto bubbleH = textHeight + 2 * vPadding;

		// Position: Top-Right
		const auto bubbleX = width() - bubbleW - st::msgDateImgDelta;
		const auto bubbleY = st::msgDateImgDelta;
		const QRect bubbleRect(bubbleX, bubbleY, bubbleW, bubbleH);

		// Hit Testing
		if (bubbleRect.contains(point)) {
			int currentX = bubbleX + hPadding;

			// --- Zone 1: Views ---
			QRect viewsRect;
			if (viewsWidth > 0) {
				viewsRect = QRect(currentX, bubbleY, viewsWidth, bubbleH);
				currentX += viewsWidth + textPadding;
			}

			// --- Zone 2: Edited + Date + ID ---
			int zone2Start = currentX;
			int calculatedZone2W = dateWidth + (edited ? (editedWidth + textPadding) : 0);
			QRect zone2Rect(zone2Start, bubbleY, calculatedZone2W, bubbleH);

			// Logic
			if (viewsWidth > 0 && viewsRect.contains(point)) {
				result.customTooltip = true;
				result.customTooltipText = QString("Views: ") + viewsText;
				return result;
			} 
			else if (zone2Rect.contains(point)) {
				const auto uploadLocal = ItemDateTime(item);
				QString text = tr::lng_uploaded(tr::now) + ": "
					+ uploadLocal.date().toString("dddd, dd MMMM yyyy") + " "
					+ uploadLocal.time().toString("HH:mm:ss");
				
				if (GetEnhancedBool("show_messages_id") && item->fullId().msg > 0) {
					text += "  ID: " + QString::number(item->fullId().msg.bare);
				}

				if (edited) {
					const auto editLocal = base::unixtime::parse(item->Get<HistoryMessageEdited>()->date);
					QString editedTrans = tr::lng_edited(tr::now);
					editedTrans = editedTrans.toUpper().left(1) + editedTrans.mid(1);
					text += "\n" + editedTrans + ": "
						+ editLocal.date().toString("dddd, dd MMMM yyyy") + " "
						+ editLocal.time().toString("HH:mm:ss");
				}

				result.customTooltip = true;
				result.customTooltipText = text;
				return result;
			}
		}
	}
	// ----------------------------------------------------------

	if (via || reply || forwarded) {
		auto rectw = paintw - usew - st::msgReplyPadding.left();
		auto innerw = rectw - (st::msgReplyPadding.left() + st::msgReplyPadding.right());
		auto recth = 0;
		auto forwardedHeightReal = forwarded ? forwarded->text.countHeight(innerw) : 0;
		auto forwardedHeight = qMin(forwardedHeightReal, kMaxGifForwardedBarLines * st::msgServiceNameFont->height);
		if (forwarded) {
			recth += st::msgReplyPadding.top() + forwardedHeight;
		} else if (via) {
			recth += st::msgReplyPadding.top() + st::msgServiceNameFont->height + (reply ? st::msgReplyPadding.top() : 0);
		}
		if (reply) {
			const auto replyMargins = reply->margins();
			recth += reply->height()
				- ((forwarded || via) ? 0 : replyMargins.top())
				- replyMargins.bottom();
		} else {
			recth += st::msgReplyPadding.bottom();
		}
		auto rectx = rightAligned ? 0 : (usew + st::msgReplyPadding.left());
		auto recty = painty;
		if (rtl()) rectx = width() - rectx - rectw;

		if (forwarded) {
			if (QRect(rectx, recty, rectw, st::msgReplyPadding.top() + forwardedHeight).contains(point)) {
				auto breakEverywhere = (forwardedHeightReal > forwardedHeight);
				auto textRequest = request.forText();
				if (breakEverywhere) {
					textRequest.flags |= Ui::Text::StateRequest::Flag::BreakEverywhere;
				}
				result = TextState(_parent, forwarded->text.getState(
					point - QPoint(rectx + st::msgReplyPadding.left(), recty + st::msgReplyPadding.top()),
					innerw,
					textRequest));
				result.symbol = 0;
				result.afterSymbol = false;
				if (breakEverywhere) {
					result.cursor = CursorState::Forwarded;
				} else {
					result.cursor = CursorState::None;
				}
				return result;
			}
			recty += forwardedHeight;
			recth -= forwardedHeight;
		} else if (via) {
			auto viah = st::msgReplyPadding.top() + st::msgServiceNameFont->height + (reply ? 0 : st::msgReplyPadding.bottom());
			if (QRect(rectx, recty, rectw, viah).contains(point)) {
				result.link = via->link;
				return result;
			}
			auto skip = st::msgServiceNameFont->height + (reply ? 2 * st::msgReplyPadding.top() : 0);
			recty += skip;
			recth -= skip;
		}
		if (reply) {
			if (forwarded || via) {
				recty += st::msgReplyPadding.top();
				recth -= st::msgReplyPadding.top() + reply->margins().top();
			} else {
				recty -= reply->margins().top();
			}
			const auto replyRect = QRect(rectx, recty, rectw, recth);
			if (replyRect.contains(point)) {
				result.link = reply->link();
				reply->saveRipplePoint(point - replyRect.topLeft());
				reply->createRippleAnimation(_parent, replyRect.size());
				return result;
			}
		}
	}
	if (!unwrapped) {
		if (const auto state = cornerStatusTextState(point, request, QPoint()); state.link) {
			return state;
		}
	}
	if (isRound) {
		const auto center = QPoint(usex + paintx + usew / 2, painty + usew / 2);
		const auto radius = usew / 2;
		const auto diff = point - center;
		if (diff.x() * diff.x() + diff.y() * diff.y() <= radius * radius) {
			ensureDataMediaCreated();
			result.link = (_spoiler && !_spoiler->revealed)
				? (_sensitiveSpoiler
					? spoilerTagLink()
					: (isRound && _parent->data()->media()->ttlSeconds())
					? _openl // Overriden.
					: _spoiler->link)
				: currentVideoLink();
		}
	} else if (QRect(usex + paintx, painty, usew, painth).contains(point)) {
		ensureDataMediaCreated();
		result.link = (_spoiler && !_spoiler->revealed)
			? (_sensitiveSpoiler
				? spoilerTagLink()
				: (isRound && _parent->data()->media()->ttlSeconds())
				? _openl // Overriden.
				: _spoiler->link)
			: currentVideoLink();
	}
	const auto checkBottomInfo = !inWebPage
		&& (unwrapped || !bubble || isBubbleBottom());
	if (checkBottomInfo) {
		auto fullRight = usex + paintx + usew;
		auto fullBottom = painty + painth;
		auto maxRight = _parent->width() - st::msgMargin.left();
		if (_parent->hasFromPhoto()) {
			maxRight -= st::msgMargin.right();
		} else {
			maxRight -= st::msgMargin.left();
		}
		if (unwrapped && !rightAligned) {
			auto infoWidth = _parent->infoWidth();

			// This is just some arbitrary point,
			// the main idea is to make info left aligned here.
			fullRight += infoWidth - st::normalFont->height;
			if (fullRight > maxRight) {
				fullRight = maxRight;
			}
		}
		
		// FIX Issue 3: Only call standard bottom info if it's a Round Video (Video Note).
		// AND implement custom tooltip for round videos.
		if (isRound) {
			// But for hit testing we use our custom bubble rect from draw()
			// Need to replicate calculation:
			const auto font = st::msgDateFont;
			const auto views = item->Get<HistoryMessageViews>();
			const auto viewsText = (views && views->views.count >= 0)
				? Lang::FormatCountToShort(std::max(views->views.count, 1)).string
				: QString();
			const auto authorName = [&] {
				if (const auto msgsigned = item->Get<HistoryMessageSigned>()) {
					return msgsigned->author;
				}
				return item->from()->name();
			}();
			const auto timeText = QLocale().toString(
				ItemDateTime(item).time(),
				GetEnhancedBool("show_seconds")
					? QLocale::system().timeFormat(QLocale::LongFormat).remove("t")
					: QLocale::system().timeFormat(QLocale::ShortFormat));
			const auto msgIdText = (GetEnhancedBool("show_messages_id") && item->fullId().msg > 0)
				? QString(" %1").arg(item->fullId().msg.bare)
				: QString();

			QString displayText = viewsText;
			if (!authorName.isEmpty()) displayText += " " + authorName;
			displayText += " " + timeText + msgIdText;

			const int iconW = st::historyViewsWidth;
			const int iconGap = 3;
			const int hPadding = st::msgDateImgPadding.x();
			const int vPadding = st::msgDateImgPadding.y();
			int totalW = hPadding + (viewsText.isEmpty() ? 0 : (iconW + iconGap)) + font->width(displayText) + hPadding;
			int totalH = font->height + 2 * vPadding;

			int bubbleX = fullRight - totalW - st::msgDateImgDelta;
			int bubbleY = fullBottom - totalH - st::msgDateImgDelta;
			QRect bubbleRect(bubbleX, bubbleY, totalW, totalH);

			if (bubbleRect.contains(point)) {
				QString tooltip;
				// Row 1: Author, Views, Shares
				QString row1;
				if (const auto authorName = [&] {
					if (const auto msgsigned = item->Get<HistoryMessageSigned>()) {
						return msgsigned->author;
					}
					return item->from()->name();
				}(); !authorName.isEmpty()) {
					row1 += tr::lng_signed_author(tr::now, lt_user, authorName);
				}
				if (views && views->views.count >= 0) {
					if (!row1.isEmpty()) row1 += ", ";
					row1 += tr::lng_views_tooltip(tr::now, lt_count_decimal, views->views.count);
				}
				if (views && views->forwardsCount > 0) {
					if (!row1.isEmpty()) row1 += ", ";
					row1 += tr::lng_forwards_tooltip(tr::now, lt_count_decimal, views->forwardsCount);
				}
				tooltip = row1;

				// Row 2: Uploaded (Localized) + ID
				const auto uploadLocal = ItemDateTime(item);
				
				QString row2 = tr::lng_uploaded(tr::now) + ": "
					+ uploadLocal.date().toString("dddd, dd MMMM yyyy") + " "
					+ uploadLocal.time().toString("HH:mm:ss");
				
				if (GetEnhancedBool("show_messages_id") && item->fullId().msg > 0) {
					row2 += " ID: " + QString::number(item->fullId().msg.bare);
				}

				if (const auto edited = item->Get<HistoryMessageEdited>()) {
					const auto editLocal = base::unixtime::parse(edited->date);
					QString editedTrans = tr::lng_edited(tr::now);
					editedTrans = editedTrans.toUpper().left(1) + editedTrans.mid(1);
					row2 += "\n" + editedTrans + ": "
						+ editLocal.date().toString("dddd, dd MMMM yyyy") + " "
						+ editLocal.time().toString("HH:mm:ss");
				}
				
				if (!tooltip.isEmpty()) tooltip += "\n";
				tooltip += row2;

				result.customTooltip = true;
				result.customTooltipText = tooltip;
				return result;
			}
		}
		// -----------------------------------------------------------------------

		if (const auto size = bubble ? std::nullopt : _parent->rightActionSize()) {
			const auto rightActionWidth = size->width();
			auto fastShareLeft = _parent->hasRightLayout()
				? (paintx + usex - size->width() - st::historyFastShareLeft)
				: (fullRight + st::historyFastShareLeft);
			auto fastShareTop = fullBottom
				- st::historyFastShareBottom
				- size->height();
			if (fastShareLeft + rightActionWidth > maxRight) {
				fastShareLeft = fullRight
					- rightActionWidth
					- st::msgDateImgDelta;
				fastShareTop -= st::msgDateImgDelta
					+ st::msgDateImgPadding.y()
					+ st::msgDateFont->height
					+ st::msgDateImgPadding.y();
			}
			if (QRect(QPoint(fastShareLeft, fastShareTop), *size).contains(point)) {
				result.link = _parent->rightActionLink(point
					- QPoint(fastShareLeft, fastShareTop));
			}
		}
		if (_transcribe && _transcribe->contains(point)) {
			result.link = _transcribe->link();
		}
	}
	return result;
}

void Gif::clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) {
	File::clickHandlerPressedChanged(handler, pressed);
	if (!handler) {
		return;
	} else if (_transcribe && (handler == _transcribe->link())) {
		if (pressed) {
			_transcribe->addRipple([=] { repaint(); });
		} else {
			_transcribe->stopRipple();
		}
	}
}

bool Gif::fullFeaturedGrouped(RectParts sides) const {
	return (sides & RectPart::Left) && (sides & RectPart::Right);
}

QSize Gif::sizeForGroupingOptimal(int maxWidth, bool last) const {
	return sizeForAspectRatio();
}

QSize Gif::sizeForGrouping(int width) const {
	return sizeForAspectRatio();
}

void Gif::drawGrouped(
		Painter &p,
		const PaintContext &context,
		const QRect &geometry,
		RectParts sides,
		Ui::BubbleRounding rounding,
		float64 highlightOpacity,
		not_null<uint64*> cacheKey,
		not_null<QPixmap*> cache) const {
	ensureDataMediaCreated();
	const auto item = _parent->data();
	const auto loaded = dataLoaded();
	const auto displayLoading = item->isSending()
		|| item->hasFailed()
		|| _data->displayLoading();
	const auto st = context.st;
	const auto sti = context.imageStyle();
	_smallGroupPart = !fullFeaturedGrouped(sides);
	const auto cornerDownload = downloadInCorner();
	const auto canBePlayed = _dataMedia->canBePlayed();

	const auto revealed = _spoiler
		? _spoiler->revealAnimation.value(_spoiler->revealed ? 1. : 0.)
		: 1.;
	const auto fullHiddenBySpoiler = (revealed == 0.);
	if (revealed < 1.) {
		validateSpoilerImageCache(geometry.size(), rounding);
	}

	const auto autoplay = !_smallGroupPart
		&& autoplayEnabled()
		&& canBePlayed
		&& CanPlayInline(_data);
	const auto canStartPlay = autoplay
		&& !_streamed
		&& !fullHiddenBySpoiler;
	const auto shouldBePlaying = !autoplayUnderCursor() || underCursor();
	if (!shouldBePlaying && _videoTimestamp != 0) {
		const_cast<Gif*>(this)->stopAnimation();
	} else if (canStartPlay) {
		const_cast<Gif*>(this)->playAnimation(true);
	} else {
		checkStreamedIsStarted();
	}

	const auto streamingMode = _streamed || autoplay;
	const auto activeOwnPlaying = activeOwnStreamed();

	const auto streamed = activeOwnPlaying
		? &activeOwnPlaying->instance
		: nullptr;
	const auto streamedForWaiting = _streamed
		? &_streamed->instance
		: nullptr;

	if (displayLoading
		&& (!streamedForWaiting
			|| item->isSending()
			|| _data->uploading()
			|| (cornerDownload && _data->loading()))) {
		ensureAnimation();
		if (!_animation->radial.animating()) {
			_animation->radial.start(dataProgress());
		}
	}
	updateStatusText();
	const auto radial = isRadialAnimation()
		|| (streamedForWaiting && streamedForWaiting->waitingShown());

	if (streamed && !fullHiddenBySpoiler) {
		const auto original = sizeForAspectRatio();
		const auto originalWidth = style::ConvertScale(original.width());
		const auto originalHeight = style::ConvertScale(original.height());
		const auto pixSize = Ui::GetImageScaleSizeForGeometry(
			{ originalWidth, originalHeight },
			{ geometry.width(), geometry.height() });
		auto request = ::Media::Streaming::FrameRequest{
			.resize = pixSize * style::DevicePixelRatio(),
			.outer = geometry.size() * style::DevicePixelRatio(),
			.rounding = MediaRoundingMask(rounding),
		};
		if (activeOwnPlaying->instance.playerLocked()) {
			if (activeOwnPlaying->frozenFrame.isNull()) {
				activeOwnPlaying->frozenRequest = request;
				activeOwnPlaying->frozenFrame = streamed->frame(request);
				activeOwnPlaying->frozenStatusText = _statusText;
			} else if (activeOwnPlaying->frozenRequest != request) {
				activeOwnPlaying->frozenRequest = request;
				activeOwnPlaying->frozenFrame = streamed->frame(request);
			}
			p.drawImage(geometry, activeOwnPlaying->frozenFrame);
		} else {
			if (activeOwnPlaying) {
				activeOwnPlaying->frozenFrame = QImage();
				activeOwnPlaying->frozenStatusText = QString();
			}
			p.drawImage(geometry, streamed->frame(request));
			const auto paused = context.paused
				|| (autoplayUnderCursor() && !underCursor());
			if (!paused) {
				streamed->markFrameShown();
			}
		}
	} else if (!fullHiddenBySpoiler) {
		validateGroupedCache(geometry, rounding, cacheKey, cache);
		p.drawPixmap(geometry, *cache);
	}

	if (revealed < 1.) {
		p.setOpacity(1. - revealed);
		p.drawImage(geometry.topLeft(), _spoiler->background);
		fillImageSpoiler(p, _spoiler.get(), geometry, context);
		p.setOpacity(1.);
	}

	const auto overlayOpacity = context.selected()
		? (1. - highlightOpacity)
		: highlightOpacity;
	if (overlayOpacity > 0.) {
		p.setOpacity(overlayOpacity);
		fillImageOverlay(p, geometry, rounding, context);
		if (!context.selected()) {
			fillImageOverlay(p, geometry, rounding, context);
		}
		p.setOpacity(1.);
	}

	// Force show standard icon for streamable videos that aren't playing/fully loaded,
	// even if the thumbnail is loaded (which sets loaded=true).
	// This ensures grid videos always show the download/play arrow.
	const auto forceShowForVideo = !_sensitiveSpoiler 
		&& _data->isVideoFile() 
		&& !_dataMedia->loaded(); // loaded() is true only if full file is loaded

	const auto paintInCenter = !_sensitiveSpoiler
		&& (radial
			|| (!streamingMode
				&& ((!loaded && !_data->loading()) || !autoplay))
			|| forceShowForVideo);
	if (paintInCenter) {
		const auto radialRevealed = 1.;
		const auto opacity = (item->isSending() || _data->uploading())
			? 1.
			: streamedForWaiting
			? streamedForWaiting->waitingOpacity()
			: (radial && loaded)
			? _animation->radial.opacity()
			: 1.;
		const auto radialOpacity = opacity * radialRevealed;
		const auto radialSize = st::historyGroupRadialSize;
		const auto inner = QRect(
			geometry.x() + (geometry.width() - radialSize) / 2,
			geometry.y() + (geometry.height() - radialSize) / 2,
			radialSize,
			radialSize);
		p.setPen(Qt::NoPen);
		if (context.selected()) {
			p.setBrush(st->msgDateImgBgSelected());
		} else if (isThumbAnimation()) {
			auto over = _animation->a_thumbOver.value(1.);
			p.setBrush(anim::brush(st->msgDateImgBg(), st->msgDateImgBgOver(), over));
		} else {
			auto over = ClickHandler::showAsActive(
				(_data->loading() || _data->uploading()) ? _cancell : _savel);
			p.setBrush(over ? st->msgDateImgBgOver() : st->msgDateImgBg());
		}
		p.setOpacity(radialOpacity * p.opacity());

		{
			PainterHighQualityEnabler hq(p);
			p.drawEllipse(inner);
		}

		p.setOpacity(radialOpacity);
		const auto icon = [&]() -> const style::icon * {
			if (_data->waitingForAlbum()) {
				return &sti->historyFileThumbWaiting;
			} else if (streamingMode && !_data->uploading()) {
				return nullptr;
			} else if ((loaded || canBePlayed) && (!radial || cornerDownload)) {
				return &sti->historyFileThumbPlay;
			} else if (radial || _data->loading()) {
				if (!item->isSending() || _data->uploading()) {
					return &sti->historyFileThumbCancel;
				}
				return nullptr;
			}
			return &sti->historyFileThumbDownload;
		}();
		const auto previous = _data->waitingForAlbum()
			? &sti->historyFileThumbCancel
			: nullptr;
		if (icon) {
			if (previous && radialOpacity > 0. && radialOpacity < 1.) {
				PaintInterpolatedIcon(p, *icon, *previous, radialOpacity, inner);
			} else {
				icon->paintInCenter(p, inner);
			}
		}
		p.setOpacity(radialRevealed);
		if (radial) {
			const auto line = st::historyGroupRadialLine;
			const auto rinner = inner.marginsRemoved({ line, line, line, line });
			if (streamedForWaiting && !_data->uploading()) {
				Ui::InfiniteRadialAnimation::Draw(
					p,
					streamedForWaiting->waitingState(),
					rinner.topLeft(),
					rinner.size(),
					width(),
					sti->historyFileThumbRadialFg,
					st::msgFileRadialLine);
			} else if (!cornerDownload) {
				_animation->radial.draw(
					p,
					rinner,
					st::msgFileRadialLine,
					sti->historyFileThumbRadialFg);
			}
		}
		p.setOpacity(1.);
	}
	// Differentiation removed. All items (Single Row or Side-by-Side)
	// now use the unified paintInCenter logic above to draw the icon.
	if (cornerDownload) {
		drawCornerStatus(p, context, geometry.topLeft());
	}
}

TextState Gif::getStateGrouped(
		const QRect &geometry,
		RectParts sides,
		QPoint point,
		StateRequest request) const {
	if (!geometry.contains(point)) {
		return {};
	}
	if (downloadInCorner()) {
		const auto state = cornerStatusTextState(
			point,
			request,
			geometry.topLeft());
		if (state.link) {
			return state;
		}
	}
	ensureDataMediaCreated();

	auto link = (_spoiler && !_spoiler->revealed)
		? (_sensitiveSpoiler ? spoilerTagLink() : _spoiler->link)
		: currentVideoLink();
	return TextState(_parent, std::move(link));
}

ClickHandlerPtr Gif::currentVideoLink() const {
	return _data->uploading()
		? _cancell
		: _realParent->isSending()
		? nullptr
		: dataLoaded()
		? _openl
		: (_data->loading() && _smallGroupPart)
		? _cancell
		: _dataMedia->canBePlayed()
		? _openl
		: _data->loading()
		? _cancell
		: _savel;
}

void Gif::ensureDataMediaCreated() const {
	if (_dataMedia && (!_videoCover || _videoCoverMedia)) {
		return;
	}
	_dataMedia = _data->createMediaView();
	_videoCoverMedia = _videoCover
		? _videoCover->createMediaView()
		: nullptr;
	dataMediaCreated();
}

void Gif::dataMediaCreated() const {
	Expects(_dataMedia != nullptr);

	if (_videoCoverMedia) {
		_videoCoverMedia->wanted(
			Data::PhotoSize::Large,
			_realParent->fullId());
	} else {
		_dataMedia->goodThumbnailWanted();
		_dataMedia->thumbnailWanted(_realParent->fullId());
		if (!autoplayEnabled()) {
			_dataMedia->videoThumbnailWanted(_realParent->fullId());
		}
	}
	history()->owner().registerHeavyViewPart(_parent);
	togglePollingStory(true);
}

void Gif::togglePollingStory(bool enabled) const {
	if (!_storyId || _pollingStory == enabled) {
		return;
	}
	const auto polling = Data::Stories::Polling::Chat;
	if (!enabled) {
		_data->owner().stories().unregisterPolling(_storyId, polling);
	} else if (
			!_data->owner().stories().registerPolling(_storyId, polling)) {
		return;
	}
	_pollingStory = enabled;
}

bool Gif::uploading() const {
	return _data->uploading();
}

void Gif::hideSpoilers() {
	if (_spoiler) {
		_spoiler->revealed = false;
	}
}

bool Gif::needsBubble() const {
	if (_storyId) {
		return true;
	} else if (_data->isVideoMessage()) {
		return false;
	}
	const auto item = _parent->data();
	return item->repliesAreComments()
		|| item->externalReply()
		|| item->viaBot()
		|| !item->emptyText()
		|| _parent->displayReply()
		|| _parent->displayForwardedFrom()
		|| _parent->displayFromName()
		|| _parent->displayedTopicButton();
}

//bool Gif::needInfoDisplay() const {
//	return _data->isVideoMessage();
//}

bool Gif::unwrapped() const {
	return isUnwrapped();
}

QRect Gif::contentRectForReactions() const {
	if (!isUnwrapped()) {
		return QRect(0, 0, width(), height());
	}
	auto paintx = 0, painty = 0, paintw = width(), painth = height();
	auto usex = 0, usew = paintw;
	const auto rightAligned = _parent->hasRightLayout();
	const auto item = _parent->data();
	const auto via = item->Get<HistoryMessageVia>();
	const auto reply = _parent->Get<Reply>();
	const auto forwarded = item->Get<HistoryMessageForwarded>();
	if (via || reply || forwarded) {
		usew = maxWidth() - additionalWidth(reply, via, forwarded);
	}
	accumulate_max(usew, _parent->reactionsOptimalWidth());
	if (rightAligned) {
		usex = width() - usew;
	}
	if (rtl()) usex = width() - usex - usew;
	return style::rtlrect(usex + paintx, painty, usew, painth, width());
}

std::optional<int> Gif::reactionButtonCenterOverride() const {
	if (!isUnwrapped()) {
		return std::nullopt;
	}
	const auto right = resolveCustomInfoRightBottom().x()
		- _parent->infoWidth()
		- 3 * st::msgDateImgPadding.x();
	return right - st::reactionCornerSize.width() / 2;
}

QPoint Gif::resolveCustomInfoRightBottom() const {
	const auto inner = contentRectForReactions();
	auto fullBottom = inner.y() + inner.height();
	auto fullRight = inner.x() + inner.width();
	const auto unwrapped = isUnwrapped();
	if (unwrapped) {
		auto maxRight = _parent->width() - st::msgMargin.left();
		if (_parent->hasFromPhoto()) {
			maxRight -= st::msgMargin.right();
		} else {
			maxRight -= st::msgMargin.left();
		}
		const auto infoWidth = _parent->infoWidth();
		const auto rightAligned = _parent->hasRightLayout();
		if (!rightAligned) {
			// This is just some arbitrary point,
			// the main idea is to make info left aligned here.
			fullRight += infoWidth - st::normalFont->height;
			if (fullRight > maxRight) {
				fullRight = maxRight;
			}
		}
	}
	const auto skipx = unwrapped
		? st::msgDateImgPadding.x()
		: (st::msgDateImgDelta + st::msgDateImgPadding.x());
	const auto skipy = unwrapped
		? st::msgDateImgPadding.y()
		: (st::msgDateImgDelta + st::msgDateImgPadding.y());
	return QPoint(fullRight - skipx, fullBottom - skipy);
}

int Gif::additionalWidth() const {
	const auto item = _parent->data();
	return additionalWidth(
		_parent->Get<Reply>(),
		item->Get<HistoryMessageVia>(),
		item->Get<HistoryMessageForwarded>());
}

bool Gif::isUnwrapped() const {
	return _data->isVideoMessage() && (_parent->media() == this);
}

void Gif::validateGroupedCache(
		const QRect &geometry,
		Ui::BubbleRounding rounding,
		not_null<uint64*> cacheKey,
		not_null<QPixmap*> cache) const {
	using Option = Images::Option;

	ensureDataMediaCreated();

	const auto good = _videoCoverMedia
		? _videoCoverMedia->image(Data::PhotoSize::Large)
		: _dataMedia->goodThumbnail();
	const auto thumb = _videoCoverMedia
		? nullptr
		: _dataMedia->thumbnail();
	const auto image = good
		? good
		: thumb
		? thumb
		: _videoCoverMedia
		? _videoCoverMedia->thumbnailInline()
		: _dataMedia->thumbnailInline();
	const auto blur = !good
		&& (!thumb
			|| (thumb->width() < kUseNonBlurredThreshold
				&& thumb->height() < kUseNonBlurredThreshold));

	const auto loadLevel = good ? 3 : thumb ? 2 : image ? 1 : 0;
	const auto width = geometry.width();
	const auto height = geometry.height();
	const auto options = (blur ? Option::Blur : Option(0));
	const auto key = (uint64(width) << 48)
		| (uint64(height) << 32)
		| (uint64(options) << 16)
		| (uint64(rounding.key()) << 8)
		| (uint64(loadLevel));
	if (*cacheKey == key) {
		return;
	}

	if (!image && !_data->hasThumbnail() && _data->isVideoFile()) {
		static const auto placeholder = QImage(u":/icons/video_placeholder.png"_q);
		if (!placeholder.isNull()) {
			*cacheKey = key;
			const auto pixSize = placeholder.size()
				.scaled(geometry.size(), Qt::KeepAspectRatio);
			const auto ratio = style::DevicePixelRatio();
			auto scaled = Images::Prepare(
				placeholder,
				pixSize * ratio,
				{ .options = options, .outer = geometry.size() });
			auto rounded = Images::Round(
				std::move(scaled),
				MediaRoundingMask(rounding));
			*cache = Ui::PixmapFromImage(std::move(rounded));
			return;
		}
	}

	const auto imageSize = image ? image->size() : sizeForAspectRatio();
	const auto originalWidth = style::ConvertScale(imageSize.width());
	const auto originalHeight = style::ConvertScale(imageSize.height());
	const auto pixSize = Ui::GetImageScaleSizeForGeometry(
		{ originalWidth, originalHeight },
		{ width, height });
	const auto ratio = style::DevicePixelRatio();

	*cacheKey = key;
	auto scaled = Images::Prepare(
		(image ? image : Image::BlankMedia().get())->original(),
		pixSize * ratio,
		{ .options = options, .outer = { width, height } });
	auto rounded = Images::Round(
		std::move(scaled),
		MediaRoundingMask(rounding));
	*cache = Ui::PixmapFromImage(std::move(rounded));
}

void Gif::setStatusSize(int64 newSize) const {
	if (newSize < 0) {
		_statusSize = newSize;
		_statusText = Ui::FormatDurationText(-newSize - 1);
	} else if (_data->isVideoMessage()) {
		_statusSize = newSize;
		_statusText = Ui::FormatDurationText(_data->duration() / 1000);
	} else {
		File::setStatusSize(
			newSize,
			_data->size,
			_data->isVideoFile() ? (_data->duration() / 1000) : -2,
			0);
	}
}

void Gif::updateStatusText() const {
	ensureDataMediaCreated();
	auto statusSize = int64();
	if (_data->status == FileDownloadFailed || _data->status == FileUploadFailed) {
		statusSize = Ui::FileStatusSizeFailed;
	} else if (_data->uploading()) {
		statusSize = _data->uploadingData->offset;
	} else if (!downloadInCorner() && _data->loading()) {
		statusSize = _data->loadOffset();
	} else if (dataLoaded() || _dataMedia->canBePlayed()) {
		statusSize = Ui::FileStatusSizeLoaded;
	} else {
		statusSize = Ui::FileStatusSizeReady;
	}
	const auto round = activeRoundStreamed();
	const auto own = activeOwnStreamed();
	if (round || (own && _data->isVideoFile())) {
		const auto frozen = own && !own->frozenFrame.isNull();
		const auto streamed = round ? round : &own->instance;
		const auto state = streamed->player().prepareLegacyState();
		if (state.length) {
			auto position = int64(0);
			if (::Media::Player::IsStoppedAtEnd(state.state)) {
				position = state.length;
			} else if (!::Media::Player::IsStoppedOrStopping(state.state)) {
				position = state.position;
			}
			if (!frozen) {
				statusSize = -1 - int((state.length - position) / state.frequency + 1);
			}
			_videoPosition = std::max(
				crl::time(position * crl::time(1000) / state.frequency),
				crl::time(1));
		} else {
			if (!frozen) {
				statusSize = -1 - (_data->duration() / 1000);
			}
			_videoPosition = 0;
		}
	}
	if (statusSize != _statusSize) {
		setStatusSize(statusSize);
	}
}

QString Gif::additionalInfoString() const {
	if (_data->isVideoMessage()) {
		updateStatusText();
		return _statusText;
	}
	return QString();
}

bool Gif::isReadyForOpen() const {
	return true;
}

bool Gif::hasHeavyPart() const {
	return (_spoiler && _spoiler->animation) || _streamed || _dataMedia;
}

void Gif::unloadHeavyPart() {
	stopAnimation();
	_dataMedia = nullptr;
	if (_spoiler) {
		_spoiler->background = _spoiler->cornerCache = QImage();
		_spoiler->animation = nullptr;
	}
	_thumbCache = QImage();
	_videoThumbnailFrame = nullptr;
	togglePollingStory(false);
}

bool Gif::enforceBubbleWidth() const {
	return true;
}

int Gif::additionalWidth(
		const Reply *reply,
		const HistoryMessageVia *via,
		const HistoryMessageForwarded *forwarded) const {
	int result = 0;
	if (forwarded) {
		accumulate_max(result, st::msgReplyPadding.left() + st::msgReplyPadding.left() + forwarded->text.maxWidth() + st::msgReplyPadding.right());
	} else if (via) {
		accumulate_max(result, st::msgReplyPadding.left() + st::msgReplyPadding.left() + via->maxWidth + st::msgReplyPadding.left());
	}
	if (reply) {
		accumulate_max(result, st::msgReplyPadding.left() + reply->maxWidth());
	}
	return result;
}

::Media::Streaming::Instance *Gif::activeRoundStreamed() const {
	return ::Media::Player::instance()->roundVideoStreamed(_parent->data());
}

Gif::Streamed *Gif::activeOwnStreamed() const {
	return (_streamed
		&& _streamed->instance.player().ready()
		&& !_streamed->instance.player().videoSize().isEmpty())
		? _streamed.get()
		: nullptr;
}

::Media::Streaming::Instance *Gif::activeCurrentStreamed() const {
	if (const auto streamed = activeRoundStreamed()) {
		return streamed;
	} else if (const auto owned = activeOwnStreamed()) {
		return &owned->instance;
	}
	return nullptr;
}

::Media::View::PlaybackProgress *Gif::videoPlayback() const {
	return ::Media::Player::instance()->roundVideoPlayback(_parent->data());
}

void Gif::playAnimation(bool autoplay) {
	ensureDataMediaCreated();
	if (_data->isVideoMessage() && !autoplay) {
		return;
	} else if (_streamed && autoplay) {
		return;
	} else if ((_streamed && autoplayEnabled())
		|| (!autoplay && _data->isVideoFile())) {
		_parent->delegate()->elementOpenDocument(
			_data,
			_parent->data()->fullId(),
			true);
		return;
	}
	if (_streamed) {
		stopAnimation();
	} else if (_dataMedia->canBePlayed()) {
		if (!autoplayEnabled()) {
			history()->owner().checkPlayingAnimations();
		}
		createStreamedPlayer();
	}
}

void Gif::createStreamedPlayer() {
	const auto quality = Core::App().settings().videoQuality();
	const auto chosen = _data->chooseQuality(_realParent, quality);
	if (_streamed && _streamed->chosen == chosen) {
		return;
	}
	auto shared = _data->owner().streaming().sharedDocument(
		chosen,
		_data,
		_realParent,
		_realParent->fullId());
	if (!shared) {
		return;
	}
	setStreamed(std::make_unique<Streamed>(
		chosen,
		std::move(shared),
		[=] { repaintStreamedContent(); }));

	_streamed->instance.player().updates(
	) | rpl::on_next_error([=](::Media::Streaming::Update &&update) {
		handleStreamingUpdate(std::move(update));
	}, [=](::Media::Streaming::Error &&error) {
		handleStreamingError(std::move(error));
	}, _streamed->instance.lifetime());

	_streamed->instance.switchQualityRequests(
	) | rpl::on_next([=](int quality) {
		auto now = Core::App().settings().videoQuality();
		if (now.manual || now.height == quality) {
			return;
		}
		Core::App().settings().setVideoQuality({
			.manual = 0,
			.height = uint32(quality),
		});
		Core::App().saveSettingsDelayed();
		createStreamedPlayer();
	}, _streamed->instance.lifetime());

	if (_streamed->instance.ready()) {
		streamingReady(base::duplicate(_streamed->instance.info()));
	}
	checkStreamedIsStarted();
}

void Gif::startStreamedPlayer() const {
	Expects(_streamed != nullptr);

	auto options = ::Media::Streaming::PlaybackOptions();
	options.audioId = AudioMsgId(_data, _realParent->fullId());
	options.waitForMarkAsShown = true;
	//if (!_streamed->withSound) {
	options.mode = ::Media::Streaming::Mode::Video;
	options.loop = true;
	options.position = _videoTimestamp
		? (_videoTimestamp * crl::time(1000))
		: _parent->history()->session().local().mediaLastPlaybackPosition(
			_data->id);
	//}
	_streamed->instance.play(options);
}

void Gif::checkStreamedIsStarted() const {
	if (!_streamed || _streamed->instance.playerLocked()) {
		return;
	}
	if (_streamed->instance.active()) {
		if (_streamed->instance.paused()) {
			_streamed->instance.resume();
		}
	} else if (!_streamed->instance.failed()) {
		startStreamedPlayer();
	}
}

void Gif::setStreamed(std::unique_ptr<Streamed> value) {
	const auto removed = (_streamed && !value);
	const auto set = (!_streamed && value);
	_streamed = std::move(value);
	if (set) {
		history()->owner().registerHeavyViewPart(_parent);
		togglePollingStory(true);
	} else if (removed) {
		_videoPosition = 0;
		_parent->checkHeavyPart();
	}
}

void Gif::handleStreamingUpdate(::Media::Streaming::Update &&update) {
	using namespace ::Media::Streaming;

	v::match(update.data, [&](Information &update) {
		streamingReady(std::move(update));
	}, [](PreloadedVideo) {
	}, [&](UpdateVideo) {
		repaintStreamedContent();
	}, [](PreloadedAudio) {
	}, [](UpdateAudio) {
	}, [](WaitingForData) {
	}, [](SpeedEstimate) {
	}, [](MutedByOther) {
	}, [](Finished) {
	});
}

void Gif::handleStreamingError(::Media::Streaming::Error &&error) {
}

void Gif::repaintStreamedContent() {
	const auto own = activeOwnStreamed();
	if (own && !own->frozenFrame.isNull()) {
		return;
	} else if (_parent->delegate()->elementAnimationsPaused()
		&& !activeRoundStreamed()) {
		return;
	}
	repaint();
}

void Gif::streamingReady(::Media::Streaming::Information &&info) {
	if (info.video.size.width() * info.video.size.height()
		> kMaxInlineArea) {
		_data->dimensions = info.video.size;
		stopAnimation();
	} else {
		history()->owner().requestViewResize(_parent);
	}
}

void Gif::stopAnimation() {
	if (_streamed) {
		setStreamed(nullptr);
		history()->owner().requestViewResize(_parent);
	}
}

void Gif::checkAnimation() {
	if (_streamed && !autoplayEnabled()) {
		stopAnimation();
	}
}

float64 Gif::dataProgress() const {
	ensureDataMediaCreated();
	return (_data->uploading()
		|| (!_parent->data()->isSending() && !_parent->data()->hasFailed()))
		? _dataMedia->progress()
		: 0;
}

bool Gif::dataFinished() const {
	return (!_parent->data()->isSending() && !_parent->data()->hasFailed())
		? (!_data->loading() && !_data->uploading())
		: false;
}

bool Gif::dataLoaded() const {
	ensureDataMediaCreated();
	return !_parent->data()->isSending()
		&& !_parent->data()->hasFailed()
		&& _dataMedia->loaded();
}

//bool Gif::needInfoDisplay() const {
//	const auto item = _parent->data();
//	if (item->isFakeAboutView()) {
//		return false;
//	}
//	return item->isSending()
//		|| item->awaitingVideoProcessing()
//		|| _data->uploading()
//		|| _parent->isUnderCursor()
//		|| (_parent->delegate()->elementContext() == Context::ChatPreview)
//		// Don't show the GIF badge if this message has text.
//		|| (!_parent->hasBubble() && _parent->isLastAndSelfMessage());
//}

//bool Gif::needCornerStatusDisplay() const {
//	return _data->isVideoFile()
//		|| needInfoDisplay();
//}

bool Gif::needCornerStatusDisplay() const {
	return _data->isVideoFile();
}

void Gif::ensureTranscribeButton() const {
	if (_data->isVideoMessage()
		&& !_parent->data()->media()->ttlSeconds()
		&& !_parent->data()->isScheduled()
		&& !_parent->data()->isAdminLogEntry()
		&& (_data->session().premium()
			|| _data->session().api().transcribes().trialsSupport())) {
		if (!_transcribe) {
			_transcribe = std::make_unique<TranscribeButton>(
				_realParent,
				true);
		}
	} else {
		_transcribe = nullptr;
	}
}

} // namespace HistoryView
