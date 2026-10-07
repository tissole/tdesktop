/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/export_controller.h"

#include "export/export_api_wrap.h"
#include "export/export_settings.h"
#include "export/data/export_data_types.h"
#include "export/output/export_output_abstract.h"
#include "export/output/export_output_result.h"
#include "export/output/export_output_stats.h"
#include "data/data_dedup_db.h"
#include <crl/crl_on_main.h>
#include "mtproto/mtp_instance.h"
#include "ui/text/format_values.h"

#include <QtCore/QFile>
#include <QtCore/QDir>
#include <optional>
#include <QtCore/QTextStream>
#include <QtCore/QDateTime>

namespace Export {
namespace {

const auto kNullStateCallback = [](ProcessingState&) {};

// History-without-media output is buffered and written in whole chunks so
// one disk write covers many message slices instead of one per slice.
const auto kDialogWriteBatch = 100;

// Group order matches Output::Stats group indices.

Settings NormalizeSettings(const Settings &settings) {
	if (!settings.onlySinglePeer()) {
		return base::duplicate(settings);
	}
	auto result = base::duplicate(settings);
	result.types = result.fullChats = Settings::Type::AnyChatsMask;
	result.chatSelectionActive = false;
	result.selectedChats.clear();
	return result;
}

} // namespace

// A global run is incremental: its root persists and is found, not
// made. The root is per account - EX_Global_<id>_<name> - matched by the
// bare account id only; the name part is cosmetic, so a renamed account
// still finds its tree (renaming the stale name part when found).
QString ResolveGlobalRoot(
		const QString &location,
		uint64 accountId,
		const QString &accountName,
		QString *oldRoot) {
	const auto stem = u"EX_Global_"_q
		+ QString::number(accountId);
	const auto wanted = accountName.isEmpty()
		? stem
		: (stem + '_' + accountName);
	const auto parent = QDir(location);
	const auto existing = parent.entryList(
		{ stem, stem + u"_*"_q },
		QDir::Dirs | QDir::NoDotAndDotDot);
	const auto finish = [&](const QString &name) {
		const auto root = parent.absoluteFilePath(name) + '/';
		QDir().mkpath(root);
		return root;
	};
	if (existing.contains(wanted)) {
		return finish(wanted);
	}
	if (!existing.isEmpty()) {
		const auto from = parent.absoluteFilePath(existing.front());
		if (QDir().rename(from, parent.absoluteFilePath(wanted))) {
			if (oldRoot) {
				*oldRoot = from + '/';
			}
			return finish(wanted);
		}
		return finish(existing.front());
	}
	return finish(wanted);
}

namespace {

QString GlobalRootPath(
		const QString &location,
		const Environment &environment) {
	return ResolveGlobalRoot(
		location,
		environment.accountId,
		environment.accountName);
}

} // namespace

QString ChatBlockHeader(const Data::DialogInfo &info) {
	const auto display = info.lastName.isEmpty()
		? QString::fromUtf8(info.name)
		: (QString::fromUtf8(info.name)
			+ ' '
			+ QString::fromUtf8(info.lastName));
	auto folder = info.relativePath;
	if (folder.endsWith('/')) {
		folder.chop(1);
	}
	return u"Chat: "_q
		+ QString::number(Data::PeerToBareId(info.peerId))
		+ u". "_q
		+ display
		+ u" ("_q
		+ folder
		+ u')';
}

void WriteStatsGroups(QTextStream &stream, const FinishedState &state) {
	auto dataGroups = 0;
	for (const auto i : Output::Stats::kDisplayOrder) {
		if (state.groupFiles[i] || state.groupSkipped[i]) {
			++dataGroups;
		}
	}
	// Single-category runs skip totals: the total is the category itself.
	if (dataGroups > 1) {
		stream << "Total unique files: " << state.filesCount << " ("
			<< Ui::FormatSizeText(state.bytesCount) << "), Total duplicates: "
			<< state.skippedFiles << " ("
			<< Ui::FormatSizeText(state.skippedBytes) << ")\n\n";
	}
	for (const auto i : Output::Stats::kDisplayOrder) {
		const auto &group = Output::Stats::kGroupStats[i];
		const auto files = state.groupFiles[i];
		const auto skipped = state.groupSkipped[i];
		if (!files && !skipped) {
			continue;
		}
		stream << group.name << ": " << files
			<< " ("
			<< Ui::FormatSizeText(state.groupBytes[i]) << ')';
		if (skipped > 0) {
			stream << ", Dups: " << skipped << ", ("
				<< Ui::FormatSizeText(state.groupSkippedBytes[i])
				<< ')';
		}
		stream << '\n';
	}
	if (state.linkMessages) {
		stream << "Links: " << state.linkTotal << " ("
			<< (state.linkTotal - state.linkDuplicates) << ')';
		if (const auto dup = state.linkDuplicates) {
			stream << ", Duplicates: " << dup;
		}
		stream << '\n';
	}
	for (auto i = 0; i != Output::Stats::kGroups; ++i) {
		if (Output::Stats::kGroupStats[i].type
			!= MediaSettings::Type::Poll) {
			continue;
		}
		if (const auto polls = state.groupFiles[i]) {
			stream << Output::Stats::kGroupStats[i].name << ": "
				<< polls << '\n';
		}
	}
	if (state.textMessages) {
		stream << "Text messages: " << state.textMessages << '\n';
	}
}

[[nodiscard]] FinishedState StatsDifference(
	const Output::Stats &current,
	const Output::Stats &base);

class ControllerObject {
public:
	ControllerObject(
		crl::weak_on_queue<ControllerObject> weak,
		QPointer<MTP::Instance> mtproto,
		const MTPInputPeer &peer);
	ControllerObject(
		crl::weak_on_queue<ControllerObject> weak,
		QPointer<MTP::Instance> mtproto,
		const MTPInputPeer &peer,
		int32 topicRootId,
		uint64 peerId,
		const QString &topicTitle);

	rpl::producer<State> state() const;

	// Password step.
	//void submitPassword(const QString &password);
	//void requestPasswordRecover();
	//rpl::producer<PasswordUpdate> passwordUpdate() const;
	//void reloadPasswordState();
	//void cancelUnconfirmedPassword();

	// Processing step.
	void startExport(
		const Settings &settings,
		const Environment &environment,
		const QString &singlePeerFolder = QString());
	void startResumeExport(
		const Settings &settings,
		const Environment &environment,
		const ::Data::ExResumeRecord &record);
	void requestChatList(
		Settings settings,
		FnMut<void(Data::DialogsInfo&&)> done);
	void setCachedDialogs(Data::DialogsInfo info);
	void refreshOnlyMyMessages();
	void startResumeExportGlobal(
		const Settings &settings,
		const Environment &environment,
		const ::Data::ExResumeRecord &marker,
		std::vector<::Data::ExResumeRecord> rows);
	void startUpdateExportGlobal(
		const Settings &settings,
		const Environment &environment,
		const ::Data::ExResumeRecord &marker,
		std::vector<::Data::ExResumeRecord> rows);
	void startUpdateExport(
		const Settings &settings,
		const Environment &environment,
		const ::Data::ExResumeRecord &record,
		const QString &newFolderName = QString());
	void setUpdateConfirmHandler(
		Fn<void(int anyNew, int selectedNew, FnMut<void(bool)> proceed)> handler);
	void closeDialogFiles();
	void onUpdateChecked(int anyNew, int selectedNew);
	void startScan(
		const Settings &settings,
		const Environment &environment,
		const QString &singlePeerFolder = QString());
	void skipFile(uint64 randomId);
	void cancelExportFast();
	void setSessionId(uint64 sessionId);
	void setDedupDb(const QString &path);
	void setSharedTakeoutId(uint64 id);
	void setTakeoutRefreshHook(Fn<void()> hook);
	void takeoutRefreshDone(uint64 id);
	void requestPause();
	void resumeExport();
	rpl::producer<bool> pauseChanges() const;
	rpl::producer<bool> canPauseChanges() const;

private:
	using Step = ProcessingState::Step;
	using DownloadProgress = ApiWrap::DownloadProgress;

	[[nodiscard]] bool stopped() const;
	void setState(State &&state);
	void ioError(const QString &path);
	bool ioCatchError(Output::Result result);
	void setFinishedState();
	FinishedState statsShown(const Output::Stats &base) const;
	void recordChatStats();
	void applyGlobalResumeScope();
	Output::Result writeStatsFile() const;
	Output::Result writeFailedJson() const;
	Output::Result writeLinksFile() const;
	void failCurrentDialog(
		PeerId peerId,
		const QString &name,
		const QString &error);

	//void requestPasswordState();
	//void passwordStateDone(const MTPaccount_Password &password);

	void fillExportSteps();
	void fillSubstepsInSteps(const ApiWrap::StartInfo &info);
	void exportNext();
	void initialize();
	void initialized(const ApiWrap::StartInfo &info);
	void collectDialogsList();
	void exportPersonalInfo();
	void exportUserpics();
	void exportStories();
	void exportProfileMusic();
	void exportContacts();
	void exportSessions();
	void exportOtherData();
	void exportDialogs();
	void exportNextDialog();
	void exportTopic();
	bool batchDialogSlices() const;
	bool flushPendingSlice();

	template <typename Callback = const decltype(kNullStateCallback) &>
	ProcessingState prepareState(
		Step step,
		Callback &&callback = kNullStateCallback) const;
	ProcessingState stateInitializing() const;
	ProcessingState stateDialogsList(int processed) const;
	ProcessingState statePersonalInfo() const;
	ProcessingState stateUserpics(const DownloadProgress &progress) const;
	ProcessingState stateStories(const DownloadProgress &progress) const;
	ProcessingState stateProfileMusic(const DownloadProgress &progress) const;
	ProcessingState stateContacts() const;
	ProcessingState stateSessions() const;
	ProcessingState stateOtherData() const;
	ProcessingState stateDialogs(const DownloadProgress &progress) const;
	ProcessingState stateTopic(const DownloadProgress &progress) const;
	DownloadProgress selectedProgress() const;
	void fillMessagesState(
		ProcessingState &result,
		const Data::DialogsInfo &info,
		int index,
		const DownloadProgress &progress) const;

	int substepsInStep(Step step) const;

	ApiWrap _api;
	Settings _settings;
	Environment _environment;
	std::optional<::Data::ExResumeRecord> _resumeRecord;
	bool _dialogOpen = false;
	bool _dialogOpened = false;
	bool _freshFolder = false;
	bool _updateCounting = false;
	Fn<void(int anyNew, int selectedNew, FnMut<void(bool)> proceed)> _updateConfirm;

	Data::DialogsInfo _dialogsInfo;
	int _dialogIndex = -1;
	std::optional<Data::DialogsInfo> _cachedDialogs;
	Data::FailedChats _failedChats;
	uint64 _sessionId = 0;
	bool _globalResume = false;
	bool _globalUpdate = false;
	std::optional<::Data::ExResumeRecord> _globalMarker;
	std::vector<::Data::ExResumeRecord> _resumeRows;
	PeerId _resumePeerId = PeerId();
	int _resumedFailedCount = 0;
	[[nodiscard]] int totalFailedCount() const {
		return _resumedFailedCount + int(_failedChats.size());
	}
	[[nodiscard]] const ::Data::ExResumeRecord *findResumeRow(
		PeerId peerId) const;
	[[nodiscard]] const Data::DialogInfo *findFreshDialog(
		PeerId peerId) const;
	[[nodiscard]] bool chatIncludedInRun(
		const Data::DialogInfo &info) const;
	void filterRunDialogs();
	struct ChatStatBlock {
		QString header;
		FinishedState state;
	};
	std::vector<ChatStatBlock> _chatStats;
	QByteArray _statsChatBase;
	QString _chatBlockHeader;

	int _messagesWritten = 0;
	int _messagesCount = 0;
	bool _selectedCounting = false;
	Data::MessagesSlice _pendingSlice;

	int _userpicsWritten = 0;
	int _userpicsCount = 0;

	int _storiesWritten = 0;
	int _storiesCount = 0;

	int _profileMusicWritten = 0;
	int _profileMusicCount = 0;

	// rpl::variable<State> fails to compile in MSVC :(
	State _state;
	rpl::event_stream<State> _stateChanges;

	Output::Stats _stats;
	// Update only: the counters this run started from, so the window can
	// show this session's files. stats.txt keeps the total.
	QByteArray _statsSessionBase;
	bool _updateSession = false;

	std::vector<int> _substepsInStep;
	int _substepsTotal = 0;
	mutable int _substepsPassed = 0;
	mutable Step _lastProcessingStep = Step::Initializing;

	std::unique_ptr<Output::AbstractWriter> _writer;
	std::vector<Step> _steps;
	int _stepIndex = -1;
	bool _scanMode = false;
	// "id_name" from the chat itself, never the on-disk folder.
	QString _singlePeerFolder;

	int32 _topicRootId = 0;
	uint64 _topicPeerId = 0;
	QString _topicTitle;

	rpl::lifetime _lifetime;

};

ControllerObject::ControllerObject(
	crl::weak_on_queue<ControllerObject> weak,
	QPointer<MTP::Instance> mtproto,
	const MTPInputPeer &peer)
: _api(mtproto, weak.runner())
, _state(PasswordCheckState{}) {
	_api.errors(
	) | rpl::on_next([=](const MTP::Error &error) {
		setState(ApiErrorState{ error });
	}, _lifetime);

	_api.ioErrors(
	) | rpl::on_next([=](const Output::Result &result) {
		ioCatchError(result);
	}, _lifetime);

	//requestPasswordState();
	auto state = PasswordCheckState();
	state.checked = false;
	state.requesting = false;
	state.singlePeer = peer;
	setState(std::move(state));
}

ControllerObject::ControllerObject(
	crl::weak_on_queue<ControllerObject> weak,
	QPointer<MTP::Instance> mtproto,
	const MTPInputPeer &peer,
	int32 topicRootId,
	uint64 peerId,
	const QString &topicTitle)
: _api(mtproto, weak.runner())
, _state(PasswordCheckState{})
, _topicRootId(topicRootId)
, _topicPeerId(peerId)
, _topicTitle(topicTitle) {
	_api.errors(
	) | rpl::on_next([=](const MTP::Error &error) {
		setState(ApiErrorState{ error });
	}, _lifetime);

	_api.ioErrors(
	) | rpl::on_next([=](const Output::Result &result) {
		ioCatchError(result);
	}, _lifetime);

	//requestPasswordState();
	auto state = PasswordCheckState();
	state.checked = false;
	state.requesting = false;
	state.singlePeer = peer;
	setState(std::move(state));
}

rpl::producer<State> ControllerObject::state() const {
	return rpl::single(
		_state
	) | rpl::then(
		_stateChanges.events()
	) | rpl::filter([](const State &state) {
		const auto password = std::get_if<PasswordCheckState>(&state);
		return !password || !password->requesting;
	});
}

bool ControllerObject::stopped() const {
	return v::is<CancelledState>(_state)
		|| v::is<ApiErrorState>(_state)
		|| v::is<OutputErrorState>(_state)
		|| v::is<FinishedState>(_state);
}

void ControllerObject::setState(State &&state) {
	if (stopped()) {
		return;
	}
	_state = std::move(state);
	_stateChanges.fire_copy(_state);
}

void ControllerObject::ioError(const QString &path) {
	setState(OutputErrorState{ path });
}

bool ControllerObject::ioCatchError(Output::Result result) {
	if (!result) {
		ioError(result.path);
		return true;
	}
	return false;
}

//void ControllerObject::submitPassword(const QString &password) {
//
//}
//
//void ControllerObject::requestPasswordRecover() {
//
//}
//
//rpl::producer<PasswordUpdate> ControllerObject::passwordUpdate() const {
//	return nullptr;
//}
//
//void ControllerObject::reloadPasswordState() {
//	//_mtp.request(base::take(_passwordRequestId)).cancel();
//	requestPasswordState();
//}
//
//void ControllerObject::requestPasswordState() {
//	if (_passwordRequestId) {
//		return;
//	}
//	//_passwordRequestId = _mtp.request(MTPaccount_GetPassword(
//	//)).done([=](const MTPaccount_Password &result) {
//	//	_passwordRequestId = 0;
//	//	passwordStateDone(result);
//	//}).fail([=](const MTP::Error &error) {
//	//	apiError(error);
//	//}).send();
//}
//
//void ControllerObject::passwordStateDone(const MTPaccount_Password &result) {
//	auto state = PasswordCheckState();
//	state.checked = false;
//	state.requesting = false;
//	state.hasPassword;
//	state.hint;
//	state.unconfirmedPattern;
//	setState(std::move(state));
//}
//
//void ControllerObject::cancelUnconfirmedPassword() {
//
//}

void ControllerObject::startExport(
		const Settings &settings,
		const Environment &environment,
		const QString &singlePeerFolder) {
	if (!_settings.path.isEmpty()) {
		return;
	}
	_updateCounting = false;
	_settings = NormalizeSettings(settings);
	_environment = environment;
	_settings.singleTopicRootId = _topicRootId;
	_settings.singleTopicPeerId = _topicPeerId;

	_settings.path = _settings.onlySinglePeer()
		? Output::NormalizePath(_settings, singlePeerFolder)
		: GlobalRootPath(_settings.path, _environment);
	_singlePeerFolder = singlePeerFolder;
	_freshFolder = false;
	_scanMode = false;
	_updateSession = false;
	_failedChats.clear();
	_chatStats.clear();
	_globalResume = false;
	_globalUpdate = false;
	_globalMarker = std::nullopt;
	_resumeRows.clear();
	_resumePeerId = PeerId();
	_resumedFailedCount = 0;
	_statsSessionBase.clear();
	_api.setScanMode(false);
	_writer = Output::CreateWriter(_settings.format);
	fillExportSteps();
	exportNext();
}

void ControllerObject::startResumeExportGlobal(
		const Settings &settings,
		const Environment &environment,
		const ::Data::ExResumeRecord &marker,
		std::vector<::Data::ExResumeRecord> rows) {
	if (!_settings.path.isEmpty()) {
		return;
	}
	_updateCounting = false;
	_settings = NormalizeSettings(settings);
	_environment = environment;
	_settings.singleTopicRootId = _topicRootId;
	_settings.singleTopicPeerId = _topicPeerId;
	if (!_settings.path.endsWith('/')) {
		_settings.path += '/';
	}
	_singlePeerFolder = QString();
	_freshFolder = false;
	_resumeRecord = std::nullopt;
	_scanMode = false;
	_updateSession = false;
	_statsSessionBase.clear();
	_globalResume = true;
	_globalUpdate = false;
	_globalMarker = marker;
	_resumeRows = std::move(rows);
	_resumedFailedCount = marker.failedCount;
	_resumePeerId = PeerId();
	_failedChats.clear();
	_chatStats.clear();
	_api.setScanMode(false);
	_writer = Output::CreateWriter(_settings.format);
	fillExportSteps();
	exportNext();
}

void ControllerObject::startUpdateExportGlobal(
		const Settings &settings,
		const Environment &environment,
		const ::Data::ExResumeRecord &marker,
		std::vector<::Data::ExResumeRecord> rows) {
	if (!_settings.path.isEmpty()) {
		return;
	}
	_updateCounting = false;
	_settings = NormalizeSettings(settings);
	_environment = environment;
	_settings.singleTopicRootId = _topicRootId;
	_settings.singleTopicPeerId = _topicPeerId;
	if (!_settings.path.endsWith('/')) {
		_settings.path += '/';
	}
	_singlePeerFolder = QString();
	_freshFolder = false;
	_resumeRecord = std::nullopt;
	_scanMode = false;
	_updateSession = true;
	_statsSessionBase.clear();
	_globalResume = false;
	_globalUpdate = true;
	_globalMarker = marker;
	_resumeRows = std::move(rows);
	_resumedFailedCount = 0;
	_resumePeerId = PeerId();
	_failedChats.clear();
	_chatStats.clear();
	_api.setScanMode(false);
	_writer = Output::CreateWriter(_settings.format);
	fillExportSteps();
	exportNext();
}

void ControllerObject::onUpdateChecked(int anyNew, int selectedNew) {
	// The walk is parked: all count responses are in, nothing runs.
	// Hop to main for UI, proceed/abort back here is safe.
	crl::on_main([=, handler = _updateConfirm]() mutable {
		_updateCounting = false;
		if (handler) {
			handler(anyNew, selectedNew, [=](bool proceed) mutable {
				if (proceed) {
					_api.proceedUpdate();
					return;
				}
				_api.abortUpdate();
				cancelExportFast();
			});
			return;
		}
		if (selectedNew > 0) {
			_api.proceedUpdate();
		} else {
			_api.abortUpdate();
			cancelExportFast();
		}
	});
}

void ControllerObject::startResumeExport(
		const Settings &settings,
		const Environment &environment,
		const ::Data::ExResumeRecord &record) {
	if (!_settings.path.isEmpty()) {
		return;
	}
	// Caller passes the stored folder in settings.path: same-chat
	// resume/update never renumbers, it reuses the exact path.
	_updateCounting = false;
	_settings = NormalizeSettings(settings);
	_environment = environment;
	_settings.singleTopicRootId = _topicRootId;
	_settings.singleTopicPeerId = _topicPeerId;
	if (!_settings.path.endsWith('/')) {
		_settings.path += '/';
	}
	_singlePeerFolder = QString();
	_freshFolder = false;
	_resumeRecord = record;
	_scanMode = false;
	_updateSession = false;
	_statsSessionBase.clear();
	_globalResume = false;
	_globalUpdate = false;
	_api.setScanMode(false);
	_writer = Output::CreateWriter(_settings.format);
	fillExportSteps();
	exportNext();
}

void ControllerObject::startUpdateExport(
		const Settings &settings,
		const Environment &environment,
		const ::Data::ExResumeRecord &record,
		const QString &newFolderName) {
	if (v::is<CancelledState>(_state)) {
		return;
	}
	closeDialogFiles();
	_api.setUpdateCheck(0, nullptr);
	_settings = Settings();
	_environment = Environment();
	_state = ProcessingState();
	_steps.clear();
	_stepIndex = -1;
	_substepsInStep.clear();
	_substepsTotal = 0;
	_substepsPassed = 0;
	_lastProcessingStep = Step::Initializing;
	_dialogIndex = -1;
	_dialogOpen = false;
	_dialogOpened = false;
	_messagesWritten = 0;
	_messagesCount = 0;
	_selectedCounting = false;
	_pendingSlice = Data::MessagesSlice();
	_userpicsWritten = 0;
	_userpicsCount = 0;
	_storiesWritten = 0;
	_storiesCount = 0;
	_profileMusicWritten = 0;
	_profileMusicCount = 0;
	_stats.reset();
	_settings = NormalizeSettings(settings);
	_environment = environment;
	_settings.singleTopicRootId = _topicRootId;
	_settings.singleTopicPeerId = _topicPeerId;
	if (!newFolderName.isEmpty()) {
		_singlePeerFolder = newFolderName;
		_settings.path = Output::NormalizePath(
			_settings,
			newFolderName);
	} else {
		_singlePeerFolder = QString();
	}
	_freshFolder = !newFolderName.isEmpty();
	_resumeRecord = record;
	if (!_settings.path.endsWith('/')) {
		_settings.path += '/';
	}
	_scanMode = false;
	_updateSession = true;
	_statsSessionBase.clear();
	_globalResume = false;
	_globalUpdate = false;
	_api.setScanMode(false);
	_updateCounting = true;
	_writer = Output::CreateWriter(_settings.format);
	_api.setUpdateCheck(
		record.updateAnchor ? record.updateAnchor : record.lastId,
		[=](int anyNew, int selectedNew) {
			onUpdateChecked(anyNew, selectedNew);
		});
	fillExportSteps();
	exportNext();
}

void ControllerObject::startScan(
		const Settings &settings,
		const Environment &environment,
		const QString &singlePeerFolder) {
	if (!_settings.path.isEmpty()) {
		return;
	}
	_updateCounting = false;
	_settings = NormalizeSettings(settings);
	_environment = environment;
	_settings.singleTopicRootId = _topicRootId;
	_settings.singleTopicPeerId = _topicPeerId;

	_settings.path = _settings.onlySinglePeer()
		? Output::NormalizePath(_settings, singlePeerFolder)
		: GlobalRootPath(_settings.path, _environment);
	_singlePeerFolder = singlePeerFolder;
	_scanMode = true;
	_updateSession = false;
	_failedChats.clear();
	_chatStats.clear();
	_globalResume = false;
	_globalUpdate = false;
	_globalMarker = std::nullopt;
	_resumeRows.clear();
	_resumePeerId = PeerId();
	_resumedFailedCount = 0;
	_statsSessionBase.clear();
	_api.setScanMode(true);
	_writer = nullptr;
	QDir().mkpath(_settings.path);
	fillExportSteps();
	exportNext();
}

void ControllerObject::skipFile(uint64 randomId) {
	if (stopped()) {
		return;
	}
	_api.skipFile(randomId);
}

void ControllerObject::requestPause() {
	if (stopped()) {
		return;
	}
	_api.requestPause();
}

void ControllerObject::closeDialogFiles() {
	if (!_dialogOpen || !_writer) {
		return;
	}
	_dialogOpen = false;
	_dialogOpened = false;
	// Closes the open messages file; folder gets deleted right after.
	ioCatchError(_writer->writeDialogEnd());
}

void ControllerObject::setUpdateConfirmHandler(
		Fn<void(int anyNew, int selectedNew, FnMut<void(bool)> proceed)> handler) {
	_updateConfirm = std::move(handler);
}

void ControllerObject::resumeExport() {
	if (stopped()) {
		return;
	}
	// A boundary-parked run has no live walk: restart the loop at the
	// parked chat. An in-chat pause resumes inside ApiWrap instead.
	const auto restart = !_settings.onlySinglePeer()
		&& !_scanMode
		&& _api.pausedAtChatBoundary();
	_api.resumeExport();
	if (restart) {
		exportNextDialog();
	}
}

rpl::producer<bool> ControllerObject::pauseChanges() const {
	return _api.pauseChanges();
}

rpl::producer<bool> ControllerObject::canPauseChanges() const {
	return _api.canPauseChanges();
}

void ControllerObject::fillExportSteps() {
	using Type = Settings::Type;
	_steps.push_back(Step::Initializing);
	if (_settings.onlySingleTopic()) {
		_steps.push_back(Step::Topic);
		return;
	}
	if (_settings.types & Type::AnyChatsMask) {
		_steps.push_back(Step::DialogsList);
	}
	if (_settings.types & Type::PersonalInfo) {
		_steps.push_back(Step::PersonalInfo);
	}
	if (_settings.types & Type::Userpics) {
		_steps.push_back(Step::Userpics);
	}
	if (_settings.types & Type::Stories) {
		_steps.push_back(Step::Stories);
	}
	if (_settings.types & Type::ProfileMusic) {
		_steps.push_back(Step::ProfileMusic);
	}
	if (_settings.types & Type::Contacts) {
		_steps.push_back(Step::Contacts);
	}
	if (_settings.types & Type::Sessions) {
		_steps.push_back(Step::Sessions);
	}
	if (_settings.types & Type::OtherData) {
		_steps.push_back(Step::OtherData);
	}
	if (_settings.types & Type::AnyChatsMask) {
		_steps.push_back(Step::Dialogs);
	}
}

void ControllerObject::fillSubstepsInSteps(const ApiWrap::StartInfo &info) {
	auto result = std::vector<int>();
	const auto push = [&](Step step, int count) {
		const auto index = static_cast<int>(step);
		if (index >= result.size()) {
			result.resize(index + 1, 0);
		}
		result[index] = count;
	};
	push(Step::Initializing, 1);
	if (_settings.types & Settings::Type::AnyChatsMask) {
		push(Step::DialogsList, 1);
	}
	if (_settings.types & Settings::Type::PersonalInfo) {
		push(Step::PersonalInfo, 1);
	}
	if (_settings.types & Settings::Type::Userpics) {
		push(Step::Userpics, 1);
	}
	if (_settings.types & Settings::Type::Stories) {
		push(Step::Stories, 1);
	}
	if (_settings.types & Settings::Type::ProfileMusic) {
		push(Step::ProfileMusic, 1);
	}
	if (_settings.types & Settings::Type::Contacts) {
		push(Step::Contacts, 1);
	}
	if (_settings.types & Settings::Type::Sessions) {
		push(Step::Sessions, 1);
	}
	if (_settings.types & Settings::Type::OtherData) {
		push(Step::OtherData, 1);
	}
	if (_settings.types & Settings::Type::AnyChatsMask) {
		push(Step::Dialogs, info.dialogsCount);
	}
	if (_settings.onlySingleTopic()) {
		push(Step::Topic, 1);
	}
	_substepsInStep = std::move(result);
	_substepsTotal = ranges::accumulate(_substepsInStep, 0);
}

void ControllerObject::cancelExportFast() {
	_updateCounting = false;
	_api.cancelExportFast();
	setState(CancelledState());
}

void ControllerObject::setSessionId(uint64 sessionId) {
	_sessionId = sessionId;
	_api.setSessionId(sessionId);
}

void ControllerObject::setDedupDb(const QString &path) {
	_api.setDedupDb(path);
}

void ControllerObject::setSharedTakeoutId(uint64 id) {
	_api.setSharedTakeoutId(id);
}

void ControllerObject::setTakeoutRefreshHook(Fn<void()> hook) {
	_api.setTakeoutRefreshHook(std::move(hook));
}

void ControllerObject::takeoutRefreshDone(uint64 id) {
	_api.takeoutRefreshDone(id);
}

void ControllerObject::requestChatList(
		Settings settings,
		FnMut<void(Data::DialogsInfo&&)> done) {
	_api.requestChatList(std::move(settings), std::move(done));
}

void ControllerObject::setCachedDialogs(Data::DialogsInfo info) {
	_cachedDialogs = std::move(info);
}

void ControllerObject::refreshOnlyMyMessages() {
	for (auto &info : _dialogsInfo.chats) {
		const auto setting = SettingsFromDialogsType(info.type);
		info.onlyMyMessages = ((_settings.fullChats & setting) != setting);
	}
	for (auto &info : _dialogsInfo.left) {
		info.onlyMyMessages = true;
	}
}

void ControllerObject::exportNext() {
	if (++_stepIndex >= _steps.size()) {
		if (!_scanMode && ioCatchError(_writer->finish())) {
			return;
		}
		if (ioCatchError(writeStatsFile())) {
			return;
		}
		if (ioCatchError(writeFailedJson())) {
			return;
		}
		if (ioCatchError(writeLinksFile())) {
			return;
		}
		_api.finishExport([=] {
			setFinishedState();
		});
		return;
	}

	const auto step = _steps[_stepIndex];
	switch (step) {
	case Step::Initializing: return initialize();
	case Step::DialogsList: return collectDialogsList();
	case Step::PersonalInfo: return exportPersonalInfo();
	case Step::Userpics: return exportUserpics();
	case Step::Stories: return exportStories();
	case Step::ProfileMusic: return exportProfileMusic();
	case Step::Contacts: return exportContacts();
	case Step::Sessions: return exportSessions();
	case Step::OtherData: return exportOtherData();
	case Step::Dialogs: return exportDialogs();
	case Step::Topic: return exportTopic();
	}
	Unexpected("Step in ControllerObject::exportNext.");
}

void ControllerObject::initialize() {
	if (!_updateCounting) {
		setState(stateInitializing());
	}
	_api.startExport(_settings, &_stats, [=](ApiWrap::StartInfo info) {
		initialized(info);
	});
	if (_resumeRecord) {
		_api.setResumeCheckpoint(*_resumeRecord);
	}
	// After the restore, so this is the total before this update.
	if (_updateSession) {
		_statsSessionBase = _stats.serialize();
	}
	_api.setWriterStateGetter([=] {
		return _writer
			? _writer->dialogState()
			: Output::DialogState();
	});
	_api.pauseChanges(
	) | rpl::on_next([=](bool paused) {
		if (paused && !_settings.onlySinglePeer() && !_scanMode) {
			ioCatchError(writeStatsFile());
		}
	}, _lifetime);
	if (_writer) {
		_api.setPauseFlushHandler([=](Data::MessagesSlice prefix) {
			if (prefix.list.empty()) {
				flushPendingSlice();
				return prefix;
			}
			if (batchDialogSlices()) {
				for (auto &entry : prefix.peers) {
					_pendingSlice.peers.emplace(
						entry.first,
						std::move(entry.second));
				}
				for (auto &message : prefix.list) {
					_pendingSlice.list.push_back(std::move(message));
				}
				flushPendingSlice();
			} else if (ioCatchError(_writer->writeDialogSlice(prefix))) {
				return prefix;
			}
			return prefix;
		});
	}
}



void ControllerObject::initialized(const ApiWrap::StartInfo &info) {
	if (!_scanMode
		&& ioCatchError(_writer->start(_settings, _environment, &_stats))) {
		return;
	}
	fillSubstepsInSteps(info);
	exportNext();
}

void ControllerObject::collectDialogsList() {
	if (_cachedDialogs && _settings.chatSelectionActive) {
		_dialogsInfo = std::move(*_cachedDialogs);
		_cachedDialogs = std::nullopt;
		refreshOnlyMyMessages();
		exportNext();
		return;
	}
	_cachedDialogs = std::nullopt;
	setState(stateDialogsList(0));
	_api.requestDialogsList([=](int count) {
		if (count > 0) {
			setState(stateDialogsList(count - 1));
		}
		return true;
	}, [=](Data::DialogsInfo &&result) {
		_dialogsInfo = std::move(result);
		exportNext();
	});
}

void ControllerObject::exportPersonalInfo() {
	setState(statePersonalInfo());
	_api.requestPersonalInfo([=](Data::PersonalInfo &&result) {
		if (ioCatchError(_writer->writePersonal(result))) {
			return;
		}
		exportNext();
	});
}

void ControllerObject::exportUserpics() {
	_api.requestUserpics([=](Data::UserpicsInfo &&start) {
		if (ioCatchError(_writer->writeUserpicsStart(start))) {
			return false;
		}
		_userpicsWritten = 0;
		_userpicsCount = start.count;
		return true;
	}, [=](DownloadProgress progress) {
		setState(stateUserpics(progress));
		return true;
	}, [=](Data::UserpicsSlice &&slice) {
		if (ioCatchError(_writer->writeUserpicsSlice(slice))) {
			return false;
		}
		_userpicsWritten += slice.list.size();
		setState(stateUserpics(DownloadProgress()));
		return true;
	}, [=] {
		if (ioCatchError(_writer->writeUserpicsEnd())) {
			return;
		}
		exportNext();
	});
}

void ControllerObject::exportStories() {
	_api.requestStories([=](Data::StoriesInfo &&start) {
		if (ioCatchError(_writer->writeStoriesStart(start))) {
			return false;
		}
		_storiesWritten = 0;
		_storiesCount = start.count;
		return true;
	}, [=](DownloadProgress progress) {
		setState(stateStories(progress));
		return true;
	}, [=](Data::StoriesSlice &&slice) {
		if (ioCatchError(_writer->writeStoriesSlice(slice))) {
			return false;
		}
		_storiesWritten += slice.list.size();
		setState(stateStories(DownloadProgress()));
		return true;
	}, [=] {
		if (ioCatchError(_writer->writeStoriesEnd())) {
			return;
		}
		exportNext();
	});
}

void ControllerObject::exportProfileMusic() {
	_api.requestProfileMusic([=](Data::ProfileMusicInfo &&start) {
		if (ioCatchError(_writer->writeProfileMusicStart(start))) {
			return false;
		}
		_profileMusicWritten = 0;
		_profileMusicCount = start.count;
		return true;
	}, [=](DownloadProgress progress) {
		setState(stateProfileMusic(progress));
		return true;
	}, [=](Data::ProfileMusicSlice &&slice) {
		if (ioCatchError(_writer->writeProfileMusicSlice(slice))) {
			return false;
		}
		_profileMusicWritten += slice.list.size();
		setState(stateProfileMusic(DownloadProgress()));
		return true;
	}, [=] {
		if (ioCatchError(_writer->writeProfileMusicEnd())) {
			return;
		}
		exportNext();
	});
}

void ControllerObject::exportContacts() {
	setState(stateContacts());
	_api.requestContacts([=](Data::ContactsList &&result) {
		if (ioCatchError(_writer->writeContactsList(result))) {
			return;
		}
		exportNext();
	});
}

void ControllerObject::exportSessions() {
	setState(stateSessions());
	_api.requestSessions([=](Data::SessionsList &&result) {
		if (ioCatchError(_writer->writeSessionsList(result))) {
			return;
		}
		exportNext();
	});
}

void ControllerObject::exportOtherData() {
	setState(stateOtherData());
	const auto relativePath = "lists/other_data.json";
	_api.requestOtherData(relativePath, [=](Data::File &&result) {
		if (ioCatchError(_writer->writeOtherData(result))) {
			return;
		}
		exportNext();
	});
}

void ControllerObject::exportDialogs() {
	if (!_settings.onlySinglePeer() && !_scanMode) {
		if (_globalUpdate && _globalMarker) {
			auto ordered = Data::DialogsInfo();
			auto estimate = int64(0);
			auto eligible = 0;
			for (const auto peerId : _globalMarker->dialogIds) {
				const auto fresh = findFreshDialog(peerId);
				if (!fresh || !chatIncludedInRun(*fresh)) {
					continue;
				}
				(fresh->isLeftChannel ? ordered.left : ordered.chats
					).push_back(*fresh);
				const auto row = findResumeRow(peerId);
				if (!row
					|| row->state != u"done"_q
					|| !row->coversWholeChat()) {
					continue;
				}
				const auto anchor = row->updateAnchor
					? row->updateAnchor
					: row->lastId;
				estimate += std::max(
					int64(0),
					int64(fresh->topMessageId) - int64(anchor.bare));
				++eligible;
			}
			_dialogsInfo = std::move(ordered);
			const auto confirm = _updateConfirm;
			crl::on_main([=, confirm = std::move(confirm)]() mutable {
				if (confirm) {
					confirm(
						int(estimate),
						eligible,
						[=](bool go) mutable {
							if (!go) {
								cancelExportFast();
								return;
							}
							if (!_scanMode
								&& ioCatchError(_writer->writeDialogsStart(
									_dialogsInfo))) {
								return;
							}
							exportNextDialog();
						});
				} else if (estimate > 0) {
					if (!_scanMode
						&& ioCatchError(_writer->writeDialogsStart(
							_dialogsInfo))) {
						return;
					}
					exportNextDialog();
				} else {
					cancelExportFast();
				}
			});
			return;
		}
		if (_globalResume && _globalMarker) {
			applyGlobalResumeScope();
			_api.setGlobalDialogIds(_globalMarker->dialogIds);
		} else {
			filterRunDialogs();
			auto ids = std::vector<PeerId>();
			ids.reserve(_dialogsInfo.chats.size() + _dialogsInfo.left.size());
			for (const auto &info : _dialogsInfo.chats) {
				ids.push_back(info.peerId);
			}
			for (const auto &info : _dialogsInfo.left) {
				ids.push_back(info.peerId);
			}
			_api.setGlobalDialogIds(std::move(ids));
		}
		_api.commitGlobalMarker(u"run"_q, totalFailedCount());
	}
	if (!_settings.onlySinglePeer()) {
		filterRunDialogs();
	}
	if (!_scanMode && ioCatchError(_writer->writeDialogsStart(_dialogsInfo))) {
		return;
	}

	exportNextDialog();
}

bool ControllerObject::chatIncludedInRun(
		const Data::DialogInfo &info) const {
	const auto setting = SettingsFromDialogsType(info.type);
	if (((_settings.types & setting) != 0)
		|| (info.migratedToChannelId
			&& (((_settings.types & Settings::Type::PublicGroups) != 0)
				|| ((_settings.types & Settings::Type::PrivateGroups)
					!= 0)))) {
		return !_settings.chatSelectionActive
			|| _settings.selectedChats.contains(info.peerId);
	}
	return false;
}

void ControllerObject::filterRunDialogs() {
	auto &chats = _dialogsInfo.chats;
	auto &left = _dialogsInfo.left;
	chats.erase(
		ranges::remove_if(
			chats,
			[&](const Data::DialogInfo &info) {
				return !chatIncludedInRun(info);
			}),
		end(chats));
	left.erase(
		ranges::remove_if(
			left,
			[&](const Data::DialogInfo &info) {
				return !chatIncludedInRun(info);
			}),
		end(left));
}

const ::Data::ExResumeRecord *ControllerObject::findResumeRow(
		PeerId peerId) const {
	for (const auto &row : _resumeRows) {
		if (row.peerId == peerId) {
			return &row;
		}
	}
	return nullptr;
}

const Data::DialogInfo *ControllerObject::findFreshDialog(
		PeerId peerId) const {
	for (const auto &info : _dialogsInfo.chats) {
		if (info.peerId == peerId) {
			return &info;
		}
	}
	for (const auto &info : _dialogsInfo.left) {
		if (info.peerId == peerId) {
			return &info;
		}
	}
	return nullptr;
}

void ControllerObject::applyGlobalResumeScope() {
	Expects(_globalMarker.has_value());
	// Keep the snapshot order; chats created while paused are not in
	// the stored list and wait for the next Export.
	auto ordered = Data::DialogsInfo();
	auto previousBlob = QByteArray();
	auto lastStats = QByteArray();
	for (const auto peerId : _globalMarker->dialogIds) {
		const auto fresh = findFreshDialog(peerId);
		if (!fresh) {
			auto failed = Data::FailedChat();
			failed.peerId = peerId;
			failed.error = u"CHAT_NOT_FOUND"_q;
			_failedChats.push_back(std::move(failed));
			continue;
		}
		(fresh->isLeftChannel ? ordered.left : ordered.chats
			).push_back(*fresh);
		if (const auto row = findResumeRow(peerId)) {
			if (!row->stats.isEmpty()) {
				if (row->state == u"done"_q) {
					auto current = Output::Stats();
					current.restore(row->stats);
					auto base = Output::Stats();
					base.restore(previousBlob);
					auto block = ChatStatBlock();
					block.header = ChatBlockHeader(*fresh);
					block.state = StatsDifference(current, base);
					_chatStats.push_back(std::move(block));
				}
				previousBlob = row->stats;
				lastStats = row->stats;
			}
		}
	}
	_dialogsInfo = std::move(ordered);
	// Only the interrupted chat keeps an open writer object (HTML file
	// or JSON nesting): failed chats were closed and re-walk fresh.
	// A boundary park names a chat with no row yet; an in-walk pause or
	// crash leaves the last unfinished row, found by reverse scan.
	_resumePeerId = _globalMarker->resumePeerId;
	if (!_resumePeerId) {
		for (auto i = _globalMarker->dialogIds.size(); i != 0;) {
			const auto peerId = _globalMarker->dialogIds[--i];
			const auto row = findResumeRow(peerId);
			if (row
				&& row->state != u"done"_q
				&& (row->lastId
					|| row->msgsDone != 0
					|| !row->stats.isEmpty())) {
				_resumePeerId = peerId;
				break;
			}
		}
	}
	// Seed the accumulator with the last finished chat's totals so the
	// TOTAL line and later checkpoints stay cumulative across sessions.
	if (!lastStats.isEmpty()) {
		_stats.restore(lastStats);
	}
}

bool ControllerObject::batchDialogSlices() const {
	if (!_settings.onlySinglePeer()) {
		return false;
	}
	using Type = MediaSettings::Type;
	const auto fileTypes = Type::Photo | Type::Video | Type::VoiceMessage
		| Type::VideoMessage | Type::Sticker | Type::GIF | Type::File
		| Type::Audio;
	return ((_settings.media.types & fileTypes) == 0);
}

bool ControllerObject::flushPendingSlice() {
	if (_scanMode) {
		_pendingSlice = Data::MessagesSlice();
		return true;
	}
	if (_pendingSlice.list.empty()) {
		return true;
	}
	if (!_dialogOpened) {
		_pendingSlice = Data::MessagesSlice();
		return true;
	}
	auto slice = std::move(_pendingSlice);
	_pendingSlice = Data::MessagesSlice();
	if (ioCatchError(_writer->writeDialogSlice(slice))) {
		return false;
	}
	return true;
}

void ControllerObject::exportNextDialog() {
	auto index = _dialogIndex + 1;
	auto info = _dialogsInfo.item(index);
	const auto global = !_settings.onlySinglePeer() && !_scanMode;
	if (global && (_globalResume || _globalUpdate)) {
		if (_globalResume) {
			while (info) {
				const auto row = findResumeRow(info->peerId);
				if (!row || row->state != u"done"_q) {
					break;
				}
				if (ioCatchError(_writer->writeDialogSkipped(
						*info,
						row->total))) {
					return;
				}
				_dialogIndex = index;
				info = _dialogsInfo.item(++index);
			}
		}
		if (_globalUpdate) {
			while (info) {
				const auto row = findResumeRow(info->peerId);
				if (row
					&& row->state == u"done"_q
					&& row->coversWholeChat()) {
					break;
				}
				if (row
					&& ioCatchError(_writer->writeDialogSkipped(
						*info,
						row->total))) {
					return;
				}
				_dialogIndex = index;
				info = _dialogsInfo.item(++index);
			}
		}
		if (info) {
			if (_globalUpdate) {
				if (const auto row = findResumeRow(info->peerId)) {
					_resumeRecord = *row;
					_api.setResumeCheckpoint(*row, false);
					_api.liftUpdateCeiling();
				}
			} else if (const auto row = findResumeRow(info->peerId);
				row
				&& row->state != u"done"_q
				&& info->peerId == _resumePeerId) {
				// The interrupted chat: continue where it stopped. The
				// accumulator keeps its seeded totals; only the walk
				// position is restored from the row. Failed chats
				// re-walk from scratch instead: their writer objects
				// were already closed.
				_resumeRecord = *row;
				_api.setResumeCheckpoint(*row, false);
			}
		}
	}
	if (info) {
		if (global
			&& !_globalUpdate
			&& _api.parkGlobalAtBoundary(
				info->peerId,
				totalFailedCount())) {
			return;
		}
		_dialogIndex = index;
		if (global) {
			const auto peerId = info->peerId;
			const auto name = QString::fromUtf8(info->name);
			_api.setChatFailHandler([=](const MTP::Error &e) {
				failCurrentDialog(peerId, name, e.type());
			});
		}
		_api.requestMessages(*info, [=](const Data::DialogInfo &info) {
		_dialogOpen = true;
		if (!_scanMode && _dialogOpened) {
			_messagesWritten = 0;
		} else if (!_scanMode
			&& _resumeRecord
			&& !_freshFolder
			&& (_settings.onlySinglePeer()
				|| _globalResume
				|| _globalUpdate)) {
			auto state = Output::DialogState();
			state.messagesCount = _resumeRecord->htmlIndex;
			state.dateMessageId = _resumeRecord->dateIndex;
			state.lastIds = _resumeRecord->repliedIndex;
			state.lastMessage = _resumeRecord->lastMsg;
			if (ioCatchError(
				_updateSession
				? _writer->writeDialogUpdateStart(info, state)
				: _writer->resumeDialogStart(info, state)
			)) {
				return false;
			}
			_messagesWritten = _resumeRecord->msgsDone;
			_resumeRecord = std::nullopt;
			_dialogOpened = true;
		} else {
			if (!_scanMode
				&& ioCatchError(_writer->writeDialogStart(info))) {
				return false;
			}
			_messagesWritten = 0;
			_dialogOpened = true;
		}
		if (!_settings.onlySinglePeer() && !_scanMode) {
			_statsChatBase = _stats.serialize();
			_chatBlockHeader = ChatBlockHeader(info);
		}
		// File-filtered exports count selected messages, not
		// walked ones: the ApiWrap probes carry the exact total.
		_selectedCounting = _api.hasSelectedTotal();
		const auto rangeTotal = _api.rangeDenominator();
		_messagesCount = rangeTotal
			? rangeTotal
			: (_selectedCounting
				? _api.selectedTotal()
				: ranges::accumulate(
					info.messagesCountPerSplit,
					0));
		setState(stateDialogs(selectedProgress()));
		if (!rangeTotal
			&& (_settings.singlePeerFrom || _settings.singlePeerTill)) {
			_api.requestRangeTotal([=](int count) {
				_messagesCount = count;
				setState(stateDialogs(selectedProgress()));
			});
		}
			return true;
		}, [=](DownloadProgress progress) {
			setState(stateDialogs(progress));
			return true;
		}, [=](Data::MessagesSlice &&result) {
			if (_scanMode) {
				_messagesWritten += result.list.size();
				setState(stateDialogs(DownloadProgress()));
				return true;
			}
		if (batchDialogSlices()) {
			const auto size = result.list.size();
			for (auto &entry : result.peers) {
				_pendingSlice.peers.emplace(
					entry.first,
					std::move(entry.second));
			}
			for (auto &message : result.list) {
				_pendingSlice.list.push_back(std::move(message));
			}
			_messagesWritten += size;
			setState(stateDialogs(selectedProgress()));
			if (int(_pendingSlice.list.size()) < kDialogWriteBatch) {
				return true;
			}
			return flushPendingSlice();
		}
		if (ioCatchError(_writer->writeDialogSlice(result))) {
			return false;
		}
		_messagesWritten += result.list.size();
		setState(stateDialogs(selectedProgress()));
		return true;
		}, [=] {
			_api.setChatFailHandler(nullptr);
			if (!flushPendingSlice()) {
				return;
			}
			if (!_scanMode && ioCatchError(_writer->writeDialogEnd())) {
				return;
			}
			_dialogOpen = false;
			_dialogOpened = false;
			if (!_settings.onlySinglePeer() && !_scanMode) {
				recordChatStats();
				if (ioCatchError(writeStatsFile())) {
					return;
				}
			}
			exportNextDialog();
		});
		return;
	}
	_api.setChatFailHandler(nullptr);
	if (!_settings.onlySinglePeer() && !_scanMode && !_globalUpdate) {
		_api.commitGlobalMarker(u"done"_q, totalFailedCount());
	}
	if (!_scanMode && ioCatchError(_writer->writeDialogsEnd())) {
		return;
	}
	exportNext();
}

void ControllerObject::failCurrentDialog(
		PeerId peerId,
		const QString &name,
		const QString &error) {
	// Global runs only: one bad chat is recorded and the loop continues.
	// The handler is consumed, so a second stale failure cannot re-enter.
	_api.setChatFailHandler(nullptr);
	if (stopped()) {
		return;
	}
	auto failed = Data::FailedChat();
	failed.peerId = peerId;
	failed.name = name;
	failed.error = error;
	_failedChats.push_back(std::move(failed));
	if (!_globalUpdate) {
		_api.commitGlobalMarker(u"run"_q, totalFailedCount());
	}
	// Close the half-open writer dialog cleanly so the next chat starts
	// from a closed state; the partial folder is kept as-is.
	closeDialogFiles();
	_messagesWritten = 0;
	_pendingSlice = Data::MessagesSlice();
	_resumeRecord = std::nullopt;
	exportNextDialog();
}

Output::Result ControllerObject::writeFailedJson() const {
	if (_failedChats.empty()) {
		return Output::Result::Success();
	}
	const auto dir = _settings.path + QString::fromLatin1("lists/");
	if (!QDir().mkpath(dir)) {
		return Output::Result(Output::Result::Type::Error, dir);
	}
	auto file = QFile(dir + QString::fromLatin1("failed.json"));
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		return Output::Result(
			Output::Result::Type::Error,
			file.fileName());
	}
	auto stream = QTextStream(&file);
	const auto escape = [](const QString &value) {
		auto result = QString();
		result.reserve(value.size());
		for (const auto c : value) {
			if (c == u'"') {
				result += u"\\\""_q;
			} else if (c == u'\\') {
				result += u"\\\\"_q;
			} else if (c.unicode() < 0x20) {
				result += u" "_q;
			} else {
				result += c;
			}
		}
		return result;
	};
	stream << u"[\n"_q;
	auto first = true;
	for (const auto &failed : _failedChats) {
		if (!first) {
			stream << u",\n"_q;
		}
		first = false;
		stream << u"  { \"peer_id\": "_q << qint64(failed.peerId.value)
			<< u", \"name\": \""_q << escape(failed.name)
			<< u"\", \"error\": \""_q << escape(failed.error)
			<< u"\" }"_q;
	}
	stream << u"\n]\n"_q;
	stream.flush();
	if (stream.status() != QTextStream::Ok) {
		return Output::Result(
			Output::Result::Type::Error,
			file.fileName());
	}
	return Output::Result::Success();
}

template <typename Callback>
ProcessingState ControllerObject::prepareState(
		Step step,
		Callback &&callback) const {
	if (step != _lastProcessingStep) {
		_substepsPassed += substepsInStep(_lastProcessingStep);
		_lastProcessingStep = step;
	}

	auto result = ProcessingState();
	callback(result);
	result.step = step;
	result.substepsPassed = _substepsPassed;
	result.substepsNow = substepsInStep(_lastProcessingStep);
	result.substepsTotal = _substepsTotal;
	return result;
}

ProcessingState ControllerObject::stateInitializing() const {
	return ProcessingState();
}

ProcessingState ControllerObject::stateDialogsList(int processed) const {
	const auto step = Step::DialogsList;
	return prepareState(step, [&](ProcessingState &result) {
		result.entityIndex = processed;
		result.entityCount = std::max(
			processed,
			substepsInStep(Step::Dialogs));
	});
}
ProcessingState ControllerObject::statePersonalInfo() const {
	return prepareState(Step::PersonalInfo);
}

ProcessingState ControllerObject::stateUserpics(
		const DownloadProgress &progress) const {
	return prepareState(Step::Userpics, [&](ProcessingState &result) {
		result.entityIndex = _userpicsWritten + progress.itemIndex;
		result.entityCount = std::max(_userpicsCount, result.entityIndex);
		result.bytesRandomId = progress.randomId;
		if (!progress.path.isEmpty()) {
			const auto last = progress.path.lastIndexOf('/');
			result.bytesName = progress.path.mid(last + 1);
		}
		result.bytesLoaded = progress.ready;
		result.bytesCount = progress.total;
	});
}

ProcessingState ControllerObject::stateStories(
		const DownloadProgress &progress) const {
	return prepareState(Step::Stories, [&](ProcessingState &result) {
		result.entityIndex = _storiesWritten + progress.itemIndex;
		result.entityCount = std::max(_storiesCount, result.entityIndex);
		result.bytesRandomId = progress.randomId;
		if (!progress.path.isEmpty()) {
			const auto last = progress.path.lastIndexOf('/');
			result.bytesName = progress.path.mid(last + 1);
		}
		result.bytesLoaded = progress.ready;
		result.bytesCount = progress.total;
	});
}

ProcessingState ControllerObject::stateProfileMusic(
		const DownloadProgress &progress) const {
	return prepareState(Step::ProfileMusic, [&](ProcessingState &result) {
		result.entityIndex = _profileMusicWritten + progress.itemIndex;
		result.entityCount = std::max(_profileMusicCount, result.entityIndex);
		result.bytesRandomId = progress.randomId;
		if (!progress.path.isEmpty()) {
			const auto last = progress.path.lastIndexOf('/');
			result.bytesName = progress.path.mid(last + 1);
		}
		result.bytesLoaded = progress.ready;
		result.bytesCount = progress.total;
	});
}

ProcessingState ControllerObject::stateContacts() const {
	return prepareState(Step::Contacts);
}

ProcessingState ControllerObject::stateSessions() const {
	return prepareState(Step::Sessions);
}

ProcessingState ControllerObject::stateOtherData() const {
	return prepareState(Step::OtherData);
}

ProcessingState ControllerObject::stateDialogs(
		const DownloadProgress &progress) const {
	const auto step = Step::Dialogs;
	return prepareState(step, [&](ProcessingState &result) {
		fillMessagesState(
			result,
			_dialogsInfo,
			_dialogIndex,
			progress);
	});
}

ControllerObject::DownloadProgress ControllerObject::selectedProgress() const {
	auto result = DownloadProgress();
	if (_selectedCounting) {
		result.itemIndex = _api.chatSelectedDone();
	}
	return result;
}

void ControllerObject::fillMessagesState(
		ProcessingState &result,
		const Data::DialogsInfo &info,
		int index,
		const DownloadProgress &progress) const {
	const auto dialog = info.item(index);
	Assert(dialog != nullptr);

	result.entityIndex = index;
	result.entityCount = info.chats.size() + info.left.size();
	result.entityName = dialog->name;
	result.entityType = (dialog->type == Data::DialogInfo::Type::Self)
		? ProcessingState::EntityType::SavedMessages
		: (dialog->type == Data::DialogInfo::Type::Replies)
		? ProcessingState::EntityType::RepliesMessages
		: (dialog->type == Data::DialogInfo::Type::VerifyCodes)
		? ProcessingState::EntityType::VerifyCodes
		: ProcessingState::EntityType::Chat;
	// Selected mode reports absolute selected ordinals from the
	// walk; otherwise the walked base plus the slice position.
	result.itemIndex = _selectedCounting
		? progress.itemIndex
		: _messagesWritten + progress.itemIndex;
	result.itemCount = std::max(_messagesCount, result.itemIndex);
	result.bytesRandomId = progress.randomId;
	if (!progress.path.isEmpty()) {
		const auto last = progress.path.lastIndexOf('/');
		result.bytesName = progress.path.mid(last + 1);
	}
	result.bytesLoaded = progress.ready;
	result.bytesCount = progress.total;
}

int ControllerObject::substepsInStep(Step step) const {
	Expects(_substepsInStep.size() > static_cast<int>(step));

	return _substepsInStep[static_cast<int>(step)];
}

void ControllerObject::exportTopic() {
	auto topicInfo = Data::DialogInfo();
	topicInfo.type = Data::DialogInfo::Type::PublicSupergroup;
	topicInfo.name = _topicTitle.toUtf8();
	topicInfo.peerId = PeerId(_topicPeerId);
	topicInfo.relativePath = QString();

	if (!_scanMode && ioCatchError(_writer->writeDialogStart(topicInfo))) {
		return;
	}

	_api.requestTopicMessages(
		PeerId(_topicPeerId),
		_settings.singlePeer,
		_topicRootId,
		[=](int count) {
			_messagesWritten = 0;
			_messagesCount = count;
			setState(stateTopic(DownloadProgress()));
			return true;
		},
		[=](DownloadProgress progress) {
			setState(stateTopic(progress));
			return true;
		},
		[=](Data::MessagesSlice &&slice) {
			if (_scanMode) {
				_messagesWritten += slice.list.size();
				setState(stateTopic(DownloadProgress()));
				return true;
			}
			if (batchDialogSlices()) {
				const auto size = slice.list.size();
				for (auto &entry : slice.peers) {
					_pendingSlice.peers.emplace(
						entry.first,
						std::move(entry.second));
				}
				for (auto &message : slice.list) {
					_pendingSlice.list.push_back(std::move(message));
				}
				_messagesWritten += size;
				setState(stateTopic(DownloadProgress()));
				if (int(_pendingSlice.list.size()) < kDialogWriteBatch) {
					return true;
				}
				return flushPendingSlice();
			}
			if (ioCatchError(_writer->writeDialogSlice(slice))) {
				return false;
			}
			_messagesWritten += slice.list.size();
			setState(stateTopic(DownloadProgress()));
			return true;
		},
		[=] {
			if (!flushPendingSlice()) {
				return;
			}
			if (!_scanMode && ioCatchError(_writer->writeDialogEnd())) {
				return;
			}
			if (!_scanMode && ioCatchError(_writer->finish())) {
				return;
			}
			if (ioCatchError(writeStatsFile())) {
				return;
			}
			if (!_scanMode && ioCatchError(writeLinksFile())) {
				return;
			}
			_api.finishExport([=] {
				setFinishedState();
			});
		});
}

ProcessingState ControllerObject::stateTopic(
		const DownloadProgress &progress) const {
	return prepareState(Step::Topic, [&](ProcessingState &result) {
		result.entityType = ProcessingState::EntityType::Topic;
		result.entityName = _topicTitle;
		result.entityIndex = 0;
		result.entityCount = 1;
		result.itemIndex = _messagesWritten + progress.itemIndex;
		result.itemCount = std::max(_messagesCount, result.itemIndex);
		result.bytesRandomId = progress.randomId;
		if (!progress.path.isEmpty()) {
			const auto last = progress.path.lastIndexOf('/');
			result.bytesName = progress.path.mid(last + 1);
		}
		result.bytesLoaded = progress.ready;
		result.bytesCount = progress.total;
	});
}

void ControllerObject::setFinishedState() {
	// An update shows this session's files, a main export the whole thing.
	// A blob that fails to restore leaves the base at zero.
	auto base = Output::Stats();
	if (_updateSession) {
		base.restore(_statsSessionBase);
	}
	auto state = statsShown(base);
	state.path = _scanMode
		? (_settings.path + QString::fromLatin1("stats.txt"))
		: _writer->mainFilePath();
	state.failedChats = _failedChats;
	setState(std::move(state));
}

FinishedState ControllerObject::statsShown(const Output::Stats &base) const {
	return StatsDifference(_stats, base);
}

FinishedState StatsDifference(
		const Output::Stats &current,
		const Output::Stats &base) {
	auto state = FinishedState();
	const auto shown = [&](auto value, auto was) {
		return std::max(value - was, int64(0));
	};
	for (auto i = 0; i != Output::Stats::kGroups; ++i) {
		const auto type = Output::Stats::kGroupStats[i].type;
		state.groupFiles[i] = shown(
			current.typeFiles(type),
			base.typeFiles(type));
		state.groupBytes[i] = shown(
			current.typeBytes(type),
			base.typeBytes(type));
		state.groupSkipped[i] = shown(
			current.typeSkipped(type),
			base.typeSkipped(type));
		state.groupSkippedBytes[i] = shown(
			current.typeSkippedBytes(type),
			base.typeSkippedBytes(type));
		if (type == MediaSettings::Type::Poll) {
			continue;
		}
		state.filesCount += int(state.groupFiles[i]);
		state.bytesCount += state.groupBytes[i];
		state.skippedFiles += state.groupSkipped[i];
		state.skippedBytes += state.groupSkippedBytes[i];
	}
	state.textMessages = shown(current.textMessages(), base.textMessages());
	state.linkMessages = shown(current.linkMessages(), base.linkMessages());
	state.linkTotal = shown(current.linkTotal(), base.linkTotal());
	state.linkDuplicates = shown(
		current.linkDuplicates(),
		base.linkDuplicates());
	state.messagesTotal = shown(
		current.messagesTotal(),
		base.messagesTotal());
	return state;
}

void ControllerObject::recordChatStats() {
	auto base = Output::Stats();
	base.restore(_statsChatBase);
	auto block = ChatStatBlock();
	block.header = _chatBlockHeader;
	block.state = statsShown(base);
	_chatStats.push_back(std::move(block));
}

constexpr auto kDiagBuild = 17;

Output::Result ControllerObject::writeStatsFile() const {
	const auto path = _settings.path + QString::fromLatin1("stats.txt");
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		return Output::Result(Output::Result::Type::Error, path);
	}
	auto stream = QTextStream(&file);
	const auto date = QDateTime::currentDateTime().toString(
		"dd.MM.yyyy, hh.mm.ss");
	const auto cut = _singlePeerFolder.indexOf('_');
	auto done = false;
	if (cut > 0) {
		const auto id = _singlePeerFolder.mid(0, cut);
		const auto name = _singlePeerFolder.mid(cut + 1);
		const auto digits = id.startsWith('-') ? id.mid(1) : id;
		auto numeric = !digits.isEmpty() && !name.isEmpty();
		for (const auto c : digits) {
			if (!c.isDigit()) {
				numeric = false;
				break;
			}
		}
		if (numeric) {
			stream << id << ". " << name << ", " << date << "\n\n";
			done = true;
		}
	}
	if (!done) {
		stream << QDir(_settings.path).dirName() << ", " << date << "\n\n";
	}
	if (!_settings.onlySinglePeer() && !_scanMode && !_chatStats.empty()) {
		stream << "Run by account " << _sessionId << "\n\n";
		for (const auto &block : _chatStats) {
			stream << block.header << '\n';
			WriteStatsGroups(stream, block.state);
			stream << '\n';
		}
		stream << "TOTAL ("
			<< int(_chatStats.size())
			<< ((_chatStats.size() == 1) ? " chat)\n" : " chats)\n");
		WriteStatsGroups(stream, statsShown(Output::Stats()));
	} else {
		auto totalFiles = int64(0);
		auto totalBytes = int64(0);
		auto skippedFiles = int64(0);
		auto skippedBytes = int64(0);
		for (auto i = 0; i != Output::Stats::kGroups; ++i) {
			const auto type = Output::Stats::kGroupStats[i].type;
			if (type == MediaSettings::Type::Poll) {
				continue;
			}
			totalFiles += _stats.typeFiles(type);
			totalBytes += _stats.typeBytes(type);
			skippedFiles += _stats.typeSkipped(type);
			skippedBytes += _stats.typeSkippedBytes(type);
		}
		LOG(("ExportDiag: summary messages=%1 unique=%2 dupes=%3 build=%4")
			.arg(_stats.messagesTotal())
			.arg(totalFiles)
			.arg(skippedFiles)
			.arg(kDiagBuild));
		for (const auto i : Output::Stats::kDisplayOrder) {
			const auto &group = Output::Stats::kGroupStats[i];
			const auto files = _stats.typeFiles(group.type);
			const auto skipped = _stats.typeSkipped(group.type);
			if (!files && !skipped) {
				continue;
			}
			LOG(("ExportDiag: group %1 unique=%2 uniquebytes=%3 dupes=%4 dupebytes=%5")
				.arg(group.key)
				.arg(files)
				.arg(_stats.typeBytes(group.type))
				.arg(skipped)
				.arg(_stats.typeSkippedBytes(group.type)));
		}
		WriteStatsGroups(stream, statsShown(Output::Stats()));
	}
	if (!_failedChats.empty()) {
		stream << "Failed chats: " << _failedChats.size() << '\n';
	}
	stream.flush();
	if (stream.status() != QTextStream::Ok) {
		return Output::Result(Output::Result::Type::Error, path);
	}
	return Output::Result::Success();
}

Output::Result ControllerObject::writeLinksFile() const {
	using Type = MediaSettings::Type;
	if (!(_settings.media.types & (Type::Link | Type::FullHistory))) {
		return Output::Result::Success();
	}
	const auto urls = _api.linkUrls();
	if (urls.empty()) {
		return Output::Result::Success();
	}
	const auto path = _settings.path + QString::fromLatin1("links.txt");
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		return Output::Result(Output::Result::Type::Error, path);
	}
	auto stream = QTextStream(&file);
	for (const auto &url : urls) {
		stream << url << '\n';
	}
	stream.flush();
	if (stream.status() != QTextStream::Ok) {
		return Output::Result(Output::Result::Type::Error, path);
	}
	return Output::Result::Success();
}

Controller::Controller(
	QPointer<MTP::Instance> mtproto,
	const MTPInputPeer &peer)
: _wrapped(std::move(mtproto), peer) {
}

Controller::Controller(
	QPointer<MTP::Instance> mtproto,
	const MTPInputPeer &peer,
	int32 topicRootId,
	uint64 peerId,
	const QString &topicTitle)
: _wrapped(
	std::move(mtproto),
	peer,
	static_cast<int32>(topicRootId),
	static_cast<uint64>(peerId),
	topicTitle) {
}

rpl::producer<State> Controller::state() const {
	return _wrapped.producer_on_main([=](const Implementation &unwrapped) {
		return unwrapped.state();
	});
}

//void Controller::submitPassword(const QString &password) {
//	_wrapped.with([=](Implementation &unwrapped) {
//		unwrapped.submitPassword(password);
//	});
//}
//
//void Controller::requestPasswordRecover() {
//	_wrapped.with([=](Implementation &unwrapped) {
//		unwrapped.requestPasswordRecover();
//	});
//}
//
//rpl::producer<PasswordUpdate> Controller::passwordUpdate() const {
//	return _wrapped.producer_on_main([=](const Implementation &unwrapped) {
//		return unwrapped.passwordUpdate();
//	});
//}
//
//void Controller::reloadPasswordState() {
//	_wrapped.with([=](Implementation &unwrapped) {
//		unwrapped.reloadPasswordState();
//	});
//}
//
//void Controller::cancelUnconfirmedPassword() {
//	_wrapped.with([=](Implementation &unwrapped) {
//		unwrapped.cancelUnconfirmedPassword();
//	});
//}

void Controller::startExport(
		const Settings &settings,
		const Environment &environment,
		const QString &singlePeerFolder) {
	LOG(("Export Info: Started export to '%1'.").arg(settings.path));

	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.startExport(settings, environment, singlePeerFolder);
	});
}

void Controller::startScan(
		const Settings &settings,
		const Environment &environment,
		const QString &singlePeerFolder) {
	LOG(("Export Info: Started scan to '%1'.").arg(settings.path));

	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.startScan(settings, environment, singlePeerFolder);
	});
}

void Controller::startResumeExport(
		const Settings &settings,
		const Environment &environment,
		const ::Data::ExResumeRecord &record) {
	LOG(("Export Info: Resumed export to '%1'.").arg(settings.path));

	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.startResumeExport(settings, environment, record);
	});
}

void Controller::startResumeExportGlobal(
		const Settings &settings,
		const Environment &environment,
		const ::Data::ExResumeRecord &marker,
		std::vector<::Data::ExResumeRecord> rows) {
	LOG(("Export Info: Resumed global export to '%1'.").arg(settings.path));

	_wrapped.with([=, rows = std::move(rows)](
			Implementation &unwrapped) mutable {
		unwrapped.startResumeExportGlobal(
			settings,
			environment,
			marker,
			std::move(rows));
	});
}

void Controller::startUpdateExportGlobal(
		const Settings &settings,
		const Environment &environment,
		const ::Data::ExResumeRecord &marker,
		std::vector<::Data::ExResumeRecord> rows) {
	LOG(("Export Info: Global update export to '%1'.").arg(settings.path));

	_wrapped.with([=, rows = std::move(rows)](
			Implementation &unwrapped) mutable {
		unwrapped.startUpdateExportGlobal(
			settings,
			environment,
			marker,
			std::move(rows));
	});
}

void Controller::startUpdateExport(
		const Settings &settings,
		const Environment &environment,
		const ::Data::ExResumeRecord &record,
		const QString &newFolderName) {
	LOG(("Export Info: Update export to '%1'.").arg(settings.path));

	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.startUpdateExport(
			settings,
			environment,
			record,
			newFolderName);
	});
}

void Controller::setUpdateConfirmHandler(
		Fn<void(int anyNew, int selectedNew, FnMut<void(bool)> proceed)> handler) {
	_wrapped.with([=, handler = std::move(handler)](
			Implementation &unwrapped) mutable {
		unwrapped.setUpdateConfirmHandler(std::move(handler));
	});
}

void Controller::skipFile(uint64 randomId) {
	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.skipFile(randomId);
	});
}

void Controller::closeDialogFiles() {
	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.closeDialogFiles();
	});
}

void Controller::requestPause() {
	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.requestPause();
	});
}

void Controller::resumeExport() {
	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.resumeExport();
	});
}

rpl::producer<bool> Controller::pauseChanges() const {
	return _wrapped.producer_on_main([=](const Implementation &unwrapped) {
		return unwrapped.pauseChanges();
	});
}

rpl::producer<bool> Controller::canPauseChanges() const {
	return _wrapped.producer_on_main([=](const Implementation &unwrapped) {
		return unwrapped.canPauseChanges();
	});
}

void Controller::cancelExportFast() {
	LOG(("Export Info: Cancelled export."));

	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.cancelExportFast();
	});
}

void Controller::setSessionId(uint64 sessionId) {
	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.setSessionId(sessionId);
	});
}

void Controller::setDedupDb(const QString &path) {
	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.setDedupDb(path);
	});
}

void Controller::setSharedTakeoutId(uint64 id) {
	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.setSharedTakeoutId(id);
	});
}

void Controller::setTakeoutRefreshHook(Fn<void()> hook) {
	_wrapped.with([=, hook = std::move(hook)](Implementation &unwrapped) mutable {
		unwrapped.setTakeoutRefreshHook(std::move(hook));
	});
}

void Controller::takeoutRefreshDone(uint64 id) {
	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.takeoutRefreshDone(id);
	});
}

void Controller::requestChatList(
		Settings settings,
		FnMut<void(Data::DialogsInfo&&)> done) {
	_wrapped.with([=, done = std::move(done)](
			Implementation &unwrapped) mutable {
		unwrapped.requestChatList(std::move(settings), std::move(done));
	});
}

void Controller::setCachedDialogs(Data::DialogsInfo info) {
	_wrapped.with([=, info = std::move(info)](
			Implementation &unwrapped) mutable {
		unwrapped.setCachedDialogs(std::move(info));
	});
}

rpl::lifetime &Controller::lifetime() {
	return _lifetime;
}

Controller::~Controller() {
	LOG(("Export Info: Controller destroyed."));
}

} // namespace Export