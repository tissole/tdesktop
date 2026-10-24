/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_scan/view/chat_scan_box.h"

#include "chat_scan/chat_scan_index.h"
#include "chat_scan/chat_scan_top_bar.h"
#include "base/algorithm.h"
#include "ui/qt_object_factory.h"

#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_dedup_db.h"
#include "data/data_download_manager.h"
#include "data/data_peer.h"
#include "export/export_settings.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtproto/mtproto_response.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "data/data_peer_id.h"
#include "ui/painter.h"
#include "ui/toast/toast.h"
#include "ui/text/format_values.h"
#include "ui/text/text_utilities.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QtCore/QTextStream>

#include <algorithm>
#include <map>

namespace {

using Type = Export::MediaSettings::Type;

// Hashing pace assumed until measured, so the ETA shows from the first render.
constexpr auto kNominalHashPerMs = 20.0 / 1000.;
constexpr auto kEtaMeasureMs = int64(15000);
constexpr auto kPinAfterMessages = 2000;
constexpr auto kPinSampleCeil = 20000;

// Only the leading digits are meaningful, so the total is rounded.
int64 RoundToFewDigits(int64 value) {
	auto step = int64(1);
	while (value / step >= 1000) {
		step *= 10;
	}
	return ((value + step / 2) / step) * step;
}

[[nodiscard]] int ScanResolvedCount(const ChatScan::State &state) {
	return state.hashDone;
}

[[nodiscard]] QString FormatSizeWithBytes(int64 size) {
	return Ui::FormatSizeText(size)
		+ u" / "_q
		+ QString::number(size)
		+ u" bytes"_q;
}

QString FormatEtaTime(int seconds) {
	if (seconds <= 0) {
		return QString();
	}
	const auto days = seconds / 86400;
	const auto hours = (seconds % 86400) / 3600;
	const auto mins = (seconds % 3600) / 60;
	if (days > 0) {
		return u"%1d %2h %3m"_q.arg(days).arg(hours).arg(mins);
	}
	if (hours > 0) {
		return u"%1h %2m"_q.arg(hours).arg(mins);
	}
	if (mins > 0) {
		return u"%1m"_q.arg(mins);
	}
	return u"%1s"_q.arg(seconds);
}

using ScanKey = uint64;

std::map<ScanKey, std::shared_ptr<ChatScan::Api>> &RunningScans() {
	static auto scans = std::map<ScanKey, std::shared_ptr<ChatScan::Api>>();
	return scans;
}

class ScanGrid : public Ui::RpWidget {
public:
	explicit ScanGrid(QWidget *parent) : Ui::RpWidget(parent) {
	}

	int addRow(
			object_ptr<Ui::FlatLabel> &&name,
			object_ptr<Ui::FlatLabel> &&unique,
			object_ptr<Ui::FlatLabel> &&dups) {
		const auto index = int(_rows.size());
		_rows.push_back({
			std::move(name),
			std::move(unique),
			std::move(dups) });
		auto &row = _rows.back();
		for (const auto cell : {
				row.name.data(),
				row.unique.data(),
				row.dups.data() }) {
			cell->setParent(this);
			cell->show();
			cell->heightValue(
			) | rpl::on_next([=] {
				if (!_inResize) {
					relayout();
				}
			}, _lifetime);
			cell->naturalWidthValue(
			) | rpl::skip(1) | rpl::on_next([=] {
				if (!_inResize) {
					relayout();
				}
			}, _lifetime);
		}
		return index;
	}

	[[nodiscard]] Ui::FlatLabel *nameAt(int index) const {
		return _rows[index].name.data();
	}

	[[nodiscard]] Ui::FlatLabel *uniqueAt(int index) const {
		return _rows[index].unique.data();
	}

	[[nodiscard]] Ui::FlatLabel *dupsAt(int index) const {
		return _rows[index].dups.data();
	}

	[[nodiscard]] int rowsCount() const {
		return int(_rows.size());
	}

protected:
	int resizeGetHeight(int newWidth) override {
		_inResize = true;
		const auto result = layout(newWidth);
		_inResize = false;
		return result;
	}

	void visibleTopBottomUpdated(int visibleTop, int visibleBottom) override {
		for (const auto &row : _rows) {
			setChildVisibleTopBottom(row.name, visibleTop, visibleBottom);
			setChildVisibleTopBottom(row.unique, visibleTop, visibleBottom);
			setChildVisibleTopBottom(row.dups, visibleTop, visibleBottom);
		}
	}

private:
	struct GridRow {
		object_ptr<Ui::FlatLabel> name;
		object_ptr<Ui::FlatLabel> unique;
		object_ptr<Ui::FlatLabel> dups;
		int top = 0;
	};

	void relayout() {
		if (width() <= 0) {
			return;
		}
		const auto height = layout(width());
		resize(width(), height);
		update();
	}

	int layout(int newWidth) {
		if (_rows.empty() || newWidth <= 0) {
			return 0;
		}
		const auto gap = st::defaultVerticalListSkip;
		auto nameWidth = 0;
		for (const auto &row : _rows) {
			nameWidth = std::max(nameWidth, row.name->naturalWidth());
		}
		const auto statsWidth = std::max(
			(newWidth - nameWidth - 2 * gap) / 2,
			0);
		const auto uniqueLeft = nameWidth + gap;
		const auto dupsLeft = uniqueLeft + statsWidth + gap;
		auto top = 0;
		for (auto &row : _rows) {
			row.name->resizeToNaturalWidth(nameWidth);
			row.unique->resizeToWidth(statsWidth);
			row.dups->resizeToWidth(statsWidth);
			const auto height = std::max({
				row.name->heightNoMargins(),
				row.unique->heightNoMargins(),
				row.dups->heightNoMargins() });
			row.top = top;
			row.name->moveToLeft(0, top, newWidth);
			row.unique->moveToLeft(uniqueLeft, top, newWidth);
			row.dups->moveToLeft(dupsLeft, top, newWidth);
			top += height + gap;
		}
		return top;
	}

	std::vector<GridRow> _rows;
	rpl::lifetime _lifetime;
	bool _inResize = false;

};

class AskOverlay final : public Ui::RpWidget {
public:
	explicit AskOverlay(QWidget *parent) : Ui::RpWidget(parent) {
	}

protected:
	void paintEvent(QPaintEvent *event) override {
		Painter p(this);
		p.fillRect(rect(), st::layerBg);
	}

};

class AskCard final : public Ui::RpWidget {
public:
	explicit AskCard(QWidget *parent) : Ui::RpWidget(parent) {
	}

protected:
	void paintEvent(QPaintEvent *event) override {
		Painter p(this);
		p.fillRect(rect(), st::boxBg);
	}

};

} // namespace

ChatScanBox::ChatScanBox(
	QWidget*,
	not_null<PeerData*> peer,
	bool rescan,
	bool autostart)
: _peer(peer)
, _session(&peer->session())
, _rescan(rescan)
, _autostart(autostart)
, _deleteApi(&peer->session().mtp()) {
}

void ChatScanBox::prepare() {
	setTitle(tr::lng_chat_scan_title());
	addTopButton(st::boxTitleClose, [=] {
		closeBox();
	});
	const auto content = setInnerWidget(
		object_ptr<Ui::VerticalLayout>(this));
	_content = content;
	_statusLabel = content->add(object_ptr<Ui::FlatLabel>(
		content,
		_statusText.value(),
		st::boxLabel));
	_progressLabel = content->add(object_ptr<Ui::FlatLabel>(
		content,
		_progressText.value(),
		st::boxLabel));
	const auto rows = {
		std::pair{ int(Type::Photo), tr::lng_chat_scan_cat_photo(tr::now) },
		std::pair{ int(Type::Video), tr::lng_chat_scan_cat_video(tr::now) },
		std::pair{ int(Type::Audio), tr::lng_chat_scan_cat_audio(tr::now) },
		std::pair{ int(Type::File), tr::lng_chat_scan_cat_file(tr::now) },
		std::pair{ int(Type::VideoMessage), tr::lng_chat_scan_cat_round(tr::now) },
		std::pair{ int(Type::VoiceMessage), tr::lng_chat_scan_cat_voice(tr::now) },
		std::pair{ int(Type::GIF), tr::lng_chat_scan_cat_gif(tr::now) },
		std::pair{ int(Type::Sticker), tr::lng_chat_scan_cat_sticker(tr::now) },
	};
	const auto grid = content->add(object_ptr<ScanGrid>(content));
	grid->addRow(
		object_ptr<Ui::FlatLabel>(grid, st::boxLabel),
		object_ptr<Ui::FlatLabel>(
			grid,
			tr::lng_chat_scan_unique_files(tr::now),
			st::boxLabel),
		object_ptr<Ui::FlatLabel>(
			grid,
			tr::lng_chat_scan_duplicate_files(tr::now),
			st::boxLabel));
	for (const auto &[category, name] : rows) {
		auto row = Row();
		row.category = category;
		row.name = name;
		const auto gridRow = grid->addRow(
			object_ptr<Ui::FlatLabel>(grid, st::boxLabel),
			object_ptr<Ui::FlatLabel>(grid, st::boxLabel),
			object_ptr<Ui::FlatLabel>(grid, st::boxLabel));
		auto marked = Ui::Text::Bold(name);
		grid->nameAt(gridRow)->setMarkedText(marked);
		row.unique = grid->uniqueAt(gridRow);
		row.dups = grid->dupsAt(gridRow);
		row.unique->setText(u"—"_q);
		row.dups->setText(u"—"_q);
		_rows.push_back(std::move(row));
	}
	_totalRow = grid->addRow(
		object_ptr<Ui::FlatLabel>(grid, st::boxLabel),
		object_ptr<Ui::FlatLabel>(grid, st::boxLabel),
		object_ptr<Ui::FlatLabel>(grid, st::boxLabel));
	{
		auto marked = Ui::Text::Bold(tr::lng_chat_scan_total(tr::now));
		grid->nameAt(_totalRow)->setMarkedText(marked);
	}
	_totalUniqueLabel = grid->uniqueAt(_totalRow);
	_totalDupsLabel = grid->dupsAt(_totalRow);
	_confirmLabel = content->add(object_ptr<Ui::FlatLabel>(
		content,
		_confirmText.value(),
		st::boxLabel));
	_confirmLabel->hide();
	_revoke = content->add(object_ptr<Ui::Checkbox>(
		content,
		tr::lng_chat_scan_revoke(tr::now),
		false,
		st::defaultBoxCheckbox));
	_revoke->hide();
	_actionButton = addButton(tr::lng_chat_scan_scan(), [=] {
		actionPressed();
	});
	_auxButton = addButton(tr::lng_cancel(), [=] {
		auxPressed();
	});
	_newButton = addButton(tr::lng_chat_scan_new(), [=] {
		runScanNew();
	});
	updateBar(std::nullopt, true, std::nullopt, false);
	setDimensionsToContent(st::boxWideWidth, _content);
	attachApi();
	if (_api) {
		subscribeApi();
		render(_api->currentState());
		return;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!_rescan && db.isOpen()) {
		if (db.isChatScanned(_peer->id)) {
			_reportAppend = true;
			renderResults(db.readChatScan(_peer->id));
			return;
		}
		const auto cursor = db.readScanCursor(_peer->id);
		if (cursor
			&& cursor->phase != int(ChatScan::State::Phase::Finished)
			&& cursor->phase != int(ChatScan::State::Phase::Cancelled)) {
			_api = std::make_shared<ChatScan::Api>(_session, _peer);
			RunningScans()[scanKey()] = _api;
			ChatScan::FireScanTopBarEvent({ _peer, _api });
			subscribeApi();
			if (_api->loadPaused(*cursor)) {
				_reportAppend = true;
				render(_api->currentState());
				return;
			}
			RunningScans()[scanKey()].reset();
			_api.reset();
		}
	}
	if (!_autostart) {
		_newButton->hide();
		_showingResults = false;
		updateBar(
			tr::lng_chat_scan_scan(tr::now),
			true,
			tr::lng_cancel(tr::now),
			false);
		return;
	}
	setupApi(_rescan);
}

ScanKey ChatScanBox::scanKey() const {
	return _peer->id.value;
}

void ChatScanBox::resizeEvent(QResizeEvent *e) {
	if (_askOverlay) {
		_askOverlay->setGeometry(rect());
	}
	BoxContent::resizeEvent(e);
}

void ChatScanBox::attachApi() {
	auto &slot = RunningScans()[scanKey()];
	if (slot) {
		const auto phase = slot->currentState().phase;
		if (phase != ChatScan::State::Phase::Finished
			&& phase != ChatScan::State::Phase::Cancelled) {
			_api = slot;
			return;
		}
		slot.reset();
	}
	_api.reset();
}

void ChatScanBox::subscribeApi() {
	_api->state(
	) | rpl::on_next([=](const ChatScan::State &state) {
		render(state);
	}, lifetime());
}

void ChatScanBox::setupApi(bool incremental) {
	if (const auto slot = RunningScans()[scanKey()]) {
		if (slot != _api) {
			const auto phase = slot->currentState().phase;
			if (phase != ChatScan::State::Phase::Finished
				&& phase != ChatScan::State::Phase::Cancelled
				&& phase != ChatScan::State::Phase::Failed) {
				_api = slot;
				subscribeApi();
				return;
			}
		}
	}
	if (_api) {
		_api->finishTakeout();
	}
	_showingResults = false;
	_projectedFiles = 0;
	_pinnedFiles = 0;
	updateBar(
		tr::lng_chat_scan_pause(tr::now),
		true,
		tr::lng_cancel(tr::now),
		false);
	_api = std::make_shared<ChatScan::Api>(_session, _peer);
	RunningScans()[scanKey()] = _api;
	ChatScan::FireScanTopBarEvent({ _peer, _api });
	subscribeApi();
	_reportAppend = false;
	if (incremental) {
		_api->rescan();
	} else {
		_api->start();
	}
}

void ChatScanBox::cancelPressed() {
	if (_confirming || _deleting) {
		_deleting = false;
		_confirming = false;
		_settleTimer.cancel();
		_confirmLabel->hide();
		_revoke->hide();
		auto &db = Core::App().downloadManager().ensureDedupDb();
		renderResults(db.readChatScan(_peer->id));
	} else if (_api) {
		askCancel();
	} else {
		closeBox();
	}
}

void ChatScanBox::actionPressed() {
	if (_confirming) {
		confirmDelete();
		return;
	}
	if (_deleting) {
		return;
	}
	if (_showingResults) {
		startDelete();
		return;
	}
	if (_api) {
		using Phase = ChatScan::State::Phase;
		const auto phase = _api->currentState().phase;
		if (phase == Phase::Scanning || phase == Phase::Resolving) {
			_api->pause();
			writePausedReport();
			return;
		}
		if (phase == Phase::Paused || phase == Phase::Failed) {
			_reportAppend = true;
			_api->resume();
			return;
		}
	}
	setupApi(false);
}

void ChatScanBox::auxPressed() {
	if (_showingResults && !_confirming && !_deleting) {
		askRescan();
		return;
	}
	cancelPressed();
}

void ChatScanBox::updateBar(
		std::optional<QString> action,
		bool actionEnabled,
		std::optional<QString> aux,
		bool showNew) {
	if (action != _barAction) {
		_barAction = action;
		if (action) {
			_actionButton->setText(rpl::single(*action));
			_actionButton->show();
		} else {
			_actionButton->hide();
		}
	} else if (action && _actionButton->isHidden()) {
		_actionButton->show();
	}
	if (action && (actionEnabled != _barActionEnabled)) {
		_barActionEnabled = actionEnabled;
		_actionButton->setEnabled(actionEnabled);
	}
	if (aux != _barAux) {
		_barAux = aux;
		if (aux) {
			_auxButton->setText(rpl::single(*aux));
			_auxButton->show();
		} else {
			_auxButton->hide();
		}
	} else if (aux && _auxButton->isHidden()) {
		_auxButton->show();
	}
	if (showNew != _barNew) {
		_barNew = showNew;
		if (showNew) {
			_newButton->show();
		} else {
			_newButton->hide();
		}
	}
}

void ChatScanBox::askCancel() {
	askQuestion(tr::lng_chat_scan_cancel_confirm(tr::now), [=] {
		if (_api) {
			_api->cancel();
		}
		clearStats();
		closeBox();
	});
}

void ChatScanBox::askQuestion(const QString &text, Fn<void()> onYes) {
	if (_askOverlay && !_askOverlay->isHidden()) {
		return;
	}
	_askYes = std::move(onYes);
	if (!_askOverlay) {
		_askOverlay = object_ptr<AskOverlay>(this);
		const auto overlay = _askOverlay.data();
		const auto card = object_ptr<AskCard>(overlay);
		const auto content = object_ptr<Ui::VerticalLayout>(card);
		_askLabel = content->add(
			object_ptr<Ui::FlatLabel>(content, st::boxLabel),
			QMargins(
				st::boxPadding.left(),
				st::boxPadding.top(),
				st::boxPadding.right(),
				st::defaultVerticalListSkip));
		const auto row = content->add(
			object_ptr<Ui::RpWidget>(content),
			QMargins(
				st::boxPadding.left(),
				0,
				st::boxPadding.right(),
				st::boxPadding.bottom()),
			style::al_justify);
		_askYesButton = Ui::CreateChild<Ui::RoundButton>(
			row,
			tr::lng_box_yes(),
			st::defaultBoxButton);
		_askNoButton = Ui::CreateChild<Ui::RoundButton>(
			row,
			tr::lng_box_no(),
			st::defaultBoxButton);
		const auto place = [=] {
			row->resize(row->width(), st::defaultBoxButton.height);
			_askNoButton->moveToRight(
				_askYesButton->width() + st::defaultVerticalListSkip,
				0);
			_askYesButton->moveToRight(0, 0);
		};
		row->widthValue() | rpl::on_next(place, row->lifetime());
		row->heightValue() | rpl::on_next(place, row->lifetime());
		_askYesButton->setClickedCallback([=] {
			if (const auto onstack = base::take(_askYes)) {
				onstack();
			}
		});
		_askNoButton->setClickedCallback([=] {
			hideQuestion();
		});
		const auto relayout = [overlay, card = card.data(), content = content.data()] {
			const auto cardWidth = st::boxWidth;
			const auto innerWidth = cardWidth
				- st::boxPadding.left()
				- st::boxPadding.right();
			content->resizeToWidth(innerWidth);
			card->resize(cardWidth, content->height());
			card->move(
				(overlay->width() - cardWidth) / 2,
				(overlay->height() - card->height()) / 2);
		};
		overlay->sizeValue(
		) | rpl::on_next([=](QSize) { relayout(); }, overlay->lifetime());
		content->heightValue(
		) | rpl::on_next([=](int) { relayout(); }, card->lifetime());
	}
	_askLabel->setText(text);
	_askOverlay->setGeometry(rect());
	_askOverlay->raise();
	_askOverlay->show();
}

void ChatScanBox::hideQuestion() {
	if (_askOverlay && !_askOverlay->isHidden()) {
		_askOverlay->hide();
	}
	_askYes = nullptr;
}

void ChatScanBox::clearStats() {
	for (auto &row : _rows) {
		if (row.unique) {
			row.unique->setText(u"—"_q);
		}
		if (row.dups) {
			row.dups->setText(u"—"_q);
		}
	}
	if (_totalUniqueLabel) {
		_totalUniqueLabel->setText(u"—"_q);
	}
	if (_totalDupsLabel) {
		_totalDupsLabel->setText(u"—"_q);
	}
}

void ChatScanBox::askRescan() {
	askQuestion(tr::lng_chat_scan_rescan_confirm(tr::now), [=] {
		hideQuestion();
		auto &db = Core::App().downloadManager().ensureDedupDb();
		db.clearChatScan(_peer->id);
		ChatScan::ForgetScannedPeer(_peer->id);
		setupApi(false);
	});
}

void ChatScanBox::runScanNew() {
	setupApi(true);
}

QString ChatScanBox::etaSuffix(int done, int total) {
	const auto now = int64(crl::now());
	if (_etaSeedTime == 0) {
		_etaSeedTime = now;
		_etaSeedDone = done;
		_etaPerMs = kNominalHashPerMs;
	} else {
		const auto elapsed = now - _etaSeedTime;
		if (elapsed >= kEtaMeasureMs && done > _etaSeedDone) {
			_etaPerMs = double(done - _etaSeedDone) / elapsed;
			_etaSeedTime = now;
			_etaSeedDone = done;
		}
	}
	const auto remaining = std::max(int(0), total - done);
	if (remaining <= 0 || _etaPerMs <= 0.) {
		return QString();
	}
	const auto seconds = int64(remaining / (_etaPerMs * 1000));
	if (seconds <= 0) {
		return QString();
	}
	return u" · "_q
		+ tr::lng_chat_scan_left(
			tr::now,
			lt_time,
			FormatEtaTime(int(seconds)));
}

QString ChatScanBox::unhashedSuffix(const ChatScan::State &state) const {
	return (state.failedHashes > 0)
		? (u" — "_q
			+ tr::lng_chat_scan_unhashed(
				tr::now,
				lt_count,
				state.failedHashes))
		: QString();
}

QString ChatScanBox::floodSuffix(const ChatScan::State &state) const {
	const auto left = state.floodUntilMs - crl::now();
	if (left <= 0) {
		return QString();
	}
	const auto seconds = int((left + 999) / 1000);
	return u" · "_q
		+ tr::lng_chat_scan_waiting(tr::now, lt_count, seconds);
}

void ChatScanBox::refreshFloodSuffix() {
	_floodSuffixTimer.cancel();
	if (!_api) {
		return;
	}
	render(_api->currentState());
}

void ChatScanBox::render(const ChatScan::State &state) {
	using Phase = ChatScan::State::Phase;
	_confirming = false;
	_confirmLabel->hide();
	_revoke->hide();
	if (state.phase != Phase::Scanning && state.phase != Phase::Resolving) {
		_etaSeedTime = 0;
		_etaSeedDone = 0;
		_etaPerMs = 0.;
	}
	if (state.phase == Phase::Finished) {
		_finishedUnhashed = std::max(0, state.hashTotal - state.hashDone);
		auto &db = Core::App().downloadManager().ensureDedupDb();
		renderResults(db.readChatScan(_peer->id));
		return;
	}
	_progressLabel->show();
	_showingResults = false;
	if (state.phase == Phase::Scanning) {
		_statusText = tr::lng_chat_scan_scanning(tr::now) + floodSuffix(state);
		const auto known = int64(state.hashTotal);
		if (const auto server = int64(state.mediaTotal)) {
			_projectedFiles = std::max(server, known);
		} else {
			const auto sampleGoal = std::clamp<int64>(
				state.totalCount / 10,
				kPinAfterMessages,
				kPinSampleCeil);
			const auto sampleEnough = (state.scannedCount >= sampleGoal)
				|| (state.scannedCount >= state.totalCount / 20);
			if (!_pinnedFiles
				&& sampleEnough
				&& known > 0
				&& state.totalCount > state.scannedCount) {
				_pinnedFiles = RoundToFewDigits(
					int64(state.totalCount) * known / state.scannedCount);
			}
			_projectedFiles = std::max(
				std::max(_projectedFiles, _pinnedFiles),
				known);
		}
		const auto resolved = ScanResolvedCount(state);
		_progressText = QString::number(resolved)
			+ u" / "_q
			+ QString::number(_projectedFiles)
			+ unhashedSuffix(state)
			+ etaSuffix(resolved, int(_projectedFiles));
		updateBar(
			tr::lng_chat_scan_pause(tr::now),
			true,
			tr::lng_cancel(tr::now),
			false);
	} else if (state.phase == Phase::Paused) {
		_statusText = tr::lng_chat_scan_paused(tr::now);
		updateBar(
			tr::lng_chat_scan_resume(tr::now),
			true,
			tr::lng_cancel(tr::now),
			false);
	} else if (state.phase == Phase::Resolving) {
		_statusText = tr::lng_chat_scan_resolving(tr::now) + floodSuffix(state);
		_projectedFiles = state.mediaTotal
			? std::max(int64(state.mediaTotal), int64(state.hashTotal))
			: std::max(_projectedFiles, int64(state.hashTotal));
		const auto resolved = ScanResolvedCount(state);
		_progressText = QString::number(resolved)
			+ u" / "_q
			+ QString::number(_projectedFiles)
			+ unhashedSuffix(state)
			+ etaSuffix(resolved, int(_projectedFiles));
		updateBar(
			tr::lng_chat_scan_pause(tr::now),
			true,
			tr::lng_cancel(tr::now),
			false);
	} else if (state.phase == Phase::Failed) {
		_statusText = tr::lng_chat_scan_failed(tr::now)
			+ u": "_q
			+ state.error;
		updateBar(
			tr::lng_chat_scan_resume(tr::now),
			true,
			std::nullopt,
			false);
	} else if (state.phase == Phase::Cancelled) {
		_statusText = tr::lng_cancel(tr::now);
		_progressLabel->hide();
		clearStats();
		updateBar(std::nullopt, true, std::nullopt, false);
	}
	auto uniqueTotal = 0;
	auto uniqueBytes = int64(0);
	auto dupTotal = 0;
	auto dupBytes = int64(0);
	for (auto &row : _rows) {
		const auto i = state.categories.find(row.category);
		if (i == state.categories.end()) {
			if (row.unique) {
				row.unique->setText(u"—"_q);
			}
			if (row.dups) {
				row.dups->setText(u"—"_q);
			}
			continue;
		}
		uniqueTotal += i->second.uniqueCount;
		uniqueBytes += i->second.uniqueBytes;
		dupTotal += i->second.dupCount;
		dupBytes += i->second.dupBytes;
		if (row.unique) {
			row.unique->setText(
				QString::number(i->second.uniqueCount)
				+ u" ("_q
				+ Ui::FormatSizeText(i->second.uniqueBytes)
				+ u")"_q);
		}
		if (row.dups) {
			row.dups->setText(i->second.dupCount > 0
				? QString::number(i->second.dupCount)
				+ u" ("_q
				+ Ui::FormatSizeText(i->second.dupBytes)
				+ u")"_q
				: u"—"_q);
		}
	}
	if (_totalUniqueLabel) {
		_totalUniqueLabel->setText(
			QString::number(uniqueTotal)
			+ u" ("_q
			+ Ui::FormatSizeText(uniqueBytes)
			+ u")"_q);
	}
	if (_totalDupsLabel) {
		_totalDupsLabel->setText(dupTotal > 0
			? QString::number(dupTotal)
			+ u" ("_q
			+ Ui::FormatSizeText(dupBytes)
			+ u")"_q
			: u"—"_q);
	}
	if (state.phase == Phase::Failed || state.phase == Phase::Cancelled) {
		_progressText = QString();
	}
	const auto flooding = _api
		&& (state.phase == Phase::Scanning
			|| state.phase == Phase::Resolving)
		&& (state.floodUntilMs > crl::now());
	if (flooding) {
		_floodSuffixTimer.setCallback([=] {
			refreshFloodSuffix();
		});
		_floodSuffixTimer.callOnce(crl::time(1000));
	}
}

void ChatScanBox::renderResults(const std::vector<Data::ChatScanRow> &rows) {
	auto categories = base::flat_map<int, ChatScan::CategoryTotals>();
	auto dupTotal = 0;
	auto dupBytes = int64(0);
	_deleteFinished = false;
	for (const auto &row : rows) {
		auto &totals = categories[row.category];
		totals.uniqueCount = row.uniqueCount;
		totals.uniqueBytes = row.uniqueBytes;
		totals.dupCount = row.dupCount;
		totals.dupBytes = row.dupBytes;
		dupTotal += row.dupCount;
		dupBytes += row.dupBytes;
		_deleteFinished = _deleteFinished || row.deleteFinished;
	}
	if (_deleteFinished) {
		for (auto &[category, totals] : categories) {
			totals.dupCount = 0;
			totals.dupBytes = 0;
		}
		dupTotal = 0;
		dupBytes = 0;
	}
	showResults(categories, dupTotal, dupBytes);
}

void ChatScanBox::showResults(
		const base::flat_map<int, ChatScan::CategoryTotals> &categories,
		int dupTotal,
		int64 dupBytes) {
	hideQuestion();
	_statusText = tr::lng_chat_scan_results(tr::now)
		+ ((_finishedUnhashed > 0)
			? (u" — "_q + tr::lng_chat_scan_unhashed(
				tr::now,
				lt_count,
				_finishedUnhashed))
			: QString());
	_progressLabel->hide();
	_showingResults = true;
	updateBar(
		tr::lng_chat_scan_delete_short(tr::now),
		(dupTotal > 0 && !_deleteFinished),
		tr::lng_chat_scan_rescan(tr::now),
		true);
	_newButton->setText(rpl::single(tr::lng_chat_scan_new(tr::now)));
	auto totalCount = 0;
	auto totalBytes = int64(0);
	for (auto &row : _rows) {
		const auto i = categories.find(row.category);
		if (i == categories.end()) {
			if (row.unique) {
				row.unique->setText(u"—"_q);
			}
			if (row.dups) {
				row.dups->setText(u"—"_q);
			}
			continue;
		}
		totalCount += i->second.uniqueCount;
		totalBytes += i->second.uniqueBytes;
		if (row.unique) {
			row.unique->setText(
				QString::number(i->second.uniqueCount)
				+ u" ("_q
				+ Ui::FormatSizeText(i->second.uniqueBytes)
				+ u")"_q);
		}
		if (row.dups) {
			row.dups->setText(i->second.dupCount > 0
				? QString::number(i->second.dupCount)
				+ u" ("_q
				+ Ui::FormatSizeText(i->second.dupBytes)
				+ u")"_q
				: u"—"_q);
		}
	}
	if (_totalUniqueLabel) {
		_totalUniqueLabel->setText(
			QString::number(totalCount)
			+ u" ("_q
			+ Ui::FormatSizeText(totalBytes)
			+ u")"_q);
	}
	if (_totalDupsLabel) {
		_totalDupsLabel->setText(dupTotal > 0
			? QString::number(dupTotal)
			+ u" ("_q
			+ Ui::FormatSizeText(dupBytes)
			+ u")"_q
			: u"—"_q);
	}
	_confirming = false;
	_confirmLabel->hide();
	_revoke->hide();
	if (dupTotal > 0 && !_deleteFinished) {
		writeDupReport(false);
	}
}

bool ChatScanBox::canDeleteMessages() const {
	if (_peer->isChannel()) {
		return _peer->asChannel()->canDeleteMessages();
	} else if (_peer->isChat()) {
		return _peer->asChat()->canDeleteMessages();
	}
	return true;
}

void ChatScanBox::startDelete() {
	if (!canDeleteMessages()) {
		Ui::Toast::Show(tr::lng_chat_scan_no_delete_rights(tr::now));
		return;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	const auto total = db.chatScanDupCount(_peer->id, -1);
	if (total <= 0) {
		return;
	}
	_deleteCollectIds.clear();
	_deleteCollectIds.reserve(size_t(total));
	_deleteCollectAbove = 0;
	_deleteCollecting = true;
	deleteCollectStep();
}

void ChatScanBox::deleteCollectStep() {
	constexpr auto kBatch = 5000;
	if (!_deleteCollecting) {
		return;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		_deleteCollecting = false;
		return;
	}
	const auto chunk = db.readChatScanDupsBefore(
		_peer->id,
		-1,
		kBatch,
		MsgId(_deleteCollectAbove));
	for (const auto &dup : chunk) {
		_deleteCollectAbove = int64(dup.msgId.bare);
		_deleteCollectIds.push_back(int32(dup.msgId.bare));
	}
	if (int(chunk.size()) == kBatch) {
		_deleteCollectTimer.setCallback([=] {
			deleteCollectStep();
		});
		_deleteCollectTimer.callOnce(0);
		return;
	}
	_deleteCollecting = false;
	auto ids = std::move(_deleteCollectIds);
	_deleteCollectIds.clear();
	askQuestion(tr::lng_chat_scan_delete_confirm(tr::now),
		[=, ids = std::move(ids)]() mutable {
			hideQuestion();
			beginDelete(std::move(ids));
		});
}

void ChatScanBox::beginDelete(std::vector<int32> ids) {
	_deleteIds = std::move(ids);
	_confirming = true;
	_confirmText = tr::lng_chat_scan_confirm(
		tr::now,
		lt_count,
		int(_deleteIds.size()));
	_confirmLabel->show();
	auto showRevoke = true;
	auto revokeDefault = false;
	if (_peer->isSelf()) {
		showRevoke = false;
	} else if (_peer->isChannel()) {
		showRevoke = false;
	} else if (_peer->isChat()) {
		revokeDefault = _peer->asChat()->canDeleteMessages();
	}
	_revoke->setChecked(revokeDefault);
	_revoke->setVisible(showRevoke);
	updateBar(
		tr::lng_chat_scan_delete_short(tr::now),
		true,
		tr::lng_cancel(tr::now),
		false);
}

void ChatScanBox::confirmDelete() {
	_confirming = false;
	_confirmLabel->hide();
	_revoke->hide();
	_deleting = true;
	_deletePos = 0;
	_deleteTotal = int(_deleteIds.size());
	_statusText = tr::lng_chat_scan_deleting(tr::now);
	_progressLabel->show();
	updateBar(std::nullopt, true, tr::lng_cancel(tr::now), false);
	sendNextDeleteBatch();
}

void ChatScanBox::sendNextDeleteBatch() {
	if (!_deleting || _deletePos >= _deleteIds.size()) {
		finishDelete(QString());
		return;
	}
	const auto count = std::min(
		size_t(100),
		_deleteIds.size() - _deletePos);
	auto ids = QVector<MTPint>();
	ids.reserve(int(count));
	for (auto i = size_t(0); i < count; ++i) {
		ids.push_back(MTP_int(_deleteIds[_deletePos + i]));
	}
	_progressText = QString::number(_deletePos)
		+ u" / "_q
		+ QString::number(_deleteTotal);
	const auto done = [=](const MTPmessages_AffectedMessages &result) {
		if (!_deleting) {
			return;
		}
		auto &db = Core::App().downloadManager().ensureDedupDb();
		db.beginTransaction();
		for (auto i = size_t(0); i < count; ++i) {
			const auto msgId = MsgId(_deleteIds[_deletePos + i]);
			db.removeChatScanDup(_peer->id, msgId);
			db.removeChatScanItem(_peer->id, msgId);
		}
		db.commitTransaction();
		_deletePos += count;
		sendNextDeleteBatch();
	};
	const auto failed = [=](const MTP::Error &error) {
		if (!_deleting) {
			return;
		}
		const auto &type = error.type();
		if (type.startsWith(u"FLOOD_WAIT_"_q)) {
			const auto seconds = std::max(
				type.mid(u"FLOOD_WAIT_"_q.size()).toInt(),
				1);
			_statusText = tr::lng_chat_scan_waiting(
				tr::now,
				lt_count,
				seconds);
			_floodTimer.setCallback([=] {
				sendNextDeleteBatch();
			});
			_floodTimer.callOnce(seconds * crl::time(1000));
		} else {
			finishDelete(type);
		}
	};
	if (_peer->isChannel()) {
		_deleteApi.request(MTPchannels_DeleteMessages(
			_peer->asChannel()->inputChannel(),
			MTP_vector<MTPint>(ids)
		)).done(std::move(done)).fail(std::move(failed)).send();
	} else {
		using Flag = MTPmessages_DeleteMessages::Flag;
		_deleteApi.request(MTPmessages_DeleteMessages(
			MTP_flags(_revoke->checked() ? Flag::f_revoke : Flag(0)),
			MTP_vector<MTPint>(ids)
		)).done(std::move(done)).fail(std::move(failed)).send();
	}
}

void ChatScanBox::finishDelete(const QString &error) {
	_deleting = false;
	_floodTimer.cancel();
	if (!error.isEmpty()) {
		if (error == u"CHAT_ADMIN_REQUIRED"_q
			|| error == u"CHAT_WRITE_FORBIDDEN"_q
			|| error == u"MESSAGE_DELETE_FORBIDDEN"_q) {
			Ui::Toast::Show(tr::lng_chat_scan_no_delete_rights(tr::now));
		}
		auto &db = Core::App().downloadManager().ensureDedupDb();
		_statusText = error;
		renderResults(db.readChatScan(_peer->id));
		return;
	}
	settleDelete(0);
}

void ChatScanBox::settleDelete(int attempt) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	const auto peerId = _peer->id;
	if (db.chatScanDupCount(peerId, -1) == 0) {
		writeDupReport(true);
		db.markChatDeleteFinished(peerId);
		ChatScan::ForgetScannedPeer(peerId);
		ChatScan::RememberScannedPeer(peerId);
		_statusText = tr::lng_chat_scan_done(tr::now);
		renderResults(db.readChatScan(peerId));
		return;
	}
	if (attempt >= 10) {
		renderResults(db.readChatScan(peerId));
		return;
	}
	_settleTimer.setCallback([=] {
		settleDelete(attempt + 1);
	});
	_settleTimer.callOnce(crl::time(3000));
}

QString ChatScanBox::scanReportPath() {
	const auto peerId = _peer->id;
	const auto chatId = _peer->isChannel()
		? int64(peerToChannel(peerId).bare)
		: _peer->isChat()
		? int64(peerToChat(peerId).bare)
		: _peer->isUser()
		? int64(peerToUser(peerId).bare)
		: int64(peerId.value);
	auto folder = Core::App().settings().downloadPath();
	if (folder.isEmpty()) {
		folder = File::DefaultDownloadPath(_session);
	}
	if (!folder.endsWith(u"/"_q)) {
		folder += u"/"_q;
	}
	auto safe = _peer->name();
	if (safe.isEmpty()) {
		safe = u"chat"_q;
	}
	for (const auto &bad : {
			u"/"_q,
			u"\\"_q,
			u":"_q,
			u"*"_q,
			u"?"_q,
			u"\""_q,
			u"<"_q,
			u">"_q,
			u"|"_q }) {
		safe = safe.replace(bad, u"_"_q);
	}
	QDir().mkpath(folder);
	return folder
		+ QString::number(chatId)
		+ u"_"_q
		+ safe
		+ u"_duplicates.txt"_q;
}

void ChatScanBox::upsertScanReport(
		const QString &status,
		const std::map<int, std::pair<int, int64>> &uniques,
		bool append) {
	if (_reportJob) {
		_reportQueuedStatus = status;
		_reportQueuedUniques = uniques;
		_reportQueuedAppend = append;
		return;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	auto job = std::make_unique<ReportJob>();
	job->status = status;
	job->path = scanReportPath();
	job->uniques = uniques;
	job->parsing = append;
	for (const auto &row : _rows) {
		job->names[row.category] = row.name;
		job->categories.push_back(row.category);
	}
	_reportJob = std::move(job);
	reportStep();
}

void ChatScanBox::reportStep() {
	constexpr auto kParseLines = 20000;
	constexpr auto kDupBatch = 5000;
	if (!_reportJob) {
		return;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		_reportJob.reset();
		return;
	}
	auto &job = *_reportJob;
	if (job.parsing) {
		if (!job.file) {
			job.file = std::make_unique<QFile>(job.path);
			if (job.file->open(QIODevice::ReadOnly | QIODevice::Text)) {
				job.stream = std::make_unique<QTextStream>(job.file.get());
			}
		}
		auto read = 0;
		while (job.stream && !job.stream->atEnd() && read < kParseLines) {
			const auto raw = job.stream->readLine();
			++read;
			if (raw.startsWith(u"Date: "_q)) {
				job.dateLine = raw;
				continue;
			}
			for (const auto &entry : job.names) {
				if (!entry.second.isEmpty()
					&& raw.startsWith(entry.second + u" — "_q)) {
					job.currentCategory = entry.first;
					break;
				}
			}
			const auto msgPos = raw.lastIndexOf(u"msg "_q);
			if (job.currentCategory < 0 || msgPos < 0) {
				continue;
			}
			auto ok = false;
			const auto id = raw.mid(msgPos + 4).simplified().toLongLong(&ok);
			auto &known = job.listed[job.currentCategory];
			if (!ok || id <= 0 || known.contains(id)) {
				continue;
			}
			known[id] = {
				raw.startsWith(u"[x] "_q) ? raw.mid(4) : raw,
				int64(0),
				false };
		}
		if (job.stream && !job.stream->atEnd()) {
			_reportTimer.setCallback([=] {
				reportStep();
			});
			_reportTimer.callOnce(0);
			return;
		}
		job.stream.reset();
		job.file.reset();
		job.parsing = false;
	}
	while (job.categoryPos < job.categories.size()) {
		const auto category = job.categories[job.categoryPos];
		const auto chunk = db.readChatScanDupsBefore(
			_peer->id,
			category,
			kDupBatch,
			MsgId(job.pageAbove));
		for (const auto &dup : chunk) {
			job.pageAbove = int64(dup.msgId.bare);
			job.listed[category][int64(dup.msgId.bare)] = {
				(dup.filename.isEmpty()
					? job.names.at(category)
					: dup.filename)
				+ u" | "_q
				+ FormatSizeWithBytes(dup.size)
				+ u" | msg "_q
				+ QString::number(dup.msgId.bare),
				dup.size,
				true };
		}
		if (int(chunk.size()) == kDupBatch) {
			_reportTimer.setCallback([=] {
				reportStep();
			});
			_reportTimer.callOnce(0);
			return;
		}
		++job.categoryPos;
		job.pageAbove = 0;
	}
	renderReport();
}

void ChatScanBox::renderReport() {
	auto &job = *_reportJob;
	const auto status = job.status;
	const auto uniques = job.uniques;
	const auto deletedText = [](int deleted) {
		return deleted > 0
			? u" + %1 deleted"_q.arg(deleted)
			: QString();
	};
	const auto chatId = _peer->isChannel()
		? int64(peerToChannel(_peer->id).bare)
		: _peer->isChat()
		? int64(peerToChat(_peer->id).bare)
		: _peer->isUser()
		? int64(peerToUser(_peer->id).bare)
		: int64(_peer->id.value);
	QFile file(job.path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return;
	}
	auto stream = QTextStream(&file);
	stream << u"Status: "_q << status << u"\n"_q;
	stream << u"Chat: "_q
		<< _peer->name()
		<< u" ("_q
		<< chatId
		<< u")\n"_q;
	stream << u"Date: "_q
		<< (job.dateLine.isEmpty()
			? QDate::currentDate().toString(Qt::ISODate)
			: job.dateLine.mid(6))
		<< u"\n"_q;
	struct ReportRow {
		QString name;
		QString unique;
		QString dups;
	};
	auto table = std::vector<ReportRow>();
	auto uniqueTotal = 0;
	auto uniqueTotalBytes = int64(0);
	auto dupTotal = 0;
	auto dupBytes = int64(0);
	for (const auto &row : _rows) {
		const auto found = job.listed.find(row.category);
		if (found == job.listed.end() || found->second.empty()) {
			continue;
		}
		auto dups = 0;
		auto bytes = int64(0);
		auto deletedCount = 0;
		for (const auto &[id, item] : found->second) {
			if (item.inDb) {
				++dups;
				bytes += item.size;
			} else {
				++deletedCount;
			}
		}
		const auto unique = uniques.find(row.category);
		const auto uniqueCount = (unique != uniques.end())
			? unique->second.first
			: 0;
		const auto uniqueBytes = (unique != uniques.end())
			? unique->second.second
			: int64(0);
		uniqueTotal += uniqueCount;
		uniqueTotalBytes += uniqueBytes;
		dupTotal += dups;
		dupBytes += bytes;
		auto entry = ReportRow();
		entry.name = job.names.at(row.category);
		entry.unique = QString::number(uniqueCount)
			+ u" ("_q
			+ FormatSizeWithBytes(uniqueBytes)
			+ u")"_q;
		entry.dups = QString::number(dups)
			+ u" ("_q
			+ FormatSizeWithBytes(bytes)
			+ u")"_q
			+ deletedText(deletedCount);
		table.push_back(std::move(entry));
	}
	const auto totalUnique = QString::number(uniqueTotal)
		+ u" ("_q
		+ FormatSizeWithBytes(uniqueTotalBytes)
		+ u")"_q;
	const auto totalDups = QString::number(dupTotal)
		+ u" ("_q
		+ FormatSizeWithBytes(dupBytes)
		+ u")"_q;
	const auto pad = [](const QString &text, int width) {
		return (text + QString(width, QChar(u' '))).left(width);
	};
	auto nameWidth = int(QString(u"Total"_q).size());
	auto uniqueWidth = int(QString(u"Unique files"_q).size());
	for (const auto &row : table) {
		nameWidth = std::max(nameWidth, int(row.name.size()));
		uniqueWidth = std::max(uniqueWidth, int(row.unique.size()));
	}
	uniqueWidth = std::max(uniqueWidth, int(totalUnique.size()));
	stream << u"\n"_q
		<< pad(QString(), nameWidth)
		<< u"  "_q
		<< pad(u"Unique files"_q, uniqueWidth)
		<< u"  Duplicate files\n"_q;
	for (const auto &row : table) {
		stream << pad(row.name, nameWidth)
			<< u"  "_q
			<< pad(row.unique, uniqueWidth)
			<< u"  "_q
			<< row.dups
			<< u"\n"_q;
	}
	stream << pad(u"Total"_q, nameWidth)
		<< u"  "_q
		<< pad(totalUnique, uniqueWidth)
		<< u"  "_q
		<< totalDups
		<< u"\n"_q;
	for (const auto &row : _rows) {
		const auto found = job.listed.find(row.category);
		if (found == job.listed.end() || found->second.empty()) {
			continue;
		}
		auto dups = 0;
		auto bytes = int64(0);
		auto deletedCount = 0;
		for (const auto &[id, item] : found->second) {
			if (item.inDb) {
				++dups;
				bytes += item.size;
			} else {
				++deletedCount;
			}
		}
		stream << u"\n"_q
			<< job.names.at(row.category)
			<< u" — "_q
			<< dups
			<< u" dups ("_q
			<< FormatSizeWithBytes(bytes)
			<< u")"_q
			<< deletedText(deletedCount)
			<< u":\n"_q;
		for (const auto &[id, item] : found->second) {
			stream << (item.inDb ? QString() : u"[x] "_q)
				<< item.line
				<< u"\n"_q;
		}
	}
	stream.flush();
	_reportJob.reset();
	if (!_reportQueuedStatus.isEmpty()) {
		const auto next = _reportQueuedStatus;
		const auto nextUniques = _reportQueuedUniques;
		const auto nextAppend = _reportQueuedAppend;
		_reportQueuedStatus.clear();
		_reportQueuedUniques.clear();
		_reportQueuedAppend = false;
		upsertScanReport(next, nextUniques, nextAppend);
	}
}

void ChatScanBox::writeDupReport(bool deleted) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	auto uniques = std::map<int, std::pair<int, int64>>();
	for (const auto &row : db.readChatScan(_peer->id)) {
		uniques[row.category] = {
			row.uniqueCount,
			row.uniqueBytes };
	}
	upsertScanReport(
		deleted ? u"Deleted"_q : u"Not deleted"_q,
		uniques,
		deleted || _reportAppend);
}

void ChatScanBox::writePausedReport() {
	if (!_api) {
		return;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	auto uniques = std::map<int, std::pair<int, int64>>();
	const auto state = _api->currentState();
	for (const auto &entry : state.categories) {
		uniques[entry.first] = {
			entry.second.uniqueCount,
			entry.second.uniqueBytes };
	}
	upsertScanReport(u"Paused"_q, uniques, _reportAppend);
}
