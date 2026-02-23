/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/export_controller.h"

#include <memory>
#include <QStringLiteral>

#include "export/export_api_wrap.h"
#include "export/export_settings.h"
#include "export/data/export_data_types.h"
#include "export/output/export_output_abstract.h"
#include "export/output/export_output_result.h"
#include "export/output/export_output_stats.h"
#include "mtproto/mtp_instance.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/show.h"
#include "ui/text/format_values.h"
#include "lang/lang_keys.h"
#include "base/weak_ptr.h"

#include <QtCore/QFile>
#include <QtCore/QTextStream>

namespace Export {
namespace {

const auto kNullStateCallback = [](ProcessingState&) {};

void WriteScanStatsFile(
		const QString &path,
		const std::map<MediaSettings::Type, Output::StatItem> &stats,
		int64 chatId,
		const QString &chatName) {
	if (!QDir().mkpath(path)) {
		return;
	}
	auto sanitizedName = chatName;
	sanitizedName.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
	const auto fileName = "scan_results_" + QString::number(chatId) + "_" + sanitizedName + ".txt";
	QFile file(path + fileName);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		return;
	}

	QTextStream out(&file);
	out << "Export Scan Results\n";
	out << "-------------------\n";

	using Type = MediaSettings::Type;
	const std::vector<Type> order = {
		Type::Photo, Type::Video, Type::VideoMessage, Type::Audio,
		Type::VoiceMessage, Type::File, Type::Sticker, Type::GIF,
		Type::Text, Type::Link
	};

	int categoriesCount = 0;
	int totalUniqueMessagesCount = 0;
	int totalTotalMessagesCount = 0;
	int64 totalUniqueMediaSize = 0;
	int64 totalMediaSize = 0;

	for (const auto type : order) {
		const auto it = stats.find(type);
		if (it == stats.end() || it->second.totalCount <= 0) {
			continue;
		}
		const auto &item = it->second;
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
		if (label.isEmpty()) continue;

		categoriesCount++;

		if (type == Type::Text) {
			out << label << ": " << item.totalCount << "\n";
		} else if (type == Type::Link) {
			if (item.uniqueCount == item.totalCount) {
				out << label << ": " << item.uniqueCount << "\n";
			} else {
				out << label << ": " << item.uniqueCount << ", " << item.totalCount << "\n";
			}
		} else {
			const auto uniqueStr = QString::number(item.uniqueCount) + " (" + Ui::FormatSizeText(item.uniqueSize) + ")";
			const auto totalStr = QString::number(item.totalCount) + " (" + Ui::FormatSizeText(item.totalSize) + ")";
			out << label << ": " << uniqueStr << ", " << totalStr << "\n";
			totalUniqueMediaSize += item.uniqueSize;
			totalMediaSize += item.totalSize;
		}

		if (type != Type::Link && type != Type::Text) {
			totalUniqueMessagesCount += item.uniqueCount;
			totalTotalMessagesCount += item.totalCount;
		}
	}

	if (categoriesCount > 1 && totalTotalMessagesCount > 0) {
		const auto uniqueStr = QString::number(totalUniqueMessagesCount) + " (" + Ui::FormatSizeText(totalUniqueMediaSize) + ")";
		const auto totalStr = QString::number(totalTotalMessagesCount) + " (" + Ui::FormatSizeText(totalMediaSize) + ")";
		out << "\nTotal media files: " << uniqueStr << ", " << totalStr << "\n";
	} else if (totalTotalMessagesCount <= 0 && categoriesCount > 0) {
		out << "\nNo items found in this range.\n";
	} else if (categoriesCount <= 0) {
		out << "\nNo messages found in this range.\n";
	}
}

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
		const MTPInputPeer &peer,
		const QString &name,
		int64 id);

	rpl::producer<State> state() const;

	// Password step.
	//void submitPassword(const QString &password);
	//void requestPasswordRecover();
	//rpl::producer<PasswordUpdate> passwordUpdate() const;
	//void reloadPasswordState();
	//void cancelUnconfirmedPassword();

	// Processing step.
	void runScan(
		const Settings &settings,
		const Environment &environment);
	void startExport(
		const Settings &settings,
		const Environment &environment);
	void skipFile(uint64 randomId);
	void cancelExportFast(bool keepCache = false);
	void clearResults();

	// Message export functions
	void exportNextDialog();
	void startExportMessages(const Data::DialogInfo *info, uint64 fromId, uint64 tillId);

private:
	using Step = ProcessingState::Step;
	using DownloadProgress = ApiWrap::DownloadProgress;

	base::flat_map<uint64, FileDownloadProgress> _activeDownloads;

	[[nodiscard]] bool stopped() const;
	void setState(State &&state);
	void ioError(const QString &path);
	bool ioCatchError(Output::Result result);
	void setFinishedState();

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
	void exportContacts();
	void exportSessions();
	void exportOtherData();
	void exportDialogs();

	template <typename Callback = const decltype(kNullStateCallback) &>
	ProcessingState prepareState(
		Step step,
		Callback &&callback = kNullStateCallback) const;
	ProcessingState stateInitializing() const;
	ProcessingState stateDialogsList(int processed) const;
	ProcessingState statePersonalInfo() const;
	ProcessingState stateUserpics(const DownloadProgress &progress) const;
	ProcessingState stateStories(const DownloadProgress &progress) const;
	ProcessingState stateContacts() const;
	ProcessingState stateSessions() const;
	ProcessingState stateOtherData() const;
	ProcessingState stateScanning(int itemIndex, int itemCount) const;
	ProcessingState stateDialogs(const DownloadProgress &progress) const;
	void fillMessagesState(
		ProcessingState &result,
		const Data::DialogsInfo &info,
		int index,
		const DownloadProgress &progress) const;

	int substepsInStep(Step step) const;

	ApiWrap _api;
	Settings _settings;
	Environment _environment;

	bool _isScanning = false;
	bool _scanStatsFound = false;
	Output::Stats _scanStats;

	Data::DialogsInfo _dialogsInfo;
	int _dialogIndex = -1;

	int _messagesWritten = 0;
	int _messagesCount = 0;
	int _messagesTextCount = 0;
	int _messagesMediaCount = 0;
	int _messagesTotalCount = 0;
	int _messagesTextTotal = 0;
	int _userpicsWritten = 0;
	int _userpicsCount = 0;

	int _storiesWritten = 0;
	int _storiesCount = 0;

	int _contentFilesCount = 0;
	bool _messagesInRangeCountFixed = false;

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

	rpl::lifetime _lifetime;

};

ControllerObject::ControllerObject(
	crl::weak_on_queue<ControllerObject> weak,
	QPointer<MTP::Instance> mtproto,
	const MTPInputPeer &peer,
	const QString &name,
	int64 id)
: _api(mtproto, weak.runner())
, _state(PasswordCheckState{}) {
	_api.errors(
	) | rpl::start_with_next([=](const MTP::Error &error) {
		setState(ApiErrorState{ error });
	}, _lifetime);

	_api.ioErrors(
	) | rpl::start_with_next([=](const Output::Result &result) {
		ioCatchError(result);
	}, _lifetime);

	//requestPasswordState();
	auto state = PasswordCheckState();
	state.checked = false;
	state.requesting = false;
	state.singlePeer = peer;
	state.singlePeerName = name;
	state.singlePeerId = id;
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
		|| v::is<ValueErrorState>(_state)
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

void ControllerObject::clearResults() {
	_scanStats.clear();
	_scanStatsFound = false;
	_messagesCount = 0;
	_stats.clear();
	_api.clearState(false);
	_dialogIndex = -1;
}

void ControllerObject::runScan(
		const Settings &settings,
		const Environment &environment) {
	if (_isScanning) {
		return;
	}
	_api.clearResults();
	clearResults();
	_isScanning = true;
	_stepIndex = -1;
	_settings = NormalizeSettings(settings);
	_settings.path = Output::NormalizePath(_settings);
	_environment = environment;

	_writer = Output::CreateNullWriter();

	_steps.clear();
	_steps.push_back(Step::Initializing);
	if (_settings.types & Settings::Type::AnyChatsMask) {
		_steps.push_back(Step::DialogsList);
		_steps.push_back(Step::Scanning);
	}

	exportNext();
}

void ControllerObject::startExport(
		const Settings &settings,
		const Environment &environment) {
	_api.clearResults();
	_stepIndex = -1;
	_dialogIndex = -1;

	_messagesInRangeCountFixed = true;
	_settings = NormalizeSettings(settings);
	_environment = environment;

	_stats.clear();
	int totalTotalFilesCount = 0;
	int totalMessagesFound = 0;
	using MediaType = MediaSettings::Type;
	for (const auto &pair : _scanStats.byType()) {
		const auto type = pair.first;
		const auto &item = pair.second;
		
		// If Full History is selected, add up all media/text items
		if (_settings.media.types & MediaType::FullHistory) {
			totalTotalFilesCount += item.totalCount;
		} else if (_settings.media.types & type) {
			// If specific filter selected, add those counts
			totalTotalFilesCount += item.totalCount;
		}
		
		if (type != MediaType::Link) {
			totalMessagesFound += item.totalCount;
		}
	}
	
	if (totalTotalFilesCount > 0) {
		// totalTotalFilesCount is the sum of category matches.
		// We use this as the main progress denominator.
		_stats.setExpectedFilesCount(totalTotalFilesCount);
		_messagesCount = totalTotalFilesCount;
		_scanStatsFound = true;
	} else {
		_messagesCount = 0;
		_stats.setExpectedFilesCount(0);
		_scanStatsFound = false;
	}

	_isScanning = false;
	_messagesWritten = 0;
	_messagesTextCount = 0;
	_messagesMediaCount = 0;
	_messagesTotalCount = 0;
	_messagesTextTotal = 0;
	_userpicsWritten = 0;
	_userpicsCount = 0;
	_storiesWritten = 0;
	_storiesCount = 0;
	_contentFilesCount = 0;

	_settings.path = Output::NormalizePath(_settings);
	_writer = Output::CreateWriter(_settings.format);
	fillExportSteps();
	exportNext();
}

void ControllerObject::skipFile(uint64 randomId) {
	if (stopped()) {
		return;
	}
	_activeDownloads.remove(randomId);
	_api.skipFile(randomId);
}

void ControllerObject::fillExportSteps() {
	using Type = Settings::Type;
	_steps.push_back(Step::Initializing);
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
	if (_isScanning) {
		push(Step::Scanning, info.dialogsCount);
	}
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
	_substepsInStep = std::move(result);
	_substepsTotal = ranges::accumulate(_substepsInStep, 0);
}

void ControllerObject::cancelExportFast(bool keepCache) {
	_isScanning = false;
	_api.clearState(keepCache);
	clearResults();
	setState(CancelledState());
}

void ControllerObject::exportNext() {
	if (++_stepIndex >= _steps.size()) {
		if (_isScanning) {
			auto stats = _scanStats.byType();
			WriteScanStatsFile(_settings.path, stats, _settings.singlePeerId, _settings.singlePeerName);
			_isScanning = false;
			_stepIndex = -1;
			setState(ScanDoneState{ std::move(stats) });
			return;
		}
		if (ioCatchError(_writer->finish())) {
			return;
		}
		(void)_writer->writeUniqueLinks(_api.visitedLinks());
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
	case Step::Contacts: return exportContacts();
	case Step::Sessions: return exportSessions();
	case Step::OtherData: return exportOtherData();
	case Step::Scanning:
	case Step::Dialogs: return exportDialogs();
	}
	Unexpected("Step in ControllerObject::exportNext.");
}

void ControllerObject::initialize() {
	setState(stateInitializing());
	_api.startExport(_settings, &_stats, [=](ApiWrap::StartInfo info) {
		initialized(info);
	}, _isScanning, &_scanStats);
}

void ControllerObject::initialized(const ApiWrap::StartInfo &info) {
	if (ioCatchError(_writer->start(_settings, _environment, &_stats))) {
		return;
	}
	fillSubstepsInSteps(info);
	if (_isScanning) {
		_messagesCount = info.serverTotalCount;
		_messagesInRangeCountFixed = true;
	}
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
		if (progress.total > 0 && progress.ready >= progress.total) {
			_activeDownloads.remove(progress.randomId);
			if (!progress.isAuxiliary) {
				_contentFilesCount++;
			}
		} else if (progress.total > 0) {
			_activeDownloads[progress.randomId] = {
				progress.randomId,
				progress.path,
				progress.ready,
				progress.total
			};
		}
		setState(stateUserpics(progress));
		return true;
	}, [=](Data::UserpicsSlice &&slice) {
		if (ioCatchError(_writer->writeUserpicsSlice(slice))) {
			return false;
		}
		_userpicsWritten += slice.list.size();
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
		if (progress.total > 0 && progress.ready >= progress.total) {
			_activeDownloads.remove(progress.randomId);
			if (!progress.isAuxiliary) {
				_contentFilesCount++;
			}
		} else if (progress.total > 0) {
			_activeDownloads[progress.randomId] = {
				progress.randomId,
				progress.path,
				progress.ready,
				progress.total
			};
		}
		setState(stateStories(progress));
		return true;
	}, [=](Data::StoriesSlice &&slice) {
		if (ioCatchError(_writer->writeStoriesSlice(slice))) {
			return false;
		}
		_storiesWritten += slice.list.size();
		return true;
	}, [=] {
		if (ioCatchError(_writer->writeStoriesEnd())) {
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

void ControllerObject::exportNextDialog() {
	const auto index = ++_dialogIndex;
	const auto info = _dialogsInfo.item(index);
	if (!info) {
		if (ioCatchError(_writer->writeDialogsEnd())) {
			return;
		}
		exportNext();
		return;
	}

	const auto fromId = _settings.useIdRange
		? _settings.singlePeerFromId
		: 0;
	auto tillId = _settings.useIdRange
		? _settings.singlePeerTillId
		: 0;

	if (tillId > 0 && tillId > info->topMessageId) {
		// Automatically correct tillId to the last message ID instead of showing an error
		tillId = info->topMessageId;
	}
	startExportMessages(info, fromId, tillId);
}

void ControllerObject::startExportMessages(const Data::DialogInfo *info, uint64 fromId, uint64 tillId) {
	if (!_messagesCount || !_scanStatsFound) {
		int count = 0;
		for (int splitCount : info->messagesCountPerSplit) {
			count += splitCount;
		}
		_messagesCount = count;
	}

	_api.requestMessages(*info, fromId, tillId, [=](const Data::DialogInfo &info) {
		if (ioCatchError(_writer->writeDialogStart(info))) {
			return false;
		}
		_messagesWritten = 0;
		if (!_stats.expectedFilesCount()) {
			_stats.setExpectedFilesCount(_messagesCount);
		}
		// Reset counters for new dialog
		_messagesTextCount = 0;
		_messagesMediaCount = 0;
		_messagesTotalCount = 0;

		setState(stateDialogs(DownloadProgress{
			.messagesTotalCount = 0,
		}));
		return true;
	}, [=](DownloadProgress progress) {
		if (progress.total > 0 && progress.ready >= progress.total) {
			_activeDownloads.remove(progress.randomId);
			if (!progress.isAuxiliary) {
				_contentFilesCount++;
			}
		} else if (progress.total > 0) {
			_activeDownloads[progress.randomId] = {
				progress.randomId,
				progress.path,
				progress.ready,
				progress.total
			};
		}
		_messagesTextCount = progress.messagesTextCount;
		_messagesMediaCount = progress.messagesMediaCount;
		_messagesTotalCount = progress.messagesTotalCount;
		_messagesTextTotal = progress.messagesTextTotal;
		if (_isScanning) {
			setState(stateScanning(progress.itemIndex, _messagesTextTotal));
		} else {
			setState(stateDialogs(progress));
		}
		return true;
	}, [=](Data::MessagesSlice &&result) {
		if (ioCatchError(_writer->writeDialogSlice(result))) {
			return false;
		}
		_messagesWritten += result.list.size();
		return true;
	}, [=] {
		if (ioCatchError(_writer->writeDialogEnd())) {
			return;
		}
		exportNextDialog();
	});
}

ProcessingState ControllerObject::stateInitializing() const {
	return prepareState(Step::Initializing);
}

ProcessingState ControllerObject::stateDialogsList(int processed) const {
	return prepareState(Step::DialogsList, [=](ProcessingState &result) {
		result.itemIndex = processed;
	});
}

ProcessingState ControllerObject::statePersonalInfo() const {
	return prepareState(Step::PersonalInfo);
}

ProcessingState ControllerObject::stateUserpics(const DownloadProgress &progress) const {
	return prepareState(Step::Userpics, [=](ProcessingState &result) {
		result.itemIndex = progress.itemIndex;
		result.itemCount = progress.total;
	});
}

ProcessingState ControllerObject::stateStories(const DownloadProgress &progress) const {
	return prepareState(Step::Stories, [=](ProcessingState &result) {
		result.itemIndex = progress.itemIndex;
		result.itemCount = progress.total;
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

ProcessingState ControllerObject::stateScanning(int itemIndex, int itemCount) const {
	return prepareState(Step::Scanning, [=](ProcessingState &result) {
		result.itemIndex = itemIndex;
		result.itemCount = itemCount;
	});
}

ProcessingState ControllerObject::stateDialogs(const DownloadProgress &progress) const {
	return prepareState(Step::Dialogs, [=](ProcessingState &result) {
		fillMessagesState(
			result,
			_dialogsInfo,
			_dialogIndex,
			progress);
		result.selectedStats = _stats.byType();
	});
}

void ControllerObject::setFinishedState() {
	auto totalUniqueCount = 0;
	auto totalUniqueSize = int64(0);
	auto totalTotalCount = 0;
	auto totalTotalSize = int64(0);
	const auto breakdown = _stats.byType();
	using Type = MediaSettings::Type;
	const bool linksOnly = (_settings.media.types == Type::Link);
	(void)linksOnly;

	for (const auto &pair : breakdown) {
		const auto type = pair.first;
		const auto &item = pair.second;
		if (type != Type::Link && type != Type::Text) {
			totalUniqueCount += item.uniqueCount;
			totalUniqueSize += item.uniqueSize;
			totalTotalCount += item.totalCount;
			totalTotalSize += item.totalSize;
		}
	}

	setState(FinishedState{
		.path = _writer->mainFilePath(),
		.totalUniqueCount = totalUniqueCount,
		.totalUniqueSize = totalUniqueSize,
		.totalTotalCount = totalTotalCount,
		.totalTotalSize = totalTotalSize,
		.fullHistory = !!(_settings.media.types & Type::FullHistory),
		.fullRange = (_settings.singlePeerFrom == 0 && _settings.singlePeerTill == 0) && !_settings.useIdRange,
		.breakdown = breakdown,
	});
}

template <typename Callback>
ProcessingState ControllerObject::prepareState(
		Step step,
		Callback &&callback) const {
	auto result = ProcessingState();
	result.step = step;
	result.isScanning = _isScanning;

	if (_lastProcessingStep != step) {
		_substepsPassed += substepsInStep(_lastProcessingStep);
		_lastProcessingStep = step;
	}
	result.substepsPassed = _substepsPassed;
	result.substepsNow = substepsInStep(step);
	result.substepsTotal = _substepsTotal;

	callback(result);
	return result;
}

int ControllerObject::substepsInStep(Step step) const {
	const auto index = static_cast<int>(step);
	return (index >= 0 && index < _substepsInStep.size())
		? _substepsInStep[index]
		: 0;
}

void ControllerObject::fillMessagesState(
		ProcessingState &result,
		const Data::DialogsInfo &info,
		int index,
		const DownloadProgress &progress) const {
	const auto i = info.item(index);
	Assert(i != nullptr);

	switch (i->type) {
	case Data::DialogInfo::Type::Self:
	case Data::DialogInfo::Type::Replies:
		result.entityType = ProcessingState::EntityType::SavedMessages;
		break;
	case Data::DialogInfo::Type::Personal:
	case Data::DialogInfo::Type::Bot:
	case Data::DialogInfo::Type::PrivateGroup:
	case Data::DialogInfo::Type::PrivateSupergroup:
	case Data::DialogInfo::Type::PublicSupergroup:
	case Data::DialogInfo::Type::PrivateChannel:
	case Data::DialogInfo::Type::PublicChannel:
		result.entityType = ProcessingState::EntityType::Chat;
		break;
	case Data::DialogInfo::Type::VerifyCodes:
		result.entityType = ProcessingState::EntityType::VerifyCodes;
		break;
	default:
		result.entityType = ProcessingState::EntityType::Other;
		break;
	}

	result.entityName = i->name;
	result.entityIndex = index + 1;
	result.entityCount = info.chats.size() + info.left.size();
	
	result.itemIndex = progress.itemIndex;
	result.itemCount = (_messagesCount > 0) ? _messagesCount : progress.messagesTextTotal;

	result.activeDownloads = _activeDownloads;
}


Controller::Controller(
	QPointer<MTP::Instance> mtproto,
	const MTPInputPeer &peer,
	const QString &name,
	int64 id)
: _wrapped(std::move(mtproto), peer, std::move(name), std::move(id)) {
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

void Controller::runScan(
		const Settings &settings,
		const Environment &environment) {
	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.runScan(settings, environment);
	});
}

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

void Controller::cancelExportFast(bool keepCache) {
	LOG(("Export Info: Cancelled export."));

	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.cancelExportFast(keepCache);
	});
}

void Controller::clearResults() {
	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.clearResults();
	});
}

rpl::lifetime &Controller::lifetime() {
	return _lifetime;
}

Controller::~Controller() {
	LOG(("Export Info: Controller destroyed."));
}

} // namespace Export