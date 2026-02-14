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
	result.isScanning = state.isScanning;
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
		const auto isDialogs = (state.step == Step::Dialogs);
		const auto info = ((isScanning || isDialogs) && state.itemCount > 0)
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
		pushMain(tr::lng_export_scanning(tr::now));
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

	if (!state.selectedStats.empty()) {
		using MediaType = MediaSettings::Type;
		const std::vector<MediaType> order = {
			MediaType::Photo,
			MediaType::Video,
			MediaType::VideoMessage,
			MediaType::Audio,
			MediaType::VoiceMessage,
			MediaType::File,
			MediaType::Sticker,
			MediaType::GIF,
			MediaType::Text,
			MediaType::Link
		};

		for (const auto type : order) {
			const auto it = state.selectedStats.find(type);
			if (it == state.selectedStats.end() || it->second.totalCount <= 0) {
				continue;
			}
			const auto &item = it->second;
			QString label;
			switch (type) {
			case MediaType::Photo: label = tr::lng_export_option_photos(tr::now); break;
			case MediaType::Video: label = tr::lng_export_option_video_files(tr::now); break;
			case MediaType::VoiceMessage: label = tr::lng_export_option_voice_messages(tr::now); break;
			case MediaType::VideoMessage: label = tr::lng_export_option_video_messages(tr::now); break;
			case MediaType::Audio: label = tr::lng_export_option_audios(tr::now); break;
			case MediaType::Sticker: label = tr::lng_export_option_stickers(tr::now); break;
			case MediaType::GIF: label = tr::lng_export_option_gifs(tr::now); break;
			case MediaType::File: label = tr::lng_export_option_files(tr::now); break;
			case MediaType::Text: label = tr::lng_export_option_text_messages(tr::now); break;
			case MediaType::Link: label = tr::lng_export_option_links(tr::now); break;
			}
			if (!label.isEmpty()) {
				QString text;
				if (type == MediaType::Text) {
					text = label + ": " + Lang::FormatCountDecimal(item.totalCount);
				} else if (type == MediaType::Link) {
					text = "Unique " + label + "/Total " + label + ": " 
						+ Lang::FormatCountDecimal(item.uniqueCount) + "/" 
						+ Lang::FormatCountDecimal(item.totalCount);
				} else {
					text = "Unique " + label + "/Total " + label + ": " 
						+ Lang::FormatCountDecimal(item.uniqueCount) + " (" + Ui::FormatSizeText(item.uniqueSize) + ")/"
						+ Lang::FormatCountDecimal(item.totalCount) + " (" + Ui::FormatSizeText(item.totalSize) + ")";
				}
				result.rows.push_back({ Content::kDoneId, text, QString(), 1. });
			}
		}
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
	const std::vector<Type> order = {
		Type::Photo,
		Type::Video,
		Type::VideoMessage,
		Type::Audio,
		Type::VoiceMessage,
		Type::File,
		Type::Sticker,
		Type::GIF,
		Type::Text,
		Type::Link
	};

	const auto fullHistory = state.fullHistory;
	const auto fullRange = state.fullRange;
	const auto showAllCategories = fullHistory && fullRange;

	int categoriesCount = 0;
	int totalUniqueMessagesCount = 0;
	int totalTotalMessagesCount = 0;
	int64 totalUniqueMediaSize = 0;
	int64 totalMediaSize = 0;
	const bool linksOnly = (state.breakdown.size() == 1 && state.breakdown.begin()->first == Type::Link);
	(void)linksOnly;

	for (const auto type : order) {
		const auto it = state.breakdown.find(type);
		if (!showAllCategories && (it == state.breakdown.end() || it->second.totalCount <= 0)) {
			continue;
		}
		const auto &item = (it != state.breakdown.end()) ? it->second : Output::StatItem();
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
		case Type::Text: label = tr::lng_export_option_text_messages(tr::now); break;
		case Type::Link: label = tr::lng_export_option_links(tr::now); break;
		}
		QString text;
		
		if (type == Type::Text) {
			text = label + ": " + Lang::FormatCountDecimal(item.totalCount);
		} else if (type == Type::Link) {
			text = "Unique " + label + "/Total " + label + ": " 
				+ Lang::FormatCountDecimal(item.uniqueCount) + "/" 
				+ Lang::FormatCountDecimal(item.totalCount);
		} else {
			text = "Unique " + label + "/Total " + label + ": " 
				+ Lang::FormatCountDecimal(item.uniqueCount) + " (" + Ui::FormatSizeText(item.uniqueSize) + ")/"
				+ Lang::FormatCountDecimal(item.totalCount) + " (" + Ui::FormatSizeText(item.totalSize) + ")";

			totalUniqueMediaSize += item.uniqueSize;
			totalMediaSize += item.totalSize;
		}

		if (type != Type::Link) {
			totalUniqueMessagesCount += item.uniqueCount;
			totalTotalMessagesCount += item.totalCount;
		}

		categoriesCount++;
		result.rows.push_back({ "stat_" + QString::number((int)type), text, QString(), 1. });
	}

	if (categoriesCount > 1 && totalTotalMessagesCount > 0) {
		const auto label = "Total unique/Total messages: ";
		const auto uniqueStr = Lang::FormatCountDecimal(totalUniqueMessagesCount)
			+ " (" + Ui::FormatSizeText(totalUniqueMediaSize) + ")";
		const auto totalStr = Lang::FormatCountDecimal(totalTotalMessagesCount)
			+ " (" + Ui::FormatSizeText(totalMediaSize) + ")";

		QString totalText = label + uniqueStr + "/" + totalStr;
		result.rows.push_back({ "stat_summary", totalText, QString(), 1. });
	}

	return result;
}

} // namespace View
} // namespace Export
