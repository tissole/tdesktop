/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/export_api_wrap.h"

#include "export/export_settings.h"
#include "export/export_global_dedup.h"
#include "export/data/export_data_types.h"
#include "export/output/export_output_result.h"
#include "export/output/export_output_file.h"
#include "export/output/export_output_stats.h"
#include "mtproto/mtproto_response.h"
#include "base/bytes.h"
#include "base/options.h"
#include "base/random.h"
#include "base/call_delayed.h"
#include "core/mime_type.h"
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <set>
#include <deque>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <algorithm>
#include <QRegularExpression>


namespace Export {
namespace {

constexpr auto kMegabyte = 1024 * 1024;

// Simple per-DC throttling:
// - Delay controls when chunk STARTS are spaced
// - Multiple chunks can be IN FLIGHT simultaneously
constexpr auto kChunkSize = 1024 * 1024;           // 1 MB per chunk
constexpr auto kSameDcDelay = 200;                 // 200ms between chunk starts (same-DC)
constexpr auto kDifferentDcDelay = 60;              // 0ms = fire as fast as possible (different-DC)
constexpr auto kSameDcConcurrentChunks = 1;        // Max chunks downloading at once (same-DC)
constexpr auto kDifferentDcConcurrentChunks = 4;   // Max chunks downloading at once (different-DC)

// Volume-based rate limiting (DISABLED - kept for future testing)
// Server allows ~86 MB per 15s window based on 115Mb/s for 6s
// To re-enable: uncomment trackBytes() and calculateDelay() in throttler
//constexpr auto kRateLimitWindowMs = 15000;        // Server's rate limit window
//constexpr auto kTargetRateBytesPerSec = 5LL * 1024 * 1024;  // 5 MB/s (~40 Mb/s) target

//int GetChunkSizeForFile(int64 fileSize, bool isSameDc) {
//	if (fileSize > 750 * kMegabyte) {
//		return 1024 * 1024;  // 1 MB  — very large files
//	} else if (fileSize > 375 * kMegabyte) {
//		return 512 * 1024;   // 512 KB — large files
//	} else if (fileSize > 32 * kMegabyte) {
//		return 256 * 1024;   // 256 KB — medium files
//	} else if (fileSize > 1 * kMegabyte) {
//		return 128 * 1024;   // 256 KB — small files
//	}
//	return 64 * 1024;       // 128 KB — very small files
//}

// All other size-based functions commented out for future testing:
//int GetChunkSizeForFile(int64 fileSize, bool isSameDc) {
//return isSameDc ? kSameDcChunkSize : kDifferentDcChunkSize;
//}

//int GetConcurrentChunksForFile(int64 fileSize, bool isSameDc) {
//	if (isSameDc) {
//		// Same-DC: size-based
//		if (fileSize >= kSameDcLargeFileThreshold) {
//			return kSameDcConcurrentChunksLarge;   // 2 for large files (>= 100 MB)
//		}
//		return kSameDcConcurrentChunksSmall;       // 1 for small files (< 100 MB)
//	}
//	// Different-DC: size-based
//	if (fileSize >= kDifferentDcLargeFileThreshold) {
//		return kDifferentDcConcurrentChunksLarge;  // 4 for large files (>= 100 MB)
//	}
//	return kDifferentDcConcurrentChunksSmall;      // 2 for small files (< 100 MB)
//}


//int GetConcurrentChunksForFile(int64 fileSize) {
//	if (fileSize > 750 * kMegabyte) {
//		return 2;  // Very large: maximum pipeline depth
//	} else if (fileSize > 375 * kMegabyte) {
//		return 2;  // Large
//	} else if (fileSize > 32 * kMegabyte) {
//		return 2;  // Medium
//	}
//	return 3;      // Small files: 2 concurrent chunks
//}


// Other slice limits.
constexpr auto kUserpicsSliceLimit = 100;
constexpr auto kChatsSliceLimit = 100;
constexpr auto kMessagesSliceLimit = 100;
constexpr auto kTopPeerSliceLimit = 100;
constexpr auto kFileMaxSize = 4000 * int64(1024 * 1024);
constexpr auto kLocationCacheSize = 1'000'000; // kept for LoadedFileCache ctor
// constexpr auto kDedupMapLimit = 1'000'000;
constexpr auto kMaxEmojiPerRequest = 100;
constexpr auto kStoriesSliceLimit = 100;
constexpr auto kProfileMusicSliceLimit = 100;

ApiWrap::LocationKey ComputeLocationKey(const Data::FileLocation &value) {
	auto result = ApiWrap::LocationKey();
	value.data.match([&](const MTPDinputDocumentFileLocation &data) {
		result.type = (2ULL << 24);
		result.id = data.vid().v;
	}, [&](const MTPDinputPhotoFileLocation &data) {
		result.type = (6ULL << 24);
		result.id = data.vid().v;
	}, [&](const MTPDinputTakeoutFileLocation &data) {
		result.type = (5ULL << 24);
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

uint32 CalculateTakeoutFlags(const Settings &settings, bool isScanning) {
	using Type = Settings::Type;
	const auto sizeLimit = isScanning ? kFileMaxSize : settings.media.sizeLimit;
	const auto hasFiles = isScanning
		|| settings.media.types
		|| (settings.types & Type::Userpics)
		|| (settings.types & Type::Stories);

	using Flag = MTPaccount_initTakeoutSession::Flag;
	return static_cast<uint32>(Flag(0)
		| (settings.types & Type::Contacts ? Flag::f_contacts : Flag(0))
		| (hasFiles ? Flag::f_files : Flag(0))
		| ((hasFiles && sizeLimit < kFileMaxSize)
			? Flag::f_file_max_size
			: Flag(0))
		| (settings.types & (Type::PersonalChats | Type::BotChats)
			? Flag::f_message_users
			: Flag(0))
		| (settings.types & Type::PrivateGroups
			? (Flag::f_message_chats | Flag::f_message_megagroups)
			: Flag(0))
		| (settings.types & Type::PublicGroups
			? Flag::f_message_megagroups
			: Flag(0))
		| (settings.types & (Type::PrivateChannels | Type::PublicChannels)
			? Flag::f_message_channels
			: Flag(0)));
}

} // namespace

ApiWrap::RequestThrottler::RequestThrottler(
	Fn<void(FnMut<void()>)> runner,
	std::shared_ptr<bool> guard,
	crl::time batchDelay)
: _runner(runner)
, _guard(std::move(guard))
, _batchDelayMs(batchDelay) {
}

ApiWrap::RequestThrottler::~RequestThrottler() = default;

void ApiWrap::RequestThrottler::schedule(FnMut<void()> task) {
	_runner([this, guard = _guard, task = std::move(task)]() mutable {
		if (!*guard) {
			return;
		}
		_taskQueue.push_back(std::move(task));
		// Start processing if not already processing
		if (!_processing) {
			processNext();
		}
	});
}

void ApiWrap::RequestThrottler::processNext() {
	Expects(!_taskQueue.empty());
	
	_processing = true;
	
	// Calculate actual delay needed based on last fire time
	const auto now = crl::now();
	const auto elapsedSinceLastFire = now - _lastFireTime;
	const auto remainingDelay = (_lastFireTime > 0)
		? std::max(crl::time(0), _batchDelayMs - elapsedSinceLastFire)
		: crl::time(0);
	
	// If we need to wait, schedule delayed fire
	if (remainingDelay > 0) {
		const auto delay = remainingDelay;
		const auto runner = _runner;
		const auto guard = _guard;
		
		crl::on_main([=] {
			base::call_delayed(delay, [=] {
				if (!*guard) {
					return;
				}
				runner([=] {
					if (!*guard) {
						return;
					}
					fireNextAndSchedule();
				});
			});
		});
	} else {
		// No delay needed, fire immediately
		fireNextAndSchedule();
	}
}

void ApiWrap::RequestThrottler::fireNextAndSchedule() {
	// Fire ONE chunk from queue
	auto task = std::move(_taskQueue.front());
	_taskQueue.pop_front();
	
	// Record fire time for spacing
	_lastFireTime = crl::now();
	
	task();
	
	// If queue still has tasks, schedule next
	if (!_taskQueue.empty()) {
		const auto delay = _batchDelayMs;
		const auto runner = _runner;
		const auto guard = _guard;
		
		crl::on_main([=] {
			base::call_delayed(delay, [=] {
				if (!*guard) {
					return;
				}
				runner([=] {
					if (!*guard) {
						return;
					}
					processNext();
				});
			});
		});
	} else {
		_processing = false;
	}
}

// Volume tracking (DISABLED - kept for future testing)
//void ApiWrap::RequestThrottler::trackBytes(int64 bytes) {
//	_bytesTransferred += bytes;
//	
//	if (_windowStart == 0) {
//		_windowStart = crl::now();
//	}
//	
//	const auto elapsed = crl::now() - _windowStart;
//	if (elapsed >= kRateLimitWindowMs) {
//		// Reset window
//		_bytesTransferred = 0;
//		_windowStart = crl::now();
//	}
//}
//
//crl::time ApiWrap::RequestThrottler::calculateDelay() const {
//	const auto elapsed = crl::now() - _windowStart;
//	if (elapsed == 0 || _windowStart == 0) return 0;
//	
//	const auto elapsedSec = elapsed / 1000.0;
//	const auto currentRate = _bytesTransferred / elapsedSec;  // bytes/s
//	
//	if (currentRate > kTargetRateBytesPerSec) {
//		// Going too fast - calculate how long to wait
//		const auto targetBytes = kTargetRateBytesPerSec * elapsedSec;
//		const auto excessBytes = _bytesTransferred - targetBytes;
//		const auto waitMs = crl::time(excessBytes * 1000.0 / kTargetRateBytesPerSec);
//		return std::max(crl::time(50), waitMs);
//	}
//	
//	return 0;  // Under limit, no delay needed
//}

struct ApiWrap::StartProcess {
	FnMut<void(StartInfo)> done;

	enum class Step {
		UserpicsCount,
		StoriesCount,
		MediaCounts,
		ProfileMusicCount,
		SplitRanges,
		DialogsCount,
		LeftChannelsCount,
	};
	std::deque<Step> steps;
	int splitIndex = 0;
	int pendingCounts = 0;
	StartInfo info;
};

struct ApiWrap::ContactsProcess {
	FnMut<void(Data::ContactsList&&)> done;

	Data::ContactsList result;

	int topPeersOffset = 0;
};

struct ApiWrap::UserpicsProcess {
	FnMut<bool(Data::UserpicsInfo&&)> start;
	Fn<bool(ApiWrap::DownloadProgress)> fileProgress;
	Fn<bool(Data::UserpicsSlice&&)> handleSlice;
	FnMut<void()> finish;

	int processed = 0;
	std::optional<Data::UserpicsSlice> slice;
	uint64 maxId = 0;
	bool lastSlice = false;
	int pendingFiles = 0;
	bool processing = false;
};

struct ApiWrap::StoriesProcess {
	FnMut<bool(Data::StoriesInfo&&)> start;
	Fn<bool(ApiWrap::DownloadProgress)> fileProgress;
	Fn<bool(Data::StoriesSlice&&)> handleSlice;
	FnMut<void()> finish;

	int processed = 0;
	std::optional<Data::StoriesSlice> slice;
	int offsetId = 0;
	bool lastSlice = false;
	int pendingFiles = 0;
	bool processing = false;
};

struct ApiWrap::ProfileMusicProcess {
	FnMut<bool(Data::ProfileMusicInfo&&)> start;
	Fn<bool(DownloadProgress)> fileProgress;
	Fn<bool(Data::ProfileMusicSlice&&)> handleSlice;
	FnMut<void()> finish;

	int processed = 0;
	std::optional<Data::ProfileMusicSlice> slice;
	int offsetId = 0;
	bool lastSlice = false;
	int fileIndex = 0;
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

	FileProcess(Data::File &file, const QString &fullPath, Output::Stats *stats, int64 initialOffset)
	: fileRef(file)
	, outputFile(fullPath, initialOffset, stats) {
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
	std::set<int64> scheduledOffsets;					   // offsets currently scheduled or in-flight
	std::deque<int64> pendingRetryOffsets;				   // offsets that need retry
	std::unordered_map<int64, int> retryCounts;			   // per-offset retry counter
	bool active = false;
	LocationKey dedupKey;
	// Callbacks from duplicate files waiting for this download to finish.
	std::vector<FnMut<void(QString)>> pendingDone;
	// Dedup tracking for global dedup manager
	uint64 dedupDocId = 0;
	SizeNameKey dedupSizeName;
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

struct ApiWrap::AbstractMessagesProcess {
	Fn<bool(DownloadProgress)> fileProgress;
	Fn<bool(Data::MessagesSlice&&)> handleSlice;
	FnMut<void()> done;

	FnMut<void(MTPmessages_Messages&&)> requestDone;

	Data::ParseMediaContext context;
	std::optional<Data::MessagesSlice> slice;
	bool lastSlice = false;
	int fileIndex = 0;
};

struct ApiWrap::ChatProcess : AbstractMessagesProcess {
	Data::DialogInfo info;
	int64 fromId = 0;
	int64 tillId = 0;

	FnMut<bool(const Data::DialogInfo &)> start;

	int localSplitIndex = 0;
	int32 largestIdPlusOne = 1;

	int pendingFiles = 0;
	bool processing = false;

	// Track items processed
	int messagesProcessed = 0;
	int messagesTotalCount = 0;

	struct MessageStats {
		MediaSettings::Type type = MediaSettings::Type::Text;
		int64 size = 0;
		bool unique = false;
		int links = 0;
		int linksUnique = 0;
		int linkMsgIncr = 0;
		bool selected = false;
		bool withinRange = false;
	};
	std::vector<MessageStats> messageStats;

	// Map file randomId -> message index in current slice
	std::unordered_map<uint64, int> fileToMessageIndex;

	// Per-message parts in current slice
	std::vector<int> messageFilesRequired;
	std::vector<int> messageFilesDone;

	base::flat_set<LocationKey> seenLocations;

	Data::MessagesSlice pendingBatch;
	struct BatchStats {
		int localTotalCount = 0;
		int64 totalSize = 0;
		int uniqueCount = 0;
		int64 uniqueSize = 0;
		int messagesWithLinks = 0;
	};
	std::map<MediaSettings::Type, BatchStats> batchStats;
	int batchProcessed = 0;

	// Emoji id -> list of message indices depending on that emoji in current slice
	std::unordered_map<uint64, std::vector<int>> emojiToMessageIndices;
};

struct ApiWrap::TopicProcess : AbstractMessagesProcess {
	PeerId peerId = 0;
	MTPInputPeer inputPeer;
	int32 topicRootId = 0;
	QString relativePath;

	FnMut<bool(int count)> start;

	int32 offsetId = 0;
	int localTotalCount = 0;
	int processedCount = 0;
	bool processing = false;
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

template <typename Request>
auto ApiWrap::mainRequest(Request &&request, std::optional<uint64> takeoutId) {
	const auto id = takeoutId ? takeoutId : _takeoutId;
	Expects(id.has_value());

	auto original = std::move(_mtp.request(MTPInvokeWithTakeout<Request>(
		MTP_long(*id),
		std::forward<Request>(request)
	)).toDC(MTP::ShiftDcId(0, MTP::kExportDcShift)));

	return RequestBuilder<MTPInvokeWithTakeout<Request>>(
		std::move(original),
		[=](const MTP::Error &result) { error(result); });
}

// Non-takeout request for restricted chats
template <typename Request>
auto ApiWrap::normalRequest(Request &&request) {
	auto original = std::move(_mtp.request(std::forward<Request>(request)));

	return RequestBuilder<Request>(
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

ApiWrap::ApiWrap(base::weak_qptr<MTP::Instance> weak, Fn<void(FnMut<void()>)> runner, int mainDcId)
: _mtp(weak, runner)
, _mainDcId(mainDcId)
, _lifetimeGuard(std::make_shared<bool>(true))
, _throttlerSameDc(runner, _lifetimeGuard, kSameDcDelay)
, _throttlerDifferentDc(runner, _lifetimeGuard, kDifferentDcDelay)
{
}

void ApiWrap::scheduleBatchDelay(crl::time delay) {
	const auto runner = _throttlerSameDc.runner();
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
		FnMut<void(StartInfo)> done,
		bool isScanning,
		Output::Stats *scanStats) {
	_settings = std::make_unique<Settings>(settings);
	_stats = stats;
	_isScanning = isScanning;
	_scanStats = scanStats;
	_usingServerCounts = false;
	_serverCountTrustedTypes.clear();
	_dedupById.clear();
	_dedupBySizeName.clear();
	_dedupByIdInProgress.clear();
	_dedupBySizeNameInProgress.clear();
	_visitedLinks.clear();
	_reservedPaths.clear();
	_serverTotalCount = 0;
	_chatProcess = nullptr;
	_startProcess = std::make_unique<StartProcess>();
	_startProcess->done = std::move(done);

	if (!_settings->path.isEmpty()) {
		loadProgress(_settings->path);

		// Initialize global dedup manager in the parent directory (cross-chat)
		if (!_globalDedupOwned) {
			const auto parentPath = QFileInfo(_settings->path).absolutePath();
			_globalDedupOwned = std::make_unique<GlobalDedupManager>(parentPath);
			_globalDedup = _globalDedupOwned.get();
			_globalDedup->load();
		}

		if (_exportProgress && !_isScanning) {
			_dedupById.clear();
			for (const auto &[id, path] : _exportProgress->dedupById) {
				_dedupById[id] = path;
			}
			for (const auto &[mapKey, path] : _exportProgress->dedupBySizeName) {
				const int underscorePos = mapKey.indexOf('_');
				if (underscorePos > 0) {
					const int64 size = mapKey.left(underscorePos).toLongLong();
					const QString name = mapKey.mid(underscorePos + 1);
					_dedupBySizeName[{size, name}] = path;
				}
			}

			if (_resumeMode) {
				// Restore type-specific stats so the second row counters are correct
				_stats->clear();
				for (const auto &[typeInt, counter] : _exportProgress->typeCounters) {
					const auto type = static_cast<MediaSettings::Type>(typeInt);
					_stats->increment(type, counter.totalSize, counter.uniqueSize, counter.localTotalCount, counter.uniqueCount, counter.messagesWithLinks);
					if (type != MediaSettings::Type::Text && type != MediaSettings::Type::Link) {
						for (int i = 0; i < counter.uniqueCount; ++i) {
							_stats->incrementUserMediaFiles();
						}
					}
				}
				_stats->setTotalMessages(_exportProgress->messagesProcessed);

				_scanStats->clear();
				for (const auto &[typeInt, counter] : _exportProgress->scanStats) {
					const auto type = static_cast<MediaSettings::Type>(typeInt);
					_scanStats->increment(type, counter.totalSize, counter.uniqueSize, counter.localTotalCount, counter.uniqueCount, counter.messagesWithLinks);
					if (type != MediaSettings::Type::Text && type != MediaSettings::Type::Link) {
						for (int i = 0; i < counter.uniqueCount; ++i) {
							_scanStats->incrementUserMediaFiles();
						}
					}
				}
				_scanStats->setTotalMessages(_exportProgress->scanTotalMessages);
			} else {
				_exportProgress->lastMessageId = 0;
				_exportProgress->messagesProcessed = 0;
				_exportProgress->typeCounters.clear();
				_exportProgress->incompleteFiles.clear();
				_exportProgress->settings = *_settings;
			}
		} else {
			_exportProgress = std::make_unique<ExportProgress>();
			_exportProgress->settings = *_settings;
		}

		if (!_resumeMode) {
			QDir dir(_settings->path);
			const auto partialFiles = dir.entryList(QStringList() << "*.partial", QDir::Files | QDir::NoDotAndDotDot);
			if (!partialFiles.isEmpty()) {
				for (const auto &partial : partialFiles) {
					const QFileInfo fi(_settings->path + '/' + partial);
					IncompleteFile incomplete;
					incomplete.filename = partial.mid(0, partial.length() - 8);
					incomplete.bytesDownloaded = fi.size();
					incomplete.totalSize = 0;
					incomplete.messageId = 0;
					_exportProgress->incompleteFiles.push_back(std::move(incomplete));
				}
			}
			saveProgress();
		}
	}

	const bool hasDateOrIdRange = (_settings->singlePeerFrom != 0)
		|| (_settings->singlePeerTill != 0)
		|| _settings->useIdRange;
	const bool hasExtFilter =
		_settings->media.extensionFilterMode != MediaSettings::ExtFilterMode::None
		&& !_settings->media.extensionFilter.isEmpty();

	// Check if exactly one media type is selected OR a supported combo (for server filter optimization)
	const auto types = _settings->media.types;
	using Type = MediaSettings::Type;
	// Exclude types without server filter (must count locally): Text, Sticker, FullHistory
	const auto serverIndexedTypes = types & ~(Type::Text | Type::Sticker | Type::FullHistory);

	// Only trust server counts when:
	// - No extension filter
	// - No date/id range
	// - Full Size (4000 MB)
	// - Single media type OR supported pairs (Photo+Video, Voice+Round)
	// - Selected type has server index (Text/Sticker/FullHistory excluded)
	// - Single peer export
	const bool hasServerIndexedType = (serverIndexedTypes != 0);
	const bool noRange = !hasDateOrIdRange;
	const bool fullSize = (_settings->media.sizeLimit == kFileMaxSize);

	// Single media type check (for server filter)
	const bool singleMedia = serverIndexedTypes != 0
		&& (serverIndexedTypes & (serverIndexedTypes - 1)) == 0;

	// Supported pairs (Photo+Video, Voice+Round)
	const bool photoVideo = (types & Type::Photo) && (types & Type::Video);
	const bool voiceRound = (types & Type::VoiceMessage) && (types & Type::VideoMessage);
	const bool supportedPair = photoVideo || voiceRound;

	if (!hasExtFilter
			&& noRange
			&& fullSize
			&& (singleMedia || supportedPair)
			&& hasServerIndexedType
			&& _settings->onlySinglePeer()) {
		_usingServerCounts = true;
	}

	using Step = StartProcess::Step;
	if (_settings->types & Settings::Type::Userpics) {
		_startProcess->steps.push_back(Step::UserpicsCount);
	}
	if (_settings->types & Settings::Type::Stories) {
		_startProcess->steps.push_back(Step::StoriesCount);
	}
	// When scanning with an extension filter, skip the MediaCounts server round-trips.
	// Server counts are unfiltered so they are wrong for stats, and since
	// _usingServerCounts is already false in this case, they serve no purpose.
	// Skipping them means scan starts immediately instead of waiting for N responses.
	const bool skipMediaCountsForFilteredScan = _isScanning && hasExtFilter;
	if ((_isScanning || _settings->onlySinglePeer()) && !skipMediaCountsForFilteredScan) {
		_startProcess->steps.push_back(Step::MediaCounts);
	}
	if (_settings->types & Settings::Type::ProfileMusic) {
		_startProcess->steps.push_back(Step::ProfileMusicCount);
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

	const auto flags = CalculateTakeoutFlags(*_settings, _isScanning);
	const auto sizeLimit = _isScanning ? kFileMaxSize : _settings->media.sizeLimit;

	auto start = [=] {
		if (_startProcess) sendNextStartRequest();
	};

	if (_takeoutId.has_value() && (_takeoutFlags != flags || _takeoutSizeLimit != sizeLimit)) {
		mainRequest(MTPaccount_FinishTakeoutSession(
			MTP_flags(0)
		), *_takeoutId).done([=] {
			_takeoutId = std::nullopt;
			startMainSession(flags, start);
		}).fail([=](const MTP::Error &) {
			_takeoutId = std::nullopt;
			startMainSession(flags, start);
			return true;
		}).send();
	} else if (_takeoutId.has_value()) {
		sendNextStartRequest();
	} else {
		startMainSession(flags, start);
	}
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
	case Step::MediaCounts:
		return requestMediaCounts();
	case Step::ProfileMusicCount:
		return requestProfileMusicCount();
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
		MTP_int(0),	 // offset
		MTP_long(0), // max_id
		MTP_int(0)	 // limit
	)).done([=](const MTPphotos_Photos &result) {
		if (!_settings || !_startProcess) return;

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
		if (!_settings || !_startProcess) return;

		_startProcess->info.storiesCount = result.data().vcount().v;

		sendNextStartRequest();
	}).send();
}

void ApiWrap::requestMediaCounts() {
	Expects(_startProcess != nullptr);

	using Type = MediaSettings::Type;
	const auto types = _settings->media.types;

	std::vector<std::pair<Type, MTPMessagesFilter>> filters;

	auto add = [&](Type type, const MTPMessagesFilter &filter) {
		if ((types & type) || (types & Type::FullHistory)) {
			filters.push_back({ type, filter });
		}
	};

	add(Type::Photo, MTP_inputMessagesFilterPhotos());
	add(Type::Video, MTP_inputMessagesFilterVideo());
	add(Type::File, MTP_inputMessagesFilterDocument());
	add(Type::Audio, MTP_inputMessagesFilterMusic());
	add(Type::VoiceMessage, MTP_inputMessagesFilterVoice());
	add(Type::VideoMessage, MTP_inputMessagesFilterRoundVideo());
	add(Type::Link, MTP_inputMessagesFilterUrl());
	add(Type::GIF, MTP_inputMessagesFilterGif());

	// Add total messages count request to get an accurate denominator for range-filtered exports.
	filters.push_back({ Type::FullHistory, MTP_inputMessagesFilterEmpty() });

	if (filters.empty()) {
		sendNextStartRequest();
		return;
	}

	_startProcess->pendingCounts = filters.size();

	for (const auto &pair : filters) {
		const auto type = pair.first;
		const auto filter = pair.second;

		const auto minId = (_settings->useIdRange && _settings->singlePeerFromId > 0) ? std::max(int64(0), int64(_settings->singlePeerFromId) - 1) : int64(0);
		const auto maxId = (_settings->useIdRange && _settings->singlePeerTillId > 0) ? (int64(_settings->singlePeerTillId) + 1) : int64(0);
		const auto minDate = _settings->useIdRange ? 0 : _settings->singlePeerFrom;
		const auto maxDate = _settings->useIdRange ? 0 : _settings->singlePeerTill;

		mainRequest(MTPmessages_Search(
			MTP_flags(0),
			_settings->singlePeer,
			MTP_string(""),
			MTP_inputPeerEmpty(),
			MTP_inputPeerEmpty(),
			MTP_vector<MTPReaction>(),
			MTP_int(0),
			filter,
			MTP_int(minDate),
			MTP_int(maxDate),
			MTP_int(0),
			MTP_int(0),
			MTP_int(100),
			MTP_int(int32(maxId)),
			MTP_int(int32(minId)),
			MTP_long(0)
		)).done([=](const MTPmessages_Messages &result) {
			if (!_settings || !_startProcess) return;

			const auto count = result.match(
				[](const MTPDmessages_messages &data) { return int(data.vmessages().v.size()); },
				[](const MTPDmessages_messagesSlice &data) { return data.vcount().v; },
				[](const MTPDmessages_channelMessages &data) { return data.vcount().v; },
				[](const MTPDmessages_messagesNotModified &) { return 0; }
			);



			const bool isLink = (type == Type::Link);
			const bool fullSize = (_settings->media.sizeLimit >= kFileMaxSize);
			const bool hasExtFilter = _settings->media.extensionFilterMode != MediaSettings::ExtFilterMode::None
				&& !_settings->media.extensionFilter.isEmpty();
			const bool hasDateOrIdRange = (_settings->singlePeerFrom != 0)
				|| (_settings->singlePeerTill != 0)
				|| _settings->useIdRange;
			const bool noRange = !hasDateOrIdRange;

			const bool canTrustServerCount = noRange && !hasExtFilter && fullSize && [&] {
				return (type == Type::Photo || type == Type::VoiceMessage || type == Type::VideoMessage || type == Type::GIF)
					|| (type == Type::File || type == Type::Audio || type == Type::Video);
			}();

			if (canTrustServerCount) {
				_serverCountTrustedTypes.insert(type);
			}

			if (_scanStats && type != Type::Sticker && type != Type::Text) {
				if (isLink) {
					if (canTrustServerCount) {
						_scanStats->setMessagesWithLinks(type, count);
						_scanStats->setLocalTotalCount(type, count);
					}
				} else if (canTrustServerCount || _usingServerCounts) {
					_scanStats->setLocalTotalCount(type, count);
				}
			}
			if (_stats && type != Type::Sticker && type != Type::Text) {
				if (isLink) {
					if (canTrustServerCount) {
						_stats->setMessagesWithLinks(type, count);
						_stats->setLocalTotalCount(type, count);
					}
				} else if (canTrustServerCount || _usingServerCounts) {
					_stats->setLocalTotalCount(type, count);
				}
			}
			if (_exportProgress) {
				const auto typeInt = static_cast<int>(type);
				if (isLink) {
					if (canTrustServerCount) {
						_exportProgress->typeCounters[typeInt].messagesWithLinks = count;
						_exportProgress->typeCounters[typeInt].localTotalCount = count;
						if (_isScanning) {
							_exportProgress->scanStats[typeInt].messagesWithLinks = count;
							_exportProgress->scanStats[typeInt].localTotalCount = count;
						}
					}
				} else if (canTrustServerCount || _usingServerCounts) {
					_exportProgress->typeCounters[typeInt].localTotalCount = count;
					if (_isScanning) {
						_exportProgress->scanStats[typeInt].localTotalCount = count;
					}
				}
			}
			if (type == Type::FullHistory) {
				_serverTotalCount = count;
			} else if (!_usingServerCounts) {
				// Sum up counts of selected types for the estimate
				// if FullHistory is not available.
				if (_serverTotalCount == 0) {
					_serverTotalCount += count;
				}
			}

			if (--_startProcess->pendingCounts == 0) {
				sendNextStartRequest();
			}
		}).fail([=](const MTP::Error &) {
			if (!_settings || !_startProcess) return false;
			if (--_startProcess->pendingCounts == 0) {
				sendNextStartRequest();
			}
			return true;
		}).send();
	}
}

void ApiWrap::requestProfileMusicCount() {
	Expects(_startProcess != nullptr);

	mainRequest(MTPusers_GetSavedMusic(
		_user,
		MTP_int(0), // offset
		MTP_int(0), // limit
		MTP_long(0) // hash
	)).done([=](const MTPusers_SavedMusic &result) {
		Expects(_settings != nullptr);
		Expects(_startProcess != nullptr);

		const auto count = result.match(
		[](const MTPDusers_savedMusic &data) {
			return data.vcount().v;
		}, [](const MTPDusers_savedMusicNotModified &data) {
			return -1;
		});
		if (count < 0) {
			error("Unexpected messagesNotModified received.");
			return;
		}
		_startProcess->info.profileMusicCount = count;

		sendNextStartRequest();
	}).send();
}

void ApiWrap::requestSplitRanges() {
	Expects(_startProcess != nullptr);

	mainRequest(MTPmessages_GetSplitRanges(
	)).done([=](const MTPVector<MTPMessageRange> &result) {
		if (!_settings || !_startProcess) return;
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
		if (!_settings || !_startProcess) return;

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
	process->info.serverTotalCount = _serverTotalCount;
	process->info.serverCountIsAccurate = true; // Total count is now requested with filters/range.
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

void ApiWrap::startMainSession(uint32 flags, FnMut<void()> done) {
	const auto sizeLimit = _isScanning ? kFileMaxSize : _settings->media.sizeLimit;

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
			MTPaccount_initTakeoutSession(
				MTP_flags(MTPaccount_initTakeoutSession::Flags::from_raw(flags)),
				MTP_long(sizeLimit))
		)).done([=, done = std::move(done)](
				const MTPaccount_Takeout &result) mutable {
			_takeoutId = result.match([](const MTPDaccount_takeout &data) {
				return data.vid().v;
			});
			_takeoutFlags = flags;
			_takeoutSizeLimit = sizeLimit;
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
		if (!_settings) return;
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
		LocationKey(),
		[](FileProgress progress) { return true; },
		[=](const QString &result) { otherDataDone(result); },
		0,
		SizeNameKey());
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
		if (!_userpicsProcess) return;

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

	_userpicsProcess->processing = true;
	const auto guard = gsl::finally([&] {
		_userpicsProcess->processing = false;
		if (_userpicsProcess && _userpicsProcess->pendingFiles == 0) {
			finishUserpicsSlice();
		}
	});

	for (auto &photo : _userpicsProcess->slice->list) {
		(void)processFileLoad(
			photo.image.file,
			Data::FileOrigin(),
			[=](FileProgress value) { return loadUserpicProgress(value); },
			[=](const QString &path) { loadUserpicDone(path); },
			nullptr,
			nullptr,
			false);
		if (!_userpicsProcess || !_userpicsProcess->slice) {
			return;
		}
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

	return _userpicsProcess->fileProgress(ApiWrap::DownloadProgress{
		.randomId = process.randomId,
		.path = process.relativePath,
		.itemIndex = _userpicsProcess->processed, // This is an approximation now.
		.ready = progress.ready,
		.total = progress.total,
		.isAuxiliary = false,
		.messagesTotalCount = _chatProcess ? _chatProcess->messagesTotalCount : 0,
	});
}

void ApiWrap::loadUserpicDone(const QString &relativePath) {

	Expects(_userpicsProcess != nullptr);



	--_userpicsProcess->pendingFiles;

	Assert(_userpicsProcess->pendingFiles >= 0);

	if (_userpicsProcess->pendingFiles == 0 && !_userpicsProcess->processing) {

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
		if (!_storiesProcess) return;

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

	_storiesProcess->processing = true;
	const auto guard = gsl::finally([&] {
		_storiesProcess->processing = false;
		if (_storiesProcess && _storiesProcess->pendingFiles == 0) {
			finishStoriesSlice();
		}
	});

	for (auto &story : _storiesProcess->slice->list) {
		const auto origin = Data::FileOrigin{ .storyId = story.id };
		(void)processFileLoad(
			story.file(),
			origin,
			[=](FileProgress value) { return loadStoryProgress(value); },
			[=](const QString &path) { loadStoryDone(path); },
			nullptr,
			&story,
			false);
		if (!_storiesProcess || !_storiesProcess->slice) {
			return;
		}

		(void)processFileLoad(
			story.thumb().file,
			origin,
			[=](FileProgress value) { return loadStoryThumbProgress(value); },
			[=](const QString &path) { loadStoryThumbDone(path); },
			nullptr,
			&story,
			true);
		if (!_storiesProcess || !_storiesProcess->slice) {
			return;
		}
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
	return loadStoryProgress(progress, false);
}

bool ApiWrap::loadStoryProgress(FileProgress progress, bool auxiliary) {
	const auto it = _fileProcesses.find(progress.randomId);
	if (it == end(_fileProcesses)) {
		return false;
	}
	const auto &process = *it->second;

	Expects(_storiesProcess != nullptr);

	return _storiesProcess->fileProgress(ApiWrap::DownloadProgress{
		.randomId = process.randomId,
		.path = process.relativePath,
		.itemIndex = _storiesProcess->processed,
		.ready = progress.ready,
		.total = progress.total,
		.isAuxiliary = auxiliary,
		.messagesTotalCount = _chatProcess ? _chatProcess->messagesTotalCount : 0 });
}

void ApiWrap::loadStoryDone(const QString &relativePath) {
	Expects(_storiesProcess != nullptr);
	--_storiesProcess->pendingFiles;
	Assert(_storiesProcess->pendingFiles >= 0);
	if (_storiesProcess->pendingFiles == 0 && !_storiesProcess->processing) {
		finishStoriesSlice();
	}
}

bool ApiWrap::loadStoryThumbProgress(FileProgress progress) {
	return loadStoryProgress(progress, true);
}

void ApiWrap::loadStoryThumbDone(const QString &relativePath) {
	Expects(_storiesProcess != nullptr);
	--_storiesProcess->pendingFiles;
	Assert(_storiesProcess->pendingFiles >= 0);
	if (_storiesProcess->pendingFiles == 0 && !_storiesProcess->processing) {
		finishStoriesSlice();
	}
}

void ApiWrap::finishStories() {
	Expects(_storiesProcess != nullptr);

	base::take(_storiesProcess)->finish();
}

void ApiWrap::requestProfileMusic(
		FnMut<bool(Data::ProfileMusicInfo&&)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::ProfileMusicSlice&&)> slice,
		FnMut<void()> finish) {
	Expects(_profileMusicProcess == nullptr);

	_profileMusicProcess = std::make_unique<ProfileMusicProcess>();
	_profileMusicProcess->start = std::move(start);
	_profileMusicProcess->fileProgress = std::move(progress);
	_profileMusicProcess->handleSlice = std::move(slice);
	_profileMusicProcess->finish = std::move(finish);

	mainRequest(MTPusers_GetSavedMusic(
		_user,
		MTP_int(0), // offset
		MTP_int(kProfileMusicSliceLimit), // limit
		MTP_long(0) // hash
	)).done([=](const MTPusers_SavedMusic &result) mutable {
		Expects(_profileMusicProcess != nullptr);

		auto startInfo = result.match(
		[](const MTPDusers_savedMusic &data) {
			return Data::ProfileMusicInfo{ data.vcount().v };
		}, [](const MTPDusers_savedMusicNotModified &data) {
			return Data::ProfileMusicInfo{ 0 };
		});
		if (!_profileMusicProcess->start(std::move(startInfo))) {
			return;
		}

		handleProfileMusicSlice(result);
	}).send();
}

void ApiWrap::handleProfileMusicSlice(const MTPusers_SavedMusic &result) {
	Expects(_profileMusicProcess != nullptr);
	Expects(_selfId.has_value());

	auto context = Data::ParseMediaContext();
	context.selfPeerId = peerFromUser(*_selfId);

	auto slice = result.match([&](const MTPDusers_savedMusic &data) {
		if (data.vdocuments().v.size() < kProfileMusicSliceLimit) {
			_profileMusicProcess->lastSlice = true;
		}
		auto result = Data::MessagesSlice();
		for (const auto &doc : data.vdocuments().v) {
			auto message = Data::Message();
			message.id = ++_profileMusicProcess->processed;
			message.date = 0;
			message.media.content = Data::ParseDocument(
				context,
				doc,
				"profile_music/",
				0);
			result.list.push_back(std::move(message));
		}
		return result;
	}, [&](const MTPDusers_savedMusicNotModified &) {
		_profileMusicProcess->lastSlice = true;
		return Data::MessagesSlice();
	});

	auto profileSlice = Data::ProfileMusicSlice();
	profileSlice.list.reserve(slice.list.size());
	for (auto &message : slice.list) {
		if (v::is<Data::Document>(message.media.content)) {
			const auto &doc = v::get<Data::Document>(message.media.content);
			if (doc.isAudioFile) {
				profileSlice.list.push_back(std::move(message));
			}
		}
	}

	loadProfileMusicFiles(std::move(profileSlice));
}

void ApiWrap::loadProfileMusicFiles(Data::ProfileMusicSlice &&slice) {
	Expects(_profileMusicProcess != nullptr);
	Expects(!_profileMusicProcess->slice.has_value());

	if (slice.list.empty()) {
		_profileMusicProcess->lastSlice = true;
	}
	_profileMusicProcess->slice = std::move(slice);
	_profileMusicProcess->fileIndex = 0;
	loadNextProfileMusic();
}

void ApiWrap::loadNextProfileMusic() {
	Expects(_profileMusicProcess != nullptr);
	Expects(_profileMusicProcess->slice.has_value());

	for (auto &list = _profileMusicProcess->slice->list
		; _profileMusicProcess->fileIndex < list.size()
		; ++_profileMusicProcess->fileIndex) {
		auto &message = list[_profileMusicProcess->fileIndex];
		const auto origin = Data::FileOrigin{ .messageId = message.id };
		const auto ready = processFileLoad(
			message.file(),
			origin,
			[=](FileProgress value) { return loadProfileMusicProgress(value); },
			[=](const QString &path) { loadProfileMusicDone(path); },
			&message);
		if (!ready) {
			return;
		}
		const auto thumbProgress = [=](FileProgress value) {
			return loadProfileMusicThumbProgress(value);
		};
		const auto thumbReady = processFileLoad(
			message.thumb().file,
			origin,
			thumbProgress,
			[=](const QString &path) { loadProfileMusicThumbDone(path); },
			&message);
		if (!thumbReady) {
			return;
		}
	}
	finishProfileMusicSlice();
}

void ApiWrap::finishProfileMusicSlice() {
	Expects(_profileMusicProcess != nullptr);
	Expects(_profileMusicProcess->slice.has_value());

	auto slice = *base::take(_profileMusicProcess->slice);
	if (!slice.list.empty()) {
		_profileMusicProcess->processed += slice.list.size();
		_profileMusicProcess->offsetId = slice.list.back().id;
		if (!_profileMusicProcess->handleSlice(std::move(slice))) {
			return;
		}
	}
	if (_profileMusicProcess->lastSlice) {
		finishProfileMusic();
		return;
	}

	mainRequest(MTPusers_GetSavedMusic(
		_user,
		MTP_int(_profileMusicProcess->offsetId),
		MTP_int(kProfileMusicSliceLimit),
		MTP_long(0)
	)).done([=](const MTPusers_SavedMusic &result) {
		handleProfileMusicSlice(result);
	}).send();
}

bool ApiWrap::loadProfileMusicProgress(FileProgress progress) {
	const auto it = _fileProcesses.find(progress.randomId);
	if (it == end(_fileProcesses)) {
		return false;
	}
	const auto &process = *it->second;

	Expects(_profileMusicProcess != nullptr);
	Expects(_profileMusicProcess->slice.has_value());
	Expects((_profileMusicProcess->fileIndex >= 0)
		&& (_profileMusicProcess->fileIndex
			< _profileMusicProcess->slice->list.size()));

	return _profileMusicProcess->fileProgress(DownloadProgress{
		process.randomId,
		process.relativePath,
		_profileMusicProcess->fileIndex,
		progress.ready,
		progress.total });
}

void ApiWrap::loadProfileMusicDone(const QString &relativePath) {
	Expects(_profileMusicProcess != nullptr);
	Expects(_profileMusicProcess->slice.has_value());
	Expects((_profileMusicProcess->fileIndex >= 0)
		&& (_profileMusicProcess->fileIndex
			< _profileMusicProcess->slice->list.size()));

	const auto index = _profileMusicProcess->fileIndex;
	auto &file = _profileMusicProcess->slice->list[index].file();
	file.relativePath = relativePath;
	if (relativePath.isEmpty()) {
		file.skipReason = Data::File::SkipReason::Unavailable;
	}
	loadNextProfileMusic();
}

bool ApiWrap::loadProfileMusicThumbProgress(FileProgress progress) {
	return loadProfileMusicProgress(progress);
}

void ApiWrap::loadProfileMusicThumbDone(const QString &relativePath) {
	Expects(_profileMusicProcess != nullptr);
	Expects(_profileMusicProcess->slice.has_value());
	Expects((_profileMusicProcess->fileIndex >= 0)
		&& (_profileMusicProcess->fileIndex
			< _profileMusicProcess->slice->list.size()));

	const auto index = _profileMusicProcess->fileIndex;
	auto &file = _profileMusicProcess->slice->list[index].thumb().file;
	file.relativePath = relativePath;
	if (relativePath.isEmpty()) {
		file.skipReason = Data::File::SkipReason::Unavailable;
	}
	loadNextProfileMusic();
}

void ApiWrap::finishProfileMusic() {
	Expects(_profileMusicProcess != nullptr);

	base::take(_profileMusicProcess)->finish();
}

void ApiWrap::requestContacts(FnMut<void(Data::ContactsList&&)> done) {
	Expects(_contactsProcess == nullptr);

	_contactsProcess = std::make_unique<ContactsProcess>();
	_contactsProcess->done = std::move(done);
	mainRequest(MTPcontacts_GetSaved(
	)).done([=](const MTPVector<MTPSavedContact> &result) {
		if (!_contactsProcess) return;
		_contactsProcess->result = Data::ParseContactsList(result);

		const auto resolve = [=](int index, const auto &resolveNext) -> void {
			if (index == _contactsProcess->result.list.size()) {
				return requestTopPeersSlice();
			}
			const auto &contact = _contactsProcess->result.list[index];
			mainRequest(MTPcontacts_ResolvePhone(
				MTP_string(qs(contact.phoneNumber))
			)).done([=](const MTPcontacts_ResolvedPeer &result) {
				if (!_contactsProcess) return;
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
		if (!_contactsProcess) return;

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
		if (!_takeoutId) return;
		auto list = Data::ParseSessionsList(result);
		mainRequest(MTPaccount_GetWebAuthorizations(
		)).done([=, done = std::move(done), list = std::move(list)](
				const MTPaccount_WebAuthorizations &result) mutable {
			if (!_takeoutId) return;
			list.webList = Data::ParseWebSessionsList(result).webList;
			done(std::move(list));
		}).send();
	}).send();
}

void ApiWrap::requestMessages(
		const Data::DialogInfo &info,
		int64 fromId,
		int64 tillId,
		FnMut<bool(const Data::DialogInfo &)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::MessagesSlice&&)> slice,
		FnMut<void()> done) {
	Expects(_chatProcess == nullptr);
	Expects(_selfId.has_value());

	// In resume mode, adjust fromId to skip already exported messages
	if (_resumeMode && _exportProgress && _exportProgress->lastMessageId > 0) {
		const auto resumeFromId = static_cast<int64>(_exportProgress->lastMessageId) + 1;
		if (fromId == 0 || resumeFromId > fromId) {
			fromId = resumeFromId;
		}
	}

	_chatProcess = std::make_unique<ChatProcess>();
	_chatProcess->context.selfPeerId = peerFromUser(*_selfId);
	_chatProcess->info = info;
	_chatProcess->fromId = fromId;
	_chatProcess->tillId = tillId;
	_chatProcess->start = std::move(start);
	_chatProcess->fileProgress = std::move(progress);
	_chatProcess->handleSlice = std::move(slice);
	_chatProcess->done = std::move(done);

	if (_exportProgress && tillId > 0) {
		_exportProgress->rangeEndMsgId = static_cast<uint64>(tillId);
	}

	if (fromId > 0) {
		_chatProcess->largestIdPlusOne = int32(std::min(int64(std::numeric_limits<int32>::max()), fromId));
	}

	if (_exportProgress) {
		_chatProcess->messagesProcessed = _exportProgress->messagesProcessed;

		if (_scanStats && _exportProgress->scanTotalMessages > 0) {
			_scanStats->setTotalMessages(_exportProgress->scanTotalMessages);
		}

		if (!_isScanning) {
			_resumeIdThreshold = _exportProgress->lastMessageId;
		}
	} else {
		_chatProcess->messagesProcessed = 0;
		_chatProcess->messagesTotalCount = 0;
	}

	if (_settings->useIdRange) {
		requestMessagesCount(0);
	} else {
		resolveDates();
	}
}

void ApiWrap::requestMessagesCount(int localSplitIndex) {
	Expects(_chatProcess != nullptr);
	Expects(localSplitIndex < _chatProcess->info.splits.size());

	const auto filter = getFilter();
	const auto peer = _chatProcess->info.splits[localSplitIndex] >= 0
		? _chatProcess->info.input
		: _chatProcess->info.migratedFromInput;

	const auto minId = (_chatProcess->fromId > 0) ? std::max(int64(0), _chatProcess->fromId - 1) : int64(0);
	const auto maxId = (_chatProcess->tillId > 0) ? (_chatProcess->tillId + 1) : int64(0);

	using Flag = MTPmessages_Search::Flag;
	auto searchFlags = Flag(0);

	mainRequest(MTPmessages_Search(
		MTP_flags(searchFlags),
		peer,
		MTP_string(""), // q
		MTP_inputPeerEmpty(), // from_id
		MTP_inputPeerEmpty(), // saved_peer_id
		MTP_vector<MTPReaction>(), // saved_reaction
		MTP_int(0), // top_msg_id
		filter,
		MTP_int(0), // min_date — use min_id/max_id instead; date range already resolved to IDs
		MTP_int(0), // max_date — use min_id/max_id instead; date range already resolved to IDs
		MTP_int(0), // offset_id
		MTP_int(0), // add_offset
		MTP_int(1), // limit
		MTP_int(int32(maxId)), // max_id
		MTP_int(int32(minId)), // min_id
		MTP_long(0) // hash
	)).done([=](const MTPmessages_Messages &result) {
		if (!_chatProcess || !_settings) return;

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
		const auto hasDateRange = (_settings->singlePeerFrom > 0) || (_settings->singlePeerTill > 0);
		const auto skipSplit = hasDateRange
			&& (_chatProcess->fromId == 0)
			&& (_chatProcess->tillId == 0);
		if (skipSplit) {
			messagesCountLoaded(localSplitIndex, 0);
			return;
		}
		
		// If scanning text/history in a specific range, use the ID difference as a better estimate
		// than the total chat count returned by the server.
		const auto realCount = count;
		
		checkFirstMessageDate(localSplitIndex, realCount);
	}).send();
}

void ApiWrap::checkFirstMessageDate(int localSplitIndex, int count) {
	Expects(_chatProcess != nullptr);
	Expects(localSplitIndex < _chatProcess->info.splits.size());

	messagesCountLoaded(localSplitIndex, count);
}

void ApiWrap::messagesCountLoaded(int localSplitIndex, int count) {
	Expects(_chatProcess != nullptr);
	Expects(localSplitIndex < _chatProcess->info.splits.size());

	const auto delta = count - _chatProcess->info.messagesCountPerSplit[localSplitIndex];
	_chatProcess->info.messagesCountPerSplit[localSplitIndex] = count;
	
	_chatProcess->messagesTotalCount += delta;

	if (localSplitIndex + 1 < _chatProcess->info.splits.size()) {
		requestMessagesCount(localSplitIndex + 1);
	} else if (_chatProcess->start(_chatProcess->info)) {
		requestMessagesSlice();
	}
}

void ApiWrap::resolveDates() {
	const auto fromDate = _settings->singlePeerFrom;
	const auto tillDate = _settings->singlePeerTill;

	if (fromDate == 0 && tillDate == 0) {
		requestMessagesCount(0);
		return;
	}

	const auto peer = _chatProcess->info.input;

	const auto resolveTill = [=] {
		if (tillDate == 0) {
			if (_chatProcess) requestMessagesCount(0);
			return;
		}
		mainRequest(MTPmessages_GetHistory(
			MTPmessages_getHistory(
				peer,
				MTP_int(0), // offset_id
				MTP_int(tillDate),
				MTP_int(0), // add_offset
				MTP_int(1), // limit
				MTP_int(0), // max_id
				MTP_int(0), // min_id
				MTP_long(0)) // hash
		)).done([=](const MTPmessages_Messages &result) {
			if (!_chatProcess || !_settings) return;
			result.match([&](const MTPDmessages_messagesNotModified &) {
			}, [&](const auto &data) {
				if (!data.vmessages().v.isEmpty()) {
					_chatProcess->tillId = data.vmessages().v[0].match([](const auto &m) {
						return int64(m.vid().v);
					});
					if (_exportProgress && _chatProcess->tillId > 0) {
						_exportProgress->rangeEndMsgId = static_cast<uint64>(_chatProcess->tillId);
					}
				}
			});
			requestMessagesCount(0);
		}).fail([=](const MTP::Error &) {
			requestMessagesCount(0);
			return true;
		}).send();
	};

	if (fromDate > 0) {
		mainRequest(MTPmessages_GetHistory(
			MTPmessages_getHistory(
				peer,
				MTP_int(0), // offset_id
				MTP_int(fromDate),
				MTP_int(0), // add_offset
				MTP_int(1), // limit
				MTP_int(0), // max_id
				MTP_int(0), // min_id
				MTP_long(0)) // hash
		)).done([=](const MTPmessages_Messages &result) {
			if (!_chatProcess || !_settings) return;
			result.match([&](const MTPDmessages_messagesNotModified &) {
			}, [&](const auto &data) {
				if (!data.vmessages().v.isEmpty()) {
					const auto msg = data.vmessages().v[0];
					const auto id = msg.match([](const auto &m) {
						return int64(m.vid().v);
					});
					const auto date = msg.match([](const MTPDmessageEmpty &) {
						return TimeId(0);
					}, [](const auto &m) {
						return TimeId(m.vdate().v);
					});
					_chatProcess->fromId = (date > 0 && date < fromDate) ? (id + 1) : id;
					_chatProcess->largestIdPlusOne = int32(std::max(int64(1), std::min(int64(std::numeric_limits<int32>::max()), _chatProcess->fromId)));
				}
			});
			resolveTill();
		}).fail([=](const MTP::Error &) {
			resolveTill();
			return true;
		}).send();
	} else {
		resolveTill();
	}
}

void ApiWrap::finishExport(FnMut<void()> done) {
	// Simple direct finish — same as reference implementation.
	// The export controller ensures this is called only after all message slices
	// are processed. File downloads complete before finishMessages() fires done(),
	// so there is no need to defer here.

	if (_exportProgress) {
		_exportProgress->isComplete = true;
		_exportProgress->lastExportDate = QDateTime::currentDateTime().toString(Qt::ISODate);
		saveProgress();
	}

	const auto takeoutId = base::take(_takeoutId);
	const auto guard = gsl::finally([&] {
		clearState();
	});

	if (!takeoutId) {
		if (done) {
			done();
		}
		return;
	}

	mainRequest(MTPaccount_FinishTakeoutSession(
		MTP_flags(MTPaccount_FinishTakeoutSession::Flag::f_success)
	), takeoutId).done(std::move(done)).send();
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

	auto &process = *it->second;
	for (const auto &[requestId, offset] : process.activeRequestOffsets) {
		_mtp.request(requestId).cancel();
	}

	finishFile(randomId, QString());
}

void ApiWrap::cancelExportFast(bool keepCache) {
	const auto takeoutId = base::take(_takeoutId);
	if (takeoutId.has_value()) {
		const auto requestId = mainRequest(MTPaccount_FinishTakeoutSession(
			MTP_flags(0)
		), takeoutId).send();
		_mtp.request(requestId).detach();
	}
	clearState(keepCache);
}

void ApiWrap::requestSinglePeerDialog() {
	auto doneSinglePeer = [=](const auto &result) {
		if (!_settings || !_dialogsProcess) return;
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
		if (!_settings || !_dialogsProcess) return;
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
		if (!_settings || !_dialogsProcess) return;

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
		if (!_leftChannelsProcess) return;

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
		return !!(types & SettingsFromDialogsType(info.type));
	};
	auto filtered = ranges::views::all(
		from
	) | ranges::views::filter([&](const Data::DialogInfo &info) {
		if (goodByTypes(info)) {
			return true;
		} else if (info.migratedToChannelId
			&& ((types & Settings::Type::PublicGroups)
				|| (types & Settings::Type::PrivateGroups))) {
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
		if (!_chatProcess) return;

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
		if (!_chatProcess) return;

		result.match(
			[](const MTPDmessages_messages &data) {
			return int(data.vmessages().v.size());
		}, [](const MTPDmessages_messagesSlice &data) {
			return data.vcount().v;
		}, [](const MTPDmessages_channelMessages &data) {
			return data.vcount().v;
		}, [](const MTPDmessages_messagesNotModified &data) {
			return -1;
		});

		if (auto requestDone = base::take(_chatProcess->requestDone)) {
			requestDone(std::move(result));
		}
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

	const auto minId = (_chatProcess->fromId > 0) ? std::max(int64(0), _chatProcess->fromId - 1) : int64(0);
	const auto maxId = (_chatProcess->tillId > 0) ? (_chatProcess->tillId + 1) : int64(0);
	const auto filter = getFilter();
	// Use search during scanning ONLY if we have a specific media filter active.
	// This reduces the number of messages transferred when only one type is selected.
	// We use the getFilter() helper which returns Empty for complex/multi-type cases.
	const bool mediaFilterActive = (filter.type() != mtpc_inputMessagesFilterEmpty);
	const auto useSearch = (_isScanning && mediaFilterActive) || (!_isScanning && (_chatProcess->info.onlyMyMessages
		|| mediaFilterActive));

	// Check if chat has forwarding restriction (noforwards flag)
	// If set, use normal requests instead of takeout (takeout silently fails for restricted chats)
	const bool hasRestriction = _chatProcess->info.hasForwardRestriction;

	if (useSearch) {
		using Flag = MTPmessages_Search::Flag;
		auto searchFlags = (_chatProcess->info.onlyMyMessages ? Flag::f_from_id : Flag(0));

		if (hasRestriction) {
			// Use normal request for restricted chats
			normalRequest(MTPInvokeWithMessagesRange<MTPmessages_Search>(
				_splits[realSplitIndex],
				MTPmessages_Search(
					MTP_flags(searchFlags),
					realPeerInput,
					MTP_string(), // query
					MTP_inputPeerSelf(),
					MTP_inputPeerEmpty(), // saved_peer_id
					MTP_vector<MTPReaction>(), // saved_reaction
					MTP_int(0), // top_msg_id
					filter,
					MTP_int(0), // min_date
					MTP_int(0), // max_date
					MTP_int(offsetId),
					MTP_int(addOffset),
					MTP_int(limit),
					MTP_int(int32(maxId)), // max_id
					MTP_int(int32(minId)), // min_id
					MTP_long(0) // hash
				)
			)).done(doneHandler).send();
		} else {
			// Use takeout for unrestricted chats
			splitRequest(realSplitIndex, MTPmessages_Search(
				MTP_flags(searchFlags),
				realPeerInput,
				MTP_string(), // query
				MTP_inputPeerSelf(),
				MTP_inputPeerEmpty(), // saved_peer_id
				MTP_vector<MTPReaction>(), // saved_reaction
				MTP_int(0), // top_msg_id
				filter,
				MTP_int(0), // min_date
				MTP_int(0), // max_date
				MTP_int(offsetId),
				MTP_int(addOffset),
				MTP_int(limit),
				MTP_int(int32(maxId)), // max_id
				MTP_int(int32(minId)), // min_id
				MTP_long(0) // hash
			))
			.fail([=](const MTP::Error &error) {
				if (!_chatProcess || !_settings) return false;

				if (error.type() == u"CHAT_FORWARDS_RESTRICTED"_q) {
					// Chat has forwarding restrictions - retry without takeout
					_chatProcess->info.hasForwardRestriction = true;
					requestChatMessages(
						splitIndex,
						offsetId,
						addOffset,
						limit,
						base::take(_chatProcess->requestDone));
					return true;
				}
				return false;
			}).done(doneHandler).send();
		}
	} else {
		if (hasRestriction) {
			// Use normal request for restricted chats
			normalRequest(MTPInvokeWithMessagesRange<MTPmessages_GetHistory>(
				_splits[realSplitIndex],
				MTPmessages_GetHistory(
					realPeerInput,
					MTP_int(offsetId),
					MTP_int(0), // offset_date
					MTP_int(addOffset),
					MTP_int(limit),
					MTP_int(int32(maxId)), // max_id
					MTP_int(int32(minId)), // min_id
					MTP_long(0)  // hash
				)
			)).done(doneHandler).send();
		} else {
			// Use takeout for unrestricted chats
			splitRequest(realSplitIndex, MTPmessages_GetHistory(
				realPeerInput,
				MTP_int(offsetId),
				MTP_int(0), // offset_date
				MTP_int(addOffset),
				MTP_int(limit),
				MTP_int(int32(maxId)), // max_id
				MTP_int(int32(minId)), // min_id
				MTP_long(0)  // hash
			))
			.fail([=](const MTP::Error &error) {
				if (!_chatProcess || !_settings) return false;

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
				} else if (error.type() == u"CHAT_FORWARDS_RESTRICTED"_q) {
					// Chat has forwarding restrictions - retry without takeout
					// This handles users/bots where we couldn't pre-check the flag
					_chatProcess->info.hasForwardRestriction = true;
					requestChatMessages(
						splitIndex,
						offsetId,
						addOffset,
						limit,
						base::take(_chatProcess->requestDone));
					return true;
				}
				return false;
			}).done(doneHandler).send();
		}
	}
}


bool ApiWrap::shouldCountLocally(MediaSettings::Type type) const {
	if (!_usingServerCounts) {
		return true;
	}
	using Type = MediaSettings::Type;
	return (type == Type::Text)
		|| (type == Type::Sticker)
		|| (type == Type::Link);
}

bool ApiWrap::shouldCountLocally() const {
	return !_usingServerCounts;
}

MTPMessagesFilter ApiWrap::getFilter() const {
	using Type = MediaSettings::Type;
	const auto types = _settings->media.types;

	if ((types & Type::Text) || (types & Type::FullHistory) || (types & Type::Sticker)) {
		return MTP_inputMessagesFilterEmpty();
	}

	if (types == MediaSettings::Types(0)) {
		return MTP_inputMessagesFilterEmpty();
	}

	const auto photo = (types & Type::Photo);
	const auto video = (types & Type::Video);
	const auto file = (types & Type::File);
	const auto voice = (types & Type::VoiceMessage);
	const auto round = (types & Type::VideoMessage);
	const auto gif = (types & Type::GIF);
	const auto audio = (types & Type::Audio);
	const auto link = (types & Type::Link);

	// Count how many media flags are set
	int selectedCount = (photo ? 1 : 0) + (video ? 1 : 0) + (file ? 1 : 0)
		+ (voice ? 1 : 0) + (round ? 1 : 0) + (gif ? 1 : 0)
		+ (audio ? 1 : 0) + (link ? 1 : 0);

	if (selectedCount == 1) {
		if (photo) return MTP_inputMessagesFilterPhotos();
		if (video) return MTP_inputMessagesFilterVideo();
		if (file) return MTP_inputMessagesFilterDocument();
		if (voice) return MTP_inputMessagesFilterVoice();
		if (round) return MTP_inputMessagesFilterRoundVideo();
		if (gif) return MTP_inputMessagesFilterGif();
		if (audio) return MTP_inputMessagesFilterMusic();
		if (link) return MTP_inputMessagesFilterUrl();
	} else if (selectedCount == 2 && photo && video) {
		return MTP_inputMessagesFilterPhotoVideo();
	} else if (selectedCount == 2 && voice && round) {
		return MTP_inputMessagesFilterRoundVoice();
	}

	return MTP_inputMessagesFilterEmpty();
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

	_chatProcess->messageFilesRequired.assign(s.list.size(), 0);
	_chatProcess->messageFilesDone.assign(s.list.size(), 0);
	_chatProcess->messageStats.assign(s.list.size(), ChatProcess::MessageStats());
	_chatProcess->emojiToMessageIndices.clear();

	for (int i = 0; i < int(s.list.size()); ++i) {
		const auto &message = s.list[i];



		auto &ms = _chatProcess->messageStats[i];

		const auto skippedByDate = Data::SkipMessageByDate(message, *_settings);
		if (skippedByDate) {
			ms.withinRange = false;
			onMessagePartDone(i, false);
			continue;
		}

		ms.withinRange = true;

		const auto hasMedia = !std::holds_alternative<v::null_t>(message.media.content);

		using MediaType = MediaSettings::Type;
		auto messageType = MediaType::Text;
		if (hasMedia) {
			messageType = v::match(message.media.content, [&](const Data::Document &data) {
				if (data.isSticker) return MediaType::Sticker;
				if (data.isVideoMessage) return MediaType::VideoMessage;
				if (data.isVoiceMessage) return MediaType::VoiceMessage;
				if (data.isAnimated) return MediaType::GIF; // GIF takes precedence over Video
				if (data.isVideoFile) return MediaType::Video; // Video takes precedence over File
				if (data.isAudioFile) return MediaType::Audio;
				return MediaType::File;
			}, [](const Data::Photo &data) {
				return MediaType::Photo;
			}, [](const Data::WebPage &data) {
				return MediaType::Link;
			}, [&](const Data::PaidMedia &data) {
				for (const auto &item : data.extended) {
					if (!item) continue;
					const auto type = v::match(item->content, [&](const Data::Document &doc) {
						if (doc.isSticker) return MediaType::Sticker;
						if (doc.isVideoMessage) return MediaType::VideoMessage;
						if (doc.isAnimated) return MediaType::GIF;
						if (doc.isVideoFile) return MediaType::Video;
						if (doc.isVoiceMessage) return MediaType::VoiceMessage;
						if (doc.isAudioFile) return MediaType::Audio;
						return MediaType::File;
					}, [](const Data::Photo &) {
						return MediaType::Photo;
					}, [](const auto &) {
						return MediaType::File;
					});
					if (type != MediaType::File) return type;
				}
				return MediaType::File;
			}, [](const v::null_t &) {
				return MediaType::Text;
			}, [](const auto &) {
				return static_cast<MediaType>(0);
			});
		} else {
			messageType = MediaType::Text;
		}

		if (messageType == static_cast<MediaType>(0) && hasMedia) {
			if (const auto doc = std::get_if<Data::Document>(&message.media.content)) {
				if (doc->isSticker) messageType = MediaType::Sticker;
				else if (doc->isVideoMessage) messageType = MediaType::VideoMessage;
				else if (doc->isVoiceMessage) messageType = MediaType::VoiceMessage;
				else if (doc->isAnimated) messageType = MediaType::GIF;
				else if (doc->isVideoFile) messageType = MediaType::Video;
				else if (doc->isAudioFile) messageType = MediaType::Audio;
				else messageType = MediaType::File;
			}
		}

		const bool hasFile = message.file().location || message.thumb().file.location;
		const auto fullSize = message.file().size;
		const auto types = _settings->media.types;
		const bool fullHistorySelected = (types & MediaSettings::Type::FullHistory);

		std::vector<QString> linksInThisMessage;
		bool hasWebPageMedia = false;

		// WebPage media first to establish context for text links
		v::match(message.media.content, [&](const Data::WebPage &webpage) {
			hasWebPageMedia = true;
			const auto url = QString::fromUtf8(webpage.url);
			if (!url.isEmpty()) {
				linksInThisMessage.push_back(url);
			}
		}, [](const auto &) {});

		for (const auto &part : message.text) {
			using T = Data::TextPart::Type;
			if (part.type == T::Url || part.type == T::TextUrl) {
				const auto url = (part.type == T::TextUrl) ? QString::fromUtf8(part.additional) : QString::fromUtf8(part.text);
				if (url.isEmpty()) continue;

				bool substantial = (part.type == T::TextUrl)
					|| url.contains(u"://")
					|| url.startsWith(u"mailto:")
					|| url.startsWith(u"tg:")
					|| hasWebPageMedia;

				if (substantial) {
					linksInThisMessage.push_back(url);
				}
			}
		}
		for (const auto &row : message.inlineButtonRows) {
			for (const auto &button : row) {
				if (button.type == Data::HistoryMessageMarkupButton::Type::Url || button.type == Data::HistoryMessageMarkupButton::Type::Auth) {
					const auto url = QString::fromUtf8(button.data);
					if (!url.isEmpty()) {
						linksInThisMessage.push_back(url);
					}
				}
			}
		}
		const auto addFromText = [&](const std::vector<Data::TextPart> &text) {
			for (const auto &part : text) {
				using T = Data::TextPart::Type;
				if (part.type == T::Url || part.type == T::TextUrl) {
					const auto url = (part.type == T::TextUrl) ? QString::fromUtf8(part.additional) : QString::fromUtf8(part.text);
					if (url.isEmpty()) continue;

					bool substantial = (part.type == T::TextUrl)
						|| url.contains(u"://")
						|| url.startsWith(u"mailto:")
						|| url.startsWith(u"tg:")
						|| hasWebPageMedia;

					if (substantial) {
						linksInThisMessage.push_back(url);
					}
				}
			}
		};		v::match(message.media.content, [&](const Data::Poll &poll) {
			addFromText(poll.question);
			for (const auto &answer : poll.answers) {
				addFromText(answer.text);
			}
		}, [&](const Data::TodoList &list) {
			addFromText(list.title);
			for (const auto &item : list.items) {
				addFromText(item.text);
			}
		}, [](const auto &) {});
		const auto hasAnyLink = !linksInThisMessage.empty() || hasWebPageMedia;
		const bool linkSelectedForStats = (types & MediaSettings::Type::Link) || fullHistorySelected;

		if (hasAnyLink && linkSelectedForStats) {
			int uniqueInMsg = 0;
			for (const auto &url : linksInThisMessage) {
				if (_visitedLinks.find(url) == _visitedLinks.end()) {
					_visitedLinks.insert(url);
					uniqueInMsg++;
				}
			}
			const int linksCount = int(linksInThisMessage.size());
			ms.links = linksCount;
			ms.linksUnique = uniqueInMsg;
			ms.linkMsgIncr = 1;
		} else if (_isScanning && !fullHistorySelected) {
			// If message has media but we didn't detect it as a link, check if it's a link preview
			// that the server might count.
			if (hasMedia && std::holds_alternative<Data::WebPage>(message.media.content)) {
			}
		}

		const bool gifUsingServerCounts = (_isScanning && _usingServerCounts && messageType == MediaSettings::Type::GIF);
		const auto oversized = (hasFile && _settings->media.sizeLimit > 0 && fullSize > _settings->media.sizeLimit)
			&& !hasAnyLink && (messageType != MediaSettings::Type::Text && messageType != MediaSettings::Type::Link)
			&& !gifUsingServerCounts;

		const bool mediaSelected = (types & messageType) || fullHistorySelected;

		bool selected = (mediaSelected && (!oversized || fullHistorySelected))
			|| (hasAnyLink && linkSelectedForStats)
			|| (!hasMedia && ((types & MediaSettings::Type::Text) || fullHistorySelected));

		if (!selected) {
			onMessagePartDone(i, false);
			continue;
		}

		ms.selected = true;
		ms.type = messageType;
		ms.size = fullSize;
		ms.unique = false; // Reset to ensure correct deduplication result

		bool isMediaForSum = (messageType != MediaSettings::Type::Link && messageType != MediaSettings::Type::Text);
		bool countThis = isMediaForSum
			|| ((types & MediaSettings::Type::Text) || fullHistorySelected)
			|| (hasAnyLink && linkSelectedForStats);

		if (countThis) {
			bool uniqueBubble = false;
			uint64 bubbleDocId = 0;
			QString bubbleName;
			v::match(message.media.content, [&](const Data::Document &data) {
				bubbleDocId = data.id;
				bubbleName = QString::fromUtf8(data.name);
			}, [&](const Data::Photo &data) {
				bubbleDocId = data.id;
			}, [](const auto &) {});
			const int64 bubbleSize = message.file().size;
			
			if (bubbleDocId == 0 && bubbleSize == 0) {
				uniqueBubble = true;
			} else {
				const auto dedup = dedupLookup(bubbleDocId, bubbleSize, bubbleName);
				if (!dedup.found) {
					uniqueBubble = true;
					if (bubbleDocId != 0 || (bubbleSize > 0 && !bubbleName.isEmpty())) {
						dedupRegister(bubbleDocId, bubbleSize, bubbleName, QString());
					}
				}
			}
			if (uniqueBubble) {
				ms.unique = true;
			}
		}

		int required = 0;
		if (ms.selected) {
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
						if (!_resolvedCustomEmoji.contains(id)) {
							++required;
							++_chatProcess->pendingFiles;
							_chatProcess->emojiToMessageIndices[id].push_back(i);
						}
					}
				}
			}
			for (const auto &reaction : message.reactions) {
				if (reaction.type == Data::Reaction::Type::CustomEmoji) {
					if (const auto id = reaction.documentId.toULongLong()) {
						if (!_resolvedCustomEmoji.contains(id)) {
							++required;
							++_chatProcess->pendingFiles;
							_chatProcess->emojiToMessageIndices[id].push_back(i);
						}
					}
				}
			}
		}
		_chatProcess->messageFilesRequired[i] = required;

if (required == 0) {
			onMessagePartDone(i, ms.selected);
		}
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
		if (!_chatProcess || !_settings) return false;
		LOG(("Export Error: Failed to get documents for emoji."));
		finalize();
		return true;
	}).done([=](const MTPVector<MTPDocument> &result) {
		if (!_chatProcess || !_settings) return;
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
		const auto fileProgress = [=](FileProgress progress) {
			if (_chatProcess) {
				return loadMessageEmojiProgress(progress);
			} else if (_topicProcess) {
				return loadTopicEmojiProgress(progress);
			}
			return true;
		};
		const auto ready = processFileLoad(
			file,
			{ .customEmojiId = id },
			fileProgress,
			[=](const QString &path) { loadCustomEmojiDone(id, path); });
		if (!ready) {
			return std::nullopt;
		}
		using SkipReason = Data::File::SkipReason;
		if (file.skipReason == SkipReason::Unavailable) {
			return Data::TextPart::UnavailableEmoji();
		} else if (file.skipReason == SkipReason::FileType
			|| file.skipReason == SkipReason::FileSize) {
			return QByteArray();
		} else {
			return file.relativePath.toUtf8();
		}
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

	_chatProcess->processing = true;
	const auto guard = gsl::finally([&] {
		_chatProcess->processing = false;
		if (_chatProcess && _chatProcess->pendingFiles == 0) {
			finishMessagesSlice();
		}
	});

	for (int i = 0; i < int(_chatProcess->slice->list.size()); ++i) {
		if (_chatProcess->messageFilesRequired[i] == 0) {
			continue;
		}
		auto &message = _chatProcess->slice->list[i];

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

		// Check dedup state for the main file BEFORE processFileLoad registers
		// it as in-progress.
		uint64 parentDocId = 0;
		QString parentName;
		v::match(message.media.content, [&](const Data::Document &data) {
			parentDocId = data.id;
			parentName = QString::fromUtf8(data.name);
		}, [&](const Data::Photo &data) {
			parentDocId = data.id;
		}, [](const auto &) {});
		const int64 parentSize = message.file().size;
		const bool mainFileAlreadySeen = message.file().location
			&& dedupLookup(parentDocId, parentSize, parentName).found;

		if (message.file().location) {
			(void)processFileLoad(
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
				[=](const QString &path) { loadMessageFileDone(i, path); },
				&message,
				nullptr,
				false);
		}

		if (message.thumb().file.location) {
			if (!mainFileAlreadySeen) {
				(void)processFileLoad(
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
					[=](const QString &path) { loadMessageThumbDone(i, path); },
					&message,
					nullptr,
					true);
			} else {
				loadMessageThumbDone(i, QString());
			}
		}

		for (const auto &part : message.text) {
			if (part.type == Data::TextPart::Type::CustomEmoji) {
				if (const auto id = part.additional.toULongLong()) {
					if (const auto it = _resolvedCustomEmoji.find(id); it != _resolvedCustomEmoji.end()) {
						(void)processFileLoad(
							it->second.file,
							{ .customEmojiId = id },
							[=](FileProgress value) {
								if (_chatProcess
								 && _chatProcess->fileToMessageIndex.find(value.randomId)
									 == end(_chatProcess->fileToMessageIndex)) {
									_chatProcess->fileToMessageIndex.emplace(value.randomId, i);
								}
								return loadMessageEmojiProgress(value);
							},
							[=](const QString &path) { loadMessageEmojiDone(id, path); },
							nullptr,
							nullptr,
							false);
					}
				}
			}
		}
		for (const auto &reaction : message.reactions) {
			if (reaction.type == Data::Reaction::Type::CustomEmoji) {
				if (const auto id = reaction.documentId.toULongLong()) {
					if (const auto it = _resolvedCustomEmoji.find(id); it != _resolvedCustomEmoji.end()) {
						(void)processFileLoad(
							it->second.file,
							{ .customEmojiId = id },
							[=](FileProgress value) {
								if (_chatProcess
								 && _chatProcess->fileToMessageIndex.find(value.randomId)
									 == end(_chatProcess->fileToMessageIndex)) {
									_chatProcess->fileToMessageIndex.emplace(value.randomId, i);
								}
								return loadMessageEmojiProgress(value);
							},
							[=](const QString &path) { loadMessageEmojiDone(id, path); },
							nullptr,
							nullptr,
							false);
					}
				}
			}
		}
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
				_chatProcess->largestIdPlusOne = (_chatProcess->fromId > 0)
					? int32(std::min(int64(std::numeric_limits<int32>::max()), _chatProcess->fromId))
					: 1;
				requestMessagesSlice();
			}
		} else {
			requestMessagesSlice();
		}
		return;
	}

	auto slice = *base::take(_chatProcess->slice);
	if (!slice.list.empty()) {
		_chatProcess->largestIdPlusOne = slice.list.back().id + 1;

		if (_chatProcess->tillId > 0
			&& _chatProcess->largestIdPlusOne > _chatProcess->tillId) {
			_chatProcess->lastSlice = true;
		}

		// 2. Collect messages for writing
		const auto splitIndex = _chatProcess->info.splits[_chatProcess->localSplitIndex];
		bool migrated = (splitIndex < 0);
		auto textBatch = Data::MessagesSlice();
		textBatch.peers = slice.peers;
		for (int i = 0; i < int(slice.list.size()); ++i) {
			if (_chatProcess->messageFilesRequired[i] == 0) {
				auto &message = slice.list[i];
				if (_resumeIdThreshold > 0 && message.id <= _resumeIdThreshold) continue;
				if (Data::SkipMessageByDate(message, *_settings)) continue;
				textBatch.list.push_back(std::move(message));
			}
		}

		if (!textBatch.list.empty()) {
			if (migrated) textBatch = Data::AdjustMigrateMessageIds(std::move(textBatch));
			_chatProcess->pendingBatch.peers = textBatch.peers;
			for (auto &msg : textBatch.list) _chatProcess->pendingBatch.list.push_back(std::move(msg));
		}

		// 3. Dispatch writer ONLY every 2000 messages for speed
		if (_chatProcess->pendingBatch.list.size() >= 2000 || _chatProcess->lastSlice) {
			if (!_chatProcess->pendingBatch.list.empty()) {
				if (!_chatProcess->handleSlice(std::move(_chatProcess->pendingBatch))) return;
				_chatProcess->pendingBatch = Data::MessagesSlice();
			}
			saveProgress(); // Disk Save
		}

		// 4. Update UI once per slice (Smooth high-speed jumps)
		_chatProcess->fileProgress(ApiWrap::DownloadProgress{
			.itemIndex = _chatProcess->messagesProcessed,
			.ready = -1,
			.total = -1,
			.isAuxiliary = true,
			.messagesTotalCount = _chatProcess->messagesTotalCount
		});
	}

	_chatProcess->fileToMessageIndex.clear();

	if (_chatProcess->lastSlice) {
		if (++_chatProcess->localSplitIndex < _chatProcess->info.splits.size()) {
			_chatProcess->lastSlice = false;
			_chatProcess->largestIdPlusOne = (_chatProcess->fromId > 0)
				? int32(std::min(int64(std::numeric_limits<int32>::max()), _chatProcess->fromId))
				: 1;
			requestMessagesSlice();
		} else {
			finishMessages();
		}
	} else {
		requestMessagesSlice();
	}
}

bool ApiWrap::loadMessageFileProgress(FileProgress progress) {
	return loadMessageFileProgress(progress, false);
}

bool ApiWrap::loadMessageFileProgress(FileProgress progress, bool auxiliary) {
	const auto it = _fileProcesses.find(progress.randomId);
	if (it == end(_fileProcesses)) {
		return false;
	}
	const auto &process = *it->second;

	if (!_chatProcess) {
		return false;
	}

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

	const int itemIndex = _chatProcess->messagesProcessed;
	const auto localTotalCount = _chatProcess->messagesTotalCount;
	const auto fileProgressCb = _chatProcess->fileProgress;
	return fileProgressCb(ApiWrap::DownloadProgress{
		.randomId = process.randomId,
		.path = process.relativePath,
		.itemIndex = itemIndex,
		.ready = progress.ready,
		.total = progress.total,
		.isAuxiliary = auxiliary,
		.messagesTotalCount = localTotalCount });
}

void ApiWrap::loadMessageFileDone(int index, const QString &relativePath) {
	if (!_chatProcess) {
		LOG(("Export Error: loadMessageFileDone called but _chatProcess is null"));
		return;
	}

	onMessagePartDone(index);
	if (_chatProcess->pendingFiles > 0) {
		--_chatProcess->pendingFiles;
	}
	if (_chatProcess->pendingFiles == 0 && !_chatProcess->processing) {
		finishMessagesSlice();
	}
}

bool ApiWrap::loadMessageThumbProgress(FileProgress progress) {
	return loadMessageFileProgress(progress, true);
}

void ApiWrap::loadMessageThumbDone(int index, const QString &relativePath) {
	if (!_chatProcess) return;
	onMessagePartDone(index);
	if (_chatProcess->pendingFiles > 0) {
		--_chatProcess->pendingFiles;
	}
	if (_chatProcess->pendingFiles == 0 && !_chatProcess->processing) {
		finishMessagesSlice();
	}
}

bool ApiWrap::loadMessageEmojiProgress(FileProgress progress) {
	return loadMessageFileProgress(progress, true);
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
				onMessagePartDone(messageIndex);
				--_chatProcess->pendingFiles;
			}
			_chatProcess->emojiToMessageIndices.erase(j);
			if (_chatProcess->pendingFiles == 0 && !_chatProcess->processing) {
				finishMessagesSlice();
				return;
			}
		}
	}
	// Only continue loading if not already processing to avoid recursive calls.
	if (_chatProcess && _chatProcess->slice && !_chatProcess->processing) {
		loadNextMessageFile();
	}
}

bool ApiWrap::loadTopicEmojiProgress(FileProgress progress) {
	const auto it = _fileProcesses.find(progress.randomId);
	if (it == end(_fileProcesses)) {
		return false;
	}
	const auto &process = *it->second;
	Expects(_topicProcess != nullptr);
	Expects(_topicProcess->slice.has_value());
	Expects((_topicProcess->fileIndex >= 0)
		&& (_topicProcess->fileIndex < _topicProcess->slice->list.size()));

	return _topicProcess->fileProgress(DownloadProgress{
		.randomId = process.randomId,
		.path = process.relativePath,
		.itemIndex = _topicProcess->fileIndex,
		.ready = progress.ready,
		.total = progress.total });
}

void ApiWrap::loadCustomEmojiDone(uint64 id, const QString &relativePath) {
	const auto i = _resolvedCustomEmoji.find(id);
	if (i != end(_resolvedCustomEmoji)) {
		i->second.file.relativePath = relativePath;
		if (relativePath.isEmpty()) {
			i->second.file.skipReason = Data::File::SkipReason::Unavailable;
		}
	}
	// Only continue loading if not already processing to avoid recursive calls.
	if (_chatProcess && !_chatProcess->processing) {
		loadNextMessageFile();
	} else if (_topicProcess && !_topicProcess->processing) {
		loadNextTopicMessageFile();
	}
}

void ApiWrap::finishMessages() {
	if (!_chatProcess) return;
	flushBatchStats();

	if (_isScanning) {
		_chatProcess->messagesTotalCount = _chatProcess->messagesProcessed;
		_chatProcess->fileProgress(ApiWrap::DownloadProgress{
			.itemIndex = _chatProcess->messagesProcessed,
			.ready = -1,
			.total = -1,
			.isAuxiliary = true,
			.messagesTotalCount = _chatProcess->messagesTotalCount
		});
	}

	Expects(!_chatProcess->slice.has_value());

	const auto process = base::take(_chatProcess);
	process->done();
}

Data::Message *ApiWrap::currentFileMessage() const {
	Expects(_chatProcess != nullptr);
	Expects(_chatProcess->slice.has_value());

	return &_chatProcess->slice->list[_chatProcess->fileIndex];
}

Data::FileOrigin ApiWrap::currentFileMessageOrigin() const {
	Expects(_chatProcess != nullptr);
	Expects(_chatProcess->slice.has_value());

	const auto splitIndex = _chatProcess->info.splits[
		_chatProcess->localSplitIndex];
	auto result = Data::FileOrigin();
	result.messageId = currentFileMessage()->id;
	result.split = (splitIndex >= 0)
		? splitIndex
		: (int(_splits.size()) + splitIndex);
	result.peer = (splitIndex >= 0)
		? _chatProcess->info.input
		: _chatProcess->info.migratedFromInput;
	return result;
}

void ApiWrap::requestTopicMessages(
		PeerId peerId,
		MTPInputPeer inputPeer,
		int32 topicRootId,
		FnMut<bool(int count)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::MessagesSlice&&)> slice,
		FnMut<void()> done) {
	Expects(_topicProcess == nullptr);
	Expects(_selfId.has_value());

	_topicProcess = std::make_unique<TopicProcess>();
	_topicProcess->context.selfPeerId = peerFromUser(*_selfId);
	_topicProcess->peerId = peerId;
	_topicProcess->inputPeer = inputPeer;
	_topicProcess->topicRootId = topicRootId;
	_topicProcess->relativePath = "chats/chat_"
		+ QString::number(peerId.value)
		+ "/topic_"
		+ QString::number(topicRootId)
		+ "/";
	_topicProcess->start = std::move(start);
	_topicProcess->fileProgress = std::move(progress);
	_topicProcess->handleSlice = std::move(slice);
	_topicProcess->done = std::move(done);

	mainRequest(MTPchannels_GetMessages(
		MTP_inputChannel(
			inputPeer.c_inputPeerChannel().vchannel_id(),
			inputPeer.c_inputPeerChannel().vaccess_hash()),
		MTP_vector<MTPInputMessage>(
			1,
			MTP_inputMessageID(MTP_int(topicRootId)))
	)).done([=](const MTPmessages_Messages &rootResult) {
		Expects(_topicProcess != nullptr);

		auto rootSlice = rootResult.match([&](
				const MTPDmessages_messagesNotModified &) {
			return Data::MessagesSlice();
		}, [&](const auto &data) {
			return Data::ParseMessagesSlice(
				_topicProcess->context,
				data.vmessages(),
				data.vusers(),
				data.vchats(),
				_topicProcess->relativePath);
		});

		auto rootSlicePtr = std::make_shared<Data::MessagesSlice>(
			std::move(rootSlice));

		requestTopicReplies(
			0,
			0,
			kMessagesSliceLimit,
			[=](const MTPmessages_Messages &result) {
				Expects(_topicProcess != nullptr);

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
				_topicProcess->localTotalCount = count;
				if (!_topicProcess->start(count)) {
					return;
				}

				if (!rootSlicePtr->list.empty()) {
					collectMessagesCustomEmoji(*rootSlicePtr);
					_topicProcess->slice = std::move(*rootSlicePtr);
					_topicProcess->fileIndex = 0;
					resolveTopicCustomEmoji();
					return;
				}

				requestTopicMessagesSlice();
			});
	}).send();
}

void ApiWrap::requestTopicMessagesSlice() {
	Expects(_topicProcess != nullptr);

	const auto offsetId = (_topicProcess->offsetId == 0)
		? 1
		: (_topicProcess->offsetId + 1);
	requestTopicReplies(
		offsetId,
		-kMessagesSliceLimit,
		kMessagesSliceLimit,
		[=](const MTPmessages_Messages &result) {
			Expects(_topicProcess != nullptr);

			result.match([&](const MTPDmessages_messagesNotModified &data) {
				error("Unexpected messagesNotModified received.");
			}, [&](const auto &data) {
				if constexpr (MTPDmessages_messages::Is<decltype(data)>()) {
					_topicProcess->lastSlice = true;
				}
				auto slice = Data::ParseMessagesSlice(
					_topicProcess->context,
					data.vmessages(),
					data.vusers(),
					data.vchats(),
					_topicProcess->relativePath);
				if (slice.list.empty()) {
					_topicProcess->lastSlice = true;
				}
				loadTopicMessagesFiles(std::move(slice));
			});
		});
}

void ApiWrap::requestTopicReplies(
		int offsetId,
		int addOffset,
		int limit,
		FnMut<void(MTPmessages_Messages&&)> done) {
	Expects(_topicProcess != nullptr);

	_topicProcess->requestDone = std::move(done);
	const auto doneHandler = [=](MTPmessages_Messages &&result) {
		Expects(_topicProcess != nullptr);
		base::take(_topicProcess->requestDone)(std::move(result));
	};

	mainRequest(MTPmessages_GetReplies(
		_topicProcess->inputPeer,
		MTP_int(_topicProcess->topicRootId),
		MTP_int(offsetId),
		MTP_int(0),
		MTP_int(addOffset),
		MTP_int(limit),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0)
	)).done(doneHandler).send();
}

void ApiWrap::loadTopicMessagesFiles(Data::MessagesSlice &&slice) {
	Expects(_topicProcess != nullptr);
	Expects(!_topicProcess->slice.has_value());

	collectMessagesCustomEmoji(slice);

	if (slice.list.empty()) {
		_topicProcess->lastSlice = true;
	}
	_topicProcess->slice = std::move(slice);
	_topicProcess->fileIndex = 0;

	resolveTopicCustomEmoji();
}

void ApiWrap::resolveTopicCustomEmoji() {
	if (_unresolvedCustomEmoji.empty()) {
		loadNextTopicMessageFile();
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
		resolveTopicCustomEmoji();
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
				_topicProcess->context,
				entry,
				_topicProcess->relativePath,
				TimeId());
			_resolvedCustomEmoji.emplace(document.id, std::move(document));
		}
		finalize();
	}).send();
}

void ApiWrap::loadNextTopicMessageFile() {
	Expects(_topicProcess != nullptr);
	Expects(_topicProcess->slice.has_value());

	_topicProcess->processing = true;
	const auto guard = gsl::finally([&] {
		_topicProcess->processing = false;
	});

	for (auto &list = _topicProcess->slice->list
		; _topicProcess->fileIndex < list.size()
		; ++_topicProcess->fileIndex) {
		auto &message = list[_topicProcess->fileIndex];
		if (!messageCustomEmojiReady(message)) {
			return;
		}
		const auto origin = Data::FileOrigin{
			.peer = _topicProcess->inputPeer,
			.messageId = message.id
		};
		const auto ready = processFileLoad(
			message.file(),
			origin,
			[=](FileProgress progress) { return loadTopicEmojiProgress(progress); },
			[=, &message](const QString &path) {
				loadTopicMessageFileOrThumbDone(message.file(), path);
			},
			&message);
		if (!ready) {
			return;
		}
		const auto thumbReady = processFileLoad(
			message.thumb().file,
			origin,
			[=](FileProgress progress) { return loadTopicEmojiProgress(progress); },
			[=, &message](const QString &path) {
				loadTopicMessageFileOrThumbDone(message.thumb().file, path);
			},
			&message);
		if (!thumbReady) {
			return;
		}
	}
	finishTopicMessagesSlice();
}

void ApiWrap::finishTopicMessagesSlice() {
	Expects(_topicProcess != nullptr);
	Expects(_topicProcess->slice.has_value());

	auto slice = *base::take(_topicProcess->slice);
	if (!slice.list.empty()) {
		_topicProcess->offsetId = slice.list.back().id;
		_topicProcess->processedCount += slice.list.size();
		if (!_topicProcess->handleSlice(std::move(slice))) {
			return;
		}
	}

	const auto reachedTotal = _topicProcess->localTotalCount > 0
		&& _topicProcess->processedCount >= _topicProcess->localTotalCount;

	if (!_topicProcess->lastSlice && !reachedTotal) {
		requestTopicMessagesSlice();
	} else {
		finishTopicMessages();
	}
}

void ApiWrap::loadTopicMessageFileOrThumbDone(
		Data::File &file,
		const QString &relativePath) {
	Expects(_topicProcess != nullptr);
	Expects(_topicProcess->slice.has_value());
	Expects((_topicProcess->fileIndex >= 0)
		&& (_topicProcess->fileIndex < _topicProcess->slice->list.size()));

	file.relativePath = relativePath;
	if (relativePath.isEmpty()) {
		file.skipReason = Data::File::SkipReason::Unavailable;
	}
	loadNextTopicMessageFile();
}

void ApiWrap::finishTopicMessages() {
	Expects(_topicProcess != nullptr);
	Expects(!_topicProcess->slice.has_value());

	const auto process = base::take(_topicProcess);
	process->done();
}

bool ApiWrap::processFileLoad(
		Data::File &file,
		const Data::FileOrigin &origin,
		Fn<bool(FileProgress)> progress,
		FnMut<void(QString)> done,
		Data::Message *message,
		Data::Story *story,
		bool isThumb) {
	using SkipReason = Data::File::SkipReason;

	using Type = MediaSettings::Type;
	const auto media = message
		? &message->media
		: story
		? &story->media
		: nullptr;
	auto type = Type(0);
	if (media) {
		type = v::match(media->content, [&](const Data::Document &data) {
			if (data.isSticker) return Type::Sticker;
			if (data.isVideoMessage) return Type::VideoMessage;
			if (data.isVoiceMessage) return Type::VoiceMessage;
			if (data.isAnimated) return Type::GIF;
			if (data.isVideoFile) return Type::Video;
			if (data.isAudioFile) return Type::Audio;
			return Type::File;
		}, [](const Data::Photo &data) {
			return Type::Photo;
		}, [](const Data::WebPage &data) {
			return Type::Link;
		}, [](const v::null_t &) {
			return Type::Text;
		}, [](const auto &data) {
			return static_cast<Type>(0);
		});
	} else if (message && message->file().location) {
		type = Type::Photo;
	} else {
		type = Type::Text;
	}

	// Fix: If it's a document but type was detected as 0, force it to File
	if (type == Type(0) && media && std::holds_alternative<Data::Document>(media->content)) {
		type = Type::File;
	}

	const auto fullSize = message
		? message->file().size
		: story
		? story->file().size
		: file.size;

	// Early skip checks (defensive - main filtering happens earlier in loadMessagesFiles)
	if (message && Data::SkipMessageByDate(*message, *_settings)) {
		file.skipReason = SkipReason::DateLimits;
		done(QString());
		return true;
	}

	const auto types = _settings->media.types;
	const bool fullHistorySelected = (types & MediaSettings::Type::FullHistory);
	const auto oversized = (file.location && _settings->media.sizeLimit > 0 && fullSize > _settings->media.sizeLimit && !fullHistorySelected && type != Type::Link);

	if (oversized) {
		file.skipReason = SkipReason::FileSize;
		done(QString());
		return true;
	}

	// Extract doc ID and filename for new unified dedup map.
	uint64 dedupDocId = 0;
	QString dedupName;
	if (message) {
		v::match(message->media.content, [&](const Data::Document &data) {
			dedupDocId = data.id;
			dedupName = QString::fromUtf8(data.name);
		}, [&](const Data::Photo &data) {
			dedupDocId = data.id;
		}, [](const auto &) {});
	}
	const int64 dedupSize = fullSize;

	const bool typeSelected = (types & type) || fullHistorySelected;
	const auto skipDownload = fullHistorySelected
		|| (types == MediaSettings::Types(0))
		|| !(types & type);

	// Extension filter: applies to Video, Audio, File, and Sticker types when a filter is active.
	if ((type == Type::Video || type == Type::Audio || type == Type::File || type == Type::Sticker)
			&& _settings->media.extensionFilterMode != MediaSettings::ExtFilterMode::None
			&& !_settings->media.extensionFilter.isEmpty()) {
		QString ext;
		if (type == Type::Sticker) {
			if (const auto doc = std::get_if<Data::Document>(&media->content)) {
				if (doc->isAnimated) ext = u"tgs"_q;
				else if (doc->isVideoFile) ext = u"webm"_q;
				else ext = u"webp"_q;
			}
		} else {
			const auto dotPos = dedupName.lastIndexOf(u'.');
			ext = (dotPos >= 0) ? dedupName.mid(dotPos + 1).toLower() : QString();
		}

		if (!ext.isEmpty()) {
			const auto &filterList = _settings->media.extensionFilter;
			const bool inList = filterList.contains(ext, Qt::CaseInsensitive);
			const bool blocked =
				(_settings->media.extensionFilterMode == MediaSettings::ExtFilterMode::Whitelist && !inList)
				|| (_settings->media.extensionFilterMode == MediaSettings::ExtFilterMode::Blacklist && inList);
			if (blocked) {
				file.skipReason = Data::File::SkipReason::FileType;
				done(QString());
				return true;
			}
		}
	}

	if (_isScanning) {
		done(QString());
		return true;
	}

	if (_stats
		&& (origin.messageId != 0 || origin.storyId != 0)
		&& !isThumb) {
		const bool isLinkOrText = (type == Type::Link || type == Type::Text);
		const bool alreadyProcessedInPreviousSession = _resumeMode
			&& _exportProgress
			&& message
			&& message->id <= static_cast<int32>(_exportProgress->lastMessageId);

		if (typeSelected && (!oversized || fullHistorySelected || isLinkOrText) && !alreadyProcessedInPreviousSession) {
			const auto dedup = dedupLookup(dedupDocId, dedupSize, dedupName);
			const bool alreadyVisited = dedup.found;

			if (!alreadyVisited) {
				if (type != Type::Link && type != Type::Text) {
					dedupRegister(dedupDocId, dedupSize, dedupName,
						skipDownload ? QString("processed") : QString());
				} else {
					dedupRegister(dedupDocId, dedupSize, dedupName, "processed");
				}
			}
			if (!alreadyVisited && !skipDownload) {
				if (type != Type::Link && type != Type::Text) {
					dedupRegister(dedupDocId, dedupSize, dedupName, QString());
				}
			}
		}
	}

	if (!file.relativePath.isEmpty()
		|| file.skipReason != SkipReason::None) {
		done(file.relativePath);
		return true;
	} else if (!isThumb) {
		// Check global dedup first (cross-chat deduplication)
		if (_globalDedup && dedupDocId != 0) {
			if (_globalDedup->hasDocumentId(dedupDocId)) {
				file.isDuplicate = true;
				// Still process locally but mark as duplicate
			}
		}
		if (_globalDedup && !file.isDuplicate && dedupSize > 0 && !dedupName.isEmpty()) {
			if (_globalDedup->hasFingerprint(dedupName, dedupSize)) {
				file.isDuplicate = true;
			}
		}

		if (file.isDuplicate) {
			file.skipReason = SkipReason::Duplicate;
			done(QString());
			return true;
		}

		const auto dedup = dedupLookup(dedupDocId, dedupSize, dedupName);
		if (dedup.found) {
			if (!dedup.path.isEmpty() && dedup.path != "processed") {
				file.relativePath = dedup.path;
				done(file.relativePath);
				return true;
			} else if (dedup.path == "processed") {
				file.skipReason = SkipReason::FileType;
				done(QString());
				return true;
			} else if (dedup.path.isEmpty()) {
				uint64 activeRandomId = 0;
				if (dedupDocId != 0) {
					const auto it = _dedupByIdInProgress.find(dedupDocId);
					if (it != _dedupByIdInProgress.end()) {
						activeRandomId = it->second;
					}
				}
				if (!activeRandomId && dedupSize > 0 && !dedupName.isEmpty()) {
					const auto it = _dedupBySizeNameInProgress.find({ dedupSize, dedupName });
					if (it != _dedupBySizeNameInProgress.end()) {
						activeRandomId = it->second;
					}
				}
				if (activeRandomId) {
					const auto pit = _fileProcesses.find(activeRandomId);
					if (pit != _fileProcesses.end()) {
						pit->second->pendingDone.push_back(
							[filePtr = &file, doneMove = std::move(done)](QString path) mutable {
								filePtr->relativePath = path;
								doneMove(path);
							});
						return true;
					}
				}
			}
		}
	}

	auto wrapDone = [=, done = std::move(done)](const QString &path) mutable {
		if (!isThumb) {
			if (!path.isEmpty()) {
				dedupUpdate(dedupDocId, dedupSize, dedupName, path);
			}
		}
		done(path);
	};

	if (!file.location && file.content.isEmpty()) {
		file.skipReason = SkipReason::Unavailable;
		wrapDone(QString());
		return true;
	} else if (writePreloadedFile(file, origin)) {
		wrapDone(file.relativePath);
		return true;
	}

	if (!story && skipDownload) {
		file.skipReason = SkipReason::FileType;
		wrapDone(QString());
		return true;
	} else if (oversized) {
		file.skipReason = SkipReason::FileSize;
		wrapDone(QString());
		return true;
	}

	// Register as in-progress BEFORE starting download so duplicates can find it
	const auto dedupSizeName = SizeNameKey{ dedupSize, dedupName };
	if (!isThumb) {
		if (dedupDocId != 0) {
			_dedupByIdInProgress[dedupDocId] = 1; // Placeholder
		}
		if (dedupSize > 0 && !dedupName.isEmpty()) {
			_dedupBySizeNameInProgress[dedupSizeName] = 1; // Placeholder
		}
	}

	loadFile(file, origin, LocationKey(), std::move(progress), std::move(wrapDone), dedupDocId, dedupSizeName);

	// Update with real randomId after loadFile assigns it
	if (file.randomId && !isThumb) {
		if (dedupDocId != 0) {
			_dedupByIdInProgress[dedupDocId] = file.randomId;
		}
		if (dedupSize > 0 && !dedupName.isEmpty()) {
			_dedupBySizeNameInProgress[dedupSizeName] = file.randomId;
		}
	}

	return true;
}

bool ApiWrap::writePreloadedFile(
		Data::File &file,
		const Data::FileOrigin &origin) {
	Expects(_settings != nullptr);

	using namespace Output;

	// Inline content (small files sent as raw bytes, not downloaded).
	if (!file.content.isEmpty()) {
		const auto &folder = _settings->path;
		const auto &suggested = file.suggestedPath;
		const auto position = suggested.indexOf(QLatin1Char('.'));
		const auto base = (position >= 0) ? suggested.mid(0, position) : suggested;
		const auto ext = (position >= 0) ? suggested.mid(position) : QString();
		
		auto relativePath = Output::File::PrepareRelativePath(folder, suggested);
		if (_reservedPaths.contains(relativePath)) {
			int attempt = 0;
			do {
				++attempt;
				relativePath = base + QString(" (%1)").arg(attempt) + ext;
			} while (QFile::exists(folder + relativePath)
				|| _reservedPaths.contains(relativePath));
		}
		_reservedPaths.emplace(relativePath);

		// Use .partial extension during write
		const auto partialPath = folder + relativePath + u".partial"_q;
		
		// Write to partial file (always from scratch for inline content)
		{
			Output::File outputFile(partialPath, _stats);
			if (const auto result = outputFile.writeBlock(file.content)) {
				file.relativePath = relativePath;
				
				// Close file before rename
				outputFile.close();
				
				// Rename .partial file to final name
				const auto finalPath = folder + relativePath;
				if (QFile::exists(partialPath) && !QFile::exists(finalPath)) {
					if (!QFile::rename(partialPath, finalPath)) {
						QFile::copy(partialPath, finalPath);
						QFile::remove(partialPath);
					}
				} else if (QFile::exists(partialPath) && QFile::exists(finalPath)) {
					QFile::remove(partialPath);
				}
				
				// Track progress
				if (_exportProgress) {
					onFileCompleted(relativePath, outputFile.size(), 0);
				}
			} else {
				ioError(result);
			}
		}
		return true;
	}
	return false;
}

void ApiWrap::loadFile(
		Data::File &file,
		const Data::FileOrigin &origin,
		const LocationKey &dedupKey,
		std::function<bool(FileProgress)> progress,
		base::unique_function<void(QString)> done,
		uint64 dedupDocId,
		const SizeNameKey &dedupSizeName) {
	Expects(file.location);

	auto process = prepareFileProcess(file, origin, dedupKey);
	process->progress = std::move(progress);
	process->done = std::move(done);
	process->dedupKey = dedupKey;
	process->dedupDocId = dedupDocId;
	process->dedupSizeName = dedupSizeName;

	const auto randomId = process->randomId;
	file.randomId = randomId;
	process->location.randomId = randomId;

	_fileProcesses.emplace(randomId, std::move(process));
	_fileDownloadQueue.push_back(randomId);

	scheduleMoreFiles();
}

std::unique_ptr<ApiWrap::FileProcess> ApiWrap::prepareFileProcess(
	Data::File &file,
	const Data::FileOrigin &origin,
	const LocationKey &dedupKey)
{
	Expects(_settings != nullptr);
	const auto &folder = _settings->path;
	const auto &suggested = file.suggestedPath;
	const auto position = suggested.indexOf(QLatin1Char('.'));
	const auto base = (position >= 0) ? suggested.mid(0, position) : suggested;
	const auto ext	= (position >= 0) ? suggested.mid(position) : QString();
	auto relativePath = Output::File::PrepareRelativePath(folder, suggested);
	if (_reservedPaths.contains(relativePath)) {
		int attempt = 0;
		do {
			++attempt;
			relativePath = base + QString(" (%1)").arg(attempt) + ext;
		} while (QFile::exists(folder + relativePath)
			|| _reservedPaths.contains(relativePath));
	}
	_reservedPaths.emplace(relativePath);

	// ALWAYS use .partial extension for downloads (for resume support)
	auto finalPath = folder + relativePath + u".partial"_q;

	// Check for existing .partial file and determine initial offset (for resume)
	int64 initialOffset = 0;
	{
		QFileInfo partialInfo(finalPath);
		if (partialInfo.exists() && partialInfo.size() > 0 && partialInfo.size() < file.size) {
			// Valid partial file found - resume from this offset
			initialOffset = partialInfo.size();
		} else if (partialInfo.exists() && partialInfo.size() >= file.size) {
			// File appears complete - use it and skip download
			initialOffset = file.size;
		}
	}

	auto result = std::make_unique<FileProcess>(
		file,
		finalPath,
		_stats,
		initialOffset);

	result->relativePath = relativePath;
	result->location = file.location;
	result->size = file.size;
	result->origin = origin;
	result->randomId = base::RandomValue<uint64>();
	result->dedupKey = dedupKey;
	result->offset = initialOffset;

	return result;
}

void ApiWrap::scheduleMoreFiles() {
	_scheduleMoreFilesPending = false;

	// Count currently active downloads (just 1 at a time)
	int activeCount = 0;
	for (const auto &[id, process] : _fileProcesses) {
		if (process->active) {
			++activeCount;
		}
	}

	if (activeCount >= 1) {
		return;  // Already downloading, wait for completion
	}

	// Activate first file in queue
	if (!_fileDownloadQueue.empty()) {
		const auto pid = _fileDownloadQueue.front();
		_fileDownloadQueue.pop_front();
		
		const auto pit = _fileProcesses.find(pid);
		if (pit != end(_fileProcesses)) {
			auto &process = *pit->second;
			process.active = true;
			loadFilePart(process);
		}
	}
}


void ApiWrap::finishFile(uint64 randomId, const QString &relativePath) {
	auto it = _fileProcesses.find(randomId);
	if (it == end(_fileProcesses)) {
		return;
	}

	// Track progress: file completed
	if (!relativePath.isEmpty() && _exportProgress) {
		const auto &process = it->second;
		const auto messageId = process->origin.messageId;
		onFileCompleted(process->relativePath, process->outputFile.size(), messageId);
	}
    

	// --- CORRECT FIX: Send 100% progress BEFORE erasing from the map ---
	// This ensures the internal callbacks can still find the file in _fileProcesses
	// and successfully forward the "complete" signal to the UI to clear the progress bar.
	if (it->second->progress) {
		auto finalSize = it->second->size > 0 ? it->second->size : it->second->outputFile.size();
		if (finalSize <= 0) {
			finalSize = 1; // Guarantee ready == total and > 0 so the UI clears it immediately
		}
		it->second->progress({
			.randomId = randomId,
			.ready = finalSize,
			.total = finalSize
		});
	}
    
	auto process = std::move(it->second);

	if (process->active) {
		--_filesDownloading;
	}

	// ALWAYS rename .partial file to final name on completion
	QString actualRelativePath = relativePath;
	if (!relativePath.isEmpty()) {
		// CRITICAL: Close the file handle before renaming.
		// On Windows, an open file handle will prevent rename.
		process->outputFile.close();
		
		const auto partialPath = process->outputFile.path();
		const auto finalPath = _settings->path + relativePath;
		
		if (QFile::exists(partialPath)) {
			if (QFile::exists(finalPath)) {
				// Final file already exists, clean up partial
				QFile::remove(partialPath);
			} else {
				// Try to rename
				if (!QFile::rename(partialPath, finalPath)) {
					// Rename failed - try copy + delete as fallback
					if (QFile::copy(partialPath, finalPath)) {
						QFile::remove(partialPath);
					} else {
						LOG(("Export Error: FAILED to rename or copy partial '%1' to final '%2'").arg(partialPath, finalPath));
					}
				}
			}
		}
	}

	process->fileRef.relativePath = actualRelativePath;
	if (actualRelativePath.isEmpty()) {
		process->fileRef.skipReason = Data::File::SkipReason::Unavailable;
	}

	// Update global dedup manager after successful download
	if (!actualRelativePath.isEmpty() && _globalDedup && !process->fileRef.isDuplicate) {
		if (process->dedupDocId != 0) {
			_globalDedup->addDocumentId(process->dedupDocId);
		}
		if (process->dedupSizeName.size > 0 && !process->dedupSizeName.name.isEmpty()) {
			_globalDedup->addFingerprint(process->dedupSizeName.name, process->dedupSizeName.size);
		}
		_globalDedup->save();
	}

	// Fire any duplicate callbacks that were waiting for this download.
	// The dedup map now has the real path, so duplicates get the same path.
	for (auto &cb : process->pendingDone) {
		cb(relativePath);
	}

	process->done(relativePath);

	_fileProcesses.erase(it);

	// Clean up in-progress tracking entries for this process.
	for (auto it2 = _dedupByIdInProgress.begin();
			it2 != _dedupByIdInProgress.end(); ) {
		if (it2->second == randomId) {
			it2 = _dedupByIdInProgress.erase(it2);
		} else {
			++it2;
		}
	}
	for (auto it2 = _dedupBySizeNameInProgress.begin();
			it2 != _dedupBySizeNameInProgress.end(); ) {
		if (it2->second == randomId) {
			it2 = _dedupBySizeNameInProgress.erase(it2);
		} else {
			++it2;
		}
	}

	scheduleMoreFiles();
}

void ApiWrap::loadFilePart(FileProcess &process) {
	if (!process.active) {
		return;
	}

	const auto randomId = process.randomId;
	const auto isSameDc = (process.location.dcId == _mainDcId);
	const auto chunkSize = kChunkSize;
	const auto maxConcurrent = isSameDc ? kSameDcConcurrentChunks : kDifferentDcConcurrentChunks;

	auto &throttler = isSameDc ? _throttlerSameDc : _throttlerDifferentDc;

	// Check how many chunks are already scheduled/in-flight
	const auto currentScheduled = int(process.scheduledOffsets.size()) + int(process.activeRequestOffsets.size());
	if (currentScheduled >= maxConcurrent) {
		return;  // At capacity, wait for completion
	}

	// First, retry failed offset (if any)
	while (!process.pendingRetryOffsets.empty()) {
		const auto currentInFlight = int(process.scheduledOffsets.size()) + int(process.activeRequestOffsets.size());
		if (currentInFlight >= maxConcurrent) {
			return;  // At capacity
		}

		const auto retryOffset = process.pendingRetryOffsets.front();
		process.pendingRetryOffsets.pop_front();

		// If this offset is already scheduled, skip it
		if (process.scheduledOffsets.contains(retryOffset)) {
			continue;
		}

		auto &requests = process.requests;
		requests.push_back({ retryOffset });
		process.scheduledOffsets.insert(retryOffset);

		// Schedule via throttler
		throttler.schedule([=] {
			const auto it = _fileProcesses.find(randomId);
			if (it == end(_fileProcesses) || !it->second->active) {
				return;
			}
			auto &proc = *it->second;

			const auto requestId = fileRequest(
				proc.location,
				retryOffset,
				chunkSize
			).done([=](const MTPupload_File &result) {
				filePartDone(randomId, retryOffset, result);
			}).fail([=](const MTP::Error &error) {
				// Handle errors
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

				// For other errors (including FLOOD_WAIT and server errors),
				// propagate to error handler like the origin repo does.
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

		// Don't return - continue to fill more slots if available
	}

	// Schedule fresh chunks while we have capacity
	while (true) {
		const auto currentInFlight = int(process.scheduledOffsets.size()) + int(process.activeRequestOffsets.size());
		if (currentInFlight >= maxConcurrent) {
			return;  // At capacity
		}

		if (process.size > 0 && process.offset >= process.size) {
			return;  // All chunks requested
		}

		const auto offset = process.offset;
		process.requests.push_back({ offset });
		process.offset += chunkSize;
		process.scheduledOffsets.insert(offset);

		// Schedule via throttler (spaces chunk STARTS by delay)
		throttler.schedule([=] {
			const auto it = _fileProcesses.find(randomId);
			if (it == end(_fileProcesses) || !it->second->active) {
				return;
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

				// For other errors (including FLOOD_WAIT and server errors),
				// propagate to error handler like the origin repo does.
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
		finishFile(randomId, process.relativePath);
	} else if (process.active) {
		// File still downloading - schedule more chunks if capacity available
		loadFilePart(process);
		scheduleMoreFiles();
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
	clearState();
	_ioErrors.fire_copy(result);
}

void ApiWrap::flushBatchStats() {
	if (!_chatProcess) return;

	for (auto &[type, batch] : _chatProcess->batchStats) {
		

		const auto typeInt = static_cast<int>(type);
		auto &target = _exportProgress->typeCounters[typeInt];
		
		const bool trusted = _usingServerCounts || _serverCountTrustedTypes.contains(type);
		const auto localTotalCountIncr = trusted ? 0 : batch.localTotalCount;
		const auto linkMsgIncr = (type == MediaSettings::Type::Link && trusted) ? 0 : batch.messagesWithLinks;

		if (_usingServerCounts) {
			target.uniqueCount += batch.uniqueCount;
			target.uniqueSize += batch.uniqueSize;
			target.totalSize += batch.totalSize;
			if (type == MediaSettings::Type::Link) {
				target.localTotalCount += batch.localTotalCount;
			}
		} else {
			target.localTotalCount += batch.localTotalCount;
			target.totalSize += batch.totalSize;
			target.uniqueCount += batch.uniqueCount;
			target.uniqueSize += batch.uniqueSize;
			if (type != MediaSettings::Type::Link) {
				target.messagesWithLinks += batch.messagesWithLinks;
			}
		}

		if (_isScanning) {
			_scanStats->increment(type, batch.totalSize, batch.uniqueSize, localTotalCountIncr, batch.uniqueCount, linkMsgIncr);
		} else if (_stats) {
			_stats->increment(type, batch.totalSize, batch.uniqueSize, localTotalCountIncr, batch.uniqueCount, linkMsgIncr);
		}
	}
	_chatProcess->batchStats.clear();

	_chatProcess->messagesProcessed += _chatProcess->batchProcessed;
	_exportProgress->messagesProcessed = _chatProcess->messagesProcessed;
	_chatProcess->batchProcessed = 0;
}

void ApiWrap::onMessagePartDone(int index, bool isSelected) {
	if (!_chatProcess) return;
	auto &done = _chatProcess->messageFilesDone[index];
	const auto need = _chatProcess->messageFilesRequired[index];
	if (++done < std::max(need, 1)) {
		return;
	}

	const auto &ms = _chatProcess->messageStats[index];
	const auto &s = *_chatProcess->slice;
	const auto messageId = s.list[index].id;

	const bool alreadyCounted = (_resumeIdThreshold > 0 && messageId <= _resumeIdThreshold);
	if (!alreadyCounted) {
		if (messageId > _exportProgress->lastMessageId) {
			_exportProgress->lastMessageId = messageId;
		}

		using Type = MediaSettings::Type;

		_chatProcess->batchProcessed++;

		if (ms.linkMsgIncr > 0) {
			auto &linkBatch = _chatProcess->batchStats[Type::Link];
			linkBatch.localTotalCount += ms.links;
			if (ms.selected) {
				linkBatch.uniqueCount += ms.linksUnique;
			}
			linkBatch.messagesWithLinks += ms.linkMsgIncr;
		}

		const bool scanningLinksOnly = _isScanning && (_settings->media.types == Type::Link);
		if (ms.type != Type::Link && !scanningLinksOnly) {
			auto &batch = _chatProcess->batchStats[ms.type];
			if (ms.selected) {
				batch.localTotalCount++;
				batch.totalSize += ms.size;
				if (ms.unique) {
					batch.uniqueCount++;
					batch.uniqueSize += ms.size;
				}
			}
		}

		if (done == std::max(need, 1)) {
			if (_isScanning) {
				_scanStats->incrementTotalMessages();
				if (_chatProcess->messagesTotalCount == 0 || _scanStats->totalMessagesCount() > _chatProcess->messagesTotalCount) {
					_chatProcess->messagesTotalCount = _scanStats->totalMessagesCount();
				}
			} else if (_stats) {
				_stats->incrementTotalMessages();
				const bool hasRange = (_settings->singlePeerFrom != 0 || _settings->singlePeerTill != 0) || _settings->useIdRange;
				if (_chatProcess->messagesTotalCount == 0 || (hasRange && _stats->totalMessagesCount() > _chatProcess->messagesTotalCount)) {
					_chatProcess->messagesTotalCount = _stats->totalMessagesCount();
				}
			}
		}

		const bool lastOfScan = _isScanning && _chatProcess->lastSlice && (_chatProcess->messagesProcessed + _chatProcess->batchProcessed >= _chatProcess->messagesTotalCount);
		const bool endOfRange = !_isScanning && (_chatProcess->messagesTotalCount > 0 && _chatProcess->messagesProcessed + _chatProcess->batchProcessed >= _chatProcess->messagesTotalCount);
		if (_chatProcess->batchProcessed >= 100 || endOfRange || lastOfScan) {
			flushBatchStats();
			if (!_isScanning && (_chatProcess->messagesProcessed % 5000 == 0 || endOfRange)) {
				saveProgress();
			}
			_chatProcess->fileProgress(ApiWrap::DownloadProgress{
				.itemIndex = _chatProcess->messagesProcessed,
				.ready = -1,
				.total = -1,
				.isAuxiliary = true,
				.messagesTotalCount = _chatProcess->messagesTotalCount
			});
		}
	}

	if (!_isScanning && need > 0) {
		if (isSelected) {
			Data::MessagesSlice single;
			single.list.push_back(std::move(_chatProcess->slice->list[index]));
			if (!_chatProcess->handleSlice(std::move(single))) {
				return;
			}
		}
	}
}

void ApiWrap::clearResults() {
	if (_chatProcess) {
		base::take(_chatProcess)->done();
	}
	_stats = nullptr;
	_scanStats = nullptr;
	_dedupById.clear();
	_dedupBySizeName.clear();
	_dedupByIdInProgress.clear();
	_dedupBySizeNameInProgress.clear();
	_visitedLinks.clear();
	_reservedPaths.clear();
}

ApiWrap::DedupResult ApiWrap::dedupLookup(
		uint64 docId,
		int64 size,
		const QString &name) const {
	if (docId != 0) {
		const auto it = _dedupById.find(docId);
		if (it != _dedupById.end()) {
			return { true, it->second };  // filter 1 skip
		}
	}
	if (size > 0 && !name.isEmpty()) {
		const auto it = _dedupBySizeName.find({ size, name });
		if (it != _dedupBySizeName.end()) {
			return { true, it->second };  // filter 2 skip
		}
	}
	return { false, {} };  // passes both filters → download
}

void ApiWrap::dedupRegister(
		uint64 docId,
		int64 size,
		const QString &name,
		const QString &path) {
	if (docId != 0) {
		_dedupById.emplace(docId, path);
	}
	if (size > 0 && !name.isEmpty()) {
		_dedupBySizeName.emplace(SizeNameKey{ size, name }, path);
	}
}

void ApiWrap::dedupUpdate(
		uint64 docId,
		int64 size,
		const QString &name,
		const QString &path) {
	if (docId != 0) {
		_dedupById[docId] = path;
	}
	if (size > 0 && !name.isEmpty()) {
		_dedupBySizeName[SizeNameKey{ size, name }] = path;
	}
}

base::flat_set<QString> ApiWrap::visitedLinks() const {
	return _visitedLinks;
}

void ApiWrap::clearState(bool keepCache) {
	_takeoutId = std::nullopt;
	_takeoutFlags = 0;
	_takeoutSizeLimit = 0;
	_settings = nullptr;
	_isScanning = false;
	if (!keepCache) {
		clearResults();
	} else {
		_stats = nullptr;
		_scanStats = nullptr;
	}
	_startProcess = nullptr;
	_contactsProcess = nullptr;
	_userpicsProcess = nullptr;
	_storiesProcess = nullptr;
	_otherDataProcess = nullptr;
	_leftChannelsProcess = nullptr;
	_dialogsProcess = nullptr;
	_chatProcess = nullptr;
	_fileProcesses.clear();
	_fileDownloadQueue.clear();
	_filesDownloading = 0;
	_scheduleMoreFilesPending = false;
	_unresolvedCustomEmoji.clear();
	_resolvedCustomEmoji.clear();
}

// =================== Resume Support ===================

void ApiWrap::loadProgress(const QString &folder) {
	const auto path = ExportProgress::progressFilePath(folder);
	_exportProgress = ExportProgress::load(path);
}

void ApiWrap::saveProgress() {
	if (!_exportProgress || !_settings) {
		return;
	}
	if (_isScanning) {
		return;
	}
	_exportProgress->settings = *_settings;
	if (_chatProcess) {
		_exportProgress->messagesProcessed = _chatProcess->messagesProcessed;
		_exportProgress->messagesTotalCount = _chatProcess->messagesTotalCount;
	}
	if (_stats) {
		const auto byType = _stats->byType();
		_exportProgress->typeCounters.clear();
		for (const auto &[type, item] : byType) {
			TypeCounter counter;
			counter.uniqueCount = item.uniqueCount;
			counter.uniqueSize = item.uniqueSize;
			counter.localTotalCount = item.localTotalCount;
			counter.totalSize = item.totalSize;
			counter.messagesWithLinks = item.messagesWithLinks;
			_exportProgress->typeCounters[static_cast<int>(type)] = counter;
		}
	}

	if (_scanStats) {
		_exportProgress->scanTotalMessages = _scanStats->totalMessagesCount();
		const auto byType = _scanStats->byType();
		_exportProgress->scanStats.clear();
		for (const auto &[type, item] : byType) {
			TypeCounter counter;
			counter.uniqueCount = item.uniqueCount;
			counter.uniqueSize = item.uniqueSize;
			counter.localTotalCount = item.localTotalCount;
			counter.totalSize = item.totalSize;
			counter.messagesWithLinks = item.messagesWithLinks;
			_exportProgress->scanStats[static_cast<int>(type)] = counter;
		}
	}

	_exportProgress->dedupById.clear();
	for (const auto &[id, path] : _dedupById) {
		_exportProgress->dedupById[id] = path;
	}
	_exportProgress->dedupBySizeName.clear();
	for (const auto &[key, path] : _dedupBySizeName) {
		const QString mapKey = QString::number(key.size) + "_" + key.name;
		_exportProgress->dedupBySizeName[mapKey] = path;
	}

	const auto path = ExportProgress::progressFilePath(_settings->path);
	
	// Ensure the export directory exists before saving
	QDir dir(_settings->path);
	if (!dir.exists()) {
		if (!dir.mkpath(dir.absolutePath())) {
			LOG(("Export Error: Failed to create directory {1}").arg(_settings->path));
			return;
		}
	}
	
	if (!_exportProgress->save(path)) {
		LOG(("Export Error: Failed to save progress to {1}").arg(path));
	}
}

void ApiWrap::updateMessageProgress(uint64 messageId) {
	if (!_exportProgress || messageId == 0) {
		return;
	}
	if (messageId > _exportProgress->lastMessageId) {
		_exportProgress->lastMessageId = messageId;
		saveProgress();
	}
}

void ApiWrap::onFileCompleted(const QString &filename, int64 size, uint64 messageId) {
	if (!_exportProgress) {
		return;
	}
	
	_exportProgress->lastFilename = filename;
	_exportProgress->lastFileSize = size;
	if (messageId > _exportProgress->lastMessageId) {
		_exportProgress->lastMessageId = messageId;
	}
	
	// Remove from incomplete files list if present
	auto &incomplete = _exportProgress->incompleteFiles;
	incomplete.erase(
		std::remove_if(incomplete.begin(), incomplete.end(),
			[&](const IncompleteFile &f) { return f.filename == filename; }),
		incomplete.end()
	);
	
	saveProgress();
}

void ApiWrap::onFileStarted(const QString &filename, int64 totalSize, uint64 messageId) {
	if (!_exportProgress) {
		return;
	}
	
	// Add to incomplete files list
	auto &incomplete = _exportProgress->incompleteFiles;
	// Remove old entry if exists
	incomplete.erase(
		std::remove_if(incomplete.begin(), incomplete.end(),
			[&](const IncompleteFile &f) { return f.filename == filename; }),
		incomplete.end()
	);
	
	IncompleteFile entry;
	entry.filename = filename;
	entry.bytesDownloaded = 0;
	entry.totalSize = totalSize;
	entry.messageId = messageId;
	incomplete.push_back(std::move(entry));
	
	saveProgress();
}

void ApiWrap::removePartialFile(const QString &filename) {
	if (!_settings) {
		return;
	}
	const auto partialPath = _settings->path + '/' + filename + u".partial"_q;
	QFile::remove(partialPath);
}

ApiWrap::~ApiWrap() {
	if (_lifetimeGuard) {
		*_lifetimeGuard = false;
	}
}

} // namespace Export
