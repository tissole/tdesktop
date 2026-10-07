/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "export/export_controller.h"
#include "export/export_settings.h"
#include "export/view/export_view_content.h"
#include "data/data_dedup_db.h"
#include "base/unique_qptr.h"
#include "base/timer.h"
#include "mtproto/sender.h"
#include <optional>

namespace Ui {
class SeparatePanel;
class BoxContent;
} // namespace Ui

namespace Main {
class Account;
class Session;
} // namespace Main

namespace Export {
namespace View {

base::weak_qptr<Ui::BoxContent> SuggestStart(not_null<Main::Session*> session);
void ClearSuggestStart(not_null<Main::Session*> session);
bool IsDefaultPath(not_null<Main::Session*> session, const QString &path);
void ResolveSettings(not_null<Main::Session*> session, Settings &settings);

class Panel;

class ProgressWidget;

class PanelController {
public:
	PanelController(
		not_null<Main::Session*> session,
		not_null<Controller*> process);
	~PanelController();

	[[nodiscard]] Main::Session &session() const {
		return *_session;
	}

	void activatePanel();
	void stopWithConfirmation(Fn<void()> callback = nullptr);
	[[nodiscard]] bool isExportRunning() const;
	void pauseRunningExport();
	void cancelRunningExport();

	[[nodiscard]] rpl::producer<> stopRequests() const;

	[[nodiscard]] rpl::lifetime &lifetime() {
		return _lifetime;
	}

	auto progressState() const {
		return ContentFromState(
			_settings.get(),
			rpl::single(_state) | rpl::then(_process->state()));
	}

private:
	void fillParams(const PasswordCheckState &state);
	void stopExport();
	void createPanel();
	void updateState(State &&state);
	void showSettings();
	void showProgress(bool scanning = false);
	void showError(const ApiErrorState &error);
	void showError(const OutputErrorState &error);
	void showError(const QString &text);
	void showCriticalError(const QString &text);

	void saveSettings() const;
	void ensureSharedTakeout(
		FnMut<void(uint64)> done,
		int64 fileMaxSize);
	void finishExportTakeout();
	void refreshResumeRow();
	void refreshGlobalRow();
	void requestChatList(
		Settings snapshot,
		FnMut<void(Data::DialogsInfo&&)> done);
	void applyRowSettings(
		Settings &settings,
		const ::Data::ExResumeRecord &row);
	void applyMarkerSettings(
		Settings &settings,
		const ::Data::ExResumeRecord &row);
	[[nodiscard]] bool updateCoversWholeChat(
		const ::Data::ExResumeRecord &row) const;
	void validateIdRange(FnMut<void()> proceed);
	// Stored folder is gone (moved drive, deleted): offer Locate (point at
	// the moved tree, paths rebased, run continues) or New folder
	// (recreate, run continues). Calls proceed with the folder to run into,
	// or an empty string for recreate; calls nothing if the user backs out.
	void resolveMissingFolder(
		QString missingPath,
		bool isGlobal,
		FnMut<void(QString &&folder)> proceed);
	// One queued single-chat run (own folder, own scope snapshot).
	struct SingleRun {
		Settings settings;
		::Data::ExResumeRecord row;
		bool update = false;
	};
	void startSingleResume(Settings settings, ::Data::ExResumeRecord row);
	void startSingleUpdate(Settings settings, ::Data::ExResumeRecord row);
	void startNextSingleRun();
	void showFolderPicker(
		std::vector<::Data::ExResumeRecord> rows,
		bool update,
		FnMut<void(std::vector<int> &&indices)> done);

	const not_null<Main::Session*> _session;
	const not_null<Main::Account*> _account;
	// Deferred takeout callbacks (queued in ApiWrap, fired on late
	// responses) must never touch a destroyed panel or session:
	// every one of them checks this flag first.
	const std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
	const not_null<Controller*> _process;
	std::unique_ptr<Settings> _settings;
	base::Timer _saveSettingsTimer;
	MTP::Sender _mtp;
	mtpRequestId _rangeRequestId = 0;

	base::unique_qptr<Ui::SeparatePanel> _panel;
	QPointer<ProgressWidget> _progress;

	State _state;
	std::vector<::Data::ExResumeRecord> _resumeRows;
	std::vector<SingleRun> _pendingRuns;
	// Scope as shown at panel open (neutralised when continue-state
	// exists): Resume/Update proceed only while live scope still equals it.
	Settings _scopeShown;
	std::optional<::Data::ExResumeRecord> _globalRow;
	bool _globalHasUnfinished = false;
	std::optional<Data::DialogsInfo> _chatListCache;
	bool _chatListLoading = false;
	std::vector<FnMut<void(Data::DialogsInfo&&)>> _chatListWaiters;
	int _startGen = 0;
	bool _paused = false;
	bool _pausePending = false;
	bool _running = false;
	bool _scanning = false;
	base::weak_qptr<Ui::BoxContent> _confirmStopBox;
	rpl::event_stream<rpl::producer<>> _panelCloseEvents;
	bool _stopRequested = false;
	rpl::lifetime _lifetime;

};

} // namespace View
} // namespace Export