/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "info/media/info_media_common.h"
#include "base/weak_ptr.h"

#include <map>
#include <optional>
#include <tuple>

namespace Data {
struct DownloadedId;
} // namespace Data

namespace EnhancedForward {
struct JobSnapshot;
struct FinishedFwRun;
} // namespace EnhancedForward

namespace Info {
class AbstractController;
} // namespace Info

namespace Info::Downloads {

class Provider final
	: public Media::ListProvider
	, private Media::ListSectionDelegate
	, public base::has_weak_ptr {
public:
	enum class Filter {
		All,
		Downloads,
		Uploads,
		Forwards,
	};

	explicit Provider(not_null<AbstractController*> controller);

	void setFilter(Filter filter);
	void refreshForwards();
	[[nodiscard]] rpl::producer<bool> hasDownloadsValue() const;
	[[nodiscard]] rpl::producer<bool> hasUploadsValue() const;

	Media::Type type() override;
	bool hasSelectRestriction() override;
	rpl::producer<bool> hasSelectRestrictionChanges() override;
	bool isPossiblyMyItem(not_null<const HistoryItem*> item) override;

	std::optional<int> fullCount() override;

	void restart() override;
	void checkPreload(
		QSize viewport,
		not_null<Media::BaseLayout*> topLayout,
		not_null<Media::BaseLayout*> bottomLayout,
		bool preloadTop,
		bool preloadBottom) override;
	void refreshViewer() override;
	rpl::producer<> refreshed() override;

	void setSearchQuery(QString query) override;
	void jumpToMessage(MsgId messageId, Fn<void(FullMsgId)> callback) override;

	std::vector<Media::ListSection> fillSections(
		not_null<Overview::Layout::Delegate*> delegate) override;
	rpl::producer<not_null<Media::BaseLayout*>> layoutRemoved() override;
	Media::BaseLayout *lookupLayout(const HistoryItem *item) override;
	bool isMyItem(not_null<const HistoryItem*> item) override;
	bool isAfter(
		not_null<const HistoryItem*> a,
		not_null<const HistoryItem*> b) override;

	Media::ListItemSelectionData computeSelectionData(
		not_null<const HistoryItem*> item,
		TextSelection selection) override;
	void applyDragSelection(
		Media::ListSelectedMap &selected,
		not_null<const HistoryItem*> fromItem,
		bool skipFrom,
		not_null<const HistoryItem*> tillItem,
		bool skipTill) override;

	bool allowSaveFileAs(
		not_null<const HistoryItem*> item,
		not_null<DocumentData*> document) override;
	bool isUploadItem(not_null<const HistoryItem*> item) const;
	bool isEnhancedForward(not_null<const HistoryItem*> item) const;
	bool isDownloading(not_null<const HistoryItem*> item) const;
	bool isDownloaded(not_null<const HistoryItem*> item) const;
	bool isUploading(not_null<const HistoryItem*> item) const;
	bool isUploaded(not_null<const HistoryItem*> item) const;
	bool isEnhancedForwardFinished(not_null<const HistoryItem*> item) const;
	bool isForwardAggregate(not_null<const HistoryItem*> item) const;
	[[nodiscard]] std::optional<PeerId> forwardAggregateDst(
		not_null<const HistoryItem*> item) const;
	[[nodiscard]] uint64 forwardAggregateGroupId(
		not_null<const HistoryItem*> item) const;
	[[nodiscard]] bool isForwardAggregatePaused(
		not_null<const HistoryItem*> item) const;
	[[nodiscard]] bool isForwardAggregateFinished(
		not_null<const HistoryItem*> item) const;
	void toggleForwardRun(not_null<const HistoryItem*> item);
	void cancelForwardRun(not_null<const HistoryItem*> item);
	void clearForwardRun(not_null<const HistoryItem*> item);
	bool isDownloadAggregate(not_null<const HistoryItem*> item) const;
	[[nodiscard]] uint64 downloadAggregateBatchId(
		not_null<const HistoryItem*> item) const;
	[[nodiscard]] bool isDownloadAggregateFinished(
		not_null<const HistoryItem*> item) const;
	void clearDownloadRun(uint64 batchId);
	bool isUploadAggregate(not_null<const HistoryItem*> item) const;
	[[nodiscard]] uint64 uploadAggregateBatchId(
		not_null<const HistoryItem*> item) const;
	[[nodiscard]] bool isUploadAggregateFinished(
		not_null<const HistoryItem*> item) const;
	void clearUploadRun(uint64 batchId);
	void clearTransferRow(not_null<const HistoryItem*> item);
	QString showInFolderPath(
		not_null<const HistoryItem*> item,
		not_null<DocumentData*> document) override;

	[[nodiscard]] rpl::producer<QString> counterValue() const;


	int64 scrollTopStatePosition(not_null<HistoryItem*> item) override;
	HistoryItem *scrollTopStateItem(
		Media::ListScrollTopState state) override;
	void saveState(
		not_null<Media::Memento*> memento,
		Media::ListScrollTopState scrollState) override;
	void restoreState(
		not_null<Media::Memento*> memento,
		Fn<void(Media::ListScrollTopState)> restoreScrollState) override;

private:
	struct Element {
		not_null<HistoryItem*> item;
		int64 started = 0; // unixtime * 1000
		QString path;
		int order = 0;     // insertion order (source order for forwards)
		// Forward rows (live aggregate + one row per finished action)
		// reuse their first source message as the carrier item.
		PeerId fwDst;
		int fwRow = 0; // 0 = normal row, 1 = forward row
		uint64 fwGroup = 0; // 0 = live aggregate, else finished action id
		// Download batch headers reuse their first file as the carrier item.
		uint64 dlBatch = 0; // 0 = ungrouped, else grouped batch id
		int dlRow = 0; // 0 = normal row, 1 = batch header
		// Upload batch headers reuse their first file as the carrier item.
		uint64 ulBatch = 0; // 0 = ungrouped, else grouped batch id
		int ulRow = 0; // 0 = normal row, 1 = batch header

		QStringList words;
		base::flat_set<QChar> letters;
		bool found = false;
	};

	struct PendingFinishedUpload {
		not_null<Main::Session*> session;
		int64 started = 0;
		QString path;
		uint64 batchId = 0;
	};

	bool sectionHasFloatingHeader() override;
	QString sectionTitle(not_null<const Media::BaseLayout*> item) override;
	bool sectionItemBelongsHere(
		not_null<const Media::BaseLayout*> item,
		not_null<const Media::BaseLayout*> previous) override;

	[[nodiscard]] bool searchMode() const;
	void fillSearchIndex(Element &element);
	[[nodiscard]] bool computeIsFound(const Element &element) const;
	void updateAvailability();

	void itemRemoved(not_null<const HistoryItem*> item);
	void markLayoutsStale();
	void clearStaleLayouts();

	void refreshPostponed(bool added);
	void addPostponed(not_null<const Data::DownloadedId*> entry);
	void performRefresh();
	void performAdd();
	void addElementNow(Element &&element);
	void refreshEF(
		not_null<Main::Session*> session,
		const std::vector<EnhancedForward::JobSnapshot> &jobs);
	bool ensureFwAggregate(
		not_null<Main::Session*> session,
		const PeerId &dst,
		not_null<HistoryItem*> carrier);
	bool ensureFwFinishedGroup(
		not_null<Main::Session*> session,
		const PeerId &dst,
		uint64 actionId,
		not_null<HistoryItem*> carrier);
	void removeFwAggregate(
		not_null<Main::Session*> session,
		const PeerId &dst);
	void removeFwFinishedGroup(
		not_null<Main::Session*> session,
		uint64 actionId);
	void syncDlRuns();
	void touchDlHeaders();
	[[nodiscard]] bool isDlHeaderLayout(
		not_null<const Media::BaseLayout*> layout) const;
	void removeDlHeader(uint64 batchId);
	void syncUlRuns();
	void touchUlHeaders();
	[[nodiscard]] bool isUlHeaderLayout(
		not_null<const Media::BaseLayout*> layout) const;
	void removeUlHeader(uint64 batchId);
	void remove(not_null<const HistoryItem*> item);
	void trackItemSession(not_null<const HistoryItem*> item);
	void updateCounter();
	void addFinishedUpload(
		not_null<Main::Session*> session,
		FullMsgId itemId,
		const PendingFinishedUpload &upload);
	void resolvePendingFinishedUploads(not_null<Main::Session*> session);

	[[nodiscard]] Media::BaseLayout *getLayout(
		Element element,
		not_null<Overview::Layout::Delegate*> delegate);
	[[nodiscard]] std::unique_ptr<Media::BaseLayout> createLayout(
		Element element,
		not_null<Overview::Layout::Delegate*> delegate);
	void appendGroupedDownloads(
		Media::ListSection &section,
		not_null<Overview::Layout::Delegate*> delegate,
		bool search,
		Fn<bool(const Element&)> accept);
	void appendGroupedUploads(
		Media::ListSection &section,
		not_null<Overview::Layout::Delegate*> delegate,
		bool search,
		Fn<bool(const Element&)> accept);

	const not_null<AbstractController*> _controller;

	std::vector<Element> _elements;
	std::optional<int> _fullCount;
	base::flat_set<not_null<const HistoryItem*>> _downloading;
	base::flat_set<not_null<const HistoryItem*>> _enhancedForward;
	base::flat_set<not_null<const HistoryItem*>> _downloaded;
	base::flat_set<FullMsgId> _uploading;
	base::flat_set<FullMsgId> _uploaded;
	base::flat_set<FullMsgId> _efPending;
	base::flat_set<FullMsgId> _efTransferring;
	base::flat_map<FullMsgId, PendingFinishedUpload> _pendingFinishedUploads;
	base::flat_set<FullMsgId> _pendingFinishedUploadsFetching;
	int _storiesAddToAlbumId = 0;

	Filter _filter = Filter::All;
	bool _showGroupHeaders = false;
	rpl::variable<bool> _hasDownloads = false;
	rpl::variable<bool> _hasUploads = false;
	rpl::variable<QString> _counterText;
	std::vector<Element> _addPostponed;

	std::unordered_map<
		not_null<const HistoryItem*>,
		Media::CachedItem> _layouts;
	// Live aggregate rows keyed by session and destination (group 0),
	// finished-action rows keyed by session, destination and action id.
	std::map<
		std::tuple<Main::Session*, PeerId, uint64>,
		Media::CachedItem> _fwLayouts;
	// Download batch headers keyed by session and batch id.
	std::map<
		std::tuple<Main::Session*, uint64>,
		Media::CachedItem> _dlLayouts;
	// Upload batch headers keyed by session and batch id.
	std::map<
		std::tuple<Main::Session*, uint64>,
		Media::CachedItem> _ulLayouts;
	rpl::event_stream<not_null<Media::BaseLayout*>> _layoutRemoved;
	rpl::event_stream<> _refreshed;

	QString _query;
	QStringList _queryWords;
	int _foundCount = 0;

	base::flat_map<not_null<Main::Session*>, rpl::lifetime> _trackedSessions;
	base::flat_map<not_null<Main::Session*>, uint64> _fwTickDigest;
	int _nextElementOrder = 0;
	base::flat_set<FullMsgId> _failedEFResolve;
	base::flat_set<FullMsgId> _fetchingEFResolve;
	bool _postponedRefreshSort = false;
	bool _postponedRefresh = false;
	bool _started = false;

	rpl::lifetime _lifetime;

};

} // namespace Info::Downloads
