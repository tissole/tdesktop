/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/export_controller.h"

#include <memory>
#include <QStringLiteral>

#include "export/view/export_view_settings.h"
#include "export/export_api_wrap.h"
#include "export/export_settings.h"
#include "export/export_progress.h"
#include "export/output/export_output_abstract.h"
#include "export/data/export_data_types.h"
#include "export/output/export_output_result.h"
#include "export/output/export_output_stats.h"
#include <QtCore/QFile>
#include <QtCore/QDir>
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
		const Settings &settings,
		const std::map<MediaSettings::Type, Output::StatItem> &stats) {
	if (!QDir().mkpath(settings.path)) {
		return;
	}
	auto sanitizedName = settings.singlePeerName;
	sanitizedName.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
	const auto fileName = "scan_results_" + QString::number(settings.singlePeerId) + "_" + sanitizedName + ".txt";
	QFile file(settings.path + fileName);
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

	const auto selected = settings.media.types;
	const bool fullHistory = (selected & Type::FullHistory);

	for (const auto type : order) {
		const bool isSelected = fullHistory || (selected & type);
		if (!isSelected) {
			continue;
		}

		const auto it = stats.find(type);
		if (it == stats.end() || it->second.localTotalCount <= 0) {
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
			out << label << ": " << item.localTotalCount << "\n";
		} else if (type == Type::Link) {
			const auto messagesStr = (item.messagesWithLinks > 0)
				? " (" + QString::number(item.messagesWithLinks) + " Messages)"
				: QString();
			out << label << ": " << item.uniqueCount << ", " << item.localTotalCount << messagesStr << "\n";
		} else {
			const auto uniqueStr = QString::number(item.uniqueCount) + " (" + Ui::FormatSizeText(item.uniqueSize) + ")";
			if (item.localTotalCount == item.uniqueCount && item.totalSize == item.uniqueSize) {
				out << label << ": " << uniqueStr << "\n";
			} else {
				const auto totalStr = QString::number(item.localTotalCount) + " (" + Ui::FormatSizeText(item.totalSize) + ")";
				out << label << ": " << uniqueStr << ", " << totalStr << "\n";
			}
			totalUniqueMediaSize += item.uniqueSize;
			totalMediaSize += item.totalSize;
		}

		if (type != Type::Link && type != Type::Text) {
			totalUniqueMessagesCount += item.uniqueCount;
			totalTotalMessagesCount += item.localTotalCount;
		}
	}

	if (categoriesCount > 1 && totalTotalMessagesCount > 0) {
		const auto uniqueStr = QString::number(totalUniqueMessagesCount) + " (" + Ui::FormatSizeText(totalUniqueMediaSize) + ")";
		if (totalTotalMessagesCount == totalUniqueMessagesCount && totalMediaSize == totalUniqueMediaSize) {
			out << "\nTotal Media: " << uniqueStr << "\n";
		} else {
			const auto totalStr = QString::number(totalTotalMessagesCount) + " (" + Ui::FormatSizeText(totalMediaSize) + ")";
			out << "\nTotal Media: " << uniqueStr << ", " << totalStr << "\n";
		}
	} else if (categoriesCount <= 0) {
		out << "\nNo messages found in this range.\n";
	}
}

void WriteUniqueLinksFile(
		const Settings &settings,
		const base::flat_set<QString> &links) {
	if (links.empty()) {
		return;
	}
	if (!QDir().mkpath(settings.path)) {
		return;
	}
	auto sanitizedName = settings.singlePeerName;
	sanitizedName.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
	const auto fileName = "unique_links_" + QString::number(settings.singlePeerId) + "_" + sanitizedName + ".txt";
	QFile file(settings.path + fileName);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		return;
	}
	QTextStream out(&file);
	for (const auto &link : links) {
		out << link << "\n";
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
	void runScan(
		const Export::Settings &settings,
		const Export::Environment &environment);
	void runUpdateScan(
		const Export::Settings &settings,
		const Export::Environment &environment,
		const QString &folderPath);
	void startExport(
		const Export::Settings &settings,
		const Export::Environment &environment);
	void resumeExport(
		const Export::Settings &settings,
		const Export::Environment &environment);
	void checkExistingExport(std::function<void(Export::View::SettingsWidget::ExistingExport)> callback) const;
	void skipFile(uint64 randomId);
	void cancelExportFast(bool keepCache = false);
	void clearResults();


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
	void exportProfileMusic();
	void exportContacts();
	void exportSessions();
	void exportOtherData();
	void exportDialogs();
	void exportNextDialog();
	void startExportMessages(const Data::DialogInfo *info, uint64 fromId, uint64 tillId);
	void exportTopic();

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
	ProcessingState stateScanning(int itemIndex, int itemCount) const;
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

	bool _isScanning = false;
	bool _scanStatsFound = false;
	Output::Stats _scanStats;

	Data::DialogsInfo _dialogsInfo;
	int _dialogIndex = -1;

	int _messagesWritten = 0;
	int _messagesCount = 0;
	int _messagesTotalCount = 0;
	int _userpicsWritten = 0;
	int _userpicsCount = 0;
	int _storiesWritten = 0;
	int _storiesCount = 0;
	int _contentFilesCount = 0;
	bool _messagesInRangeCountFixed = false;
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
: _api(mtproto, weak.runner(), mtproto ? mtproto->mainDcId() : 0)
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
: _api(mtproto, weak.runner(), mtproto ? mtproto->mainDcId() : 0)
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
	_api.cancelExportFast(false);
	_dialogIndex = -1;
	_state = PasswordCheckState();
}

void ControllerObject::runScan(
		const Settings &settings,
		const Environment &environment) {
	if (_isScanning) {
		return;
	}
	// Reset any terminal state so setState() calls during scan are not blocked.
	if (stopped()) {
		_state = v::null;
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

void ControllerObject::runUpdateScan(
		const Settings &settings,
		const Environment &environment,
		const QString &folderPath) {
	if (_isScanning) {
		return;
	}
	if (stopped()) {
		_state = v::null;
	}
	_api.clearResults();
	clearResults();
	_isScanning = true;
	_stepIndex = -1;

	// Load progress to get last message ID
	const auto progressPath = ExportProgress::progressFilePath(folderPath);
	auto progress = ExportProgress::load(progressPath);
	uint64 lastId = 0;
	if (progress) {
		lastId = progress->lastMessageId;
		// Keep existing settings but move the range to start after lastId
		_settings = progress->settings;
	} else {
		_settings = NormalizeSettings(settings);
	}
	
	_settings.path = folderPath;
	_settings.useIdRange = true;
	_settings.singlePeerFromId = lastId;
	_settings.singlePeerTillId = 0; // To current latest
	
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
	// Reset any terminal state (CancelledState, FinishedState, error states) so that
	// setState() calls during the export are not blocked by stopped() returning true.
	if (stopped()) {
		_state = v::null;
	}
	_api.clearResults();
	_stepIndex = -1;
	_dialogIndex = -1;

	_messagesInRangeCountFixed = true;
	_settings = NormalizeSettings(settings);
	_environment = environment;
	_settings.singleTopicRootId = _topicRootId;
	_settings.singleTopicPeerId = _topicPeerId;

	_stats.clear();
	using MediaType = MediaSettings::Type;
	const bool fullHistoryMode = (_settings.media.types & MediaType::FullHistory);
	int totalMediaFilesCount = 0; // media-only count for file download progress

	for (const auto &pair : _scanStats.byType()) {
		const auto type = pair.first;
		const auto &item = pair.second;
		const bool isDownloadable = (type != MediaType::Text && type != MediaType::Link);
		const bool selected = fullHistoryMode
			? isDownloadable
			: (bool)(_settings.media.types & type);
		if (selected && isDownloadable) {
			totalMediaFilesCount += item.localTotalCount;
		}
	}

	if (totalMediaFilesCount > 0 || _scanStats.localTotalCount() > 0) {
		_stats.setExpectedFilesCount(totalMediaFilesCount);
		_scanStatsFound = true;
	} else {
		_messagesCount = 0;
		_stats.setExpectedFilesCount(0);
		_scanStatsFound = false;
	}

	_isScanning = false;
	_messagesWritten = 0;
	_messagesTotalCount = 0;
	_userpicsWritten = 0;
	_userpicsCount = 0;
	_storiesWritten = 0;
	_storiesCount = 0;
	_contentFilesCount = 0;

	_settings.path = Output::NormalizePath(_settings);
	_writer = Output::CreateWriter(_settings.format);
	_steps.clear();
	fillExportSteps();
	exportNext();
}

void ControllerObject::resumeExport(
		const Settings &settings,
		const Environment &environment) {
	// Reset any terminal state
	if (stopped()) {
		_state = State();
	}
	_api.clearResults();
	_stepIndex = -1;
	_dialogIndex = -1;

	_messagesInRangeCountFixed = true;
	_settings = NormalizeSettings(settings);
	_environment = environment;
	_settings.singleTopicRootId = _topicRootId;
	_settings.singleTopicPeerId = _topicPeerId;

	_stats.clear();
	using MediaType = MediaSettings::Type;
	const bool fullHistoryMode = (_settings.media.types & MediaType::FullHistory);
	int totalMediaFilesCount = 0;

	for (const auto &pair : _scanStats.byType()) {
		const auto type = pair.first;
		const auto &item = pair.second;
		const bool isDownloadable = (type != MediaType::Text && type != MediaType::Link);
		const bool selected = fullHistoryMode
			? isDownloadable
			: (bool)(_settings.media.types & type);
		if (selected && isDownloadable) {
			totalMediaFilesCount += item.localTotalCount;
		}
	}

	if (totalMediaFilesCount > 0 || _scanStats.localTotalCount() > 0) {
		_stats.setExpectedFilesCount(totalMediaFilesCount);
		_scanStatsFound = true;
	} else {
		_messagesCount = 0;
		_stats.setExpectedFilesCount(0);
		_scanStatsFound = false;
	}

	_isScanning = false;
	_messagesWritten = 0;
	_contentFilesCount = 0;

	// Note: _messagesCount will be restored in ApiWrap::startExport via progress.
	
	// Enable resume mode in ApiWrap
	_api.setResumeMode(true);

	_settings.path = Output::NormalizePath(_settings);

	_writer = Output::CreateWriter(_settings.format);
	_steps.clear();
	fillExportSteps();
	
	// Skip steps that were already processed before resume
	// We call exportNext() which will execute Step::Initializing.
	// Step::Initializing will call ApiWrap::startExport, which will load progress
	// and then call initialized(), which will correctly start the message export.
	exportNext();
}

void ControllerObject::checkExistingExport(std::function<void(Export::View::SettingsWidget::ExistingExport)> callback) const {
	using ExistingExport = Export::View::SettingsWidget::ExistingExport;
	if (!_settings.onlySinglePeer()) {
		// Also check the state for single peer info
		const auto *password = std::get_if<PasswordCheckState>(&_state);
		if (!password || password->singlePeer.type() == mtpc_inputPeerEmpty) {
			callback(ExistingExport::None);
			return;
		}
	}

	// Get the peer ID from _settings or fallback to _state
	int64 targetPeerId = _settings.singlePeerId;
	QString targetPeerName = _settings.singlePeerName;
	MTPInputPeer sourcePeer = _settings.singlePeer;
	
	if (targetPeerId == 0 && sourcePeer.type() == mtpc_inputPeerEmpty) {
		// Try to get from _state
		const auto *password = std::get_if<PasswordCheckState>(&_state);
		if (password) {
			targetPeerId = password->singlePeerId;
			targetPeerName = password->singlePeerName;
			sourcePeer = password->singlePeer;
		}
	}

	// If still no peer ID, try to extract from singlePeer
	if (targetPeerId == 0 && sourcePeer.type() != mtpc_inputPeerEmpty) {
		targetPeerId = sourcePeer.match([](const MTPDinputPeerUser &data) {
			return static_cast<int64>(data.vuser_id().v);
		}, [](const MTPDinputPeerUserFromMessage &data) {
			return static_cast<int64>(data.vuser_id().v);
		}, [](const MTPDinputPeerChat &data) {
			return static_cast<int64>(data.vchat_id().v);
		}, [](const MTPDinputPeerChannel &data) {
			// Channel IDs are stored as -(10^12 + channel_id)
			return -(1000000000000LL + static_cast<int64>(data.vchannel_id().v));
		}, [](const MTPDinputPeerChannelFromMessage &data) {
			return -(1000000000000LL + static_cast<int64>(data.vchannel_id().v));
		}, [&](const MTPDinputPeerSelf &data) {
			return int64(-1);
		}, [](const MTPDinputPeerEmpty &data) {
			return int64(0);
		});
	}

	if (targetPeerId == 0) {
		callback(ExistingExport::None);
		return;
	}

	// Build a temporary settings to get the base path
	auto tempSettings = Settings();
	tempSettings.singlePeer = sourcePeer;
	tempSettings.singlePeerId = targetPeerId;
	tempSettings.singlePeerName = targetPeerName;
	
	const auto basePath = Output::NormalizePath(tempSettings);
	if (basePath.isEmpty()) {
		callback(ExistingExport::None);
		return;
	}

	// Find parent directory (strip the subfolder name)
	const auto parentPath = [&] {
		const auto idx = basePath.lastIndexOf('/');
		return idx > 0 ? basePath.left(idx) : basePath;
	}();

	const auto checkInFolder = [&](const QString &folderPath) -> std::optional<ExistingExport> {
		const auto progressPath = ExportProgress::progressFilePath(folderPath);
		if (QFile::exists(progressPath)) {
			QFile file(progressPath);
			if (file.open(QIODevice::ReadOnly)) {
				const auto doc = QJsonDocument::fromJson(file.readAll());
				if (!doc.isNull() && doc.isObject()) {
					const auto progress = ExportProgress::fromJson(doc.object());
					using Type = MediaSettings::Type;
					const auto types = progress.settings.media.types;
					if (!(types & ~(Type::Text | Type::Link | Type::FullHistory))) {
						return ExistingExport::None;
					}
					if (progress.isComplete || (progress.rangeEndMsgId > 0 && progress.lastMessageId >= progress.rangeEndMsgId)) {
						return ExistingExport::Complete;
					}
					if (progress.lastMessageId > 0) {
						return ExistingExport::Incomplete;
					}
				}
			}
		}
		
		const QDir folderDir(folderPath);
		const auto partialFiles = folderDir.entryList(QStringList() << "*.partial", QDir::Files | QDir::NoDotAndDotDot);
		if (!partialFiles.isEmpty()) {
			return ExistingExport::Incomplete;
		}
		return std::nullopt;
	};

	// Search for any ChatExport folder containing this peer ID
	{
		const QDir parentDir(parentPath);
		const auto entries = parentDir.entryList(QStringList() << "ChatExport_*", QDir::Dirs | QDir::NoDotAndDotDot);
		const auto idStr = QString::number(targetPeerId);
		for (const auto &entry : entries) {
			if (entry.contains(idStr)) {
				if (const auto result = checkInFolder(parentPath + '/' + entry)) {
					callback(*result);
					return;
				}
			}
		}
	}

	// Fallback: check today's generated path
	if (const auto result = checkInFolder(basePath)) {
		callback(*result);
		return;
	}

	callback(ExistingExport::None);
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

void ControllerObject::cancelExportFast(bool keepCache) {
	_isScanning = false;
	// cancelExportFast sends FinishTakeoutSession to Telegram before clearing.
	// Using clearState() directly would abandon the takeout without closing it,
	// leaving it open on the server and stalling the next scan/export attempt.
	_api.cancelExportFast(keepCache);
	clearResults();
	setState(CancelledState());
}

void ControllerObject::exportNext() {
	if (++_stepIndex >= _steps.size()) {
		if (_isScanning) {
			auto stats = _scanStats.byType();
			const auto selected = _settings.media.types;
			const bool fullHistory = (selected & MediaSettings::Type::FullHistory);
			for (auto it = stats.begin(); it != stats.end();) {
				const bool isSelected = fullHistory || (selected & it->first);
				const bool hasAnyCount = (it->second.localTotalCount > 0) || (it->second.uniqueCount > 0);
				if (!isSelected || !hasAnyCount) {
					it = stats.erase(it);
				} else {
					++it;
				}
			}
			WriteScanStatsFile(_settings, stats);
			WriteUniqueLinksFile(_settings, _api.visitedLinks());
			_api.saveScanProgress();
			_isScanning = false;
			_stepIndex = -1;
			setState(ScanDoneState{ std::move(stats) });
			return;
		}
		if (ioCatchError(_writer->finish())) {
			return;
		}
		WriteUniqueLinksFile(_settings, _api.visitedLinks());
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
	case Step::Scanning:
	case Step::Dialogs: return exportDialogs();
	case Step::Topic: return exportTopic();
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

	if (const auto progress = _api.progress()) {
		if (!_isScanning && _api.isResumeMode()) {
			_messagesWritten = progress->messagesProcessed;
			if (progress->scanTotalMessages > 0) {
				_messagesCount = progress->scanTotalMessages;
				_messagesInRangeCountFixed = true;
			} else if (progress->messagesTotalCount > 0) {
				_messagesCount = progress->messagesTotalCount;
				_messagesInRangeCountFixed = true;
			}
		}
	}

	if (_isScanning) {
		if (info.serverCountIsAccurate && info.serverTotalCount > 0) {
			_messagesCount = info.serverTotalCount;
			_messagesInRangeCountFixed = true;
		}
	} else if (!_scanStatsFound && info.serverCountIsAccurate && info.serverTotalCount > 0) {
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
	// Skip recalculation if already fixed by initialized() from saved progress.
	// Otherwise we incorrectly use the full-chat count instead of the saved range count.
	if (_messagesInRangeCountFixed) {
		// _messagesCount and _messagesWritten already set by initialized().
	} else if (!_messagesCount || !_scanStatsFound) {
		// If scan was done, use the count of messages that were actually selected during scan.
		// This matches the date/size filter, unlike messagesCountPerSplit which is the full chat count.
		const int scanTotal = _scanStats.totalMessagesCount();
		if (_scanStatsFound && scanTotal > 0) {
			_messagesCount = scanTotal;
		} else {
			const bool hasRange = (fromId > 0 || tillId > 0);
			const bool fullHistoryMode = (_settings.media.types & MediaSettings::Type::FullHistory);
			const int serverRangeTotal = _stats.localTotalCount();
			if (hasRange && serverRangeTotal > 0 && !fullHistoryMode) {
				_messagesCount = serverRangeTotal;
			} else if (hasRange && fullHistoryMode) {
				_messagesCount = 0;
			} else {
				int count = 0;
				for (int splitCount : info->messagesCountPerSplit) {
					count += splitCount;
				}
				_messagesCount = count;
			}
		}
	}

	_api.requestMessages(*info, fromId, tillId, [=](const Data::DialogInfo &info) {
		if (ioCatchError(_writer->writeDialogStart(info))) {
			return false;
		}
		_messagesWritten = 0;
		if (!_stats.expectedFilesCount()) {
			_stats.setExpectedFilesCount(_messagesCount);
		}

		if (_messagesTotalCount > 0) {
			_messagesCount = _messagesTotalCount;
		}

		setState(stateDialogs(DownloadProgress{
			.messagesTotalCount = _messagesTotalCount,
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
		_messagesTotalCount = progress.messagesTotalCount;
	// If the server count was inaccurate (range active), update _messagesCount
		// from the actual range count as it accumulates from requestMessagesCount.
		// This replaces the full-chat count with the range-specific count.
		if (_messagesTotalCount > 0) {
			_messagesCount = _messagesTotalCount;
		}
		if (_isScanning) {
			setState(stateScanning(progress.itemIndex, _messagesTotalCount));
		} else {
			setState(stateDialogs(progress));
		}
		return true;
	}, [=](Data::MessagesSlice &&result) {
		if (ioCatchError(_writer->writeDialogSlice(result))) {
			return false;
		}
		_messagesWritten += result.list.size();
		
		// Update progress with last message ID for resume
		if (!result.list.empty()) {
			const auto lastMsgId = static_cast<uint64>(result.list.back().id);
			_api.updateMessageProgress(lastMsgId);
		}
		
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
		result.expectedStats = _scanStats.byType();
	});
}

void ControllerObject::setFinishedState() {
	auto totalUniqueCount = 0;
	auto totalUniqueSize = int64(0);
	auto totalTotalCount = 0;
	auto totalTotalSize = int64(0);
	using Type = MediaSettings::Type;

	// Use export stats if populated, else fall back to scan stats.
	auto exportBreakdown = _stats.byType();
	const bool exportStatsPopulated = std::any_of(
		exportBreakdown.begin(), exportBreakdown.end(),
		[](const auto &p) { return p.second.localTotalCount > 0; });
	auto scanBreakdownFull = _scanStats.byType();
	// Only use scan stats as fallback if they were gathered with the same settings
	// (same media types). If scan was full-history but export is selective, scan stats
	// have all types but we only want what the export actually processed.
	// Check: if export has non-empty stats but scan has MORE types, prefer export stats.
	const bool scanStatsPopulated = _scanStatsFound && std::any_of(
		scanBreakdownFull.begin(), scanBreakdownFull.end(),
		[](const auto &p) { return p.second.localTotalCount > 0; });
	auto breakdown = exportStatsPopulated
		? exportBreakdown
		: (scanStatsPopulated ? scanBreakdownFull : exportBreakdown);
	// Supplement export stats with scan data for any type that wasn't counted
	// during export (e.g. files skipped due to size limit).
	if (exportStatsPopulated && scanStatsPopulated) {
		for (const auto &[type, scanItem] : scanBreakdownFull) {
			auto it = breakdown.find(type);
			if (it == breakdown.end() || it->second.localTotalCount == 0) {
				breakdown[type] = scanItem;
			} else if (it->second.uniqueCount == 0 && scanItem.uniqueCount > 0) {
				// Export stats have localTotalCount (from server seed) but no uniqueCount/uniqueSize.
				// Use scan stats for the unique side so the finished view shows correct data.
				it->second.uniqueCount = scanItem.uniqueCount;
				it->second.uniqueSize = scanItem.uniqueSize;
			}
		}
	}

	// messagesWithLinks: copy from whichever stats have it (export first, then scan).
	int mwlForBreakdown = 0;
	const auto exportBreakdownForLinks = _stats.byType();
	const auto exportLinkIt = exportBreakdownForLinks.find(Type::Link);
	if (exportLinkIt != exportBreakdownForLinks.end())
		mwlForBreakdown = exportLinkIt->second.messagesWithLinks;
	if (mwlForBreakdown <= 0) {
		const auto scanBreakdown = _scanStats.byType();
		const auto scanLinkIt = scanBreakdown.find(Type::Link);
		if (scanLinkIt != scanBreakdown.end())
			mwlForBreakdown = scanLinkIt->second.messagesWithLinks;
	}
	if (mwlForBreakdown > 0)
		breakdown[Type::Link].messagesWithLinks = mwlForBreakdown;

	for (const auto &pair : breakdown) {
		const auto type = pair.first;
		const auto &item = pair.second;
		if (type != Type::Link && type != Type::Text) {
			totalUniqueCount += item.uniqueCount;
			totalUniqueSize += item.uniqueSize;
			totalTotalCount += item.localTotalCount;
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
	result.fullHistory = !!(_settings.media.types & MediaSettings::Type::FullHistory);

	if (_lastProcessingStep != step) {
		_substepsPassed += substepsInStep(_lastProcessingStep);
		_lastProcessingStep = step;
	}
	result.substepsPassed = _substepsPassed;
	result.substepsNow = substepsInStep(step);
	result.substepsTotal = _substepsTotal;

	result.selectedStats = _stats.byType();
	result.expectedStats = _scanStats.byType();

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
	// Always use total message count for the "X / Y" progress counter.
	// Using media file count from scan stats was wrong: it excluded Text and Link messages.
	result.itemCount = (_messagesCount > 0) ? _messagesCount : progress.messagesTotalCount;
	result.activeDownloads = _activeDownloads;

	result.selectedStats = _stats.byType();
	result.expectedStats = _scanStats.byType();
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
			if (ioCatchError(_writer->writeDialogSlice(slice))) {
				return false;
			}
			_messagesWritten += slice.list.size();
			setState(stateTopic(DownloadProgress()));
			return true;
		},
		[=] {
			if (ioCatchError(_writer->writeDialogEnd())) {
				return;
			}
			if (ioCatchError(_writer->finish())) {
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

void Controller::runScan(
		const Settings &settings,
		const Environment &environment) {
	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.runScan(settings, environment);
	});
}

void Controller::runUpdateScan(
		const Settings &settings,
		const Environment &environment,
		const QString &folderPath) {
	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.runUpdateScan(settings, environment, folderPath);
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

void Controller::resumeExport(
		const Settings &settings,
		const Environment &environment) {
	LOG(("Export Info: Resuming export from '%1'.").arg(settings.path));

	_wrapped.with([=](Implementation &unwrapped) {
		unwrapped.resumeExport(settings, environment);
	});
}

void Controller::checkExistingExport(std::function<void(Export::View::SettingsWidget::ExistingExport)> callback) const {
	_wrapped.with([callback = std::move(callback)](const Implementation &unwrapped) {
		unwrapped.checkExistingExport(std::move(callback));
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