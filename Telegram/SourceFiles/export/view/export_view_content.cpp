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
		not_null<Settings*> settings,
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
		const auto info = (state.entityCount > 0)
			? (QString::number(state.entityIndex + 1)
				+ " / "
				+ QString::number(state.entityCount))
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
		const auto addProgress = (state.entityCount == 1
			&& !state.entityIndex)
			? addPart(state.itemIndex, state.itemCount)
			: addPart(state.entityIndex, state.entityCount);
		push("main", label, info, doneProgress + addProgress);
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
	case Step::DialogsList:
		pushMain(tr::lng_export_state_chats_list(tr::now));
		break;
	case Step::PersonalInfo:
		pushMain(tr::lng_export_option_info(tr::now));
		break;
	case Step::Userpics:
		pushMain(tr::lng_export_state_userpics(tr::now));
		pushBytes(
			"userpic" + QString::number(state.entityIndex),
			state.bytesName,
			state.bytesRandomId);
		break;
	case Step::Contacts:
		pushMain(tr::lng_export_option_contacts(tr::now));
		break;
	case Step::Stories:
		pushMain(tr::lng_export_option_stories(tr::now));
		pushBytes(
			"story" + QString::number(state.entityIndex),
			state.bytesName,
			state.bytesRandomId);
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
				? (QString::number(state.itemIndex)
					+ " / "
					+ QString::number(state.itemCount))
				: QString()),
			(state.itemCount > 0
				? (state.itemIndex / float64(state.itemCount))
				: 0.));
		pushBytes(
			("file"
				+ QString::number(state.entityIndex)
				+ '_'
				+ QString::number(state.itemIndex)),
			state.bytesName,
			state.bytesRandomId);
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
				: QString()),
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
	const auto requiredRows = settings->onlySinglePeer() ? 1 : 2;
	while (result.rows.size() < requiredRows) {
		result.rows.emplace_back();
	}
	return result;
}

Content ContentFromState(const FinishedState &state) {
	const auto pollGroup = [&] {
		for (auto i = 0; i != Output::Stats::kGroups; ++i) {
			if (Output::Stats::kGroupStats[i].type
				== MediaSettings::Type::Poll) {
				return i;
			}
		}
		Unexpected("Poll group in stats.");
	}();
	const auto groupName = [](int index) {
		switch (index) {
		case 0: return tr::lng_export_option_photos(tr::now);
		case 1: return tr::lng_export_option_video_files(tr::now);
		case 2: return tr::lng_export_option_voice_messages(tr::now);
		case 3: return tr::lng_export_option_video_messages(tr::now);
		case 4: return tr::lng_export_option_stickers(tr::now);
		case 5: return tr::lng_export_option_gifs(tr::now);
		case 6: return tr::lng_export_option_files(tr::now);
		case 7: return tr::lng_export_option_text_messages(tr::now);
		case 8: return tr::lng_export_option_audios(tr::now);
		case 9: return tr::lng_export_option_full_history(tr::now);
		case 10: return tr::lng_export_option_links(tr::now);
		case 11: return tr::lng_export_option_polls(tr::now);
		}
		Unexpected("Group in ContentFromState.");
	};
	auto result = Content();
	result.rows.push_back({
		Content::kDoneId,
		tr::lng_export_finished(tr::now),
		QString(),
		1.,
		0,
		true });
	auto dataGroups = 0;
	for (const auto i : Output::Stats::kDisplayOrder) {
		if (state.groupFiles[i] || state.groupSkipped[i]) {
			++dataGroups;
		}
	}
	// Single-category runs skip totals: the total is the category itself.
	if (dataGroups > 1) {
		result.rows.push_back({
			Content::kDoneId,
			tr::lng_export_stats_total(
				tr::now,
				lt_amount,
				QString::number(state.filesCount),
				lt_size,
				Ui::FormatSizeText(state.bytesCount)),
			QString(),
			1. });
		if (state.skippedFiles > 0) {
			result.rows.push_back({
				Content::kDoneId,
				tr::lng_export_total_skipped(
					tr::now,
					lt_amount,
					QString::number(state.skippedFiles),
					lt_size,
					Ui::FormatSizeText(state.skippedBytes)),
				QString(),
				1. });
		}
	}
	for (const auto i : Output::Stats::kDisplayOrder) {
		const auto files = state.groupFiles[i];
		const auto skipped = state.groupSkipped[i];
		if (!files && !skipped) {
			continue;
		}
		result.rows.push_back({
			Content::kDoneId,
			tr::lng_export_selected_label(
				tr::now,
				lt_label,
				groupName(i),
				lt_amount,
				QString::number(files),
				lt_size,
				Ui::FormatSizeText(state.groupBytes[i])),
			(skipped > 0)
				? tr::lng_export_stats_skipped(
					tr::now,
					lt_count,
					int(skipped),
					lt_size,
					Ui::FormatSizeText(state.groupSkippedBytes[i]))
				: QString(),
			1. });
	}
	if (state.linkMessages > 0) {
		const auto amount = QString("%1 (%2)")
			.arg(state.linkTotal)
			.arg(state.linkTotal - state.linkDuplicates);
		result.rows.push_back({
			Content::kDoneId,
			tr::lng_export_stats_group(
				tr::now,
				lt_label,
				tr::lng_export_option_links(tr::now),
				lt_amount,
				amount),
			(state.linkDuplicates > 0)
				? (QString("Dups: ")
					+ QString::number(state.linkDuplicates))
				: QString(),
			1. });
	}
	if (state.groupFiles[pollGroup] > 0) {
		result.rows.push_back({
			Content::kDoneId,
			tr::lng_export_stats_group(
				tr::now,
				lt_label,
				groupName(pollGroup),
				lt_amount,
				QString::number(state.groupFiles[pollGroup])),
			QString(),
			1. });
	}
	if (state.textMessages > 0) {
		result.rows.push_back({
			Content::kDoneId,
			tr::lng_export_stats_group(
				tr::now,
				lt_label,
				tr::lng_export_option_text_messages(tr::now),
				lt_amount,
				QString::number(state.textMessages)),
			QString(),
			1. });
	}
	return result;
}

} // namespace View
} // namespace Export