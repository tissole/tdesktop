/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <optional>
#include <deque>
#include <utility>
#include "base/timer.h"
#include "base/unique_function.h"
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
enum class ForwardOptions;
enum class GroupingOptions;
class PhotoMedia;
} // namespace Data

namespace EnhancedForward {

struct Split {
	std::vector<not_null<HistoryItem*>> restricted;
	std::vector<not_null<HistoryItem*>> normal;
};

// Fetches the no-forwards restriction flag for each distinct source peer
// asynchronously, then calls `done` with the classified Split. Never blocks.
void classifyItems(
	const std::vector<not_null<HistoryItem*>> &items,
	base::unique_function<void(Split)> done);

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
	float64 downloadProgress = -1; // -1 = no download phase
	float64 uploadProgress = -1;   // -1 = no upload phase
	bool textOnly = false;
	bool cancelled = false;
	bool sent = false;
	bool dedupSkipped = false;
};

struct ForwardProgress {
	State state = State::Idle;
	int sent = 0;
	int total = 0;
	int skipped = 0;
	PeerId destPeer;
	int currentDownload = -1;
	int currentUpload = -1;
	ItemInfo downloadItem;
	ItemInfo uploadItem;
	float64 downloadProgress = 0;
	float64 uploadProgress = 0;
	qint64 downloadSpeed = 0;
	qint64 uploadSpeed = 0;
	std::vector<FullMsgId> sourceIds;
	std::vector<TrackedItem> items;
};

struct JobSnapshot {
	PeerId peer;
	PeerId srcPeer;
	ForwardProgress progress;
	bool active = false;    // running/paused now
	bool finished = false;  // finished/cancelled, awaiting clear
	bool resumable = false; // persisted unfinished DB row, no live state
};

void startForwardSession(
		not_null<Main::Session*> session,
		const PeerId &peerId,
		const PeerId &srcPeer,
		const std::vector<FullMsgId> &sourceIds,
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
	int unfinishedFiles = 0;
	std::vector<FullMsgId> sourceMsgs;
	std::vector<bool> uploadDone;
	std::vector<uint64> fileId;
	std::vector<int> uploadedParts;
};

[[nodiscard]] qint64 PersistedForwardBytes(
	not_null<Main::Session*> session,
	qint64 *total = nullptr);

// Downloaded bytes of one persisted forward item (from its temp file),
// or nullopt when the item has no pending record.
[[nodiscard]] std::optional<qint64> persistedItemBytes(
	not_null<Main::Session*> session,
	const FullMsgId &sourceId);

// Total file size of one persisted forward item (from DB file_size),
// or nullopt when the item has no pending record.
[[nodiscard]] std::optional<qint64> persistedItemFileSize(
	not_null<Main::Session*> session,
	const FullMsgId &sourceId);

[[nodiscard]] std::vector<SavedJob> GetUnfinishedJobs(
	not_null<Main::Session*> session);

[[nodiscard]] std::vector<SavedJob> GetFinishedJobs(
	not_null<Main::Session*> session);

void EnsureForwardSourceMessages(
	not_null<Main::Session*> session,
	const std::vector<FullMsgId> &sourceIds,
	Fn<void(bool succeeded)> done);

[[nodiscard]] std::optional<SavedJob> GetUnfinishedJobByDst(
	const PeerId &dstId,
	not_null<Main::Session*> session);

// Fires the destination peer whenever its forward state changes.
[[nodiscard]] rpl::producer<PeerId> stateChanges();

// Fires whenever the forward job counters (total/sent/skipped) change, so the
// UI can refresh the counter text from the maintained totals - mirrors the
// download/upload jobCounterChanged signals (no scanning, no polling).
[[nodiscard]] rpl::producer<> counterChanges();

// In-memory job snapshots only (ActiveStates + FinishedStates): the cheap
// push-based view used by jobsValue and by the UI update path. No DB reads.
[[nodiscard]] std::vector<JobSnapshot> MemoryJobs(
	not_null<Main::Session*> session);
// Same as MemoryJobs plus the persisted resume rows that were not seeded yet
// (one-off SQL, used by context menus and other non-hot paths).
[[nodiscard]] std::vector<JobSnapshot> AllJobs(
	not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<std::vector<JobSnapshot>> jobsValue(
	not_null<Main::Session*> session);

// Loads the persisted resume rows once and merges them into the in-memory
// states, so a job left unfinished or finished from a previous run is
// rendered by the push-based jobsValue stream without reading the database
// on every update. Safe to call multiple times per session (idempotent).
void EnsureResumeStatesSeeded(not_null<Main::Session*> session);

// The last completed forward's (done, total): keeps the transfer-manager
// counter visible until the next forward replaces it, across restarts.
[[nodiscard]] std::pair<int, int> LastBatchCounts();

// Returns true if the given upload id is currently handled by an active
// Enhanced Forward pipeline (so it should be shown as "EF", not a plain
// upload in the transfer manager / upload bar).
[[nodiscard]] bool isEnhancedUpload(const FullMsgId &uploadId);

// Returns true if the given upload source path lives inside the Enhanced
// Forward temp directory. Used as a fallback marker for uploads that were
// interrupted before their id could be registered (e.g. after restart).
[[nodiscard]] bool isEnhancedTempUpload(
	not_null<Main::Session*> session,
	const QString &filename);

void cancelItem(
	not_null<Main::Session*> session,
	const PeerId &peer,
	int itemIndex);
// True if the given message is currently a part of an active enhanced-forward
// job (used to route per-item cancel actions away from the generic download
// / upload cancel).
[[nodiscard]] bool isEnhancedForwardItem(
	not_null<Main::Session*> session,
	not_null<const HistoryItem*> item);
// Cancels just that one message inside its active forward job.
void cancelItemByMessage(
	not_null<Main::Session*> session,
	not_null<const HistoryItem*> item);
void CancelAll(not_null<Main::Session*> session);

void notifyTransfersUpdated();
void ClearFinished(
	not_null<Main::Session*> session,
	const PeerId &peer);
// Removes a single finished forwarded item (by its source message) from the
// Forwards history and from the persisted done list.
void ClearFinishedItems(
	not_null<Main::Session*> session,
	const FullMsgId &sourceId);

// Shows a quit-confirmation box if an enhanced forward is running, then calls
// quit on confirmation (the global quit flow aggregates all accounts).

struct ItemTask {
	FullMsgId sourceId;
	QString path;
	bool textOnly = false;
	bool isPhoto = false;
	bool cancelled = false;
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
	bool dedupHashPending = false;
	bool dedupPrechecked = false;
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
	[[nodiscard]] static auto Active()
		-> std::unordered_map<PeerId, std::weak_ptr<Pipeline>> &;

	static void Start(
		not_null<ApiWrap*> api,
		std::vector<not_null<HistoryItem*>> &&items,
		const Api::SendAction &action,
		Data::ForwardOptions forwardOptions,
		Data::GroupingOptions groupOptions,
		std::shared_ptr<SavedJob> resumeJob);

	Pipeline(
		not_null<ApiWrap*> api,
		std::vector<not_null<HistoryItem*>> &&items,
		const Api::SendAction &action,
		Data::ForwardOptions forwardOptions,
		Data::GroupingOptions groupOptions,
		std::shared_ptr<SavedJob> resumeJob);

	~Pipeline();

	void cancelItem(int idx);
	void adjustAlbumCount(int idx);

	[[nodiscard]] bool containsUpload(const FullMsgId &uploadId) const {
		return _uploadIndex && _uploadIndex->find(uploadId) != end(*_uploadIndex);
	}

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
	void downloadFailed(int idx);
	void dedupCheckItem(int idx);
	void dedupHashed(
		int idx,
		QByteArray &&hash,
		qint64 size,
		uint64 mediaId,
		bool needsDelete);
	void skipAsDuplicate(int idx);
	void premarkDuplicate(int idx);
	void startSession();
	void runNextPrecheck();
	void remotePrechecked(int idx, QByteArray &&hash, qint64 size);
	void refreshSourceItemState(int idx);
	void appendItems(std::vector<not_null<HistoryItem*>> &&items);

	not_null<ApiWrap*> _api;
	Main::Session &_session;
	Api::SendAction _action;
	Data::ForwardOptions _forwardOptions;
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
	crl::time _lastDlProgressMs = 0;
	crl::time _lastShadowRepaintMs = 0;

	PeerId _peerId;
	PeerId _srcPeer;
	int _sent = 0;
	int _skippedCount = 0;

	int _downloadCursor = 0;
	int _uploadCursor = 0;
	std::deque<int> _dedupPrecheckQueue;
};

} // namespace EnhancedForward