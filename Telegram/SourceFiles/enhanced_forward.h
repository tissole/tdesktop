/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <optional>
#include "data/data_peer_id.h"
#include "rpl/producer.h"
#include "api/api_common.h"

class HistoryItem;
class PhotoData;
class DocumentData;
class ApiWrap;
struct SendingAlbum;
struct FilePrepareResult;

namespace Storage {
struct UploadedMedia;
struct UploadProgress;
} // namespace Storage

namespace Api {
struct RemoteFileInfo;
} // namespace Api

namespace Data {
enum class GroupingOptions;
class PhotoMedia;
} // namespace Data

namespace EnhancedForward {

struct Split {
	std::vector<not_null<HistoryItem*>> restricted;
	std::vector<not_null<HistoryItem*>> normal;
};

[[nodiscard]] Split classifyItems(
	const std::vector<not_null<HistoryItem*>> &items);
[[nodiscard]] bool checkMsgRestriction(not_null<HistoryItem*> item);

enum class State : uint8_t {
	Idle,
	Sending,
	Paused,
	Finished,
	Cancelled,
};

struct ItemInfo {
	QString name;
	qint64 size = 0;
};

enum class ItemState : uint8 {
	Pending,
	Downloading,
	Uploading,
	Done,
	Failed,
};

struct TrackedItem {
	ItemState state = ItemState::Pending;
	ItemInfo info;
	float64 progress = 0;
};

struct ForwardProgress {
	State state = State::Idle;
	int sent = 0;
	int total = 0;
	PeerId destPeer;
	int currentDownload = -1;
	int currentUpload = -1;
	ItemInfo downloadItem;
	ItemInfo uploadItem;
	float64 downloadProgress = 0;
	float64 uploadProgress = 0;
	qint64 downloadSpeed = 0;
	qint64 uploadSpeed = 0;
	std::vector<TrackedItem> items;
};

void startForwardSession(
		not_null<Main::Session*> session,
		const PeerId &peerId,
		int totalItems,
		Fn<void()> saveCallback);

void markItemSent(
	not_null<Main::Session*> session,
	const PeerId &peerId);

void markItemSkipped(
	not_null<Main::Session*> session,
	const PeerId &peerId);

void cancelForward(
	const PeerId &id,
	not_null<Main::Session*> session);

void pauseForward(
	const PeerId &id,
	not_null<Main::Session*> session);

void resumeForward(
	const PeerId &id,
	not_null<Main::Session*> session);

void cancelCurrentItem(
	const PeerId &id,
	not_null<Main::Session*> session);

void setCancelCallback(
	const PeerId &id,
	not_null<Main::Session*> session,
	Fn<void()> callback);

void setPauseCallback(
	const PeerId &id,
	not_null<Main::Session*> session,
	Fn<void()> callback);

void setResumeCallback(
	const PeerId &id,
	not_null<Main::Session*> session,
	Fn<void()> callback);

void updateDownloadProgress(
	not_null<Main::Session*> session,
	const PeerId &peerId,
	int itemIndex,
	const ItemInfo &info,
	float64 progress);

void updateUploadProgress(
	not_null<Main::Session*> session,
	const PeerId &peerId,
	int itemIndex,
	const ItemInfo &info,
	float64 progress);

void updateItemState(
	not_null<Main::Session*> session,
	const PeerId &peerId,
	int itemIndex,
	ItemState state,
	const ItemInfo &info,
	float64 progress);

[[nodiscard]] bool isForwarding(const PeerId &id);

[[nodiscard]] std::optional<PeerId> activeJobPeer();

void saveProgressForPeer(
	const PeerId &peer,
	not_null<Main::Session*> session);

[[nodiscard]] bool isPaused(const PeerId &id);

[[nodiscard]] ForwardProgress currentProgress(const PeerId &id);

void ClearProgressForPeer(const PeerId &peerId);
void CleanupPartialFilesForPeer(
	not_null<Main::Session*> session,
	const PeerId &peerId);

struct SavedJob {
	PeerId srcId = PeerId();
	PeerId dstId = PeerId();
	int total = 0;
	int sent = 0;
	std::vector<FullMsgId> sourceMsgs;
	std::vector<bool> uploadDone;
	std::vector<uint64> fileId;
	std::vector<int> uploadedParts;
};

[[nodiscard]] std::vector<SavedJob> GetUnfinishedJobs();

[[nodiscard]] std::optional<SavedJob> GetUnfinishedJobByDst(
	const PeerId &dstId);

// Fires the destination peer whenever its forward state changes.
[[nodiscard]] rpl::producer<PeerId> stateChanges();

struct ItemTask {
	FullMsgId sourceId;
	QString path;
	bool textOnly = false;
	bool isPhoto = false;
	MessageGroupId sourceGroup;

	bool needsDownload = false;
	bool downloadStarted = false;
	bool downloadDone = false;
	qint64 downloadedBytes = 0;
	std::shared_ptr<Data::PhotoMedia> photoView;
	uint64 mediaId = 0;
	QByteArray fileHash;
	qint64 fileSize = 0;
	bool dedupNeedsHash = false;
	bool dedupSkipped = false;

	bool uploadStarted = false;
	bool uploadDone = false;
	bool sent = false;
	FullMsgId localMsgId;
	HistoryItem *sentItem = nullptr;
	FullMsgId uploadId;
	std::shared_ptr<FilePrepareResult> prepared;
	Api::RemoteFileInfo uploadInfo;
	uint64 fileId = 0;
	int uploadedParts = 0;
	int retries = 0;
	int lastSavedPct = -10;
	qint64 partSize = 0;
};

class Pipeline final : public std::enable_shared_from_this<Pipeline> {
public:
	static void Start(
		not_null<ApiWrap*> api,
		std::vector<not_null<HistoryItem*>> &&items,
		const Api::SendAction &action,
		Data::GroupingOptions groupOptions,
		std::shared_ptr<SavedJob> resumeJob);

	Pipeline(
		not_null<ApiWrap*> api,
		std::vector<not_null<HistoryItem*>> &&items,
		const Api::SendAction &action,
		Data::GroupingOptions groupOptions,
		std::shared_ptr<SavedJob> resumeJob);

	~Pipeline();

private:
	void run();
	void saveProgress();
	bool loadProgress();
	void setupCallbacks();
	void sendNext();
	void pumpUploads();
	void startUploadForItem(int idx);
	void onUploadDone(const Storage::UploadedMedia &data);
	void onUploadFail(const FullMsgId &fullId);
	void onUploadProgress(const Storage::UploadProgress &data);
	void checkItem(int idx);
	void pumpDownloads();
	void dedupCheckItem(int idx);
	void skipAsDuplicate(int idx);

	not_null<ApiWrap*> _api;
	Main::Session &_session;
	Api::SendAction _action;
	Data::GroupingOptions _groupOptions;

	int _n = 0;
	std::vector<ItemTask> _items;
	std::shared_ptr<base::flat_map<FullMsgId, int>> _uploadIndex;
	base::flat_map<
		MessageGroupId,
		std::shared_ptr<SendingAlbum>> _albums;

	int _current = 0;
	bool _downloadInFlight = false;
	bool _uploadInFlight = false;
	
	std::shared_ptr<rpl::lifetime> _uploadLifetime;
	std::shared_ptr<rpl::lifetime> _dlLifetime;

	QString _downloadPath;
	uint64 _runId = 0;

	PeerId _peerId;
	PeerId _srcPeer;
	int _sent = 0;
	int _skippedCount = 0;

	int _downloadCursor = 0;
	int _uploadCursor = 0;
};

} // namespace EnhancedForward