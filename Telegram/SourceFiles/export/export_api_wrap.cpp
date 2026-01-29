/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/export_api_wrap.h"

#include "export/export_settings.h"
#include "export/data/export_data_types.h"
#include "export/output/export_output_result.h"
#include "export/output/export_output_file.h"
#include "export/output/export_output_stats.h"
#include "mtproto/mtproto_response.h"
#include "base/bytes.h"
#include "base/options.h"
#include "base/random.h"
#include "base/call_delayed.h"
#include <set>
#include <deque>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <algorithm>
#include <QRegularExpression>


namespace Export {
namespace {



constexpr auto kMaxParallelFiles = 4;
constexpr auto kMegabyte = 1024 * 1024;

// Rate limiting: Target 20 requests/sec for safety margin (one every 50ms)
// Version 1: Balanced increase for higher throughput.
constexpr auto kMinRequestIntervalMs = 1000 / 18;

// Transient retry settings (per-chunk).
constexpr auto kMaxChunkRetries = 3;
constexpr auto kRetryBaseDelayMs = 200;   // 200, 400, 800 ms
constexpr auto kRetryMaxDelayMs = 2000;   // clamp upper bound

int GetChunkSizeForFile(int64 fileSize) {
	return 1 * kMegabyte;
}

int GetConcurrentChunksForFile(int64 fileSize) {
	if (fileSize > 300 * kMegabyte) {
		return 2; // More concurrency for large files
	}
	return 2; // Less concurrency for smaller files
}


// Other slice limits.
constexpr auto kUserpicsSliceLimit = 100;
constexpr auto kChatsSliceLimit = 100;
constexpr auto kMessagesSliceLimit = 100;
constexpr auto kTopPeerSliceLimit = 100;
constexpr auto kFileMaxSize = 4000 * int64(1024 * 1024);
constexpr auto kLocationCacheSize = 1'000'000;
constexpr auto kMaxEmojiPerRequest = 100;
constexpr auto kStoriesSliceLimit = 100;

struct LocationKey {
	uint64 type;
	uint64 id;

	inline bool operator<(const LocationKey &other) const {
		return std::tie(type, id) < std::tie(other.type, other.id);
	}
};

LocationKey ComputeLocationKey(const Data::FileLocation &value) {
	auto result = LocationKey();
	result.type = value.dcId;
	value.data.match([&](const MTPDinputDocumentFileLocation &data) {
		const auto letter = data.vthumb_size().v.isEmpty()
			? char(0)
			: data.vthumb_size().v[0];
		result.type |= (2ULL << 24);
		result.type |= (uint64(uint32(letter)) << 16);
		result.id = data.vid().v;
	}, [&](const MTPDinputPhotoFileLocation &data) {
		const auto letter = data.vthumb_size().v.isEmpty()
			? char(0)
			: data.vthumb_size().v[0];
		result.type |= (6ULL << 24);
		result.type |= (uint64(uint32(letter)) << 16);
		result.id = data.vid().v;
	}, [&](const MTPDinputTakeoutFileLocation &data) {
		result.type |= (5ULL << 24);
	}, [](const auto &data) {
		Unexpected("File location type in Export::ComputeLocationKey.");
	});
	return result;
}

Settings::Type SettingsFromDialogsType(Data::DialogInfo::Type type) {
	using DialogType = Data::DialogInfo::Type;
	switch (type) {
	case DialogType::Self:
	case DialogType::Personal:
		return Settings::Type::PersonalChats;
	case DialogType::Bot:
		return Settings::Type::BotChats;
	case DialogType::PrivateGroup:
	case DialogType::PrivateSupergroup:
		return Settings::Type::PrivateGroups;
	case DialogType::PublicSupergroup:
		return Settings::Type::PublicGroups;
	case DialogType::PrivateChannel:
		return Settings::Type::PrivateChannels;
	case DialogType::PublicChannel:
		return Settings::Type::PublicChannels;
	}
	return Settings::Type(0);
}

} // namespace

ApiWrap::RequestThrottler::RequestThrottler(
	Fn<void(FnMut<void()>)> runner,
	std::shared_ptr<bool> guard)
: _runner(runner)
, _guard(std::move(guard)) {
}

ApiWrap::RequestThrottler::~RequestThrottler() = default;

void ApiWrap::RequestThrottler::schedule(FnMut<void()> task) {
	_runner([this, guard = _guard, task = std::move(task)]() mutable {
		if (!*guard) {
			return;
		}
		_taskQueue.push_back(std::move(task));
		// Try to process immediately if we have tokens
		processQueueNow();
	});
}

void ApiWrap::RequestThrottler::tryProcessQueue() {
	// Just delegate to processQueueNow - this is for external calls
	processQueueNow();
}

void ApiWrap::RequestThrottler::refreshTokens() {
	const auto now = crl::now();
	if (_lastRefresh == 0) {
		_lastRefresh = now;
		return;
	}
	const auto elapsed = now - _lastRefresh;
	if (elapsed >= kMinRequestIntervalMs) {
		const auto add = int(elapsed / kMinRequestIntervalMs);
		_tokens = std::min(10, _tokens + add); // Version 1: Burst capacity 12
		_lastRefresh += add * kMinRequestIntervalMs;
	}
}

void ApiWrap::RequestThrottler::processQueueNow() {
	refreshTokens();
	// Process as many tasks as we have tokens for - runs synchronously
	while (!_taskQueue.empty() && _tokens > 0) {
		--_tokens;
		auto task = std::move(_taskQueue.front());
		_taskQueue.pop_front();
		task();
	}
	if (!_taskQueue.empty() && !_retryScheduled) {
		_retryScheduled = true;
		const auto nextRefresh = _lastRefresh + kMinRequestIntervalMs;
		const auto now = crl::now();
		const auto delay = std::max(crl::time(1), nextRefresh - now);
		const auto runner = _runner;

		crl::on_main([=, guard = _guard] {
			base::call_delayed(delay, [=] {
				if (!*guard) {
					return;
				}
				runner([=] {
					if (!*guard) {
						return;
					}
					_retryScheduled = false;
					processQueueNow();
				});
			});
		});
	}
}

class ApiWrap::LoadedFileCache {
public:
	using Location = Data::FileLocation;

	LoadedFileCache(int limit);

	void save(const Location &location, const QString &relativePath);
	std::optional<QString> find(const Location &location) const;

private:
	int _limit = 0;
	std::map<LocationKey, QString> _map;
	std::deque<LocationKey> _list;

};

struct ApiWrap::StartProcess {
	FnMut<void(StartInfo)> done;

	enum class Step {
		UserpicsCount,
		StoriesCount,
		SplitRanges,
		DialogsCount,
		LeftChannelsCount,
	};
	std::deque<Step> steps;
	int splitIndex = 0;
	StartInfo info;
};

struct ApiWrap::ContactsProcess {
	FnMut<void(Data::ContactsList&&)> done;

	Data::ContactsList result;

	int topPeersOffset = 0;
};

struct ApiWrap::UserpicsProcess {
	FnMut<bool(Data::UserpicsInfo&&)> start;
	Fn<bool(DownloadProgress)> fileProgress;
	Fn<bool(Data::UserpicsSlice&&)> handleSlice;
	FnMut<void()> finish;

	int processed = 0;
	std::optional<Data::UserpicsSlice> slice;
	uint64 maxId = 0;
	bool lastSlice = false;
	int pendingFiles = 0;
};

struct ApiWrap::StoriesProcess {
	FnMut<bool(Data::StoriesInfo&&)> start;
	Fn<bool(DownloadProgress)> fileProgress;
	Fn<bool(Data::StoriesSlice&&)> handleSlice;
	FnMut<void()> finish;

	int processed = 0;
	std::optional<Data::StoriesSlice> slice;
	int offsetId = 0;
	bool lastSlice = false;
	int pendingFiles = 0;
};

struct ApiWrap::OtherDataProcess {
	Data::File file;
	FnMut<void(Data::File&&)> done;
};

struct ApiWrap::FileProcess {
	FileProcess(Data::File &file, const QString &fullPath, Output::Stats *stats)
	: fileRef(file)
	, outputFile(fullPath, stats) {
	}

	Data::File &fileRef;
	Output::File outputFile;
	QString relativePath;

	Fn<bool(FileProgress)> progress;
	FnMut<void(const QString &relativePath)> done;

	uint64 randomId = 0;
	Data::FileLocation location;
	Data::FileOrigin origin;
	int64 offset = 0;
	int64 size = 0;
	struct Request {
		int64 offset = 0;
		QByteArray bytes;
	};
	std::deque<Request> requests;
	std::map<mtpRequestId, int64> activeRequestOffsets;
	std::set<int64> scheduledOffsets;                      // offsets currently scheduled or in-flight
    std::deque<int64> pendingRetryOffsets;                 // offsets that need retry
    std::unordered_map<int64, int> retryCounts;            // per-offset retry counter
	bool active = false;
};

struct ApiWrap::ChatsProcess {
	Fn<bool(int count)> progress;
	FnMut<void(Data::DialogsInfo&&)> done;

	Data::DialogsInfo info;
	int processedCount = 0;
	std::map<PeerId, int> indexByPeer;
};

struct ApiWrap::LeftChannelsProcess : ChatsProcess {
	int fullCount = 0;
	int offset = 0;
	bool finished = false;
};

struct ApiWrap::DialogsProcess : ChatsProcess {
	int splitIndexPlusOne = 0;
	TimeId offsetDate = 0;
	int32 offsetId = 0;
	MTPInputPeer offsetPeer = MTP_inputPeerEmpty();
};

struct ApiWrap::ChatProcess {
	Data::DialogInfo info;
	int32 fromId = 0;
	int32 tillId = 0;

	FnMut<bool(const Data::DialogInfo &)> start;
	Fn<bool(DownloadProgress)> fileProgress;
	Fn<bool(Data::MessagesSlice&&)> handleSlice;
	FnMut<void()> done;

	FnMut<void(MTPmessages_Messages&&)> requestDone;

	int localSplitIndex = 0;
	int32 largestIdPlusOne = 1;

	Data::ParseMediaContext context;
	std::optional<Data::MessagesSlice> slice;
	bool lastSlice = false;
	int pendingFiles = 0;

	// Track messages processed (only when a whole message is completed)
	int messagesProcessed = 0;

	// Map file randomId -> message index in current slice
	std::unordered_map<uint64, int> fileToMessageIndex;

	// Per-message parts in current slice
	std::vector<int> messageFilesRequired;
	std::vector<int> messageFilesDone;

	// Emoji id -> list of message indices depending on that emoji in current slice
	std::unordered_map<uint64, std::vector<int>> emojiToMessageIndices;
};



template <typename Request>
class ApiWrap::RequestBuilder {
public:
	using Original = MTP::ConcurrentSender::SpecificRequestBuilder<Request>;
	using Response = typename Request::ResponseType;

	RequestBuilder(
		Original &&builder,
		Fn<void(const MTP::Error&)> commonFailHandler);

	[[nodiscard]] RequestBuilder &done(FnMut<void()> &&handler);
	[[nodiscard]] RequestBuilder &done(
		FnMut<void(Response &&)> &&handler);
	[[nodiscard]] RequestBuilder &fail(
		Fn<bool(const MTP::Error&)> &&handler);

	mtpRequestId send();

private:
	Original _builder;
	Fn<void(const MTP::Error&)> _commonFailHandler;

};

template <typename Request>
ApiWrap::RequestBuilder<Request>::RequestBuilder(
	Original &&builder,
	Fn<void(const MTP::Error&)> commonFailHandler)
: _builder(std::move(builder))
, _commonFailHandler(std::move(commonFailHandler)) {
}

template <typename Request>
auto ApiWrap::RequestBuilder<Request>::done(
	FnMut<void()> &&handler
) -> RequestBuilder& {
	if (handler) {
		[[maybe_unused]] auto &silence_warning = _builder.done(std::move(handler));
	}
	return *this;
}

template <typename Request>
auto ApiWrap::RequestBuilder<Request>::done(
	FnMut<void(Response &&)> &&handler
) -> RequestBuilder& {
	if (handler) {
		[[maybe_unused]] auto &silence_warning = _builder.done(std::move(handler));
	}
	return *this;
}

template <typename Request>
auto ApiWrap::RequestBuilder<Request>::fail(
	Fn<bool(const MTP::Error &)> &&handler
) -> RequestBuilder& {
	if (handler) {
		[[maybe_unused]] auto &silence_warning = _builder.fail([
			common = base::take(_commonFailHandler),
			specific = std::move(handler)
		](const MTP::Error &error) {
			if (!specific(error)) {
				common(error);
			}
		});
	}
	return *this;
}

template <typename Request>
mtpRequestId ApiWrap::RequestBuilder<Request>::send() {
	return _commonFailHandler
		? _builder.fail(base::take(_commonFailHandler)).send()
		: _builder.send();
}

ApiWrap::LoadedFileCache::LoadedFileCache(int limit) : _limit(limit) {
	Expects(limit >= 0);
}

void ApiWrap::LoadedFileCache::save(
		const Location &location,
		const QString &relativePath) {
	if (!location) {
		return;
	}
	const auto key = ComputeLocationKey(location);
	_map[key] = relativePath;
	_list.push_back(key);
	if (_list.size() > _limit) {
		const auto key = _list.front();
		_list.pop_front();
		_map.erase(key);
	}
}

std::optional<QString> ApiWrap::LoadedFileCache::find(
		const Location &location) const {
	if (!location) {
		return std::nullopt;
	}
	const auto key = ComputeLocationKey(location);
	if (const auto i = _map.find(key); i != end(_map)) {
		return i->second;
	}
	return std::nullopt;
}

template <typename Request>
auto ApiWrap::mainRequest(Request &&request) {
	Expects(_takeoutId.has_value());

	auto original = std::move(_mtp.request(MTPInvokeWithTakeout<Request>(
		MTP_long(*_takeoutId),
		std::forward<Request>(request)
	)).toDC(MTP::ShiftDcId(0, MTP::kExportDcShift)));

	return RequestBuilder<MTPInvokeWithTakeout<Request>>(
		std::move(original),
		[=](const MTP::Error &result) { error(result); });
}

template <typename Request>
auto ApiWrap::splitRequest(int index, Request &&request) {
	Expects(index < _splits.size());

	return mainRequest(MTPInvokeWithMessagesRange<Request>(
		_splits[index],
		std::forward<Request>(request)));
}

auto ApiWrap::fileRequest(const Data::FileLocation &location, int64 offset, int chunkSize) {
	Expects(location);
	Expects(_takeoutId.has_value());

	auto original = std::move(_mtp.request(MTPInvokeWithTakeout<MTPupload_GetFile>(
		MTP_long(*_takeoutId),
		MTPupload_GetFile(
			MTP_flags(0),
			location.data,
			MTP_long(offset),
			MTP_int(chunkSize))
	)).toDC(MTP::ShiftDcId(location.dcId, MTP::kExportMediaDcShift)));

	return RequestBuilder<MTPInvokeWithTakeout<MTPupload_GetFile>>(
		std::move(original),
		[=](const MTP::Error &result) { error(result); });
}

ApiWrap::ApiWrap(base::weak_qptr<MTP::Instance> weak, Fn<void(FnMut<void()>)> runner)
: _mtp(weak, runner)
, _fileCache(std::make_unique<LoadedFileCache>(kLocationCacheSize))
, _lifetimeGuard(std::make_shared<bool>(true))
, _throttler(runner, _lifetimeGuard)
{
}

void ApiWrap::scheduleBatchDelay(crl::time delay) {
	const auto runner = _throttler.runner();
	crl::on_main([=, guard = _lifetimeGuard] {
		base::call_delayed(delay, [=] {
			runner([=] {
				if (*guard) {
					scheduleMoreFiles();
				}
			});
		});
	});
}


rpl::producer<MTP::Error> ApiWrap::errors() const {
	return _errors.events();
}

rpl::producer<Output::Result> ApiWrap::ioErrors() const {
	return _ioErrors.events();
}

void ApiWrap::startExport(
		const Settings &settings,
		Output::Stats *stats,
		FnMut<void(StartInfo)> done) {
	Expects(_settings == nullptr);
	Expects(_startProcess == nullptr);

	_settings = std::make_unique<Settings>(settings);
	_stats = stats;
	_startProcess = std::make_unique<StartProcess>();
	_startProcess->done = std::move(done);

	using Step = StartProcess::Step;
	if (_settings->types & Settings::Type::Userpics) {
		_startProcess->steps.push_back(Step::UserpicsCount);
	}
	if (_settings->types & Settings::Type::Stories) {
		_startProcess->steps.push_back(Step::StoriesCount);
	}
	if (_settings->types & Settings::Type::AnyChatsMask) {
		_startProcess->steps.push_back(Step::SplitRanges);
		_startProcess->steps.push_back(Step::DialogsCount);
	}
	if (_settings->types & Settings::Type::GroupsChannelsMask) {
		if (!_settings->onlySinglePeer()) {
			_startProcess->steps.push_back(Step::LeftChannelsCount);
		}
	}
	startMainSession([=] {
		sendNextStartRequest();
	});
}

void ApiWrap::sendNextStartRequest() {
	Expects(_startProcess != nullptr);

	auto &steps = _startProcess->steps;
	if (steps.empty()) {
		finishStartProcess();
		return;
	}
	using Step = StartProcess::Step;
	const auto step = steps.front();
	steps.pop_front();
	switch (step) {
	case Step::UserpicsCount:
		return requestUserpicsCount();
	case Step::StoriesCount:
		return requestStoriesCount();
	case Step::SplitRanges:
		return requestSplitRanges();
	case Step::DialogsCount:
		return requestDialogsCount();
	case Step::LeftChannelsCount:
		return requestLeftChannelsCount();
	}
	Unexpected("Step in ApiWrap::sendNextStartRequest.");
}

void ApiWrap::requestUserpicsCount() {
	Expects(_startProcess != nullptr);

	mainRequest(MTPphotos_GetUserPhotos(
		_user,
		MTP_int(0),  // offset
		MTP_long(0), // max_id
		MTP_int(0)   // limit
	)).done([=](const MTPphotos_Photos &result) {
		Expects(_settings != nullptr);
		Expects(_startProcess != nullptr);

		_startProcess->info.userpicsCount = result.match(
		[](const MTPDphotos_photos &data) {
			return int(data.vphotos().v.size());
		}, [](const MTPDphotos_photosSlice &data) {
			return data.vcount().v;
		});

		sendNextStartRequest();
	}).send();
}

void ApiWrap::requestStoriesCount() {
	Expects(_startProcess != nullptr);

	mainRequest(MTPstories_GetStoriesArchive(
		MTP_inputPeerSelf(),
		MTP_int(0), // offset_id
		MTP_int(0) // limit
	)).done([=](const MTPstories_Stories &result) {
		Expects(_settings != nullptr);
		Expects(_startProcess != nullptr);

		_startProcess->info.storiesCount = result.data().vcount().v;

		sendNextStartRequest();
	}).send();
}

void ApiWrap::requestSplitRanges() {
	Expects(_startProcess != nullptr);

	mainRequest(MTPmessages_GetSplitRanges(
	)).done([=](const MTPVector<MTPMessageRange> &result) {
		_splits = result.v;
		if (_splits.empty()) {
			_splits.push_back(MTP_messageRange(
				MTP_int(1),
				MTP_int(std::numeric_limits<int>::max())));
		}
		_startProcess->splitIndex = useOnlyLastSplit()
			? (_splits.size() - 1)
			: 0;

		sendNextStartRequest();
	}).send();
}

void ApiWrap::requestDialogsCount() {
	Expects(_startProcess != nullptr);

	if (_settings->onlySinglePeer()) {
		_startProcess->info.dialogsCount
			= (_settings->singlePeer.type() == mtpc_inputPeerChannel
				? 1
				: _splits.size());
		sendNextStartRequest();
		return;
	}

	const auto offsetDate = 0;
	const auto offsetId = 0;
	const auto offsetPeer = MTP_inputPeerEmpty();
	const auto limit = 1;
	const auto hash = uint64(0);
	splitRequest(_startProcess->splitIndex, MTPmessages_GetDialogs(
		MTP_flags(0),
		MTPint(), // folder_id
		MTP_int(offsetDate),
		MTP_int(offsetId),
		offsetPeer,
		MTP_int(limit),
		MTP_long(hash)
	)).done([=](const MTPmessages_Dialogs &result) {
		Expects(_settings != nullptr);
		Expects(_startProcess != nullptr);

		const auto count = result.match(
		[](const MTPDmessages_dialogs &data) {
			return int(data.vdialogs().v.size());
		}, [](const MTPDmessages_dialogsSlice &data) {
			return data.vcount().v;
		}, [](const MTPDmessages_dialogsNotModified &data) {
			return -1;
		});
		if (count < 0) {
			error("Unexpected dialogsNotModified received.");
			return;
		}
		_startProcess->info.dialogsCount += count;

		if (++_startProcess->splitIndex >= _splits.size()) {
			sendNextStartRequest();
		} else {
			requestDialogsCount();
		}
	}).send();
}

void ApiWrap::requestLeftChannelsCount() {
	Expects(_startProcess != nullptr);
	Expects(_leftChannelsProcess == nullptr);

	_leftChannelsProcess = std::make_unique<LeftChannelsProcess>();
	requestLeftChannelsSliceGeneric([=] {
		Expects(_startProcess != nullptr);
		Expects(_leftChannelsProcess != nullptr);

		_startProcess->info.dialogsCount
			+= _leftChannelsProcess->fullCount;
		sendNextStartRequest();
	});
}

void ApiWrap::finishStartProcess() {
	Expects(_startProcess != nullptr);

	const auto process = base::take(_startProcess);
	process->done(process->info);
}

bool ApiWrap::useOnlyLastSplit() const {
	return !(_settings->types & Settings::Type::NonChannelChatsMask);
}

void ApiWrap::requestLeftChannelsList(
		Fn<bool(int count)> progress,
		FnMut<void(Data::DialogsInfo&&)> done) {
	Expects(_leftChannelsProcess != nullptr);

	_leftChannelsProcess->progress = std::move(progress);
	_leftChannelsProcess->done = std::move(done);
	requestLeftChannelsSlice();
}

void ApiWrap::requestLeftChannelsSlice() {
	requestLeftChannelsSliceGeneric([=] {
		Expects(_leftChannelsProcess != nullptr);

		if (_leftChannelsProcess->finished) {
			const auto process = base::take(_leftChannelsProcess);
			process->done(std::move(process->info));
		} else {
			requestLeftChannelsSlice();
		}
	});
}

void ApiWrap::requestDialogsList(
		Fn<bool(int count)> progress,
		FnMut<void(Data::DialogsInfo&&)> done) {
	Expects(_dialogsProcess == nullptr);

	_dialogsProcess = std::make_unique<DialogsProcess>();
	_dialogsProcess->splitIndexPlusOne = _splits.size();
	_dialogsProcess->progress = std::move(progress);
	_dialogsProcess->done = std::move(done);

	requestDialogsSlice();
}

void ApiWrap::startMainSession(FnMut<void()> done) {
	using Type = Settings::Type;
	const auto sizeLimit = _settings->media.sizeLimit;
	const auto hasFiles = ((_settings->media.types != 0) && (sizeLimit > 0))
		|| (_settings->types & Type::Userpics)
		|| (_settings->types & Type::Stories);

	using Flag = MTPaccount_InitTakeoutSession::Flag;
	const auto flags = Flag(0)
		| (_settings->types & Type::Contacts ? Flag::f_contacts : Flag(0))
		| (hasFiles ? Flag::f_files : Flag(0))
		| ((hasFiles && sizeLimit < kFileMaxSize)
			? Flag::f_file_max_size
			: Flag(0))
		| (_settings->types & (Type::PersonalChats | Type::BotChats)
			? Flag::f_message_users
			: Flag(0))
		| (_settings->types & Type::PrivateGroups
			? (Flag::f_message_chats | Flag::f_message_megagroups)
			: Flag(0))
		| (_settings->types & Type::PublicGroups
			? Flag::f_message_megagroups
			: Flag(0))
		| (_settings->types & (Type::PrivateChannels | Type::PublicChannels)
			? Flag::f_message_channels
			: Flag(0));

	_mtp.request(MTPusers_GetUsers(
		MTP_vector<MTPInputUser>(1, MTP_inputUserSelf())
	)).done([=, done = std::move(done)](
			const MTPVector<MTPUser> &result) mutable {
		for (const auto &user : result.v) {
			user.match([&](const MTPDuser &data) {
				if (data.is_self()) {
					_selfId.emplace(data.vid());
				}
			}, [&](const MTPDuserEmpty&) {
			});
		}
		if (!_selfId) {
			error("Could not retrieve selfId.");
			return;
		}
		_mtp.request(MTPaccount_InitTakeoutSession(
			MTP_flags(flags),
			MTP_long(sizeLimit)
		)).done([=, done = std::move(done)](
				const MTPaccount_Takeout &result) mutable {
			_takeoutId = result.match([](const MTPDaccount_takeout &data) {
				return data.vid().v;
			});
			done();
		}).fail([=](const MTP::Error &result) {
			error(result);
		}).toDC(MTP::ShiftDcId(0, MTP::kExportDcShift)).send();
	}).fail([=](const MTP::Error &result) {
		error(result);
	}).send();
}

void ApiWrap::requestPersonalInfo(FnMut<void(Data::PersonalInfo&&)> done) {
	mainRequest(MTPusers_GetFullUser(
		_user
	)).done([=, done = std::move(done)](const MTPusers_UserFull &result) mutable {
		result.match([&](const MTPDusers_userFull &data) {
			if (!data.vusers().v.empty()) {
				done(Data::ParsePersonalInfo(data));
			} else {
				error("Bad user type.");
			}
		});
	}).send();
}

void ApiWrap::requestOtherData(
		const QString &suggestedPath,
		FnMut<void(Data::File&&)> done) {
	Expects(_otherDataProcess == nullptr);

	_otherDataProcess = std::make_unique<OtherDataProcess>();
	_otherDataProcess->done = std::move(done);
	_otherDataProcess->file.location.data = MTP_inputTakeoutFileLocation();
	_otherDataProcess->file.suggestedPath = suggestedPath;
	loadFile(
		_otherDataProcess->file,
		Data::FileOrigin(),
		[](FileProgress progress) { return true; },
		[=](const QString &result) { otherDataDone(result); });
}

void ApiWrap::otherDataDone(const QString &relativePath) {
	Expects(_otherDataProcess != nullptr);

	const auto process = base::take(_otherDataProcess);
	process->file.relativePath = relativePath;
	if (relativePath.isEmpty()) {
		process->file.skipReason = Data::File::SkipReason::Unavailable;
	}
	process->done(std::move(process->file));
}

void ApiWrap::requestUserpics(
		FnMut<bool(Data::UserpicsInfo&&)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::UserpicsSlice&&)> slice,
		FnMut<void()> finish) {
	Expects(_userpicsProcess == nullptr);

	_userpicsProcess = std::make_unique<UserpicsProcess>();
	_userpicsProcess->start = std::move(start);
	_userpicsProcess->fileProgress = std::move(progress);
	_userpicsProcess->handleSlice = std::move(slice);
	_userpicsProcess->finish = std::move(finish);

	mainRequest(MTPphotos_GetUserPhotos(
		_user,
		MTP_int(0), // offset
		MTP_long(_userpicsProcess->maxId),
		MTP_int(kUserpicsSliceLimit)
	)).done([=](const MTPphotos_Photos &result) mutable {
		Expects(_userpicsProcess != nullptr);

		auto startInfo = result.match(
		[](const MTPDphotos_photos &data) {
			return Data::UserpicsInfo{ int(data.vphotos().v.size()) };
		}, [](const MTPDphotos_photosSlice &data) {
			return Data::UserpicsInfo{ data.vcount().v };
		});
		if (!_userpicsProcess->start(std::move(startInfo))) {
			return;
		}

		handleUserpicsSlice(result);
	}).send();
}

void ApiWrap::handleUserpicsSlice(const MTPphotos_Photos &result) {
	Expects(_userpicsProcess != nullptr);

	result.match([&](const auto &data) {
		if constexpr (MTPDphotos_photos::Is<decltype(data)>()) {
			_userpicsProcess->lastSlice = true;
		}
		loadUserpicsFiles(Data::ParseUserpicsSlice(
			data.vphotos(),
			_userpicsProcess->processed));
	});
}

void ApiWrap::loadUserpicsFiles(Data::UserpicsSlice &&slice) {
	Expects(_userpicsProcess != nullptr);
	Expects(!_userpicsProcess->slice.has_value());

	if (slice.list.empty()) {
		_userpicsProcess->lastSlice = true;
	}
	_userpicsProcess->slice = std::move(slice);
	_userpicsProcess->pendingFiles = 0;
	for (const auto &photo : _userpicsProcess->slice->list) {
		if (photo.image.file.location) {
			_userpicsProcess->pendingFiles++;
		}
	}

	if (_userpicsProcess->pendingFiles > 0) {
		loadNextUserpic();
	} else {
		finishUserpicsSlice();
	}
}

void ApiWrap::loadNextUserpic() {
	Expects(_userpicsProcess != nullptr);
	Expects(_userpicsProcess->slice.has_value());

	for (auto &photo : _userpicsProcess->slice->list) {
		processFileLoad(
			photo.image.file,
			Data::FileOrigin(),
			[=](FileProgress value) { return loadUserpicProgress(value); },
			[=](const QString &path) { loadUserpicDone(path); });
	}
}

void ApiWrap::finishUserpicsSlice() {
	Expects(_userpicsProcess != nullptr);
	Expects(_userpicsProcess->slice.has_value());

	auto slice = *base::take(_userpicsProcess->slice);
	if (!slice.list.empty()) {
		_userpicsProcess->processed += slice.list.size();
		_userpicsProcess->maxId = slice.list.back().id;
		if (!_userpicsProcess->handleSlice(std::move(slice))) {
			return;
		}
	}
	if (_userpicsProcess->lastSlice) {
		finishUserpics();
		return;
	}

	mainRequest(MTPphotos_GetUserPhotos(
		_user,
		MTP_int(0), // offset
		MTP_long(_userpicsProcess->maxId),
		MTP_int(kUserpicsSliceLimit)
	)).done([=](const MTPphotos_Photos &result) {
		handleUserpicsSlice(result);
	}).send();
}

bool ApiWrap::loadUserpicProgress(FileProgress progress) {
	const auto it = _fileProcesses.find(progress.randomId);
	if (it == end(_fileProcesses)) {
		return false;
	}
	const auto &process = *it->second;

	Expects(_userpicsProcess != nullptr);

	return _userpicsProcess->fileProgress({
		.randomId = process.randomId,
		.path = process.relativePath,
		.itemIndex = _userpicsProcess->processed, // This is an approximation now.
		.ready = progress.ready,
		.total = progress.total,
	});
}

void ApiWrap::loadUserpicDone(const QString &relativePath) {
	Expects(_userpicsProcess != nullptr);

	--_userpicsProcess->pendingFiles;
	Assert(_userpicsProcess->pendingFiles >= 0);

	if (_userpicsProcess->pendingFiles == 0) {
		finishUserpicsSlice();
	}
}

void ApiWrap::finishUserpics() {
	Expects(_userpicsProcess != nullptr);

	base::take(_userpicsProcess)->finish();
}

void ApiWrap::requestStories(
		FnMut<bool(Data::StoriesInfo&&)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::StoriesSlice&&)> slice,
		FnMut<void()> finish) {
	Expects(_storiesProcess == nullptr);

	_storiesProcess = std::make_unique<StoriesProcess>();
	_storiesProcess->start = std::move(start);
	_storiesProcess->fileProgress = std::move(progress);
	_storiesProcess->handleSlice = std::move(slice);
	_storiesProcess->finish = std::move(finish);

	mainRequest(MTPstories_GetStoriesArchive(
		MTP_inputPeerSelf(),
		MTP_int(_storiesProcess->offsetId),
		MTP_int(kStoriesSliceLimit)
	)).done([=](const MTPstories_Stories &result) mutable {
		Expects(_storiesProcess != nullptr);

		auto startInfo = Data::StoriesInfo{ result.data().vcount().v };
		if (!_storiesProcess->start(std::move(startInfo))) {
			return;
		}

		handleStoriesSlice(result);
	}).send();
}

void ApiWrap::handleStoriesSlice(const MTPstories_Stories &result) {
	Expects(_storiesProcess != nullptr);

	loadStoriesFiles(Data::ParseStoriesSlice(
		result.data().vstories(),
		_storiesProcess->processed));
}

void ApiWrap::loadStoriesFiles(Data::StoriesSlice &&slice) {
	Expects(_storiesProcess != nullptr);
	Expects(!_storiesProcess->slice.has_value());

	if (!slice.lastId) {
		_storiesProcess->lastSlice = true;
	}
	_storiesProcess->slice = std::move(slice);
	_storiesProcess->pendingFiles = 0;
	for (const auto &story : _storiesProcess->slice->list) {
		if (story.file().location) {
			_storiesProcess->pendingFiles++;
		}
		if (story.thumb().file.location) {
			_storiesProcess->pendingFiles++;
		}
	}

	if (_storiesProcess->pendingFiles > 0) {
		loadNextStory();
	} else {
		finishStoriesSlice();
	}
}

void ApiWrap::loadNextStory() {
	Expects(_storiesProcess != nullptr);
	Expects(_storiesProcess->slice.has_value());

	for (auto &story : _storiesProcess->slice->list) {
		const auto origin = Data::FileOrigin{ .storyId = story.id };
		processFileLoad(
			story.file(),
			origin,
			[=](FileProgress value) { return loadStoryProgress(value); },
			[=](const QString &path) { loadStoryDone(path); },
			nullptr,
			&story);

		processFileLoad(
			story.thumb().file,
			origin,
			[=](FileProgress value) { return loadStoryThumbProgress(value); },
			[=](const QString &path) { loadStoryThumbDone(path); },
			nullptr,
			&story);
	}
}

void ApiWrap::finishStoriesSlice() {
	Expects(_storiesProcess != nullptr);
	Expects(_storiesProcess->slice.has_value());

	auto slice = *base::take(_storiesProcess->slice);
	if (slice.lastId) {
		_storiesProcess->processed += slice.list.size();
		_storiesProcess->offsetId = slice.lastId;
		if (!_storiesProcess->handleSlice(std::move(slice))) {
			return;
		}
	}
	if (_storiesProcess->lastSlice) {
		finishStories();
		return;
	}

	mainRequest(MTPstories_GetStoriesArchive(
		MTP_inputPeerSelf(),
		MTP_int(_storiesProcess->offsetId),
		MTP_int(kStoriesSliceLimit)
	)).done([=](const MTPstories_Stories &result) {
		handleStoriesSlice(result);
	}).send();
}

bool ApiWrap::loadStoryProgress(FileProgress progress) {
	const auto it = _fileProcesses.find(progress.randomId);
	if (it == end(_fileProcesses)) {
		return false;
	}
	const auto &process = *it->second;

	Expects(_storiesProcess != nullptr);

	return _storiesProcess->fileProgress({
		.randomId = process.randomId,
		.path = process.relativePath,
		.itemIndex = _storiesProcess->processed,
		.ready = progress.ready,
		.total = progress.total });
}

void ApiWrap::loadStoryDone(const QString &relativePath) {
	Expects(_storiesProcess != nullptr);
	--_storiesProcess->pendingFiles;
	Assert(_storiesProcess->pendingFiles >= 0);
	if (_storiesProcess->pendingFiles == 0) {
		finishStoriesSlice();
	}
}

bool ApiWrap::loadStoryThumbProgress(FileProgress progress) {
	return loadStoryProgress(progress);
}

void ApiWrap::loadStoryThumbDone(const QString &relativePath) {
	Expects(_storiesProcess != nullptr);
	--_storiesProcess->pendingFiles;
	Assert(_storiesProcess->pendingFiles >= 0);
	if (_storiesProcess->pendingFiles == 0) {
		finishStoriesSlice();
	}
}

void ApiWrap::finishStories() {
	Expects(_storiesProcess != nullptr);

	base::take(_storiesProcess)->finish();
}

void ApiWrap::requestContacts(FnMut<void(Data::ContactsList&&)> done) {
	Expects(_contactsProcess == nullptr);

	_contactsProcess = std::make_unique<ContactsProcess>();
	_contactsProcess->done = std::move(done);
	mainRequest(MTPcontacts_GetSaved(
	)).done([=](const MTPVector<MTPSavedContact> &result) {
		_contactsProcess->result = Data::ParseContactsList(result);

		const auto resolve = [=](int index, const auto &resolveNext) -> void {
			if (index == _contactsProcess->result.list.size()) {
				return requestTopPeersSlice();
			}
			const auto &contact = _contactsProcess->result.list[index];
			mainRequest(MTPcontacts_ResolvePhone(
				MTP_string(qs(contact.phoneNumber))
			)).done([=](const MTPcontacts_ResolvedPeer &result) {
				auto &contact = _contactsProcess->result.list[index];
				contact.userId = result.data().vpeer().match([&](
						const MTPDpeerUser &user) {
					return UserId(user.vuser_id());
				}, [](const auto &) {
					return UserId();
				});
				resolveNext(index + 1, resolveNext);
			}).fail([=](const MTP::Error &) {
				resolveNext(index + 1, resolveNext);
				return true;
			}).send();
		};

		if (base::options::lookup<bool>("show-peer-id-below-about").value()) {
			resolve(0, resolve);
		} else {
			requestTopPeersSlice();
		}

	}).send();
}

void ApiWrap::requestTopPeersSlice() {
	Expects(_contactsProcess != nullptr);

	using Flag = MTPcontacts_GetTopPeers::Flag;
	mainRequest(MTPcontacts_GetTopPeers(
		MTP_flags(Flag::f_correspondents
			| Flag::f_bots_inline
			| Flag::f_phone_calls),
		MTP_int(_contactsProcess->topPeersOffset),
		MTP_int(kTopPeerSliceLimit),
		MTP_long(0) // hash
	)).done([=](const MTPcontacts_TopPeers &result) {
		Expects(_contactsProcess != nullptr);

		if (!Data::AppendTopPeers(_contactsProcess->result, result)) {
			error("Unexpected data in ApiWrap::requestTopPeersSlice.");
			return;
		}

		const auto offset = _contactsProcess->topPeersOffset;
		const auto loaded = result.match(
		[](const MTPDcontacts_topPeersNotModified &data) {
			return true;
		}, [](const MTPDcontacts_topPeersDisabled &data) {
			return true;
		}, [&](const MTPDcontacts_topPeers &data) {
			for (const auto &category : data.vcategories().v) {
				const auto loaded = category.match(
				[&](const MTPDtopPeerCategoryPeers &data) {
					return offset + data.vpeers().v.size() >= data.vcount().v;
				});
				if (!loaded) {
					return false;
				}
			}
			return true;
		});

		if (loaded) {
			auto process = base::take(_contactsProcess);
			process->done(std::move(process->result));
		} else {
			_contactsProcess->topPeersOffset = std::max(std::max(
				_contactsProcess->result.correspondents.size(),
				_contactsProcess->result.inlineBots.size()),
				_contactsProcess->result.phoneCalls.size());
			requestTopPeersSlice();
		}
	}).send();
}

void ApiWrap::requestSessions(FnMut<void(Data::SessionsList&&)> done) {
	mainRequest(MTPaccount_GetAuthorizations(
	)).done([=, done = std::move(done)](
			const MTPaccount_Authorizations &result) mutable {
		auto list = Data::ParseSessionsList(result);
		mainRequest(MTPaccount_GetWebAuthorizations(
		)).done([=, done = std::move(done), list = std::move(list)](
				const MTPaccount_WebAuthorizations &result) mutable {
			list.webList = Data::ParseWebSessionsList(result).webList;
			done(std::move(list));
		}).send();
	}).send();
}

void ApiWrap::requestMessages(
		const Data::DialogInfo &info,
		int32 fromId,
		int32 tillId,
		FnMut<bool(const Data::DialogInfo &)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::MessagesSlice&&)> slice,
		FnMut<void()> done) {
	Expects(_chatProcess == nullptr);
	Expects(_selfId.has_value());

	_chatProcess = std::make_unique<ChatProcess>();
	_chatProcess->context.selfPeerId = peerFromUser(*_selfId);
	_chatProcess->info = info;
	_chatProcess->fromId = fromId;
	_chatProcess->tillId = tillId;
	_chatProcess->start = std::move(start);
	_chatProcess->fileProgress = std::move(progress);
	_chatProcess->handleSlice = std::move(slice);
	_chatProcess->done = std::move(done);

	requestMessagesCount(0);
}

void ApiWrap::requestMessagesCount(int localSplitIndex) {
	Expects(_chatProcess != nullptr);
	Expects(localSplitIndex < _chatProcess->info.splits.size());

	requestChatMessages(
		_chatProcess->info.splits[localSplitIndex],
		0, // offset_id
		0, // add_offset
		1, // limit
		[=](const MTPmessages_Messages &result) {
		Expects(_chatProcess != nullptr);

		const auto count = result.match(
			[](const MTPDmessages_messages &data) {
			return int(data.vmessages().v.size());
		}, [](const MTPDmessages_messagesSlice &data) {
			return data.vcount().v;
		}, [](const MTPDmessages_channelMessages &data) {
			return data.vcount().v;
		}, [](const MTPDmessages_messagesNotModified &data) {
			return -1;
		});
		if (count < 0) {
			error("Unexpected messagesNotModified received.");
			return;
		}
		const auto skipSplit = !Data::SingleMessageAfter(
			result,
			_settings->singlePeerFrom);
		if (skipSplit) {
			// No messages from the requested range, skip this split.
			messagesCountLoaded(localSplitIndex, 0);
			return;
		}
		checkFirstMessageDate(localSplitIndex, count);
	});
}

void ApiWrap::checkFirstMessageDate(int localSplitIndex, int count) {
	Expects(_chatProcess != nullptr);
	Expects(localSplitIndex < _chatProcess->info.splits.size());

	if (_settings->singlePeerTill <= 0) {
		messagesCountLoaded(localSplitIndex, count);
		return;
	}

	// Request first message in this split to check if its' date < till.
	requestChatMessages(
		_chatProcess->info.splits[localSplitIndex],
		1, // offset_id
		-1, // add_offset
		1, // limit
		[=](const MTPmessages_Messages &result) {
		Expects(_chatProcess != nullptr);

		const auto skipSplit = !Data::SingleMessageBefore(
			result,
			_settings->singlePeerTill);
		messagesCountLoaded(localSplitIndex, skipSplit ? 0 : count);
	});
}

void ApiWrap::messagesCountLoaded(int localSplitIndex, int count) {
	Expects(_chatProcess != nullptr);
	Expects(localSplitIndex < _chatProcess->info.splits.size());

	_chatProcess->info.messagesCountPerSplit[localSplitIndex] = count;
	if (localSplitIndex + 1 < _chatProcess->info.splits.size()) {
		requestMessagesCount(localSplitIndex + 1);
	} else if (_chatProcess->start(_chatProcess->info)) {
		requestMessagesSlice();
	}
}

void ApiWrap::finishExport(FnMut<void()> done) {
	const auto guard = gsl::finally([&] { _takeoutId = std::nullopt; });

	mainRequest(MTPaccount_FinishTakeoutSession(
		MTP_flags(MTPaccount_FinishTakeoutSession::Flag::f_success)
	)).done(std::move(done)).send();
}

void ApiWrap::skipFile(uint64 randomId) {
	auto it = _fileProcesses.find(randomId);
	if (it == end(_fileProcesses)) {
		// Not found, maybe not processed yet. Remove from queue.
		_fileDownloadQueue.erase(
			std::remove(
				_fileDownloadQueue.begin(),
				_fileDownloadQueue.end(),
				randomId),
			_fileDownloadQueue.end());
		return;
	}

	LOG(("Export Info: File skipped."));
	auto &process = *it->second;
	for (const auto &[requestId, offset] : process.activeRequestOffsets) {
		_mtp.request(requestId).cancel();
	}

	finishFile(randomId, QString());
}

void ApiWrap::cancelExportFast() {
	if (_takeoutId.has_value()) {
		const auto requestId = mainRequest(MTPaccount_FinishTakeoutSession(
			MTP_flags(0)
		)).send();
		_mtp.request(requestId).detach();
	}
}

void ApiWrap::requestSinglePeerDialog() {
	auto doneSinglePeer = [=](const auto &result) {
		appendSinglePeerDialogs(
			Data::ParseDialogsInfo(_settings->singlePeer, result));
	};
	const auto requestUser = [&](const MTPInputUser &data) {
		mainRequest(MTPusers_GetUsers(
			MTP_vector<MTPInputUser>(1, data)
		)).done(std::move(doneSinglePeer)).send();
	};
	_settings->singlePeer.match([&](const MTPDinputPeerUser &data) {
		requestUser(MTP_inputUser(data.vuser_id(), data.vaccess_hash()));
	}, [&](const MTPDinputPeerChat &data) {
		mainRequest(MTPmessages_GetChats(
			MTP_vector<MTPlong>(1, data.vchat_id())
		)).done(std::move(doneSinglePeer)).send();
	}, [&](const MTPDinputPeerChannel &data) {
		mainRequest(MTPchannels_GetChannels(
			MTP_vector<MTPInputChannel>(
				1,
				MTP_inputChannel(data.vchannel_id(), data.vaccess_hash()))
		)).done(std::move(doneSinglePeer)).send();
	}, [&](const MTPDinputPeerSelf &data) {
		requestUser(MTP_inputUserSelf());
	}, [&](const MTPDinputPeerUserFromMessage &data) {
		Unexpected("From message peer in ApiWrap::requestSinglePeerDialog.");
	}, [&](const MTPDinputPeerChannelFromMessage &data) {
		Unexpected("From message peer in ApiWrap::requestSinglePeerDialog.");
	}, [](const MTPDinputPeerEmpty &data) {
		Unexpected("Empty peer in ApiWrap::requestSinglePeerDialog.");
	});
}

mtpRequestId ApiWrap::requestSinglePeerMigrated(
		const Data::DialogInfo &info) {
	const auto input = info.input.match([&](
		const MTPDinputPeerChannel & data) {
		return MTP_inputChannel(
			data.vchannel_id(),
			data.vaccess_hash());
	}, [](auto&&) -> MTPinputChannel {
		Unexpected("Peer type in a supergroup.");
	});
	return mainRequest(MTPchannels_GetFullChannel(
		input
	)).done([=](const MTPmessages_ChatFull &result) {
		auto info = result.match([&](
				const MTPDmessages_chatFull &data) {
			const auto migratedChatId = data.vfull_chat().match([&](
					const MTPDchannelFull &data) {
				return data.vmigrated_from_chat_id().value_or_empty();
			}, [](auto &&other) -> BareId {
				return 0;
			});
			return migratedChatId
				? Data::ParseDialogsInfo(
					MTP_inputPeerChat(MTP_long(migratedChatId)),
					MTP_messages_chats(data.vchats()))
				: Data::DialogsInfo();
		});
		appendSinglePeerDialogs(std::move(info));
	}).send();
}

void ApiWrap::appendSinglePeerDialogs(Data::DialogsInfo &&info) {
	const auto isSupergroupType = [](Data::DialogInfo::Type type) {
		using Type = Data::DialogInfo::Type;
		return (type == Type::PrivateSupergroup)
			|| (type == Type::PublicSupergroup);
	};
	const auto isChannelType = [](Data::DialogInfo::Type type) {
		using Type = Data::DialogInfo::Type;
		return (type == Type::PrivateChannel)
			|| (type == Type::PublicChannel);
	};

	auto migratedRequestId = mtpRequestId(0);
	const auto last = _dialogsProcess->splitIndexPlusOne - 1;
	for (auto &info : info.chats) {
		if (isSupergroupType(info.type) && !migratedRequestId) {
			migratedRequestId = requestSinglePeerMigrated(info);
			continue;
		} else if (isChannelType(info.type) || info.isMonoforum) {
			continue;
		}
		for (auto i = last; i != 0; --i) {
			info.splits.push_back(i - 1);
			info.messagesCountPerSplit.push_back(0);
		}
	}

	if (!migratedRequestId) {
		_dialogsProcess->processedCount += info.chats.size();
	}
	appendDialogsSlice(std::move(info));

	if (migratedRequestId
		|| !_dialogsProcess->progress(_dialogsProcess->processedCount)) {
		return;
	}
	finishDialogsList();
}

void ApiWrap::requestDialogsSlice() {
	Expects(_dialogsProcess != nullptr);

	if (_settings->onlySinglePeer()) {
		requestSinglePeerDialog();
		return;
	}

	const auto splitIndex = _dialogsProcess->splitIndexPlusOne - 1;
	const auto hash = uint64(0);
	splitRequest(splitIndex, MTPmessages_GetDialogs(
		MTP_flags(0),
		MTPint(), // folder_id
		MTP_int(_dialogsProcess->offsetDate),
		MTP_int(_dialogsProcess->offsetId),
		_dialogsProcess->offsetPeer,
		MTP_int(kChatsSliceLimit),
		MTP_long(hash)
	)).done([=](const MTPmessages_Dialogs &result) {
		if (result.type() == mtpc_messages_dialogsNotModified) {
			error("Unexpected dialogsNotModified received.");
			return;
		}
		auto finished = result.match(
		[](const MTPDmessages_dialogs &data) {
			return true;
		}, [](const MTPDmessages_dialogsSlice &data) {
			return data.vdialogs().v.isEmpty();
		}, [](const MTPDmessages_dialogsNotModified &data) {
			return true;
		});

		auto info = Data::ParseDialogsInfo(result);
		_dialogsProcess->processedCount += info.chats.size();
		const auto last = info.chats.empty()
			? Data::DialogInfo()
			: info.chats.back();
		appendDialogsSlice(std::move(info));

		if (!_dialogsProcess->progress(_dialogsProcess->processedCount)) {
			return;
		}

		if (!finished && last.topMessageDate > 0) {
			_dialogsProcess->offsetId = last.topMessageId;
			_dialogsProcess->offsetDate = last.topMessageDate;
			_dialogsProcess->offsetPeer = last.input;
		} else if (!useOnlyLastSplit()
			&& --_dialogsProcess->splitIndexPlusOne > 0) {
			_dialogsProcess->offsetId = 0;
			_dialogsProcess->offsetDate = 0;
			_dialogsProcess->offsetPeer = MTP_inputPeerEmpty();
		} else {
			requestLeftChannelsIfNeeded();
			return;
		}
		requestDialogsSlice();
	}).send();
}

void ApiWrap::appendDialogsSlice(Data::DialogsInfo &&info) {
	Expects(_dialogsProcess != nullptr);
	Expects(_dialogsProcess->splitIndexPlusOne <= _splits.size());

	appendChatsSlice(
		*_dialogsProcess,
		_dialogsProcess->info.chats,
		std::move(info.chats),
		_dialogsProcess->splitIndexPlusOne - 1);
}

void ApiWrap::requestLeftChannelsIfNeeded() {
	if (_settings->types & Settings::Type::GroupsChannelsMask) {
		requestLeftChannelsList([=](int count) {
			Expects(_dialogsProcess != nullptr);

			return _dialogsProcess->progress(
				_dialogsProcess->processedCount + count);
		}, [=](Data::DialogsInfo &&result) {
			Expects(_dialogsProcess != nullptr);

			_dialogsProcess->info.left = std::move(result.left);
			finishDialogsList();
		});
	} else {
		finishDialogsList();
	}
}

void ApiWrap::finishDialogsList() {
	Expects(_dialogsProcess != nullptr);

	const auto process = base::take(_dialogsProcess);
	Data::FinalizeDialogsInfo(process->info, *_settings);
	process->done(std::move(process->info));
}

void ApiWrap::requestLeftChannelsSliceGeneric(FnMut<void()> done) {
	Expects(_leftChannelsProcess != nullptr);

	mainRequest(MTPchannels_GetLeftChannels(
		MTP_int(_leftChannelsProcess->offset)
	)).done([=, done = std::move(done)](
			const MTPmessages_Chats &result) mutable {
		Expects(_leftChannelsProcess != nullptr);

		appendLeftChannelsSlice(Data::ParseLeftChannelsInfo(result));

		const auto process = _leftChannelsProcess.get();
		process->offset += result.match(
		[](const auto &data) {
			return int(data.vchats().v.size());
		});

		process->fullCount = result.match(
		[](const MTPDmessages_chats &data) {
			return int(data.vchats().v.size());
		}, [](const MTPDmessages_chatsSlice &data) {
			return data.vcount().v;
		});

		process->finished = result.match(
		[](const MTPDmessages_chats &data) {
			return true;
		}, [](const MTPDmessages_chatsSlice &data) {
			return data.vchats().v.isEmpty();
		});

		if (process->progress) {
			if (!process->progress(process->info.left.size())) {
				return;
			}
		}

		done();
	}).send();
}

void ApiWrap::appendLeftChannelsSlice(Data::DialogsInfo &&info) {
	Expects(_leftChannelsProcess != nullptr);
	Expects(!_splits.empty());

	appendChatsSlice(
		*_leftChannelsProcess,
		_leftChannelsProcess->info.left,
		std::move(info.left),
		_splits.size() - 1);
}

void ApiWrap::appendChatsSlice(
		ChatsProcess &process,
		std::vector<Data::DialogInfo> &to,
		std::vector<Data::DialogInfo> &&from,
		int splitIndex) {
	Expects(_settings != nullptr);

	const auto types = _settings->types;
	const auto goodByTypes = [&](const Data::DialogInfo &info) {
		return ((types & SettingsFromDialogsType(info.type)) != 0);
	};
	auto filtered = ranges::views::all(
		from
	) | ranges::views::filter([&](const Data::DialogInfo &info) {
		if (goodByTypes(info)) {
			return true;
		} else if (info.migratedToChannelId
			&& (((types & Settings::Type::PublicGroups) != 0)
				|| ((types & Settings::Type::PrivateGroups) != 0))) {
			return true;
		}
		return false;
	});
	to.reserve(to.size() + from.size());
	for (auto &info : filtered) {
		const auto nextIndex = to.size();
		if (info.migratedToChannelId) {
			const auto toPeerId = PeerId(info.migratedToChannelId);
			const auto i = process.indexByPeer.find(toPeerId);
			if (i != process.indexByPeer.end()
				&& Data::AddMigrateFromSlice(
					to[i->second],
					info,
					splitIndex,
					int(_splits.size()))) {
				continue;
			} else if (!goodByTypes(info)) {
				continue;
			}
		}
		const auto &[i, ok] = process.indexByPeer.emplace(
			info.peerId,
			nextIndex);
		if (ok) {
			to.push_back(std::move(info));
		}
		to[i->second].splits.push_back(splitIndex);
		to[i->second].messagesCountPerSplit.push_back(0);
	}
}

void ApiWrap::requestMessagesSlice() {
	Expects(_chatProcess != nullptr);

	const auto count = _chatProcess->info.messagesCountPerSplit[
		_chatProcess->localSplitIndex];
	if (!count) {
		loadMessagesFiles({});
		return;
	}
	requestChatMessages(
		_chatProcess->info.splits[_chatProcess->localSplitIndex],
		_chatProcess->largestIdPlusOne,
		-kMessagesSliceLimit,
		kMessagesSliceLimit,
		[=](const MTPmessages_Messages &result) {
		Expects(_chatProcess != nullptr);

		result.match([&](const MTPDmessages_messagesNotModified &data) {
			error("Unexpected messagesNotModified received.");
		}, [&](const auto &data) {
			if constexpr (MTPDmessages_messages::Is<decltype(data)>()) {
				_chatProcess->lastSlice = true;
			}
			loadMessagesFiles(Data::ParseMessagesSlice(
				_chatProcess->context,
				data.vmessages(),
				data.vusers(),
				data.vchats(),
				_chatProcess->info.relativePath));
		});
	});
}

void ApiWrap::requestChatMessages(
		int splitIndex,
		int offsetId,
		int addOffset,
		int limit,
		FnMut<void(MTPmessages_Messages&&)> done) {
	Expects(_chatProcess != nullptr);

	_chatProcess->requestDone = std::move(done);
	const auto doneHandler = [=](MTPmessages_Messages &&result) {
		Expects(_chatProcess != nullptr);

		base::take(_chatProcess->requestDone)(std::move(result));
	};
	const auto splitsCount = int(_splits.size());
	const auto realPeerInput = (splitIndex >= 0)
		? _chatProcess->info.input
		: _chatProcess->info.migratedFromInput;
	const auto outgoingInput = _chatProcess->info.isMonoforum
		? _chatProcess->info.monoforumBroadcastInput
		: MTP_inputPeerSelf();
	const auto realSplitIndex = (splitIndex >= 0)
		? splitIndex
		: (splitsCount + splitIndex);

	const auto minId = _settings->useIdRange ? _chatProcess->fromId : 0;
	const auto maxId = _settings->useIdRange ? _chatProcess->tillId : 0;

	if (_chatProcess->info.onlyMyMessages) {
		splitRequest(realSplitIndex, MTPmessages_Search(
			MTP_flags(MTPmessages_Search::Flag::f_from_id),
			realPeerInput,
			MTP_string(), // query
			MTP_inputPeerSelf(),
			MTPInputPeer(), // saved_peer_id
			MTPVector<MTPReaction>(), // saved_reaction
			MTPint(), // top_msg_id
			MTP_inputMessagesFilterEmpty(),
			MTP_int(0), // min_date
			MTP_int(0), // max_date
			MTP_int(offsetId),
			MTP_int(addOffset),
			MTP_int(limit),
			MTP_int(maxId), // max_id
			MTP_int(minId), // min_id
			MTP_long(0) // hash
		)).done(doneHandler).send();
	} else {
		splitRequest(realSplitIndex, MTPmessages_GetHistory(
			realPeerInput,
			MTP_int(offsetId),
			MTP_int(0), // offset_date
			MTP_int(addOffset),
			MTP_int(limit),
			MTP_int(maxId), // max_id
			MTP_int(minId), // min_id
			MTP_long(0)	 // hash
		)).fail([=](const MTP::Error &error) {
			Expects(_chatProcess != nullptr);

			if (error.type() == u"CHANNEL_PRIVATE"_q) {
				if (realPeerInput.type() == mtpc_inputPeerChannel
					&& !_chatProcess->info.onlyMyMessages) {

					// Perhaps we just left / were kicked from channel.
					// Just switch to only my messages.
					_chatProcess->info.onlyMyMessages = true;
					requestChatMessages(
						splitIndex,
							offsetId,
							addOffset,
							limit,
							base::take(_chatProcess->requestDone));
					return true;
				}
			}
			return false;
		}).done(doneHandler).send();
	}
}


void ApiWrap::loadMessagesFiles(Data::MessagesSlice &&slice) {
	Expects(_chatProcess != nullptr);
	Expects(!_chatProcess->slice.has_value());

	collectMessagesCustomEmoji(slice);

	if (slice.list.empty()) {
		_chatProcess->lastSlice = true;
	}
	_chatProcess->slice.emplace(std::move(slice));
	auto &s = *_chatProcess->slice;

	_chatProcess->pendingFiles = 0;
	_chatProcess->fileToMessageIndex.clear();
	_chatProcess->messageFilesRequired.assign(s.list.size(), 0);
	_chatProcess->messageFilesDone.assign(s.list.size(), 0);
	_chatProcess->emojiToMessageIndices.clear();

	for (int i = 0; i < int(s.list.size()); ++i) {
		const auto &message = s.list[i];

		// Skip counting files for messages that will be filtered by date
		// This matches the skip logic in loadNextMessageFile()
		if (Data::SkipMessageByDate(message, *_settings)) {
			continue;
		}

		int required = 0;
		if (message.file().location) {
			++required;
			++_chatProcess->pendingFiles;
		}
		if (message.thumb().file.location) {
			++required;
			++_chatProcess->pendingFiles;
		}
		for (const auto &part : message.text) {
			if (part.type == Data::TextPart::Type::CustomEmoji) {
				if (const auto id = part.additional.toULongLong()) {
					++required;
					++_chatProcess->pendingFiles;
					_chatProcess->emojiToMessageIndices[id].push_back(i);
				}
			}
		}
		for (const auto &reaction : message.reactions) {
			if (reaction.type == Data::Reaction::Type::CustomEmoji) {
				if (const auto id = reaction.documentId.toULongLong()) {
					++required;
					++_chatProcess->pendingFiles;
					_chatProcess->emojiToMessageIndices[id].push_back(i);
				}
			}
		}
		_chatProcess->messageFilesRequired[i] = required;
	}

	if (_chatProcess->pendingFiles > 0) {
		resolveCustomEmoji();
	} else {
		finishMessagesSlice();
	}
}

void ApiWrap::collectMessagesCustomEmoji(const Data::MessagesSlice &slice) {
	for (const auto &message : slice.list) {
		for (const auto &part : message.text) {
			if (part.type == Data::TextPart::Type::CustomEmoji) {
				if (const auto id = part.additional.toULongLong()) {
					if (!_resolvedCustomEmoji.contains(id)) {
						_unresolvedCustomEmoji.emplace(id);
					}
				}
			}
		}
		for (const auto &reaction : message.reactions) {
			if (reaction.type == Data::Reaction::Type::CustomEmoji) {
				if (const auto id = reaction.documentId.toULongLong()) {
					if (!_resolvedCustomEmoji.contains(id)) {
						_unresolvedCustomEmoji.emplace(id);
					}
				}
			}
		}
	}
}

void ApiWrap::resolveCustomEmoji() {
	if (_unresolvedCustomEmoji.empty()) {
		loadNextMessageFile();
		return;
	}
	const auto count = std::min(
		int(_unresolvedCustomEmoji.size()),
		kMaxEmojiPerRequest);
	auto v = QVector<MTPlong>();
	v.reserve(count);
	const auto till = end(_unresolvedCustomEmoji);
	const auto from = end(_unresolvedCustomEmoji) - count;
	for (auto i = from; i != till; ++i) {
		v.push_back(MTP_long(*i));
	}
	_unresolvedCustomEmoji.erase(from, till);
	const auto finalize = [=] {
		for (const auto &id : v) {
			if (_resolvedCustomEmoji.contains(id.v)) {
				continue;
			}
			_resolvedCustomEmoji.emplace(
				id.v,
				Data::Document{
					.file = {
						.skipReason = Data::File::SkipReason::Unavailable,
					},
				});
		}
		resolveCustomEmoji();
	};
	mainRequest(MTPmessages_GetCustomEmojiDocuments(
		MTP_vector<MTPlong>(v)
	)).fail([=](const MTP::Error &error) {
		LOG(("Export Error: Failed to get documents for emoji."));
		finalize();
		return true;
	}).done([=](const MTPVector<MTPDocument> &result) {
		for (const auto &entry : result.v) {
			auto document = Data::ParseDocument(
				_chatProcess->context,
				entry,
				_chatProcess->info.relativePath,
				TimeId());
			_resolvedCustomEmoji.emplace(document.id, std::move(document));
		}
		finalize();
	}).send();
}

std::optional<QByteArray> ApiWrap::getCustomEmoji(QByteArray &data) {
	if (const auto id = data.toULongLong()) {
		const auto i = _resolvedCustomEmoji.find(id);
		if (i == end(_resolvedCustomEmoji)) {
			return Data::TextPart::UnavailableEmoji();
		}
		auto &file = i->second.file;
		processFileLoad(
			file,
			{ .customEmojiId = id },
			[=](FileProgress value) { return loadMessageEmojiProgress(value); },
			[=](const QString &path) { loadMessageEmojiDone(id, path); });
		return std::nullopt;
	}
	return data;
}

bool ApiWrap::messageCustomEmojiReady(Data::Message &message) {
	for (auto &part : message.text) {
		if (part.type == Data::TextPart::Type::CustomEmoji) {
			auto data = getCustomEmoji(part.additional);
			if (data.has_value()) {
				part.additional = base::take(*data);
			} else {
				return false;
			}
		}
	}
	for (auto &reaction : message.reactions) {
		if (reaction.type == Data::Reaction::Type::CustomEmoji) {
			auto data = getCustomEmoji(reaction.documentId);
			if (data.has_value()) {
				reaction.documentId = base::take(*data);
			} else {
				return false;
			}
		}
	}
	return true;
}

void ApiWrap::loadNextMessageFile() {
	Expects(_chatProcess != nullptr);
	Expects(_chatProcess->slice.has_value());

	for (int i = 0; i < int(_chatProcess->slice->list.size()); ++i) {
		auto &message = _chatProcess->slice->list[i];
		if (Data::SkipMessageByDate(message, *_settings)) {
			continue;
		}
		if (!messageCustomEmojiReady(message)) {
			return;
		}

		const auto splitIndex = _chatProcess->info.splits[
			_chatProcess->localSplitIndex];
		auto origin = Data::FileOrigin();
		origin.messageId = message.id;
		origin.split = (splitIndex >= 0)
			? splitIndex
			: (int(_splits.size()) + splitIndex);
		origin.peer = (splitIndex >= 0)
			? _chatProcess->info.input
			: _chatProcess->info.migratedFromInput;

		if (message.file().location) {		
			processFileLoad(
				message.file(),
				origin,
				[=](FileProgress value) {
					if (_chatProcess
					 && _chatProcess->fileToMessageIndex.find(value.randomId)
						 == end(_chatProcess->fileToMessageIndex)) {
						_chatProcess->fileToMessageIndex.emplace(value.randomId, i);
					}
					return loadMessageFileProgress(value);
				},
				[=](const QString &path) { loadMessageFileDone(path); },
				&message);
		}

		if (message.thumb().file.location) {
			processFileLoad(
				message.thumb().file,
				origin,
				[=](FileProgress value) {
					if (_chatProcess
					 && _chatProcess->fileToMessageIndex.find(value.randomId)
						 == end(_chatProcess->fileToMessageIndex)) {
						_chatProcess->fileToMessageIndex.emplace(value.randomId, i);
					}
					return loadMessageThumbProgress(value);
				},
				[=](const QString &path) { loadMessageThumbDone(path); },
				&message);
		}
	}

	if (_chatProcess->pendingFiles == 0) {
		finishMessagesSlice();
	}
}

void ApiWrap::finishMessagesSlice() {
	Expects(_chatProcess != nullptr);

	if (!_chatProcess->slice.has_value()) {
		if (_chatProcess->lastSlice) {
			if (++_chatProcess->localSplitIndex >= _chatProcess->info.splits.size()) {
				finishMessages();
			} else {
				_chatProcess->lastSlice = false;
				_chatProcess->largestIdPlusOne = 1;
				const auto minId = _chatProcess->fromId;
				const auto maxId = _chatProcess->tillId;
				if (minId > maxId) {
					requestMessagesSlice();
				} else {
					requestMessagesSlice();
				}
			}
		}
		return;
	}

	auto slice = *base::take(_chatProcess->slice);
	if (!slice.list.empty()) {
		_chatProcess->largestIdPlusOne = slice.list.back().id + 1;

		const auto splitIndex = _chatProcess->info.splits[_chatProcess->localSplitIndex];
		if (splitIndex < 0) {
			slice = AdjustMigrateMessageIds(std::move(slice));
		}
		if (!_chatProcess->handleSlice(std::move(slice))) {
			return; // Canceled by user.
		}
	}

	_chatProcess->fileToMessageIndex.clear();

	if (_chatProcess->lastSlice) {
		if (++_chatProcess->localSplitIndex < _chatProcess->info.splits.size()) {
			_chatProcess->lastSlice = false;
			_chatProcess->largestIdPlusOne = 1;
			requestMessagesSlice();
		} else {
			finishMessages();
		}
	} else {
		requestMessagesSlice();
	}
}

bool ApiWrap::loadMessageFileProgress(FileProgress progress) {
	const auto it = _fileProcesses.find(progress.randomId);
	if (it == end(_fileProcesses)) {
		return false;
	}
	const auto &process = *it->second;

	Expects(_chatProcess != nullptr);

	auto mIt = _chatProcess->fileToMessageIndex.find(progress.randomId);
	int messageIndexInSlice = 0;
	if (mIt != end(_chatProcess->fileToMessageIndex)) {
		messageIndexInSlice = mIt->second;
	} else if (_chatProcess->slice) {
		auto &s = *_chatProcess->slice;
		for (int i = 0; i < int(s.list.size()); ++i) {
			const auto &msg = s.list[i];
			if ((msg.file().randomId && msg.file().randomId == progress.randomId)
			 || (msg.thumb().file.randomId && msg.thumb().file.randomId == progress.randomId)) {
				_chatProcess->fileToMessageIndex.emplace(progress.randomId, i);
				messageIndexInSlice = i;
				break;
			}
		}
	}

	const int itemIndex = _chatProcess->messagesProcessed + messageIndexInSlice;

	return _chatProcess->fileProgress({
		.randomId = process.randomId,
		.path = process.relativePath,
		.itemIndex = itemIndex,
		.ready = progress.ready,
		.total = progress.total });
}


void ApiWrap::loadMessageFileDone(const QString &relativePath) {
	Expects(_chatProcess != nullptr);
	--_chatProcess->pendingFiles;
	Assert(_chatProcess->pendingFiles >= 0);
	if (_chatProcess->pendingFiles == 0) {
		finishMessagesSlice();
	}
}

bool ApiWrap::loadMessageThumbProgress(FileProgress progress) {
	return loadMessageFileProgress(progress);
}

void ApiWrap::loadMessageThumbDone(const QString &relativePath) {
	Expects(_chatProcess != nullptr);
	--_chatProcess->pendingFiles;
	Assert(_chatProcess->pendingFiles >= 0);
	if (_chatProcess->pendingFiles == 0) {
		finishMessagesSlice();
	}
}

bool ApiWrap::loadMessageEmojiProgress(FileProgress progress) {
	return loadMessageFileProgress(progress);
}

void ApiWrap::loadMessageEmojiDone(uint64 id, const QString &relativePath) {
	const auto i = _resolvedCustomEmoji.find(id);
	if (i != end(_resolvedCustomEmoji)) {
		i->second.file.relativePath = relativePath;
		if (relativePath.isEmpty()) {
			i->second.file.skipReason = Data::File::SkipReason::Unavailable;
		}
	}
	if (_chatProcess && _chatProcess->slice) {
		auto j = _chatProcess->emojiToMessageIndices.find(id);
		if (j != end(_chatProcess->emojiToMessageIndices)) {
			for (const auto messageIndex : j->second) {
				// Inline onMessagePartDone(messageIndex)
				if (messageIndex >= 0
				 && messageIndex < int(_chatProcess->messageFilesRequired.size())) {
					auto &done = _chatProcess->messageFilesDone[messageIndex];
					const auto need = _chatProcess->messageFilesRequired[messageIndex];
					if (++done == need) {
						++_chatProcess->messagesProcessed;
					}
				}
				--_chatProcess->pendingFiles;
			}
			_chatProcess->emojiToMessageIndices.erase(j);
			if (_chatProcess->pendingFiles == 0) {
				finishMessagesSlice();
				return;
			}
		}
	}
	if (_chatProcess && _chatProcess->slice) {
		loadNextMessageFile();
	}
}

void ApiWrap::finishMessages() {
	Expects(_chatProcess != nullptr);
	Expects(!_chatProcess->slice.has_value());

	const auto process = base::take(_chatProcess);
	process->done();
}


void ApiWrap::processFileLoad(
		Data::File &file,
		const Data::FileOrigin &origin,
		Fn<bool(FileProgress)> progress,
		FnMut<void(QString)> done,
		Data::Message *message,
		Data::Story *story) {
	using SkipReason = Data::File::SkipReason;

	if (!file.relativePath.isEmpty()
		|| file.skipReason != SkipReason::None) {
		done(file.relativePath);
		return;
	} else if (!file.location && file.content.isEmpty()) {
		file.skipReason = SkipReason::Unavailable;
		done(QString());
		return;
	} else if (writePreloadedFile(file, origin)) {
		done(file.relativePath);
		return;
	}

	using Type = MediaSettings::Type;
	const auto media = message
		? &message->media
		: story
		? &story->media
		: nullptr;
	const auto type = media ? v::match(media->content, [&](
			const Data::Document &data) {
		if (data.isSticker) {
			return Type::Sticker;
		} else if (data.isVideoMessage) {
			return Type::VideoMessage;
		} else if (data.isVoiceMessage) {
			return Type::VoiceMessage;
		} else if (data.isAnimated) {
			return Type::GIF;
		} else if (data.isVideoFile) {
			return Type::Video;
		} else {
			return Type::File;
		}
	}, [](const auto &data) {
		return Type::Photo;
	}) : Type(0);

	const auto fullSize = message
		? message->file().size
		: story
		? story->file().size
		: file.size;
	if (!story && (_settings->media.types & type) != type) {
		file.skipReason = SkipReason::FileType;
		done(QString());
		return;
	} else if (!story && fullSize >= _settings->media.sizeLimit) {
		file.skipReason = SkipReason::FileSize;
		done(QString());
		return;
	}
    if (_stats
        && origin.messageId != 0
        && !file.suggestedPath.endsWith(u"_thumb.jpg"_q)) {
        _stats->incrementUserMediaFiles();
    }
	loadFile(file, origin, std::move(progress), std::move(done));
}

bool ApiWrap::writePreloadedFile(
		Data::File &file,
		const Data::FileOrigin &origin) {
	Expects(_settings != nullptr);

	using namespace Output;

	if (const auto path = _fileCache->find(file.location)) {
		file.relativePath = *path;
		return true;
	} else if (!file.content.isEmpty()) {
		auto process = prepareFileProcess(file, origin);
		if (const auto result = process->outputFile.writeBlock(file.content)) {
			file.relativePath = process->relativePath;
			_fileCache->save(file.location, file.relativePath);
		} else {
			ioError(result);
		}
		return true;
	}
	return false;
}

void ApiWrap::loadFile(
		Data::File &file,
		const Data::FileOrigin &origin,
		Fn<bool(FileProgress)> progress,
		FnMut<void(QString)> done) {
	Expects(file.location);

	auto process = prepareFileProcess(file, origin);
	process->progress = std::move(progress);
	process->done = std::move(done);

	const auto randomId = process->randomId;
	file.randomId = randomId;
	process->location.randomId = randomId;

	_fileProcesses.emplace(randomId, std::move(process));
	_fileDownloadQueue.push_back(randomId);

	scheduleMoreFiles();
}

std::unique_ptr<ApiWrap::FileProcess> ApiWrap::prepareFileProcess(
	Data::File &file,
	const Data::FileOrigin &origin) const
{
	Expects(_settings != nullptr);

	const auto relativePath = Output::File::PrepareRelativePath(
		_settings->path,
		file.suggestedPath);
	
	const auto fullPath = _settings->path + relativePath;
	auto result = std::make_unique<FileProcess>(
		file,
		fullPath,
		_stats);

	result->relativePath = relativePath;
	result->location = file.location;
	result->size = file.size;
	result->origin = origin;
	result->randomId = base::RandomValue<uint64>();
	return result;
}

void ApiWrap::scheduleMoreFiles() {
	while (!_fileDownloadQueue.empty()) {
		if (_filesDownloading >= kMaxParallelFiles) {
			break;
		}

		const auto randomId = _fileDownloadQueue.front();
		const auto it = _fileProcesses.find(randomId);
		if (it == end(_fileProcesses)) {
			_fileDownloadQueue.pop_front();
			continue;
		}

		_fileDownloadQueue.pop_front();
		auto &process = *it->second;
		process.active = true;
		++_filesDownloading;

		loadFilePart(process);
	}

	for (auto &pair : _fileProcesses) {
		auto &process = *pair.second;
		if (process.active) {
			loadFilePart(process);
		}
	}
}


void ApiWrap::finishFile(uint64 randomId, const QString &relativePath) {
	auto it = _fileProcesses.find(randomId);
	if (it == end(_fileProcesses)) {
		return;
	}
	auto process = std::move(it->second);
	_fileProcesses.erase(it);

	if (process->active) {
		--_filesDownloading;
	}

	process->fileRef.relativePath = relativePath;
	if (relativePath.isEmpty()) {
		process->fileRef.skipReason = Data::File::SkipReason::Unavailable;
	} else {
		_fileCache->save(process->location, relativePath);
	}

	process->done(relativePath);

	scheduleMoreFiles();
}

void ApiWrap::loadFilePart(FileProcess &process) {
	if (!process.active) {
		return;
	}

	const auto randomId = process.randomId;
	const auto requestsCount = GetConcurrentChunksForFile(process.size);
	const auto chunkSize = GetChunkSizeForFile(process.size);

	// Count how many requests are already in flight or scheduled in the throttler
	const auto currentScheduled = int(process.scheduledOffsets.size());
	if (currentScheduled >= requestsCount) {
		return;
	}

	// How many more chunks can we schedule?
	int slotsAvailable = requestsCount - currentScheduled;

	// First, retry failed offsets (if any)
	while (slotsAvailable > 0 && !process.pendingRetryOffsets.empty()) {
		const auto retryOffset = process.pendingRetryOffsets.front();
		process.pendingRetryOffsets.pop_front();

		// If this offset is already scheduled, skip it
		if (process.scheduledOffsets.contains(retryOffset)) {
			continue;
		}

		auto &requests = process.requests;
		const auto rIt = ranges::find(
			requests,
			retryOffset,
			[](const FileProcess::Request &r) { return r.offset; });
		if (rIt == end(requests)) {
			requests.push_back({ retryOffset });
		}

		process.scheduledOffsets.insert(retryOffset);

		// Schedule via throttler - look up process by ID inside lambda
		_throttler.schedule([=] {
			const auto it = _fileProcesses.find(randomId);
			if (it == end(_fileProcesses) || !it->second->active) {
				return; // Process was removed or deactivated
			}
			auto &proc = *it->second;

			const auto requestId = fileRequest(
				proc.location,
				retryOffset,
				chunkSize
			).done([=](const MTPupload_File &result) {
				filePartDone(randomId, retryOffset, result);
			}).fail([=](const MTP::Error &error) {
				// Handle errors - same as before but look up by ID
				if (const auto itp = _fileProcesses.find(randomId); itp != end(_fileProcesses)) {
					auto &p = *itp->second;
					p.scheduledOffsets.erase(retryOffset);
					for (auto it = p.activeRequestOffsets.begin(); it != p.activeRequestOffsets.end();) {
						if (it->second == retryOffset) it = p.activeRequestOffsets.erase(it);
						else ++it;
					}
				}

				if (error.type() == u"LOCATION_INVALID"_q
					|| error.type() == u"VERSION_INVALID"_q
					|| error.type() == u"LOCATION_NOT_AVAILABLE"_q) {
					filePartUnavailable(randomId);
					return true;
				} else if (error.code() == 400
					&& error.type().startsWith(u"FILE_REFERENCE_"_q)) {
					filePartRefreshReference(randomId, retryOffset);
					return true;
				}

				if (error.code() == 420 || error.type().startsWith(u"FLOOD_WAIT"_q)) {
					static const auto FloodWaitRegExp = QRegularExpression("^FLOOD_WAIT_(\\d+)$");
					const auto match = FloodWaitRegExp.match(error.type());
					const int seconds = match.hasMatch() ? match.captured(1).toInt() : 2;
					if (const auto itp = _fileProcesses.find(randomId); itp != end(_fileProcesses)) {
						auto &p = *itp->second;
						if (std::find(p.pendingRetryOffsets.begin(), p.pendingRetryOffsets.end(), retryOffset)
							== p.pendingRetryOffsets.end()) {
							p.pendingRetryOffsets.push_back(retryOffset);
						}
					}
					scheduleBatchDelay(seconds * 1000);
					return true;
				}

				if (error.code() >= 500 || error.type() == u"TIMEOUT"_q
					|| error.type() == u"RPC_CALL_FAIL"_q || error.type() == u"INTERNAL"_q) {
					if (const auto itp = _fileProcesses.find(randomId); itp != end(_fileProcesses)) {
						auto &p = *itp->second;
						const int tries = ++p.retryCounts[retryOffset];
						if (tries <= kMaxChunkRetries) {
							p.pendingRetryOffsets.push_back(retryOffset);
							scheduleBatchDelay(std::min(kRetryBaseDelayMs << (tries - 1), kRetryMaxDelayMs));
							return true;
						}
					}
					filePartUnavailable(randomId);
					return true;
				}

				return false;
			}).send();

			if (requestId) {
				proc.activeRequestOffsets.emplace(requestId, retryOffset);
			} else {
				// Failed to send request immediately
				if (const auto itp = _fileProcesses.find(randomId); itp != end(_fileProcesses)) {
					auto &p = *itp->second;
					p.scheduledOffsets.erase(retryOffset);
					p.pendingRetryOffsets.push_back(retryOffset);
				}
			}
		});

		--slotsAvailable;
	}

	// Then fill remaining slots with fresh offsets
	while (slotsAvailable > 0) {
		if (process.size > 0 && process.offset >= process.size) {
			break;
		}

		const auto offset = process.offset;
		process.requests.push_back({ offset });
		process.offset += chunkSize;
		process.scheduledOffsets.insert(offset);

		// Schedule via throttler - look up process by ID inside lambda
		_throttler.schedule([=] {
			const auto it = _fileProcesses.find(randomId);
			if (it == end(_fileProcesses) || !it->second->active) {
				return; // Process was removed or deactivated
			}
			auto &proc = *it->second;

			const auto requestId = fileRequest(
				proc.location,
				offset,
				chunkSize
			).done([=](const MTPupload_File &result) {
				filePartDone(randomId, offset, result);
			}).fail([=](const MTP::Error &error) {
				// Handle errors
				if (const auto itp = _fileProcesses.find(randomId); itp != end(_fileProcesses)) {
					auto &p = *itp->second;
					p.scheduledOffsets.erase(offset);
					for (auto it = p.activeRequestOffsets.begin(); it != p.activeRequestOffsets.end();) {
						if (it->second == offset) it = p.activeRequestOffsets.erase(it);
						else ++it;
					}
				}

				if (error.type() == u"TAKEOUT_FILE_EMPTY"_q && _otherDataProcess != nullptr) {
					filePartDone(randomId, 0, MTP_upload_file(
						MTP_storage_filePartial(), MTP_int(0), MTP_bytes()));
					return true;
				} else if (error.type() == u"LOCATION_INVALID"_q
					|| error.type() == u"VERSION_INVALID"_q
					|| error.type() == u"LOCATION_NOT_AVAILABLE"_q) {
					filePartUnavailable(randomId);
					return true;
				} else if (error.code() == 400
					&& error.type().startsWith(u"FILE_REFERENCE_"_q)) {
					filePartRefreshReference(randomId, offset);
					return true;
				}

				if (error.code() == 420 || error.type().startsWith(u"FLOOD_WAIT"_q)) {
					static const auto FloodWaitRegExp = QRegularExpression("^FLOOD_WAIT_(\\d+)$");
					const auto match = FloodWaitRegExp.match(error.type());
					const int seconds = match.hasMatch() ? match.captured(1).toInt() : 2;
					if (const auto itp = _fileProcesses.find(randomId); itp != end(_fileProcesses)) {
						auto &p = *itp->second;
						if (std::find(p.pendingRetryOffsets.begin(), p.pendingRetryOffsets.end(), offset)
							== p.pendingRetryOffsets.end()) {
							p.pendingRetryOffsets.push_back(offset);
						}
					}
					scheduleBatchDelay(seconds * 1000);
					return true;
				}

				if (error.code() >= 500 || error.type() == u"TIMEOUT"_q
					|| error.type() == u"RPC_CALL_FAIL"_q || error.type() == u"INTERNAL"_q) {
					if (const auto itp = _fileProcesses.find(randomId); itp != end(_fileProcesses)) {
						auto &p = *itp->second;
						const int tries = ++p.retryCounts[offset];
						if (tries <= kMaxChunkRetries) {
							p.pendingRetryOffsets.push_back(offset);
							scheduleBatchDelay(std::min(kRetryBaseDelayMs << (tries - 1), kRetryMaxDelayMs));
							return true;
						}
					}
					filePartUnavailable(randomId);
					return true;
				}

				return false;
			}).send();

			if (requestId) {
				proc.activeRequestOffsets.emplace(requestId, offset);
			} else {
				// Failed to send request immediately
				if (const auto itp = _fileProcesses.find(randomId); itp != end(_fileProcesses)) {
					auto &p = *itp->second;
					p.scheduledOffsets.erase(offset);
					p.pendingRetryOffsets.push_back(offset);
				}
			}
		});

		--slotsAvailable;

		if (process.size == 0) {
			break; // Unknown size, only request one chunk at a time
		}
	}

	if (!process.pendingRetryOffsets.empty()) {
		scheduleBatchDelay(100);
	}
}

void ApiWrap::filePartDone(
		uint64 randomId,
		int64 offset,
		const MTPupload_File &result) {
	const auto it = _fileProcesses.find(randomId);
	if (it == end(_fileProcesses)) {
		return;
	}
	auto &process = *it->second;

	auto removed = false;
	process.scheduledOffsets.erase(offset);
	for (auto i = begin(process.activeRequestOffsets); i != end(process.activeRequestOffsets); ++i) {
		if (i->second == offset) {
			process.activeRequestOffsets.erase(i);
			removed = true;
			break;
		}
	}
	if (!removed && offset != 0) {
		return;
	}

	// Clear retry bookkeeping for this offset on success.
	if (const auto itc = process.retryCounts.find(offset); itc != process.retryCounts.end()) {
		process.retryCounts.erase(itc);
	}
	if (!process.pendingRetryOffsets.empty()) {
		const auto e = std::remove(process.pendingRetryOffsets.begin(), process.pendingRetryOffsets.end(), offset);
		if (e != process.pendingRetryOffsets.end()) {
			process.pendingRetryOffsets.erase(e, process.pendingRetryOffsets.end());
		}
	}

	if (result.type() == mtpc_upload_fileCdnRedirect) {
		error("Cdn redirect is not supported.");
		return;
	}
	const auto &data = result.c_upload_file();
	const auto receivedEmpty = data.vbytes().v.isEmpty();

	if (receivedEmpty) {
		if (process.size > 0) {
			LOG(("Export Error: Empty bytes received in file part for offset %1 (size %2)").arg(offset).arg(process.size));
			filePartUnavailable(randomId);
			return;
		}
	} else {
		using Request = FileProcess::Request;
		auto &requests = process.requests;
		const auto i = ranges::find(
			requests,
			offset,
			[](const Request &request) { return request.offset; });
		Assert(i != end(requests));
		i->bytes = data.vbytes().v;
	}

	auto &outputFile = process.outputFile;
	auto &requests = process.requests;
	while (!requests.empty() && !requests.front().bytes.isEmpty()) {
		const auto &bytes = requests.front().bytes;
		if (const auto writeResult = outputFile.writeBlock(bytes); !writeResult) {
			ioError(writeResult);
			finishFile(randomId, QString());
			return;
		}
		requests.pop_front();
	}

	if (process.progress) {
		if (!process.progress({
			.randomId = randomId,
			.ready = outputFile.size(),
			.total = process.size,
		})) {
			finishFile(randomId, QString());
			return;
		}
	}

	const auto allPartsRequested = (process.size > 0)
		&& (process.offset >= process.size);
	if (process.activeRequestOffsets.empty() && process.pendingRetryOffsets.empty() && (allPartsRequested || receivedEmpty)) {
		// Mark message as fully processed if all its parts are done.
		if (_chatProcess && _chatProcess->slice) {
			const auto mIt = _chatProcess->fileToMessageIndex.find(randomId);
			if (mIt != end(_chatProcess->fileToMessageIndex)) {
				const int messageIndex = mIt->second;
				if (messageIndex >= 0
				 && messageIndex < int(_chatProcess->messageFilesRequired.size())) {
					auto &done = _chatProcess->messageFilesDone[messageIndex];
					const auto need = _chatProcess->messageFilesRequired[messageIndex];
					if (++done == need) {
						++_chatProcess->messagesProcessed;
					}
				}
			}
		}
		finishFile(randomId, process.relativePath);
	} else if (process.active) {
		// CORRECTED: A chunk slot for this file just opened up.
		// Try to immediately fill it with the next chunk for the same file.
		// No timers or delays here.
		loadFilePart(process);
		if (*_lifetimeGuard) {
			_throttler.tryProcessQueue();
		}
	}
}

void ApiWrap::filePartUnavailable(uint64 randomId) {
	LOG(("Export Error: File unavailable."));
	finishFile(randomId, QString());
}

void ApiWrap::filePartRefreshReference(uint64 randomId, int64 offset) {
	const auto it = _fileProcesses.find(randomId);
	if (it == end(_fileProcesses)) {
		return;
	}
	auto &process = *it->second;

	for (const auto &[requestId, reqOffset] : process.activeRequestOffsets) {
		_mtp.request(requestId).cancel();
		process.scheduledOffsets.erase(reqOffset);
	}
	process.activeRequestOffsets.clear();

	const auto &origin = process.origin;
	if (origin.storyId) {
		mainRequest(MTPstories_GetStoriesByID(
			MTP_inputPeerSelf(),
			MTP_vector<MTPint>(1, MTP_int(origin.storyId))
		)).fail([=](const MTP::Error &error) {
			filePartUnavailable(randomId);
			return true;
		}).done([=](const MTPstories_Stories &result) {
			filePartExtractReference(randomId, offset, result);
		}).send();
		return;
	} else if (!origin.messageId) {
		error("FILE_REFERENCE error for non-message file.");
		return;
	}
	if (origin.peer.type() == mtpc_inputPeerChannel
		|| origin.peer.type() == mtpc_inputPeerChannelFromMessage) {
		const auto channel = (origin.peer.type() == mtpc_inputPeerChannel)
			? MTP_inputChannel(
				origin.peer.c_inputPeerChannel().vchannel_id(),
				origin.peer.c_inputPeerChannel().vaccess_hash())
			: MTP_inputChannelFromMessage(
				origin.peer.c_inputPeerChannelFromMessage().vpeer(),
				origin.peer.c_inputPeerChannelFromMessage().vmsg_id(),
				origin.peer.c_inputPeerChannelFromMessage().vchannel_id());
		mainRequest(MTPchannels_GetMessages(
			channel,
			MTP_vector<MTPInputMessage>(
				1,
				MTP_inputMessageID(MTP_int(origin.messageId)))
		)).fail([=](const MTP::Error &error) {
			filePartUnavailable(randomId);
			return true;
		}).done([=](const MTPmessages_Messages &result) {
			filePartExtractReference(randomId, offset, result);
		}).send();
	} else {
		splitRequest(
			origin.split,
			MTPmessages_GetMessages(
				MTP_vector<MTPInputMessage>(
					1,
					MTP_inputMessageID(MTP_int(origin.messageId)))
			)
		).fail([=](const MTP::Error &error) {
			filePartUnavailable(randomId);
			return true;
		}).done([=](const MTPmessages_Messages &result) {
			filePartExtractReference(randomId, offset, result);
		}).send();
	}
}

void ApiWrap::filePartExtractReference(
		uint64 randomId,
		int64 offset,
		const MTPmessages_Messages &result) {
	const auto it = _fileProcesses.find(randomId);
	if (it == end(_fileProcesses)) {
		return;
	}
	auto &process = *it->second;

	const auto handle = [&](
			const MTPVector<MTPMessage> &messages,
			const MTPVector<MTPUser> &users,
			const MTPVector<MTPChat> &chats) {
		Expects(_selfId.has_value());

		auto context = Data::ParseMediaContext();
		context.selfPeerId = peerFromUser(*_selfId);
		const auto parsed = Data::ParseMessagesSlice(
			context,
			messages,
			users,
			chats,
			_chatProcess ? _chatProcess->info.relativePath : QString());
		for (const auto &message : parsed.list) {
			if (message.id == process.origin.messageId) {
				const auto refresh1 = Data::RefreshFileReference(
					process.location,
					message.file().location);
				const auto refresh2 = Data::RefreshFileReference(
					process.location,
					message.thumb().file.location);
				if (refresh1 || refresh2) {
					// Also update the original reference in the message.
					Data::RefreshFileReference(
						process.fileRef.location,
						process.location);
					loadFilePart(process);
					return;
				}
			}
		}
		filePartUnavailable(randomId);
	};

	result.match([&](const MTPDmessages_messages &data) {
		handle(data.vmessages(), data.vusers(), data.vchats());
	}, [&](const MTPDmessages_messagesSlice &data) {
		handle(data.vmessages(), data.vusers(), data.vchats());
	}, [&](const MTPDmessages_channelMessages &data) {
		handle(data.vmessages(), data.vusers(), data.vchats());
	}, [&](const MTPDmessages_messagesNotModified &data) {
		error("Unexpected messagesNotModified received.");
	});
}

void ApiWrap::filePartExtractReference(
		uint64 randomId,
		int64 offset,
		const MTPstories_Stories &result) {
	const auto it = _fileProcesses.find(randomId);
	if (it == end(_fileProcesses)) {
		return;
	}
	auto &process = *it->second;

	result.match([&](const MTPDstories_stories &data) {
		const auto stories = Data::ParseStoriesSlice(data.vstories(), 0);
		for (const auto &story : stories.list) {
			if (story.id == process.origin.storyId) {
				const auto refresh1 = Data::RefreshFileReference(
					process.location,
					story.file().location);
				const auto refresh2 = Data::RefreshFileReference(
					process.location,
					story.thumb().file.location);
				if (refresh1 || refresh2) {
					// Also update the original reference in the story.
					Data::RefreshFileReference(
						process.fileRef.location,
						process.location);
					loadFilePart(process);
					return;
				}
			}
		}
		filePartUnavailable(randomId);
	});
}

void ApiWrap::error(const MTP::Error &error) {
	LOG(("Export Error: API Error %1: %2 (%3)").arg(error.code()).arg(error.type()).arg(error.description()));
	_errors.fire_copy(error);
}

void ApiWrap::error(const QString &text) {
	error(MTP::Error(
		MTP_rpc_error(MTP_int(0), MTP_string("API_ERROR: " + text))));
}

void ApiWrap::ioError(const Output::Result &result) {
	_ioErrors.fire_copy(result);
}

ApiWrap::~ApiWrap() {
	if (_lifetimeGuard) {
		*_lifetimeGuard = false;
	}
}

} // namespace Export
