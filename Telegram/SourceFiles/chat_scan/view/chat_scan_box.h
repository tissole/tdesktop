/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/timer.h"
#include "chat_scan/chat_scan_api.h"
#include "data/data_dedup_db.h"
#include "mtproto/sender.h"
#include "ui/layers/box_content.h"

#include <QFile>
#include <QString>
#include <QTextStream>

#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class Checkbox;
class FlatLabel;
class RoundButton;
class VerticalLayout;
} // namespace Ui

class PeerData;

class ChatScanBox final : public Ui::BoxContent {
public:
	ChatScanBox(
		QWidget*,
		not_null<PeerData*> peer,
		bool rescan = false,
		bool autostart = true);

protected:
	void prepare() override;
	void resizeEvent(QResizeEvent *e) override;

private:
	struct Row {
		int category = 0;
		QString name;
		QPointer<Ui::FlatLabel> unique;
		QPointer<Ui::FlatLabel> dups;
	};

	void render(const ChatScan::State &state);
	void renderResults(const std::vector<Data::ChatScanRow> &rows);
	void showResults(
		const base::flat_map<int, ChatScan::CategoryTotals> &categories,
		int dupTotal,
		int64 dupBytes);
	void startDelete();
	void deleteCollectStep();
	void beginDelete(std::vector<int32> ids);
	void confirmDelete();
	void sendNextDeleteBatch();
	void finishDelete(const QString &error);
	void settleDelete(int attempt);
	void writeDupReport(bool deleted);
	void writePausedReport();
	[[nodiscard]] bool canDeleteMessages() const;
	[[nodiscard]] QString scanReportPath();
	// The report lists every duplicate of the chat, so building it is split
	// into bounded slices - file parse, keyed reads and the final write -
	// with an event loop turn between them: rebuilding it in one pass takes
	// minutes on a large chat and used to freeze the whole interface.
	struct ReportJob {
		struct Listed {
			QString line;
			int64 size = 0;
			bool inDb = false;
		};
		QString status;
		QString path;
		std::map<int, QString> names;
		std::map<int, std::pair<int, int64>> uniques;
		std::map<int, std::map<int64, Listed>> listed;
		QString dateLine;
		std::vector<int> categories;
		size_t categoryPos = 0;
		int currentCategory = -1;
		int64 pageAbove = 0;
		std::unique_ptr<QFile> file;
		std::unique_ptr<QTextStream> stream;
		bool parsing = false;
	};
	void upsertScanReport(
		const QString &status,
		const std::map<int, std::pair<int, int64>> &uniques,
		bool append);
	void reportStep();
	void renderReport();
	void setupApi(bool incremental);
	void askRescan();
	void askCancel();
	void askQuestion(const QString &text, Fn<void()> onYes);
	void hideQuestion();
	void clearStats();
	void runScanNew();
	void cancelPressed();
	void actionPressed();
	void auxPressed();
	void updateBar(
		std::optional<QString> action,
		bool actionEnabled,
		std::optional<QString> aux,
		bool showNew);
	void attachApi();
	void subscribeApi();
	[[nodiscard]] uint64 scanKey() const;
	// Rate window over the hashed files. Seeded with a nominal pace so the
	// estimate is shown from the first render and then replaced by the
	// measured one.
	[[nodiscard]] QString etaSuffix(int done, int total);
	[[nodiscard]] QString unhashedSuffix(const ChatScan::State &state) const;
	[[nodiscard]] QString floodSuffix(const ChatScan::State &state) const;
	void refreshFloodSuffix();

	const not_null<PeerData*> _peer;
	const not_null<Main::Session*> _session;
	const bool _rescan = false;
	const bool _autostart = true;
	bool _reportAppend = false;

	std::shared_ptr<ChatScan::Api> _api;
	MTP::Sender _deleteApi;
	base::Timer _floodTimer;
	base::Timer _settleTimer;
	base::Timer _floodSuffixTimer;
	base::Timer _reportTimer;
	base::Timer _deleteCollectTimer;
	std::unique_ptr<ReportJob> _reportJob;
	QString _reportQueuedStatus;
	std::map<int, std::pair<int, int64>> _reportQueuedUniques;
	bool _reportQueuedAppend = false;
	std::vector<int32> _deleteCollectIds;
	int64 _deleteCollectAbove = 0;
	bool _deleteCollecting = false;
	int _finishedUnhashed = 0;
	// Largest total-files estimate shown this session: the projection is
	// only an estimate of a number the scan does not know yet, and it may
	// wobble while the walk measures the chat, so the panel never walks the
	// denominator backwards.
	int64 _projectedFiles = 0;
	// The projected count is pinned once enough of the chat has been walked:
	// it describes the whole chat, so it must stop moving instead of being
	// recomputed from a density that keeps being refined page by page.
	int64 _pinnedFiles = 0;
	int64 _etaSeedTime = 0;
	int64 _etaSeedDone = 0;
	double _etaPerMs = 0.;

	QPointer<Ui::VerticalLayout> _content;
	rpl::variable<QString> _statusText;
	rpl::variable<QString> _progressText;
	QPointer<Ui::FlatLabel> _statusLabel;
	QPointer<Ui::FlatLabel> _progressLabel;
	std::vector<Row> _rows;
	int _totalRow = 0;
	rpl::variable<QString> _confirmText;
	QPointer<Ui::FlatLabel> _confirmLabel;
	QPointer<Ui::FlatLabel> _totalUniqueLabel;
	QPointer<Ui::FlatLabel> _totalDupsLabel;
	QPointer<Ui::Checkbox> _revoke;

	QPointer<Ui::RoundButton> _actionButton;
	QPointer<Ui::RoundButton> _auxButton;
	QPointer<Ui::RoundButton> _newButton;
	std::optional<QString> _barAction;
	bool _barActionEnabled = true;
	std::optional<QString> _barAux;
	bool _barNew = false;

	object_ptr<Ui::RpWidget> _askOverlay = nullptr;
	QPointer<Ui::FlatLabel> _askLabel;
	QPointer<Ui::RoundButton> _askYesButton;
	QPointer<Ui::RoundButton> _askNoButton;
	Fn<void()> _askYes;

	std::vector<int32> _deleteIds;
	size_t _deletePos = 0;
	int _deleteTotal = 0;
	bool _deleteFinished = false;
	bool _confirming = false;
	bool _deleting = false;
	bool _showingResults = false;
};
