/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk_forward/bulk_forward_filter_box.h"

#include "base/unixtime.h"
#include "bulk_forward/bulk_forward_count.h"
#include "lang/lang_keys.h"
#include "rpl/variable.h"
#include "ui/boxes/choose_date_time.h"
#include "ui/text/format_values.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace BulkForward {
namespace {

constexpr auto kMegabyte = qint64(1024 * 1024);
constexpr auto kSecondsInDay = 86400;

[[nodiscard]] QString FormatDate(TimeId date) {
	return date
		? Ui::FormatDateTime(base::unixtime::parse(date))
		: tr::lng_bulk_forward_any(tr::now);
}

[[nodiscard]] QStringList ParseExtensions(const QString &text) {
	auto result = QStringList();
	for (const auto &value : text.split(
			QRegularExpression(u"[\\s,;]+"_q),
			Qt::SkipEmptyParts)) {
		auto normalized = NormalizeExtension(value);
		if (!normalized.isEmpty() && !result.contains(normalized)) {
			result.push_back(std::move(normalized));
		}
	}
	return result;
}

[[nodiscard]] qint64 ParseMegabytes(const QString &text) {
	return std::max(qint64(0), qint64(text.trimmed().toLongLong()))
		* kMegabyte;
}

[[nodiscard]] MsgId ParseMsgId(const QString &text) {
	return MsgId(std::max(qint64(0), text.trimmed().toLongLong()));
}

[[nodiscard]] QString FormatMegabytes(qint64 bytes) {
	return (bytes > 0)
		? QString::number(bytes / kMegabyte)
		: QString();
}

} // namespace

object_ptr<Ui::GenericBox> FilterBox(FilterBoxArgs &&args) {
	auto filter = std::move(args.filter);
	auto submit = std::move(args.submit);
	const auto session = args.session;
	const auto peer = args.peer;
	const auto topicRootId = args.topicRootId;
	const auto selectionCount = int(filter.selection.size());
	const auto selectionAvailable = args.selectionAvailable
		&& (selectionCount > 0);
	if (!selectionAvailable && filter.scope == Scope::Selection) {
		filter.scope = Scope::All;
	}

	return Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_bulk_forward_title());
		box->setWidth(st::boxWideWidth);

		const auto content = box->verticalLayout();
		const auto probe = box->lifetime().make_state<CountProbe>(
			session,
			peer,
			topicRootId);
		const auto countLabel = content->add(
			object_ptr<Ui::FlatLabel>(content, QString(), st::boxLabel),
			st::boxRowPadding);
		countLabel->setVisible(false);
		probe->text() | rpl::on_next([=](const QString &text) {
			countLabel->setText(text);
			countLabel->setVisible(!text.isEmpty());
		}, countLabel->lifetime());
		const auto scopeGroup = std::make_shared<Ui::RadioenumGroup<Scope>>(
			filter.scope);

		Ui::AddSubsectionTitle(content, tr::lng_bulk_forward_scope());

		const auto addScope = [&](Scope scope, const QString &label) {
			content->add(
				object_ptr<Ui::Radioenum<Scope>>(
					content,
					scopeGroup,
					scope,
					label),
				st::boxRowPadding);
		};
		addScope(Scope::All, tr::lng_bulk_forward_scope_all(tr::now));
		addScope(Scope::DateRange, tr::lng_bulk_forward_scope_dates(tr::now));
		addScope(Scope::IdRange, tr::lng_bulk_forward_scope_ids(tr::now));
		if (selectionAvailable) {
			addScope(
				Scope::Selection,
				tr::lng_bulk_forward_scope_selection(
					tr::now,
					lt_count,
					selectionCount));
		}

		const auto dateWrap = content->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				content,
				object_ptr<Ui::VerticalLayout>(content)));

		const auto dates = std::make_shared<std::pair<TimeId, TimeId>>(
			filter.fromDate,
			filter.tillDate);
		const auto fromText = box->lifetime().make_state<
			rpl::variable<QString>>(FormatDate(dates->first));
		const auto tillText = box->lifetime().make_state<
			rpl::variable<QString>>(FormatDate(dates->second));

		const auto addDateRow = [&](
				not_null<Ui::VerticalLayout*> container,
				rpl::variable<QString> *text,
				TimeId *value,
				rpl::variable<QString> *otherText,
				TimeId *otherValue,
				bool isFrom,
				rpl::producer<QString> title) {
			const auto button = container->add(
				object_ptr<Ui::SettingsButton>(
					container,
					text->value(),
					st::settingsButtonNoIcon));
			button->setClickedCallback([=] {
				box->uiShow()->showBox(Box([=](
						not_null<Ui::GenericBox*> dateBox) {
					const auto now = base::unixtime::now();
					Ui::ChooseDateTimeBox(dateBox, {
						.title = title,
						.submit = tr::lng_settings_save(),
						.done = [=](TimeId result) {
							if (!result) {
								return;
							}
							*value = result;
							*text = FormatDate(result);
							if (*otherValue
								&& (isFrom
									? (result > *otherValue)
									: (result < *otherValue))) {
								*otherValue = result;
								*otherText = FormatDate(result);
							}
							dateBox->closeBox();
						},
						.min = [] {
							return TimeId(0);
						},
						.time = *value ? *value : now,
						.max = [] {
							return base::unixtime::now() + kSecondsInDay;
						},
					});
				}));
			});
		};
		addDateRow(
			dateWrap->entity(),
			fromText,
			&dates->first,
			tillText,
			&dates->second,
			true,
			tr::lng_bulk_forward_from_date());
		addDateRow(
			dateWrap->entity(),
			tillText,
			&dates->second,
			fromText,
			&dates->first,
			false,
			tr::lng_bulk_forward_till_date());

		const auto idWrap = content->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				content,
				object_ptr<Ui::VerticalLayout>(content)));
		const auto idInner = idWrap->entity();

		const auto addField = [&](
				not_null<Ui::VerticalLayout*> container,
				rpl::producer<QString> placeholder,
				const QString &value) {
			return container->add(
				object_ptr<Ui::InputField>(
					container,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					std::move(placeholder),
					value),
				st::boxRowPadding);
		};

		const auto fromIdField = addField(
			idInner,
			tr::lng_bulk_forward_any(),
			filter.fromId ? QString::number(filter.fromId.bare) : QString());
		const auto tillIdField = addField(
			idInner,
			tr::lng_bulk_forward_any(),
			filter.tillId ? QString::number(filter.tillId.bare) : QString());

		Ui::AddSubsectionTitle(content, tr::lng_bulk_forward_types());

		auto typeBoxes = std::vector<
			std::pair<not_null<Ui::Checkbox*>, MediaType>>();
		const auto addType = [&](MediaType type, const QString &label) {
			const auto checkbox = content->add(
				object_ptr<Ui::Checkbox>(
					content,
					label,
					(filter.types & type).value() != 0),
				style::margins(
					st::boxRowPadding.left(),
					st::boxRowPadding.top() + st::settingsCheckboxesSkip,
					st::boxRowPadding.right(),
					st::boxRowPadding.bottom()));
			typeBoxes.emplace_back(checkbox, type);
		};
		addType(MediaType::Photo, tr::lng_export_option_photos(tr::now));
		addType(MediaType::Video, tr::lng_export_option_video_files(tr::now));
		addType(MediaType::Voice, tr::lng_export_option_voice_messages(tr::now));
		addType(
			MediaType::RoundVideo,
			tr::lng_export_option_video_messages(tr::now));
		addType(MediaType::Sticker, tr::lng_export_option_stickers(tr::now));
		addType(MediaType::Gif, tr::lng_export_option_gifs(tr::now));
		addType(MediaType::File, tr::lng_export_option_files(tr::now));
		addType(MediaType::Audio, tr::lng_export_option_audios(tr::now));
		addType(MediaType::Text, tr::lng_export_option_text_messages(tr::now));
		addType(MediaType::Link, tr::lng_export_option_links(tr::now));
		addType(MediaType::Poll, tr::lng_export_option_polls(tr::now));
		addType(MediaType::Other, tr::lng_bulk_forward_other(tr::now));

		Ui::AddSubsectionTitle(content, tr::lng_bulk_forward_extensions());

		const auto extensionGroup = std::make_shared<
			Ui::RadioenumGroup<ExtensionMode>>(filter.extensionMode);
		const auto addExtensionMode = [&](
				ExtensionMode mode,
				const QString &label) {
			content->add(
				object_ptr<Ui::Radioenum<ExtensionMode>>(
					content,
					extensionGroup,
					mode,
					label),
				st::boxRowPadding);
		};
		addExtensionMode(
			ExtensionMode::None,
			tr::lng_bulk_forward_extensions_any(tr::now));
		addExtensionMode(
			ExtensionMode::Whitelist,
			tr::lng_bulk_forward_extensions_only(tr::now));
		addExtensionMode(
			ExtensionMode::Blacklist,
			tr::lng_bulk_forward_extensions_except(tr::now));

		const auto extensionField = addField(
			content,
			rpl::single(QString()),
			filter.extensions.join(u", "_q));

		Ui::AddSubsectionTitle(content, tr::lng_bulk_forward_size());

		const auto minSizeField = addField(
			content,
			tr::lng_bulk_forward_size_min(),
			FormatMegabytes(filter.minSize));
		const auto maxSizeField = addField(
			content,
			tr::lng_bulk_forward_size_max(),
			FormatMegabytes(filter.maxSize));

		const auto state = std::make_shared<Filter>(filter);
		const auto collect = [=]() -> Filter& {
			state->scope = scopeGroup->current();
			state->fromDate = dates->first;
			state->tillDate = dates->second;
			auto fromId = ParseMsgId(fromIdField->getLastText());
			auto tillId = ParseMsgId(tillIdField->getLastText());
			if (fromId && tillId && fromId > tillId) {
				std::swap(fromId, tillId);
			}
			state->fromId = fromId;
			state->tillId = tillId;
			state->types = MediaTypes();
			for (const auto &[checkbox, type] : typeBoxes) {
				if (checkbox->checked()) {
					state->types |= type;
				}
			}
			state->extensionMode = extensionGroup->current();
			state->extensions = ParseExtensions(
				extensionField->getLastText());
			auto minSize = ParseMegabytes(minSizeField->getLastText());
			auto maxSize = ParseMegabytes(maxSizeField->getLastText());
			if (minSize > 0 && maxSize > 0 && minSize > maxSize) {
				std::swap(minSize, maxSize);
			}
			state->minSize = minSize;
			state->maxSize = maxSize;
			return *state;
		};

		const auto apply = box->addButton(tr::lng_settings_apply(), [=] {
			auto result = collect();
			if (!IsValid(result)) {
				return;
			}
			submit(std::move(result));
			box->closeBox();
		});
		const auto update = [=] {
			auto current = collect();
			apply->setEnabled(IsValid(current));
			probe->request(current);
		};

		scopeGroup->changes() | rpl::on_next([=](Scope scope) {
			dateWrap->toggle(
				scope == Scope::DateRange,
				anim::type::instant);
			idWrap->toggle(scope == Scope::IdRange, anim::type::instant);
			update();
		}, box->lifetime());

		extensionGroup->changes() | rpl::on_next([=](ExtensionMode) {
			update();
		}, box->lifetime());

		for (const auto &[checkbox, type] : typeBoxes) {
			checkbox->checkedChanges() | rpl::on_next([=](bool) {
				update();
			}, checkbox->lifetime());
		}

		for (const auto field : {
				fromIdField,
				tillIdField,
				extensionField,
				minSizeField,
				maxSizeField }) {
			field->changes() | rpl::on_next([=] {
				update();
			}, field->lifetime());
		}

		dateWrap->toggle(
			filter.scope == Scope::DateRange,
			anim::type::instant);
		idWrap->toggle(filter.scope == Scope::IdRange, anim::type::instant);

		fromText->value() | rpl::on_next([=](const QString &) {
			update();
		}, box->lifetime());
		tillText->value() | rpl::on_next([=](const QString &) {
			update();
		}, box->lifetime());
		update();
	});
}

} // namespace BulkForward
