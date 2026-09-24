/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QByteArray>

#include "export/export_settings.h"
#include "export/data/export_data_types.h"
#include "export/output/export_output_abstract.h"
#include "data/data_dedup_db.h"
#include "mtproto/mtproto_concurrent_sender.h"
#include "data/data_peer_id.h"

namespace Data {
class DedupDb;
} // namespace Data

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
struct ProfileMusicInfo;
struct ProfileMusicSlice;
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
	ApiWrap(
		base::weak_qptr<MTP::Instance> weak,
		Fn<void(FnMut<void()>)> runner);

	rpl::producer<MTP::Error> errors() const;
	rpl::producer<Output::Result> ioErrors() const;

	struct StartInfo {
		int userpicsCount = 0;
		int storiesCount = 0;
		int profileMusicCount = 0;
		int dialogsCount = 0;
	};
	void startExport(
		const Settings &settings,
		Output::Stats *stats,
		FnMut<void(StartInfo)> done);

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

	void requestProfileMusic(
		FnMut<bool(Data::ProfileMusicInfo&&)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::ProfileMusicSlice&&)> slice,
		FnMut<void()> finish);

	void requestContacts(FnMut<void(Data::ContactsList&&)> done);

	void requestSessions(FnMut<void(Data::SessionsList&&)> done);

	void requestMessages(
		const Data::DialogInfo &info,
		FnMut<bool(const Data::DialogInfo &)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::MessagesSlice&&)> slice,
		FnMut<void()> done);

	void requestRangeTotal(Fn<void(int)> done);
	[[nodiscard]] bool hasSelectedTotal() const;
	[[nodiscard]] int selectedTotal() const;
	[[nodiscard]] int rangeDenominator() const;
	[[nodiscard]] int chatSelectedDone() const;

	void requestTopicMessages(
		PeerId peerId,
		MTPInputPeer inputPeer,
		int32 topicRootId,
		FnMut<bool(int count)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::MessagesSlice&&)> slice,
		FnMut<void()> done);

	void finishExport(FnMut<void()> done);
	void skipFile(uint64 randomId);
	void cancelExportFast();
	bool requestPause();
	void resumeExport();
	[[nodiscard]] bool exportPaused() const;
	rpl::producer<bool> pauseChanges() const;
	void setPauseFlushHandler(
		Fn<Data::MessagesSlice(Data::MessagesSlice)> handler);
	void setSessionId(uint64 sessionId);
	void setDedupDb(const QString &path);
	void setScanMode(bool scan);
	void setResumeCheckpoint(const ::Data::ExResumeRecord &record);
	void setWriterStateGetter(Fn<Output::DialogState()> getter);
	void refreshTakeoutSession(FnMut<void(uint64)> done);
	void setSharedTakeoutId(uint64 id);
	void setTakeoutRefreshHook(Fn<void()> hook);
	void takeoutRefreshDone(uint64 id);
	void setUpdateCheck(int knownTotal, Fn<void(int newCount)> handler);
	void proceedUpdate();
	void abortUpdate();
	[[nodiscard]] std::vector<QString> linkUrls() const;

	~ApiWrap();

private:
	struct StartProcess;
	struct ContactsProcess;
	struct UserpicsProcess;
	struct StoriesProcess;
	struct ProfileMusicProcess;
	struct OtherDataProcess;
	struct FileProcess;
	struct FileProgress;
	struct FilePolicy;
	struct MessageFileWork;
	struct ChatsProcess;
	struct LeftChannelsProcess;
	struct DialogsProcess;
	struct AbstractMessagesProcess;
	struct ChatProcess;
	struct TopicProcess;

	struct Range {
		bool active = false;
		int32 from = 0;
		int32 till = 0;

		// Upper bound only, and it cannot describe the selected kinds.
		// 0 when a bound is unset.
		[[nodiscard]] int span() const {
			return (from > 0 && till >= from) ? int(till - from + 1) : 0;
		}

		// A lower bound is an existing id, so the range cannot be empty.
		[[nodiscard]] bool nonEmpty() const {
			return active && from > 0;
		}
	};

	[[nodiscard]] Range currentRange() const;
	[[nodiscard]] Range currentRange(int splitPosition) const;

	void requestScanCount();
	void decideScanMethod();
	bool scanAdvanceFilter();

	void startMainSession(FnMut<void()> done);
	void sendNextStartRequest();
	void requestUserpicsCount();
	void requestStoriesCount();
	void requestProfileMusicCount();
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
	void loadStoryDone(const QString &relativePath);
	bool loadStoryThumbProgress(FileProgress value);
	void loadStoryThumbDone(const QString &relativePath);
	void finishStoriesSlice();
	void finishStories();

	void handleProfileMusicSlice(const MTPusers_SavedMusic &result);
	void loadProfileMusicFiles(Data::ProfileMusicSlice &&slice);
	void loadNextProfileMusic();
	bool loadProfileMusicProgress(FileProgress value);
	void loadProfileMusicDone(const QString &relativePath);
	bool loadProfileMusicThumbProgress(FileProgress value);
	void loadProfileMusicThumbDone(const QString &relativePath);
	void finishProfileMusicSlice();
	void finishProfileMusic();

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
	void messagesCountLoaded(int localSplitIndex, int count);
	void resolveDates();
	void requestMessagesSlice();
	void consumeChatPage(MTPmessages_Messages result);
	void firePagePrefetch();
	bool setupExportSearch();
	void requestExportCounts();
	void fireExportCountSlot(int filterIndex, int splitPosition);
	void decideExportSearch();
	void fireScanCountSlot(int filterIndex, int splitPosition);
	void requestChatMessages(
		int splitIndex,
		int offsetId,
		int addOffset,
		int limit,
		bool withRange,
		FnMut<void(MTPmessages_Messages&&)> done);
	void startMessagesSlice(Data::MessagesSlice &&slice);
	void resumeMessagesSlice();
	void hydrateMessageDone(
		bool topic,
		int index,
		int32 rawId,
		const MTPmessages_Messages &result,
		int gen);
	void requestTopicMessagesSlice();
	void requestTopicReplies(
		int offsetId,
		int addOffset,
		int limit,
		FnMut<void(MTPmessages_Messages&&)> done);
	void collectMessagesCustomEmoji(const Data::MessagesSlice &slice);
	void resolveCustomEmoji();
	void buildMessageFileWork(
		AbstractMessagesProcess &process,
		Data::Message &message);
	void loadNextMessageFile();
	[[nodiscard]] std::optional<QByteArray> getCustomEmoji(
		Data::Message &message,
		QByteArray &data);
	bool messageCustomEmojiReady(Data::Message &message);
	bool loadMessageFileProgress(int index, FileProgress value);
	void loadMessageFileDone(
		int index,
		Data::File *file,
		const QString &relativePath);
	bool loadMessageEmojiProgress(FileProgress progress);
	void loadMessageEmojiDone(uint64 id, const QString &relativePath);
	void finishMessagesSlice();
	void finishMessages();

	void loadTopicMessagesFiles(Data::MessagesSlice &&slice);
	void resolveTopicCustomEmoji();
	void loadNextTopicMessageFile();
	bool loadTopicMessageFileProgress(int index, FileProgress value);
	void loadTopicMessageFileDone(
		int index,
		Data::File *file,
		const QString &relativePath);
	bool loadTopicEmojiProgress(FileProgress progress);
	void loadCustomEmojiDone(uint64 id, const QString &relativePath);
	void finishTopicMessagesSlice();
	void finishTopicMessages();

	[[nodiscard]] Data::Message *currentFileMessage() const;
	[[nodiscard]] Data::FileOrigin currentFileMessageOrigin() const;

	bool processFileLoad(
		Data::File &file,
		const Data::FileOrigin &origin,
		Fn<bool(FileProgress)> progress,
		FnMut<void(QString)> done,
		const FilePolicy &policy);
	bool processFileLoad(
		Data::File &file,
		const Data::FileOrigin &origin,
		Fn<bool(FileProgress)> progress,
		FnMut<void(QString)> done,
		Data::Message *message = nullptr,
		Data::Story *story = nullptr);
	bool decideFileWithMedia(
		Data::File &file,
		Data::FileOrigin origin,
		const FilePolicy &policy,
		Fn<bool(FileProgress)> progress,
		FnMut<void(QString)> done);
	bool skipDuplicateById(Data::File &file, const FilePolicy &policy);
	void recordFinishedContent(Data::File &file, const FilePolicy &policy);
	void finishFileRecord(Data::File *file);
	void noteMediaWritten(const Data::File &file);
	bool decideFileWithoutMedia(
		Data::File &file,
		const FilePolicy &policy,
		FnMut<void(QString)> done);
	bool decideFileScan(
		Data::File &file,
		const FilePolicy &policy,
		FnMut<void(QString)> done);
	bool mainFileDuplicate(const FilePolicy &policy);
	void beginSliceWalk(bool topic);
	bool skipMedia() const;
	void clearDedupRun();
	void clearExTmpOnly();
	void commitExportProgress(int32 committedMax, const QString &state);
	::Data::DedupDb *dedupDb() const;
	PeerId currentPeer() const;
	Data::File *walkParkedFile(const Data::File *file) const;
	std::unique_ptr<FileProcess> prepareFileProcess(
		const Data::File &file,
		const Data::FileOrigin &origin);
	bool filePartChunkFailed(int64 offset);
	bool writePreloadedFile(
		Data::File &file,
		const Data::FileOrigin &origin);
	void loadFile(
		const Data::File &file,
		const Data::FileOrigin &origin,
		Fn<bool(FileProgress)> progress,
		FnMut<void(QString)> done);
	void loadFilePart();
	void filePartDone(int64 offset, const MTPupload_File &result);
	void parkForPause();
	void filePartUnavailable();
	[[nodiscard]] QString filePartMediaFolder() const;
	void filePartRetryReference(
		int64 offset,
		Data::FileLocation location);
	void filePartRefreshReference(int64 offset);
	void filePartExtractCustomEmojiReference(
		int64 offset,
		uint64 customEmojiId,
		const QString &folder,
		const MTPVector<MTPDocument> &result);
	void filePartExtractRichReference(
		int64 offset,
		const Data::FileOrigin &origin,
		const QString &folder,
		const MTPmessages_Messages &result);
	void filePartExtractReference(
		int64 offset,
		const MTPmessages_Messages &result);
	void filePartExtractReference(
		int64 offset,
		const MTPstories_Stories &result);

	template <typename Request>
	class RequestBuilder;

	template <typename Request>
	[[nodiscard]] auto mainRequest(Request &&request);

	template <typename Request>
	[[nodiscard]] auto splitRequest(int index, Request &&request);

	[[nodiscard]] auto fileRequest(
		const Data::FileLocation &location,
		int64 offset);

	void error(const MTP::Error &error);
	void error(const QString &text);
	void ioError(const Output::Result &result);

	Fn<void(FnMut<void()>)> _runner;
	MTP::ConcurrentSender _mtp;
	std::optional<uint64> _takeoutId;
	std::optional<UserId> _selfId;
	Output::Stats *_stats = nullptr;
	uint64 _sessionId = 0;
	std::unique_ptr<::Data::DedupDb> _dedupDb;
	bool _resumeArmed = false;
	int32 _resumeLastId = 0;
	int _resumeSplitIndex = 0;
	int _resumeSelectedDone = 0;
	int _resumeFilterIndex = 0;
	bool _resumeFilterRedo = false;
	int32 _walkIdFloor = 0;
	uint64 _resumeDocId = 0;
	QString _resumePausedFile;
	QString _resumeFolder;
	base::flat_map<QString, uint64> _preparedFileIds;
	bool _updateMode = false;
	int _updateKnownTotal = 0;
	Fn<void(int newCount)> _updateCheckHandler;
	Fn<Output::DialogState()> _writerStateGetter;

	std::unique_ptr<Settings> _settings;
	MTPInputUser _user = MTP_inputUserSelf();

	std::unique_ptr<StartProcess> _startProcess;
	std::unique_ptr<ContactsProcess> _contactsProcess;
	std::unique_ptr<UserpicsProcess> _userpicsProcess;
	std::unique_ptr<StoriesProcess> _storiesProcess;
	std::unique_ptr<ProfileMusicProcess> _profileMusicProcess;
	std::unique_ptr<OtherDataProcess> _otherDataProcess;
	std::unique_ptr<FileProcess> _fileProcess;
	std::unique_ptr<LeftChannelsProcess> _leftChannelsProcess;
	std::unique_ptr<DialogsProcess> _dialogsProcess;
	std::unique_ptr<ChatProcess> _chatProcess;
	std::unique_ptr<TopicProcess> _topicProcess;
	base::flat_set<uint64> _unresolvedCustomEmoji;
	base::flat_map<uint64, Data::Document> _resolvedCustomEmoji;
	QVector<MTPMessageRange> _splits;
	struct DedupPending {
		uint64 docId = 0;
		QByteArray hash;
		bool photo = false;
		PeerId peer = 0;
		MediaSettings::Type type = MediaSettings::Type();
	};
	base::flat_map<Data::File*, DedupPending> _pendingHash;
	base::flat_map<uint64, QByteArray> _knownFileHash;
	base::flat_map<uint64, QByteArray> _knownFileContent;
	base::flat_set<uint64> _hashFailedDocs;
	base::flat_set<QByteArray> _knownLinks;
	base::flat_set<QByteArray> _linkUrls;
	base::flat_set<uint64> _inflightDocs;
	base::flat_set<const Data::File*> _decidingFiles;
	base::flat_set<PeerId> _dedupPeers;
	base::flat_set<uint64> _scanSeenMessages;
	bool _scanMode = false;
	int _dedupGen = 0;
	int _sliceGen = 0;

	rpl::event_stream<MTP::Error> _errors;
	rpl::event_stream<Output::Result> _ioErrors;
	rpl::event_stream<bool> _pauseChanges;
	bool _pauseRequested = false;
	bool _paused = false;
	bool _takeoutRefreshing = false;
	crl::time _takeoutRefreshedAt = 0;
	std::vector<FnMut<void(uint64)>> _takeoutWaiters;
	Fn<void()> _takeoutRefreshHook;
	bool _takeoutInvalidPending = false;
	Fn<Data::MessagesSlice(Data::MessagesSlice)> _pauseFlushHandler;
	int _pauseFlushedPrefix = 0;

};

} // namespace Export