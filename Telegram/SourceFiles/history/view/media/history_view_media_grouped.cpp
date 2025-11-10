/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/media/history_view_media_grouped.h"

#include "core/click_handler_types.h"
#include "main/main_session.h"
#include "data/data_session.h"
#include "ui/text/text_utilities.h"
#include "ui/text/text.h"
#include "history/view/history_view_element.h"
#include <QApplication>
#include <QClipboard>
#include "ui/widgets/menu/menu.h"
#include "base/unique_qptr.h"
#include "lang/lang_keys.h"
#include "window/window_session_controller.h"

#include "history/history_item_components.h"
#include "history/history_item.h"
#include "history/history.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_cursor_state.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "storage/storage_shared_media.h"
#include "lang/lang_keys.h"
#include "media/streaming/media_streaming_utility.h"
#include "ui/grouped_layout.h"
#include "ui/chat/chat_style.h"
#include "ui/chat/message_bubble.h"
#include "ui/text/text_options.h"
#include "ui/text/format_values.h"
#include "ui/painter.h"
#include "ui/power_saving.h"
#include "layout/layout_selection.h"
#include "styles/style_chat.h"
#include "styles/style_basic.h"
#include "core/enhanced_settings.h"
#include "data/data_photo.h"

namespace HistoryView {
namespace {

std::vector<Ui::GroupMediaLayout> LayoutPlaylist(
		const std::vector<QSize> &sizes) {
	Expects(!sizes.empty());

	auto result = std::vector<Ui::GroupMediaLayout>();
	result.reserve(sizes.size());
	const auto width = ranges::max_element(
			sizes,
			std::less<>(),
			&QSize::width)->width();
	auto top = 0;
	for (const auto &size : sizes) {
		result.push_back({
			.geometry = QRect(0, top, width, size.height()),
			.sides = RectPart::Left | RectPart::Right
		});
		top += size.height();
	}
	result.front().sides |= RectPart::Top;
	result.back().sides |= RectPart::Bottom;
	return result;
}

} // namespace

CaptionClickHandler::CaptionClickHandler(int partIndex)
: _partIndex(partIndex) {
}

GroupedMedia::Part::Part(
	not_null<Element*> parent,
	not_null<Data::Media*> media)
: item(media->parent())
, content(media->createView(parent, item)) {
	Assert(media->canBeGrouped());
}

void GroupedMedia::drawMessageIdInfo(
		Painter &p,
		const PaintContext &context,
		const QRect &itemGeometry,
		not_null<HistoryItem*> item) const {
	QString infoText;
	const auto edited = item->Get<HistoryMessageEdited>();
	if (edited && !item->hideEditedBadge()) {
		infoText += QString::fromUtf8("✏️");
	}

	if (GetEnhancedBool("show_messages_id")) {
		const auto msgId = item->fullId().msg;
		if (msgId > 0) {
			if (!infoText.isEmpty()) {
				infoText += ' ';
			}
			infoText += QString::number(msgId.bare);
		}
	}

	if (infoText.isEmpty()) {
		return;
	}

	const auto st = context.st;
	const auto sti = context.imageStyle();
	p.setFont(st::msgDateFont);
	// Use proper white color for text on image bubbles
	p.setPen(st->msgDateImgFg());

	auto textWidth = st::msgDateFont->width(infoText);
	const auto textHeight = st::msgDateFont->height;
	
	const auto horizontalPadding = 2;
	const auto verticalPadding = 2;

	auto dateW = textWidth + (2 * horizontalPadding);
	const auto dateH = textHeight + verticalPadding;

	if (dateW > itemGeometry.width()) {
		const auto availableWidth = itemGeometry.width()
			- (2 * horizontalPadding);
		if (availableWidth > st::msgDateFont->width("...")) {
			const QFontMetrics metrics(st::msgDateFont);
			infoText = metrics.elidedText(
				infoText,
				Qt::ElideRight,
				availableWidth);
			textWidth = st::msgDateFont->width(infoText);
			dateW = textWidth + (2 * horizontalPadding);
		} else {
			return;
		}
	}

	const auto bubbleY = itemGeometry.y() + st::msgDateImgDelta;
	const auto bubbleX = itemGeometry.x() + itemGeometry.width() - dateW - st::msgDateImgDelta;

	// Draw with uniform style brush to avoid edge transparency differences
	p.save();
p.setOpacity(0.95);
	Ui::FillRoundRect(
		p,
		bubbleX,
		bubbleY,
		dateW,
		dateH,
		sti->msgDateImgBg,
		sti->msgDateImgBgCorners);
	p.restore();

	auto font = st::msgDateFont;
	p.drawText(
		bubbleX + horizontalPadding,
		bubbleY + (dateH - textHeight) / 2 + font->ascent,
		infoText);
}

GroupedMedia::GroupedMedia(
	not_null<Element*> parent,
	const std::vector<std::unique_ptr<Data::Media>> &medias) 
: Media(parent) {
	const auto truncated = ranges::views::all(
			medias
		) | ranges::views::transform([](const std::unique_ptr<Data::Media> &v) {
			return v.get();
		}) | ranges::views::take(kMaxSize);
	const auto result = applyGroup(truncated);

	Ensures(result);
}

GroupedMedia::GroupedMedia(
	not_null<Element*> parent,
	const std::vector<not_null<HistoryItem*>> &items) 
: Media(parent) {
	const auto medias = ranges::views::all(
			items
		) | ranges::views::transform([](not_null<HistoryItem*> item) {
			return item->media();
		}) | ranges::views::take(kMaxSize);
	const auto result = applyGroup(medias);

	Ensures(result);
}

GroupedMedia::~GroupedMedia() {
	// Destroy all parts while the media object is still not destroyed.
	base::take(_parts);
}

HistoryItem *GroupedMedia::itemForText() const {
	if (_mode == Mode::Grid) {
		// This album uses per-item captions. Never allow the official
		// single bottom caption to be created for Grid albums. This prevents
		// duplicate/leftover borders when editing captions.
		return nullptr;
	}
	if (_mode == Mode::Column) {
		return Media::itemForText();
	} else if (!_captionItem) {
		_captionItem = [&]() -> HistoryItem* {
			auto result = (HistoryItem*)nullptr;
			for (const auto &part : _parts) {
				if (!part.item->emptyText()) {
					if (result == part.item) {
						// All parts are from the same message, that means
						// this is an album with a single item, single text.
						return result;
					} else if (result) {
						return nullptr;
					} else {
						result = part.item;
					}
				}
			}
			return result;
		}();
	}
	return *_captionItem;
}

bool GroupedMedia::hideMessageText() const {
	return (_mode == Mode::Column);
}

GroupedMedia::Mode GroupedMedia::DetectMode(not_null<Data::Media*> media) {
	const auto document = media->document();
	return (document && !document->isVideoFile())
		? Mode::Column
		: Mode::Grid;
}

QSize GroupedMedia::countOptimalSize() {
	_purchasedPriceTag = hasPurchasedTag();

	std::vector<QSize> sizes;
	const auto partsCount = _parts.size();
	sizes.reserve(partsCount);
	auto maxWidth = 0;
	if (_mode == Mode::Column) {
		for (const auto &part : _parts) {
			const auto &media = part.content;
			media->setBubbleRounding(bubbleRounding());
			media->initDimensions();
			accumulate_max(maxWidth, media->maxWidth());
		}
	}
	auto index = 0;
	for (const auto &part : _parts) {
		++index;
		// In Column mode the views/time/id overlay is drawn for the first row.
		// Reserve caption skip-block only for that row to avoid extra space
		// after the last item when the caption fits.
		const auto last = (_mode == Mode::Column)
			? (index == 1)
			: (index == _parts.size());
		sizes.push_back(
			part.content->sizeForGroupingOptimal(maxWidth, last));
	}

	const auto layout = (_mode == Mode::Grid)
		? Ui::LayoutMediaGroup(
			sizes,
			st::historyGroupWidthMax,
			st::historyGroupWidthMin,
			st::historyGroupSkip)
		: LayoutPlaylist(sizes);
	Assert(layout.size() == _parts.size());

	std::map<int, std::vector<int>> rows;
	for (auto i = 0; i != _parts.size(); ++i) {
		_parts[i].initialGeometry = layout[i].geometry;
		_parts[i].sides = layout[i].sides;
		rows[layout[i].geometry.y()].push_back(i);
	}

	auto y = 0.;
	const auto spacing = (_mode == Mode::Grid) ? st::historyGroupSkip : 0.;
	for (auto const& [rowY, indices] : rows) {
		auto maxMediaHeight = 0.;
		auto maxCaptionHeight = 0.;
		for (const auto i : indices) {
			auto &part = _parts[i];
			accumulate_max(maxMediaHeight, float64(part.initialGeometry.height()));

			// Calculate caption height for this specific item in Grid mode
			if (_mode == Mode::Grid) {
				const auto originalText = part.item->originalText();
				if (!originalText.empty()) {
					part.caption.setMarkedText(
						st::messageTextStyle,
						originalText,
						kDefaultTextOptions);
					const auto padding = QMargins(8, 0, 8, 0);
					part._captionHeight = part.caption.countHeight(
						part.initialGeometry.width()
							- padding.left()
							- padding.right())
						+ padding.top()
						+ padding.bottom();
				} else {
					part._captionHeight = 0.;
				}
				accumulate_max(maxCaptionHeight, float64(part._captionHeight));
			}
		}
		const auto rowHeight = maxMediaHeight + maxCaptionHeight;
		for (const auto i : indices) {
			_parts[i].initialGeometry.setY(y);
			_parts[i].initialGeometry.setHeight(maxMediaHeight);
		}
		y += rowHeight + spacing;
	}

	auto minHeight = y > 0 ? (y - spacing) : 0;
	maxWidth = 0;
	_captionsCount = 0;
	for (auto i = 0; i != _parts.size(); ++i) {
		accumulate_max(
			maxWidth,
			_parts[i].initialGeometry.x() + _parts[i].initialGeometry.width());
		if (_parts[i]._captionHeight > 0) {
			_captionsCount++;
		}
	}

	const auto groupPadding = groupedPadding();
	minHeight += groupPadding.top() + groupPadding.bottom();

	return { maxWidth, int(base::SafeRound(minHeight)) };
}

QSize GroupedMedia::countCurrentSize(int newWidth) {
	accumulate_min(newWidth, maxWidth());
	auto newHeight = 0;
	if (_mode == Mode::Grid && newWidth < st::historyGroupWidthMin) {
		return { newWidth, newHeight };
	} else if (_mode == Mode::Column) {
		auto top = 0;
		for (auto &part : _parts) {
			const auto size = part.content->sizeForGrouping(newWidth);
			part.geometry = QRect(0, top, newWidth, size.height());
			top += size.height();
		}
		newHeight = top;
	} else {
		const auto factor = newWidth / float64(maxWidth());
		const auto scale = [&](float64 value) {
			return value * factor;
		};

		std::map<int, std::vector<int>> rows;
		for (auto i = 0; i != _parts.size(); ++i) {
			rows[_parts[i].initialGeometry.y()].push_back(i);
		}

		auto y = 0.;
		const auto spacing = scale((_mode == Mode::Grid) ? st::historyGroupSkip : 0.);
		for (auto const& [rowY, indices] : rows) {
			auto maxMediaHeight = 0.;
			auto maxCaptionHeight = 0.;
			for (const auto i : indices) {
				auto &part = _parts[i];
				const auto initial = part.initialGeometry;
				const auto sides = part.sides;
				const auto needRightSkip = !(sides & RectPart::Right);
				const auto left = scale(initial.x());
				const auto width = scale(initial.x() + initial.width() + (needRightSkip ? spacing : 0))
					- left
					- (needRightSkip ? scale(spacing) : 0);
				part.geometry = QRect(left, y, width, scale(initial.height()));
				accumulate_max(maxMediaHeight, float64(part.geometry.height()));

				const auto originalText = part.item->originalText();
				if ((_mode == Mode::Grid) &&
					!originalText.empty()) { // REMOVED GetEnhancedBool check
					part.caption.setMarkedText(
						st::messageTextStyle,
						originalText,
						kDefaultTextOptions);
					const auto padding = QMargins(8, 0, 8, 0);
					part._captionHeight = part.caption.countHeight(part.geometry.width() - padding.left() - padding.right()) + padding.top() + padding.bottom();
				} else {
					part._captionHeight = 0;
				}
				accumulate_max(maxCaptionHeight, float64(part._captionHeight));
			}

			if (!indices.empty()) {
				auto &last_part_in_row = _parts[indices.back()];
				if (!(last_part_in_row.sides & RectPart::Right)) {
					// Fix: Only expand the last item in the row if it's not the last item in the whole album
					// This prevents the extra space on the right side of the last item
					if (last_part_in_row.geometry.x() + last_part_in_row.geometry.width() < newWidth) {
						const auto availableWidth = newWidth - last_part_in_row.geometry.x();
						last_part_in_row.geometry.setWidth(availableWidth);
					}
				}
			}
			const auto rowHeight = maxMediaHeight + maxCaptionHeight;
			for (const auto i : indices) {
				auto &part = _parts[i];
				// If this is the last item in the row (rightmost), remove the right border
				if (i == indices.back()) {
					part.sides = part.sides & (~RectPart::Right);
				}
				part.geometry.setY(y + (maxMediaHeight - part.geometry.height()));
				const auto mediaGeometry = part.geometry;
				part.captionRect = (part._captionHeight > 0)
					? QRect(
						mediaGeometry.left(),
						mediaGeometry.bottom(),
						mediaGeometry.width(),
						part._captionHeight)
					: QRect();
			}
			y += rowHeight + spacing;
		}
		newHeight = (y > 0) ? (y - spacing) : 0;
	}

	const auto groupPadding = groupedPadding();
	newHeight += groupPadding.top() + groupPadding.bottom();

	return { newWidth, int(base::SafeRound(newHeight)) };
}

void GroupedMedia::refreshParentId(
		not_null<HistoryItem*> realParent) {
	for (const auto &part : _parts) {
		part.content->refreshParentId(part.item);
	}
}

Ui::BubbleRounding GroupedMedia::applyRoundingSides(
		Ui::BubbleRounding already,
		RectParts sides) const {
	auto result = Ui::GetCornersFromSides(sides);
	if (!(result & RectPart::TopLeft)) {
		already.topLeft = Ui::BubbleCornerRounding::None;
	}
	if (!(result & RectPart::TopRight)) {
		already.topRight = Ui::BubbleCornerRounding::None;
	}
	if (!(result & RectPart::BottomLeft)) {
		already.bottomLeft = Ui::BubbleCornerRounding::None;
	}
	if (!(result & RectPart::BottomRight)) {
		already.bottomRight = Ui::BubbleCornerRounding::None;
	}
	return already;
}

QMargins GroupedMedia::groupedPadding() const {
	if (_mode != Mode::Column) {
		if (isBubbleBottom() && GetEnhancedBool("caption_from_file_name")) {
			// Add bottom padding only if any Grid item actually has a caption.
			bool hasAnyCaption = false;
			for (const auto &part : _parts) {
				if (part._captionHeight > 0) { hasAnyCaption = true; break; }
			}
			if (hasAnyCaption) {
				const auto padding = st::msgPadding;
				return QMargins(0, 0, 0, padding.bottom());
			}
		}
		return QMargins();
	}
	const auto normal = st::msgFileLayout.padding;
	const auto grouped = st::msgFileLayoutGrouped.padding;
	const auto topMinus = isBubbleTop() ? 0 : st::msgFileTopMinus;
	// Do not add extra bottom padding in Column mode for captions.
	// Extra padding created a visible empty row after the last caption.
	return QMargins(
		0,
		(normal.top() - grouped.top()) - topMinus,
		0,
		(normal.bottom() - grouped.bottom()));
}

Media *GroupedMedia::lookupSpoilerTagMedia() const {
	if (_parts.empty()) {
		return nullptr;
	}
	const auto media = _parts.front().content.get();
	if (media && _parts.front().item->isMediaSensitive()) {
		return media;
	}
	const auto photo = media ? media->getPhoto() : nullptr;
	return (photo && photo->extendedMediaPreview()) ? media : nullptr;
}

QImage GroupedMedia::generateSpoilerTagBackground(QRect full) const {
	const auto ratio = style::DevicePixelRatio();
	auto result = QImage(
		full.size() * ratio,
		QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(ratio);
	auto p = QPainter(&result);
	const auto shift = -full.topLeft();
	const auto skip1 = st::historyGroupSkip / 2;
	const auto skip2 = st::historyGroupSkip - skip1;
	for (const auto &part : _parts) {
		auto background = part.content->spoilerTagBackground();
		const auto extended = part.geometry.translated(shift).marginsAdded(
			{ skip1, skip1, skip2, skip2 });
		if (background.isNull()) {
			p.fillRect(extended, Qt::black);
		} else {
			p.drawImage(extended, background);
		}
	}
	p.end();

	return ::Media::Streaming::PrepareBlurredBackground(
		full.size(),
		std::move(result));
}

void GroupedMedia::drawHighlight(
		Painter &p,
		const PaintContext &context,
		int top) const {
	if (context.highlight.opacity == 0.) {
		return;
	}
	auto selection = context.highlight.range;
	if (_mode != Mode::Column) {
		if (!selection.empty() && !IsSubGroupSelection(selection)) {
			_parent->paintCustomHighlight(
				p,
				context,
				top,
				height(),
				_parent->data().get());
		}
		return;
	}
	const auto empty = selection.empty();
	const auto subpart = IsSubGroupSelection(selection);
	const auto skip = top + groupedPadding().top();
	for (auto i = 0, count = int(_parts.size()); i != count; ++i) {
		const auto &part = _parts[i];
		const auto rect = part.geometry.translated(0, skip);
		const auto full = (!i && empty)
			|| (subpart && IsGroupItemSelection(selection, i))
			|| (!subpart
				&& !selection.empty()
				&& (selection.from < part.content->fullSelectionLength()));
		if (!subpart) {
			selection = part.content->skipSelection(selection);
		}
		if (full) {
			auto copy = context;
			copy.highlight.range = {};
			_parent->paintCustomHighlight(
				p,
				copy,
				rect.y(),
				rect.height(),
				part.item);
		}
	}
}

void GroupedMedia::draw(Painter &p, const PaintContext &context) const {
	auto wasCache = false;
	auto nowCache = false;
	const auto groupPadding = groupedPadding();
	auto selection = context.selection;
	const auto fullSelection = (selection == FullSelection);
	const auto textSelection = (_mode == Mode::Column)
		&& !fullSelection
		&& !IsSubGroupSelection(selection);
	const auto inWebPage = (_parent->media() != this);
	constexpr auto kSmall = Ui::BubbleCornerRounding::Small;
	const auto rounding = inWebPage
		? Ui::BubbleRounding{ kSmall, kSmall, kSmall, kSmall }
		: adjustedBubbleRounding();
	auto highlight = context.highlight.range;
	const auto tagged = lookupSpoilerTagMedia();
	auto fullRect = QRect();
	const auto subpartHighlight = IsSubGroupSelection(highlight);
	const auto st = context.st;
	const auto stm = context.messageStyle();

	for (auto i = 0, count = int(_parts.size()); i != count; ++i) {
		const auto &part = _parts[i];
		auto partContext = context.withSelection(fullSelection
			? FullSelection
			: textSelection
			? selection
			: IsGroupItemSelection(selection, i)
			? FullSelection
			: TextSelection());
		const auto highlighted = (highlight.empty() && !i)
			|| IsGroupItemSelection(highlight, i);
		const auto highlightOpacity = highlighted
			? context.highlight.opacity
			: 0.;
		partContext.highlight.range = highlighted
			? TextSelection()
			: highlight;
		if (textSelection) {
			selection = part.content->skipSelection(selection);
		}
		if (!subpartHighlight) {
			highlight = part.content->skipSelection(highlight);
		}
		if (!part.cache.isNull()) {
			wasCache = true;
		}

		part.content->drawGrouped(
			p,
			partContext,
			part.geometry.translated(0, groupPadding.top()),
			part.sides,
			applyRoundingSides(rounding, part.sides),
			highlightOpacity,
			&part.cacheKey,
			&part.cache);

		// Draw video info bubble (duration + size) on Grid albums, top-left, for all videos.
		if (_mode == Mode::Grid) {
			const auto dataMedia = part.item->media();
			qint64 durSeconds = -1;
			qint64 sizeBytes = -1;
			if (const auto file = dynamic_cast<Data::MediaFile*>(dataMedia)) {
				const auto document = file->document();
				if (document && document->isVideoFile()) {
					durSeconds = std::max<qint64>(0, document->duration() / 1000);
					sizeBytes = document->size;
				}
			} else if (const auto photoMedia = dynamic_cast<Data::MediaPhoto*>(dataMedia)) {
				const auto photo = photoMedia->photo();
				if (photo && photo->videoCanBePlayed()) {
					if (const auto d = photo->extendedMediaVideoDuration()) {
						durSeconds = std::max<qint64>(0, *d);
					} else {
						durSeconds = 0;
					}
					sizeBytes = photo->videoByteSize(Data::PhotoSize::Large);
				}
			}

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

				auto mediaGeometry = part.geometry.translated(0, groupPadding.top());
				const auto bubbleX = mediaGeometry.x() + mediaGeometry.width() - bubbleW - st::msgDateImgDelta;
				const auto bubbleY = mediaGeometry.y() + mediaGeometry.height() - bubbleH - st::msgDateImgDelta;

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

		if (_mode == Mode::Column) {
			// Draw info for first item (views/date/id), msg-id bubbles for the rest in Column mode
			QString infoText;
			const auto item = part.item;
			const auto edited = item->Get<HistoryMessageEdited>();

			if (edited && !item->hideEditedBadge()) {
				infoText += QString::fromUtf8("✏️");
			}

			if (GetEnhancedBool("show_messages_id")) {
				const auto msgId = item->fullId().msg;
				if (msgId > 0) {
					if (!infoText.isEmpty()) {
						infoText += ' ';
					}
					infoText += QString::number(msgId.bare);
				}
			}

			if (i == 0) {
				// First item: draw views icon + views count + edited (if any) + time + id
				const auto st = context.st;
				const auto stm = context.messageStyle();
				p.setFont(st::msgDateFont);
				p.setPen(stm->msgDateFg);

				const auto itemRect = part.geometry.translated(0, groupPadding.top());
				const auto &docStyle = st::msgFileLayoutGrouped;
				const auto statustop = docStyle.statusTop - st::msgFileTopMinus;
				const auto baseY = itemRect.y() + statustop + st::msgDateFont->ascent;

				// Compose pieces
				const auto views = item->Get<HistoryMessageViews>();
				const auto viewsText = (views && views->views.count >= 0)
					? Lang::FormatCountToShort(std::max(views->views.count, 1)).string
					: QString();
				const bool editedNow = (item->Get<HistoryMessageEdited>() && !item->hideEditedBadge());
				const auto editText = editedNow ? (QString::fromUtf8("✏️") + " ") : QString();
				const auto timeText = QLocale().toString(
					ItemDateTime(item).time(),
					GetEnhancedBool("show_seconds")
						? QLocale::system().timeFormat(QLocale::LongFormat).remove("t")
						: QLocale::system().timeFormat(QLocale::ShortFormat));
				const auto idText = (GetEnhancedBool("show_messages_id") && item->fullId().msg > 0)
					? (QString(" ") + QString::number(item->fullId().msg.bare))
					: QString();

				// Metrics and layout
				const int iconGap = 1;
				const int textGap = st::msgDateFont->width(' '); // general gap between blocks
				const int iconW = st::historyViewsWidth;
				const int viewsW = viewsText.isEmpty() ? 0 : (iconW + iconGap + st::msgDateFont->width(viewsText));
				const int editedW = editedNow ? st::msgDateFont->width(editText) : 0;
				const int timeIdW = st::msgDateFont->width(timeText + idText);
				int totalW = 0;
				if (viewsW > 0) totalW += viewsW + textGap;
				if (editedW > 0) totalW += editedW + textGap;
				totalW += timeIdW;

				int x = itemRect.x() + itemRect.width() - totalW - st::msgDateImgDelta;
				// Prevent overlay from overlapping the document status text (e.g., "12.9 MB").
				// Compute the left bound of the status text area for Column grouped layout.
				int reservedLeft = 0;
				{
					const auto &docStyle = st::msgFileLayoutGrouped;
					const int nameleft = docStyle.padding.left() + docStyle.thumbSize + docStyle.thumbSkip;
					QString statusText;
					if (const auto fileMedia = dynamic_cast<Data::MediaFile*>(part.item->media())) {
						if (const auto document = fileMedia->document()) {
							statusText = Ui::FormatSizeText(document->size);
						}
					}
					if (!statusText.isEmpty()) {
						reservedLeft = nameleft + st::normalFont->width(statusText) + st::normalFont->spacew;
					}
				}
				// If the overlay would overlap the status text, push it right to leave a gap.
				if (reservedLeft > 0) {
					const auto minGap = st::normalFont->spacew;
					const auto minX = reservedLeft + minGap;
					if (x < minX) x = minX;
				}
				// Draw views icon + count
				if (viewsW > 0) {
					// Use non-inverted icon to match text color inside the bubble.
					const auto &icon = stm->historyViewsIcon;
					const int iconH = icon.height();
					const int scaledH = (iconH * iconW) / std::max(1, icon.width());
					const int iconTop = baseY - st::msgDateFont->ascent + (st::msgDateFont->height - scaledH) / 2;
					icon.paint(p, x, iconTop, iconW);
					p.drawText(x + iconW + iconGap, baseY, viewsText);
					x += viewsW + textGap;
				}
				// Draw edited icon if any
				if (editedW > 0) {
					p.drawText(x, baseY, editText);
					x += editedW + textGap;
				}
				// Draw time + id
				p.drawText(x, baseY, timeText + idText);
			} else if (!infoText.isEmpty()) {
				const auto st = context.st;
				const auto stm = context.messageStyle();
				p.setFont(st::msgDateFont);
				// Match single-document bubbles color
				p.setPen(stm->msgDateFg);

				const auto itemRect = part.geometry.translated(0, groupPadding.top());
				const auto textWidth = st::msgDateFont->width(infoText);
				// const auto textHeight = st::msgDateFont->height; // Unused.
				const auto &docStyle = st::msgFileLayoutGrouped;
				const auto topMinus = st::msgFileTopMinus;
				const auto statustop = docStyle.statusTop - topMinus;

				// Position on same row as file size (right aligned)
				const auto textX = itemRect.x() + itemRect.width() - textWidth - st::msgDateImgDelta;
				const auto textY = itemRect.y() + statustop + st::msgDateFont->ascent;

				p.drawText(textX, textY, infoText);
			}
		} else if (_mode == Mode::Grid && i > 0) {
			drawMessageIdInfo(p, context, part.geometry.translated(0, groupPadding.top()), part.item);
		}

		// FIX #8: Restore caption drawing for Grid mode.
		if ((_mode == Mode::Grid) && part._captionHeight > 0) {
			auto mediaGeometry = part.geometry.translated(0, groupPadding.top());

			if (!part.caption.isEmpty() && part._captionHeight > 0) {
				auto captionRect = QRect(
					mediaGeometry.left(),
					mediaGeometry.bottom() + 1,
					mediaGeometry.width(),
					part._captionHeight
				);
				// Use the standard message text color for captions.
				p.setPen(stm->historyTextFg);
				const auto padding = QMargins(8, 0, 8, 0);
				const auto elision = (_captionsCount > 1);

				uint16 captionOffset = 0;
				for (int j = 0; j < i; ++j) {
					captionOffset += _parts[j].caption.length();
				}
				const auto captionLength = part.caption.length();
				const auto intersection = TextSelection{
					std::max(context.selection.from, captionOffset),
					std::min(context.selection.to, uint16(captionOffset + captionLength))
				};
				const auto partSelection = (intersection.from < intersection.to)
					? TextSelection{
						uint16(intersection.from - captionOffset),
						uint16(intersection.to - captionOffset)
					}
					: TextSelection{};

				part.caption.draw(p, {
					.position = captionRect.topLeft() + QPoint(padding.left(), padding.top()),
					.availableWidth = captionRect.width() - padding.left() - padding.right(),
					.palette = &stm->textPalette,
					.selection = partSelection,
					.elisionLines = elision ? 1 : 0,
				});
			}
		}
		
		if (!part.cache.isNull()) {
			nowCache = true;
		}
		if (tagged || _purchasedPriceTag) {
			fullRect = fullRect.united(part.geometry);
		}
	}
	if (nowCache && !wasCache) {
		history()->owner().registerHeavyViewPart(_parent);
	}

	if (tagged) {
		tagged->drawSpoilerTag(p, fullRect, context, [&] {
			return generateSpoilerTagBackground(fullRect);
		});
	} else if (_purchasedPriceTag) {
		drawPurchasedTag(p, fullRect, context);
	}

	// --- Comprehensive Bubble Logic ---
	if ((_mode == Mode::Grid) && !_parts.empty() && _parent->media() == this) {
		const auto firstPart = &_parts.front();
		const auto firstItemGeometry = firstPart->geometry.translated(0, groupPadding.top());

		if (_mode == Mode::Grid && (!_parent->hasBubble() || isBubbleBottom())) {
			// --- 1. Gather data and prepare text parts ---
			const auto item = firstPart->item;
			const auto st = context.st;
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

			// --- 2. Calculate layout from right to left ---
			int totalWidth = 0;
			const int textPadding = font->width(' ');

			const int dateWidth = font->width(dateText + msgIdText);
			totalWidth += dateWidth;

			if (edited) {
				const auto editedWidth = font->width(QString::fromUtf8("✏️") + " ");
				// Include spacing before the edited icon for visual separation
				totalWidth += textPadding + editedWidth;
			}

			int viewsWidth = 0;
			const int viewsIconGap = 1; // gap between views icon and count
			if (!viewsText.isEmpty()) {
				viewsWidth = st::historyViewsWidth + viewsIconGap + font->width(viewsText);
				// Include extra spacing around views block to match Column order
				totalWidth += (2 * textPadding) + viewsWidth;
			}

			// --- 3. Draw bubble ---
			const auto sti = context.imageStyle();
			const auto textHeight = font->height;
			const auto hPadding = 2;
			const auto vPadding = st::msgDateImgPadding.y();
			const auto bubbleW = totalWidth + 2 * hPadding;
			const auto bubbleH = textHeight + 2 * vPadding;

			const auto bubbleX = firstItemGeometry.x() + firstItemGeometry.width() - bubbleW - st::msgDateImgDelta;
			const auto bubbleY = firstItemGeometry.y() + st::msgDateImgDelta;

			// Draw with uniform style brush to avoid edge transparency differences
			p.save();
			p.setOpacity(0.95);
			Ui::FillRoundRect(p, bubbleX, bubbleY, bubbleW, bubbleH, sti->msgDateImgBg, sti->msgDateImgBgCorners);
			p.restore();

			// --- 4. Draw content ---
			// Use proper white color for text on image bubbles
			p.setPen(st->msgDateImgFg());
			p.setFont(font->bold());
			const int textBaseY = bubbleY + (bubbleH - textHeight) / 2 + font->ascent;
			// Draw left-to-right: views icon + count, edited icon, time + id
			int currentLeft = bubbleX + hPadding;
			if (!viewsText.isEmpty()) {
				const auto &icon = st->historyViewsInvertedIcon();
				const int baseIconW = std::max(1, icon.width());
				const int baseIconH = icon.height();
				const int scaledIconH = (baseIconH * st::historyViewsWidth) / baseIconW;
				const int iconY = bubbleY + (bubbleH - scaledIconH) / 2;
				icon.paint(p, currentLeft, iconY, st::historyViewsWidth);
				p.drawText(currentLeft + st::historyViewsWidth + viewsIconGap, textBaseY, viewsText);
				currentLeft += viewsWidth + textPadding;
			}
			if (edited) {
				const auto editedText = QString::fromUtf8("✏️") + " ";
				const auto editedWidth = font->width(editedText);
				p.setFont(font);
				p.drawText(currentLeft, textBaseY, editedText);
				currentLeft += editedWidth + textPadding;
				p.setFont(font->bold());
			}
			p.drawText(currentLeft, textBaseY, dateText + msgIdText);

			if (const auto size = _parent->hasBubble() ? std::nullopt : _parent->rightActionSize()) {
				auto fullRight = width();
				auto fullBottom = height();
				auto fastShareLeft = _parent->hasRightLayout()
					? (-size->width() - st::historyFastShareLeft)
					: (fullRight + st::historyFastShareLeft);
				auto fastShareTop = (fullBottom - st::historyFastShareBottom - size->height());
				_parent->drawRightAction(p, context, fastShareLeft, fastShareTop, width());
			}
		}
	}
}

TextState GroupedMedia::getPartState(
		QPoint point,
		StateRequest request) const {
	auto shift = 0;
	auto i = 0;
	for (const auto &part : _parts) {
		const auto isInside = part.geometry.contains(point)
			|| (!part.captionRect.isEmpty() && part.captionRect.contains(point));
		if (isInside) {
			if (_mode == Mode::Grid
				&& !part.captionRect.isEmpty()
				&& part.captionRect.contains(point)) {
				const auto originalText = part.item->originalText();
				if (!originalText.empty()) {
					uint16 captionOffset = 0;
					for (int j = 0; j < i; ++j) {
						captionOffset += _parts[j].caption.length();
					}
					const auto state = part.caption.getState(
						point - part.captionRect.topLeft(),
						part.captionRect.width(),
						request.forText());
					auto result = TextState(part.item, state);
					result.symbol += captionOffset;
					result.link = std::make_shared<CaptionClickHandler>(i);
					return result;
				}
			}
			auto result = part.content->getStateGrouped(
				part.geometry,
				part.sides,
				point,
				request);
			result.symbol += shift;
			result.itemId = part.item->fullId();

			const auto item = part.item;
			const auto edited = item->Get<HistoryMessageEdited>();

			// --- START: MODIFIED LOGIC FOR COLUMN MODE ---
			if (_mode == Mode::Column) {
				const auto &docStyle = st::msgFileLayoutGrouped;
				const auto topMinus = st::msgFileTopMinus;
				const auto statustop = docStyle.statusTop - topMinus;
				const auto itemRect = part.geometry;
				const auto y = itemRect.y() + statustop;
				const int currentRight = itemRect.x() + itemRect.width();

				if (i == 0) {
					// Three hover areas for first item: views (icon+count), edited+time+id (if edited), time+id.
					const auto font = st::msgDateFont;
					const auto timeText = QLocale().toString(
						ItemDateTime(item).time(),
						GetEnhancedBool("show_seconds")
							? QLocale::system().timeFormat(QLocale::LongFormat).remove("t")
							: QLocale::system().timeFormat(QLocale::ShortFormat));
					const auto idText = (GetEnhancedBool("show_messages_id") && item->fullId().msg > 0)
						? QString(" %1").arg(item->fullId().msg.bare)
						: QString();
					const bool editedNow = (edited && !item->hideEditedBadge());
					const int editedW = editedNow ? font->width(QString::fromUtf8("✏️") + " ") : 0;
					const int timeIdW = font->width(timeText + idText);
					const auto views = item->Get<HistoryMessageViews>();
					const auto viewsText = (views && views->views.count >= 0)
						? Lang::FormatCountToShort(std::max(views->views.count, 1)).string
						: QString();
					const int iconGap = 1;
					const int iconW = st::historyViewsWidth;
					const int viewsW = viewsText.isEmpty() ? 0 : (iconW + iconGap + font->width(viewsText));

					// Build hover rects from the left edge of the bubble to match drawing order.
					const int textGap = font->spacew;
					const int lineTop = y;
					const int lineH = font->height;

					int totalW = 0;
					if (viewsW > 0) totalW += viewsW + textGap;
					if (editedW > 0) totalW += editedW + textGap;
					totalW += timeIdW;

					const int startX = currentRight - st::msgDateImgDelta - totalW;
					int currentX = startX;

					QRect viewsRect;
					if (viewsW > 0) {
						viewsRect = QRect(currentX, lineTop, viewsW, lineH);
						currentX += viewsW + textGap;
					}
					QRect editedOnlyRect;
					if (editedW > 0) {
						editedOnlyRect = QRect(currentX, lineTop, editedW, lineH);
						currentX += editedW + textGap;
					}
					const QRect timeIdRect(currentX, lineTop, timeIdW, lineH);
					// Edited tooltip should cover both the edited icon and the time+id area when present.
					const QRect editedRect = editedNow
						? QRect(editedOnlyRect.isNull() ? timeIdRect.left() : editedOnlyRect.left(),
							lineTop,
							(editedOnlyRect.isNull() ? 0 : editedOnlyRect.width()) + timeIdRect.width(),
							lineH)
						: QRect();

					// Views tooltip
					if (viewsW > 0 && viewsRect.contains(point)) {
						result.customTooltip = true;
						result.customTooltipText = QString("Views: ") + viewsText;
					// Edited tooltip (includes both Uploaded and Edited lines)
					} else if (editedNow && editedRect.contains(point)) {
						const auto uploadLocal = QDateTime::fromSecsSinceEpoch(item->date()).toLocalTime();
						QString tooltipText = tr::lng_uploaded(tr::now) + ": "
							+ uploadLocal.date().toString("dddd, dd MMMM yyyy") + " "
							+ uploadLocal.time().toString("HH:mm:ss");
						const auto editLocal = QDateTime::fromSecsSinceEpoch(edited->date).toLocalTime();
						QString editedTranslation = tr::lng_edited(tr::now);
						editedTranslation = editedTranslation.toUpper().left(1) + editedTranslation.mid(1);
						tooltipText += "\n" + editedTranslation + ": "
							+ editLocal.date().toString("dddd, dd MMMM yyyy") + " "
							+ editLocal.time().toString("HH:mm:ss");
						result.customTooltip = true;
						result.customTooltipText = tooltipText;
					// Uploaded tooltip (time+id area)
					} else if (timeIdRect.contains(point)) {
						const auto uploadLocal = QDateTime::fromSecsSinceEpoch(item->date()).toLocalTime();
						QString tooltipText = tr::lng_uploaded(tr::now) + ": "
							+ uploadLocal.date().toString("dddd, dd MMMM yyyy") + " "
							+ uploadLocal.time().toString("HH:mm:ss");
						result.customTooltip = true;
						result.customTooltipText = tooltipText;
					}
				} else {
				// Calculate hover rect for items 2..N.
				QString infoText;
				if (edited && !item->hideEditedBadge()) {
					infoText += QString::fromUtf8("✏️");
				}
				if (GetEnhancedBool("show_messages_id")) {
					const auto msgId = item->fullId().msg;
					if (msgId > 0) {
						if (!infoText.isEmpty()) infoText += ' ';
						infoText += QString::number(msgId.bare);
					}
				}
				if (!infoText.isEmpty()) {
					const auto textWidth = st::msgDateFont->width(infoText);
					const auto textHeight = st::msgDateFont->height;
					const auto hPadding = 2;
					const auto vPadding = st::msgDateImgPadding.y();
					const auto dateW = textWidth + (2 * hPadding);
					const auto dateH = textHeight + 2 * vPadding;
					const auto bubbleX = currentRight - dateW - st::msgDateImgDelta;
					const auto bubbleY = y;
					const QRect infoRect(bubbleX, bubbleY, dateW, dateH);

					if (infoRect.contains(point)) {
						QString tooltipText;
						// Always show Message ID tooltip
						if (GetEnhancedBool("show_messages_id")) {
							const auto msgId = item->fullId().msg;
							if (msgId > 0) {
								tooltipText = QString("Message ID: ") + QString::number(msgId.bare);
							}
						}
						// If edited, add edited time on a new line
						if (edited && !item->hideEditedBadge()) {
							const auto editUTCTime = QDateTime::fromSecsSinceEpoch(edited->date);
							const auto editLocalTime = editUTCTime.toLocalTime();
							QString editedTranslation = tr::lng_edited(tr::now);
							editedTranslation = editedTranslation.toUpper().left(1)
								+ editedTranslation.mid(1);
							if (!tooltipText.isEmpty()) tooltipText += "\n";
							tooltipText += editedTranslation + ": "
								+ editLocalTime.date().toString("dddd, dd MMMM yyyy") + " "
								+ editLocalTime.time().toString("HH:mm:ss");
						}
						if (!tooltipText.isEmpty()) {
							result.customTooltip = true;
							result.customTooltipText = tooltipText;
						}
					}
				}
			}
		}
			// --- END: MODIFIED LOGIC FOR COLUMN MODE ---
				// START: New tooltip logic for Grid album items (2..N).
			else if (_mode == Mode::Grid && (&part != &_parts.front())) {
				QString infoText;
				if (edited && !item->hideEditedBadge()) {
					infoText += QString::fromUtf8("✏️");
				}
				if (GetEnhancedBool("show_messages_id")) {
					const auto msgId = item->fullId().msg;
					if (msgId > 0) {
						if (!infoText.isEmpty()) infoText += ' ';
						infoText += QString::number(msgId.bare);
					}
				}

				if (!infoText.isEmpty()) {
					const auto textWidth = st::msgDateFont->width(infoText);
					const auto textHeight = st::msgDateFont->height;
					const auto hPadding = 2;
					const auto vPadding = st::msgDateImgPadding.y();
					const auto dateW = textWidth + (2 * hPadding);
					const auto dateH = textHeight + 2 * vPadding;
					const auto itemRect = part.geometry;

					const auto bubbleX = (dateW > itemRect.width())
						? itemRect.x()
						: (itemRect.x() + itemRect.width() - dateW);
					const auto bubbleY = itemRect.y();
					const QRect infoRect(bubbleX, bubbleY, dateW, dateH);

					if (infoRect.contains(point)) {
						QString tooltipText;
						// Always show Message ID tooltip
						if (GetEnhancedBool("show_messages_id")) {
							const auto msgId = item->fullId().msg;
							if (msgId > 0) {
								tooltipText = QString("Message ID: ") + QString::number(msgId.bare);
							}
						}
						// If edited, add edited time on a new line
						if (edited && !item->hideEditedBadge()) {
							const auto editUTCTime = QDateTime::fromSecsSinceEpoch(edited->date);
							const auto editLocalTime = editUTCTime.toLocalTime();
							QString editedTranslation = tr::lng_edited(tr::now);
							editedTranslation = editedTranslation.toUpper().left(1)
								+ editedTranslation.mid(1);
							if (!tooltipText.isEmpty()) tooltipText += "\n";
							tooltipText += editedTranslation + ": "
								+ editLocalTime.date().toString("dddd, dd MMMM yyyy") + " "
								+ editLocalTime.time().toString("HH:mm:ss");
						}
						if (!tooltipText.isEmpty()) {
							result.customTooltip = true;
							result.customTooltipText = tooltipText;
						}
					}
				}
				// END: New tooltip logic for Grid album items (2..N).
			}



			return result;
		}
		shift += part.content->fullSelectionLength();
		++i;
	}

	if (!_parts.empty() && needInfoDisplay()) {
		const auto firstPart = &_parts.front();
		const auto groupPadding = groupedPadding();
		const auto firstItemGeometry = firstPart->geometry.translated(0, groupPadding.top());

		QString infoText;
		if (const auto views = firstPart->item->Get<HistoryMessageViews>()) {
			if (views->views.count >= 0) {
				infoText += QString::number(views->views.count) + " ";
			}
		}
		const auto dateText = QLocale().toString(
			ItemDateTime(firstPart->item).time(),
			QLocale::ShortFormat);
		infoText += dateText + " ";
		const auto msgId = firstPart->item->fullId().msg;
		if (msgId > 0) {
			infoText += QString::number(msgId.bare);
		}

		const auto textWidth = st::msgDateFont->width(infoText);
		const auto textHeight = st::msgDateFont->height;
		const auto dateW = textWidth + 2 * st::msgDateImgPadding.x();
		const auto dateH = textHeight + 2 * st::msgDateImgPadding.y();
		const auto dateX = (dateW > firstItemGeometry.width()) ? firstItemGeometry.x() : (firstItemGeometry.x() + firstItemGeometry.width() - dateW);
		const auto dateY = firstItemGeometry.y();
		const auto infoRect = QRect(dateX, dateY, dateW, dateH);


	}

	return TextState(_parent->data());
}

PointState GroupedMedia::pointState(QPoint point) const {
	if (!QRect(0, 0, width(), height()).contains(point)) {
		return PointState::Outside;
	}
	const auto groupPadding = groupedPadding();
	point -= QPoint(0, groupPadding.top());
	for (const auto &part : _parts) {
		if (part.geometry.contains(point)) {
			return PointState::GroupPart;
		}
	}
	return PointState::Inside;
}

TextState GroupedMedia::textState(QPoint point, StateRequest request) const {
	const auto groupPadding = groupedPadding();
	auto result = getPartState(point - QPoint(0, groupPadding.top()), request);

	const auto isCaptionLink = result.link
		&& result.link->property(kCaptionPartIndexProperty).isValid();

	if (const auto tagged = lookupSpoilerTagMedia()) {
		if (QRect(0, 0, width(), height()).contains(point)) {
			if (auto link = tagged->spoilerTagLink()) {
				if (!isCaptionLink) {
					result.link = std::move(link);
				}
			}
		}
	}

	if ((_mode == Mode::Grid || _mode == Mode::Column) && !_parts.empty() && _parent->media() == this) {
		const auto firstPart = &_parts.front();
		const auto firstItemGeometry = firstPart->geometry.translated(0, groupPadding.top());

		if (_mode == Mode::Grid && (!_parent->hasBubble() || isBubbleBottom())) {
			const auto right = firstItemGeometry.x() + firstItemGeometry.width();
			const auto bottom = firstItemGeometry.y(); // Match top-right alignment.

			// Manually calculate the info rect to match the drawing logic.
			const auto item = firstPart->item;
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
			const int iconPadding = st::historyViewsSpace;
			const int textPadding = font->spacew;
			const int dateWidth = font->width(dateText + msgIdText);
			totalWidth += dateWidth;
			if (edited) {
				const auto editedWidth = font->width(QString::fromUtf8("✏️") + " ");
				totalWidth += editedWidth;
			}
			int viewsWidth = 0;
			if (!viewsText.isEmpty()) {
				viewsWidth = st::historyViewsWidth + iconPadding + font->width(viewsText);
				totalWidth += textPadding + viewsWidth;
			}

			const auto hPadding = 2;
			const auto vPadding = st::msgDateImgPadding.y();
			const auto bubbleW = totalWidth + 2 * hPadding;
			const auto bubbleH = font->height + 2 * vPadding;
			const auto bubbleX = right - bubbleW - st::msgDateImgDelta;
			const auto bubbleY = bottom + st::msgDateImgDelta; // 'bottom' here is actually the top edge.

			const QRect infoRect(bubbleX, bubbleY, bubbleW, bubbleH);
				// Build three hover areas for Grid first bubble from the left edge:
				// views (icon+count), edited+time+id (if edited), time+id.
				const int iconGap = 1;
				const int iconW = st::historyViewsWidth;
				const int viewsW = viewsText.isEmpty() ? 0 : (iconW + iconGap + font->width(viewsText));
				const int editedW = edited ? font->width(QString::fromUtf8("✏️") + " ") : 0;
				const int timeIdW = font->width(dateText + msgIdText);
				int hoverLeft = bubbleX + hPadding;
				QRect viewsRect, timeIdRect, editedRect;
				if (viewsW > 0) {
					viewsRect = QRect(hoverLeft, bubbleY, viewsW, bubbleH);
					hoverLeft += viewsW + textPadding;
				}
				QRect editedOnlyRect;
				if (edited) {
					editedOnlyRect = QRect(hoverLeft, bubbleY, editedW, bubbleH);
					hoverLeft += editedW + textPadding;
				}
				timeIdRect = QRect(hoverLeft, bubbleY, timeIdW, bubbleH);
				// Edited tooltip covers both the edited icon and the time+id area when present.
				if (edited) {
					editedRect = QRect(editedOnlyRect.isNull() ? timeIdRect.left() : editedOnlyRect.left(),
						bubbleY,
						(editedOnlyRect.isNull() ? 0 : editedOnlyRect.width()) + timeIdRect.width(),
						bubbleH);
				}
				// Views tooltip
				if (viewsW > 0 && viewsRect.contains(point)) {
				result.customTooltip = true;
				result.customTooltipText = QString("Views: ") + viewsText;
				return result;
			}
			// Edited tooltip includes Uploaded + Edited lines
			if (edited && editedRect.contains(point)) {
				const auto uploadLocalTime = QDateTime::fromSecsSinceEpoch(item->date()).toLocalTime();
				QString tooltipText = tr::lng_uploaded(tr::now) + ": "
					+ uploadLocalTime.date().toString("dddd, dd MMMM yyyy") + " "
					+ uploadLocalTime.time().toString("HH:mm:ss");
				const auto msgIdValue = item->fullId().msg;
				if (msgIdValue > 0) {
					tooltipText += "  ID: " + QString::number(msgIdValue.bare);
				}
				const auto editLocalTime = QDateTime::fromSecsSinceEpoch(item->Get<HistoryMessageEdited>()->date).toLocalTime();
				QString editedTranslation = tr::lng_edited(tr::now);
				editedTranslation = editedTranslation.toUpper().left(1) + editedTranslation.mid(1);
				tooltipText += "\n" + editedTranslation + ": "
					+ editLocalTime.date().toString("dddd, dd MMMM yyyy") + " "
					+ editLocalTime.time().toString("HH:mm:ss");
				result.customTooltip = true;
				result.customTooltipText = tooltipText;
				return result;
			}
			// Uploaded tooltip for time+id area
			if (timeIdRect.contains(point)) {
				const auto uploadLocalTime = QDateTime::fromSecsSinceEpoch(item->date()).toLocalTime();
				QString tooltipText = tr::lng_uploaded(tr::now) + ": "
					+ uploadLocalTime.date().toString("dddd, dd MMMM yyyy") + " "
					+ uploadLocalTime.time().toString("HH:mm:ss");
				const auto msgIdValue = item->fullId().msg;
				if (msgIdValue > 0) {
					tooltipText += "  ID: " + QString::number(msgIdValue.bare);
				}
				result.customTooltip = true;
				result.customTooltipText = tooltipText;
				return result;
			}

				// Remove conflicting right-anchored views hover area; views belong to the left.

			if (const auto size = _parent->hasBubble() ? std::nullopt : _parent->rightActionSize()) {
				auto fullRight = width();
				auto fullBottom = height();
				auto fastShareLeft = _parent->hasRightLayout()
					? (-size->width() - st::historyFastShareLeft)
					: (fullRight + st::historyFastShareLeft);
				auto fastShareTop = (fullBottom - st::historyFastShareBottom - size->height());
				if (QRect(fastShareLeft, fastShareTop, size->width(), size->height()).contains(point)) {
					result.link = _parent->rightActionLink(point
						- QPoint(fastShareLeft, fastShareTop));
				}
			}
		}
	}

	return result;
}

bool GroupedMedia::toggleSelectionByHandlerClick(
		const ClickHandlerPtr &p) const {
	for (const auto &part : _parts) {
		if (part.content->toggleSelectionByHandlerClick(p)) {
			return true;
		}
	}
	return false;
}

bool GroupedMedia::dragItemByHandler(const ClickHandlerPtr &p) const {
	for (const auto &part : _parts) {
		if (part.content->dragItemByHandler(p)) {
			return true;
		}
	}
	return false;
}

TextSelection GroupedMedia::adjustSelection(
		TextSelection selection,
		TextSelectType type) const {
	if (_mode == Mode::Grid) {
		auto checked = 0;
		for (const auto &part : _parts) {
			const auto &caption = part.caption;
			const auto length = caption.length();
			if (selection.from >= checked && selection.from < checked + length) {
				const auto partSelection = TextSelection{
					uint16(selection.from - checked),
					uint16(selection.to - checked),
				};
				const auto adjusted = caption.adjustSelection(partSelection, type);
				return {
					uint16(adjusted.from + checked),
					uint16(adjusted.to + checked),
				};
			}
			checked += length;
		}
		return selection;
	}
	if (_mode != Mode::Column) {
		return {};
	}
	auto checked = 0;
	for (const auto &part : _parts) {
		const auto modified = ShiftItemSelection(
			part.content->adjustSelection(
				UnshiftItemSelection(selection, checked),
				type),
			checked);
		const auto till = checked + part.content->fullSelectionLength();
		if (selection.from >= checked && selection.from < till) {
			selection.from = modified.from;
		}
		if (selection.to <= till) {
			selection.to = modified.to;
			return selection;
		}
		checked = till;
	}
	return selection;
}

uint16 GroupedMedia::fullSelectionLength() const {
	if (_mode == Mode::Grid) {
		uint16 result = 0;
		for (const auto &part : _parts) {
			result += part.caption.length();
		}
		return result;
	}
	if (_mode != Mode::Column) {
		return {};
	}
	auto result = 0;
	for (const auto &part : _parts) {
		result += part.content->fullSelectionLength();
	}
	return result;
}

bool GroupedMedia::hasTextForCopy() const {
	if (_mode == Mode::Grid) {
		return _captionsCount > 0;
	}
	if (_mode != Mode::Column) {
		return {};
	}
	for (const auto &part : _parts) {
		if (part.content->hasTextForCopy()) {
			return true;
		}
	}
	return false;
}

TextForMimeData GroupedMedia::selectedText(
		TextSelection selection) const {
	if (_mode == Mode::Grid) {
		TextForMimeData result;
		uint16 captionOffset = 0;
		for (const auto &part : _parts) {
			const auto captionLength = part.caption.length();
			if (!captionLength) {
				continue;
			}
			const auto intersection = TextSelection{
				std::max(selection.from, captionOffset),
				std::min(selection.to, uint16(captionOffset + captionLength))
			};
			if (intersection.from < intersection.to) {
				const auto partSelection = TextSelection{
					uint16(intersection.from - captionOffset),
					uint16(intersection.to - captionOffset)
				};
				if (!result.empty()) {
					result.append(u"\n\n"_q);
				}
				result.append(part.caption.toTextForMimeData(partSelection));
			}
			captionOffset += captionLength;
		}
		return result;
	}
	if (_mode != Mode::Column) {
		return {};
	}
	auto result = TextForMimeData();
	for (const auto &part : _parts) {
		auto text = part.content->selectedText(selection);
		if (!text.empty()) {
			if (result.empty()) {
				result = std::move(text);
			} else {
				result.append(u"\n\n"_q).append(std::move(text));
			}
		}
		selection = part.content->skipSelection(selection);
	}
	return result;
}

SelectedQuote GroupedMedia::selectedQuote(TextSelection selection) const {
	if (_mode != Mode::Column) {
		return {};
	}
	for (const auto &part : _parts) {
		const auto next = part.content->skipSelection(selection);
		if (next.to - next.from != selection.to - selection.from) {
			if (!next.empty()) {
				return SelectedQuote();
			}
			auto result = part.content->selectedQuote(selection);
			result.item = part.item;
			return result;
		}
		selection = next;
	}
	return {};
}

TextSelection GroupedMedia::selectionFromQuote(
		const SelectedQuote &quote) const {
	Expects(quote.item != nullptr);

	if (_mode != Mode::Column) {
		return {};
	}
	const auto i = ranges::find(_parts, not_null(quote.item), &Part::item);
	if (i == end(_parts)) {
		return {};
	}
	const auto index = int(i - begin(_parts));
	auto result = i->content->selectionFromQuote(quote);
	if (result.empty()) {
		return AddGroupItemSelection({}, index);
	}
	for (auto j = i; j != begin(_parts);) {
		result = (--j)->content->unskipSelection(result);
	}
	return result;
}

auto GroupedMedia::getBubbleSelectionIntervals(
		TextSelection selection) const
-> std::vector<Ui::BubbleSelectionInterval> {
	if (_mode != Mode::Column) {
		return {};
	}
	auto result = std::vector<Ui::BubbleSelectionInterval>();
	for (auto i = 0, count = int(_parts.size()); i != count; ++i) {
		const auto &part = _parts[i];
		if (!IsGroupItemSelection(selection, i)) {
			continue;
		}
		const auto &geometry = part.geometry;
		if (result.empty()
			|| (result.back().top + result.back().height
				< geometry.top())
			|| (result.back().top > geometry.top() + geometry.height())) {
			result.push_back({ geometry.top(), geometry.height() });
		} else {
			auto &last = result.back();
			const auto newTop = std::min(last.top, geometry.top());
			const auto newHeight = std::max(
				last.top + last.height - newTop,
				geometry.top() + geometry.height() - newTop);
			last = Ui::BubbleSelectionInterval{ newTop, newHeight };
		}
	}
	const auto groupPadding = groupedPadding();
	for (auto &part : result) {
		part.top += groupPadding.top();
	}
	if (IsGroupItemSelection(selection, 0)) {
		result.front().top -= groupPadding.top();
		result.front().height += groupPadding.top();
	}
	if (IsGroupItemSelection(selection, _parts.size() - 1)) {
		result.back().height = height() - result.back().top;
	}
	return result;
}

void GroupedMedia::clickHandlerActiveChanged(
		const ClickHandlerPtr &p,
		bool active) {
	for (const auto &part : _parts) {
		part.content->clickHandlerActiveChanged(p, active);
	}
}

void GroupedMedia::clickHandlerPressedChanged(
		const ClickHandlerPtr &p,
		bool pressed) {
	for (const auto &part : _parts) {
		part.content->clickHandlerPressedChanged(p, pressed);
		if (pressed && part.content->dragItemByHandler(p)) {
			// #TODO drag by item from album
			// App::pressedLinkItem(part.view);
		}
	}
}

template <typename DataMediaRange>
bool GroupedMedia::applyGroup(const DataMediaRange &medias) {
	if (validateGroupParts(medias)) {
		return true;
	}

	auto modeChosen = false;
	for (const auto media : medias) {
		const auto mediaMode = DetectMode(media);
		if (!modeChosen) {
			_mode = mediaMode;
			modeChosen = true;
		} else if (mediaMode != _mode) {
			continue;
		}
		_parts.push_back(Part(_parent, media));
	}
	if (_parts.empty()) {
		return false;
	}

	Ensures(_parts.size() <= kMaxSize);
	return true;
}

template <typename DataMediaRange>
bool GroupedMedia::validateGroupParts(
		const DataMediaRange &medias) const {
	auto i = 0;
	const auto count = _parts.size();
	for (const auto media : medias) {
		if (i >= count || _parts[i].item != media->parent()) {
			return false;
		}
		++i;
	}
	return (i == count);
}

not_null<Media*> GroupedMedia::main() const {
	Expects(!_parts.empty());

	return _parts.back().content.get();
}

void GroupedMedia::hideSpoilers() {
	for (const auto &part : _parts) {
		part.content->hideSpoilers();
	}
}

Storage::SharedMediaTypesMask GroupedMedia::sharedMediaTypes() const {
	return main()->sharedMediaTypes();
}

PhotoData *GroupedMedia::getPhoto() const {
	return main()->getPhoto();
}

DocumentData *GroupedMedia::getDocument() const {
	return main()->getDocument();
}

HistoryMessageEdited *GroupedMedia::displayedEditBadge() const {
	if (_mode == Mode::Column) {
		if (_parts.empty()) {
			return nullptr;
		}
		const auto &part = _parts.front();
		if (!part.item->hideEditedBadge()) {
			if (const auto edited = part.item->Get<HistoryMessageEdited>()) {
				return edited;
			}
		}
		return nullptr;
	}
	for (const auto &part : _parts) {
		if (!part.item->hideEditedBadge()) {
			if (const auto edited = part.item->Get<HistoryMessageEdited>()) {
				return edited;
			}
		}
	}
	return nullptr;
}

void GroupedMedia::updateNeedBubbleState() {
	_needBubble = computeNeedBubble();
}

void GroupedMedia::stopAnimation() {
	for (const auto &part : _parts) {
		part.content->stopAnimation();
	}
}

void GroupedMedia::checkAnimation() {
	for (const auto &part : _parts) {
		part.content->checkAnimation();
	}
}

bool GroupedMedia::hasHeavyPart() const {
	for (const auto &part : _parts) {
		if (!part.cache.isNull() || part.content->hasHeavyPart()) {
			return true;
		}
	}
	return false;
}

void GroupedMedia::unloadHeavyPart() {
	for (const auto &part : _parts) {
		part.content->unloadHeavyPart();
		part.cacheKey = 0;
		part.cache = QPixmap();
	}
}

void GroupedMedia::parentTextUpdated() {
	if (_parent->media() == this) {
		if (_mode == Mode::Column) {
			for (const auto &part : _parts) {
				part.content->parentTextUpdated();
			}
		} else {
			_captionItem = std::nullopt;
		}
	}
}

bool GroupedMedia::needsBubble() const {
	return _needBubble;
}

QPoint GroupedMedia::resolveCustomInfoRightBottom() const {
	// Anchor the default info to the first item's top-right row
	if (_parts.empty()) {
		const auto skipx = (st::msgDateImgDelta + st::msgDateImgPadding.x());
		const auto skipy = (st::msgDateImgDelta + st::msgDateImgPadding.y());
		return QPoint(width() - skipx, skipy);
	}
	const auto groupPad = groupedPadding();
	const auto &first = _parts.front();
	return QPoint(first.geometry.x() + first.geometry.width(), first.geometry.y() + groupPad.top());
}

std::optional<PaidInformation> GroupedMedia::paidInformation() const {
	auto result = PaidInformation();
	for (const auto &part : _parts) {
		++result.messages;
		result.stars += part.item->starsPaid();
	}
	return result;
}

bool GroupedMedia::enforceBubbleWidth() const {
	return _mode == Mode::Grid;
}

bool GroupedMedia::computeNeedBubble() const {
	// The assertion that was causing the crash is removed. It was based on a
	// faulty assumption that _captionItem would always have a value for non-Column modes,
	// which is not true for Grid albums with per-item captions.
	// Expects(_mode == Mode::Column || _captionItem.has_value()); // <--- REMOVED

	if (_mode == Mode::Column) {
		return true;
	}

	// For Grid mode, we lazily evaluate the caption item.
	// If a single caption item exists for the whole album, we need a bubble.
	if (itemForText()) {
		return true;
	}

	// If there's no single caption, we still need a bubble if there are
	// replies, forwards, or other elements that are drawn in the bubble.
	if (const auto item = _parent->data()) {
		if (item->repliesAreComments()
			|| item->externalReply()
			|| item->viaBot()
			|| _parent->displayReply()
			|| _parent->displayForwardedFrom()
			|| _parent->displayFromName()
			|| _parent->displayedTopicButton()
			) {
			return true;
		}
	}
	return false;
}

bool GroupedMedia::needInfoDisplay() const {

	const auto item = _parent->data();

	return (_mode != Mode::Column)

		&& (item->isSending()

			|| item->awaitingVideoProcessing()

			|| item->hasFailed()

			|| _parent->isUnderCursor()

			|| (_parent->delegate()->elementContext() == Context::ChatPreview)

			|| _parent->isLastAndSelfMessage());

}



QString GroupedMedia::getCaption(int partIndex) const {

	if (partIndex < 0 || partIndex >= _parts.size()) {

		return {};

	}

		return _parts[partIndex].caption.toString();

	}

bool GroupedMedia::hasSelectableText() const {
	return fullSelectionLength() > 0;
}

	

	int GroupedMedia::captionPartIndexAt(QPoint point) const {

		if (_mode == Mode::Grid) {

			for (int i = 0, size = _parts.size(); i < size; ++i) {

				if (!_parts[i].captionRect.isEmpty()

					&& _parts[i].captionRect.contains(point)) {

					return i;

				}

			}

		}

			return -1;

		}

		

} // namespace HistoryView