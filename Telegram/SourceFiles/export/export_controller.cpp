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
#include "mtproto/mtp_instance.h"
#include "ui/text/format_values.h"

#include <QtCore/QFile>
#include <QtCore/QTextStream>

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
	return result;
}

} // namespace

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
		const Environment &environment);
	void skipFile(uint64 randomId);
	void cancelExportFast();
	void setSessionId(uint64 sessionId);
	void setDedupDb(const QString &path);

private:
	using Step = ProcessingState::Step;
	using DownloadProgress = ApiWrap::DownloadProgress;

	[[nodiscard]] bool stopped() const;
	void setState(State &&state);
	void ioError(const QString &path);
	bool ioCatchError(Output::Result result);
	void setFinishedState();
	Output::Result writeStatsFile() const;
	Output::Result writeLinksFile() const;

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
	void fillMessagesState(
		ProcessingState &result,
		const Data::DialogsInfo &info,
		int index,
		const DownloadProgress &progress) const;

	int substepsInStep(Step step) const;

	ApiWrap _api;
	Settings _settings;
	Environment _environment;

	Data::DialogsInfo _dialogsInfo;
	int _dialogIndex = -1;

	int _messagesWritten = 0;
	int _messagesCount = 0;
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

	std::vector<int> _substepsInStep;
	int _substepsTotal = 0;
	mutable int _substepsPassed = 0;
	mutable Step _lastProcessingStep = Step::Initializing;

	std::unique_ptr<Output::AbstractWriter> _writer;
	std::vector<Step> _steps;
	int _stepIndex = -1;

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
		const Environment &environment) {
	if (!_settings.path.isEmpty()) {
		return;
	}
	_settings = NormalizeSettings(settings);
	_environment = environment;
	_settings.singleTopicRootId = _topicRootId;
	_settings.singleTopicPeerId = _topicPeerId;

	_settings.path = Output::NormalizePath(_settings);
	_writer = Output::CreateWriter(_settings.format);
	fillExportSteps();
	exportNext();
}

void ControllerObject::skipFile(uint64 randomId) {
	if (stopped()) {
		return;
	}
	_api.skipFile(randomId);
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
	_api.cancelExportFast();
	setState(CancelledState());
}

void ControllerObject::setSessionId(uint64 sessionId) {
	_api.setSessionId(sessionId);
}

void ControllerObject::setDedupDb(const QString &path) {
	_api.setDedupDb(path);
}

void ControllerObject::exportNext() {
	if (++_stepIndex >= _steps.size()) {
		if (ioCatchError(_writer->finish())) {
			return;
		}
		if (ioCatchError(writeStatsFile())) {
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
	setState(stateInitializing());
	_api.startExport(_settings, &_stats, [=](ApiWrap::StartInfo info) {
		initialized(info);
	});
}

void ControllerObject::initialized(const ApiWrap::StartInfo &info) {
	if (ioCatchError(_writer->start(_settings, _environment, &_stats))) {
		return;
	}
	fillSubstepsInSteps(info);
	exportNext();
}

void ControllerObject::collectDialogsList() {
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
	if (ioCatchError(_writer->writeDialogsStart(_dialogsInfo))) {
		return;
	}

	exportNextDialog();
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
	if (_pendingSlice.list.empty()) {
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
	const auto index = ++_dialogIndex;
	const auto info = _dialogsInfo.item(index);
	if (info) {
		_api.requestMessages(*info, [=](const Data::DialogInfo &info) {
			if (ioCatchError(_writer->writeDialogStart(info))) {
				return false;
			}
			_messagesWritten = 0;
			_messagesCount = ranges::accumulate(
				info.messagesCountPerSplit,
				0);
			setState(stateDialogs(DownloadProgress()));
			if (_settings.singlePeerFrom || _settings.singlePeerTill) {
				_api.requestRangeTotal([=](int count) {
					_messagesCount = count;
					setState(stateDialogs(DownloadProgress()));
				});
			}
			return true;
		}, [=](DownloadProgress progress) {
			setState(stateDialogs(progress));
			return true;
		}, [=](Data::MessagesSlice &&result) {
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
				setState(stateDialogs(DownloadProgress()));
				if (int(_pendingSlice.list.size()) < kDialogWriteBatch) {
					return true;
				}
				return flushPendingSlice();
			}
			if (ioCatchError(_writer->writeDialogSlice(result))) {
				return false;
			}
			_messagesWritten += result.list.size();
			setState(stateDialogs(DownloadProgress()));
			return true;
		}, [=] {
			if (!flushPendingSlice()) {
				return;
			}
			if (ioCatchError(_writer->writeDialogEnd())) {
				return;
			}
			exportNextDialog();
		});
		return;
	}
	if (ioCatchError(_writer->writeDialogsEnd())) {
		return;
	}
	exportNext();
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
	result.itemIndex = _messagesWritten + progress.itemIndex;
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

	if (ioCatchError(_writer->writeDialogStart(topicInfo))) {
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
			if (ioCatchError(_writer->writeDialogEnd())) {
				return;
			}
			if (ioCatchError(_writer->finish())) {
				return;
			}
			if (ioCatchError(writeStatsFile())) {
				return;
			}
			if (ioCatchError(writeLinksFile())) {
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
	auto state = FinishedState();
	state.path = _writer->mainFilePath();
	for (auto i = 0; i != Output::Stats::kGroups; ++i) {
		const auto type = Output::Stats::kGroupStats[i].type;
		state.groupFiles[i] = _stats.typeFiles(type);
		state.groupBytes[i] = _stats.typeBytes(type);
		state.groupSkipped[i] = _stats.typeSkipped(type);
		state.groupSkippedBytes[i] = _stats.typeSkippedBytes(type);
		if (type == MediaSettings::Type::Poll) {
			continue;
		}
		state.filesCount += int(state.groupFiles[i]);
		state.bytesCount += state.groupBytes[i];
		state.skippedFiles += state.groupSkipped[i];
		state.skippedBytes += state.groupSkippedBytes[i];
	}
	state.textMessages = _stats.textMessages();
	state.linkMessages = _stats.linkMessages();
	state.linkTotal = _stats.linkTotal();
	state.linkDuplicates = _stats.linkDuplicates();
	state.messagesTotal = _stats.messagesTotal();
	setState(std::move(state));
}

constexpr auto kDiagBuild = 10;

Output::Result ControllerObject::writeStatsFile() const {
	const auto path = _settings.path + QString::fromLatin1("stats.txt");
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		return Output::Result(Output::Result::Type::Error, path);
	}
	auto stream = QTextStream(&file);
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
	stream << "Total unique files: " << totalFiles << " ("
		<< Ui::FormatSizeText(totalBytes) << "), Total duplicates: "
		<< skippedFiles << " ("
		<< Ui::FormatSizeText(skippedBytes) << ")\n\n";
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
		stream << group.name << ": " << files
			<< " ("
			<< Ui::FormatSizeText(_stats.typeBytes(group.type)) << ')';
		if (skipped > 0) {
			stream << ", Dups: " << skipped << ", ("
				<< Ui::FormatSizeText(_stats.typeSkippedBytes(group.type))
				<< ')';
		}
		stream << '\n';
	}
	if (_stats.linkMessages()) {
		stream << "Links: " << _stats.linkTotal() << " ("
			<< (_stats.linkTotal() - _stats.linkDuplicates()) << ')';
		if (const auto dup = _stats.linkDuplicates()) {
			stream << ", Duplicates: " << dup;
		}
		stream << '\n';
	}
	for (auto i = 0; i != Output::Stats::kGroups; ++i) {
		if (Output::Stats::kGroupStats[i].type
			!= MediaSettings::Type::Poll) {
			continue;
		}
		stream << Output::Stats::kGroupStats[i].name << ": "
			<< _stats.typeFiles(Output::Stats::kGroupStats[i].type) << '\n';
	}
	if (const auto text = _stats.textMessages()) {
		stream << "Text messages: " << text << '\n';
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
		const Environment &environment) {
	LOG(("Export Info: Started export to '%1'.").arg(settings.path));

	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.startExport(settings, environment);
	});
}

void Controller::skipFile(uint64 randomId) {
	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.skipFile(randomId);
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

rpl::lifetime &Controller::lifetime() {
	return _lifetime;
}

Controller::~Controller() {
	LOG(("Export Info: Controller destroyed."));
}

} // namespace Export