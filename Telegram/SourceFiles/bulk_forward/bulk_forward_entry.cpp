/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk_forward/bulk_forward_entry.h"

#include "api/api_common.h"
#include "bulk_forward/bulk_forward_counters.h"
#include "bulk_forward/bulk_forward_filter_box.h"
#include "bulk_forward/bulk_forward_job.h"
#include "bulk_forward/bulk_forward_resume.h"
#include "bulk_forward/bulk_forward_settings.h"
#include "base/weak_qptr.h"
#include "chat_helpers/compose/compose_show.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "enhanced_forward.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/show.h"
#include "ui/toast/toast.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"

#include <crl/crl.h>

#include <algorithm>
#include <memory>
#include <utility>

namespace BulkForward {
namespace {

// One bulk forward at a time; the next one replaces the previous.
std::unique_ptr<Job> ActiveJob;

void AppendSummaryLine(QString *text, QString line) {
	if (line.isEmpty()) {
		return;
	} else if (!text->isEmpty()) {
		*text += u"\n"_q;
	}
	*text += line;
}

[[nodiscard]] QString BulkDoneText(const Job &job) {
	auto result = QString();
	if (const auto matched = job.matched(); matched > 0) {
		AppendSummaryLine(
			&result,
			tr::lng_bulk_forward_matched(tr::now, lt_count, matched));
	}
	const auto sent = job.sent();
	const auto skipped = job.skipped();
	auto delivered = (sent > 0)
		? tr::lng_bulk_forward_forwarded(tr::now, lt_count, sent)
		: QString();
	if (skipped > 0) {
		const auto duplicates = tr::lng_tm_fw_duplicates_skipped(
			tr::now,
			lt_count,
			skipped);
		delivered = delivered.isEmpty()
			? duplicates
			: (delivered + u", "_q + duplicates);
	}
	AppendSummaryLine(&result, std::move(delivered));
	const auto &failures = job.failures();
	if (!failures.empty()) {
		auto joined = QString();
		for (const auto &failure : failures) {
			if (!joined.isEmpty()) {
				joined += u"; "_q;
			}
			joined += failure;
		}
		AppendSummaryLine(
			&result,
			tr::lng_bulk_forward_failed_destinations(
				tr::now,
				lt_count, int(failures.size()),
				lt_error, joined));
	}
	return result;
}

void RunJob(not_null<Main::Session*> session, JobRequest request) {
	ActiveJob = std::make_unique<Job>(session);
	const auto weak = base::make_weak(ActiveJob.get());
	ActiveJob->done = [weak] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		if (strong->matched() <= 0 && strong->failures().empty()) {
			Ui::Toast::Show(tr::lng_bulk_forward_no_matches(tr::now));
			return;
		}
		const auto text = BulkDoneText(*strong);
		if (!text.isEmpty()) {
			Ui::Toast::Show(text);
		}
	};
	ActiveJob->failed = [=](QString error) {
		Ui::Toast::Show(tr::lng_bulk_forward_failed(
			tr::now,
			lt_error, error));
	};
	ActiveJob->start(std::move(request));
}

[[nodiscard]] std::vector<Destination> DestinationsFromThreads(
		const std::vector<not_null<Data::Thread*>> &threads) {
	auto result = std::vector<Destination>();
	for (const auto &thread : threads) {
		const auto peer = thread->peer();
		auto known = false;
		for (const auto &destination : result) {
			if (destination.peer == peer) {
				known = true;
				break;
			}
		}
		if (!known) {
			result.push_back({ peer, thread->topicRootId() });
		}
	}
	return result;
}

void StartJob(
		not_null<PeerData*> source,
		MsgId sourceTopicRootId,
		const Filter &filter,
		std::vector<Destination> destinations) {
	if (destinations.empty()) {
		return;
	}
	RunJob(&source->session(), JobRequest{
		.source = source,
		.sourceTopicRootId = sourceTopicRootId,
		.filter = filter,
		.destinations = std::move(destinations),
	});
}

void ResumeJob(not_null<Main::Session*> session, ResumeState state) {
	auto destinations = std::vector<Destination>();
	for (const auto &saved : state.destinations) {
		destinations.push_back({
			session->data().peer(saved.peer),
			saved.topicRootId,
		});
	}
	if (destinations.empty() || !state.source) {
		ClearResume(session);
		RemoveAllCounters(session);
		return;
	}
	RunJob(session, JobRequest{
		.source = session->data().peer(state.source),
		.sourceTopicRootId = state.sourceTopicRootId,
		.filter = state.filter,
		.destinations = std::move(destinations),
		.resume = std::move(state),
	});
}

[[nodiscard]] bool SourceForbidsForwarding(not_null<PeerData*> source) {
	if (EnhancedForward::PeerNeedsEnhancedForward(source)) {
		return true;
	}
	const auto migrated = source->migrateFrom();
	return migrated
		&& EnhancedForward::PeerNeedsEnhancedForward(
			not_null<PeerData*>(migrated));
}

void StartWithProtectedWarning(
		not_null<Window::SessionNavigation*> navigation,
		not_null<PeerData*> source,
		MsgId sourceTopicRootId,
		const Filter &filter,
		std::vector<Destination> destinations) {
	if (!SourceForbidsForwarding(source)) {
		StartJob(source, sourceTopicRootId, filter, std::move(destinations));
		return;
	}
	navigation->uiShow()->showBox(Ui::MakeConfirmBox({
		.text = tr::lng_bulk_forward_protected_about(tr::now),
		.confirmed = [=, destinations = std::move(destinations)](
				Fn<void()> &&close) mutable {
			close();
			StartJob(
				source,
				sourceTopicRootId,
				filter,
				std::move(destinations));
		},
		.confirmText = tr::lng_bulk_forward_protected_continue(),
		.title = tr::lng_bulk_forward_protected_title(),
	}));
}

void RecordCancelledRows(not_null<Main::Session*> session) {
	for (const auto &counters : AllCounters(session)) {
		const auto total = std::max(
			counters.sent,
			counters.matched - counters.skipped);
		if (counters.sent <= 0 || total <= 0) {
			continue;
		}
		EnhancedForward::RecordFinishedFwRun(
			session,
			counters.destination,
			counters.source,
			counters.firstSource,
			counters.lastSource,
			total,
			counters.sent,
			counters.skipped,
			0,
			true);
	}
}

} // namespace

void CancelActive(not_null<Main::Session*> session) {
	EnsureResumeSeeded(session);
	auto destinations = std::vector<PeerId>();
	for (const auto &counters : AllCounters(session)) {
		destinations.push_back(counters.destination);
	}
	RecordCancelledRows(session);
	if (ActiveJob && ActiveJob->session() == session) {
		ActiveJob = nullptr;
	}
	// The copy pipelines are not owned by the job, so cancelling the job
	// alone would leave them downloading and uploading. The bulk run owns
	// every destination it published a counter for.
	for (const auto &destination : destinations) {
		EnhancedForward::cancelForward(destination, session, true);
	}
	ClearResume(session);
	RemoveAllCounters(session);
}

void PauseActive(not_null<Main::Session*> session) {
	if (ActiveJob && ActiveJob->session() == session) {
		ActiveJob->pause();
	}
}

void ResumeActive(not_null<Main::Session*> session) {
	if (ActiveJob && ActiveJob->session() == session) {
		ActiveJob->resume();
		return;
	}
	// The startup resume may run before the progress surface has seeded the
	// restored run; seeding is idempotent and publishes the paused counters.
	EnsureResumeSeeded(session);
	if (auto state = TakeSeededResume(session)) {
		ResumeJob(session, std::move(*state));
	}
}

void ShowBox(
		not_null<Window::SessionNavigation*> navigation,
		not_null<PeerData*> peer,
		MsgId topicRootId,
		MessageIdsList selection) {
	auto initial = LoadLastFilter();
	const auto fromSelection = !selection.empty();
	initial.selection = std::move(selection);
	if (fromSelection) {
		initial.scope = Scope::Selection;
	}

	auto args = FilterBoxArgs();
	args.filter = std::move(initial);
	args.selectionAvailable = !args.filter.selection.empty();
	args.session = &peer->session();
	args.peer = peer;
	args.topicRootId = topicRootId;
	args.submit = [=](Filter filter) {
		SaveLastFilter(filter);

		auto chooser = std::make_shared<base::weak_qptr<Ui::BoxContent>>();
		const auto start = [=](
				std::vector<not_null<Data::Thread*>> threads) {
			auto destinations = DestinationsFromThreads(threads);
			crl::on_main([=, destinations = std::move(destinations)]() mutable {
				StartWithProtectedWarning(
					navigation,
					peer,
					topicRootId,
					filter,
					std::move(destinations));
			});
		};
		*chooser = ShowChooseRecipientsBox(
			navigation,
			[=](not_null<Data::Thread*> thread) {
				start({ thread });
				return true;
			},
			[=](
					std::vector<not_null<Data::Thread*>> threads,
					Api::SendOptions) {
				start(std::move(threads));
				if (const auto box = chooser->get()) {
					box->closeBox();
				}
			},
			tr::lng_bulk_forward_title());
	};
	navigation->uiShow()->showBox(FilterBox(std::move(args)));
}

} // namespace BulkForward
