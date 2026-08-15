/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_set.h"
#include "base/timer.h"
#include "data/data_dedup_db.h"

#include <QHash>
#include <QSet>
#include <QVector>

namespace Ui {
struct DownloadBarProgress;
struct DownloadBarContent;
} // namespace Ui

namespace Main {
class Session;
} // namespace Main

namespace Data {

// Used in serialization!
enum class DownloadType {
	Document,
	Photo,
};

// unixtime * 1000.
using DownloadDate = int64;

[[nodiscard]] inline TimeId DateFromDownloadDate(DownloadDate date) {
	return date / 1000;
}

struct DownloadId {
	uint64 objectId = 0;
	DownloadType type = DownloadType::Document;
};

struct DownloadProgress {
	int64 ready = 0;
	int64 total = 0;
};
inline constexpr bool operator==(
		const DownloadProgress &a,
		const DownloadProgress &b) {
	return (a.ready == b.ready) && (a.total == b.total);
}

struct DownloadObject {
	not_null<HistoryItem*> item;
	DocumentData *document = nullptr;
	PhotoData *photo = nullptr;
};

struct DownloadedId {
	DownloadId download;
	DownloadDate started = 0;
	QString path;
	int64 size = 0;
	FullMsgId itemId;
	uint64 peerAccessHash = 0;
	int jobIndex = 0;

	std::unique_ptr<DownloadObject> object;
};

struct DownloadingId {
	DownloadObject object;
	DownloadDate started = 0;
	QString path;
	int64 ready = 0;
	int64 total = 0;
	bool hiddenByView = false;
	bool done = false;
	bool paused = false;
	bool enhancedForward = false;
	int jobIndex = 0;
};

class DownloadManager final {
public:
	DownloadManager();
	~DownloadManager();

	[[nodiscard]] bool empty() const;

	[[nodiscard]] int jobTotal(not_null<Main::Session*> session) const;
	[[nodiscard]] int jobDone(not_null<Main::Session*> session) const;
	[[nodiscard]] int jobId(not_null<Main::Session*> session) const;
	[[nodiscard]] rpl::producer<> jobCounterChanged() const;

	[[nodiscard]] bool isDone(not_null<const HistoryItem*> item) const;

	void trackSession(not_null<Main::Session*> session);
	void itemVisibilitiesUpdated(not_null<Main::Session*> session);

	[[nodiscard]] DownloadDate computeNextStartDate();

	void addLoading(DownloadObject object, bool enhancedForward = false);
	void addLoaded(
		DownloadObject object,
		const QString &path,
		DownloadDate started);
	void removeLoading(not_null<const HistoryItem*> item);

	void clearIfFinished();
	void deleteFiles(const std::vector<GlobalMsgId> &ids);
	void deleteAll();
	[[nodiscard]] bool loadedHasNonCloudFile() const;

	[[nodiscard]] auto loadingList() const
		-> ranges::any_view<const DownloadingId*, ranges::category::input>;
	[[nodiscard]] DownloadProgress loadingProgress() const;
	[[nodiscard]] rpl::producer<> loadingListChanges() const;
	[[nodiscard]] auto loadingProgressValue() const
		-> rpl::producer<DownloadProgress>;

	[[nodiscard]] bool loadingInProgress(
		Main::Session *onlyInSession = nullptr) const;
	void loadingStopWithConfirmation(
		Fn<void()> callback,
		Main::Session *onlyInSession = nullptr);
	void quitWithConfirmation(Fn<void()> quit);

	void pause(not_null<const HistoryItem*> item);
	void resume(not_null<const HistoryItem*> item);
	void cancel(not_null<const HistoryItem*> item);
	// Shows a "Cancel download?" Yes/No confirmation box, then cancels only
	// on confirmation. Meant to back the X button both on a chat bubble's
	// progress circle and on a row in the downloads window - both should
	// route through this instead of calling cancel() directly, so a stray
	// click can't silently drop a download.
	void cancelWithConfirmation(not_null<const HistoryItem*> item);
	void pauseAll();
	void resumeAll();
	void cancelAll();
	void clearFinishedLoading();
	void clearFinishedItem(not_null<const HistoryItem*> item);
	[[nodiscard]] bool anyPaused() const;
	[[nodiscard]] bool anyResumable() const;
	[[nodiscard]] bool anyFinishedLoading() const;

	[[nodiscard]] auto loadedList()
		-> ranges::any_view<const DownloadedId*, ranges::category::input>;
	[[nodiscard]] auto loadedAdded() const
		-> rpl::producer<not_null<const DownloadedId*>>;
	[[nodiscard]] auto loadedRemoved() const
		-> rpl::producer<not_null<const HistoryItem*>>;
	[[nodiscard]] rpl::producer<> loadedResolveDone() const;

	[[nodiscard]] bool hasUnfinishedResume(
		not_null<Main::Session*> session) const;
	void showResumeUnfinished(not_null<Main::Session*> session);
	void clearFingerprintCache();

	// Decides whether downloading the given remote document should be skipped
	// because an identical file was already downloaded. Calls done(true) to
	// skip, done(false) to proceed. The content-hash check (for documents with
	// a different id but identical bytes) requires fetching two sampled chunks
	// from the server, so the result is delivered asynchronously.
	void checkDuplicate(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document,
		Fn<void(bool)> done);

	[[nodiscard]] DedupDb &dedupDb() const;

	// Counts a duplicate that was skipped in a flow and shows one aggregated
	// toast for the whole batch (e.g. "12 duplicate downloads"). A short
	// debounce merges consecutive skips so a multi-file selection produces a
	// single toast with the total count.
	void reportDuplicateSkipped(DedupDb::Table table);

private:
	void notifyDuplicateSkips();
	struct ResumeEntry {
		MsgId msgId = 0;
		DocumentId documentId = 0;
		int64 size = 0;
		QString path;
	};
	struct DeleteFilesDescriptor;
	struct SessionData {
		std::vector<DownloadedId> downloaded;
		std::vector<DownloadingId> downloading;
		int resolveNeeded = 0;
		int resolveSentRequests = 0;
		int resolveSentTotal = 0;
		int jobId = 0;
		rpl::lifetime lifetime;
	};

	void check(not_null<const HistoryItem*> item);
	void check(not_null<DocumentData*> document);
	void check(
		SessionData &data,
		std::vector<DownloadingId>::iterator i);
	void changed(not_null<const HistoryItem*> item);
	void removed(not_null<const HistoryItem*> item);
	void detach(DownloadedId &id);
	void untrack(not_null<Main::Session*> session);
	void remove(
		SessionData &data,
		std::vector<DownloadingId>::iterator i);
	void cancel(
		SessionData &data,
		std::vector<DownloadingId>::iterator i);
	void clearLoading();

	[[nodiscard]] SessionData &sessionData(not_null<Main::Session*> session);
	[[nodiscard]] const SessionData &sessionData(
		not_null<Main::Session*> session) const;
	[[nodiscard]] SessionData &sessionData(
		not_null<const HistoryItem*> item);
	[[nodiscard]] SessionData &sessionData(not_null<DocumentData*> document);

	void resolve(not_null<Main::Session*> session, SessionData &data);
	void resolveRequestsFinished(
		not_null<Main::Session*> session,
		SessionData &data);
	void checkFullResolveDone();

	[[nodiscard]] not_null<HistoryItem*> regenerateItem(
		const DownloadObject &previous);
	[[nodiscard]] not_null<HistoryItem*> generateFakeItem(
		not_null<DocumentData*> document);
	[[nodiscard]] not_null<HistoryItem*> generateItem(
		HistoryItem *previousItem,
		DocumentData *document,
		PhotoData *photo);
	void generateEntry(not_null<Main::Session*> session, DownloadedId &id);

	[[nodiscard]] HistoryItem *lookupLoadingItem(
		Main::Session *onlyInSession) const;
	void loadingStop(Main::Session *onlyInSession);

	void finishFilesDelete(DeleteFilesDescriptor &&descriptor);
	void writePostponed(not_null<Main::Session*> session);
	[[nodiscard]] Fn<std::optional<QByteArray>()> serializator(
		not_null<Main::Session*> session) const;
	[[nodiscard]] std::vector<DownloadedId> deserialize(
		not_null<Main::Session*> session,
		int *jobId = nullptr) const;

	DedupDb &ensureDedupDb() const;
	void saveToDisk();
	void saveIfIdle();
	[[nodiscard]] QString dedupDbPath() const;
	// Fetches (or reuses an already-fetched) partial remote fingerprint for
	// a document. The result (including an empty one, e.g. for small files)
	// is cached per documentId so the 2 sample chunks are only requested
	// from the server once per document, until clearFingerprintCache() is
	// called for it (e.g. on cancel).
	void fetchFingerprint(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document,
		Fn<void(QByteArray)> done);
	void clearFingerprintCache(uint64 documentId);
	void saveFileHash(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document,
		int64 size);
	void removeFileHash(uint64 documentId, int64 size);

	base::flat_map<not_null<Main::Session*>, SessionData> _sessions;
	base::flat_set<not_null<const HistoryItem*>> _loading;
	base::flat_set<not_null<DocumentData*>> _loadingDocuments;
	base::flat_set<not_null<const HistoryItem*>> _loadingDone;
	base::flat_set<not_null<const HistoryItem*>> _loaded;
	base::flat_set<not_null<HistoryItem*>> _generated;
	base::flat_set<not_null<DocumentData*>> _generatedDocuments;

	TimeId _lastStartedBase = 0;
	int _lastStartedAdded = 0;

	rpl::event_stream<> _loadingListChanges;
	rpl::variable<DownloadProgress> _loadingProgress;
	rpl::event_stream<> _jobCounterChanged;

	rpl::event_stream<not_null<const DownloadedId*>> _loadedAdded;
	rpl::event_stream<not_null<const HistoryItem*>> _loadedRemoved;
	rpl::variable<bool> _loadedResolveDone;

	base::Timer _clearLoadingTimer;

	mutable std::unique_ptr<DedupDb> _dedupDb;
	int _checksInProgress = 0;

	// Caches the partial remote fingerprint per documentId so the 2 sample
	// chunks are requested from the server only once per download attempt,
	// shared between checkDuplicate() and saveFileHash(). An empty
	// QByteArray is a valid cached value (e.g. file too small to sample).
	QHash<uint64, QByteArray> _fingerprintCache;

	base::Timer _duplicatesToastTimer;
	int _duplicatesSkipped[2] = { 0, 0 };

};

[[nodiscard]] auto MakeDownloadBarProgress()
-> rpl::producer<Ui::DownloadBarProgress>;

[[nodiscard]] rpl::producer<Ui::DownloadBarContent> MakeDownloadBarContent();

[[nodiscard]] auto MakeUploadBarProgress()
-> rpl::producer<Ui::DownloadBarProgress>;

[[nodiscard]] rpl::producer<Ui::DownloadBarContent> MakeUploadBarContent();

} // namespace Data
