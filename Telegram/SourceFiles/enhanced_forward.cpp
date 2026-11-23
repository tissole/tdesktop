/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "enhanced_forward.h"

#include "apiwrap.h"
#include "bulk_forward/bulk_forward_counters.h"
#include "bulk_forward/bulk_forward_resume.h"
#include "rpl/rpl.h"
#include "base/debug_log.h"
#include "base/call_delayed.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "data/data_msg_id.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include <QDir>
#include <QFile>
#include <QDataStream>
#include <algorithm>
#include <atomic>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_set>
#include <utility>
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "main/main_account.h"
#include "main/main_domain.h"

// HIGH-LEVEL SENDING AND ITEM RESOLUTION HELPERS:
#include "api/api_sending.h"
#include "api/api_text_entities.h"
#include "history/history_item_components.h"
#include "history/history_item_helpers.h"
#include "main/main_session_settings.h"
#include "main/main_account.h"
#include "storage/file_upload.h"
#include "storage/localimageloader.h"
#include "api/api_media.h"
#include "ui/image/image_prepare.h"
#include "lang/lang_keys.h"
#include "core/file_utilities.h"
#include "ui/text/text_utilities.h"
#include "logs.h"
#include "base/random.h"
#include <crl/crl.h>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>

// ADDITIONAL INTERNAL DATA TYPES REQUIRED FOR THE STAGE PIPELINE:
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_forward_batching.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_file_origin.h"
#include "data/data_histories.h"
#include "data/business/data_shortcut_messages.h"
#include "ui/chat/attach/attach_prepare.h"
#include "storage/storage_account.h"
#include "core/application.h"
#include "data/data_download_manager.h"
#include "info/downloads/info_downloads_widget.h"
#include "data/data_file_hash.h"
#include "data/data_forum_topic.h"
#include "ui/toast/toast.h"
#include "ui/boxes/confirm_box.h"
#include "window/window_controller.h"
#include "styles/style_layers.h"
#include "styles/style_boxes.h"
#include "settings.h"

#include <algorithm>

namespace EnhancedForward {
namespace {

struct SharedState {
	// Identifies the batch that owns this state. A finished batch is moved out
	// of ActiveStates and the next batch to the same peer gets a new value, so
	// a late callback can tell whether the state still belongs to it.
	uint64 generation = 0;
	int total = 0;
	int sent = 0;
	int skipped = 0;
	bool cancelled = false;
	bool paused = false;
	bool finished = false;
	bool resumable = false; // seeded from a persisted unfinished DB row
	bool bulkOwned = false; // cancelled rows are recorded by the bulk run
	PeerId destPeer;
	PeerId srcPeer;
	Fn<void()> cancelCallback;
	Fn<void()> pauseCallback;
	Fn<void()> resumeCallback;
	Fn<void()> saveCallback;
	int currentDownload = -1;
	std::vector<FullMsgId> sourceIds;
	std::vector<TrackedItem> items;
	int currentUpload = -1;
	ItemInfo downloadItem;
	ItemInfo uploadItem;
	float64 downloadProgress = 0;
	float64 uploadProgress = 0;
	qint64 downloadSpeed = 0;
	qint64 uploadSpeed = 0;
};

class EnhancedFileTask final : public Task {
public:
	EnhancedFileTask(
		FileLoadTask::Args &&args,
		Fn<void(std::shared_ptr<FilePrepareResult>)> &&cb)
	: _session(base::make_weak(args.session))
	, _impl(std::make_unique<FileLoadTask>(std::move(args)))
	, _cb(std::move(cb)) {}
	void process() override { _impl->process(); }
	void finish() override {
		if (!_session) {
			return;
		}
		if (_cb) _cb(_impl->peekResult());
	}
private:
	base::weak_ptr<Main::Session> _session;
	std::unique_ptr<FileLoadTask> _impl;
	Fn<void(std::shared_ptr<FilePrepareResult>)> _cb;
};

using StateMap = std::unordered_map<PeerId, SharedState>;
using FinishedStateList = std::vector<SharedState>;

rpl::event_stream<PeerId> StateChanges;
rpl::event_stream<> CounterChanges;

void NotifyCounterChanged() {
	CounterChanges.fire({});
}

void NotifyStateChanged(const PeerId &peer) {
	StateChanges.fire_copy(peer);
}

std::unordered_set<FullMsgId> &EFUploadIds() {
	static auto set = std::unordered_set<FullMsgId>();
	return set;
}

void RemoveEFUploadsForPeer(const PeerId &peer) {
	auto &set = EFUploadIds();
	for (auto it = set.begin(); it != set.end();) {
		if (it->peer == peer) {
			it = set.erase(it);
		} else {
			++it;
		}
	}
}

void EnsureEFUploadIdSync(not_null<Main::Session*> session) {
	static base::flat_set<not_null<Main::Session*>> synced;
	if (!synced.emplace(session).second) {
		return;
	}
	session->lifetime().add([session] {
		synced.remove(session);
	});
	// EF uploads are tracked by their local (negative) message id, but the
	// uploader switches its finished list to the server id once the message
	// is confirmed. Keep EFUploadIds in sync so the transfer manager keeps
	// excluding enhanced-forward uploads from the regular Uploads tab even
	// after the pipeline itself is gone.
	session->data().itemIdChanged() | rpl::on_next([session](
			const Data::Session::IdChange &change) {
		auto &ids = EFUploadIds();
		const auto oldFull = FullMsgId(change.newId.peer, change.oldId);
		if (ids.contains(oldFull)) {
			ids.erase(oldFull);
			ids.emplace(change.newId);
		}
	}, session->lifetime());
}

void SetShadowUpload(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item,
		int64 size) {
	const auto media = item->media();
	const auto doc = media ? media->document() : nullptr;
	if (!doc) {
		return;
	}
	doc->uploadingData = std::make_unique<Data::UploadState>(size);
	session->data().requestItemRepaint(item);
}

void UpdateShadowUpload(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item,
		int64 offset,
		int64 size) {
	const auto media = item->media();
	const auto doc = media ? media->document() : nullptr;
	if (!doc || !doc->uploadingData) {
		return;
	}
	doc->uploadingData->offset = std::clamp(offset, int64(0), size);
	session->data().requestItemRepaint(item);
	session->data().notifyDocumentLayoutChanged(doc);
}

void ClearShadowUpload(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item) {
	const auto media = item->media();
	const auto doc = media ? media->document() : nullptr;
	if (!doc || !doc->uploadingData) {
		return;
	}
	doc->uploadingData = nullptr;
	session->data().requestItemRepaint(item);
}

// The server id a send request created for `randomId`, if the response
// carried it. Text-only items get no local echo, so the response is the only
// place their destination id can be learned from.
[[nodiscard]] MsgId MessageIdFromUpdates(
		const MTPUpdates &updates,
		uint64 randomId) {
	auto result = MsgId(0);
	const auto scan = [&](const MTPVector<MTPUpdate> &list) {
		for (const auto &update : list.v) {
			update.match([&](const MTPDupdateMessageID &data) {
				if (data.vrandom_id().v == randomId) {
					result = MsgId(data.vid().v);
				}
			}, [](const auto &) {});
		}
	};
	updates.match([&](const MTPDupdates &data) {
		scan(data.vupdates());
	}, [&](const MTPDupdatesCombined &data) {
		scan(data.vupdates());
	}, [](const auto &) {});
	return result;
}

// Byte size of the photo variant the content fingerprint is computed from.
// Photos use the same sample plan as documents: the whole file up to
// kDedupMinPartialHashSize and one middle chunk above it.
[[nodiscard]] int64 PhotoFingerprintSize(not_null<PhotoData*> photo) {
	auto size = int64(photo->imageByteSize(Data::PhotoSize::Large));
	if (!size) {
		size = photo->videoByteSize(Data::PhotoSize::Large);
	}
	return size;
}

// Deleting a ForwardTemp file may block for a long time: an antivirus filter
// scans a just-written large file before it allows the delete, and a busy
// volume adds to that. Doing it per item on the main thread froze the UI for
// longer than the deadlock detector tolerates, so removals are queued and
// drained by a small pool of worker threads instead. The pool size follows the
// number of queued files, not the number of transfer stages: one item has one
// temp file, while a live cancel can have one file downloading and another
// uploading at the same time.
//
// Removing a file must not remove the ForwardTemp directory: a running job
// still has items waiting for their download, and deleting the now-empty
// directory makes the next item fail to create its file. The directory is
// removed only by the explicit cleanup that runs after the whole job is over.
class TempFileRemover final {
public:
	void add(const QString &path) {
		if (path.isEmpty()) {
			return;
		}
		add(std::vector<QString>{ path }, QString());
	}

	void add(
		std::vector<QString> paths,
		const QString &removeEmptyDir = QString()) {
		if (paths.empty() && removeEmptyDir.isEmpty()) {
			return;
		}
		auto schedule = false;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (!removeEmptyDir.isEmpty()) {
				_pendingDirs.push_back(removeEmptyDir);
			}
			_pending.insert(
				_pending.end(),
				std::make_move_iterator(paths.begin()),
				std::make_move_iterator(paths.end()));
			schedule = !_scheduled;
			_scheduled = true;
		}
		if (schedule) {
			crl::on_main([this] {
				crl::async([this] { drain(); });
			});
		}
	}

private:
	void drain() {
		auto batch = std::vector<QString>();
		{
			std::lock_guard<std::mutex> lock(_mutex);
			batch = std::exchange(_pending, std::vector<QString>());
		}
		if (batch.empty()) {
			finishDrain();
			return;
		}
		const auto shared = std::make_shared<std::vector<QString>>(
			std::move(batch));
		const auto next = std::make_shared<std::atomic<int>>(0);
		const auto workers = std::min(4, int(shared->size()));
		const auto remaining = std::make_shared<std::atomic<int>>(workers);
		const auto worker = [=] {
			for (auto index = next->fetch_add(1);
				index < int(shared->size());
				index = next->fetch_add(1)) {
				QFile::remove((*shared)[index]);
			}
			if (remaining->fetch_sub(1) == 1) {
				finishDrain();
			}
		};
		for (auto i = 0; i != workers; ++i) {
			crl::async(worker);
		}
	}

	void finishDrain() {
		auto again = false;
		auto dirs = std::vector<QString>();
		{
			std::lock_guard<std::mutex> lock(_mutex);
			again = !_pending.empty();
			if (!again) {
				dirs = std::exchange(_pendingDirs, std::vector<QString>());
			}
			_scheduled = again;
		}
		for (const auto &dir : dirs) {
			QDir(dir).rmdir(dir);
		}
		if (again) {
			crl::async([this] { drain(); });
		}
	}

	std::mutex _mutex;
	std::vector<QString> _pending;
	std::vector<QString> _pendingDirs;
	bool _scheduled = false;
};

TempFileRemover &TempFilesToRemove() {
	static const auto result = new TempFileRemover();
	return *result;
}

} // namespace

void ShowForwardDoneToast(int sent, int, int skipped) {
	if (sent <= 0 && skipped <= 0) {
		return;
	}
	if (sent > 0 && skipped > 0) {
		const auto forwardedText = tr::lng_tm_fw_forwarded(tr::now, lt_count, sent);
		const auto duplicatesText = tr::lng_tm_fw_duplicates_skipped(tr::now, lt_count, skipped);
		Ui::Toast::Show(forwardedText + u", "_q + duplicatesText);
	} else if (sent > 0) {
		Ui::Toast::Show(tr::lng_tm_fw_forwarded(tr::now, lt_count, sent));
	} else {
		Ui::Toast::Show(tr::lng_tm_fw_duplicates_skipped(tr::now, lt_count, skipped));
	}
}

struct ForwardBatch {
	Main::Session *session = nullptr;
	int total = 0;
	int sent = 0;
	int skipped = 0;
	QString lastFileName;
	uint64 actionId = 0;
	int64 startedAt = 0;
	crl::time lastTouchAt = 0;
	bool enhancedDone = false;
	bool normalDone = false;
	bool finished = false;
};

base::flat_map<PeerId, ForwardBatch> &ForwardBatchMap() {
	static auto result = base::flat_map<PeerId, ForwardBatch>();
	return result;
}

ForwardBatch *LookupForwardBatch(
		not_null<Main::Session*> session,
		const PeerId &dst) {
	const auto it = ForwardBatchMap().find(dst);
	if (it == end(ForwardBatchMap()) || it->second.session != session.get()) {
		return nullptr;
	}
	return &it->second;
}

uint64 CurrentForwardActionId(
		not_null<Main::Session*> session,
		const PeerId &dst) {
	const auto batch = LookupForwardBatch(session, dst);
	return batch ? batch->actionId : 0;
}

void SweepStaleForwardBatches() {
	const auto now = crl::now();
	for (auto i = begin(ForwardBatchMap()); i != end(ForwardBatchMap());) {
		const auto dst = i->first;
		auto &batch = i->second;
		if (!batch.finished
			&& batch.session
			&& (now - batch.lastTouchAt > crl::time(60000))
			&& !EfWorkPendingFor(not_null{ batch.session }, dst)
			&& !NormalForward::NfWorkPendingFor(
				not_null{ batch.session },
				dst)) {
			i = ForwardBatchMap().erase(i);
			NotifyCounterChanged();
		} else {
			++i;
		}
	}
}

void BeginForwardBatch(
		not_null<Main::Session*> session,
		const PeerId &dst,
		int total) {
	if (total <= 0) {
		return;
	}
	SweepStaleForwardBatches();
	auto &batch = ForwardBatchMap()[dst];
	if (batch.session != session.get() || batch.finished) {
		batch = ForwardBatch();
		batch.session = session.get();
		batch.startedAt = base::unixtime::now();
		batch.actionId = base::RandomValue<uint64>();
		if (!batch.actionId) {
			batch.actionId = 1;
		}
	}
	batch.total += total;
	batch.lastTouchAt = crl::now();
	Info::Downloads::SetLastActivityTab(Info::Downloads::Tab::Forwards);
	NotifyCounterChanged();
}

void ReportForwardBatchItem(
		not_null<Main::Session*> session,
		const PeerId &dst,
		int sentDelta,
		int skippedDelta) {
	const auto batch = LookupForwardBatch(session, dst);
	if (!batch || batch->finished) {
		return;
	}
	batch->sent += sentDelta;
	batch->skipped += skippedDelta;
	batch->lastTouchAt = crl::now();
	NotifyCounterChanged();
}

// Enhanced forward stores its temp file as "<runId>_<index>_<name>", so
// equal names from different messages do not collide on disk. The bar names
// the item by its original name only. A file that was already downloaded
// keeps its real name, because it does not live in the temp folder.
[[nodiscard]] QString ItemDisplayName(
		const QString &name,
		const QString &path,
		const QString &downloadPath) {
	if (!path.startsWith(downloadPath)) {
		return name;
	}
	const auto digitsEnd = [&](int from) {
		auto result = from;
		while (result < name.size() && name[result].isDigit()) {
			++result;
		}
		return result;
	};
	const auto first = digitsEnd(0);
	if (first == 0 || first >= name.size() || name[first] != u'_') {
		return name;
	}
	const auto second = digitsEnd(first + 1);
	if (second == first + 1 || second >= name.size() || name[second] != u'_') {
		return name;
	}
	return name.mid(second + 1);
}

QString ForwardBatchFirstName(
		const std::vector<not_null<HistoryItem*>> &items) {
	if (items.empty()) {
		return QString();
	}
	const auto item = items.front();
	if (const auto media = item->media()) {
		if (const auto doc = media->document()) {
			return doc->filename();
		} else if (media->photo()) {
			return u"photo_%1.jpg"_q.arg(item->fullId().msg.bare);
		}
	}
	return QString();
}

void NoteForwardBatchFile(
		not_null<Main::Session*> session,
		const PeerId &dst,
		const QString &name) {
	if (name.isEmpty()) {
		return;
	}
	const auto batch = LookupForwardBatch(session, dst);
	if (!batch) {
		return;
	}
	batch->lastFileName = name;
	NotifyCounterChanged();
}

bool ForwardBatchOwns(
		not_null<Main::Session*> session,
		const PeerId &dst) {
	return LookupForwardBatch(session, dst) != nullptr;
}

void FinishForwardBatchWorker(
		not_null<Main::Session*> session,
		const PeerId &dst,
		int worker) {
	const auto batch = LookupForwardBatch(session, dst);
	if (!batch || batch->finished) {
		return;
	}
	if (worker == 0) {
		batch->enhancedDone = true;
	} else {
		batch->normalDone = true;
	}
	batch->lastTouchAt = crl::now();
	const auto otherPending = (worker == 0)
		? NormalForward::NfWorkPendingFor(session, dst)
		: EfWorkPendingFor(session, dst);
	if ((!batch->enhancedDone || !batch->normalDone) && otherPending) {
		NotifyCounterChanged();
		SweepStaleForwardBatches();
		return;
	}
	batch->finished = true;
	const auto sent = batch->sent;
	const auto total = batch->total;
	const auto skipped = batch->skipped;
	NotifyCounterChanged();
	ShowForwardDoneToast(sent, total, skipped);
	if (sent <= 0) {
		// Nothing was transferred, so there is no progress left to show.
		// Dropping the record at once keeps the bottom-left bar from sitting
		// there for the usual five seconds after an all-duplicates batch.
		if (const auto it = ForwardBatchMap().find(dst);
			it != end(ForwardBatchMap())
			&& it->second.session == session.get()) {
			ForwardBatchMap().erase(it);
		}
		NotifyCounterChanged();
		return;
	}
	// The record stays briefly so the bar and the counters show the final
	// count, then it is dropped. Kept rows for the transfers list live in
	// the finished-runs registry instead.
	base::call_delayed(crl::time(5000), [dst] {
		const auto it = ForwardBatchMap().find(dst);
		if (it == end(ForwardBatchMap()) || !it->second.finished) {
			return;
		}
		ForwardBatchMap().erase(it);
		NotifyCounterChanged();
	});
}

void DropForwardBatch(
		not_null<Main::Session*> session,
		const PeerId &dst) {
	const auto it = ForwardBatchMap().find(dst);
	if (it == end(ForwardBatchMap()) || it->second.session != session.get()) {
		return;
	}
	ForwardBatchMap().erase(it);
	NotifyCounterChanged();
}

// Starts the shared counter unless a live one already tracks this
// destination. Returns true when a fresh record was started.
bool EnsureForwardBatch(
		not_null<Main::Session*> session,
		const PeerId &dst,
		int total) {
	if (total <= 0) {
		return false;
	}
	if (ForwardBatchOwns(session, dst)) {
		for (const auto &batch : ForwardBatches(session)) {
			if (batch.dst == dst && !batch.finished) {
				return false;
			}
		}
	}
	BeginForwardBatch(session, dst, total);
	return true;
}

std::vector<ForwardBatchSnapshot> ForwardBatches(
		not_null<Main::Session*> session) {
	auto result = std::vector<ForwardBatchSnapshot>();
	for (const auto &[dst, batch] : ForwardBatchMap()) {
		if (batch.session != session.get()) {
			continue;
		}
	result.push_back({
		.dst = dst,
		.total = batch.total,
		.sent = batch.sent,
		.skipped = batch.skipped,
		.lastFileName = batch.lastFileName,
		.startedAt = batch.startedAt,
		.finished = batch.finished,
	});
	}
	return result;
}

base::flat_map<std::tuple<Main::Session*, PeerId, uint64>, FinishedFwRun> &
FinishedFwRunsMap() {
	static auto result = base::flat_map<
		std::tuple<Main::Session*, PeerId, uint64>,
		FinishedFwRun>();
	return result;
}

Fn<std::optional<QByteArray>()> FinishedFwRunsSerializer(
		not_null<Main::Session*> session) {
	return [session]() -> std::optional<QByteArray> {
		auto result = QByteArray();
		auto stream = QDataStream(&result, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		auto count = qint32(0);
		for (const auto &[key, run] : FinishedFwRunsMap()) {
			if (std::get<0>(key) == session.get()) {
				++count;
			}
		}
		stream << qint32(6) << count;
		for (const auto &[key, run] : FinishedFwRunsMap()) {
			if (std::get<0>(key) != session.get()) {
				continue;
			}
			stream << quint64(run.dst.value)
				<< quint64(run.src.value)
				<< quint64(run.firstSource.peer.value)
				<< qint64(run.firstSource.msg.bare)
				<< qint32(run.total)
				<< qint32(run.done)
				<< qint32(run.skipped)
				<< qint64(run.finishedAt)
				<< quint64(run.runId)
				<< quint64(run.actionId)
				<< quint64(run.lastSource.peer.value)
				<< qint64(run.lastSource.msg.bare)
				<< quint8(run.cancelled ? 1 : 0);
		}
		return result;
	};
}

void WriteFinishedFwRuns(not_null<Main::Session*> session) {
	session->account().local().updateFinishedFwRuns(
		FinishedFwRunsSerializer(session));
}

void EnsureFinishedFwRunsSeeded(not_null<Main::Session*> session) {
	static auto seeded = base::flat_set<not_null<Main::Session*>>();
	if (!seeded.emplace(session).second) {
		return;
	}
	session->lifetime().add([session] {
		seeded.remove(session);
	});
	const auto blob = session->account().local().finishedFwRunsSerialized();
	if (blob.isEmpty()) {
		return;
	}
	auto stream = QDataStream(blob);
	stream.setVersion(QDataStream::Qt_5_1);
	auto version = qint32(0);
	auto count = qint32(0);
	stream >> version >> count;
	if (stream.status() != QDataStream::Ok
		|| (version < 1 || version > 6)
		|| count < 0
		|| count > blob.size() / 32) {
		return;
	}
	for (auto i = 0; i != count; ++i) {
		auto dst = quint64(0);
		auto src = quint64(0);
		auto msgPeer = quint64(0);
		auto msgId = qint64(0);
		auto total = qint32(0);
		auto done = qint32(0);
		auto skipped = qint32(0);
		auto finishedAt = qint64(0);
		auto runId = quint64(0);
		auto actionId = quint64(0);
		auto lastPeer = quint64(0);
		auto lastId = qint64(0);
		stream >> dst >> src >> msgPeer >> msgId
			>> total >> done >> skipped >> finishedAt;
		if (version == 2) {
			stream >> runId;
		} else if (version >= 3) {
			stream >> runId >> actionId;
		}
		if (version >= 5) {
			stream >> lastPeer >> lastId;
		}
		auto cancelled = quint8(0);
		if (version >= 6) {
			stream >> cancelled;
		}
		if (stream.status() != QDataStream::Ok || !dst) {
			break;
		}
		auto run = FinishedFwRun();
		run.dst = PeerId(dst);
		run.src = PeerId(src);
		run.firstSource = FullMsgId(PeerId(msgPeer), MsgId(msgId));
		run.lastSource = FullMsgId(PeerId(lastPeer), MsgId(lastId));
		run.total = total;
		run.done = done;
		run.skipped = skipped;
		run.finishedAt = finishedAt;
		run.cancelled = (cancelled != 0);
		run.runId = runId ? runId : base::RandomValue<uint64>();
		run.actionId = (version >= 4 && actionId) ? actionId : run.runId;
		FinishedFwRunsMap().emplace(
			std::make_tuple(session.get(), run.dst, run.runId),
			std::move(run));
	}
}

void RecordFinishedFwRun(
		not_null<Main::Session*> session,
		const PeerId &dst,
		const PeerId &src,
		FullMsgId firstSource,
		FullMsgId lastSource,
		int total,
		int done,
		int skipped,
		uint64 actionId,
		bool cancelled) {
	auto run = FinishedFwRun();
	run.runId = base::RandomValue<uint64>();
	if (!run.runId) {
		run.runId = 1;
	}
	run.actionId = actionId ? actionId : run.runId;
	run.dst = dst;
	run.src = src;
	run.firstSource = firstSource;
	run.lastSource = lastSource;
	run.total = total;
	run.done = done;
	run.skipped = skipped;
	run.finishedAt = base::unixtime::now();
	run.cancelled = cancelled;
	FinishedFwRunsMap().emplace(
		std::make_tuple(session.get(), dst, run.runId),
		std::move(run));
	WriteFinishedFwRuns(session);
	Info::Downloads::SetLastActivityTab(Info::Downloads::Tab::Forwards);
}

void DropFinishedFwRunGroup(
		not_null<Main::Session*> session,
		uint64 actionId) {
	auto &map = FinishedFwRunsMap();
	for (auto i = map.begin(); i != end(map);) {
		if (std::get<0>(i->first) == session.get()
			&& i->second.actionId == actionId) {
			i = map.erase(i);
		} else {
			++i;
		}
	}
	WriteFinishedFwRuns(session);
}

void ClearFinishedFwRuns(not_null<Main::Session*> session) {
	auto &map = FinishedFwRunsMap();
	for (auto i = map.begin(); i != map.end();) {
		if (std::get<0>(i->first) == session.get()) {
			i = map.erase(i);
		} else {
			++i;
		}
	}
	WriteFinishedFwRuns(session);
}

std::vector<FinishedFwRun> FinishedFwRuns(
		not_null<Main::Session*> session) {
	auto result = std::vector<FinishedFwRun>();
	for (const auto &[key, run] : FinishedFwRunsMap()) {
		if (std::get<0>(key) == session.get()) {
			result.push_back(run);
		}
	}
	return result;
}

constexpr auto kOrderConfirmGraceMs = crl::time(800);
// Upper bound for waiting on server ids of a settled copy batch. The id
// normally arrives within a second; this only exists so a lost confirmation
// cannot keep the pipeline (and its report) alive forever.
constexpr auto kSettleWaitMax = crl::time(60000);

// A re-uploaded file only gets its id once the server processes the send,
// which is a moment after the client finished uploading it. The order gate of
// the standard half may not move past such an item before that, or the items
// released behind it are created first and land before it. So each sent item
// without a server id registers the value the gate should take once the id
// appears, with a delayed fallback in case the confirmation never arrives.
struct OrderWait {
	Main::Session *session = nullptr;
	ApiWrap *api = nullptr;
	PeerId peer;
	MsgId next = MsgId();
	bool fallback = false;
};

base::flat_map<FullMsgId, OrderWait> &OrderWaits() {
	static auto result = base::flat_map<FullMsgId, OrderWait>();
	return result;
}

void ApplyOrderWait(const FullMsgId &local) {
	const auto it = OrderWaits().find(local);
	if (it == end(OrderWaits())) {
		return;
	}
	const auto wait = it->second;
	OrderWaits().erase(it);
	NormalForward::AdvanceGate(wait.api, wait.peer, wait.next);
}

void RegisterOrderWait(
		not_null<Main::Session*> session,
		not_null<ApiWrap*> api,
		const PeerId &peer,
		const FullMsgId &local,
		MsgId next) {
	auto &wait = OrderWaits()[local];
	wait.session = session.get();
	wait.api = api;
	wait.peer = peer;
	wait.next = next;
	if (wait.fallback) {
		return;
	}
	wait.fallback = true;
	base::call_delayed(kOrderConfirmGraceMs, [local] {
		ApplyOrderWait(local);
	});
}

void EnsureOrderGateWatch(not_null<Main::Session*> session) {
	static base::flat_set<not_null<Main::Session*>> tracked;
	if (!tracked.emplace(session).second) {
		return;
	}
	session->lifetime().add([session] {
		tracked.remove(session);
		auto &waits = OrderWaits();
		for (auto it = begin(waits); it != end(waits);) {
			if (it->second.session == session.get()) {
				it = waits.erase(it);
			} else {
				++it;
			}
		}
	});
	session->data().itemIdChanged(
	) | rpl::on_next([](const Data::Session::IdChange &change) {
		ApplyOrderWait(FullMsgId(change.newId.peer, change.oldId));
	}, session->lifetime());
}

[[nodiscard]] std::shared_ptr<Pipeline> LivePipeline(const PeerId &peerId) {
	const auto &active = Pipeline::Active();
	const auto it = active.find(peerId);
	return (it != end(active)) ? it->second.lock() : nullptr;
}

// Settled means the item cannot hold the order gate any more: it was skipped,
// or its message already exists on the server.
[[nodiscard]] bool OrderSettled(const ItemTask &item) {
	return item.cancelled
		|| item.dedupSkipped
		|| (item.textOnly && item.confirmed)
		|| (item.sentItem && IsServerMsgId(item.sentItem->id));
}

StateMap &ActiveStates() {
	static StateMap map;
	return map;
}

// Finished/resumable batches accumulate: re-forwarding to the same chat adds
// a new batch instead of replacing the previous one, so the Forwards tab keeps
// every sent item until the user clears it.
FinishedStateList &FinishedStates() {
	static FinishedStateList list;
	return list;
}

const SharedState *FindFinishedBatch(const PeerId &peer) {
	for (const auto &state : FinishedStates()) {
		if (state.destPeer == peer) {
			return &state;
		}
	}
	return nullptr;
}

bool HasFinishedForPeer(const PeerId &peer) {
	return FindFinishedBatch(peer) != nullptr;
}

void EraseResumableBatchesForPeer(const PeerId &peer) {
	auto &finished = FinishedStates();
	for (auto i = finished.begin(); i != finished.end();) {
		if (i->destPeer == peer && i->resumable) {
			i = finished.erase(i);
		} else {
			++i;
		}
	}
}

void processStartQueue(const PeerId &peerId);

Fn<std::optional<QByteArray>()> ForwardedDoneSerializer();
void WriteForwardedDonePostponed();
[[nodiscard]] std::vector<FullMsgId> DeserializeForwardedDone(
	const QByteArray &blob);

[[nodiscard]] FullMsgId LastSourceId(const std::vector<FullMsgId> &ids) {
	auto result = FullMsgId();
	for (const auto &id : ids) {
		if (!result || id.msg > result.msg) {
			result = id;
		}
	}
	return result;
}

void finishJob(not_null<Main::Session*> session, const PeerId &peerId) {
	// The gate may only move once the re-uploaded files have their server ids.
	if (const auto pipeline = LivePipeline(peerId)) {
		pipeline->advanceOrderGate();
	} else {
		NormalForward::AdvanceGate(&session->api(), peerId, MsgId(0));
	}
	auto &states = ActiveStates();
	const auto it = states.find(peerId);
	if (it == states.end()) return;
	auto &state = it->second;
	state.finished = true;
	for (auto i = state.items.rbegin(); i != state.items.rend(); ++i) {
		if ((i->sent || i->dedupSkipped) && !i->cancelled && !i->info.name.isEmpty()) {
			NoteForwardBatchFile(session, peerId, i->info.name);
			break;
		}
	}
	// A completed batch must leave no resume rows: they are only meant to
	// survive a pause or a crash, and GetUnfinishedJobs counts every row for
	// the session, so leftovers would keep inflating the FW counter.
	const auto anyCancelled = ranges::any_of(
		state.items,
		[](const TrackedItem &tracked) { return tracked.cancelled; });
	if (!anyCancelled) {
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			db.clearEfResumeJob(u"ef_%1_%2"_q.arg(
				state.srcPeer.value).arg(peerId.value));
		}
	}
	// Release the pipeline references so finished jobs don't keep the
	// whole Pipeline (and its per-item tasks) alive in FinishedStates.
	state.cancelCallback = nullptr;
	state.pauseCallback = nullptr;
	state.resumeCallback = nullptr;
	state.saveCallback = nullptr;
	const auto allDead = ranges::all_of(
		state.items,
		[](const TrackedItem &tracked) {
			return tracked.cancelled || tracked.dedupSkipped;
		});
	if (allDead) {
		if (!state.cancelled && state.skipped > 0) {
			auto &db = Core::App().downloadManager().ensureDedupDb();
			if (db.isOpen()) {
				RecordFinishedFwRun(
					session,
					peerId,
					state.srcPeer,
					state.sourceIds.empty()
						? FullMsgId()
						: state.sourceIds.front(),
					LastSourceId(state.sourceIds),
					state.total,
					state.sent,
					state.skipped,
					CurrentForwardActionId(session, peerId));
			}
		}
		states.erase(it);		
		CleanupPartialFilesForPeer(session, peerId);
		NotifyStateChanged(peerId);
		NotifyCounterChanged();
		session->changes().peerUpdated(
			session->data().peer(peerId),
			Data::PeerUpdate::Flag::Slowmode);
		FinishForwardBatchWorker(session, peerId, 0);
		processStartQueue(peerId);
		return;
	}
	auto &finished = FinishedStates();
	auto &batch = it->second;
	finished.push_back(std::move(batch));
	states.erase(it);
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		const auto &done = finished.back();
		if (!done.cancelled || !done.bulkOwned) {
			RecordFinishedFwRun(
				session,
				peerId,
				done.srcPeer,
				done.sourceIds.empty()
					? FullMsgId()
					: done.sourceIds.front(),
				LastSourceId(done.sourceIds),
				done.total,
				done.sent,
				done.skipped,
				CurrentForwardActionId(session, peerId),
				done.cancelled);
		}
	}
	WriteForwardedDonePostponed();
	NotifyStateChanged(peerId);
	NotifyCounterChanged();
	session->changes().peerUpdated(
		session->data().peer(peerId),
		Data::PeerUpdate::Flag::Slowmode);
	FinishForwardBatchWorker(session, peerId, 0);
	processStartQueue(peerId);
}

struct StartRequest {
	not_null<ApiWrap*> api;
	std::vector<not_null<HistoryItem*>> items;
	Api::SendAction action;
	Data::ForwardOptions forwardOptions;
	Data::GroupingOptions groupOptions;
	std::shared_ptr<SavedJob> resumeJob;
	std::shared_ptr<Data::ForwardRunSeen> runSeen;
	SettledCallback settled;
	bool destinationOnlyDedup = false;
	ItemSettledCallback itemSettled;
	bool restoreProgress = false;
	bool bulkOwned = false;
};

using StartQueueMap = std::unordered_map<PeerId, std::deque<StartRequest>>;

StartQueueMap &StartQueue() {
	static StartQueueMap map;
	return map;
}

void processStartQueue(const PeerId &peerId) {
	auto &queue = StartQueue();
	const auto it = queue.find(peerId);
	if (it == queue.end() || it->second.empty()) {
		return;
	}
	if (ActiveStates().find(peerId) != ActiveStates().end()) {
		return;
	}
	auto request = std::move(it->second.front());
	it->second.pop_front();
	if (it->second.empty()) {
		queue.erase(it);
	}
	Pipeline::Start(
		request.api,
		std::move(request.items),
		request.action,
		request.forwardOptions,
		request.groupOptions,
		std::move(request.resumeJob),
		std::move(request.runSeen),
		std::move(request.settled),
		request.destinationOnlyDedup,
		std::move(request.itemSettled),
		request.restoreProgress,
		request.bulkOwned);
}

void fireUpdate(not_null<Main::Session*> session, const PeerId &peer) {
	session->changes().peerUpdated(
		session->data().peer(peer),
		Data::PeerUpdate::Flag::Slowmode);
	NotifyStateChanged(peer);
}

base::flat_set<not_null<PeerData*>> &FlagProtectedPeers() {
	static base::flat_set<not_null<PeerData*>> result;
	return result;
}

base::flat_set<not_null<PeerData*>> &ErrorProtectedPeers() {
	static base::flat_set<not_null<PeerData*>> result;
	return result;
}

void NotePeerNoforwardsFlag(not_null<PeerData*> peer, bool value) {
	if (value) {
		FlagProtectedPeers().insert(peer);
	} else {
		FlagProtectedPeers().erase(peer);
	}
}

void MarkPeerProtectedByError(not_null<PeerData*> peer) {
	ErrorProtectedPeers().insert(peer);
}

bool PeerNeedsEnhancedForward(not_null<PeerData*> peer) {
	return FlagProtectedPeers().contains(peer)
		|| ErrorProtectedPeers().contains(peer);
}

Split classifyItems(const std::vector<not_null<HistoryItem*>> &items) {
	auto result = Split();
	for (const auto &item : items) {
		if (PeerNeedsEnhancedForward(item->history()->peer)
			|| (item->flags() & MessageFlag::NoForwards)) {
			result.restricted.push_back(item);
			result.gate = result.gate
				? std::min(result.gate, item->id)
				: item->id;
		} else {
			result.normal.push_back(item);
		}
	}
	return result;
}

void startForwardSession(
		not_null<Main::Session*> session,
		const PeerId &peerId,
		const PeerId &srcPeer,
		const std::vector<FullMsgId> &sourceIds,
		Fn<void()> saveCallback) {
	auto &states = ActiveStates();
	states.erase(peerId);
	// Keep finished batches in the tab; only stale resumable leftovers of
	// this destination are dropped (they are being resumed right now).
	EraseResumableBatchesForPeer(peerId);

	auto &state = states[peerId];
	static auto generationCounter = uint64(0);
	state.generation = ++generationCounter;
	state.total = int(sourceIds.size());
	state.sent = 0;
	state.skipped = 0;
	state.cancelled = false;
	state.paused = false;
	state.finished = false;
	state.bulkOwned = false;
	state.destPeer = peerId;
	state.srcPeer = srcPeer;
	state.sourceIds = sourceIds;
	state.items.resize(state.total);
	state.saveCallback = std::move(saveCallback);

	fireUpdate(session, peerId);
	NotifyCounterChanged();
}

void AdvanceOrderGateFor(const PeerId &peerId) {
	if (const auto pipeline = LivePipeline(peerId)) {
		pipeline->advanceOrderGate();
	}
}

void markItemSent(
		not_null<Main::Session*> session,
		const PeerId &peerId) {
	auto &states = ActiveStates();
	const auto it = states.find(peerId);
	if (it == states.end()) return;

	auto &state = it->second;
	if (state.cancelled || state.finished) return;

	state.sent++;
	ReportForwardBatchItem(session, peerId, 1, 0);
	AdvanceOrderGateFor(peerId);
	fireUpdate(session, peerId);
	NotifyCounterChanged();
	if (state.saveCallback) {
		state.saveCallback();
	}

	if (state.sent + state.skipped >= state.total) {
		finishJob(session, peerId);
	}
}

void markItemSkipped(
		not_null<Main::Session*> session,
		const PeerId &peerId) {
	auto &states = ActiveStates();
	const auto it = states.find(peerId);
	if (it == states.end()) return;

	auto &state = it->second;
	if (state.cancelled || state.finished) return;

	// The bar reports the duplicates on their own line and removes them
	// from the displayed total, so `total` stays the gross batch size here.
	state.skipped++;
	ReportForwardBatchItem(session, peerId, 0, 1);
	AdvanceOrderGateFor(peerId);
	fireUpdate(session, peerId);
	NotifyCounterChanged();
	if (state.saveCallback) {
		state.saveCallback();
	}

	if (state.sent + state.skipped >= state.total) {
		finishJob(session, peerId);
	}
}

void cancelForward(
		const PeerId &id,
		not_null<Main::Session*> session,
		bool bulkOwned) {
	if (const auto queued = StartQueue().find(id); queued != StartQueue().end()) {
		StartQueue().erase(queued);
		NotifyStateChanged(id);
	}
	DropForwardBatch(session, id);
	auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) {
		auto &finished = FinishedStates();
		auto handled = false;
		for (auto i = finished.begin(); i != finished.end();) {
			if (!i->resumable || i->destPeer != id) {
				++i;
				continue;
			}
			handled = true;
			if (i->sent > 0 && !bulkOwned) {
				RecordFinishedFwRun(
					session,
					id,
					i->srcPeer,
					i->sourceIds.empty()
						? FullMsgId()
						: i->sourceIds.front(),
					LastSourceId(i->sourceIds),
					i->total,
					i->sent,
					i->skipped,
					0,
					true);
			}
			i = finished.erase(i);
		}
		if (!handled) {
			return;
		}
		CleanupPartialFilesForPeer(session, id);
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			db.clearFwTmpRun(session->uniqueId(), id);
		}
		NotifyStateChanged(id);
		NotifyCounterChanged();
		return;
	}
	auto &state = it->second;
	if (state.cancelled || state.finished) return;

	if (!LivePipeline(id) && !state.cancelCallback) {
		// Restored from disk after a restart: no pipeline owns the files or
		// the item states, so the whole batch is dropped instead of being
		// kept as a finished history. The persisted rows are the only record
		// of its temp files, so they are removed here while the rows still
		// exist.
		const auto ownedByBulk = state.bulkOwned || bulkOwned;
		if (state.sent > 0 && !ownedByBulk) {
			RecordFinishedFwRun(
				session,
				id,
				state.srcPeer,
				state.sourceIds.empty()
					? FullMsgId()
					: state.sourceIds.front(),
				LastSourceId(state.sourceIds),
				state.total,
				state.sent,
				state.skipped,
				0,
				true);
		}
		states.erase(it);
		CleanupPartialFilesForPeer(session, id);
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			db.clearFwTmpRun(session->uniqueId(), id);
		}
		fireUpdate(session, id);
		NotifyCounterChanged();
		return;
	}

	if (state.sent == 0) {
		state.cancelled = true;
		if (state.cancelCallback) {
			state.cancelCallback();
		}
		finishJob(session, id);
		return;
	}
	// Partial cancel: items that were already forwarded stay visible as a
	// finished batch; only the remaining items are cancelled and dropped.
	// Cancelling the last remaining item settles its report synchronously,
	// which can finish and erase this state, so no reference to it may
	// outlive the loop.
	const auto pipeline = Pipeline::Active().find(id);
	const auto live = (pipeline != Pipeline::Active().end())
		? pipeline->second.lock()
		: nullptr;
	auto indexes = std::vector<int>();
	indexes.reserve(state.items.size());
	for (auto i = 0; i != int(state.items.size()); ++i) {
		if (!state.items[i].sent && !state.items[i].cancelled) {
			indexes.push_back(i);
		}
	}
	state.cancelled = true;
	if (live) {
		for (const auto i : indexes) {
			live->cancelItem(i);
		}
	} else if (state.cancelCallback) {
		state.cancelCallback();
	}
	auto &statesNow = ActiveStates();
	const auto now = statesNow.find(id);
	if (now == statesNow.end()) {
		return;
	}
	auto &stateNow = now->second;
	auto sent = 0;
	for (auto &item : stateNow.items) {
		if (item.cancelled) {
			continue;
		} else if (item.sent || item.state == ItemState::Done) {
			sent++;
		} else {
			item.cancelled = true;
		}
	}
	stateNow.sent = sent;
	{
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			db.clearEfResumeJob(u"ef_%1_%2"_q.arg(
				stateNow.srcPeer.value).arg(id.value));
		}
	}
	finishJob(session, id);
}

void setCancelCallback(
		const PeerId &id,
		not_null<Main::Session*> session,
		Fn<void()> callback) {
	auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return;

	it->second.cancelCallback = std::move(callback);
}

void setPauseCallback(
		const PeerId &id,
		not_null<Main::Session*> session,
		Fn<void()> callback) {
	auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return;

	it->second.pauseCallback = std::move(callback);
}

void setResumeCallback(
		const PeerId &id,
		not_null<Main::Session*> session,
		Fn<void()> callback) {
	auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return;

	it->second.resumeCallback = std::move(callback);
}

void pauseForward(
		const PeerId &id,
		not_null<Main::Session*> session) {
	DropForwardBatch(session, id);
	auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return;

	it->second.paused = true;
	if (it->second.pauseCallback) {
		it->second.pauseCallback();
	}
	if (it->second.saveCallback) {
		it->second.saveCallback();
	}
	fireUpdate(session, id);
}

void resumeForward(
		const PeerId &id,
		not_null<Main::Session*> session) {
	auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return;

	it->second.paused = false;
	if (it->second.resumeCallback) {
		it->second.resumeCallback();
	}
	if (it->second.saveCallback) {
		it->second.saveCallback();
	}
	fireUpdate(session, id);
}

void cancelCurrentItem(
		const PeerId &id,
		not_null<Main::Session*> session) {
	auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return;

	auto &state = it->second;
	state.currentDownload = -1;
	state.currentUpload = -1;
	state.downloadProgress = 0;
	state.uploadProgress = 0;
	fireUpdate(session, id);
}

void updateDownloadProgress(
		not_null<Main::Session*> session,
		const PeerId &peerId,
		int itemIndex,
		const ItemInfo &info,
		float64 progress) {
	auto &states = ActiveStates();
	const auto it = states.find(peerId);
	if (it == states.end()) return;

	auto &state = it->second;
	if (progress >= 1.0) {
		state.currentDownload = -1;
		state.downloadItem = ItemInfo();
		state.downloadProgress = 0;
	} else {
		state.currentDownload = itemIndex;
		state.downloadItem = info;
		state.downloadProgress = progress;
	}
	if (itemIndex >= 0 && itemIndex < int(state.items.size())) {
		state.items[itemIndex].state = (progress >= 1.0)
			? ItemState::Uploading
			: ItemState::Downloading;
		state.items[itemIndex].info = info;
		state.items[itemIndex].downloadProgress = progress;
		state.items[itemIndex].progress = progress;
	}
	fireUpdate(session, peerId);
}

void updateUploadProgress(
		not_null<Main::Session*> session,
		const PeerId &peerId,
		int itemIndex,
		const ItemInfo &info,
		float64 progress) {
	auto &states = ActiveStates();
	const auto it = states.find(peerId);
	if (it == states.end()) return;

	auto &state = it->second;
	if (progress >= 1.0) {
		state.currentUpload = -1;
		state.uploadItem = ItemInfo();
		state.uploadProgress = 0;
	} else {
		state.currentUpload = itemIndex;
		state.uploadItem = info;
		state.uploadProgress = progress;
	}
	if (itemIndex >= 0 && itemIndex < int(state.items.size())) {
		state.items[itemIndex].state = (progress >= 1.0)
			? ItemState::Done
			: ItemState::Uploading;
		state.items[itemIndex].info = info;
		state.items[itemIndex].uploadProgress = progress;
		state.items[itemIndex].progress = progress;
	}
	fireUpdate(session, peerId);
}

void updateItemState(
		not_null<Main::Session*> session,
		const PeerId &peerId,
		int itemIndex,
		ItemState newState,
		const ItemInfo &info,
		float64 progress) {
	auto &states = ActiveStates();
	const auto it = states.find(peerId);
	if (it == states.end()) return;

	auto &state = it->second;
	if (itemIndex >= 0 && itemIndex < int(state.items.size())) {
		auto &tracked = state.items[itemIndex];
		tracked.state = newState;
		// A settled item reports no name of its own: keep the display name
		// the download or upload phase already published, so the bar and the
		// batch record never fall back to the temporary file path.
		if (!info.name.isEmpty()) {
			tracked.info.name = info.name;
		}
		if (info.size > 0) {
			tracked.info.size = info.size;
		}
		tracked.progress = progress;
		if (newState == ItemState::Done || newState == ItemState::Failed) {
			tracked.downloadProgress = 1.0;
			tracked.uploadProgress = 1.0;
		}
	}
	fireUpdate(session, peerId);
}

bool isForwarding(const PeerId &id) {
	const auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return false;
	return !it->second.cancelled && !it->second.finished;
}

std::optional<PeerId> activeJobPeer() {
	const auto &states = ActiveStates();
	for (const auto &[peer, state] : states) {
		if (!state.cancelled && !state.finished) {
			return peer;
		}
	}
	return std::nullopt;
}

void saveProgressForPeer(
		const PeerId &peer,
		not_null<Main::Session*> session) {
	const auto &states = ActiveStates();
	const auto it = states.find(peer);
	if (it == states.end()) return;
	const auto &state = it->second;
	if (state.saveCallback) {
		state.saveCallback();
	}
}

bool isPaused(const PeerId &id) {
	const auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return false;
	return it->second.paused && !it->second.cancelled && !it->second.finished;
}

bool isCancelled(const PeerId &id) {
	const auto &states = ActiveStates();
	const auto it = states.find(id);
	return (it != states.end()) && it->second.cancelled;
}

ForwardProgress currentProgress(const PeerId &id) {
	ForwardProgress result;
	const auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) {
		result.state = State::Idle;
		return result;
	}
	const auto &state = it->second;
	result.sent = state.sent;
	result.total = state.total;
	result.skipped = state.skipped;
	result.destPeer = state.destPeer;
	result.currentDownload = state.currentDownload;
	result.currentUpload = state.currentUpload;
	result.downloadItem = state.downloadItem;
	result.uploadItem = state.uploadItem;
	result.downloadProgress = state.downloadProgress;
	result.uploadProgress = state.uploadProgress;
	result.downloadSpeed = state.downloadSpeed;
	result.uploadSpeed = state.uploadSpeed;
	result.sourceIds = state.sourceIds;
	result.items = state.items;

	if (state.cancelled) {
		result.state = State::Cancelled;
	} else if (state.finished) {
		result.state = State::Finished;
	} else if (state.paused) {
		result.state = State::Paused;
	} else {
		result.state = State::Sending;
	}

	return result;
}

void ClearProgressForPeer(const PeerId &peerId) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		db.clearEfResumeForPeer(peerId);
	}
}

void CleanupPartialFilesForPeer(
		not_null<Main::Session*> session,
		const PeerId &peerId) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	const auto tempDir = File::DefaultDownloadPath(session) + "ForwardTemp/";
	const auto records = db.loadEfResumeItemsForPeer(
		session->uniqueId(),
		peerId);
	auto paths = std::vector<QString>();
	for (const auto &record : records) {
		if (!record.localPath.isEmpty()
				&& record.localPath.startsWith(tempDir)) {
			paths.push_back(record.localPath);
		}
	}
	db.clearEfResumeForPeer(peerId);
	TempFilesToRemove().add(std::move(paths));
}

void CleanupLeftoverForwardFiles(
		not_null<Main::Session*> session,
		const QString &tempDir,
		bool removeEmptyDir = false) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	auto keep = base::flat_set<QString>();
	if (db.isOpen()) {
		for (const auto &record : db.loadUnfinishedEfResumeItems(session->uniqueId())) {
			if (!record.localPath.isEmpty()) {
				keep.emplace(record.localPath);
			}
		}
	}
	QDir dir(tempDir);
	if (!dir.exists()) {
		return;
	}
	const auto entries = dir.entryList(QDir::Files | QDir::NoDotAndDotDot);
	auto paths = std::vector<QString>();
	for (const auto &entry : entries) {
		auto path = dir.absoluteFilePath(entry);
		if (!keep.contains(path)) {
			paths.push_back(std::move(path));
		}
	}
	TempFilesToRemove().add(
		std::move(paths),
		removeEmptyDir ? dir.absolutePath() : QString());
}

std::vector<SavedJob> GetUnfinishedJobs(
		not_null<Main::Session*> session) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return {};
	}
	const auto records = db.loadUnfinishedEfResumeItems(session->uniqueId());
	base::flat_map<QString, SavedJob> jobs;
	for (const auto &record : records) {
		auto &job = jobs[record.jobId];
		if (job.dstId == PeerId()) {
			job.dstId = record.peerId;
			job.srcId = record.sourceId.peer;
		}
		job.total++;
		if (record.state.toInt() >= 3) {
			job.sent++;
		} else {
			job.unfinishedFiles++;
		}
		job.paused = job.paused || record.paused;
		job.sourceMsgs.push_back(record.sourceId);
		job.uploadDone.push_back(record.state.toInt() >= 2);
		job.fileId.push_back(record.fileId);
		job.uploadedParts.push_back(record.uploadedParts);
	}
	std::vector<SavedJob> result;
	for (auto &[jobId, job] : jobs) {
		if (job.unfinishedFiles > 0) {
			result.push_back(std::move(job));
		} else {
			db.clearEfResumeJob(jobId);
		}
	}
	return result;
}

qint64 PersistedForwardBytes(
		not_null<Main::Session*> session,
		qint64 *total) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		if (total) {
			*total = 0;
		}
		return 0;
	}
	auto ready = qint64(0);
	auto tot = qint64(0);
	for (const auto &r : db.loadUnfinishedEfResumeItems(session->uniqueId())) {
		if (r.state.toInt() >= 3) {
			continue;
		}
		const auto localSize = QFileInfo::exists(r.localPath)
			? qint64(QFileInfo(r.localPath).size())
			: qint64(0);
		ready += localSize;
		if (r.fileSize > 0) {
			tot += r.fileSize;
		} else if (const auto msg = session->data().message(r.sourceId)) {
			if (const auto media = msg->media()) {
				if (const auto doc = media->document()) {
					tot += doc->size;
					continue;
				} else if (const auto photo = media->photo()) {
					tot += localSize;
					continue;
				}
			}
			tot += localSize;
		} else {
			tot += localSize;
		}
	}
	if (total) {
		*total = tot;
	}
	return ready;
}

std::optional<qint64> persistedItemBytes(
		not_null<Main::Session*> session,
		const FullMsgId &sourceId) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return std::nullopt;
	}
	for (const auto &r : db.loadUnfinishedEfResumeItems(session->uniqueId())) {
		if (r.state.toInt() >= 3 || r.sourceId != sourceId) {
			continue;
		}
		return QFileInfo::exists(r.localPath)
			? std::optional<qint64>(QFileInfo(r.localPath).size())
			: std::nullopt;
	}
	return std::nullopt;
}

std::optional<qint64> persistedItemFileSize(
		not_null<Main::Session*> session,
		const FullMsgId &sourceId) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return std::nullopt;
	}
	for (const auto &r : db.loadUnfinishedEfResumeItems(session->uniqueId())) {
		if (r.state.toInt() >= 3 || r.sourceId != sourceId) {
			continue;
		}
		if (r.fileSize > 0) {
			return r.fileSize;
		}
		if (const auto msg = session->data().message(r.sourceId)) {
			if (const auto media = msg->media()) {
				if (const auto doc = media->document()) {
					return qint64(doc->size);
				}
			}
		}
		return std::nullopt;
	}
	return std::nullopt;
}

std::optional<SavedJob> GetUnfinishedJobByDst(
		const PeerId &dstId,
		not_null<Main::Session*> session) {
	for (const auto &job : GetUnfinishedJobs(session)) {
		if (job.dstId == dstId) {
			return job;
		}
	}
	return std::nullopt;
}

void ApplyRevealedMessagesTo(
		not_null<Main::Session*> session,
		PeerId peer,
		const MTPmessages_Messages &result) {
	const auto apply = [&](const MTPVector<MTPMessage> &messages) {
		for (const auto &message : messages.v) {
			if (message.type() != mtpc_message) {
				continue;
			}
			const auto existing = session->data().message(
				FullMsgId{ peer, MsgId(message.c_message().vid().v) });
			if (existing) {
				existing->applyRevealedMedia(message.c_message());
			}
		}
	};
	if (result.type() == mtpc_messages_messages) {
		apply(result.c_messages_messages().vmessages());
	} else if (result.type() == mtpc_messages_channelMessages) {
		apply(result.c_messages_channelMessages().vmessages());
	}
}

void EnsureForwardSourceMessages(
		not_null<Main::Session*> session,
		const std::vector<FullMsgId> &sourceIds,
		Fn<void(bool succeeded)> done) {
	struct Pending {
		std::vector<FullMsgId> ids;
		bool takeout = false;
	};
	// Keyed by session as well: two accounts may resolve the same peer id,
	// and one account's request must never block the other's.
	using InFlightKey = std::pair<Main::Session*, PeerId>;
	static base::flat_map<InFlightKey, std::vector<Fn<void(bool)>>> inFlight;
	auto &owner = session->data();
	auto prepared = base::flat_map<PeerId, Pending>();
	auto unresolved = base::flat_map<PeerId, std::vector<FullMsgId>>();
	auto unresolvedTakeout = base::flat_set<PeerId>();
	for (const auto &sourceId : sourceIds) {
		const auto existing = owner.message(sourceId);
		const auto refetch = existing
			&& !existing->media()
			&& (existing->flags() & MessageFlag::NoForwards);
		if (existing && !refetch) {
			continue;
		}
		if (!IsServerMsgId(sourceId.msg)) {
			continue;
		}
		const auto groupPeer = peerIsChannel(sourceId.peer)
			? sourceId.peer
			: session->userPeerId();
		if (peerIsChannel(sourceId.peer)) {
			// An unloaded channel has no access hash yet, and a request for a
			// bare channel id without one is rejected as CHANNEL_INVALID.
			// Resolve the channel first instead of sending an invalid
			// request or silently dropping the ids.
			const auto channel = owner.channelLoaded(
				peerToChannel(sourceId.peer));
			if (!channel || !channel->accessHash()) {
				unresolved[groupPeer].push_back(sourceId);
				if (refetch) {
					unresolvedTakeout.emplace(groupPeer);
				}
				continue;
			}
		}
		auto &perPeer = prepared[groupPeer];
		if (refetch) {
			perPeer.takeout = true;
		}
		perPeer.ids.push_back(sourceId);
	}
	if (prepared.empty() && unresolved.empty()) {
		if (done) {
			done(true);
		}
		return;
	}
	const auto weakSession = base::make_weak(session);
	const auto remaining = std::make_shared<int>(0);
	const auto anyFailed = std::make_shared<bool>(false);
	const auto completion = std::make_shared<Fn<void(bool)>>(std::move(done));
	const auto complete = [=](bool ok) {
		if (!ok) {
			*anyFailed = true;
		}
		if (--*remaining > 0) {
			return;
		}
		if (const auto callback = std::move(*completion)) {
			if (weakSession) {
				callback(!*anyFailed);
			}
		}
	};
	const auto finishOne = [=](const PeerId &groupPeer, bool ok) {
		const auto waiting = inFlight.find(InFlightKey{ session.get(), groupPeer });
		if (waiting == end(inFlight)) {
			return;
		}
		const auto callbacks = base::take(waiting->second);
		inFlight.erase(waiting);
		for (const auto &callback : callbacks) {
			if (callback) {
				callback(ok);
			}
		}
	};
	const auto sendPrepared = [=](
			base::flat_map<PeerId, Pending> requests) {
		auto count = 0;
		for (const auto &[groupPeer, perPeer] : requests) {
			if (!perPeer.ids.empty()) {
				++count;
			}
		}
		*remaining += count;
		for (auto &[groupPeer, perPeer] : requests) {
			if (perPeer.ids.empty()) {
				continue;
			}
			auto &waiting = inFlight[InFlightKey{ session.get(), groupPeer }];
			if (!waiting.empty()) {
				// The same peer is already being fetched. Fetch the ids of
				// this call once that request finishes, so they are never
				// silently left unresolved.
				const auto ids = perPeer.ids;
				waiting.push_back([=](bool) {
					EnsureForwardSourceMessages(
						session,
						ids,
						[=](bool ok) { complete(ok); });
				});
				continue;
			}
			waiting.push_back(complete);
			auto input = QVector<MTPInputMessage>();
			input.reserve(int(perPeer.ids.size()));
			for (const auto &id : perPeer.ids) {
				input.push_back(MTP_inputMessageID(MTP_int(id.msg.bare)));
			}
			if (const auto channelId = peerToChannel(groupPeer)) {
				const auto channel = session->data().channelLoaded(channelId);
				if (!channel || !channel->accessHash()) {
					finishOne(groupPeer, false);
					continue;
				}
				auto inner = MTPchannels_GetMessages(
					channel->inputChannel(),
					MTP_vector<MTPInputMessage>(input));
				const auto onDone = [=](const MTPmessages_Messages &result) {
					if (weakSession) {
						session->data().processExistingMessages(
							session->data().channelLoaded(channelId),
							result);
						ApplyRevealedMessagesTo(session, groupPeer, result);
					}
					finishOne(groupPeer, true);
				};
				const auto onFail = [=](const MTP::Error &) {
					finishOne(groupPeer, false);
				};
				if (const auto id = perPeer.takeout
					? session->api().takeoutId()
					: std::optional<uint64>()) {
					session->api().request(
						MTPInvokeWithTakeout<MTPchannels_GetMessages>(
							MTP_long(*id),
							std::move(inner))
					).done(onDone).fail(onFail).send();
				} else {
					session->api().request(std::move(inner)
					).done(onDone).fail(onFail).send();
				}
			} else {
				auto inner = MTPmessages_GetMessages(
					MTP_vector<MTPInputMessage>(input));
				const auto onDone = [=](const MTPmessages_Messages &result) {
					if (weakSession) {
						session->data().processExistingMessages(
							nullptr,
							result);
						ApplyRevealedMessagesTo(session, groupPeer, result);
					}
					finishOne(groupPeer, true);
				};
				const auto onFail = [=](const MTP::Error &) {
					finishOne(groupPeer, false);
				};
				if (const auto id = perPeer.takeout
					? session->api().takeoutId()
					: std::optional<uint64>()) {
					session->api().request(
						MTPInvokeWithTakeout<MTPmessages_GetMessages>(
							MTP_long(*id),
							std::move(inner))
					).done(onDone).fail(onFail).send();
				} else {
					session->api().request(std::move(inner)
					).done(onDone).fail(onFail).send();
				}
			}
		}
	};
	if (unresolved.empty()) {
		sendPrepared(std::move(prepared));
		return;
	}
	auto channels = QVector<MTPInputChannel>();
	for (const auto &[groupPeer, ids] : unresolved) {
		channels.push_back(MTP_inputChannel(
			MTP_long(peerToChannel(groupPeer).bare),
			MTP_long(0)));
	}
	session->api().request(MTPchannels_GetChannels(
		MTP_vector<MTPInputChannel>(channels)
	)).done([=](const MTPmessages_Chats &result) mutable {
		if (!weakSession) {
			return;
		}
		result.match([&](const auto &data) {
			session->data().processChats(data.vchats());
		});
		auto requests = std::move(prepared);
		auto failed = 0;
		for (const auto &[groupPeer, ids] : unresolved) {
			const auto channel = session->data().channelLoaded(
				peerToChannel(groupPeer));
			if (!channel || !channel->accessHash()) {
				++failed;
				continue;
			}
			auto &perPeer = requests[groupPeer];
			perPeer.takeout = perPeer.takeout
				|| unresolvedTakeout.contains(groupPeer);
			perPeer.ids.insert(
				end(perPeer.ids),
				begin(ids),
				end(ids));
		}
		*remaining += failed;
		sendPrepared(std::move(requests));
		for (auto i = 0; i != failed; ++i) {
			complete(false);
		}
	}).fail([=](const MTP::Error &) mutable {
		if (!weakSession) {
			return;
		}
		const auto failed = int(unresolved.size());
		*remaining += failed;
		sendPrepared(std::move(prepared));
		for (auto i = 0; i != failed; ++i) {
			complete(false);
		}
	}).send();
}

bool isEnhancedUpload(const FullMsgId &uploadId) {
	if (EFUploadIds().contains(uploadId)) {
		return true;
	}
	auto &active = Pipeline::Active();
	for (const auto &[peer, weak] : active) {
		if (const auto pipeline = weak.lock()) {
			if (pipeline->containsUpload(uploadId)) {
				return true;
			}
		}
	}
	return false;
}

bool isEnhancedTempUpload(
		not_null<Main::Session*> session,
		const QString &filename) {
	return filename.startsWith(
		File::DefaultDownloadPath(session) + "ForwardTemp/");
}

rpl::producer<PeerId> stateChanges() {
	return StateChanges.events();
}

rpl::producer<> counterChanges() {
	return CounterChanges.events();
}

std::vector<JobSnapshot> MemoryJobs(not_null<Main::Session*> session) {
	auto result = std::vector<JobSnapshot>();

	const auto belongsToSession = [&](const PeerId &peer) {
		return session->data().peerLoaded(peer)
			&& &session->data().peer(peer)->session() == session;
	};

	for (const auto &[peer, state] : ActiveStates()) {
		if (!belongsToSession(peer)) continue;
		result.push_back({
			.peer = peer,
			.srcPeer = state.srcPeer,
			.progress = currentProgress(peer),
			.active = true,
		});
	}
	for (const auto &state : FinishedStates()) {
		const auto peer = state.destPeer;
		if (!belongsToSession(peer)) continue;
		auto progress = ForwardProgress();
		progress.state = state.cancelled
			? State::Cancelled
			: state.resumable
				? State::Paused
				: State::Finished;
		progress.sent = state.sent;
		progress.total = state.total;
		progress.skipped = state.skipped;
		progress.destPeer = peer;
		progress.sourceIds = state.sourceIds;
		progress.items = state.items;
		result.push_back({
			.peer = peer,
			.srcPeer = state.srcPeer,
			.progress = std::move(progress),
			.finished = !state.resumable,
			.resumable = state.resumable,
		});
	}
	for (const auto &[peer, requests] : StartQueue()) {
		if (!belongsToSession(peer)) {
			continue;
		}
		for (const auto &request : requests) {
			auto progress = ForwardProgress();
			progress.state = State::Idle;
			progress.destPeer = peer;
			auto srcPeer = peer;
			for (const auto &item : request.items) {
				progress.total++;
				progress.sourceIds.push_back(item->fullId());
				srcPeer = item->history()->peer->id;
				auto tracked = TrackedItem();
				if (const auto media = item->media()) {
					if (const auto doc = media->document()) {
						tracked.info.name = doc->filename();
						tracked.info.size = doc->size;
					} else if (const auto photo = media->photo()) {
						tracked.info.name = QString::number(photo->id)
							+ u".jpg"_q;
					}
				}
				progress.items.push_back(tracked);
			}
			if (progress.total == 0 && request.resumeJob) {
				srcPeer = request.resumeJob->srcId;
				for (const auto &full : request.resumeJob->sourceMsgs) {
					progress.total++;
					progress.sourceIds.push_back(full);
					progress.items.push_back(TrackedItem{});
				}
			}
			result.push_back({
				.peer = peer,
				.srcPeer = srcPeer,
				.progress = std::move(progress),
			});
		}
	}
	return result;
}

std::vector<JobSnapshot> AllJobs(not_null<Main::Session*> session) {
	auto result = MemoryJobs(session);

	const auto belongsToSession = [&](const PeerId &peer) {
		return session->data().peerLoaded(peer)
			&& &session->data().peer(peer)->session() == session;
	};

	for (const auto &job : GetUnfinishedJobs(session)) {
		if (!belongsToSession(job.dstId)) continue;
		if (ActiveStates().find(job.dstId) != ActiveStates().end()
			|| HasFinishedForPeer(job.dstId)) {
			continue;
		}
		auto progress = ForwardProgress();
		progress.state = State::Paused;
		progress.sent = job.sent;
		progress.total = job.total;
		progress.destPeer = job.dstId;
		progress.sourceIds = job.sourceMsgs;
		progress.items.resize(job.total);
		for (auto i = 0; i < int(progress.items.size()); i++) {
			auto &tracked = progress.items[i];
			const auto done = (i < int(job.uploadDone.size())
				&& job.uploadDone[i]);
			tracked.state = done ? ItemState::Done : ItemState::Pending;
			tracked.sent = done;
			tracked.progress = done ? 1.0 : 0.0;
		}
		result.push_back({
			.peer = job.dstId,
			.srcPeer = job.srcId,
			.progress = std::move(progress),
			.resumable = true,
		});
	}
	return result;
}

bool EfWorkPendingFor(not_null<Main::Session*> session, const PeerId &dst) {
	for (const auto &job : AllJobs(session)) {
		if (job.peer == dst
			&& ((job.active && !job.finished) || job.resumable)) {
			return true;
		}
	}
	if (const auto it = StartQueue().find(dst); it != end(StartQueue())) {
		for (const auto &request : it->second) {
			if (&request.api->session() == session.get()) {
				return true;
			}
		}
	}
	return false;
}

rpl::producer<std::vector<JobSnapshot>> jobsValue(
		not_null<Main::Session*> session) {
	return rpl::single(MemoryJobs(session)) | rpl::then(
		StateChanges.events()
		| rpl::map([session](const PeerId &) {
			return MemoryJobs(session);
		})
	);
}

void MarkForwardedDone(
		not_null<Main::Session*> session,
		const FullMsgId &sourceId,
		const QByteArray &hash) {
	Q_UNUSED(session);
	Q_UNUSED(sourceId);
	Q_UNUSED(hash);
	// The serializer below reads the live states, where the just-sent item
	// is already marked, so nothing needs its ids passed in.
	WriteForwardedDonePostponed();
}

Fn<std::optional<QByteArray>()> ForwardedDoneSerializer() {
	return []() -> std::optional<QByteArray> {
		auto result = QByteArray();
		auto stream = QDataStream(&result, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		auto count = 0;
		for (const auto &state : FinishedStates()) {
			if (state.resumable) {
				continue;
			}
			count += int(state.sourceIds.size());
		}
		for (const auto &[_, state] : ActiveStates()) {
			const auto n = std::min(
				state.items.size(),
				state.sourceIds.size());
			for (auto i = 0U; i != n; ++i) {
				if (state.items[i].sent) {
					++count;
				}
			}
		}
		stream << qint32(1) << qint32(count);
		for (const auto &state : FinishedStates()) {
			if (state.resumable) {
				continue;
			}
			for (const auto &id : state.sourceIds) {
				stream << quint64(id.peer.value) << qint64(id.msg.bare);
			}
		}
		for (const auto &[_, state] : ActiveStates()) {
			const auto n = std::min(
				state.items.size(),
				state.sourceIds.size());
			for (auto i = 0U; i != n; ++i) {
				if (state.items[i].sent) {
					const auto &id = state.sourceIds[i];
					stream << quint64(id.peer.value)
						<< qint64(id.msg.bare);
				}
			}
		}
		return result;
	};
}

void WriteForwardedDonePostponed() {
	// The done list is global, not per account, so every account stores the
	// same blob and whichever logs in next finds it.
	for (const auto &account : Core::App().domain().orderedAccounts()) {
		if (const auto session = account->maybeSession()) {
			session->account().local().updateForwardedDone(
				ForwardedDoneSerializer());
		}
	}
}

std::vector<FullMsgId> DeserializeForwardedDone(const QByteArray &blob) {
	auto result = std::vector<FullMsgId>();
	if (blob.isEmpty()) {
		return result;
	}
	auto stream = QDataStream(blob);
	stream.setVersion(QDataStream::Qt_5_1);
	auto version = qint32(0);
	auto count = qint32(0);
	stream >> version >> count;
	if (stream.status() != QDataStream::Ok
		|| version != 1
		|| count < 0
		|| count > blob.size() / 16) {
		return {};
	}
	for (auto i = 0; i != count; ++i) {
		auto peer = quint64(0);
		auto msg = qint64(0);
		stream >> peer >> msg;
		if (stream.status() != QDataStream::Ok) {
			break;
		}
		if (!peer || !msg) {
			continue;
		}
		result.emplace_back(PeerId(peer), MsgId(msg));
	}
	return result;
}

void PauseAll(not_null<Main::Session*> session) {
	const auto jobs = GetUnfinishedJobs(session);
	for (const auto &job : jobs) {
		pauseForward(job.dstId, session);
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		db.setEfResumePaused(session->uniqueId(), PeerId(), true);
	}
	for (const auto &job : jobs) {
		NotifyStateChanged(job.dstId);
	}
	NotifyCounterChanged();
}

void EnsureResumeStatesSeeded(not_null<Main::Session*> session) {
	static base::flat_set<not_null<Main::Session*>> seeded;
	if (!seeded.emplace(session).second) {
		return;
	}
	session->lifetime().add([session] {
		seeded.remove(session);
	});
	auto &active = ActiveStates();
	auto &finished = FinishedStates();
	const auto doneItems = DeserializeForwardedDone(
		session->account().local().forwardedDoneSerialized());
	// The Forwards tab shows every forwarded item, persisted per-item so the
	// list accumulates across batches and restarts until the user clears it.
	auto seen = std::unordered_set<FullMsgId>();
	for (const auto &sourceId : doneItems) {
		if (!seen.emplace(sourceId).second) {
			continue;
		}
		const auto already = ranges::any_of(
			finished,
			[&](const SharedState &state) {
				return ranges::find(state.sourceIds, sourceId)
					!= end(state.sourceIds);
			});
		if (already) {
			continue;
		}
		auto &state = finished.emplace_back();
		state.total = 1;
		state.sent = 1;
		state.finished = true;
		state.destPeer = sourceId.peer;
		state.srcPeer = sourceId.peer;
		state.sourceIds.push_back(sourceId);
		state.items.push_back(TrackedItem{
			.state = ItemState::Done,
			.progress = 1.0,
			.uploadProgress = 1.0,
			.sent = true,
		});
		NotifyStateChanged(sourceId.peer);
		NotifyCounterChanged();
	}
	// Interrupted batches are resumed on demand; their resume data lives in
	// the EnhancedForward jobs grouped by (src, dst).
	const auto seedResumable = [&](const SavedJob &job) {
		if (active.find(job.dstId) != active.end()) {
			return;
		}
		for (const auto &state : finished) {
			if (state.destPeer == job.dstId && state.srcPeer == job.srcId) {
				return;
			}
		}
		auto &state = finished.emplace_back();
		state.total = job.total;
		state.sent = job.sent;
		state.skipped = 0;
		state.cancelled = false;
		state.paused = job.paused;
		state.finished = true;
		state.resumable = true;
		state.destPeer = job.dstId;
		state.srcPeer = job.srcId;
		state.sourceIds = job.sourceMsgs;
		state.items.resize(job.sourceMsgs.size());
		for (auto i = 0; i < int(state.items.size()); i++) {
			const auto done = (i < int(job.uploadDone.size())
				&& job.uploadDone[i]);
			auto &item = state.items[i];
			item.state = done ? ItemState::Done : ItemState::Pending;
			item.sent = done;
			item.progress = done ? 1.0 : 0.0;
			item.uploadProgress = done ? 1.0 : -1.0;
		}
		NotifyStateChanged(job.dstId);
		NotifyCounterChanged();
	};
	for (const auto &job : GetUnfinishedJobs(session)) {
		seedResumable(job);
	}
}

void cancelItem(
		not_null<Main::Session*> session,
		const PeerId &peer,
		int itemIndex) {
	auto &states = ActiveStates();
	const auto it = states.find(peer);
	if (it == states.end()) return;
	auto &state = it->second;
	if (state.cancelled || state.finished) return;
	if (itemIndex < 0 || itemIndex >= int(state.items.size())) return;

	const auto pipeline = Pipeline::Active().find(peer);
	if (pipeline != Pipeline::Active().end()) {
		pipeline->second.lock()->cancelItem(itemIndex);
	} else {
		// No live pipeline: just drop the item from the visible state.
		if (state.total > 0) state.total--;
		state.items[itemIndex].cancelled = true;
		fireUpdate(session, peer);
		NotifyCounterChanged();
	}
}

bool isEnhancedForwardItem(
		not_null<Main::Session*> session,
		not_null<const HistoryItem*> item) {
	const auto id = item->fullId();
	for (const auto &job : AllJobs(session)) {
		if (!job.active && !job.resumable) {
			continue;
		}
		if (ranges::find(job.progress.sourceIds, id) != end(job.progress.sourceIds)) {
			return true;
		}
	}
	return false;
}

void cancelQueuedItem(
		not_null<Main::Session*> session,
		const PeerId &peer,
		const FullMsgId &sourceId) {
	// Remove one item from a batch that is still waiting in the start queue:
	// the item must not reappear when the active forward is resumed or done.
	auto &queue = StartQueue();
	const auto it = queue.find(peer);
	if (it == end(queue)) {
		return;
	}
	auto changed = false;
	for (auto request = it->second.begin(); request != it->second.end();) {
		const auto before = int(request->items.size());
		request->items.erase(
			ranges::remove_if(request->items, [&](not_null<HistoryItem*> item) {
				return item->fullId() == sourceId;
			}),
			end(request->items));
		if (request->items.size() != before) {
			changed = true;
		}
		if (request->items.empty()) {
			request = it->second.erase(request);
		} else {
			++request;
		}
	}
	if (it->second.empty()) {
		queue.erase(it);
	}
	if (changed) {
		NotifyStateChanged(peer);
		NotifyCounterChanged();
	}
}

void RemoveEfTempFile(const QString &path) {
	TempFilesToRemove().add(path);
}

void dropPersistedEfItem(
		not_null<Main::Session*> session,
		const PeerId &destPeer,
		const FullMsgId &sourceId) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	auto localPath = QString();
	if (db.isOpen()) {
		for (const auto &r : db.loadUnfinishedEfResumeItems(
				session->uniqueId())) {
			if (r.sourceId == sourceId && !r.localPath.isEmpty()) {
				localPath = r.localPath;
				break;
			}
		}
	}
	db.removeEfResumeBySource(
		session->uniqueId(),
		destPeer,
		sourceId.msg);
	RemoveEfTempFile(localPath);
	if (const auto srcItem = session->data().message(sourceId)) {
		Core::App().downloadManager().removeLoading(srcItem);
	}
	auto &finished = FinishedStates();
	for (auto i = finished.begin(); i != finished.end();) {
		if (!i->resumable || i->destPeer != destPeer) {
			++i;
			continue;
		}
		const auto it = ranges::find(i->sourceIds, sourceId);
		if (it == end(i->sourceIds)) {
			++i;
			continue;
		}
		const auto idx = int(it - begin(i->sourceIds));
		i->sourceIds.erase(begin(i->sourceIds) + idx);
		if (idx < int(i->items.size())) {
			i->items.erase(begin(i->items) + idx);
		}
		i->total = int(i->sourceIds.size());
		i->sent = std::min(i->sent, i->total);
		if (i->total == 0) {
			NotifyStateChanged(destPeer);
			i = finished.erase(i);
			continue;
		}
		++i;
	}
	NotifyStateChanged(destPeer);
	NotifyCounterChanged();
}

void cancelItemByMessage(
		not_null<Main::Session*> session,
		not_null<const HistoryItem*> item) {
	const auto id = item->fullId();
	auto jobs = AllJobs(session);
	for (auto i = 0; i < int(jobs.size()); i++) {
		const auto &job = jobs[i];
		if (job.active) {
			for (auto j = 0; j < int(job.progress.sourceIds.size()); j++) {
				if (job.progress.sourceIds[j] == id) {
					auto &states = ActiveStates();
					if (states.find(job.peer) == states.end()
						&& Pipeline::Active().find(job.peer)
							== Pipeline::Active().end()) {
						auto &db = Core::App().downloadManager().ensureDedupDb();
						if (db.isOpen()) {
							db.removeEfResumeBySource(
								session->uniqueId(),
								job.peer,
								id.msg);
						}
						fireUpdate(session, job.peer);
						NotifyCounterChanged();
						return;
					}
					cancelItem(session, job.peer, j);
					return;
				}
			}
		} else if (job.resumable) {
			for (auto j = 0; j < int(job.progress.sourceIds.size()); j++) {
				if (job.progress.sourceIds[j] != id) {
					continue;
				}
				const auto srcItem = session->data().message(id);
				const auto pipelineIt = Pipeline::Active().find(job.peer);
				if (srcItem && pipelineIt != Pipeline::Active().end()) {
					pipelineIt->second.lock()->cancelItem(j);
				} else {
					dropPersistedEfItem(session, job.peer, id);
				}
				return;
			}
		} else if (!job.finished) {
			cancelQueuedItem(session, job.peer, id);
		}
	}
}

void CancelAll(not_null<Main::Session*> session) {
	BulkForward::EnsureResumeSeeded(session);
	const auto peers = [&] {
		auto result = std::vector<PeerId>();
		for (const auto &[peer, state] : ActiveStates()) {
			if (session->data().peerLoaded(peer)
				&& &session->data().peer(peer)->session() == session) {
				result.push_back(peer);
			}
		}
		return result;
	}();
	for (const auto &peer : peers) {
		cancelForward(
			peer,
			session,
			BulkForward::HasCounters(session, peer));
	}
	auto purgedPersisted = false;
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		for (const auto &job : GetUnfinishedJobs(session)) {
			if (session->data().peerLoaded(job.dstId)
				&& &session->data().peer(job.dstId)->session() == session) {
				if (job.sent > 0
					&& !BulkForward::HasCounters(session, job.dstId)) {
					auto skipped = 0;
					for (const auto &state : FinishedStates()) {
						if (state.resumable && state.destPeer == job.dstId) {
							skipped = state.skipped;
							break;
						}
					}
					RecordFinishedFwRun(
						session,
						job.dstId,
						job.srcId,
						job.sourceMsgs.empty()
							? FullMsgId()
							: job.sourceMsgs.front(),
						LastSourceId(job.sourceMsgs),
						std::max(job.sent, job.total - skipped),
						job.sent,
						skipped,
						CurrentForwardActionId(session, job.dstId),
						true);
				}
				CleanupPartialFilesForPeer(session, job.dstId);
				purgedPersisted = true;
				auto &finished = FinishedStates();
				for (auto i = finished.begin(); i != finished.end();) {
					if (i->resumable && i->destPeer == job.dstId) {
						i = finished.erase(i);
					} else {
						++i;
					}
				}
				NotifyStateChanged(job.dstId);
			}
		}
	}
	CleanupLeftoverForwardFiles(
		session,
		File::DefaultDownloadPath(session) + "ForwardTemp/",
		true);
	// Queued batches are not running yet, but the user asked to cancel all
	// forwards: drop them too so they don't silently start afterwards.
	auto &queue = StartQueue();
	for (auto it = queue.begin(); it != queue.end();) {
		const auto before = int(it->second.size());
		it->second.erase(ranges::remove_if(
			it->second,
			[&](const StartRequest &request) {
				return &request.api->session() == session;
			}), end(it->second));
		const auto peer = it->first;
		if (it->second.empty()) {
			it = queue.erase(it);
		} else {
			++it;
		}
		if (before) {
			NotifyStateChanged(peer);
		}
	}
	if (!peers.empty() || !queue.empty() || purgedPersisted) {
		NotifyCounterChanged();
	}
}

void notifyTransfersUpdated() {
	NotifyCounterChanged();
}

void ClearFinished(
		not_null<Main::Session*> session,
		const PeerId &peer) {
	auto &finished = FinishedStates();
	for (auto i = finished.begin(); i != finished.end();) {
		if (!i->resumable) {
			i = finished.erase(i);
		} else {
			++i;
		}
	}
	RemoveEFUploadsForPeer(peer);
	WriteForwardedDonePostponed();
	NotifyStateChanged(peer);
	NotifyCounterChanged();
}

void ClearFinishedItems(
		not_null<Main::Session*> session,
		const FullMsgId &sourceId) {
	auto &finished = FinishedStates();
	for (auto i = finished.begin(); i != finished.end();) {
		if (i->resumable) {
			++i;
			continue;
		}
		const auto itemIt = ranges::find(i->sourceIds, sourceId);
		if (itemIt == end(i->sourceIds)) {
			++i;
			continue;
		}
		const auto index = int(itemIt - i->sourceIds.begin());
		if (index < int(i->items.size())) {
			i->items.erase(i->items.begin() + index);
		}
		i->sourceIds.erase(itemIt);
		i->total = std::max(0, i->total - 1);
		if (i->total <= 0 || i->items.empty()) {
			i = finished.erase(i);
		} else {
			++i;
		}
	}
	WriteForwardedDonePostponed();
	NotifyStateChanged(sourceId.peer);
	NotifyCounterChanged();
}

void ClearForwardRun(
		not_null<Main::Session*> session,
		const PeerId &dst) {
	for (const auto &job : AllJobs(session)) {
		if (job.peer == dst && job.finished && !job.resumable) {
			for (const auto &srcId : job.progress.sourceIds) {
				ClearFinishedItems(session, srcId);
			}
		}
	}
	for (const auto &batch : ForwardBatches(session)) {
		if (batch.dst == dst && batch.finished) {
			DropForwardBatch(session, dst);
		}
	}
	for (const auto &record : FinishedFwRuns(session)) {
		if (record.dst == dst) {
			DropFinishedFwRunGroup(session, record.actionId);
		}
	}
}

auto Pipeline::Active()
-> std::unordered_map<PeerId, std::weak_ptr<Pipeline>> & {
	static auto map = std::unordered_map<PeerId, std::weak_ptr<Pipeline>>();
	return map;
}

void Pipeline::Start(
		not_null<ApiWrap*> api,
		std::vector<not_null<HistoryItem*>> &&items,
		const Api::SendAction &action,
		Data::ForwardOptions forwardOptions,
		Data::GroupingOptions groupOptions,
		std::shared_ptr<SavedJob> resumeJob,
		std::shared_ptr<Data::ForwardRunSeen> runSeen,
		SettledCallback settled,
		bool destinationOnlyDedup,
		ItemSettledCallback itemSettled,
		bool restoreProgress,
		bool bulkOwned) {
	const auto dstId = action.history->peer->id;
	if (ActiveStates().find(dstId) != ActiveStates().end()) {
		// A forward already runs to this peer: merge the new files into the
		// same pipeline instead of hiding them in a queue. They are processed
		// after the current items finish (one file at a time) and share the
		// same progress rendering, pause/resume and cancel logic.
		const auto live = Active().find(dstId);
		if (!resumeJob && live != Active().end()) {
			if (const auto pipeline = live->second.lock()) {
				pipeline->_bulkOwned = pipeline->_bulkOwned || bulkOwned;
				if (auto it = ActiveStates().find(dstId); it != end(ActiveStates())) {
					it->second.bulkOwned = it->second.bulkOwned || bulkOwned;
				}
				BeginForwardBatch(
					not_null{ &api->session() },
					dstId,
					int(items.size()));
				NoteForwardBatchFile(
					not_null{ &api->session() },
					dstId,
					ForwardBatchFirstName(items));
				pipeline->appendItems(
					std::move(items),
					std::move(runSeen),
					std::move(settled),
					destinationOnlyDedup,
					std::move(itemSettled));
				NotifyStateChanged(dstId);
				NotifyCounterChanged();
				return;
			}
		}
		if (!resumeJob) {
			BeginForwardBatch(
				not_null{ &api->session() },
				dstId,
				int(items.size()));
			NoteForwardBatchFile(
				not_null{ &api->session() },
				dstId,
				ForwardBatchFirstName(items));
		}
		StartQueue()[dstId].push_back(StartRequest{
			api,
			std::move(items),
			action,
			forwardOptions,
			groupOptions,
			std::move(resumeJob),
			std::move(runSeen),
			std::move(settled),
			destinationOnlyDedup,
			std::move(itemSettled),
			restoreProgress,
			bulkOwned });
		// Let the Forwards tab and the counters pick up the queued batch.
		NotifyStateChanged(dstId);
		NotifyCounterChanged();
		return;
	}
	const auto freshTotal = int(items.size());
	const auto firstName = ForwardBatchFirstName(items);
	auto pipeline = std::make_shared<Pipeline>(
		api,
		std::move(items),
		action,
		forwardOptions,
		groupOptions,
		resumeJob,
		std::move(runSeen),
		std::move(settled),
		destinationOnlyDedup,
		std::move(itemSettled),
		restoreProgress);
	Active()[dstId] = pipeline;
	pipeline->_bulkOwned = bulkOwned;
	if (!resumeJob) {
		EnsureForwardBatch(
			not_null{ &api->session() },
			dstId,
			freshTotal);
		NoteForwardBatchFile(
			not_null{ &api->session() },
			dstId,
			firstName);
	}
	pipeline->run();
}

Pipeline::Pipeline(
		not_null<ApiWrap*> api,
		std::vector<not_null<HistoryItem*>> &&items,
		const Api::SendAction &action,
		Data::ForwardOptions forwardOptions,
		Data::GroupingOptions groupOptions,
		std::shared_ptr<SavedJob> resumeJob,
		std::shared_ptr<Data::ForwardRunSeen> runSeen,
		SettledCallback settled,
		bool destinationOnlyDedup,
		ItemSettledCallback itemSettled,
		bool restoreProgress)
: _api(api)
, _session(api->session())
, _action(action)
, _forwardOptions(forwardOptions)
, _groupOptions(groupOptions)
, _runId(base::RandomValue<uint64>())
, _runSeen(std::move(runSeen))
, _initialSettled(std::move(settled))
, _itemSettled(std::move(itemSettled))
, _destinationOnly(destinationOnlyDedup) {
	_downloadPath = File::DefaultDownloadPath(&_session) + "ForwardTemp/";
	_peerId = action.history->peer->id;
	_srcPeer = !items.empty()
		? items.front()->history()->peer->id
		: _peerId;

	if (resumeJob) {
		_n = int(resumeJob->sourceMsgs.size());
		_items.resize(_n);
		for (auto i = 0; i < _n; i++) {
			_items[i].sourceId = resumeJob->sourceMsgs[i];
		}
		_sent = resumeJob->sent;
		loadProgress();

		for (auto i = 0; i < _n; i++) {
			const auto jobUploadDone = (i < int(resumeJob->uploadDone.size()))
				? resumeJob->uploadDone[i]
				: false;
			if (jobUploadDone) {
				_items[i].downloadDone = true;
				_items[i].uploadDone = true;
				_items[i].sent = true;
			} else {
				const auto local = QFileInfo(_items[i].path);
				const auto fileExists = local.exists() && local.size() > 0;
				if (_items[i].downloadDone && fileExists) {
					_items[i].uploadDone = false;
				} else {
					_items[i].downloadDone = false;
					_items[i].needsDownload = true;
					_items[i].restoredNotDownloaded = true;
					_items[i].uploadDone = false;
				}
			}
		}
	} else {
		_n = int(items.size());
		_items.resize(_n);
		for (auto i = 0; i < _n; i++) {
			_items[i].sourceId = items[i]->fullId();
		}
		if (restoreProgress) {
			restoreProgressFromDb();
		}
	}

	EnsureEFUploadIdSync(&_session);

	// The transfer tab destroys a local upload message when its upload
	// fails, including a programmatic cancel from this pipeline. Never keep
	// a raw pointer to a message that may be gone; every later use checks
	// for a null and skips the repaint.
	_session.data().itemRemoved() | rpl::on_next([this](
			not_null<const HistoryItem*> item) {
		for (auto &task : _items) {
			if (task.sentItem == item.get()) {
				task.sentItem = nullptr;
			}
		}
	}, _itemRemovedLifetime);
}

Pipeline::~Pipeline() {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		db.clearFwTmpRun(_session.uniqueId(), _peerId);
	}
	if (db.isOpen()) {
		for (const auto &item : _items) {
			// Release only the in-flight dedup registrations. Items that
			// were actually sent keep their finished ('f') content record
			// so a later forward of the same item is detected as a
			// duplicate; wiping them here broke dedup for the next run.
			if (item.mediaId && !item.sent) {
				db.removePending(
					Data::DedupDb::Table::Uploads,
					item.mediaId,
					item.isPhoto);
			}
		}
	}
	auto &active = Active();
	const auto it = active.find(_peerId);
	if (it != active.end() && it->second.lock().get() == this) {
		active.erase(it);
	}
	// Last resort: never leave the standard half held by a gate nobody will
	// open any more. Delayed, and only if the gate did not move meanwhile, so
	// a pending confirmation or a new forward is never clobbered.
	if (const auto gate = NormalForward::CurrentGate(_peerId)) {
		base::call_delayed(kOrderConfirmGraceMs, [api = _api, peer = _peerId, gate] {
			if (NormalForward::CurrentGate(peer) == gate) {
				NormalForward::AdvanceGate(api, peer, MsgId(0));
			}
		});
	}
	for (const auto &item : _items) {
		if (const auto srcItem = _session.data().message(item.sourceId)) {
			ClearShadowUpload(&_session, srcItem);
		}
	}
}

void Pipeline::run() {
	const auto self = shared_from_this();
	if (_initialSettled) {
		auto indexes = std::vector<int>();
		indexes.reserve(_n);
		for (auto i = 0; i != _n; ++i) {
			indexes.push_back(i);
		}
		addReport(
			std::move(indexes),
			std::move(_initialSettled),
			std::move(_itemSettled));
	}
	for (auto i = 0; i != _n; ++i) {
		const auto &item = _items[i];
		if (item.cancelled) {
			settleItem(i, SettleResult::Cancelled);
		} else if (item.dedupSkipped) {
			settleItem(i, classifySkip(item));
		} else if (item.sentItem && IsServerMsgId(item.sentItem->id)) {
			settleItem(i, SettleResult::Created, item.sentItem->fullId());
		} else if (item.sent) {
			// Restored from a checkpoint: the message was created in an
			// earlier session and its id is no longer known.
			settleItem(i, SettleResult::Cancelled);
		}
	}
	if (!_takeoutRefetchTried) {
		_takeoutRefetchTried = true;
		auto restricted = std::vector<FullMsgId>();
		for (auto i = 0; i < _n; i++) {
			const auto srcItem = _session.data().message(_items[i].sourceId);
			if (srcItem
				&& !srcItem->media()
				&& (srcItem->flags() & MessageFlag::NoForwards)) {
				restricted.push_back(_items[i].sourceId);
			}
		}
		if (!restricted.empty()) {
			const auto session = &_session;
			// Active() holds only a weak_ptr, so this must be the last strong
			// reference or the pipeline dies with this call stack.
			_api->ensureTakeout(
				_session.data().peer(restricted.front().peer),
				[self, session, restricted](bool) {
					EnsureForwardSourceMessages(
						session,
						restricted,
						[self](bool succeeded) {
							self->run();
						});
				});
			return;
		}
	}
	{
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			db.setEfResumePaused(_session.uniqueId(), _peerId, false);
		}
	}
	Info::Downloads::SetLastActivityTab(Info::Downloads::Tab::Forwards);
	_uploadLifetime = std::make_shared<rpl::lifetime>();
	_dlLifetime = std::make_shared<rpl::lifetime>();
	_uploadIndex = std::make_shared<base::flat_map<FullMsgId, int>>();

	// Classify items
	base::flat_map<MessageGroupId, int> albumItemCounts;
	for (auto i = 0; i < _n; i++) {
		const auto srcItem = _session.data().message(_items[i].sourceId);
		if (!srcItem) { _items[i].textOnly = true; continue; }
		const auto media = srcItem->media();
		if (!media) {
			_items[i].textOnly = true;
		} else if (media->photo()) {
			_items[i].isPhoto = true;
			if (const auto sg = srcItem->groupId()) {
				albumItemCounts[sg]++;
			}
		} else if (media->document()) {
			if (const auto sg = srcItem->groupId()) {
				albumItemCounts[sg]++;
			}
		} else {
			_items[i].textOnly = true;
		}
	}

	const auto regroupAll = (_groupOptions == Data::GroupingOptions::RegroupAll);
	const auto separate = (_groupOptions == Data::GroupingOptions::Separate);
	MessageGroupId regroupAllId;
	if (regroupAll) {
		regroupAllId = MessageGroupId::FromRaw(
			_action.history->peer->id,
			base::RandomValue<uint64>(),
			false);
	}

	for (auto i = 0; i < _n; i++) {
		if (_items[i].textOnly) continue;
		const auto srcItem = _session.data().message(_items[i].sourceId);
		if (!srcItem) { _items[i].textOnly = true; continue; }

		MessageGroupId sg;
		if (separate) {
			sg = MessageGroupId();
		} else if (regroupAll) {
			sg = regroupAllId;
		} else {
			sg = srcItem->groupId();
		}

		if (sg) {
			_items[i].sourceGroup = sg;
			if (_albums.find(sg) == _albums.end()) {
				auto album = std::make_shared<SendingAlbum>();
				album->options = _action.options;
				album->expectedCount = [&] {
					if (regroupAll) {
						int count = 0;
						for (auto j = 0; j < _n; j++) {
							if (!_items[j].textOnly) count++;
						}
						return count;
					} else {
						auto it = albumItemCounts.find(sg);
						return (it != albumItemCounts.end()) ? it->second : 1;
					}
				}();
				_api->_sendingAlbums.emplace(album->groupId, album);
				_albums.emplace(sg, std::move(album));
			}
		}
	}

	for (auto i = 0; i < _n; i++) {
		if (_items[i].textOnly || _items[i].path.isEmpty()) {
			const auto item = _session.data().message(_items[i].sourceId);
			if (!item) {
				adjustAlbumCount(i);
				_items[i].textOnly = true;
				_items[i].downloadDone = true;
				_items[i].uploadDone = true;
				continue;
			}
			const auto media = item->media();
			if (const auto doc = media ? media->document() : nullptr) {
				const auto filepath = doc->filepath(true);
				if (!filepath.isEmpty()) {
					_items[i].path = filepath;
					_items[i].downloadDone = true;
					EnhancedForward::updateDownloadProgress(
						&_session, _peerId, i,
						{ doc->filename(), doc->size },
						1.0);
				} else {
					auto name = doc->filename();
					if (name.isEmpty()) name = u"file"_q;
					name.replace(QRegularExpression("[:<>\"\\\\|?*]"), "_");
				_items[i].path = QDir(_downloadPath).absoluteFilePath(
					QString::number(_runId) + u"_"_q
					+ QString::number(i) + u"_"_q + name);
				QDir().mkpath(QFileInfo(_items[i].path).absolutePath());
				_items[i].needsDownload = true;
				}
			} else if (const auto photo = media ? media->photo() : nullptr) {
				const auto v = photo->activeMediaView();
				const auto destPath = QDir(_downloadPath).absoluteFilePath(
					QString::number(_runId) + u"_"_q
					+ QString::number(i) + u"_"_q + QString::number(photo->id) + u".jpg"_q);
				_items[i].path = destPath;
				if (v && v->loaded() && v->saveToFile(destPath)) {
					_items[i].downloadDone = true;
					EnhancedForward::updateDownloadProgress(
						&_session, _peerId, i,
						{ QString::number(photo->id) + u".jpg"_q, 0 },
						1.0);
				} else {
					_items[i].needsDownload = true;
				}
			} else {
				adjustAlbumCount(i);
				_items[i].textOnly = true;
				_items[i].downloadDone = true;
				_items[i].uploadDone = true;
			}
		}
		for (auto i = 0; i < _n; i++) {
			if (_items[i].textOnly || _items[i].fileSize > 0) {
				continue;
			}
			if (const auto srcItem = _session.data().message(_items[i].sourceId)) {
				if (const auto media = srcItem->media()) {
					if (const auto doc = media->document()) {
						_items[i].fileSize = doc->size;
					} else if (const auto photo = media->photo()) {
						_items[i].fileSize = PhotoFingerprintSize(
							not_null{ photo });
					}
				}
			}
		}
	}

	_session.uploader().photoReady(
	) | rpl::on_next([self](const Storage::UploadedMedia &data) {
		self->onUploadDone(data);
	}, *_uploadLifetime);

	_session.uploader().documentReady(
	) | rpl::on_next([self](const Storage::UploadedMedia &data) {
		self->onUploadDone(data);
	}, *_uploadLifetime);

	_session.uploader().photoFailed(
	) | rpl::on_next([self](const FullMsgId &fullId) {
		self->onUploadFail(fullId);
	}, *_uploadLifetime);

	_session.uploader().documentFailed(
	) | rpl::on_next([self](const FullMsgId &fullId) {
		self->onUploadFail(fullId);
	}, *_uploadLifetime);

	_session.uploader().photoProgressInfo(
	) | rpl::on_next([self](const Storage::UploadFileProgress &data) {
		self->onUploadProgress(data);
	}, *_uploadLifetime);

	_session.uploader().documentProgressInfo(
	) | rpl::on_next([self](const Storage::UploadFileProgress &data) {
		self->onUploadProgress(data);
	}, *_uploadLifetime);

	_session.data().documentLoadProgress(
	) | rpl::on_next([self](not_null<DocumentData*> doc) {
		const auto now = crl::now();
		if (now - self->_lastDlProgressMs < 150) {
			return;
		}
		self->_lastDlProgressMs = now;
		for (auto i = 0; i < self->_n; i++) {
			self->checkItem(i);
			auto &item = self->_items[i];
			if (item.cancelled
				|| item.dedupSkipped
				|| item.downloadDone
				|| !item.downloadStarted) {
				continue;
			}
			const auto src = self->_session.data().message(item.sourceId);
			const auto media = src ? src->media() : nullptr;
			const auto srcDoc = media ? media->document() : nullptr;
			if (!srcDoc || srcDoc != doc || !srcDoc->loading()) {
				continue;
			}
			EnhancedForward::updateDownloadProgress(
				&self->_session,
				self->_peerId,
				i,
				{ srcDoc->filename(), srcDoc->size },
				srcDoc->progress());
			self->_session.data().notifyDocumentLayoutChanged(srcDoc);
		}
		for (auto i = 0; i < self->_n; i++) {
			const auto &item = self->_items[i];
			if (item.sentItem
				&& item.sentItem->media()
				&& item.sentItem->media()->document() == doc) {
				self->_session.data().requestItemRepaint(item.sentItem);
			}
		}
	}, *_dlLifetime);

	_session.downloaderTaskFinished(
	) | rpl::on_next([self] {
		for (auto i = 0; i < self->_n; i++) {
			self->checkItem(i);
			if (self->_items[i].sentItem) {
				self->_session.data().requestItemRepaint(
					self->_items[i].sentItem);
			}
		}
	}, *_dlLifetime);

	EnsureOrderGateWatch(&_session);

	{
		const auto dedupOn = GetEnhancedBool("prevent_forward_duplicates");
		for (auto i = 0; i < _n; i++) {
			auto &item = _items[i];
			if (item.textOnly || item.uploadDone || item.dedupSkipped) continue;
			if (!item.mediaId) {
				const auto srcItem = _session.data().message(item.sourceId);
				const auto media = srcItem ? srcItem->media() : nullptr;
				const auto doc = media ? media->document() : nullptr;
				const auto photo = media ? media->photo() : nullptr;
				if (!doc && !photo) continue;
				item.mediaId = doc ? uint64(doc->id) : uint64(photo->id);
			}
			if (item.downloadDone && !item.path.isEmpty()) {
				item.dedupNeedsHash = true;
			}
		}
		// Pre-mark the repeated mediaIds inside this very batch, so the
		// Forwards tab shows only the unique files from the start and the
		// duplicates never download at all.
		base::flat_set<uint64> seenMediaIds;
		for (auto i = 0; i < _n; i++) {
			const auto &item = _items[i];
			if (item.dedupSkipped || item.textOnly || !item.mediaId) {
				continue;
			}
			if (!seenMediaIds.emplace(item.mediaId).second) {
				premarkDuplicate(i);
			}
		}
		auto &dedupDb = Core::App().downloadManager().ensureDedupDb();
		if (dedupOn && dedupDb.isOpen()) {
			for (auto i = 0; i < _n; i++) {
				if (_items[i].mediaId && !_items[i].dedupSkipped) {
					dedupDb.removePending(
						Data::DedupDb::Table::Uploads,
						_items[i].mediaId,
						_items[i].isPhoto);
				}
			}
		}
		for (auto i = 0; i < _n; i++) {
			if (_items[i].dedupNeedsHash && !_items[i].dedupSkipped) {
				dedupCheckItem(i);
			}
		}
	}

	if (GetEnhancedBool("prevent_forward_duplicates")) {
		// Pre-check all media in order: the duplicate files in this batch
		// share content hashes but NOT docIds, so the same-docId premarking
		// above can't catch them. Querying the remote fingerprints up-front
		// (before the download queue starts) lets us mark them all before the
		// Forwards tab renders, so only the unique files are shown up-front.
		// Documents and photos follow the same rule: files above the partial
		// hash size are sampled, smaller ones are hashed after the download.
		for (auto i = 0; i < _n; i++) {
			auto &item = _items[i];
			if (item.dedupSkipped || item.textOnly || item.downloadDone) {
				continue;
			}
			if (!item.needsDownload) {
				continue;
			}
			const auto srcItem = _session.data().message(item.sourceId);
			const auto media = srcItem ? srcItem->media() : nullptr;
			const auto doc = media ? media->document() : nullptr;
			const auto photo = media ? media->photo() : nullptr;
			if (!doc && !photo) {
				continue;
			}
			const auto size = doc
				? qint64(doc->size)
				: PhotoFingerprintSize(not_null{ photo });
			if (!item.mediaId) {
				item.mediaId = doc ? uint64(doc->id) : uint64(photo->id);
			}
			if (size <= Data::kDedupMinPartialHashSize) {
				continue;
			}
			_dedupPrecheckQueue.push_back(i);
		}
		if (!_dedupPrecheckQueue.empty()) {
			runNextPrecheck();
			return;
		}
	}
	startSession();
}

void Pipeline::saveProgress() {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	const auto jobId = u"ef_%1_%2"_q.arg(_srcPeer.value).arg(_peerId.value);
	for (auto i = 0; i < int(_items.size()); i++) {
		const auto &it = _items[i];
		// Dedup-skipped items are never forwarded and never resumed, so they
		// must not leave resume rows behind (they would re-appear as finished
		// history after a restart).
		if (it.dedupSkipped || it.cancelled) {
			continue;
		}
		auto record = Data::EfResumeItem();
		record.jobId = jobId;
		record.itemIndex = i;
		record.peerId = _peerId;
		record.sourceId = it.sourceId;
		record.state = QString::number(it.uploadDone
			? (it.sent ? 3 : 2)
			: (it.downloadDone ? 1 : 0));
		record.localPath = it.path;
		record.fileId = it.fileId;
		record.uploadedParts = it.uploadedParts;
		record.fileHash = it.fileHash;
		record.mediaId = it.mediaId;
		record.fileSize = it.fileSize;
		record.sessionId = _session.uniqueId();
		record.paused = isPaused(_peerId);
		db.insertEfResumeItem(record);
	}
}

bool Pipeline::loadProgress() {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return false;
	}
	const auto records = db.loadEfResumeItemsForPeer(
		_session.uniqueId(),
		_peerId);
	if (records.empty()) {
		return false;
	}
	for (const auto &record : records) {
		for (auto i = 0; i < int(_items.size()); i++) {
			if (_items[i].sourceId != record.sourceId) {
				continue;
			}
			const auto state = record.state.toInt();
			_items[i].path = record.localPath;
			_items[i].downloadDone = (state >= 1);
			_items[i].uploadDone = (state >= 2);
			_items[i].sent = (state >= 3);
			_items[i].fileId = record.fileId;
			_items[i].uploadedParts = record.uploadedParts;
			_items[i].fileHash = record.fileHash;
			_items[i].mediaId = record.mediaId
				? record.mediaId
				: _items[i].mediaId;
			_items[i].fileSize = record.fileSize;
			break;
		}
	}
	return true;
}

void Pipeline::restoreProgressFromDb() {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	const auto records = db.loadEfResumeItemsForPeer(
		_session.uniqueId(),
		_peerId);
	for (const auto &record : records) {
		const auto state = record.state.toInt();
		if (state >= 3) {
			// A resumed scan page repeats the message that was delivered
			// last; its destination record marks it as a duplicate, so it
			// has to start fresh instead of settling as a cancellation.
			continue;
		}
		for (auto &item : _items) {
			if (item.sourceId != record.sourceId) {
				continue;
			}
			item.path = record.localPath;
			item.downloadDone = (state >= 1);
			// A finished upload may still have lost its send response, so it
			// is uploaded again from the saved part count and then sent.
			item.uploadDone = false;
			item.fileId = record.fileId;
			item.uploadedParts = record.uploadedParts;
			item.fileHash = record.fileHash;
			if (record.mediaId) {
				item.mediaId = record.mediaId;
			}
			item.fileSize = record.fileSize;
			if (!item.downloadDone) {
				item.needsDownload = true;
				item.restoredNotDownloaded = true;
			}
			break;
		}
	}
}

void Pipeline::setupCallbacks() {
	const auto self = shared_from_this();
	EnhancedForward::setCancelCallback(
		_peerId,
		&_session,
		[self] {
			auto &dedupDb = Core::App().downloadManager().ensureDedupDb();
			for (auto i = 0; i < self->_n; i++) {
				const auto msg = self->_session.data().message(self->_items[i].sourceId);
				if (!msg) continue;
				const auto media = msg->media();
				if (const auto doc = media ? media->document() : nullptr) {
					doc->cancel();
				} else if (const auto photo = media ? media->photo() : nullptr) {
					photo->cancel();
				}
				if (self->_items[i].uploadId != FullMsgId()) {
					self->_session.uploader().cancel(self->_items[i].uploadId);
					self->_uploadIndex->erase(self->_items[i].uploadId);
				}
				if (!self->_items[i].uploadDone
					&& self->_items[i].mediaId
					&& dedupDb.isOpen()) {
					dedupDb.removePending(
						Data::DedupDb::Table::Uploads,
						self->_items[i].mediaId,
						self->_items[i].isPhoto);
				}
			}
			EnhancedForward::CleanupPartialFilesForPeer(&self->_session, self->_peerId);
			self->settleAllCancelled();
		});

	EnhancedForward::setPauseCallback(
		_peerId,
		&_session,
		[self] {
			for (auto i = 0; i < self->_n; i++) {
				if (self->_items[i].textOnly || self->_items[i].uploadDone) continue;
				if (!self->_items[i].downloadDone) {
					const auto item = self->_session.data().message(self->_items[i].sourceId);
					if (item) {
						const auto media = item->media();
						if (const auto doc = media ? media->document() : nullptr) {
							doc->pause();
						} else if (const auto photo = media ? media->photo() : nullptr) {
							photo->cancel();
						}
					}
				}
				if (self->_items[i].uploadId != FullMsgId()) {
					self->_session.uploader().pause(self->_items[i].uploadId);
				}
			}
			self->_downloadInFlight = false;
			self->_uploadInFlight = false;
			self->saveProgress();
		});

	EnhancedForward::setResumeCallback(
		_peerId,
		&_session,
		[self] {
			self->loadProgress();
			self->_session.uploader().unpause();
			for (auto i = 0; i < self->_n; i++) {
				if (self->_items[i].textOnly || self->_items[i].uploadDone) continue;
				const auto srcItem = self->_session.data().message(self->_items[i].sourceId);
				if (!srcItem) {
					self->adjustAlbumCount(i);
					self->_items[i].textOnly = true;
					self->_items[i].downloadDone = true;
					self->_items[i].uploadDone = true;
					continue;
				}
				const auto media = srcItem->media();
				if (!media || (!media->document() && !media->photo())) {
					continue;
				}
				const auto local = QFileInfo(self->_items[i].path);
				if (self->_items[i].downloadDone
					&& (!local.exists() || local.size() <= 0)) {
					self->_items[i].downloadDone = false;
				}
				if (const auto doc = media->document()) {
					const auto path = self->_items[i].path;
					const auto attached = doc->loading()
						&& doc->loadingFilePath() == path;
					const auto complete = (doc->size > 0
						&& local.exists()
						&& local.size() >= doc->size);
					if (complete && !self->_items[i].downloadDone) {
						// Every byte is already on disk: release the loader,
						// which can only wait on a file it sees as finished,
						// and let the upload start.
						self->_items[i].downloadDone = true;
						self->_items[i].downloadedBytes = local.size();
						self->_items[i].dedupNeedsHash = true;
						if (attached) {
							doc->pause();
							doc->cancel();
						}
					} else if (!complete
						&& attached
						&& doc->downloadPaused()) {
						doc->resume();
					}
				}
				self->_items[i].downloadStarted = false;
				// Items with a live uploader entry resume on their own after
				// the unpause above; recreating the upload would send the
				// same file twice, so keep their state untouched.
				const auto liveUpload = self->_items[i].uploadStarted
					&& self->_items[i].uploadId != FullMsgId();
				if (!liveUpload) {
					self->_items[i].uploadStarted = false;
				}
				if (!self->_items[i].downloadDone) {
					self->_items[i].needsDownload = true;
				}
				self->_items[i].uploadDone = false;
			}
			self->_downloadCursor = 0;
			self->_uploadCursor = 0;
			self->_downloadInFlight = false;
			self->_uploadInFlight = false;
			for (auto i = 0; i < self->_n; i++) {
				if (self->_items[i].dedupNeedsHash) {
					self->dedupCheckItem(i);
				}
			}
			self->pumpDownloads();
			self->pumpUploads();
			self->sendNext();
		});

	// A loader can stop reporting progress while its last requests stay
	// unanswered, leaving a complete file on disk that never reaches
	// downloadDone and therefore never uploads. Poll the in-flight items so
	// that a full-size file is finalized even without another progress event.
	const auto weak = std::weak_ptr<Pipeline>(self);
	if (!_downloadWatchdog.isActive()) {
		_downloadWatchdog.setCallback([weak] {
			if (const auto strong = weak.lock()) {
				strong->checkStalledDownloads();
			}
		});
		_downloadWatchdog.callEach(2000);
	}
}

void Pipeline::checkStalledDownloads() {
	if (EnhancedForward::isPaused(_peerId)) {
		return;
	}
	if (EnhancedForward::currentProgress(_peerId).state
		== EnhancedForward::State::Cancelled) {
		return;
	}
	constexpr auto kStallTimeout = 30000;
	constexpr auto kMaxStallRestarts = 2;
	const auto now = crl::now();
	for (auto i = 0; i != _n; ++i) {
		auto &item = _items[i];
		if (!item.downloadStarted
			|| item.downloadDone
			|| item.cancelled
			|| item.dedupSkipped) {
			continue;
		}
		checkItem(i);
		if (item.downloadDone || item.cancelled) {
			continue;
		}
		const auto srcItem = _session.data().message(item.sourceId);
		const auto media = srcItem ? srcItem->media() : nullptr;
		const auto doc = media ? media->document() : nullptr;
		if (!doc || !doc->loading() || doc->loadingFilePath() != item.path) {
			continue;
		}
		if (doc->downloadPaused()) {
			continue;
		}
		const auto offset = qint64(doc->loadOffset());
		if (_watchdogIndex != i) {
			_watchdogIndex = i;
			_watchdogOffset = offset;
			_watchdogStamp = now;
			continue;
		}
		if (offset != _watchdogOffset) {
			_watchdogOffset = offset;
			_watchdogStamp = now;
			continue;
		}
		if (now - _watchdogStamp < kStallTimeout) {
			continue;
		}
		_watchdogIndex = -1;
		_watchdogStamp = now;
		if (item.stallRestarts >= kMaxStallRestarts) {
			doc->cancel();
			item.downloadedBytes = 0;
			item.downloadDone = false;
			_downloadInFlight = false;
			downloadFailed(i, true);
			continue;
		}
		++item.stallRestarts;
		restartStalledDownload(i);
	}
}

bool Pipeline::startDocumentDownload(
		int idx,
		not_null<HistoryItem*> srcItem,
		not_null<DocumentData*> doc) {
	auto &item = _items[idx];
	if (item.path.isEmpty()) {
		return false;
	}
	const auto folder = QFileInfo(item.path).absolutePath();
	if (!folder.isEmpty()) {
		QDir().mkpath(folder);
	}
	doc->save(
		Data::FileOrigin(FullMsgId(srcItem->history()->peer->id, srcItem->id)),
		item.path,
		LoadFromCloudOrLocal,
		false,
		true);
	const auto loading = doc->loading()
		&& doc->loadingFilePath() == item.path;
	const auto info = QFileInfo(item.path);
	const auto complete = info.exists() && info.size() >= doc->size;
	if (!loading && !complete) {
		return false;
	}
	if (loading) {
		Core::App().downloadManager().addLoading(
			{ .item = srcItem, .document = doc },
			true);
	}
	return true;
}

void Pipeline::restartStalledDownload(int i) {
	auto &item = _items[i];
	const auto srcItem = _session.data().message(item.sourceId);
	const auto media = srcItem ? srcItem->media() : nullptr;
	const auto doc = media ? media->document() : nullptr;
	if (!doc || !doc->loading()) {
		return;
	}
	// The loader holds requests that will never be answered and the file on
	// disk may already be full size because a later part was written past a
	// hole. Cancelling drops the file so the retry starts clean instead of
	// resuming at the file size and never asking for the missing parts.
	item.downloadedBytes = 0;
	item.downloadDone = false;
	doc->cancel();
	if (!startDocumentDownload(i, srcItem, doc)) {
		downloadFailed(i);
		return;
	}
	checkItem(i);
}

void Pipeline::sendNext() {
	const auto self = shared_from_this();
	if (EnhancedForward::currentProgress(_peerId).state == EnhancedForward::State::Cancelled) {
		_downloadWatchdog.cancel();
		if (_uploadLifetime) _uploadLifetime->destroy();
		if (_dlLifetime) _dlLifetime->destroy();
		_session.data().sendHistoryChangeNotifications();
		return;
	}
	if (EnhancedForward::isPaused(_peerId)) {
		return;
	}

	// Send text-only items; media items are handled by upload callbacks
	while (_current < _n) {
		const auto i = _current;

		if (_items[i].cancelled) {
			_current++;
			_items[i].sent = true;
			continue;
		}

		if (_items[i].textOnly) {
			_current++;
			const auto srcItem = _session.data().message(_items[i].sourceId);
			if (srcItem) {
				const auto randomId = base::RandomValue<uint64>();
				const auto history = _action.history;
				const auto peer = history->peer;
				auto caption = srcItem->originalText();
				TextUtilities::Trim(caption);
				auto sentEntities = Api::EntitiesToMTP(
					&_session,
					caption.entities,
					Api::ConvertOption::SkipLocal);
				using SendFlag = MTPmessages_SendMessage::Flag;
				auto sendFlags = SendFlag(0)
					| (ShouldSendSilent(peer, _action.options) ? SendFlag::f_silent : SendFlag(0))
					| (!sentEntities.v.isEmpty() ? SendFlag::f_entities : SendFlag(0))
					| (_action.options.scheduled ? SendFlag::f_schedule_date : SendFlag(0));
				if (_action.replyTo) {
					sendFlags |= SendFlag::f_reply_to;
				}
				if (_action.options.sendAs) {
					sendFlags |= SendFlag::f_send_as;
				}
				if (_action.options.effectId) {
					sendFlags |= SendFlag::f_effect;
				}
				const auto done = [this, i, randomId](
						const MTPUpdates &updates,
						const MTP::Response &) {
					const auto newId = MessageIdFromUpdates(updates, randomId);
					EnhancedForward::updateItemState(
						&_session, _peerId, i,
						ItemState::Done,
						ItemInfo(),
						1.0);
					_items[i].confirmed = true;
					settleItem(
						i,
						newId ? SettleResult::Created : SettleResult::Cancelled,
						newId ? FullMsgId(_peerId, newId) : FullMsgId());
					EnhancedForward::markItemSent(&_session, _peerId);
				};
				const auto fail = [this, i, randomId](const MTP::Error &error, const MTP::Response &) {
					_api->sendMessageFail(error, _action.history->peer, randomId, FullMsgId());
					EnhancedForward::updateItemState(
						&_session, _peerId, i,
						ItemState::Done,
						ItemInfo(),
						1.0);
					_items[i].confirmed = true;
					settleItem(i, SettleResult::Cancelled);
					EnhancedForward::markItemSent(&_session, _peerId);
				};
				_session.data().histories().sendPreparedMessage(
					history,
					_action.replyTo,
					randomId,
					Data::Histories::PrepareMessage<MTPmessages_SendMessage>(
						MTP_flags(sendFlags),
						peer->input(),
						Data::Histories::ReplyToPlaceholder(),
						MTP_string(caption.text),
						MTP_long(randomId),
						MTPReplyMarkup(),
						sentEntities,
						MTP_int(_action.options.scheduled),
						MTP_int(_action.options.scheduleRepeatPeriod),
						(_action.options.sendAs ? _action.options.sendAs->input() : MTP_inputPeerEmpty()),
						Data::ShortcutIdToMTP(&_session, _action.options.shortcutId),
						MTP_long(_action.options.effectId),
						MTP_long(0),
						Api::SuggestToMTP(_action.options.suggest),
						MTPInputRichMessage()),
					done,
					fail);
			} else {
				_items[i].confirmed = true;
				settleItem(i, SettleResult::Cancelled);
				EnhancedForward::markItemSent(&_session, _peerId);
			}
			_items[i].sent = true;
			continue;
		}

		// Media item: skip if already sent, wait if not yet sent
		if (!_items[i].sent) return;
		_current++;
	}

	// Check if all items are done
	for (auto i = 0; i < _n; i++) {
		if (!_items[i].sent) return;
	}

	auto done = 0;
	for (auto i = 0; i < _n; i++) {
		if (!_items[i].cancelled && !_items[i].dedupSkipped) {
			done++;
		}
	}
	if (!EnhancedForward::ForwardBatchOwns(&_session, _peerId)
		&& (done > 0 || _skippedCount > 0)) {
		EnhancedForward::ShowForwardDoneToast(
			done,
			done + _skippedCount,
			_skippedCount);
	}

	// Cleanup
	_session.data().sendHistoryChangeNotifications();
	_session.changes().historyUpdated(
		_action.history,
		(_action.options.scheduled
			? Data::HistoryUpdate::Flag::ScheduledSent
			: Data::HistoryUpdate::Flag::MessageSent));
	for (auto i = 0; i < _n; i++) {
		if (!_items[i].path.isEmpty() && _items[i].path.startsWith(_downloadPath)) {
			RemoveEfTempFile(_items[i].path);
		}
	}
	const auto session = &_session;
	const auto path = _downloadPath;
	crl::on_main([=] {
		CleanupLeftoverForwardFiles(session, path, true);
	});
	for (auto i = 0; i < _n; i++) {
		refreshSourceItemState(i);
	}
	_downloadWatchdog.cancel();
	if (_uploadLifetime) _uploadLifetime->destroy();
	if (_dlLifetime) _dlLifetime->destroy();
}

void Pipeline::pumpUploads() {
	const auto self = shared_from_this();
	if (EnhancedForward::isPaused(_peerId)) return;
	while (true) {
		if (_uploadInFlight) return;
		while (_uploadCursor < _n
				&& (_items[_uploadCursor].dedupSkipped
					|| _items[_uploadCursor].cancelled
					|| _items[_uploadCursor].uploadDone)) {
			_uploadCursor++;
		}
		if (_uploadCursor >= _n) return;
		auto &candidate = _items[_uploadCursor];
		if (!candidate.downloadDone) return;
		if (candidate.dedupHashPending) return;
		// After a resume the cursor is reset to zero: skip items whose upload
		// is already queued (they were paused and unpaused on resume) so the
		// file is not uploaded and sent twice.
		if (candidate.uploadStarted
			&& candidate.uploadId != FullMsgId()
			&& ranges::any_of(
				_session.uploader().activeUploads(),
				[&](const auto &info) {
					return info.itemId == candidate.uploadId;
				})) {
			_uploadCursor++;
			continue;
		}
		const auto i = _uploadCursor;
		_items[i].uploadStarted = true;
		_uploadInFlight = true;
		_uploadCursor++;
		startUploadForItem(i);
		return;
	}
}

void Pipeline::startUploadForItem(int i) {
	auto &item = _items[i];
	const auto srcItem = _session.data().message(item.sourceId);
	if (!srcItem || item.textOnly) {
		item.uploadDone = true;
		_uploadInFlight = false;
		pumpUploads();
		sendNext();
		return;
	}
	const auto to = FileLoadTo(
		_action.history->peer->id,
		_action.options,
		_action.replyTo,
		_action.replaceMediaOf);
	const auto media = srcItem->media();
	const auto doc = media ? media->document() : nullptr;
	const auto cover = media ? media->videoCover() : nullptr;
	auto args = FileLoadTask::Args{
		.session = &_session,
		.filepath = item.path,
		.content = QByteArray(),
		.information = nullptr,
		.videoCover = nullptr,
		.coverInput = cover
			? std::make_optional(cover->mtpInput())
			: std::nullopt,
		.type = item.isPhoto ? SendMediaType::Photo : SendMediaType::File,
		.to = to,
		.caption = (_forwardOptions
			== Data::ForwardOptions::NoNamesAndCaptions)
			? TextWithTags()
			: TextWithTags{
				srcItem->originalText().text,
				TextUtilities::ConvertEntitiesToTextTags(srcItem->originalText().entities) },
		.spoiler = false,
		.album = nullptr,
		.forceFile = false,
		.sendLargePhotos = item.isPhoto,
		.idOverride = 0,
		.displayName = {},
	};
	if (doc) {
		args.displayName = doc->filename();
	}
	if (!item.isPhoto && !item.path.isEmpty()) {
		auto info = FileLoadTask::ReadMediaInformation(
			item.path,
			QByteArray(),
			doc ? doc->mimeString() : QString());
		if (info) {
			if (doc) {
				if (const auto sd = doc->song()) {
					if (auto song = std::get_if<Ui::PreparedFileInformation::Song>(
							&info->media)) {
						if (song->title.isEmpty()) song->title = sd->title;
						if (song->performer.isEmpty()) {
							song->performer = sd->performer;
						}
					}
				}
				if (!doc->inlineThumbnailBytes().isEmpty()) {
					const auto inlineThumb = Images::FromInlineBytes(
						doc->inlineThumbnailBytes());
					if (!inlineThumb.isNull()) {
						if (auto song = std::get_if<Ui::PreparedFileInformation::Song>(
								&info->media)) {
							if (song->cover.isNull()) song->cover = inlineThumb;
						} else if (info->fileThumbnail.isNull()) {
							info->fileThumbnail = inlineThumb;
						}
					}
				}
			}
			args.information = std::move(info);
		}
	}
	const auto self = shared_from_this();
	auto tasks = std::vector<std::unique_ptr<Task>>();
	tasks.push_back(std::make_unique<EnhancedFileTask>(
		std::move(args),
		[this, i, self](std::shared_ptr<FilePrepareResult> result) {
			auto &item = _items[i];
			if (item.cancelled) {
				_uploadInFlight = false;
				pumpUploads();
				return;
			}
			const auto prepareState = EnhancedForward::currentProgress(_peerId).state;
			if (result && result->filesize > 0) {
				item.prepared = std::move(result);
				if (item.fileId == 0) {
					item.fileId = base::RandomValue<uint64>();
				}
				item.prepared->fileId = item.fileId;
				const auto path = item.path;
				if (!path.isEmpty()) {
					const auto realSize = QFile(path).size();
					if (realSize > item.prepared->filesize) {
						item.prepared->filesize = realSize;
						item.prepared->partssize = realSize;
					}
				}
				if (item.prepared->partssize > 0 && !item.prepared->fileparts.empty()) {
					item.partSize = item.prepared->partssize / int(item.prepared->fileparts.size());
				}
			} else if (prepareState == EnhancedForward::State::Sending) {
				adjustAlbumCount(i);
				item.textOnly = true;
				item.uploadDone = true;
				_uploadInFlight = false;
				pumpUploads();
				sendNext();
				return;
			} else {
				// The batch was paused while preparing: keep the item pending
				// for resume instead of marking it done.
				item.uploadDone = (prepareState
					!= EnhancedForward::State::Paused);
				_uploadInFlight = false;
				pumpUploads();
				return;
			}

			if (EnhancedForward::isPaused(_peerId)) {
				// The batch was paused while this file was being prepared.
				// Do not start the upload - the resume callback restarts it.
				item.uploadDone = false;
				_uploadInFlight = false;
				saveProgress();
				return;
			}

			// Create local message with media (like SendConfirmedFile)
			const auto localMsgId = FullMsgId(
				_peerId,
				_session.data().nextLocalMessageId());
			item.localMsgId = localMsgId;

			auto flags = NewMessageFlags(_action.history->peer);
			if (_action.replyTo) {
				flags |= MessageFlag::HasReplyInfo;
			}
			FillMessagePostFlags(_action, _action.history->peer, flags);
			if (_action.options.scheduled) {
				flags |= MessageFlag::IsOrWasScheduled;
			}

			const auto &prepared = item.prepared;
			const auto groupId = item.sourceGroup
				? [&] {
					const auto albumIt = _albums.find(item.sourceGroup);
					return albumIt != _albums.end()
						? albumIt->second->groupId
						: uint64(0);
				}()
				: uint64(0);

			const auto msgMedia = MTPMessageMedia([&] {
				if (prepared->type == SendMediaType::Photo) {
					using Flag = MTPDmessageMediaPhoto::Flag;
					return MTP_messageMediaPhoto(
						MTP_flags(Flag::f_photo),
						prepared->photo,
						MTPint(),
						MTPDocument());
				} else {
					using Flag = MTPDmessageMediaDocument::Flag;
					return MTP_messageMediaDocument(
						MTP_flags(Flag::f_document),
						prepared->document,
						MTPVector<MTPDocument>(),
						MTPPhoto(),
						MTPint(),
						MTPint());
				}
			}());

			auto caption = TextWithEntities{
				prepared->caption.text,
				TextUtilities::ConvertTextTagsToEntities(prepared->caption.tags)
			};
			TextUtilities::Trim(caption);

			// Start the upload BEFORE creating the local message, exactly
			// like Api::SendConfirmedFile does. This way the DocumentData is
			// created with the real (InMemoryLocation) thumbnail already
			// attached, so isSongWithCover() is true when the view is built
			// and the cover gets requested/loaded during the session.
			item.uploadId = localMsgId;
			_uploadIndex->emplace(localMsgId, i);
			EFUploadIds().emplace(localMsgId);
			_session.uploader().upload(localMsgId, item.prepared, item.uploadedParts);
			saveProgress();

			if (const auto srcItem = _session.data().message(item.sourceId)) {
				SetShadowUpload(
					&_session,
					srcItem,
					item.prepared ? int64(item.prepared->filesize) : item.fileSize);
				if (const auto media = srcItem->media()) {
					if (const auto doc = media->document()) {
						// The destination chat already renders the prepared
						// thumbnail. Mirror it onto the source document's
						// shared media view so the transfer manager rows for
						// this source item show the same cover instead of a
						// black box. Media-view only: never persist an
						// in-memory thumb into the document location, because
						// serialized in-memory thumbs are rejected on reload.
						if (!doc->hasThumbnail() && !item.prepared->thumb.isNull()) {
							doc->createMediaView()->setThumbnail(item.prepared->thumb);
						}
					}
				}
			}

			const auto localMsg = _action.history->addNewLocalMessage({
				.id = localMsgId.msg,
				.flags = flags,
				.from = NewMessageFromId(_action),
				.replyTo = _action.replyTo,
				.date = NewMessageDate(_action.options),
				.shortcutId = _action.options.shortcutId,
				.starsPaid = _action.options.starsApproved,
				.postAuthor = NewMessagePostAuthor(_action),
				.groupedId = groupId,
				.effectId = _action.options.effectId,
				.suggest = HistoryMessageSuggestInfo(_action.options),
			}, caption, msgMedia);

			// Add to album items if part of an album
			if (groupId) {
				const auto albumIt = _albums.find(item.sourceGroup);
				if (albumIt != _albums.end()) {
					const auto &album = albumIt->second;
					album->expectedCount = 0;
					for (auto j = 0; j < _n; j++) {
						const auto &other = _items[j];
						if (other.sourceGroup == item.sourceGroup
							&& !other.textOnly
							&& !other.cancelled
							&& !other.dedupSkipped) {
							album->expectedCount++;
						}
					}
					album->items.emplace_back(kEmptyTaskId);
					album->items.back().msgId = localMsg->fullId();
				}
			}

			// Set up upload tracking (use localMsgId as upload ID)
			item.sentItem = localMsg.get();

			const auto fileSize = prepared ? qint64(prepared->filesize) : qint64(0);
			const auto partSize = item.partSize;
			const auto totalParts = (partSize > 0 && fileSize > 0)
				? int((fileSize + partSize - 1) / partSize)
				: 0;
			const auto initialProgress = (totalParts > 0)
				? float64(item.uploadedParts) / float64(totalParts)
				: 0.0;
			EnhancedForward::updateUploadProgress(
				&_session, _peerId, i,
				{ prepared
					? ItemDisplayName(
						prepared->filename,
						item.path,
						_downloadPath)
					: QString(),
				  fileSize },
				initialProgress);
		}));
	_api->_fileLoader->addTasks(std::move(tasks));
}

void Pipeline::onUploadDone(const Storage::UploadedMedia &data) {
	if (data.fullId.peer != _peerId) return;
	const auto it = _uploadIndex->find(data.fullId);
	if (it == _uploadIndex->end()) return;
	const auto idx = it->second;
	auto &item = _items[idx];
	item.uploadInfo = std::move(data.info);
	item.uploadDone = true;
	item.retries = 0;
	if (!item.fileHash.isEmpty() && item.mediaId) {
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			db.insert(Data::DedupDb::Table::Uploads, {
				.hash = item.fileHash,
				.documentId = item.mediaId,
				.isPhoto = item.isPhoto,
				.status = u"f"_q,
			});
			db.insertFwTmp(
				_session.uniqueId(),
				_peerId,
				item.mediaId,
				item.isPhoto,
				item.fileHash);
		}
	}
	EnhancedForward::updateUploadProgress(
		&_session, _peerId, idx,
		{ item.prepared
			? ItemDisplayName(
				item.prepared->filename,
				item.path,
				_downloadPath)
			: QString(),
		  item.prepared ? qint64(item.prepared->filesize) : qint64(0) },
		1.0);
	saveProgress();

	if (const auto srcItem = _session.data().message(item.sourceId)) {
		ClearShadowUpload(&_session, srcItem);
	}

	// The Uploader itself already calls sendUploadedPhoto/Document
	// via its internal subscription (file_upload.cpp lines 183/199).
	// We just track completion here.
	if (item.sentItem) {
		_session.data().requestItemRepaint(item.sentItem);
	}
	item.sent = true;
	EnhancedForward::markItemSent(&_session, _peerId);
	MarkForwardedDone(&_session, item.sourceId, item.fileHash);

	if (const auto srcItem = _session.data().message(item.sourceId)) {
		Core::App().downloadManager().removeLoading(srcItem);
	}

	// The uploader queues the ForwardTemp removal on a worker after this
	// callback. Refresh on the next main-loop turn so the cached local
	// location is cleared after the uploader has finished its completion
	// path, without waiting for the asynchronous file removal.
	const auto self = shared_from_this();
	crl::on_main([self, idx] {
		self->refreshSourceItemState(idx);
	});

	_uploadInFlight = false;
	pumpUploads();
	sendNext();
}

void Pipeline::refreshSourceItemState(int idx) {
	if (idx < 0 || idx >= _n) {
		return;
	}
	const auto &item = _items[idx];
	if (const auto srcItem = _session.data().message(item.sourceId)) {
		if (const auto media = srcItem->media()) {
			if (const auto doc = media->document()) {
				if (doc->filepath(true).startsWith(_downloadPath)) {
					_session.local().removeFileLocation(doc->mediaKey());
					[[maybe_unused]] const auto refreshedPath = doc->filepath(true);
				}
				_session.data().requestDocumentViewRepaint(doc);
			}
		}
		_session.data().requestItemRepaint(srcItem);
	}
	if (item.sentItem) {
		if (const auto media = item.sentItem->media()) {
			if (const auto doc = media->document()) {
				[[maybe_unused]] const auto refreshedPath = doc->filepath(true);
				_session.data().requestDocumentViewRepaint(doc);
			}
		}
		_session.data().requestItemRepaint(item.sentItem);
	}
}

void Pipeline::onUploadFail(const FullMsgId &fullId) {
	if (fullId.peer != _peerId) return;
	const auto it = _uploadIndex->find(fullId);
	if (it == _uploadIndex->end()) return;
	const auto idx = it->second;
	auto &item = _items[idx];
	_uploadInFlight = false;
	const auto state = EnhancedForward::currentProgress(_peerId).state;
	if (state == EnhancedForward::State::Paused) {
		item.uploadDone = false;
		return;
	}
	if (state != EnhancedForward::State::Sending) {
		item.uploadDone = true;
		if (const auto srcItem = _session.data().message(item.sourceId)) {
			ClearShadowUpload(&_session, srcItem);
		}
		return;
	}
	constexpr auto kMaxUploadRetries = 3;
	if (item.retries < kMaxUploadRetries) {
		item.retries++;
		item.uploadedParts = 0;
		item.fileId = base::RandomValue<uint64>();
		item.uploadDone = false;
		_uploadInFlight = true;
		startUploadForItem(idx);
		return;
	}
	_uploadInFlight = false;
	item.uploadDone = false;
	item.retries = 0;
	item.uploadedParts = 0;
	item.fileId = base::RandomValue<uint64>();
	if (const auto srcItem = _session.data().message(item.sourceId)) {
		ClearShadowUpload(&_session, srcItem);
	}
	if (!item.fileHash.isEmpty() && item.mediaId) {
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			db.removePending(
				Data::DedupDb::Table::Uploads,
				item.mediaId,
				item.isPhoto);
		}
	}
	const auto history = _session.data().history(_peerId);
	const auto fileName = item.prepared
		? ItemDisplayName(item.prepared->filename, item.path, _downloadPath)
		: u"file"_q;
	const auto text = tr::lng_tm_fw_upload_failed(
		tr::now,
		lt_file_name,
		fileName);
	const auto randomId = base::RandomValue<uint64>();
	const auto newId = FullMsgId(_peerId, _session.data().nextLocalMessageId());
	_session.data().registerMessageRandomId(randomId, newId);
	history->addNewLocalMessage({
		.id = newId.msg,
		.flags = MessageFlags(),
		.from = NewMessageFromId(_action),
		.date = NewMessageDate(_action.options),
	}, TextWithEntities::Simple(text), MTP_messageMediaEmpty());
	saveProgress();
	EnhancedForward::pauseForward(_peerId, &_session);
}

void Pipeline::downloadFailed(int idx, bool force) {
	auto &item = _items[idx];
	constexpr auto kMaxDownloadRetries = 3;
	if (!force && item.retries < kMaxDownloadRetries) {
		item.retries++;
		item.downloadedBytes = 0;
		item.downloadDone = false;
		_downloadInFlight = false;
		_downloadCursor = idx;
		pumpDownloads();
		return;
	}
	_downloadInFlight = false;
	item.retries = 0;
	if (item.mediaId) {
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			db.removePending(
				Data::DedupDb::Table::Uploads,
				item.mediaId,
				item.isPhoto);
		}
	}
	const auto srcItem = _session.data().message(item.sourceId);
	const auto media = srcItem ? srcItem->media() : nullptr;
	const auto doc = media ? media->document() : nullptr;
	const auto photo = media ? media->photo() : nullptr;
	auto name = doc
		? doc->filename()
		: (photo ? QString::number(photo->id) + u".jpg"_q : QString());
	if (name.isEmpty()) {
		name = ItemDisplayName(
			QFileInfo(item.path).fileName(),
			item.path,
			_downloadPath);
	}
	if (name.isEmpty()) {
		name = u"file"_q;
	}
	const auto history = _session.data().history(_peerId);
	const auto text = tr::lng_tm_fw_download_failed(
		tr::now,
		lt_file_name,
		name);
	const auto randomId = base::RandomValue<uint64>();
	const auto newId = FullMsgId(_peerId, _session.data().nextLocalMessageId());
	_session.data().registerMessageRandomId(randomId, newId);
	history->addNewLocalMessage({
		.id = newId.msg,
		.flags = MessageFlags(),
		.from = NewMessageFromId(_action),
		.date = NewMessageDate(_action.options),
	}, TextWithEntities::Simple(text), MTP_messageMediaEmpty());
	saveProgress();
	EnhancedForward::pauseForward(_peerId, &_session);
}

void Pipeline::onUploadProgress(const Storage::UploadFileProgress &data) {
	if (data.fullId.peer != _peerId) return;
	const auto it = _uploadIndex->find(data.fullId);
	if (it == _uploadIndex->end()) return;
	const auto idx = it->second;
	auto &item = _items[idx];
	if (item.uploadDone) return;
	const auto prepared = item.prepared;
	const auto filename = prepared
		? ItemDisplayName(prepared->filename, item.path, _downloadPath)
		: QString();
	const auto filesize = prepared ? qint64(prepared->filesize) : qint64(0);
	const auto size = (filesize > 0) ? filesize : data.size;
	const auto p = (size > 0) ? std::clamp(float64(data.offset) / float64(size), 0., 1.) : 0.;
	if (data.partSize > 0) {
		item.partSize = data.partSize;
	}
		if (data.size > 0 && item.partSize > 0) {
			item.uploadedParts = int(data.offset / item.partSize);
			const auto newPct = (data.size > 0) ? int(float64(data.offset) / float64(data.size) * 100) : 0;
			const auto lastSaved = item.lastSavedPct;
			if (newPct >= lastSaved + 10
				|| newPct >= 99
				|| (newPct > 0 && lastSaved == 0)) {
				item.lastSavedPct = newPct;
				saveProgress();
			}
		}
		const auto now = crl::now();
		if (now - _lastShadowRepaintMs >= 150) {
			_lastShadowRepaintMs = now;
			if (const auto srcItem = _session.data().message(item.sourceId)) {
				UpdateShadowUpload(&_session, srcItem, data.offset, size);
			}
		}
	EnhancedForward::updateUploadProgress(&_session, _peerId, idx, { filename, filesize }, p);
}

void Pipeline::checkItem(int i) {
	const auto self = shared_from_this();
	auto &item = _items[i];
	if (item.downloadDone) return;
	if (!item.downloadStarted) return;
	if (EnhancedForward::currentProgress(_peerId).state == EnhancedForward::State::Cancelled) {
		return;
	}
	if (EnhancedForward::isPaused(_peerId)) return;

	const auto fi = QFileInfo(item.path);
	if (fi.exists()) {
		const auto srcItem = _session.data().message(item.sourceId);
		const auto media = srcItem ? srcItem->media() : nullptr;
		if (const auto doc = media ? media->document() : nullptr) {
			if (doc->size > 0 && fi.size() < doc->size) {
				item.downloadedBytes = fi.size();
				EnhancedForward::updateDownloadProgress(
					&_session, _peerId, i,
					{ doc->filename(), doc->size },
					float64(fi.size()) / float64(doc->size));
				_session.data().notifyDocumentLayoutChanged(doc);
				if (srcItem) {
					_session.data().requestItemRepaint(srcItem);
				}
				return;
			}
			// The file is full size, but the downloader may still hold it open
			// for writing: preparing it now runs the cover extraction, whose 7z
			// opens the file read-only and fails with a sharing violation.
			if (doc->loading() && doc->loadingFilePath() == item.path) {
				if (doc->loadOffset() < doc->size) {
					// The file has grown to full size while some parts are
					// still missing; wait for them.
					item.downloadedBytes = fi.size();
					EnhancedForward::updateDownloadProgress(
						&_session, _peerId, i,
						{ doc->filename(), doc->size },
						1.0);
					return;
				}
				// Every byte is on disk, but a loader whose remaining
				// requests were cancelled after the last part was written
				// never finalizes and would wait here forever. Mark the item
				// first, so the progress notification from pause() cannot
				// re-enter this function, then close the idle loader without
				// deleting the file.
				item.downloadDone = true;
				item.downloadedBytes = fi.size();
				item.retries = 0;
				_downloadInFlight = false;
				doc->pause();
				doc->cancel();
			}
		}
		item.downloadDone = true;
		item.downloadedBytes = fi.size();
		item.retries = 0;
		item.restoredNotDownloaded = false;
		_downloadInFlight = false;
		EnhancedForward::updateDownloadProgress(
			&_session, _peerId, i,
			{ ItemDisplayName(fi.fileName(), item.path, _downloadPath),
				fi.size() },
			1.0);
		dedupCheckItem(i);
		pumpUploads();
		pumpDownloads();
		return;
	}
	const auto srcItem = _session.data().message(item.sourceId);
	if (!srcItem) {
		return;
	}
	const auto media = srcItem->media();
	if (const auto doc = media ? media->document() : nullptr) {
		if (doc->loading()) {
			item.downloadedBytes = qint64(doc->loadOffset());
			EnhancedForward::updateDownloadProgress(
				&_session, _peerId, i,
				{ doc->filename(), doc->size },
				doc->progress());
			_session.data().notifyDocumentLayoutChanged(doc);
			_session.data().requestItemRepaint(srcItem);
		} else if (doc->status == FileDownloadFailed) {
			downloadFailed(i);
			return;
		}
	} else if (const auto photo = media ? media->photo() : nullptr) {
		const auto v = item.photoView ? item.photoView : photo->activeMediaView();
		const auto loaded = v && v->loaded();
		const auto failed = photo->failed(Data::PhotoSize::Large);
		const auto loading = photo->loading(Data::PhotoSize::Large);
		if (loaded) {
			if (v->saveToFile(item.path)) {
				item.downloadDone = true;
				item.downloadedBytes = QFile(item.path).size();
				item.retries = 0;
				EnhancedForward::updateDownloadProgress(
					&_session, _peerId, i,
					{ QString::number(photo->id) + u".jpg"_q, 0 },
					1.0);
				_downloadInFlight = false;
				dedupCheckItem(i);
				pumpUploads();
				pumpDownloads();
			} else {
				adjustAlbumCount(i);
				item.textOnly = true;
				item.downloadDone = true;
				item.uploadDone = true;
				sendNext();
			}
		} else if (failed) {
			downloadFailed(i);
			return;
		} else if (!loading) {
			adjustAlbumCount(i);
			item.textOnly = true;
			item.downloadDone = true;
			item.uploadDone = true;
			_downloadInFlight = false;
			sendNext();
			pumpDownloads();
		}
	}
}

void Pipeline::pumpDownloads() {
	const auto self = shared_from_this();
	while (_downloadCursor < _n
			&& (!_items[_downloadCursor].needsDownload
				|| _items[_downloadCursor].downloadDone
				|| _items[_downloadCursor].cancelled)) {
		_downloadCursor++;
	}
	if (EnhancedForward::isPaused(_peerId)) return;
	if (_downloadInFlight) return;
	if (_downloadCursor >= _n) return;

	const auto i = _downloadCursor;
	auto &item = _items[i];
	item.downloadStarted = true;
	_downloadInFlight = true;
	_downloadCursor++;
	const auto srcItem = _session.data().message(item.sourceId);
	const auto media = srcItem ? srcItem->media() : nullptr;
	const auto doc = media ? media->document() : nullptr;
	const auto photo = media ? media->photo() : nullptr;

	if (doc && doc->size > 0) {
		const auto local = QFileInfo(item.path);
		const auto fullSize = local.exists() && local.size() >= doc->size;
		if (fullSize && item.restoredNotDownloaded) {
			// The previous session never recorded this file as downloaded,
			// so its full size proves nothing: the old loader could have
			// written later parts past missing ones, and a fresh loader
			// cannot see the holes. Keep only files recorded as complete.
			if (!QFile::remove(item.path)) {
				item.downloadedBytes = 0;
				_downloadInFlight = false;
				downloadFailed(i, true);
				return;
			}
			item.downloadedBytes = 0;
		} else if (fullSize) {
			item.downloadDone = true;
			item.downloadedBytes = local.size();
			_downloadInFlight = false;
			if (doc->loading() && doc->loadingFilePath() == item.path) {
				// A loader that finds the file already complete never
				// requests another part and so never finishes: close it and
				// keep the file, so the upload can start right away.
				doc->pause();
				doc->cancel();
			}
			EnhancedForward::updateDownloadProgress(
				&_session, _peerId, i,
				{ ItemDisplayName(
					local.fileName(),
					item.path,
					_downloadPath),
				  local.size() },
				1.0);
			dedupCheckItem(i);
			pumpUploads();
			pumpDownloads();
			return;
		}
	}

	if (!item.dedupPrechecked && (doc || photo)) {
		const auto mediaId = doc ? uint64(doc->id) : uint64(photo->id);
		item.mediaId = mediaId;
		const auto dedupSetting = GetEnhancedBool(
			"prevent_forward_duplicates");
		const auto dedupOn = !_destinationOnly && dedupSetting;
		const auto sessionId = _session.uniqueId();
		auto &dedupDb = Core::App().downloadManager().ensureDedupDb();
		const auto knownById = dedupDb.isOpen()
			&& (dedupDb.chatTableContainsDocId(
					_peerId,
					mediaId,
					item.isPhoto,
					dedupOn)
				|| dedupDb.containsFwTmpItem(
					sessionId,
					_peerId,
					mediaId,
					item.isPhoto)
				|| (dedupSetting && seenInBatch(mediaId, item.isPhoto))
				|| (dedupOn
					&& !seenInRun(mediaId, item.isPhoto)
					&& dedupDb.containsDocId(
						Data::DedupDb::Table::Uploads,
						mediaId,
						item.isPhoto)));
		if (knownById) {
			skipAsDuplicate(i);
			return;
		}
		markSeen(mediaId, item.isPhoto);
		const auto size = doc
			? qint64(doc->size)
			: PhotoFingerprintSize(not_null{ photo });
		const auto remotePrecheck = (size > Data::kDedupMinPartialHashSize)
			&& dedupDb.isOpen();
		if (dedupOn && dedupDb.isOpen()) {
			// In-flight upload registration; the hash fills in once the
			// remote fingerprint (or the local file hash) is computed.
			dedupDb.addPending(
				Data::DedupDb::Table::Uploads,
				mediaId,
				QByteArray(),
				item.isPhoto);
		}
		if (remotePrecheck && !item.dedupPrechecked) {
			const auto origin = Data::FileOrigin(
				FullMsgId(srcItem->history()->peer->id, srcItem->id));
			const auto isPhoto = item.isPhoto;
			// The chunk request can outlive this pipeline: the run may finish
			// or be cancelled while its fingerprint is still in flight. Never
			// touch the item list through a raw pointer captured here.
			const auto weak = std::weak_ptr<Pipeline>(self);
			const auto startDownload = [=] {
				const auto strong = weak.lock();
				if (!strong) {
					return;
				}
				auto &it = strong->_items[i];
				if (doc) {
					if (!strong->startDocumentDownload(i, srcItem, doc)) {
						strong->downloadFailed(i);
						return;
					}
				} else {
					it.photoView = photo->createMediaView();
					photo->load(Data::PhotoSize::Large, origin);
				}
				strong->checkItem(i);
				if (it.downloadDone) {
					strong->pumpDownloads();
				}
			};
			auto done = [=](QByteArray hash) {
				const auto strong = weak.lock();
				if (!strong) {
					return;
				}
				auto &it = strong->_items[i];
				if (hash.isEmpty()) {
					it.dedupNeedsHash = true;
					startDownload();
					return;
				}
				it.fileHash = hash;
				it.fileSize = size;
				auto &db = Core::App().downloadManager().ensureDedupDb();
				// This media passed the id check above, so any content-hash
				// match means the same content was forwarded before: skip it
				// and attach this id to the hash for future id-only dedup.
				if (db.chatTableContainsHash(strong->_peerId, hash, dedupOn)) {
					strong->skipAsDuplicate(i);
					return;
				}
				if (db.isOpen()
					&& db.containsFwTmpHash(
						sessionId,
						strong->_peerId,
						hash)) {
					strong->skipAsDuplicate(i);
					return;
				}
				if (dedupOn) {
					const auto duplicate = strong->seenInBatch(hash)
						|| (!strong->seenInRun(hash)
							&& db.seekDocumentId(
								Data::DedupDb::Table::Uploads,
								hash,
								0) != 0);
					if (duplicate) {
						strong->skipAsDuplicate(i);
						return;
					}
					strong->markSeen(hash);
					db.addPending(
						Data::DedupDb::Table::Uploads,
						mediaId,
						hash,
						isPhoto);
				}
				// The remote fingerprint is the same value the local file
				// would produce, so there is nothing left to hash after the
				// download: skip the post-download pass like the up-front
				// precheck queue does.
				it.dedupPrechecked = true;
				it.dedupNeedsHash = false;
				startDownload();
			};
			if (doc) {
				Data::RemoteFileFingerprint(
					&_session,
					doc,
					std::move(done));
			} else {
				Data::RemotePhotoFingerprint(
					&_session,
					not_null{ photo },
					std::move(done));
			}
			return;
		}
		item.dedupNeedsHash = true;
	}

	if (doc) {
		if (!startDocumentDownload(i, srcItem, doc)) {
			downloadFailed(i);
			return;
		}
		saveProgress();
	} else if (photo) {
		item.photoView = photo->createMediaView();
		photo->load(
			Data::PhotoSize::Large,
			Data::FileOrigin(FullMsgId(srcItem->history()->peer->id, srcItem->id)));
	} else {
		item.textOnly = true;
		item.downloadDone = true;
		item.uploadDone = true;
		_downloadInFlight = false;
		sendNext();
		pumpDownloads();
		return;
	}
	checkItem(i);
	if (item.downloadDone) {
		pumpDownloads();
	}
}

bool Pipeline::seenInBatch(uint64 mediaId, bool isPhoto) const {
	return mediaId && _seenMedia.contains({ mediaId, isPhoto });
}

bool Pipeline::seenInBatch(const QByteArray &hash) const {
	return !hash.isEmpty() && _seenHashes.contains(hash);
}

bool Pipeline::seenInRun(uint64 mediaId, bool isPhoto) const {
	return _runSeen
		&& mediaId
		&& _runSeen->media.contains({ mediaId, isPhoto });
}

bool Pipeline::seenInRun(const QByteArray &hash) const {
	return _runSeen && !hash.isEmpty() && _runSeen->hashes.contains(hash);
}

void Pipeline::markSeen(uint64 mediaId, bool isPhoto) {
	if (!mediaId) {
		return;
	}
	const auto key = std::pair<uint64, bool>(mediaId, isPhoto);
	_seenMedia.insert(key);
	if (_runSeen) {
		_runSeen->media.insert(key);
	}
}

void Pipeline::markSeen(const QByteArray &hash) {
	if (hash.isEmpty()) {
		return;
	}
	_seenHashes.insert(hash);
	if (_runSeen) {
		_runSeen->hashes.insert(hash);
	}
}

void Pipeline::addReport(
		std::vector<int> indexes,
		SettledCallback done,
		ItemSettledCallback itemSettled) {
	if (!done) {
		return;
	}
	if (indexes.empty()) {
		done(std::vector<SettledItem>());
		return;
	}
	auto report = std::make_unique<BatchReport>();
	report->pending = std::move(indexes);
	report->done = std::move(done);
	report->itemSettled = std::move(itemSettled);
	for (const auto i : report->pending) {
		_itemReport.emplace_or_assign(i, report.get());
	}
	_reports.push_back(std::move(report));
	if (!_settleLifetime) {
		// A settled report can arrive after the pipeline's own cleanup, when
		// the last server id is confirmed. The subscription holds the
		// pipeline until every report is delivered; the watchdog bounds that
		// wait and dropSettling() releases it afterwards.
		_settleLifetime = std::make_shared<rpl::lifetime>();
		const auto self = shared_from_this();
		_session.data().itemIdChanged() | rpl::on_next([self](
				const Data::Session::IdChange &change) {
			if (IsServerMsgId(change.newId.msg)) {
				self->settleServerId(change.oldId, change.newId);
			}
		}, *_settleLifetime);
		armSettleWatchdog();
	}
}

void Pipeline::settleItem(
		int idx,
		SettleResult result,
		FullMsgId destination) {
	if (idx < 0 || idx >= _n) {
		return;
	}
	const auto it = _itemReport.find(idx);
	if (it == end(_itemReport)) {
		return;
	}
	auto *report = it->second;
	_itemReport.erase(it);
	report->pending.erase(
		std::remove(report->pending.begin(), report->pending.end(), idx),
		report->pending.end());
	auto settled = SettledItem{
		.source = _items[idx].sourceId,
		.destination = destination,
		.mediaId = _items[idx].mediaId,
		.isPhoto = _items[idx].isPhoto,
		.hash = _items[idx].fileHash,
		.size = _items[idx].fileSize,
		.result = result,
	};
	if (report->itemSettled) {
		report->itemSettled(settled);
	}
	report->results.push_back(std::move(settled));
	if (!report->pending.empty()) {
		return;
	}
	auto results = std::move(report->results);
	std::sort(
		results.begin(),
		results.end(),
		[](const SettledItem &a, const SettledItem &b) {
			return (a.source.peer != b.source.peer)
				? (a.source.peer < b.source.peer)
				: (a.source.msg < b.source.msg);
		});
	auto done = std::move(report->done);
	for (auto i = _reports.begin(); i != _reports.end(); ++i) {
		if (i->get() == report) {
			_reports.erase(i);
			break;
		}
	}
	if (_reports.empty()) {
		// The subscription may be the one currently delivering this very
		// result, so release it outside the emission.
		const auto weak = std::weak_ptr<Pipeline>(shared_from_this());
		crl::on_main([weak] {
			if (const auto strong = weak.lock()) {
				if (strong->_reports.empty()) {
					strong->dropSettling();
				}
			}
		});
	}
	if (done) {
		done(std::move(results));
	}
}

SettleResult Pipeline::classifySkip(const ItemTask &item) const {
	const auto mediaId = item.mediaId;
	const auto isPhoto = item.isPhoto;
	const auto &hash = item.fileHash;
	// Content already accepted by this batch or this run is handled by
	// another item's message, so the fan-out must not resend it anywhere.
	if ((mediaId && seenInBatch(mediaId, isPhoto))
		|| (!hash.isEmpty() && seenInBatch(hash))) {
		return SettleResult::CoveredByRun;
	}
	if ((mediaId && seenInRun(mediaId, isPhoto))
		|| (!hash.isEmpty() && seenInRun(hash))) {
		return SettleResult::CoveredByRun;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return SettleResult::DestinationDuplicate;
	}
	const auto sessionId = _session.uniqueId();
	const auto destination = (mediaId
			&& (db.chatTableContainsDocId(_peerId, mediaId, isPhoto, false)
				|| db.containsFwTmpItem(
					sessionId,
					_peerId,
					mediaId,
					isPhoto)))
		|| (!hash.isEmpty()
			&& (db.chatTableContainsHash(_peerId, hash, false)
				|| db.containsFwTmpHash(sessionId, _peerId, hash)));
	if (destination) {
		return SettleResult::DestinationDuplicate;
	}
	if (_destinationOnly
		|| !GetEnhancedBool("prevent_forward_duplicates")) {
		return SettleResult::CoveredByRun;
	}
	const auto global = (mediaId
			&& (db.chatTableContainsDocId(_peerId, mediaId, isPhoto, true)
				|| db.containsDocId(
					Data::DedupDb::Table::Uploads,
					mediaId,
					isPhoto)))
		|| (!hash.isEmpty()
			&& (db.chatTableContainsHash(_peerId, hash, true)
				|| db.seekDocumentId(
					Data::DedupDb::Table::Uploads,
					hash,
					0) != 0));
	return global ? SettleResult::GlobalDuplicate : SettleResult::CoveredByRun;
}

void Pipeline::settleServerId(MsgId oldId, const FullMsgId &newId) {
	for (auto i = 0; i != _n; ++i) {
		const auto &item = _items[i];
		if (item.localMsgId.msg == oldId
			&& item.localMsgId.peer == newId.peer) {
			settleItem(i, SettleResult::Created, newId);
		}
	}
}

void Pipeline::settleAllCancelled() {
	auto indexes = std::vector<int>();
	for (auto i = 0; i != _n; ++i) {
		if (_itemReport.contains(i)) {
			indexes.push_back(i);
		}
	}
	for (const auto i : indexes) {
		settleItem(
			i,
			_items[i].dedupSkipped
				? SettleResult::CoveredByRun
				: SettleResult::Cancelled);
	}
}

void Pipeline::dropSettling() {
	if (_settleLifetime) {
		_settleLifetime->destroy();
		_settleLifetime = nullptr;
	}
}

void Pipeline::armSettleWatchdog() {
	const auto weak = std::weak_ptr<Pipeline>(shared_from_this());
	base::call_delayed(kSettleWaitMax, [weak] {
		const auto strong = weak.lock();
		if (!strong) {
			return;
		}
		auto pending = false;
		auto working = false;
		for (auto i = 0; i != strong->_n; ++i) {
			if (!strong->_itemReport.contains(i)) {
				continue;
			}
			pending = true;
			if (!strong->_items[i].sent) {
				working = true;
				break;
			}
		}
		if (!pending) {
			return;
		}
		if (working || isPaused(strong->_peerId)) {
			strong->armSettleWatchdog();
			return;
		}
		strong->settleAllCancelled();
	});
}

void Pipeline::dedupCheckItem(int i) {
	const auto self = shared_from_this();
	auto &item = _items[i];
	if (item.dedupSkipped || item.uploadDone || item.dedupHashPending) return;
	// The precheck queue already hashed this item and registered it (or
	// marked it as a duplicate) before the download started.
	if (item.dedupPrechecked) {
		pumpUploads();
		return;
	}
	if (!item.downloadDone || item.path.isEmpty()) {
		return;
	}
	if (!item.fileHash.isEmpty() && item.fileSize > 0 && item.mediaId) {
		// The content hash is already known: taken from the server before
		// the download, or restored with the resume record. Nothing has to be
		// read from the file, only the duplicate decision has to run.
		item.dedupNeedsHash = false;
		item.dedupHashPending = true;
		const auto size = item.fileSize;
		const auto mediaId = item.mediaId;
		const auto needsDelete = item.path.startsWith(_downloadPath);
		auto hash = item.fileHash;
		const auto weak = std::weak_ptr<Pipeline>(self);
		crl::on_main([weak, i, hash, size, mediaId, needsDelete]() mutable {
			if (const auto alive = weak.lock()) {
				alive->dedupHashed(
					i,
					std::move(hash),
					size,
					mediaId,
					needsDelete);
			}
		});
		return;
	}
	item.dedupNeedsHash = false;
	item.dedupHashPending = true;

	const auto path = item.path;
	const auto mediaId = item.mediaId;
	const auto needsDelete = item.path.startsWith(_downloadPath);
	auto docSize = int64(0);
	auto asPhoto = false;
	const auto srcItem = _session.data().message(item.sourceId);
	const auto media = srcItem ? srcItem->media() : nullptr;
	if (const auto photo = media ? media->photo() : nullptr) {
		asPhoto = true;
	} else if (const auto doc = media ? media->document() : nullptr) {
		docSize = int64(doc->size);
	} else {
		item.dedupHashPending = false;
		return;
	}

	const auto weak = std::weak_ptr<Pipeline>(self);
	crl::async([=]() mutable {
		QByteArray hash;
		qint64 size = 0;
		if (asPhoto) {
			QFile f(path);
			if (f.open(QIODevice::ReadOnly)) {
				const auto content = f.readAll();
				if (!content.isEmpty()) {
					hash = Data::ContentFingerprint(content);
					size = content.size();
				}
			}
		} else {
			hash = Data::FileFingerprint(path, docSize);
			size = docSize;
		}
		crl::on_main([=, hash = std::move(hash)]() mutable {
			if (const auto alive = weak.lock()) {
				alive->dedupHashed(
					i,
					std::move(hash),
					size,
					mediaId,
					needsDelete);
			}
		});
	});
}

void Pipeline::dedupHashed(
		int i,
		QByteArray &&hash,
		qint64 size,
		uint64 mediaId,
		bool needsDelete) {
	if (i < 0 || i >= _n) return;
	auto &item = _items[i];
	item.dedupHashPending = false;
	item.fileHash = hash;
	item.fileSize = size;
	if (hash.isEmpty() || !mediaId || item.uploadDone || item.dedupSkipped) {
		pumpUploads();
		return;
	}
	auto &dedupDb = Core::App().downloadManager().ensureDedupDb();
	if (!dedupDb.isOpen()) {
		pumpUploads();
		return;
	}
	// The seek runs before this id is registered, so it can never match
	// itself: any hit is a genuinely different source doc with the same
	// content. A previous 'f' row of this exact id is caught by selfDone,
	// unless this very run put it there for another destination: a fan-out
	// copy of that content is expected, not a duplicate.
	const auto dedupSetting = GetEnhancedBool("prevent_forward_duplicates");
	const auto dedupOn = !_destinationOnly && dedupSetting;
	const auto sessionId = _session.uniqueId();
	const auto selfDone = (!seenInRun(mediaId, item.isPhoto)
			&& dedupOn
			&& dedupDb.hashForDocId(
				Data::DedupDb::Table::Uploads,
				mediaId,
				item.isPhoto) == hash)
		|| (dedupDb.hashForFwTmpItem(
			sessionId,
			_peerId,
			mediaId,
			item.isPhoto) == hash);
	const auto duplicate = (dedupSetting && seenInBatch(hash))
		|| dedupDb.chatTableContainsHash(
			_peerId,
			hash,
			dedupOn)
		|| (dedupDb.isOpen()
			&& dedupDb.containsFwTmpHash(sessionId, _peerId, hash))
		|| (dedupOn
			&& !seenInRun(hash)
			&& dedupDb.seekDocumentId(
				Data::DedupDb::Table::Uploads,
				hash,
				0) != 0);
	if (duplicate || selfDone) {
		if (needsDelete) {
			RemoveEfTempFile(item.path);
		}
		skipAsDuplicate(i);
		return;
	}
	markSeen(mediaId, item.isPhoto);
	markSeen(hash);
	dedupDb.addPending(
		Data::DedupDb::Table::Uploads,
		mediaId,
		hash,
		item.isPhoto);
	pumpUploads();
}

void Pipeline::premarkDuplicate(int i) {
	auto &item = _items[i];
	if (item.dedupSkipped) {
		return;
	}
	if (item.fileSize <= 0) {
		if (const auto srcItem = _session.data().message(item.sourceId)) {
			if (const auto media = srcItem->media()) {
				if (const auto doc = media->document()) {
					item.fileSize = doc->size;
				} else if (const auto photo = media->photo()) {
					item.fileSize = PhotoFingerprintSize(not_null{ photo });
				}
			}
		}
	}
	item.dedupSkipped = true;
	item.downloadDone = true;
	item.uploadDone = true;
	item.sent = true;
	_skippedCount++;
	adjustAlbumCount(i);
	if (const auto srcItem = _session.data().message(item.sourceId)) {
		Core::App().downloadManager().removeLoading(srcItem);
	}
	auto &states = ActiveStates();
	const auto sit = states.find(_peerId);
	if (sit != states.end() && i >= 0 && i < int(sit->second.items.size())) {
		sit->second.items[i].dedupSkipped = true;
		sit->second.items[i].sent = true;
	}
	EnhancedForward::markItemSkipped(&_session, _peerId);
	// Attach this doc id to the existing content hash so a future forward of
	// the exact same item is caught by the O(1) id lookup without hashing.
	// Persisted only when the matched content is finished: an alias based on
	// an in-flight row would outlive that row's cancel and block this
	// content forever with nothing actually uploaded.
	if (item.mediaId && !item.fileHash.isEmpty()) {
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			if (!_destinationOnly
				&& GetEnhancedBool("prevent_forward_duplicates")) {
				if (db.containsFinishedHash(
					Data::DedupDb::Table::Uploads,
					item.fileHash)) {
					db.insert(Data::DedupDb::Table::Uploads, {
						.hash = item.fileHash,
						.documentId = item.mediaId,
						.isPhoto = item.isPhoto,
						.status = u"f"_q,
					});
				}
			}
			db.insertFwTmp(
				_session.uniqueId(),
				_peerId,
				item.mediaId,
				item.isPhoto,
				item.fileHash);
		}
	}
	settleItem(i, classifySkip(item));
	saveProgress();
}

void Pipeline::startSession() {
	_sessionStarted = true;
	_sessionItemCount = _n;
	const auto self = shared_from_this();
	std::vector<FullMsgId> sourceIds;
	sourceIds.reserve(_n);
	for (auto i = 0; i < _n; i++) {
		sourceIds.push_back(_items[i].sourceId);
	}
	EnhancedForward::startForwardSession(
		&_session,
		_peerId,
		_srcPeer,
		sourceIds,
		[self] { self->saveProgress(); });
	auto &states = ActiveStates();
	const auto sit = states.find(_peerId);
	if (sit != states.end()) {
		auto &state = sit->second;
		_stateGeneration = state.generation;
		state.bulkOwned = _bulkOwned;
		auto restoredSent = 0;
		for (auto i = 0; i < _n; i++) {
			const auto tracked = (i < int(state.items.size()));
			if (tracked && _items[i].sent && !_items[i].dedupSkipped) {
				++restoredSent;
				state.items[i].sent = true;
				state.items[i].state = ItemState::Done;
				state.items[i].downloadProgress = 1.0;
				state.items[i].uploadProgress = 1.0;
				state.items[i].progress = 1.0;
			}
			if (tracked && _items[i].dedupSkipped) {
				state.items[i].dedupSkipped = true;
				state.items[i].sent = true;
			}
		}
		state.total = 0;
		state.skipped = 0;
		state.sent = restoredSent;
		for (auto i = 0; i < _n; i++) {
			if (_items[i].dedupSkipped || _items[i].cancelled) {
				state.skipped++;
			} else {
				state.total++;
			}
		}
		if (state.skipped > 0) {
			// Duplicates found before the session existed could not report
			// themselves: the shared batch counters drive the bar and the
			// final toast, so pass them the whole pre-session amount once.
			EnhancedForward::ReportForwardBatchItem(
				&_session,
				_peerId,
				0,
				state.skipped);
		}
		EnhancedForward::NotifyStateChanged(_peerId);
		EnhancedForward::NotifyCounterChanged();
		saveProgress();
		// If everything was dedup-skipped or cancelled up front there is
		// nothing to send: finish right away so the job doesn't linger as
		// "active", which would block new forwards (they'd be queued) and
		// trigger the quit "unfinished forward" prompt.
		if (state.total == 0) {
			EnhancedForward::finishJob(&_session, _peerId);
			// Nothing was downloaded, but the temp folder was created during
			// setup - remove it so an all-duplicates forward leaves no trace.
			const auto session = &_session;
			const auto path = _downloadPath;
			crl::on_main([=] {
				CleanupLeftoverForwardFiles(session, path, true);
			});
			return;
		}
	}
	// The pause/resume/cancel callbacks must be installed only after the
	// ActiveStates entry exists (startForwardSession above), otherwise the
	// setters bail out early and Pause never stops the in-flight transfers.
	setupCallbacks();
	pumpDownloads();
	pumpUploads();
	sendNext();
}

void Pipeline::appendItems(
		std::vector<not_null<HistoryItem*>> &&items,
		std::shared_ptr<Data::ForwardRunSeen> runSeen,
		SettledCallback settled,
		bool destinationOnlyDedup,
		ItemSettledCallback itemSettled) {
	if (items.empty()) {
		if (settled) {
			settled(std::vector<SettledItem>());
		}
		return;
	}
	if (runSeen) {
		_runSeen = std::move(runSeen);
	}
	_destinationOnly = _destinationOnly || destinationOnlyDedup;
	auto knownSources = base::flat_set<FullMsgId>();
	knownSources.reserve(_items.size() + items.size());
	for (const auto &existing : _items) {
		knownSources.emplace(existing.sourceId);
	}
	auto duplicates = std::vector<int>();
	for (const auto &srcItem : items) {
		const auto sourceId = srcItem->fullId();
		if (_session.data().message(sourceId) != srcItem) {
			continue;
		}
		const auto media = srcItem->media();
		const auto duplicate = !knownSources.emplace(sourceId).second
			&& media
			&& (media->document() || media->photo());
		ItemTask it;
		it.sourceId = sourceId;
		if (duplicate) {
			// This pipeline already owns the source media for the same
			// destination, so the repeat is an in-flight duplicate in both
			// dedup modes. Settle it without another download or upload.
			if (!media) {
				it.textOnly = true;
			} else if (const auto doc = media->document()) {
				it.mediaId = uint64(doc->id);
				it.fileSize = doc->size;
			} else if (const auto photo = media->photo()) {
				it.isPhoto = true;
				it.mediaId = uint64(photo->id);
				it.fileSize = PhotoFingerprintSize(not_null{ photo });
			} else {
				it.textOnly = true;
			}
			it.dedupSkipped = true;
			it.downloadDone = true;
			it.uploadDone = true;
			it.sent = true;
			++_skippedCount;
			duplicates.push_back(int(_items.size()));
			_items.push_back(std::move(it));
			continue;
		}
		if (!media) {
			it.textOnly = true;
		} else if (auto doc = media->document()) {
			it.mediaId = uint64(doc->id);
			const auto localPath = doc->filepath(true);
			if (!localPath.isEmpty()) {
				it.path = localPath;
				it.downloadDone = true;
			} else {
				auto name = doc->filename();
				if (name.isEmpty()) {
					name = u"file"_q;
				}
				name.replace(QRegularExpression("[:<>\"\\\\|?*]"), "_");
				it.path = QDir(_downloadPath).absoluteFilePath(
					QString::number(_runId) + u"_"_q
					+ QString::number(_items.size()) + u"_"_q + name);
				QDir().mkpath(QFileInfo(it.path).absolutePath());
				it.needsDownload = true;
			}
		} else if (auto photo = media->photo()) {
			it.isPhoto = true;
			it.mediaId = uint64(photo->id);
			it.path = QDir(_downloadPath).absoluteFilePath(
				QString::number(_runId) + u"_"_q
				+ QString::number(_items.size()) + u"_"_q
				+ QString::number(photo->id) + u".jpg"_q);
			QDir().mkpath(QFileInfo(it.path).absolutePath());
			it.needsDownload = true;
		} else {
			it.textOnly = true;
		}
		_items.push_back(std::move(it));
	}
	const auto oldN = _n;
	_n = int(_items.size());
	const auto hasReport = (settled != nullptr);
	if (settled) {
		auto indexes = std::vector<int>();
		indexes.reserve(_n - oldN);
		for (auto i = oldN; i != _n; ++i) {
			indexes.push_back(i);
		}
		addReport(
			std::move(indexes),
			std::move(settled),
			std::move(itemSettled));
	}
	// Extend the visible job state so the Forwards tab and the counters see
	// the newly added items right away and keep them after the current item.
	if (const auto j = ActiveStates().find(_peerId); j != end(ActiveStates())) {
		auto &state = j->second;
		for (auto i = oldN; i < _n; i++) {
			state.sourceIds.push_back(_items[i].sourceId);
			auto tracked = TrackedItem();
			if (_items[i].dedupSkipped) {
				tracked.dedupSkipped = true;
				tracked.sent = true;
			}
			if (const auto srcItem = _session.data().message(_items[i].sourceId)) {
				if (const auto media = srcItem->media()) {
					if (const auto doc = media->document()) {
						tracked.info.name = doc->filename();
						tracked.info.size = doc->size;
					} else if (const auto photo = media->photo()) {
						tracked.info.name = QString::number(photo->id)
							+ u".jpg"_q;
					}
				}
			}
			state.items.push_back(tracked);
			state.total++;
		}
	}
	// Files that were already on disk when the batch was appended did not go
	// through the constructor precheck, so hash them before the upload starts.
	for (auto i = oldN; i < _n; i++) {
		if (_items[i].downloadDone && !_items[i].path.isEmpty()) {
			dedupCheckItem(i);
		}
	}
	if (!duplicates.empty()) {
		// The batch report callbacks may append more items to this very
		// pipeline, so settle the duplicates on the next event loop turn
		// instead of re-entering appendItems from inside this call. The
		// pipeline is kept alive for that single turn: the bulk report it
		// carries must fire even if the last owning reference is dropped
		// before the deferred call runs.
		const auto self = shared_from_this();
		crl::on_main([self, indexes = std::move(duplicates), hasReport]() mutable {
			self->settleAppendedDuplicates(std::move(indexes), hasReport);
		});
	}
	NotifyStateChanged(_peerId);
	NotifyCounterChanged();
	if (!EnhancedForward::isPaused(_peerId)) {
		pumpDownloads();
		pumpUploads();
		sendNext();
	}
}

void Pipeline::settleAppendedDuplicates(
		std::vector<int> indexes,
		bool hasReport) {
	for (const auto i : indexes) {
		if (i < 0 || i >= _n || !_items[i].dedupSkipped) {
			continue;
		}
		if (hasReport && !_itemReport.contains(i)) {
			// A cancel settled this copy before the deferred turn.
			continue;
		}
		if (_sessionStarted && i >= _sessionItemCount) {
			// Copies appended before the session started were already counted
			// by startSession(), which reports the whole pre-session skipped
			// amount once. The generation check keeps a late callback from
			// touching the state of a newer batch to the same peer after this
			// pipeline has already finished.
			const auto it = ActiveStates().find(_peerId);
			if (it != end(ActiveStates())
				&& it->second.generation == _stateGeneration) {
				markItemSkipped(&_session, _peerId);
			}
		}
		settleItem(i, SettleResult::CoveredByRun);
	}
}

void Pipeline::runNextPrecheck() {
	if (_dedupPrecheckQueue.empty()) {
		startSession();
		return;
	}
	const auto i = _dedupPrecheckQueue.front();
	_dedupPrecheckQueue.pop_front();
	auto &item = _items[i];
	if (item.dedupSkipped || item.downloadDone || item.mediaId == 0) {
		runNextPrecheck();
		return;
	}
	// ID fast path: the exact same source media was already forwarded (or is
	// currently being forwarded), so skip it without computing a hash.
	auto &dedupDb = Core::App().downloadManager().ensureDedupDb();
	const auto dedupSetting = GetEnhancedBool("prevent_forward_duplicates");
	const auto dedupOn = !_destinationOnly && dedupSetting;
	const auto knownById = dedupDb.isOpen()
		&& (dedupDb.chatTableContainsDocId(
				_peerId,
				item.mediaId,
				item.isPhoto,
				dedupOn)
			|| dedupDb.containsFwTmpItem(
				_session.uniqueId(),
				_peerId,
				item.mediaId,
				item.isPhoto)
			|| (dedupSetting && seenInBatch(item.mediaId, item.isPhoto))
			|| (dedupOn
				&& !seenInRun(item.mediaId, item.isPhoto)
				&& dedupDb.containsDocId(
					Data::DedupDb::Table::Uploads,
					item.mediaId,
					item.isPhoto)));
	if (knownById) {
		premarkDuplicate(i);
		runNextPrecheck();
		return;
	}
	const auto srcItem = _session.data().message(item.sourceId);
	const auto media = srcItem ? srcItem->media() : nullptr;
	const auto doc = media ? media->document() : nullptr;
	const auto photo = media ? media->photo() : nullptr;
	if (!doc && !photo) {
		runNextPrecheck();
		return;
	}
	const auto size = doc
		? qint64(doc->size)
		: PhotoFingerprintSize(not_null{ photo });
	const auto self = shared_from_this();
	auto done = [self, i, size](QByteArray hash) {
		if (const auto alive = self.get()) {
			alive->remotePrechecked(i, std::move(hash), size);
		}
	};
	if (doc) {
		Data::RemoteFileFingerprint(&_session, doc, std::move(done));
	} else {
		Data::RemotePhotoFingerprint(
			&_session,
			not_null{ photo },
			std::move(done));
	}
}

void Pipeline::remotePrechecked(int i, QByteArray &&hash, qint64 size) {
	if (i < 0 || i >= _n) {
		runNextPrecheck();
		return;
	}
	auto &item = _items[i];
	if (hash.isEmpty() || item.dedupSkipped || item.downloadDone) {
		// A photo can fail its fingerprint synchronously (a missing or
		// invalid size location), so continue on the event loop instead of
		// recursing once for every such item in the queue.
		const auto weak = std::weak_ptr<Pipeline>(shared_from_this());
		crl::on_main([weak] {
			if (const auto strong = weak.lock()) {
				strong->runNextPrecheck();
			}
		});
		return;
	}
	item.fileHash = hash;
	item.fileSize = size;
	item.dedupPrechecked = true;
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		runNextPrecheck();
		return;
	}
	// It reached here only if its own id was NOT registered (the id fast path
	// above already skips it otherwise), so a content-hash match means a
	// different source doc with the same content was forwarded before: skip it
	// and attach this id to the existing hash for future id-only dedup.
	const auto dedupSetting = GetEnhancedBool("prevent_forward_duplicates");
	const auto dedupOn = !_destinationOnly && dedupSetting;
	const auto sessionId = _session.uniqueId();
	const auto duplicate = (dedupSetting && seenInBatch(hash))
		|| db.chatTableContainsHash(_peerId, hash, dedupOn)
		|| (db.isOpen()
			&& db.containsFwTmpHash(sessionId, _peerId, hash))
		|| (dedupOn
			&& !seenInRun(hash)
			&& db.seekDocumentId(
				Data::DedupDb::Table::Uploads,
				hash,
				0) != 0);
	if (duplicate) {
		premarkDuplicate(i);
	} else if (item.mediaId) {
		markSeen(item.mediaId, item.isPhoto);
		markSeen(hash);
		db.addPending(
			Data::DedupDb::Table::Uploads,
			item.mediaId,
			hash,
			item.isPhoto);
		item.dedupNeedsHash = false;
	}
	runNextPrecheck();
}

void Pipeline::skipAsDuplicate(int i) {
	const auto self = shared_from_this();
	auto &item = _items[i];
	item.dedupSkipped = true;
	item.downloadDone = true;
	item.uploadDone = true;
	item.sent = true;
	_downloadInFlight = false;
	_uploadInFlight = false;
	_skippedCount++;
	Core::App().downloadManager().reportDuplicateSkipped(
		Data::DedupDb::Table::Uploads,
		1);
	adjustAlbumCount(i);
	if (const auto srcItem = _session.data().message(item.sourceId)) {
		Core::App().downloadManager().removeLoading(srcItem);
	}
	refreshSourceItemState(i);
	if (item.mediaId && !item.fileHash.isEmpty()) {
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			if (!_destinationOnly
				&& GetEnhancedBool("prevent_forward_duplicates")) {
				if (db.containsFinishedHash(
					Data::DedupDb::Table::Uploads,
					item.fileHash)) {
					// Same as downloads: keep the new id -> same content mapping so
					// a future forward of this exact media is skipped O(1). Finished
					// match only - see premarkDuplicate().
					db.insert(Data::DedupDb::Table::Uploads, {
						.hash = item.fileHash,
						.documentId = item.mediaId,
						.isPhoto = item.isPhoto,
						.status = u"f"_q,
					});
				}
			}
			db.insertFwTmp(
				_session.uniqueId(),
				_peerId,
				item.mediaId,
				item.isPhoto,
				item.fileHash);
		}
	}
	auto &states = ActiveStates();
	const auto sit = states.find(_peerId);
	if (sit != states.end() && i >= 0 && i < int(sit->second.items.size())) {
		sit->second.items[i].dedupSkipped = true;
		sit->second.items[i].sent = true;
	}
	EnhancedForward::markItemSkipped(&_session, _peerId);
	settleItem(i, classifySkip(item));
	saveProgress();
	sendNext();
	pumpUploads();
	pumpDownloads();
}

void Pipeline::adjustAlbumCount(int idx) {
	if (idx < 0 || idx >= _n) return;
	const auto &item = _items[idx];
	if (!item.sourceGroup) return;
	const auto albumIt = _albums.find(item.sourceGroup);
	if (albumIt != _albums.end() && albumIt->second->expectedCount > 0) {
		albumIt->second->expectedCount--;
	}
}

void Pipeline::advanceOrderGate() {
	auto first = MsgId(0);
	auto second = MsgId(0);
	for (const auto &item : _items) {
		if (OrderSettled(item)) {
			continue;
		}
		const auto id = item.sourceId.msg;
		if (!first || id < first) {
			second = first;
			first = id;
		} else if (!second || id < second) {
			second = id;
		}
	}
	if (!first) {
		NormalForward::AdvanceGate(_api, _peerId, MsgId(0));
		return;
	}
	auto waiting = false;
	for (const auto &item : _items) {
		if (OrderSettled(item) || !item.sent || !item.sentItem) {
			continue;
		}
		const auto holds = (item.sourceId.msg == first);
		if (holds) {
			waiting = true;
		}
		RegisterOrderWait(
			&_session,
			_api,
			_peerId,
			item.sentItem->fullId(),
			holds ? second : first);
	}
	if (!waiting) {
		// Nothing at the cut is still waiting for its server id, so the gate
		// can move on to the next unsettled item right away.
		NormalForward::AdvanceGate(_api, _peerId, first);
	}
}

void Pipeline::cancelItem(int idx) {
	const auto self = shared_from_this();
	if (idx < 0 || idx >= _n) return;
	auto &item = _items[idx];
	if (item.cancelled || item.sent) return;
	item.cancelled = true;
	const auto srcItem = _session.data().message(item.sourceId);
	const auto media = srcItem ? srcItem->media() : nullptr;
	if (!item.downloadDone && srcItem) {
		if (const auto doc = media ? media->document() : nullptr) {
			doc->cancel();
		} else if (const auto photo = media ? media->photo() : nullptr) {
			photo->cancel();
		}
	}
	if (item.uploadId != FullMsgId()) {
		_session.uploader().cancel(item.uploadId);
		_uploadIndex->erase(item.uploadId);
	}
	if (const auto srcItem = _session.data().message(item.sourceId)) {
		ClearShadowUpload(&_session, srcItem);
	}
	if (!item.uploadDone && item.mediaId) {
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			db.removePending(
				Data::DedupDb::Table::Uploads,
				item.mediaId,
				item.isPhoto);
		}
	}
	{
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()) {
			const auto jobId = u"ef_%1_%2"_q.arg(_srcPeer.value).arg(
				_peerId.value);
			db.removeEfResumeItem(jobId, idx);
		}
	}
	if (!item.path.isEmpty() && item.path.startsWith(_downloadPath)) {
		RemoveEfTempFile(item.path);
	}
	if (srcItem) {
		Core::App().downloadManager().removeLoading(srcItem);
	}
	adjustAlbumCount(idx);
	refreshSourceItemState(idx);

	item.downloadDone = true;
	item.uploadDone = true;
	item.sent = true;
	if (_downloadCursor > 0 && _downloadCursor - 1 == idx) {
		_downloadInFlight = false;
	}
	if (_uploadCursor > 0 && _uploadCursor - 1 == idx) {
		_uploadInFlight = false;
	}

	auto &states = ActiveStates();
	const auto stateIt = states.find(_peerId);
	if (stateIt != states.end()
		&& idx >= 0 && idx < int(stateIt->second.items.size())) {
		auto &tracked = stateIt->second.items[idx];
		tracked.cancelled = true;
		tracked.state = ItemState::Done;
		tracked.downloadProgress = 1.0;
		tracked.uploadProgress = 1.0;
		tracked.progress = 1.0;
	}

	EnhancedForward::markItemSkipped(&_session, _peerId);
	settleItem(idx, SettleResult::Cancelled);
	if (!isCancelled(_peerId)) {
		// A whole-batch cancel clears the single resume row once instead of
		// rewriting the remaining plan for every cancelled item.
		saveProgress();
	}
	sendNext();
	pumpUploads();
	pumpDownloads();
}

namespace {

struct ProtectedMediaState {
	base::flat_set<MsgId> requested;
	std::vector<MsgId> pending;
	int retries = 0;
	bool inFlight = false;
};

auto ProtectedMediaStates()
-> base::flat_map<
	not_null<Main::Session*>,
	base::flat_map<PeerId, ProtectedMediaState>> & {
	static auto result = base::flat_map<
		not_null<Main::Session*>,
		base::flat_map<PeerId, ProtectedMediaState>>();
	return result;
}

void ForgetProtectedMediaSession(not_null<Main::Session*> session) {
	static base::flat_set<not_null<Main::Session*>> tracked;
	if (!tracked.emplace(session).second) {
		return;
	}
	session->lifetime().add([session] {
		tracked.remove(session);
		ProtectedMediaStates().erase(session);
	});
}

bool IsProtectedMediaCandidate(not_null<HistoryItem*> item) {
	// The media being in place is what says the message was already revealed.
	// The noforwards flag never goes away for these messages, so without this
	// check every view update re-requests every visible restricted message
	// again - hundreds of requests a minute, which is what makes the server
	// start failing unrelated requests such as forwards.
	return IsServerMsgId(item->id)
		&& !item->out()
		&& !item->isService()
		&& !item->media()
		&& ((item->flags() & MessageFlag::NoForwards) != MessageFlags());
}

constexpr auto kMaxProtectedMediaRetries = 3;
constexpr auto kProtectedMediaRetryDelay = 3000;
constexpr auto kMaxProtectedMediaBatch = 10;

void SendProtectedMediaRequest(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		const QVector<MTPInputMessage> &ids);

void PumpProtectedMediaRequests(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer);

void ReleaseProtectedMediaIds(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		const QVector<MTPInputMessage> &ids,
		bool failed) {
	auto &sessions = ProtectedMediaStates();
	const auto sessionIt = sessions.find(session);
	if (sessionIt == end(sessions)) {
		return;
	}
	auto &states = sessionIt->second;
	const auto state = states.find(peer->id);
	if (state == end(states)) {
		return;
	}
	state->second.inFlight = false;
	if (failed && state->second.retries < kMaxProtectedMediaRetries) {
		++state->second.retries;
		const auto retry = [=] {
			auto &retrying = ProtectedMediaStates();
			const auto sessionState = retrying.find(session);
			if (sessionState == end(retrying)) {
				return;
			}
			const auto peerState = sessionState->second.find(peer->id);
			if (peerState == end(sessionState->second)) {
				return;
			}
			if (peerState->second.inFlight) {
				// Another request is running: queue these again instead of
				// dropping them, they are retried once it finishes.
				for (const auto &id : ids) {
					const auto msgId = MsgId(id.c_inputMessageID().vid().v);
					if (!peerState->second.requested.contains(msgId)) {
						peerState->second.pending.push_back(msgId);
					}
				}
				return;
			}
			peerState->second.inFlight = true;
			for (const auto &id : ids) {
				peerState->second.requested.emplace(
					MsgId(id.c_inputMessageID().vid().v));
			}
			SendProtectedMediaRequest(session, peer, ids);
		};
		QTimer::singleShot(kProtectedMediaRetryDelay, retry);
		return;
	}
	if (!failed) {
		state->second.retries = 0;
	}
	for (const auto &id : ids) {
		state->second.requested.remove(
			MsgId(id.c_inputMessageID().vid().v));
	}
	PumpProtectedMediaRequests(session, peer);
}

void ApplyRevealedMessages(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		const MTPmessages_Messages &result,
		const QVector<MTPInputMessage> &ids) {
	session->data().processExistingMessages(
		peer->isChannel() ? peer->asChannel() : nullptr,
		result);
	ApplyRevealedMessagesTo(session, peer->id, result);
	ReleaseProtectedMediaIds(session, peer, ids, false);
}

void SendProtectedMediaRequest(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		const QVector<MTPInputMessage> &ids) {
	session->api().ensureTakeout(peer, [=](bool ready) {
		const auto id = session->api().takeoutId();
		if (!ready || !id) {
			ReleaseProtectedMediaIds(session, peer, ids, true);
			return;
		}
		session->api().request(
			MTPInvokeWithTakeout<MTPchannels_GetMessages>(
				MTP_long(*id),
				MTPchannels_GetMessages(
					MTP_inputChannel(
						MTP_long(peerToChannel(peer->id).bare),
						MTP_long(peer->asChannel()->accessHash())),
					MTP_vector<MTPInputMessage>(ids)))
		).done([=](const MTPmessages_Messages &result) {
			ApplyRevealedMessages(session, peer, result, ids);
		}).fail([=](const MTP::Error &) {
			ReleaseProtectedMediaIds(session, peer, ids, true);
		}).send();
	});
}

void PumpProtectedMediaRequests(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer) {
	auto &sessions = ProtectedMediaStates();
	const auto sessionIt = sessions.find(session);
	if (sessionIt == end(sessions)) {
		return;
	}
	auto &states = sessionIt->second;
	const auto state = states.find(peer->id);
	if (state == end(states) || state->second.inFlight) {
		return;
	}
	auto &pending = state->second.pending;
	if (pending.empty()) {
		return;
	}
	const auto count = std::min(int(pending.size()), kMaxProtectedMediaBatch);
	auto batch = QVector<MTPInputMessage>();
	batch.reserve(count);
	for (auto i = 0; i != count; ++i) {
		const auto id = pending.front();
		pending.erase(pending.begin());
		state->second.requested.emplace(id);
		batch.push_back(MTP_inputMessageID(MTP_int(id.bare)));
	}
	state->second.inFlight = true;
	SendProtectedMediaRequest(session, peer, batch);
}

} // namespace

void RevealProtectedMediaItem(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item) {
	if (!IsProtectedMediaCandidate(item)) {
		return;
	}
	const auto peer = item->history()->peer;
	if (!peerIsChannel(peer->id)) {
		return;
	}
	ForgetProtectedMediaSession(session);
	auto &state = ProtectedMediaStates()[session][peer->id];
	if (state.requested.contains(item->id)
		|| std::find(state.pending.begin(), state.pending.end(), item->id)
			!= state.pending.end()) {
		return;
	}
	// Queue it instead of dropping it: several restricted messages become
	// visible at once while the first request is still in flight, and a
	// dropped candidate is only retried on the next geometry update.
	state.pending.push_back(item->id);
	PumpProtectedMediaRequests(session, peer);
}

} // namespace EnhancedForward

namespace NormalForward {
namespace {

constexpr auto kFingerprintConcurrencyBase = 50;
constexpr auto kFingerprintConcurrencyMax = 100;
constexpr auto kMaxFingerprintRetries = 3;
constexpr auto kGeneralId = Data::ForumTopic::kGeneralId;

rpl::event_stream<> CountersChanged;

void Notify() {
	CountersChanged.fire({});
}

struct Registration {
	uint64 mediaId = 0;
	bool isPhoto = false;
	QByteArray hash;
};

class Job final : public std::enable_shared_from_this<Job> {
public:
	enum class State {
		Running,
		FloodWait,
		Paused,
		Done,
		Cancelled,
	};

	Job(
		not_null<Main::Session*> session,
		ApiWrap *api,
		PeerId dst,
		PeerId src,
		Api::SendAction action,
		Data::ForwardOptions forwardOptions,
		Data::GroupingOptions groupOptions)
	: session(session)
	, api(api)
	, dst(dst)
	, src(src)
	, action(std::move(action))
	, forwardOptions(forwardOptions)
	, groupOptions(groupOptions) {
	}

	Main::Session *session = nullptr;
	ApiWrap *api = nullptr;

	PeerId dst = PeerId();
	PeerId src = PeerId();
	Api::SendAction action;
	Data::ForwardOptions forwardOptions = Data::ForwardOptions();
	Data::GroupingOptions groupOptions = Data::GroupingOptions();

	State state = State::Running;

	int total = 0;
	int done = 0;
	int skipped = 0;
	MsgId lastMsgId = 0;
	QString lastFileName;
	int64 startedAt = base::unixtime::now();
	int floodSeconds = 0;

	std::vector<MsgId> remaining;   // source ids not yet acked
	std::vector<MsgId> queue;       // natural singles, batchable
	std::vector<MessageGroupId> queueGroups; // source groupId per queue entry
	std::vector<MsgId> soloQueue;   // album members, 1 per request
	base::flat_map<MsgId, Registration> regs;
	base::flat_set<MsgId> awaitingHash;
	base::flat_map<MsgId, QByteArray> hashes;
	base::flat_map<MsgId, uint64> mediaIds;
	base::flat_map<MsgId, bool> mediaPhotos;
	base::flat_set<std::pair<uint64, bool>> batchMedia;
	base::flat_set<QByteArray> batchHashes;
	size_t scanned = 0;             // prepare cursor over remaining
	int inflightFetches = 0;
	int soloInFlight = 0;
	constexpr static auto kMaxSoloConcurrent = 10;
	bool batchInFlight = false;
	bool pumping = false;
	bool completionFired = false;
	base::flat_map<MsgId, int> fingerprintRetries;
	std::vector<MsgId> hashRetries;

	std::vector<mtpRequestId> requests;
	base::Timer floodTimer;
	base::Timer tickTimer;
	std::optional<TimeId> videoTimestamp;

	Fn<void()> completion;

	// The smallest restricted item that has not been sent yet: normal items at
	// or after it wait, so each restricted item keeps its place in the source
	// order. Album ids are contiguous, so cutting here never splits an album
	// unless the protected item is itself a member of one.
	MsgId gate = MsgId();

	[[nodiscard]] bool gateBlocksScan() const {
		return gate
			&& scanned < remaining.size()
			&& remaining[scanned] >= gate;
	}

	[[nodiscard]] int maxBatch() const {
		return Data::ForwardBatching::MaxBatchSize(groupOptions);
	}

	[[nodiscard]] bool alive() const {
		return state == State::Running || state == State::FloodWait;
	}
};

using JobsMap = base::flat_map<PeerId, std::shared_ptr<Job>>;

JobsMap &Jobs() {
	static JobsMap result;
	return result;
}

[[nodiscard]] std::shared_ptr<Job> HoldJob(not_null<Job*> j) {
	const auto it = Jobs().find(j->dst);
	return (it != end(Jobs()) && it->second.get() == j.get())
		? it->second
		: std::shared_ptr<Job>();
}

[[nodiscard]] bool BelongsTo(
		const Job &job,
		not_null<Main::Session*> session) {
	return job.session == session.get();
}

void FireCompletion(not_null<Job*> j) {
	if (!j->completionFired) {
		j->completionFired = true;
		if (j->completion) {
			j->completion();
		}
	}
}

[[nodiscard]] std::vector<MsgId> RemainingForResume(const Job &j) {
	auto result = std::vector<MsgId>();
	result.reserve(
		j.queue.size()
		+ j.soloQueue.size()
		+ j.regs.size()
		+ j.awaitingHash.size()
		+ j.hashRetries.size()
		+ (j.remaining.size() - std::min(j.scanned, j.remaining.size())));
	result.insert(end(result), begin(j.queue), end(j.queue));
	result.insert(end(result), begin(j.soloQueue), end(j.soloQueue));
	for (const auto &entry : j.regs) {
		result.push_back(entry.first);
	}
	result.insert(
		end(result),
		begin(j.awaitingHash),
		end(j.awaitingHash));
	result.insert(
		end(result),
		begin(j.hashRetries),
		end(j.hashRetries));
	if (j.scanned < j.remaining.size()) {
		result.insert(
			end(result),
			begin(j.remaining) + j.scanned,
			end(j.remaining));
	}
	std::sort(begin(result), end(result));
	result.erase(std::unique(begin(result), end(result)), end(result));
	return result;
}

void SaveSnapshot(not_null<Job*> j) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	auto record = Data::NfResumeRecord();
	record.sessionId = j->session->uniqueId();
	record.destPeerId = j->dst;
	record.srcPeerId = j->src;
	record.total = j->total;
	record.done = j->done;
	record.skipped = j->skipped;
	record.lastMsgId = j->lastMsgId;
	switch (j->state) {
	case Job::State::FloodWait: record.state = u"flood"_q; break;
	case Job::State::Paused: record.state = u"paused"_q; break;
	default: record.state = u"running"_q; break;
	}
	record.forwardOptions = static_cast<int>(j->forwardOptions);
	record.groupOptions = static_cast<int>(j->groupOptions);
	record.remaining = RemainingForResume(*j);
	db.insertNfResume(record);
}

void Pump(not_null<Job*> j);

void StopTimers(not_null<Job*> j) {
	j->floodTimer.cancel();
	j->tickTimer.cancel();
}

void Finish(not_null<Job*> j) {
	[[maybe_unused]] const auto held = HoldJob(j);
	j->state = Job::State::Done;
	for (const auto requestId : base::take(j->requests)) {
		j->api->request(requestId).cancel();
	}
	StopTimers(j);
	auto &db = Core::App().downloadManager().ensureDedupDb();
	for (const auto &[msgId, registration] : j->regs) {
		db.removePending(
			Data::DedupDb::Table::Uploads,
			registration.mediaId,
			registration.isPhoto);
	}
	j->regs.clear();
	db.removeNfResume(j->session->uniqueId(), j->dst);
	db.clearFwTmpRun(j->session->uniqueId(), j->dst);
	EnhancedForward::RecordFinishedFwRun(
		not_null{ j->session },
		j->dst,
		j->src,
		j->remaining.empty()
			? FullMsgId()
			: FullMsgId(j->src, j->remaining.front()),
		j->lastMsgId
			? FullMsgId(j->src, j->lastMsgId)
			: FullMsgId(),
		j->total,
		j->done,
		j->skipped,
		EnhancedForward::CurrentForwardActionId(
			not_null{ j->session },
			j->dst));
	EnhancedForward::FinishForwardBatchWorker(
		not_null{ j->session },
		j->dst,
		1);
	if (!EnhancedForward::ForwardBatchOwns(not_null{ j->session }, j->dst)
		&& (j->done > 0 || j->skipped > 0)) {
		EnhancedForward::ShowForwardDoneToast(j->done, j->total, j->skipped);
	}
	FireCompletion(j);
	Jobs().remove(j->dst);
	Notify();
}

void PauseJob(not_null<Job*> j) {
	if (j->state != Job::State::Running) {
		return;
	}
	j->state = Job::State::Paused;
	SaveSnapshot(j);
	FireCompletion(j);
	Notify();
}

void CancelJob(not_null<Job*> j) {
	[[maybe_unused]] const auto held = HoldJob(j);
	j->state = Job::State::Cancelled;
	EnhancedForward::DropForwardBatch(not_null{ j->session }, j->dst);
	for (const auto requestId : base::take(j->requests)) {
		j->api->request(requestId).cancel();
	}
	StopTimers(j);
	auto &db = Core::App().downloadManager().ensureDedupDb();
	for (const auto &[msgId, registration] : j->regs) {
		db.removePending(
			Data::DedupDb::Table::Uploads,
			registration.mediaId,
			registration.isPhoto);
	}
	j->regs.clear();
	db.removeNfResume(j->session->uniqueId(), j->dst);
	db.clearFwTmpRun(j->session->uniqueId(), j->dst);
	Jobs().remove(j->dst);
	FireCompletion(j);
	Notify();
}

void PauseJob(not_null<Job*> j);

void EnqueueNatural(not_null<Job*> j, MsgId msgId) {
	const auto item = j->session->data().message(FullMsgId(j->src, msgId));
	j->queue.push_back(msgId);
	j->queueGroups.push_back(
		item ? item->groupId() : MessageGroupId());
}

void EnqueueResolved(
		not_null<Job*> j,
		MsgId msgId,
		const QByteArray &hash) {
	j->awaitingHash.remove(msgId);
	auto &db = Core::App().downloadManager().ensureDedupDb();
	const auto dedupOn = GetEnhancedBool("prevent_forward_duplicates");
	const auto sessionId = j->session->uniqueId();
	const auto mediaIt = j->mediaIds.find(msgId);
	const auto mediaId = (mediaIt != end(j->mediaIds))
		? mediaIt->second
		: uint64(0);
	const auto photoIt = j->mediaPhotos.find(msgId);
	const auto isPhoto = (photoIt != end(j->mediaPhotos))
		&& photoIt->second;
	const auto batchDuplicate = mediaId
		&& (j->batchMedia.contains({ mediaId, isPhoto })
			|| (!hash.isEmpty() && j->batchHashes.contains(hash)));
	const auto duplicate = batchDuplicate
		|| (!hash.isEmpty()
			&& (db.chatTableContainsHash(j->dst, hash, dedupOn)
				|| (db.isOpen()
					&& db.containsFwTmpHash(
						sessionId,
						j->dst,
						hash))
				|| (dedupOn
					&& db.seekDocumentId(
						Data::DedupDb::Table::Uploads,
						hash) != 0)));
	if (duplicate) {
		j->skipped++;
		EnhancedForward::ReportForwardBatchItem(
			not_null{ j->session },
			j->dst,
			0,
			1);
		return;
	}
	if (mediaId) {
		j->batchMedia.insert({ mediaId, isPhoto });
		if (!hash.isEmpty()) {
			j->batchHashes.insert(hash);
		}
	}
	if (mediaId && !hash.isEmpty()) {
		db.addPending(
			Data::DedupDb::Table::Uploads,
			mediaId,
			hash,
			isPhoto);
		j->regs.emplace_or_assign(
			msgId,
			Registration{ mediaId, isPhoto, hash });
	}
	const auto item = j->session->data().message(
		FullMsgId(j->src, msgId));
	const auto isSolo = j->groupOptions == Data::GroupingOptions::Separate
		&& item
		&& item->groupId() != MessageGroupId();
	if (isSolo) {
		j->soloQueue.push_back(msgId);
	} else {
		EnqueueNatural(j, msgId);
	}
}

void StartFingerprint(
		not_null<Job*> j,
		MsgId msgId,
		not_null<DocumentData*> document) {
	auto &retries = j->fingerprintRetries[msgId];
	if (retries >= kMaxFingerprintRetries) {
		PauseJob(j);
		return;
	}
	retries++;
	j->inflightFetches++;
	j->awaitingHash.insert(msgId);
	const auto weak = j->weak_from_this();
	Data::RemoteFileFingerprint(j->session, document, [=](QByteArray hash) {
		if (const auto strong = weak.lock()) {
			strong->inflightFetches--;
			if (hash.isEmpty()) {
				strong->awaitingHash.remove(msgId);
				strong->hashRetries.push_back(msgId);
				PauseJob(strong.get());
				return;
			}
			strong->fingerprintRetries.remove(msgId);
			strong->hashes.emplace_or_assign(msgId, std::move(hash));
			EnqueueResolved(strong.get(), msgId, strong->hashes[msgId]);
			Pump(strong.get());
		}
	});
}

void StartPhotoFingerprint(
		not_null<Job*> j,
		MsgId msgId,
		not_null<PhotoData*> photo) {
	auto &retries = j->fingerprintRetries[msgId];
	if (retries >= kMaxFingerprintRetries) {
		PauseJob(j);
		return;
	}
	retries++;
	j->inflightFetches++;
	j->awaitingHash.insert(msgId);
	const auto weak = j->weak_from_this();
	Data::RemotePhotoFingerprint(j->session, photo, [=](QByteArray hash) {
		if (const auto strong = weak.lock()) {
			strong->inflightFetches--;
			if (hash.isEmpty()) {
				strong->awaitingHash.remove(msgId);
				strong->hashRetries.push_back(msgId);
				PauseJob(strong.get());
				return;
			}
			strong->fingerprintRetries.remove(msgId);
			strong->hashes.emplace_or_assign(msgId, std::move(hash));
			EnqueueResolved(strong.get(), msgId, strong->hashes[msgId]);
			Pump(strong.get());
		}
	});
}

[[nodiscard]] MTPInputMedia InputMediaForMedia(
		not_null<Data::Session*> data,
		const Data::Media *media) {
	if (const auto photo = media->photo()) {
		return MTP_inputMediaPhoto(
			MTP_flags(0),
			photo->mtpInput(),
			MTPint(),
			MTPInputDocument());
	} else if (const auto document = media->document()) {
		return MTP_inputMediaDocument(
			MTP_flags(0),
			document->mtpInput(),
			MTPInputPhoto(),
			MTPint(),
			MTPint(),
			MTPstring());
	}
	return MTP_inputMediaEmpty();
}

[[nodiscard]] QString DescribeAcks(const MTPUpdates &result) {
	auto line = QString();
	const auto addMessage = [&](const MTPMessage &message) {
		message.match([&](const MTPDmessage &data) {
			line += u" %1:g%2"_q.arg(data.vid().v).arg(
				data.vgrouped_id().value_or_empty());
		}, [&](const MTPDmessageEmpty &) {
			line += u" ?:e"_q;
		}, [&](const MTPDmessageService &data) {
			line += u" %1:s"_q.arg(data.vid().v);
		});
	};
	result.match([&](const MTPDupdates &data) {
		for (const auto &update : data.vupdates().v) {
			update.match([&](const MTPDupdateNewMessage &d) {
				addMessage(d.vmessage());
			}, [&](const MTPDupdateNewChannelMessage &d) {
				addMessage(d.vmessage());
			}, [&](const auto &) {});
		}
	}, [&](const MTPDupdatesCombined &data) {
		for (const auto &update : data.vupdates().v) {
			update.match([&](const MTPDupdateNewMessage &d) {
				addMessage(d.vmessage());
			}, [&](const MTPDupdateNewChannelMessage &d) {
				addMessage(d.vmessage());
			}, [&](const auto &) {});
		}
	}, [&](const auto &) {});
	return line;
}

void FlushForwardBatch(
		not_null<Job*> j,
		std::vector<MsgId> chunk) {
	Expects(!chunk.empty());

	const auto history = j->session->data().history(j->dst);
	const auto srcPeer = j->session->data().peer(j->src);

	auto idsVector = QVector<MTPint>();
	auto randoms = QVector<MTPlong>();
	idsVector.reserve(int(chunk.size()));
	randoms.reserve(int(chunk.size()));
	for (const auto msgId : chunk) {
		idsVector.push_back(MTP_int(msgId.bare));
		randoms.push_back(MTP_long(base::RandomValue<uint64>()));
	}

	const auto topicRootId = j->action.replyTo.topicRootId;
	const auto realTopMsgId = (topicRootId == kGeneralId)
		? MsgId(0)
		: topicRootId;
	using Flag = MTPmessages_ForwardMessages::Flag;
	auto approved = j->action.options.starsApproved;
	const auto starsPaid = std::min(
		history->peer->starsPerMessageChecked(),
		approved);
	if (starsPaid) {
		approved -= starsPaid;
		j->action.options.starsApproved = approved;
	}
	auto flags = Flag(0)
		| Flag::f_with_my_score
		| (ShouldSendSilent(history->peer, j->action.options)
			? Flag::f_silent
			: Flag(0))
		| (realTopMsgId ? Flag::f_top_msg_id : Flag(0))
		| ((j->action.options.sendAs)
			? Flag::f_send_as
			: Flag(0))
		| (j->forwardOptions != Data::ForwardOptions::PreserveInfo
			? Flag::f_drop_author
			: Flag(0))
		| (j->forwardOptions == Data::ForwardOptions::NoNamesAndCaptions
			? Flag::f_drop_media_captions
			: Flag(0))
		| (j->videoTimestamp.has_value()
			? Flag::f_video_timestamp
			: Flag(0))
		| (j->action.options.shortcutId
			? Flag::f_quick_reply_shortcut
			: Flag(0))
		| (j->action.options.scheduled
			? Flag::f_schedule_date
			: Flag(0))
		| ((j->action.options.scheduled
				&& j->action.options.scheduleRepeatPeriod)
			? Flag::f_schedule_repeat_period
			: Flag(0))
		| (starsPaid ? Flag::f_allow_paid_stars : Flag(0));

	const auto maxSourceId = chunk.back();

	const auto weak = j->weak_from_this();
	const auto requestId = j->api->request(
		MTPmessages_ForwardMessages(
			MTP_flags(flags),
			srcPeer->input(),
			MTP_vector<MTPint>(idsVector),
			MTP_vector<MTPlong>(randoms),
			history->peer->input(),
			MTP_int(realTopMsgId.bare),
			ReplyToForMTP(history, j->action.replyTo),
			MTP_int(j->action.options.scheduled),
			MTP_int(j->action.options.scheduleRepeatPeriod),
			(j->action.options.sendAs
				? j->action.options.sendAs->input()
				: MTP_inputPeerEmpty()),
			Data::ShortcutIdToMTP(
				j->session,
				j->action.options.shortcutId),
			MTP_long(j->action.options.effectId),
			MTP_int(j->videoTimestamp.value_or(0)),
			MTP_long(starsPaid),
			Api::SuggestToMTP(j->action.options.suggest))
	).done([=](
			const MTPUpdates &result) mutable {
		const auto strong = weak.lock();
		if (!strong) {
			return;
		}
		auto job = not_null{ strong.get() };
		auto &db = Core::App().downloadManager().ensureDedupDb();
		const auto sessionId = job->session->uniqueId();
		for (const auto msgId : chunk) {
			if (const auto it = job->regs.find(msgId); it != end(job->regs)) {
				if (db.isOpen() && !it->second.hash.isEmpty()) {
					db.insert(Data::DedupDb::Table::Uploads, {
						.hash = it->second.hash,
						.documentId = it->second.mediaId,
						.isPhoto = it->second.isPhoto,
						.status = u"f"_q,
					});
					db.removePending(
						Data::DedupDb::Table::Uploads,
						it->second.mediaId,
						it->second.isPhoto);
					db.insertFwTmp(
						sessionId,
						job->dst,
						it->second.mediaId,
						it->second.isPhoto,
						it->second.hash);
				}
				job->regs.erase(it);
			}
		}
		job->api->applyUpdates(result);
		job->done += int(chunk.size());
		EnhancedForward::ReportForwardBatchItem(
			not_null{ job->session },
			job->dst,
			int(chunk.size()),
			0);
		job->lastMsgId = std::max(job->lastMsgId, maxSourceId);
		for (const auto msgId : chunk) {
			const auto item = job->session->data().message(
				FullMsgId(job->src, msgId));
			const auto media = item ? item->media() : nullptr;
			if (!media) {
				continue;
			} else if (const auto doc = media->document()) {
				job->lastFileName = doc->filename();
			} else if (media->photo()) {
				job->lastFileName = u"photo_%1.jpg"_q.arg(msgId.bare);
			}
		}
		EnhancedForward::NoteForwardBatchFile(
			not_null{ job->session },
			job->dst,
			job->lastFileName);
		job->batchInFlight = false;
		if (job->soloInFlight > 0) {
			job->soloInFlight--;
		}
		job->session->data().sendHistoryChangeNotifications();
		SaveSnapshot(job);
		Notify();
		Pump(job);
	}).fail([=](
			const MTP::Error &error) mutable {
		const auto strong = weak.lock();
		if (!strong) {
			return;
		}
		auto job = not_null{ strong.get() };
		job->batchInFlight = false;
		for (auto it = chunk.rbegin(); it != chunk.rend(); ++it) {
			job->queue.insert(job->queue.begin(), *it);
			const auto item = job->session->data().message(
				FullMsgId(job->src, *it));
			job->queueGroups.insert(
				job->queueGroups.begin(),
				item ? item->groupId() : MessageGroupId());
		}
		const auto &type = error.type();
		if (type.startsWith(u"FLOOD_WAIT_"_q)) {
			job->floodSeconds = type.mid(u"FLOOD_WAIT_"_q.size()).toInt();
			job->state = Job::State::FloodWait;
			SaveSnapshot(job);
			Notify();
			j->floodTimer.setCallback([weak] {
				if (const auto strong = weak.lock()) {
					if (strong->state == Job::State::FloodWait) {
						strong->floodSeconds = 0;
						strong->tickTimer.cancel();
						strong->state = Job::State::Running;
						SaveSnapshot(strong.get());
						Notify();
						Pump(strong.get());
					}
				}
			});
			j->floodTimer.callOnce((job->floodSeconds + 1) * 1000);
			j->tickTimer.setCallback([weak] {
				if (const auto strong = weak.lock()) {
					if (strong->floodSeconds > 0) {
						strong->floodSeconds--;
						Notify();
					}
				}
			});
			j->tickTimer.callEach(1000);
			return;
		}
		j->api->sendMessageFail(error, history->peer);
		PauseJob(j);
	}).send();
	j->requests.push_back(requestId);
	j->batchInFlight = true;
}

void Pump(not_null<Job*> j) {
	if (j->state != Job::State::Running
		|| j->pumping
		|| j->batchInFlight) {
		return;
	}
	// Separate-mode solo sends (1 id each) are independent — no
	// batchInFlight serialization needed. Batched modes serialize.
	j->pumping = true;
	const auto guard = gsl::finally([&] { j->pumping = false; });

	auto &db = Core::App().downloadManager().ensureDedupDb();
	const auto dedupOn = GetEnhancedBool("prevent_forward_duplicates");
	const auto windowSize = j->maxBatch();
	// Adaptive concurrency: when both send queues are empty the pipeline
	// is starved, so allow more fingerprint fetches to catch up.
	const auto maxInflight = (j->queue.empty() && j->soloQueue.empty())
		? kFingerprintConcurrencyMax
		: kFingerprintConcurrencyBase;

	// Check-ahead pipeline: fill the queue up to one batch ahead.
	// Doc-id check is instant; fingerprints are bounded to 2 concurrent.
	// The flush fires as soon as the queue reaches the target size.
	while (int(j->queue.size()) + int(j->soloQueue.size()) < windowSize
		&& j->scanned < j->remaining.size()) {
		const auto msgId = j->remaining[j->scanned];

		if (j->gate && msgId >= j->gate) {
			break;
		}

		const auto tmpItem = j->session->data().message(
			FullMsgId(j->src, msgId));
		const auto tmpMedia = tmpItem ? tmpItem->media() : nullptr;
		const auto tmpDoc = tmpMedia ? tmpMedia->document() : nullptr;
		const auto tmpPhoto = tmpMedia ? tmpMedia->photo() : nullptr;
		const auto tmpMediaId = tmpDoc
			? uint64(tmpDoc->id)
			: tmpPhoto
			? uint64(tmpPhoto->id)
			: uint64(0);
		auto &tmpDb = Core::App().downloadManager().ensureDedupDb();
		if (tmpMediaId
			&& tmpDb.isOpen()
			&& tmpDb.containsFwTmpItem(
				j->session->uniqueId(),
				j->dst,
				tmpMediaId,
				(tmpPhoto != nullptr))) {
			j->scanned++;
			j->skipped++;
			EnhancedForward::ReportForwardBatchItem(
				not_null{ j->session },
				j->dst,
				0,
				1);
			continue;
		}

		if (const auto it = j->hashes.find(msgId); it != end(j->hashes)) {
			j->scanned++;
			EnqueueResolved(j, msgId, it->second);
			continue;
		}

		const auto item = j->session->data().message(
			FullMsgId(j->src, msgId));
		const auto media = item ? item->media() : nullptr;
		const auto document = media ? media->document() : nullptr;
		const auto photo = media ? media->photo() : nullptr;

		if (j->awaitingHash.contains(msgId)) {
			j->scanned++;
			continue;
		}

		if (!document && !photo) {
			j->scanned++;
			EnqueueNatural(j, msgId);
			continue;
		}

		const auto mediaId = document
			? uint64(document->id)
			: uint64(photo->id);
		if (db.chatTableContainsDocId(
				j->dst,
				mediaId,
				(photo != nullptr),
				dedupOn)
			|| (dedupOn
				&& db.containsDocId(
					Data::DedupDb::Table::Uploads,
					mediaId,
					(photo != nullptr)))) {
			j->scanned++;
			j->skipped++;
			EnhancedForward::ReportForwardBatchItem(
				not_null{ j->session },
				j->dst,
				0,
				1);
			continue;
		}

		if (j->inflightFetches >= maxInflight) {
			break;
		}

		j->mediaIds.emplace(msgId, mediaId);
		j->mediaPhotos.emplace(msgId, (photo != nullptr));
		j->awaitingHash.insert(msgId);
		if (document) {
			StartFingerprint(j, msgId, document);
		} else {
			StartPhotoFingerprint(j, msgId, photo);
		}
		j->scanned++;
		continue;
	}

	if (!j->soloQueue.empty()) {
		// Album members go one per request (breaks grouping).
		auto chunk = std::vector<MsgId>(
			begin(j->soloQueue),
			begin(j->soloQueue) + 1);
		j->soloQueue.erase(begin(j->soloQueue));
		j->soloInFlight++;
		FlushForwardBatch(j, std::move(chunk));
		return;
	}

	if (!j->queue.empty()) {
		// Ensure ascending msgId order regardless of fingerprint
		// callback completion order, keeping groupIds paired.
		auto paired = std::vector<std::pair<MsgId, MessageGroupId>>();
		paired.reserve(j->queue.size());
		for (auto i = size_t(0); i < j->queue.size(); ++i) {
			paired.emplace_back(j->queue[i], j->queueGroups[i]);
		}
		std::sort(begin(paired), end(paired));
		for (auto i = size_t(0); i < paired.size(); ++i) {
			j->queue[i] = paired[i].first;
			j->queueGroups[i] = paired[i].second;
		}
		const auto readyToFlush = int(j->queue.size()) >= windowSize
			|| j->gateBlocksScan()
			|| (j->scanned >= j->remaining.size()
				&& j->inflightFetches == 0
				&& j->awaitingHash.empty());
		if (!readyToFlush) {
			return;
		}
		auto count = std::min(j->queue.size(), size_t(windowSize));
		const auto mediaAt = [&](size_t index) {
			const auto item = j->session->data().message(
				FullMsgId(j->src, j->queue[index]));
			return item ? item->media() : nullptr;
		};
		for (auto i = size_t(0); i < count; ++i) {
			if (i > 0
				&& !Data::ForwardBatching::ContinuesBatch(
					j->groupOptions,
					j->forwardOptions,
					i,
					j->queueGroups[i - 1],
					j->queueGroups[i],
					[&](size_t index) {
						return Data::ForwardBatching::KindOf(
							mediaAt(index));
					})) {
				count = i;
				break;
			}
		}
		auto chunk = std::vector<MsgId>(
			begin(j->queue),
			begin(j->queue) + count);
		if (j->groupOptions == Data::GroupingOptions::GroupAsIs
			&& chunk.size() > 10) {
			std::reverse(begin(chunk), end(chunk));
		}
		j->queue.erase(
			begin(j->queue),
			begin(j->queue) + count);
		j->queueGroups.erase(
			begin(j->queueGroups),
			begin(j->queueGroups) + count);
		j->batchInFlight = true;
		FlushForwardBatch(j, std::move(chunk));
		return;
	}

	if (j->scanned >= j->remaining.size()
		&& j->inflightFetches == 0
		&& j->awaitingHash.empty()
		&& j->queue.empty()
		&& j->soloQueue.empty()) {
		Finish(j);
	}
}
} // namespace

void RestartFailedFingerprints(not_null<Job*> j) {
	if (j->hashRetries.empty()) {
		return;
	}
	auto failed = base::take(j->hashRetries);
	for (const auto msgId : failed) {
		if (j->state != Job::State::Running) {
			return;
		}
		const auto item = j->session->data().message(
			FullMsgId(j->src, msgId));
		const auto media = item ? item->media() : nullptr;
		const auto document = media ? media->document() : nullptr;
		const auto photo = media ? media->photo() : nullptr;
		if (document) {
			StartFingerprint(j, msgId, document);
		} else if (photo) {
			StartPhotoFingerprint(j, msgId, photo);
		}
	}
}

bool NfWorkPendingFor(not_null<Main::Session*> session, const PeerId &dst) {
	for (const auto &counters : AllCounters(session)) {
		if (counters.dst == dst && counters.active) {
			return true;
		}
	}
	return false;
}

void Start(
		not_null<ApiWrap*> api,
		std::vector<not_null<HistoryItem*>> items,
		const Api::SendAction &action,
		Data::ForwardOptions forwardOptions,
		Data::GroupingOptions groupOptions,
		Fn<void()> completion,
		std::optional<TimeId> videoTimestamp,
	MsgId gate) {
	if (items.empty()) {
		if (completion) {
			completion();
		}
		return;
	}
	const auto session = not_null{ &items.front()->history()->session() };
	const auto dst = action.history->peer->id;
	const auto src = items.front()->history()->peer->id;

	auto remaining = std::vector<MsgId>();
	remaining.reserve(items.size());
	for (const auto &item : items) {
		remaining.push_back(item->fullId().msg);
	}
	std::sort(remaining.begin(), remaining.end());

	auto &jobs = Jobs();
	if (const auto it = jobs.find(dst); it != end(jobs)) {
		const auto job = not_null{ it->second.get() };
		job->total += int(remaining.size());
		EnhancedForward::BeginForwardBatch(session, dst, int(remaining.size()));
		EnhancedForward::NoteForwardBatchFile(
			session,
			dst,
			EnhancedForward::ForwardBatchFirstName(items));
		if (gate) {
			job->gate = job->gate ? std::min(job->gate, gate) : gate;
		}
		job->remaining.insert(
			end(job->remaining),
			std::make_move_iterator(begin(remaining)),
			std::make_move_iterator(end(remaining)));
		std::sort(
			begin(job->remaining) + job->scanned,
			end(job->remaining));
		if (job->state == Job::State::Paused) {
			job->state = Job::State::Running;
		}
		RestartFailedFingerprints(job);
		SaveSnapshot(job);
		Notify();
		Pump(job);
		return;
	}

	auto job = std::make_shared<Job>(
		session,
		api,
		dst,
		src,
		action,
		forwardOptions,
		groupOptions);
	if (completion) {
		job->completion = std::move(completion);
	}
	job->total = int(remaining.size());
	job->remaining = std::move(remaining);
	job->gate = gate;
	job->videoTimestamp = videoTimestamp;
	jobs.emplace(dst, std::move(job));
	auto &stored = jobs.find(dst)->second;
	EnhancedForward::EnsureForwardBatch(session, dst, stored->total);
	EnhancedForward::NoteForwardBatchFile(
		session,
		dst,
		EnhancedForward::ForwardBatchFirstName(items));
	SaveSnapshot(stored.get());
	Notify();
	Pump(stored.get());
}

void AdvanceGate(not_null<ApiWrap*> api, const PeerId &dst, MsgId next) {
	const auto it = Jobs().find(dst);
	if (it == end(Jobs())) {
		return;
	}
	const auto job = not_null{ it->second.get() };
	if (job->gate == next || !job->gate) {
		return;
	}
	// The gate only ever moves forward, so a late confirmation or fallback for
	// an earlier item cannot pull it back over items that were released since.
	if (next && next < job->gate) {
		return;
	}
	job->gate = next;
	Notify();
	Pump(job);
}

MsgId CurrentGate(const PeerId &dst) {
	const auto it = Jobs().find(dst);
	return (it == end(Jobs())) ? MsgId(0) : it->second->gate;
}

void PauseAll(not_null<Main::Session*> session) {
	auto candidates = std::vector<std::shared_ptr<Job>>();
	for (const auto &entry : Jobs()) {
		if (BelongsTo(*entry.second, session) && entry.second->alive()) {
			candidates.push_back(entry.second);
		}
	}
	for (const auto &held : candidates) {
		const auto job = held.get();
		if (!job->alive()) {
			continue;
		}
		for (const auto requestId : base::take(job->requests)) {
			job->api->request(requestId).cancel();
		}
		StopTimers(job);
		EnhancedForward::DropForwardBatch(session, job->dst);
		job->batchInFlight = false;
		job->state = Job::State::Paused;
		auto &db = Core::App().downloadManager().ensureDedupDb();
		SaveSnapshot(job);
		for (const auto &[msgId, registration] : job->regs) {
			db.removePending(
				Data::DedupDb::Table::Uploads,
				registration.mediaId,
				registration.isPhoto);
		}
		job->regs.clear();
		if (const auto it = Jobs().find(job->dst);
			it != end(Jobs()) && it->second.get() == job) {
			Jobs().remove(job->dst);
		}
		FireCompletion(job);
	}
	Notify();
}

void CancelAll(not_null<Main::Session*> session) {
	auto candidates = std::vector<std::shared_ptr<Job>>();
	for (const auto &entry : Jobs()) {
		if (BelongsTo(*entry.second, session)) {
			candidates.push_back(entry.second);
		}
	}
	for (const auto &job : candidates) {
		if (job->state != Job::State::Cancelled) {
			CancelJob(job.get());
		}
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	for (const auto &record : db.loadNfResume(session->uniqueId())) {
		db.clearFwTmpRun(session->uniqueId(), record.destPeerId);
	}
	db.clearNfResume(session->uniqueId());
	Notify();
}

void ResumeRecord(
		not_null<Main::Session*> session,
		const Data::NfResumeRecord &record) {
	if (record.remaining.empty()
		|| Jobs().contains(record.destPeerId)) {
		return;
	}
	auto action = Api::SendAction(
		session->data().history(record.destPeerId));
	const auto forwardOptions = (record.forwardOptions >= 0
		&& record.forwardOptions
			<= int(Data::ForwardOptions::NoNamesAndCaptions))
		? Data::ForwardOptions(record.forwardOptions)
		: Data::ForwardOptions::PreserveInfo;
	const auto groupOptions = (record.groupOptions >= 0
		&& record.groupOptions
			<= int(Data::GroupingOptions::Separate))
		? Data::GroupingOptions(record.groupOptions)
		: Data::GroupingOptions::GroupAsIs;
	auto job = std::make_shared<Job>(
		session,
		&session->api(),
		record.destPeerId,
		record.srcPeerId,
		std::move(action),
		forwardOptions,
		groupOptions);
	job->total = record.total;
	job->done = record.done;
	job->skipped = record.skipped;
	job->lastMsgId = record.lastMsgId;
	job->remaining = record.remaining;
	job->state = Job::State::Running;
	Jobs().emplace(record.destPeerId, job);
	if (EnhancedForward::EnsureForwardBatch(
		session,
		record.destPeerId,
		record.total)) {
		if (const auto batch = EnhancedForward::LookupForwardBatch(
			session,
			record.destPeerId)) {
			batch->sent += record.done;
			batch->skipped += record.skipped;
		}
	}
	SaveSnapshot(job.get());
	Pump(job.get());
}

void ResumeAll(not_null<Main::Session*> session) {
	auto candidates = std::vector<std::shared_ptr<Job>>();
	for (const auto &[dst, job] : Jobs()) {
		if (BelongsTo(*job, session) && job->state == Job::State::Paused) {
			candidates.push_back(job);
		}
	}
	for (const auto &job : candidates) {
		if (job->state != Job::State::Paused) {
			continue;
		}
		job->state = Job::State::Running;
		SaveSnapshot(job.get());
		Pump(job.get());
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	for (const auto &record : db.loadNfResume(session->uniqueId())) {
		ResumeRecord(session, record);
	}
	Notify();
}

void PauseDst(not_null<Main::Session*> session, const PeerId &dst) {
	auto &jobs = Jobs();
	const auto it = jobs.find(dst);
	if (it == end(jobs) || !BelongsTo(*it->second, session)) {
		return;
	}
	const auto held = it->second;
	const auto job = held.get();
	if (!job->alive()) {
		return;
	}
	for (const auto requestId : base::take(job->requests)) {
		job->api->request(requestId).cancel();
	}
	StopTimers(job);
	EnhancedForward::DropForwardBatch(session, job->dst);
	job->batchInFlight = false;
	job->state = Job::State::Paused;
	auto &db = Core::App().downloadManager().ensureDedupDb();
	SaveSnapshot(job);
	for (const auto &[msgId, registration] : job->regs) {
		db.removePending(
			Data::DedupDb::Table::Uploads,
			registration.mediaId,
			registration.isPhoto);
	}
	job->regs.clear();
	jobs.erase(it);
	FireCompletion(job);
	Notify();
}

void ResumeDst(not_null<Main::Session*> session, const PeerId &dst) {
	if (const auto it = Jobs().find(dst);
		it != end(Jobs()) && BelongsTo(*it->second, session)) {
		const auto job = it->second.get();
		if (job->state == Job::State::Paused) {
			job->state = Job::State::Running;
			SaveSnapshot(job);
			Pump(job);
			Notify();
		}
		return;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	for (const auto &record : db.loadNfResume(session->uniqueId())) {
		if (record.destPeerId == dst) {
			ResumeRecord(session, record);
			Notify();
			return;
		}
	}
}

void CancelDst(not_null<Main::Session*> session, const PeerId &dst) {
	const auto it = Jobs().find(dst);
	if (it == end(Jobs()) || !BelongsTo(*it->second, session)) {
		return;
	}
	CancelJob(it->second.get());
}

int UnfinishedCount(not_null<Main::Session*> session) {
	auto result = 0;
	for (const auto &[dst, job] : Jobs()) {
		if (BelongsTo(*job, session) && !job->remaining.empty()) {
			result++;
		}
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	return std::max(result, int(db.loadNfResume(session->uniqueId()).size()));
}

Counters CountersFor(not_null<Main::Session*> session) {
	auto result = Counters();
	for (const auto &[dst, job] : Jobs()) {
		if (BelongsTo(*job, session)) {
			result.dst = dst;
			result.done = job->done;
			result.total = job->total;
			result.skipped = job->skipped;
			result.lastFileName = job->lastFileName;
			result.startedAt = job->startedAt;
			result.floodSeconds = (job->state == Job::State::FloodWait)
				? job->floodSeconds
				: 0;
			result.paused = (job->state == Job::State::Paused);
			result.active = job->alive() || !job->remaining.empty();
			if (!job->remaining.empty()) {
				result.firstSource = FullMsgId(
					job->src,
					job->remaining.front());
			}
			break;
		}
	}
	if (!result.active) {
		auto &db = Core::App().downloadManager().ensureDedupDb();
		if (db.isOpen()
			&& !db.loadNfResume(session->uniqueId()).empty()) {
			result.active = true;
			result.paused = true;
		}
	}
	return result;
}

std::vector<Counters> AllCounters(not_null<Main::Session*> session) {
	auto result = std::vector<Counters>();
	for (const auto &[dst, job] : Jobs()) {
		if (!BelongsTo(*job, session)) {
			continue;
		}
		auto counters = Counters();
		counters.dst = dst;
		counters.done = job->done;
		counters.total = job->total;
		counters.skipped = job->skipped;
		counters.lastFileName = job->lastFileName;
		counters.startedAt = job->startedAt;
		counters.floodSeconds = (job->state == Job::State::FloodWait)
			? job->floodSeconds
			: 0;
		counters.paused = (job->state == Job::State::Paused);
		counters.active = job->alive() || !job->remaining.empty();
		if (!job->remaining.empty()) {
			counters.firstSource = FullMsgId(
				job->src,
				job->remaining.front());
		}
		result.push_back(std::move(counters));
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		for (const auto &record : db.loadNfResume(session->uniqueId())) {
			if (record.remaining.empty()
				|| Jobs().contains(record.destPeerId)) {
				continue;
			}
			auto counters = Counters();
			counters.dst = record.destPeerId;
			counters.done = record.done;
			counters.total = record.total;
			counters.skipped = record.skipped;
			counters.paused = true;
			counters.active = true;
			counters.firstSource = FullMsgId(
				record.srcPeerId,
				record.remaining.front());
			result.push_back(std::move(counters));
		}
	}
	return result;
}

rpl::producer<> countersChanged() {
	return CountersChanged.events();
}

} // namespace NormalForward
