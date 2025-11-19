/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/media/history_view_media_grouped.h"

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
	const auto font = st::msgDateFont;
	p.setFont(font);
	// Use proper white color for text on image bubbles
	p.setPen(st->msgDateImgFg());

	auto textWidth = font->width(infoText);
	const auto textHeight = font->height;
	
	const auto horizontalPadding = 2;
	const auto verticalPadding = 2;

	auto dateW = textWidth + (2 * horizontalPadding);
	const auto dateH = textHeight + verticalPadding;

	if (dateW > itemGeometry.width()) {
		const auto availableWidth = itemGeometry.width()
			- (2 * horizontalPadding);
		if (availableWidth > font->width("...")) {
			const QFontMetrics metrics(font);
			infoText = metrics.elidedText(
				infoText,
				Qt::ElideRight,
				availableWidth);
			textWidth = font->width(infoText);
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

	p.setFont(font->bold());
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

	auto minHeight = 0;
	for (auto i = 0, count = int(layout.size()); i != count; ++i) {
		const auto &item = layout[i];
		accumulate_max(maxWidth, item.geometry.x() + item.geometry.width());
		accumulate_max(minHeight, item.geometry.y() + item.geometry.height());
		_parts[i].initialGeometry = item.geometry;
		_parts[i].sides = item.sides;
	}

		// Calculate caption heights for Grid mode without modifying the layout structure
	if (_mode == Mode::Grid) {
		// Group items by rows to calculate caption heights
		std::map<int, std::vector<int>> rows;
		for (auto i = 0; i != _parts.size(); ++i) {
			rows[_parts[i].initialGeometry.y()].push_back(i);
		}

		// Identify which items have captions
		std::vector<int> captionIndices;
		for (auto i = 0; i != _parts.size(); ++i) {
			if (!_parts[i].item->originalText().empty()) {
				captionIndices.push_back(i);
			}
		}
		const auto captionCount = captionIndices.size();
		const bool singleCaptionAnywhere = (captionCount == 1); // Any single caption should span full width

		// Calculate caption heights for each row
		const auto padding = QMargins(8, 2, 8, 2);  // left, top, right, bottom (2px top/bottom)
		auto lastRowBottom = 0;

		// For multiple captions, calculate a uniform caption height for ALL items with captions
		auto uniformCaptionHeight = 0;
		if (!singleCaptionAnywhere && captionCount > 0) {
			// Calculate the height needed for a single line of elided text with padding
			const auto textHeight = st::messageTextStyle.font->height;
			uniformCaptionHeight = textHeight + padding.top() + padding.bottom();

			// For consistency, we use a fixed height for all multi-captions to ensure uniformity
			// regardless of actual text content or item width differences
		}

		for (auto const& [rowY, indices] : rows) {
			// Find the bottom of this row's media
			auto rowBottom = 0;
			for (const auto i : indices) {
				rowBottom = std::max(rowBottom, _parts[i].initialGeometry.bottom());
			}
			lastRowBottom = rowBottom;

			// Calculate caption heights for this row
			for (const auto i : indices) {
				auto &part = _parts[i];
				const auto originalText = part.item->originalText();
				if (!originalText.empty()) {
					if (singleCaptionAnywhere) {
						// Case 1: Single caption anywhere, show it in full at bottom
						Ui::Text::String caption(
							st::messageTextStyle,
							originalText,
							Ui::ItemTextDefaultOptions());
						const auto captionWidth = maxWidth - padding.left() - padding.right();  // Full album width minus padding
						part._captionHeight = int(caption.countHeight(captionWidth) + padding.top() + padding.bottom());
					} else {
						// Case 2: Multiple captions, use uniform height for all items with captions
						part._captionHeight = int(uniformCaptionHeight);
					}
				} else {
					part._captionHeight = 0;
				}
			}

			// For multiple captions, add height for each row that has captions
			if (!singleCaptionAnywhere) {
				// All items in this row already have the same uniformCaptionHeight,
				// so we can use it directly without recalculating
				if (uniformCaptionHeight > 0) {
					// Add the uniform caption height to the total grid height
					minHeight += uniformCaptionHeight;
				}
			}
		}

		// For single caption, add height once at the end (after all rows)
		if (singleCaptionAnywhere && captionCount > 0) {
			const auto captionIdx = captionIndices[0];
			if (_parts[captionIdx]._captionHeight > 0) {
				minHeight += _parts[captionIdx]._captionHeight;
			}
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
		// Original Grid scaling logic - preserve layout structure
		const auto initialSpacing = st::historyGroupSkip;
		const auto factor = newWidth / float64(maxWidth());
		const auto scale = [&](int value) {
			return int(base::SafeRound(value * factor));
		};
		const auto spacing = scale(initialSpacing);
		for (auto &part : _parts) {
			const auto sides = part.sides;
			const auto initialGeometry = part.initialGeometry;
			const auto needRightSkip = !(sides & RectPart::Right);
			const auto needBottomSkip = !(sides & RectPart::Bottom);
			const auto initialLeft = initialGeometry.x();
			const auto initialTop = initialGeometry.y();
			const auto initialRight = initialLeft
				+ initialGeometry.width()
				+ (needRightSkip ? initialSpacing : 0);
			const auto initialBottom = initialTop
				+ initialGeometry.height()
				+ (needBottomSkip ? initialSpacing : 0);
			const auto left = scale(initialLeft);
			const auto top = scale(initialTop);
			const auto width = scale(initialRight)
				- left
				- (needRightSkip ? spacing : 0);
			const auto height = scale(initialBottom)
				- top
				- (needBottomSkip ? spacing : 0);
			part.geometry = QRect(left, top, width, height);

			accumulate_max(newHeight, top + height);
		}

		// Calculate caption positions and add to total height
		if (_mode == Mode::Grid) {
			// Group items by rows to position captions
			std::map<int, std::vector<int>> rows;
			for (auto i = 0; i != _parts.size(); ++i) {
				rows[_parts[i].initialGeometry.y()].push_back(i);
			}

			// Identify which items have captions
			std::vector<int> captionIndices;
			for (auto i = 0; i != _parts.size(); ++i) {
				if (!_parts[i].item->originalText().empty()) {
					captionIndices.push_back(i);
				}
			}
			const auto captionCount = captionIndices.size();
			const bool singleCaptionAnywhere = (captionCount == 1);

			const auto padding = QMargins(8, 2, 8, 2);
			auto lastRowBottom = 0;
			auto isLastRow = false;
			auto cumulativeCaptionOffset = 0;

			// Calculate the standard spacing used in this layout
			// We need this to subtract it from the offset so captions 'eat' the gap
			const auto factor = newWidth / float64(maxWidth());
			const auto spacing = int(base::SafeRound(st::historyGroupSkip * factor));

			// For multiple captions, calculate a uniform caption height
			auto uniformCaptionHeight = 0;
			if (!singleCaptionAnywhere && captionCount > 0) {
				const auto textHeight = st::messageTextStyle.font->height;
				uniformCaptionHeight = textHeight + padding.top() + padding.bottom();
			}

			for (auto const& [rowY, indices] : rows) {
				auto rowBottom = 0;

				// Find the bottom of this row's media
				for (const auto i : indices) {
					rowBottom = std::max(rowBottom, _parts[i].geometry.bottom());
				}
				lastRowBottom = rowBottom;

				// Check if this is the last row
				isLastRow = (rowY == rows.rbegin()->first);

				// Calculate caption heights for this row
				for (const auto i : indices) {
					auto &part = _parts[i];
					const auto originalText = part.item->originalText();
					if (!originalText.empty()) {
						if (singleCaptionAnywhere) {
							Ui::Text::String caption(
								st::messageTextStyle,
								originalText,
								Ui::ItemTextDefaultOptions());
							const auto captionWidth = newWidth - padding.left() - padding.right();
							part._captionHeight = caption.countHeight(captionWidth) + padding.top() + padding.bottom();
						} else {
							part._captionHeight = uniformCaptionHeight;
						}
					} else {
						part._captionHeight = 0;
					}
				}

				// Position captions for this row (multiple captions only)
				if (!singleCaptionAnywhere) {
					if (uniformCaptionHeight > 0) {
						for (const auto i : indices) {
							auto &part = _parts[i];
							if (part._captionHeight > 0) {
								part.captionRect = QRect(
									part.geometry.left(),
									rowBottom,
									part.geometry.width(),
									uniformCaptionHeight);
							} else {
								part.captionRect = QRect();
							}
						}

						// Shift logic:
						// If not last row, we want to consume the standard spacing.
						// The visual gap becomes just 'uniformCaptionHeight'.
						if (!isLastRow) {
							// We shift down by caption height, but shift UP by spacing (removing the gap)
							// Use max(0) to ensure we don't overlap if caption is thinner than spacing
							const auto shiftAmount = std::max(0, uniformCaptionHeight - spacing);
							
							cumulativeCaptionOffset += shiftAmount;
							
							for (auto &shiftPart : _parts) {
								if (shiftPart.initialGeometry.y() > rowY) {
									shiftPart.geometry.translate(0, shiftAmount);
								}
							}
							// Add the *added* visual height to the total
							newHeight += shiftAmount;
						} else {
							// Last row: just add the caption height (no spacing to subtract here)
							newHeight += uniformCaptionHeight;
						}
					}
				}
			}

			// Position single caption at the bottom of all rows
			if (singleCaptionAnywhere && captionCount > 0) {
				const auto captionIdx = captionIndices[0];
				auto &captionPart = _parts[captionIdx];
				if (captionPart._captionHeight > 0) {
					captionPart.captionRect = QRect(
						0, 
						lastRowBottom, 
						newWidth, 
						captionPart._captionHeight);
					newHeight += captionPart._captionHeight;
				}
			}
		}
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
		// For Grid mode with per-item captions, do not add extra bottom padding
		// since each item has its own caption area. The old logic added extra
		// padding at the bottom when any Grid item has a caption, which makes
		// the last row visually different from the others.
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

		if (_mode == Mode::Grid) { // Video duration/size bubble logic remains
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

		if (_mode == Mode::Column) { // All Column-mode drawing logic remains
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
				const auto st = context.st;
				const auto stm = context.messageStyle();
				p.setFont(st::msgDateFont);
				p.setPen(stm->msgDateFg);

				const auto itemRect = part.geometry.translated(0, groupPadding.top());
				const auto &docStyle = st::msgFileLayoutGrouped;
				const auto statustop = docStyle.statusTop - st::msgFileTopMinus;
				const auto baseY = itemRect.y() + statustop + st::msgDateFont->ascent;

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

				const int iconGap = 1;
				const int textGap = st::msgDateFont->width(' ');
				const int iconW = st::historyViewsWidth;
				const int viewsW = viewsText.isEmpty() ? 0 : (iconW + iconGap + st::msgDateFont->width(viewsText));
				const int editedW = editedNow ? st::msgDateFont->width(editText) : 0;
				const int timeIdW = st::msgDateFont->width(timeText + idText);
				int totalW = 0;
				if (viewsW > 0) totalW += viewsW + textGap;
				if (editedW > 0) totalW += editedW + textGap;
				totalW += timeIdW;

				int x = itemRect.x() + itemRect.width() - totalW - st::msgDateImgDelta;
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
				if (reservedLeft > 0) {
					const auto minGap = st::normalFont->spacew;
					const auto minX = reservedLeft + minGap;
					if (x < minX) x = minX;
				}
				if (viewsW > 0) {
					const auto &icon = stm->historyViewsIcon;
					const int iconH = icon.height();
					const int scaledH = (iconH * iconW) / std::max(1, icon.width());
					const int iconTop = baseY - st::msgDateFont->ascent + (st::msgDateFont->height - scaledH) / 2;
					icon.paint(p, x, iconTop, iconW);
					p.drawText(x + iconW + iconGap, baseY, viewsText);
					x += viewsW + textGap;
				}
				if (editedW > 0) {
					p.drawText(x, baseY, editText);
					x += editedW + textGap;
				}
				p.drawText(x, baseY, timeText + idText);
			} else if (!infoText.isEmpty()) {
				const auto st = context.st;
				const auto stm = context.messageStyle();
				p.setFont(st::msgDateFont);
				p.setPen(stm->msgDateFg);

				const auto itemRect = part.geometry.translated(0, groupPadding.top());
				const auto textWidth = st::msgDateFont->width(infoText);
				const auto &docStyle = st::msgFileLayoutGrouped;
				const auto topMinus = st::msgFileTopMinus;
				const auto statustop = docStyle.statusTop - topMinus;

				const auto textX = itemRect.x() + itemRect.width() - textWidth - st::msgDateImgDelta;
				const auto textY = itemRect.y() + statustop + st::msgDateFont->ascent;

				p.drawText(textX, textY, infoText);
			}
		} else if (_mode == Mode::Grid && i > 0) {
			drawMessageIdInfo(p, context, part.geometry.translated(0, groupPadding.top()), part.item);
		}

		// Draw Grid mode caption in its designated caption rect (in last row)
		if ((_mode == Mode::Grid) && part._captionHeight > 0 && !part.captionRect.isEmpty()) {
			const auto originalText = part.item->originalText();
			Ui::Text::String caption(st::messageTextStyle, originalText, Ui::ItemTextDefaultOptions());

			const auto partIndex = &part - &_parts[0];
			auto captionRect = part.captionRect.translated(0, groupPadding.top());

			p.setPen(stm->historyTextFg);
			p.setFont(st::messageTextStyle.font);
			const auto padding = QMargins(8, 2, 8, 2);

			std::vector<int> captionIndices;
			for (auto j = 0; j != _parts.size(); ++j) {
				if (!_parts[j].item->originalText().empty()) {
					captionIndices.push_back(j);
				}
			}
			const bool singleCaptionAnywhere = (captionIndices.size() == 1);

			if (singleCaptionAnywhere) {
				// Draw single caption in full
				const auto availableWidth = captionRect.width() - padding.left() - padding.right();
				
				// FIX: Force layout calculation so single-line wrapping works
				caption.countHeight(availableWidth);
				const auto textHeight = caption.height(); 

				const auto availableHeight = captionRect.height();
				const auto verticalPadding = 2;

				int verticalOffset;
				if (textHeight <= availableHeight - 2 * verticalPadding) {
					verticalOffset = std::max(verticalPadding, (availableHeight - textHeight) / 2);
				} else {
					verticalOffset = verticalPadding;
				}

				p.save();
				p.setClipRect(captionRect);
				caption.draw(p,
					captionRect.left() + padding.left(),
					captionRect.top() + verticalOffset,
					availableWidth,
					style::al_left,
					0, -1, part._captionSelection);
				p.restore();
			} else {
                // ... (existing code for multiple captions / elided text) ...
				// Multiple captions - draw with formatting, elided to single line
				const auto availableWidth = captionRect.width() - padding.left() - padding.right();
				const auto textHeight = st::messageTextStyle.font->height;
				const auto verticalOffset = std::max(2, (captionRect.height() - textHeight) / 2);

				p.save();
				p.setClipRect(captionRect);
				caption.drawElided(p,
					captionRect.left() + padding.left(),
					captionRect.top() + verticalOffset,
					availableWidth,
					1, 
					style::al_left,
					0, -1, 0, false, part._captionSelection);
				p.restore();
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

	if ((_mode == Mode::Grid) && !_parts.empty() && _parent->media() == this) {
		const auto firstPart = &_parts.front();
		const auto firstItemGeometry = firstPart->geometry.translated(0, groupPadding.top());

		if (_mode == Mode::Grid && (!_parent->hasBubble() || isBubbleBottom())) {
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

			int totalWidth = 0;
			const int textPadding = font->width(' ');

			const int dateWidth = font->width(dateText + msgIdText);
			totalWidth += dateWidth;

			if (edited) {
				const auto editedWidth = font->width(QString::fromUtf8("✏️") + " ");
				totalWidth += textPadding + editedWidth;
			}

			int viewsWidth = 0;
			const int viewsIconGap = 1;
			if (!viewsText.isEmpty()) {
				viewsWidth = st::historyViewsWidth + viewsIconGap + font->width(viewsText);
				totalWidth += (2 * textPadding) + viewsWidth;
			}

			const auto sti = context.imageStyle();
			const auto textHeight = font->height;
			const auto hPadding = 2;
			const auto vPadding = st::msgDateImgPadding.y();
			const auto bubbleW = totalWidth + 2 * hPadding;
			const auto bubbleH = textHeight + 2 * vPadding;

			const auto bubbleX = firstItemGeometry.x() + firstItemGeometry.width() - bubbleW - st::msgDateImgDelta;
			const auto bubbleY = firstItemGeometry.y() + st::msgDateImgDelta;

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
		const auto groupPadding = groupedPadding();
		const auto mediaGeo = part.geometry.translated(0, groupPadding.top());
		const auto captionGeo = part.captionRect.translated(0, groupPadding.top());

		const auto isInside = mediaGeo.contains(point)
			|| (!captionGeo.isEmpty() && captionGeo.contains(point));

		if (isInside) {
			if (_mode == Mode::Grid
				&& !captionGeo.isEmpty()
				&& captionGeo.contains(point)) {
				const auto originalText = part.item->originalText();
				if (!originalText.empty()) {
					auto result = TextState(part.item);
					const auto padding = QMargins(8, 0, 8, 0);
					const auto captionWidth = captionGeo.width() - padding.left() - padding.right();
					Ui::Text::String caption(st::messageTextStyle, originalText, Ui::ItemTextDefaultOptions());

					// For single full caption, we need to get state from the multi-line text object.
					// For others, a simple check is enough for the tooltip.
					std::vector<int> captionIndices;
					for (auto j = 0; j != _parts.size(); ++j) {
						if (!_parts[j].item->originalText().empty()) {
							captionIndices.push_back(j);
						}
					}
					// const bool fullCaptionOnFirst = (captionIndices.size() == 1 && captionIndices[0] == 0);
					const bool singleCaptionAnywhere = (captionIndices.size() == 1); // Any single caption

					if (singleCaptionAnywhere && i == captionIndices[0]) {
						// This caption is not elided, allow text selection.
						const auto clickX = point.x() - captionGeo.left() - padding.left();
						const auto clickY = point.y() - captionGeo.top() - padding.top();
						const auto textStateResult = caption.getState(QPoint(clickX, clickY), captionWidth, request.forText());
						result.cursor = CursorState::Text;
						result.link = textStateResult.link;
						result.symbol = textStateResult.symbol;
						result.afterSymbol = textStateResult.afterSymbol;
						result.itemId = part.item->fullId(); // Ensure item ID is set.

						// Store caption selection information
						result._captionText = originalText.text;
						result._captionItem = part.item;

						// Apply current caption selection to result for highlighting
						if (!part._captionSelection.empty()) {
							// This is a simplified approach - in reality you'd need proper
							// symbol-based selection tracking for multi-line text
							result.symbol = textStateResult.symbol;
							result.afterSymbol = textStateResult.afterSymbol;
						}
						// Note: Full selection handling is done at Element level
					} else {
						// This caption is elided, allow text selection of visible portion.
						const auto clickX = point.x() - captionGeo.left() - padding.left();
						const auto clickY = point.y() - captionGeo.top() - padding.top();
						const auto textStateResult = caption.getState(QPoint(clickX, clickY), captionWidth, request.forText());
						result.cursor = CursorState::Text;
						result.link = textStateResult.link;
						result.symbol = textStateResult.symbol;
						result.afterSymbol = textStateResult.afterSymbol;
						result.itemId = part.item->fullId();

						// Store caption text for context menu (full text, including hidden parts)
						result._captionText = originalText.text;
						result._captionItem = part.item;

						// Apply current caption selection to result for highlighting
						if (!part._captionSelection.empty()) {
							result.symbol = textStateResult.symbol;
							result.afterSymbol = textStateResult.afterSymbol;
						}
					}

					// Tooltip logic: only show if text is wider than its container.
					if (caption.maxWidth() > captionWidth) {
						result.customTooltip = true;
						result.customTooltipText = originalText.text;
					}
					return result;
				}
			}
			auto result = part.content->getStateGrouped(
				part.geometry, // Use original geometry for content state
				part.sides,
				point - QPoint(0, groupPadding.top()), // Adjust point for content
				request);
			result.symbol += shift;
			result.itemId = part.item->fullId();

			// All the tooltip logic for message ID, views, date, etc. remains unchanged.
			const auto item = part.item;
			const auto edited = item->Get<HistoryMessageEdited>();

			if (_mode == Mode::Column) {
				const auto &docStyle = st::msgFileLayoutGrouped;
				const auto topMinus = st::msgFileTopMinus;
				const auto statustop = docStyle.statusTop - topMinus;
				const auto itemRect = part.geometry;
				const auto y = itemRect.y() + statustop;
				const int currentRight = itemRect.x() + itemRect.width();

				if (i == 0) {
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
					const QRect editedRect = editedNow
						? QRect(editedOnlyRect.isNull() ? timeIdRect.left() : editedOnlyRect.left(),
							lineTop,
							(editedOnlyRect.isNull() ? 0 : editedOnlyRect.width()) + timeIdRect.width(),
							lineH)
						: QRect();

					if (viewsW > 0 && viewsRect.contains(point - QPoint(0, groupPadding.top()))) {
						result.customTooltip = true;
						result.customTooltipText = QString("Views: ") + viewsText;
					} else if (editedNow && editedRect.contains(point - QPoint(0, groupPadding.top()))) {
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
					} else if (timeIdRect.contains(point - QPoint(0, groupPadding.top()))) {
						const auto uploadLocal = QDateTime::fromSecsSinceEpoch(item->date()).toLocalTime();
						QString tooltipText = tr::lng_uploaded(tr::now) + ": "
							+ uploadLocal.date().toString("dddd, dd MMMM yyyy") + " "
							+ uploadLocal.time().toString("HH:mm:ss");
						result.customTooltip = true;
						result.customTooltipText = tooltipText;
					}
				} else {
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

						if (infoRect.contains(point - QPoint(0, groupPadding.top()))) {
							QString tooltipText;
							if (GetEnhancedBool("show_messages_id")) {
								const auto msgId = item->fullId().msg;
								if (msgId > 0) {
									tooltipText = QString("Message ID: ") + QString::number(msgId.bare);
								}
							}
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
			} else if (_mode == Mode::Grid && (&part != &_parts.front())) {
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

					if (infoRect.contains(point - QPoint(0, groupPadding.top()))) {
						QString tooltipText;
						if (GetEnhancedBool("show_messages_id")) {
							const auto msgId = item->fullId().msg;
							if (msgId > 0) {
								tooltipText = QString("Message ID: ") + QString::number(msgId.bare);
							}
						}
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
			return result;
		}
		shift += part.content->fullSelectionLength();
		++i;
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
	if (const auto tagged = lookupSpoilerTagMedia()) {
		if (QRect(0, 0, width(), height()).contains(point)) {
			if (auto link = tagged->spoilerTagLink()) {
				result.link = std::move(link);
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

void GroupedMedia::setCaptionSelection(int partIndex, TextSelection selection) {
	if (partIndex >= 0 && partIndex < _parts.size()) {
		// Clear previous selection
		if (_selectedCaptionIndex >= 0 && _selectedCaptionIndex < _parts.size()) {
			_parts[_selectedCaptionIndex]._captionSelection = {};
		}

		// Set new selection
		_selectedCaptionIndex = partIndex;
		_parts[partIndex]._captionSelection = selection;
	}
}

TextSelection GroupedMedia::adjustSelection(
		TextSelection selection,
		TextSelectType type) const {
	if (_mode == Mode::Column) {
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
	} else if (_mode == Mode::Grid) {
		// For Grid mode, handle caption selection
		// Find which part contains the selection and update its caption selection
		for (auto i = 0; i < _parts.size(); ++i) {
			const auto &part = _parts[i];
			if (!part.item->originalText().empty()) {
				// Check if selection overlaps with this caption
				if (!selection.empty()) {
					// For simplicity, if there's any selection, select the first caption
					// In a full implementation, you'd need to track which part has focus
					_selectedCaptionIndex = i;
					part._captionSelection = selection;
				} else {
					// Clear selection
					if (_selectedCaptionIndex == i) {
						part._captionSelection = {};
					}
				}
				break; // Handle first caption found
			}
		}
		return selection;
	}
	return {};
}

uint16 GroupedMedia::fullSelectionLength() const {
	if (_mode != Mode::Column) {
		// For Grid mode, return the caption text length for the current item
		// This enables text selection for Grid captions
		auto result = 0;
		for (const auto &part : _parts) {
			const auto originalText = part.item->originalText();
			if (!originalText.text.isEmpty()) {
				result += originalText.text.size();
			}
		}
		return result;
	}
	auto result = 0;
	for (const auto &part : _parts) {
		result += part.content->fullSelectionLength();
	}
	return result;
}

bool GroupedMedia::hasTextForCopy() const {
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
	if (_mode == Mode::Column) {
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
	} else if (_mode == Mode::Grid) {
		// For Grid mode, handle caption selection
		auto result = TextForMimeData();
		for (const auto &part : _parts) {
			const auto originalText = part.item->originalText();
			if (!originalText.empty() && !part._captionSelection.empty()) {
				// Add selected caption text
				const auto captionText = part._captionSelection.from == part._captionSelection.to
					? originalText.text.mid(part._captionSelection.from, part._captionSelection.to - part._captionSelection.from)
					: originalText.text; // Full selection
				if (result.empty()) {
					result = TextForMimeData::Simple(captionText);
				} else {
					result.append(u"\n\n"_q).append(TextForMimeData::Simple(captionText));
				}
			}
		}
		return result;
	}
	return {};
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

} // namespace HistoryView