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
		const auto info = (state.itemCount > 0)
			? (Lang::FormatCountDecimal(state.itemIndex)
				+ " / "
				+ Lang::FormatCountDecimal(state.itemCount))
			: (state.entityCount > 1)
			? (Lang::FormatCountDecimal(state.entityIndex)
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
			? (state.itemCount > 0 ? (state.itemIndex / float64(state.itemCount)) : 0.)
			: (state.entityCount == 1 && !state.entityIndex)
			? addPart(state.itemIndex, state.itemCount)
			: addPart(state.entityIndex, state.entityCount);
		push("main", label, info, isScanning ? addProgress : (doneProgress + addProgress));
	};
	const auto pushBytes = [&](
			const QString &id,
			const QString &label,
			uint64 randomId) {
		if (!state.bytesCount) {
			return;
		}
		const auto progress = state.bytesLoaded / float64(state.bytesCount);
		const auto info = Ui::FormatDownloadText(
			state.bytesLoaded,
			state.bytesCount);
		push(id, label, info, progress, randomId);
	};

	switch (state.step) {
	case Step::Initializing:
		pushMain(tr::lng_export_state_initializing(tr::now));
		break;
	case Step::Scanning:
		pushMain(QString());
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
	case Step::ProfileMusic:
		pushMain(tr::lng_export_option_profile_music(tr::now));
		pushBytes(
			"music" + QString::number(state.entityIndex),
			state.bytesName,
			state.bytesRandomId);
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
				: Lang::FormatCountDecimal(state.itemIndex)),
			(state.itemCount > 0
				? (state.itemIndex / float64(state.itemCount))
				: 0.));
		break;
	case Step::Topic:
		pushMain(tr::lng_export_state_chats(tr::now));
		push(
			"topic",
			state.entityName.isEmpty()
				? tr::lng_deleted(tr::now)
				: state.entityName,
			(state.itemCount > 0
				? (QString::number(state.itemIndex)
					+ " / "
					+ QString::number(state.itemCount))
				: QString::number(state.itemIndex)),
			(state.itemCount > 0
				? (state.itemIndex / float64(state.itemCount))
				: 0.));
		pushBytes(
			"file_topic_" + QString::number(state.itemIndex),
			state.bytesName,
			state.bytesRandomId);
		break;
	default: Unexpected("Step in ContentFromState.");
	}

	if (!state.selectedStats.empty() || !state.expectedStats.empty()) {
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

		int categoriesCount = 0;
		int totalUniqueMessagesCount = 0;
		int totalTotalMessagesCount = 0;
		int64 totalUniqueMediaSize = 0;
		int64 totalMediaSize = 0;

		struct RowData {
			QString id;
			QString label;
			QString info;
			float64 progress;
		};
		std::vector<RowData> typeRows;

		for (const auto type : order) {
			const auto it = state.selectedStats.find(type);
			const auto expIt = state.expectedStats.find(type);
			const bool hasSelected = (it != state.selectedStats.end() && it->second.totalCount > 0);
			const bool hasExpected = (expIt != state.expectedStats.end() && expIt->second.totalCount > 0);
			if (!hasSelected && !hasExpected) {
				continue;
			}
			static const Output::StatItem kEmpty{};
			const auto &selectedItem = hasSelected ? it->second : kEmpty;
			const auto &expectedItem = hasExpected ? expIt->second : kEmpty;

			Output::StatItem displayItem;
			displayItem.uniqueCount = selectedItem.uniqueCount;
			displayItem.uniqueSize = selectedItem.uniqueSize;
			displayItem.totalCount = hasExpected ? expectedItem.totalCount : selectedItem.totalCount;
			displayItem.totalSize = (hasExpected && expectedItem.totalSize > 0) ? expectedItem.totalSize : selectedItem.totalSize;
			displayItem.messagesWithLinks = hasExpected ? expectedItem.messagesWithLinks : selectedItem.messagesWithLinks;

			const auto &item = displayItem;
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
			if (label.isEmpty()) continue;

			categoriesCount++;

	QString rowLabel, rowInfo;
	float64 typeProgress = 0.;
	if (type == MediaType::Text) {
		rowLabel = label + ": " + Lang::FormatCountDecimal(item.totalCount);
		typeProgress = (state.itemCount > 0)
			? std::clamp(state.itemIndex / float64(state.itemCount), 0., 1.)
			: 0.;
	} else if (type == MediaType::Link) {
		const auto sharedStr = item.messagesWithLinks > 0
			? " (" + Lang::FormatCountDecimal(item.messagesWithLinks) + " Messages)"
			: QString();
		rowLabel = label + ": " + Lang::FormatCountDecimal(item.uniqueCount)
			+ ", " + Lang::FormatCountDecimal(item.totalCount) + sharedStr;
		typeProgress = (state.itemCount > 0)
			? std::clamp(state.itemIndex / float64(state.itemCount), 0., 1.)
			: 0.;
	} else {
		rowLabel = label + ": "
			+ Lang::FormatCountDecimal(item.uniqueCount)
			+ " (" + Ui::FormatSizeText(item.uniqueSize) + "), "
			+ Lang::FormatCountDecimal(item.totalCount)
			+ " (" + Ui::FormatSizeText(item.totalSize) + ")";
		
		rowInfo = QString();

		totalUniqueMediaSize += item.uniqueSize;
		totalMediaSize += item.totalSize;
		totalUniqueMessagesCount += item.uniqueCount;
		totalTotalMessagesCount += item.totalCount;

		const int expected = hasExpected ? expectedItem.totalCount : 0;
		const int locallyProcessed = item.uniqueCount;
		if (hasExpected && expected > 0 && locallyProcessed >= 0 && !state.fullHistory) {
			typeProgress = std::clamp(locallyProcessed / float64(expected), 0., 1.);
		} else if (hasExpected && expected > 0 && item.totalCount > 0) {
			typeProgress = std::clamp(item.totalCount / float64(expected), 0., 1.);
		} else if (state.itemCount > 0) {
			typeProgress = std::clamp(state.itemIndex / float64(state.itemCount), 0., 1.);
		} else {
			typeProgress = 0.;
		}
	}
	typeRows.push_back({ "stat_" + QString::number((int)type), rowLabel, rowInfo, typeProgress });
	}

	const auto progress = (state.itemCount > 0)
		? std::clamp(state.itemIndex / float64(state.itemCount), 0., 1.)
		: 0.;
	if (categoriesCount > 1 && totalTotalMessagesCount > 0) {
		result.rows.push_back({ QString(), QString(), QString(), 0. });

	const auto label = "Total Media: ";
	QString totalLabel;
	totalLabel = label + Lang::FormatCountDecimal(totalUniqueMessagesCount)
		+ " (" + Ui::FormatSizeText(totalUniqueMediaSize) + "), "
		+ Lang::FormatCountDecimal(totalTotalMessagesCount)
		+ " (" + Ui::FormatSizeText(totalMediaSize) + ")";

	result.rows.push_back({ "stat_summary", totalLabel, QString(), progress });
}
for (auto &row : typeRows) {
	result.rows.push_back({ row.id, row.label, row.info, row.progress });
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
		"header_finished",
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

	const auto showAllCategories = false;

	int categoriesCount = 0;
	int totalUniqueMessagesCount = 0;
	int totalTotalMessagesCount = 0;
	int64 totalUniqueMediaSize = 0;
	int64 totalMediaSize = 0;

	struct RowData {
		QString id;
		QString label;
		QString info;
		float64 progress;
	};
	std::vector<RowData> typeRows;

	for (const auto type : order) {
		const auto it = state.breakdown.find(type);
		if (it == state.breakdown.end() || it->second.totalCount <= 0) {
			if (!showAllCategories) {
				continue;
			}
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
		typeRows.push_back({ "stat_" + QString::number((int)type), text, QString(), 1. });
	} else if (type == Type::Link) {
		const auto sharedStr = item.messagesWithLinks > 0
			? " (" + Lang::FormatCountDecimal(item.messagesWithLinks) + " Messages)"
			: QString();
		text = label + ": " + Lang::FormatCountDecimal(item.uniqueCount)
			+ ", " + Lang::FormatCountDecimal(item.totalCount) + sharedStr;
		typeRows.push_back({ "stat_" + QString::number((int)type), text, QString(), 1. });
	} else {
		text = label + ": " + Lang::FormatCountDecimal(item.uniqueCount)
			+ " (" + Ui::FormatSizeText(item.uniqueSize) + "), "
			+ Lang::FormatCountDecimal(item.totalCount)
			+ " (" + Ui::FormatSizeText(item.totalSize) + ")";

		totalUniqueMediaSize += item.uniqueSize;
		totalMediaSize += item.totalSize;
		totalUniqueMessagesCount += item.uniqueCount;
		totalTotalMessagesCount += item.totalCount;

		categoriesCount++;
		typeRows.push_back({ "stat_" + QString::number((int)type), text, QString(), 1. });
	}
	}

	int mediaCategoriesCount = categoriesCount;
	if (mediaCategoriesCount > 1 && totalTotalMessagesCount > 0) {
		result.rows.push_back({ QString(), QString(), QString(), 0. });

	const auto label = "Total Media: ";
	QString totalLabel;
	totalLabel = label + Lang::FormatCountDecimal(totalUniqueMessagesCount)
		+ " (" + Ui::FormatSizeText(totalUniqueMediaSize) + "), "
		+ Lang::FormatCountDecimal(totalTotalMessagesCount)
		+ " (" + Ui::FormatSizeText(totalMediaSize) + ")";

	result.rows.push_back({ "stat_summary", totalLabel, QString(), 1. });
	}
	for (auto &row : typeRows) {
		result.rows.push_back({ row.id, row.label, row.info, row.progress });
	}
	
	result.rows.push_back({ Content::kDoneId });

	return result;
}

Content ContentFromState(const ScanDoneState &state) {
	// Return a non-scanning, non-done row so the top bar clears "Scanning..."
	// without triggering showDone() in the ProgressWidget.
	auto result = Content();
	result.isScanning = false;
	result.rows.push_back({ "scan_complete", QString(), QString(), 1.0 });
	return result;
}

} // namespace View
} // namespace Export
