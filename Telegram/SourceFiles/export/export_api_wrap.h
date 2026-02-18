/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/mtproto_concurrent_sender.h"
#include "data/data_peer_id.h"

namespace base {
class Timer;
} // namespace base

namespace Export {
namespace Data {
struct File;
struct Chat;
struct Document;
struct FileLocation;
struct PersonalInfo;
struct UserpicsInfo;
struct UserpicsSlice;
struct StoriesInfo;
struct StoriesSlice;
struct ContactsList;
struct SessionsList;
struct DialogsInfo;
struct DialogInfo;
struct MessagesSlice;
struct Message;
struct Story;
struct FileOrigin;
} // namespace Data

namespace Output {
struct Result;
class Stats;
} // namespace Output

struct Settings;

class ApiWrap {
public:
	struct LocationKey {
		uint64 type = 0;
		uint64 id = 0;

		LocationKey() = default;
		LocationKey(uint64 type, uint64 id) : type(type), id(id) {
		}

		inline bool operator<(const LocationKey &other) const {
			return std::tie(type, id) < std::tie(other.type, other.id);
		}
	};

	ApiWrap(
		base::weak_qptr<MTP::Instance> weak,
		Fn<void(FnMut<void()>)> runner);

	rpl::producer<MTP::Error> errors() const;
	rpl::producer<Output::Result> ioErrors() const;

	struct StartInfo {
		int userpicsCount = 0;
		int storiesCount = 0;
		int dialogsCount = 0;
	};
	void startExport(
		const Settings &settings,
		Output::Stats *stats,
		FnMut<void(StartInfo)> done,
		bool isScanning = false,
		Output::Stats *scanStats = nullptr);

	void requestDialogsList(
		Fn<bool(int count)> progress,
		FnMut<void(Data::DialogsInfo&&)> done);

	void requestPersonalInfo(FnMut<void(Data::PersonalInfo&&)> done);

	void requestOtherData(
		const QString &suggestedPath,
		FnMut<void(Data::File&&)> done);

	struct DownloadProgress {
		uint64 randomId = 0;
		QString path;
		int itemIndex = 0;
		int64 ready = 0;
		int64 total = 0;
		bool isAuxiliary = false;
		int messagesTextCount = 0;
		int messagesMediaCount = 0;
		int messagesTotalCount = 0;
		int messagesTextTotal = 0;
		int messagesInRangeCount = 0;
		int messagesUniqueCount = 0;
	};
	void requestUserpics(
		FnMut<bool(Data::UserpicsInfo&&)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::UserpicsSlice&&)> slice,
		FnMut<void()> finish);

	void requestStories(
		FnMut<bool(Data::StoriesInfo&&)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::StoriesSlice&&)> slice,
		FnMut<void()> finish);

	void requestContacts(FnMut<void(Data::ContactsList&&)> done);

	void requestSessions(FnMut<void(Data::SessionsList&&)> done);

	void requestMessages(
		const Data::DialogInfo &info,
		int64 fromId,
		int64 tillId,
		FnMut<bool(const Data::DialogInfo &)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::MessagesSlice&&)> slice,
		FnMut<void()> done,
		int messagesInRangeCount = 0);

	void finishExport(FnMut<void()> done);
	void skipFile(uint64 randomId);
	void cancelExportFast(bool keepCache = false);
	void clearState(bool keepCache = false);

	void clearResults();

	[[nodiscard]] base::flat_set<QString> visitedLinks() const;

	~ApiWrap();

private:
	class LoadedFileCache;
	struct StartProcess;
	struct ContactsProcess;
	struct UserpicsProcess;
	struct StoriesProcess;
	struct OtherDataProcess;
	struct FileProcess;
	struct FileProgress {
		uint64 randomId = 0;
		int64 ready = 0;
		int64 total = 0;
	};
	struct ChatsProcess;
	struct LeftChannelsProcess;
	struct DialogsProcess;
	struct ChatProcess;

	void startMainSession(FnMut<void()> done);
	void sendNextStartRequest();
	void requestUserpicsCount();
	void requestStoriesCount();
	void requestSplitRanges();
	void requestDialogsCount();
	void requestLeftChannelsCount();
	void finishStartProcess();

	void requestTopPeersSlice();

	void handleUserpicsSlice(const MTPphotos_Photos &result);
	void loadUserpicsFiles(Data::UserpicsSlice &&slice);
	void loadNextUserpic();
	bool loadUserpicProgress(FileProgress value);
	void loadUserpicDone(const QString &relativePath);
	void finishUserpicsSlice();
	void finishUserpics();

	void handleStoriesSlice(const MTPstories_Stories &result);
	void loadStoriesFiles(Data::StoriesSlice &&slice);
	void loadNextStory();
	bool loadStoryProgress(FileProgress value);
	bool loadStoryProgress(FileProgress value, bool auxiliary);
	void loadStoryDone(const QString &relativePath);
	bool loadStoryThumbProgress(FileProgress value);
	void loadStoryThumbDone(const QString &relativePath);
	void finishStoriesSlice();
	void finishStories();

	void otherDataDone(const QString &relativePath);

	bool useOnlyLastSplit() const;

	void requestDialogsSlice();
	void appendDialogsSlice(Data::DialogsInfo &&info);
	void finishDialogsList();
	void requestSinglePeerDialog();
	mtpRequestId requestSinglePeerMigrated(const Data::DialogInfo &info);
	void appendSinglePeerDialogs(Data::DialogsInfo &&info);

	void requestLeftChannelsIfNeeded();
	void requestLeftChannelsList(
		Fn<bool(int count)> progress,
		FnMut<void(Data::DialogsInfo&&)> done);
	void requestLeftChannelsSliceGeneric(FnMut<void()> done);
	void requestLeftChannelsSlice();
	void appendLeftChannelsSlice(Data::DialogsInfo &&info);

	void appendChatsSlice(
		ChatsProcess &process,
		std::vector<Data::DialogInfo> &to,
		std::vector<Data::DialogInfo> &&from,
		int splitIndex);

	void requestMessagesCount(int localSplitIndex);
	void checkFirstMessageDate(int localSplitIndex, int count);
	void messagesCountLoaded(int localSplitIndex, int count);
	void resolveDates();
	void requestMediaCounts();
	void requestMessagesSlice();
	void requestChatMessages(
		int splitIndex,
		int offsetId,
		int addOffset,
		int limit,
		FnMut<void(MTPmessages_Messages&&)> done);
	void collectMessagesCustomEmoji(const Data::MessagesSlice &slice);
	void resolveCustomEmoji();
	void loadMessagesFiles(Data::MessagesSlice &&slice);
	void loadNextMessageFile();
	[[nodiscard]] std::optional<QByteArray> getCustomEmoji(QByteArray &data);
	bool messageCustomEmojiReady(Data::Message &message);
	bool loadMessageFileProgress(FileProgress value);
	bool loadMessageFileProgress(FileProgress value, bool auxiliary);
	void loadMessageFileDone(int index, const QString &relativePath);
	bool loadMessageThumbProgress(FileProgress value);
	void loadMessageThumbDone(int index, const QString &relativePath);
	bool loadMessageEmojiProgress(FileProgress progress);
	void loadMessageEmojiDone(uint64 id, const QString &relativePath);
	void finishMessagesSlice();
	void finishMessages();

	void onMessagePartDone(int index, bool isSelected = true);

	[[nodiscard]] Data::Message *currentFileMessage() const;
	[[nodiscard]] Data::FileOrigin currentFileMessageOrigin() const;

	[[nodiscard]] MTPMessagesFilter getFilter() const;

	void processFileLoad(
		Data::File &file,
		const Data::FileOrigin &origin,
		Fn<bool(FileProgress)> progress,
		FnMut<void(QString)> done,
		Data::Message *message = nullptr,
		Data::Story *story = nullptr,
		bool isThumb = false);
	std::unique_ptr<FileProcess> prepareFileProcess(
		Data::File &file,
		const Data::FileOrigin &origin,
		const LocationKey &dedupKey) const;
	bool writePreloadedFile(
		Data::File &file,
		const Data::FileOrigin &origin);
	void loadFile(
		Data::File &file,
		const Data::FileOrigin &origin,
		const LocationKey &dedupKey,
		Fn<bool(FileProgress)> progress,
		FnMut<void(QString)> done);
	void scheduleMoreFiles();	
	void loadFilePart(FileProcess &process);
	void finishFile(uint64 randomId, const QString &relativePath);
	void filePartDone(uint64 randomId, int64 offset, const MTPupload_File &result);
	void scheduleBatchDelay(crl::time delay);
	void filePartUnavailable(uint64 randomId);
	void filePartRefreshReference(uint64 randomId, int64 offset);
	void filePartExtractReference(uint64 randomId, int64 offset, const MTPmessages_Messages &result);
	void filePartExtractReference(uint64 randomId, int64 offset, const MTPstories_Stories &result);

	template <typename Request>
	class RequestBuilder;

	template <typename Request>
	[[nodiscard]] auto mainRequest(
		Request &&request,
		std::optional<uint64> takeoutId = std::nullopt);

	template <typename Request>
	[[nodiscard]] auto splitRequest(int index, Request &&request);

	[[nodiscard]] auto fileRequest(
		const Data::FileLocation &location,
		int64 offset,
		int chunkSize);

	void error(const MTP::Error &error);
	void error(const QString &text);
	void ioError(const Output::Result &result);

	MTP::ConcurrentSender _mtp;
	std::optional<uint64> _takeoutId;
	std::optional<UserId> _selfId;
	MTPInputUser _user = MTP_inputUserSelf();
	std::unique_ptr<Settings> _settings;
	Output::Stats *_stats = nullptr;
	bool _isScanning = false;
	bool _usingServerCounts = false;
	Output::Stats *_scanStats = nullptr;

	std::unique_ptr<StartProcess> _startProcess;
	std::unique_ptr<LoadedFileCache> _fileCache;
	std::unique_ptr<ContactsProcess> _contactsProcess;
	std::unique_ptr<UserpicsProcess> _userpicsProcess;
	std::unique_ptr<StoriesProcess> _storiesProcess;
	std::unique_ptr<OtherDataProcess> _otherDataProcess;
	
	std::map<uint64, std::unique_ptr<FileProcess>> _fileProcesses;
	std::deque<uint64> _fileDownloadQueue;
	class RequestThrottler {
	public:
		RequestThrottler(
			Fn<void(FnMut<void()>)> runner,
			std::shared_ptr<bool> guard);
		void schedule(FnMut<void()> task);
		void tryProcessQueue();
		[[nodiscard]] Fn<void(FnMut<void()>)> runner() const {
			return _runner;
		}
		~RequestThrottler();

	private:
		void processQueueNow();
		void refreshTokens();

		Fn<void(FnMut<void()>)> _runner;
		std::shared_ptr<bool> _guard;
		std::deque<FnMut<void()>> _taskQueue;
		int _tokens = 1; // 
		crl::time _lastRefresh = 0;
		bool _retryScheduled = false;
	};

	int _filesDownloading = 0;

	std::shared_ptr<bool> _lifetimeGuard;
	RequestThrottler _throttler;
	
	std::unique_ptr<LeftChannelsProcess> _leftChannelsProcess;
	std::unique_ptr<DialogsProcess> _dialogsProcess;
	std::unique_ptr<ChatProcess> _chatProcess;
	base::flat_set<uint64> _unresolvedCustomEmoji;
	base::flat_map<uint64, Data::Document> _resolvedCustomEmoji;
	QVector<MTPMessageRange> _splits;

	rpl::event_stream<MTP::Error> _errors;
	rpl::event_stream<Output::Result> _ioErrors;

	base::flat_map<LocationKey, QString> _scanVisited;
	base::flat_map<LocationKey, QString> _exportVisited;
	base::flat_set<QString> _visitedLinks;
	base::flat_map<LocationKey, std::vector<FnMut<void(QString)>>> _pendingFileCallbacks;
	FnMut<void()> _finishDone;
};

} // namespace Export
