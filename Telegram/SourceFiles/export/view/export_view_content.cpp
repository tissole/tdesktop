/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/view/export_view_content.h"

#include "export/export_settings.h"
#include "lang/lang_keys.h"
#include "ui/text/format_values.h"

namespace Export {
namespace View {

const QString Content::kDoneId = "done";

Content ContentFromState(
		const ProcessingState &state) {
	using Step = ProcessingState::Step;

	auto result = Content();
	const auto push = [&](
			const QString &id,
			const QString &label,
			const QString &info,
			float64 progress,
			uint64 randomId = 0) {
		result.rows.push_back({ id, label, info, progress, randomId });
	};
	const auto pushMain = [&](const QString &label) {
		const auto isScanning = (state.step == Step::Scanning);
		const auto info = (isScanning && state.itemCount > 0)
			? (Lang::FormatCountDecimal(state.itemIndex)
				+ " / "
				+ Lang::FormatCountDecimal(state.itemCount))
			: (state.entityCount > 0)
			? (Lang::FormatCountDecimal(state.entityIndex + 1)
				+ " / "
				+ Lang::FormatCountDecimal(state.entityCount))
			: QString();
		if (!state.substepsTotal) {
			push("main", label, info, 0.);
			return;
		}
		const auto substepsTotal = state.substepsTotal;
		const auto done = state.substepsPassed;
		const auto add = state.substepsNow;
		const auto doneProgress = done / float64(substepsTotal);
		const auto addPart = [&](int index, int count) {
			return (count > 0)
				? ((float64(add) * index)
					/ (float64(substepsTotal) * count))
				: 0.;
		};
		const auto addProgress = isScanning
			? addPart(state.itemIndex, state.itemCount)
			: (state.entityCount == 1 && !state.entityIndex)
			? addPart(state.itemIndex, state.itemCount)
			: addPart(state.entityIndex, state.entityCount);
		push("main", label, info, doneProgress + addProgress);
	};

	switch (state.step) {
	case Step::Initializing:
		pushMain(tr::lng_export_state_initializing(tr::now));
		break;
	case Step::Scanning:
		pushMain(tr::lng_export_analyzing(tr::now));
		break;
	case Step::DialogsList:
		pushMain(tr::lng_export_state_chats_list(tr::now));
		break;
	case Step::PersonalInfo:
		pushMain(tr::lng_export_option_info(tr::now));
		break;
	case Step::Userpics:
		pushMain(tr::lng_export_state_userpics(tr::now));
		break;
	case Step::Contacts:
		pushMain(tr::lng_export_option_contacts(tr::now));
		break;
	case Step::Stories:
		pushMain(tr::lng_export_option_stories(tr::now));
		break;
	case Step::Sessions:
		pushMain(tr::lng_export_option_sessions(tr::now));
		break;
	case Step::OtherData:
		pushMain(tr::lng_export_option_other(tr::now));
		break;
	case Step::Dialogs:
		if (state.entityCount > 1) {
			pushMain(tr::lng_export_state_chats(tr::now));
		}
		push(
			"chat" + QString::number(state.entityIndex),
			(state.entityName.isEmpty()
				? tr::lng_deleted(tr::now)
				: (state.entityType == ProcessingState::EntityType::Chat)
				? state.entityName
				: (state.entityType == ProcessingState::EntityType::SavedMessages)
				? tr::lng_saved_messages(tr::now)
				: tr::lng_replies_messages(tr::now)),
			(state.itemCount > 0
				? (Lang::FormatCountDecimal(state.itemIndex)
					+ " / "
					+ Lang::FormatCountDecimal(state.itemCount))
				: QString()),
			(state.itemCount > 0
				? (state.itemIndex / float64(state.itemCount))
				: 0.));
		break;
	default: Unexpected("Step in ContentFromState.");
	}

	for (const auto &[id, download] : state.activeDownloads) {
		const auto progress = (download.total > 0)
			? (download.ready / float64(download.total))
			: 0.;
		const auto info = Ui::FormatDownloadText(
			download.ready,
			download.total);
		const auto lastSlash = download.path.lastIndexOf('/');
		const auto name = download.path.mid(lastSlash + 1);
		push("file_" + QString::number(id), name, info, progress, id);
	}

	if (result.rows.empty()) {
		result.rows.emplace_back();
	}

	return result;
}

Content ContentFromState(const FinishedState &state) {
	auto result = Content();
	result.rows.push_back({
		Content::kDoneId,
		tr::lng_export_finished(tr::now),
		QString(),
		1. });

	using Type = MediaSettings::Type;
	int categoriesCount = 0;
	for (const auto &[type, item] : state.breakdown) {
		if (item.totalCount <= 0) continue;
		QString label;
		switch (type) {
		case Type::Photo: label = tr::lng_export_option_photos(tr::now); break;
		case Type::Video: label = tr::lng_export_option_video_files(tr::now); break;
		case Type::VoiceMessage: label = tr::lng_export_option_voice_messages(tr::now); break;
		case Type::VideoMessage: label = tr::lng_export_option_video_messages(tr::now); break;
		case Type::Audio: label = tr::lng_export_option_audios(tr::now); break;
		case Type::Sticker: label = tr::lng_export_option_stickers(tr::now); break;
		case Type::GIF: label = tr::lng_export_option_gifs(tr::now); break;
		case Type::File: label = tr::lng_export_option_files(tr::now); break;
		case Type::Link: label = tr::lng_export_option_links(tr::now); break;
		}
		if (!label.isEmpty()) {
			categoriesCount++;
			if (type == Type::Text || type == Type::Link) {
				result.rows.push_back({
					Content::kDoneId,
					tr::lng_export_downloaded_count_only(
						tr::now,
						lt_label,
						label,
						lt_amount,
						Lang::FormatCountDecimal(item.uniqueCount),
						lt_total_amount,
						Lang::FormatCountDecimal(item.totalCount)),
					QString(),
					1. });
			} else {
				result.rows.push_back({
					Content::kDoneId,
					tr::lng_export_downloaded_count(
						tr::now,
						lt_label,
						label,
						lt_amount,
						Lang::FormatCountDecimal(item.uniqueCount),
						lt_size,
						Ui::FormatSizeText(item.uniqueSize),
						lt_total_amount,
						Lang::FormatCountDecimal(item.totalCount),
						lt_total_size,
						Ui::FormatSizeText(item.totalSize)),
					QString(),
					1. });
			}
		}
	}

	result.rows.push_back({
		Content::kDoneId,
		tr::lng_export_total_text_exported(
			tr::now,
			lt_amount,
			Lang::FormatCountDecimal(state.messagesTextCount)),
		QString(),
		1. });
	result.rows.push_back({
		Content::kDoneId,
		tr::lng_export_total_media_exported(
			tr::now,
			lt_amount,
			Lang::FormatCountDecimal(state.messagesMediaCount)),
		QString(),
		1. });
	result.rows.push_back({
		Content::kDoneId,
		tr::lng_export_total_messages_exported(
			tr::now,
			lt_amount,
			Lang::FormatCountDecimal(state.messagesTotalCount)),
		QString(),
		1. });

	if (categoriesCount > 1) {
		result.rows.push_back({
			Content::kDoneId,
			tr::lng_export_total_selected(
				tr::now,
				lt_amount,
				Lang::FormatCountDecimal(state.totalUniqueCount),
				lt_size,
				Ui::FormatSizeText(state.totalUniqueSize),
				lt_total_amount,
				Lang::FormatCountDecimal(state.totalTotalCount),
				lt_total_size,
				Ui::FormatSizeText(state.totalTotalSize)),
			QString(),
			1. });
	}

	result.rows.push_back({
		Content::kDoneId,
		tr::lng_export_total_size(
			tr::now,
			lt_size,
			Ui::FormatSizeText(state.bytesCount)),
		QString(),
		1. });
	return result;
}

} // namespace View
} // namespace Export
