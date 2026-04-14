/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/view/export_view_panel_controller.h"

#include <crl/crl.h>
#include "data/data_peer_id.h"
#include "data/data_peer.h"
#include "tl/tl_basic_types.h"
#include <QtCore/QDir>

#include "export/view/export_view_settings.h"
#include "export/view/export_view_progress.h"
#include "export/export_progress.h"
#include "export/export_manager.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/separate_panel.h"
#include "ui/wrap/padding_wrap.h"
#include "mtproto/mtproto_config.h"
#include "ui/boxes/confirm_box.h"
#include "lang/lang_keys.h"
#include "storage/storage_account.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "main/main_session.h"
#include "data/data_session.h"
#include "base/platform/base_platform_info.h"
#include "base/unixtime.h"
#include "base/qt/qt_common_adapters.h"
#include "boxes/abstract_box.h" // Ui::show().
#include "styles/style_export.h"
#include "styles/style_layers.h"

namespace Export {
namespace View {
namespace {

constexpr auto kSaveSettingsTimeout = crl::time(1000);

class SuggestBox : public Ui::BoxContent {
public:
	SuggestBox(QWidget*, not_null<Main::Session*> session);

protected:
	void prepare() override;

private:
	const not_null<Main::Session*> _session;

};

SuggestBox::SuggestBox(QWidget*, not_null<Main::Session*> session)
: _session(session) {
}

void SuggestBox::prepare() {
	setTitle(tr::lng_export_suggest_title());

	addButton(tr::lng_box_ok(), [=] {
		const auto session = _session;
		closeBox();
		Core::App().exportManager().start(
			session,
			session->local().readExportSettings().singlePeer);
	});
	addButton(tr::lng_export_suggest_cancel(), [=] { closeBox(); });
	setCloseByOutsideClick(false);

	const auto content = Ui::CreateChild<Ui::FlatLabel>(
		this,
		tr::lng_export_suggest_text(tr::now),
		st::boxLabel);
	widthValue(
	) | rpl::on_next([=](int width) {
		const auto contentWidth = width
			- st::boxPadding.left()
			- st::boxPadding.right();
		content->resizeToWidth(contentWidth);
		content->moveToLeft(st::boxPadding.left(), 0);
	}, content->lifetime());
	content->heightValue(
	) | rpl::on_next([=](int height) {
		setDimensions(st::boxWidth, height + st::boxPadding.bottom());
	}, content->lifetime());
}

} // namespace

Environment PrepareEnvironment(not_null<Main::Session*> session) {
	auto result = Environment();
	result.internalLinksDomain = session->serverConfig().internalLinksDomain;
	result.aboutTelegram = tr::lng_export_about_telegram(tr::now).toUtf8();
	result.aboutContacts = tr::lng_export_about_contacts(tr::now).toUtf8();
	result.aboutFrequent = tr::lng_export_about_frequent(tr::now).toUtf8();
	result.aboutSessions = tr::lng_export_about_sessions(tr::now).toUtf8();
	result.aboutWebSessions = tr::lng_export_about_web_sessions(tr::now).toUtf8();
	result.aboutChats = tr::lng_export_about_chats(tr::now).toUtf8();
	result.aboutLeftChats = tr::lng_export_about_left_chats(tr::now).toUtf8();
	return result;
}

base::weak_qptr<Ui::BoxContent> SuggestStart(not_null<Main::Session*> session) {
	ClearSuggestStart(session);
	return Ui::show(
		Box<SuggestBox>(session),
		Ui::LayerOption::KeepOther).get();
}

void ClearSuggestStart(not_null<Main::Session*> session) {
	session->data().clearExportSuggestion();

	auto settings = session->local().readExportSettings();
	if (settings.availableAt) {
		settings.availableAt = 0;
		session->local().writeExportSettings(settings);
	}
}

bool IsDefaultPath(not_null<Main::Session*> session, const QString &path) {
	const auto check = [](const QString &value) {
		const auto result = value.endsWith('/')
			? value.mid(0, value.size() - 1)
			: value;
		return Platform::IsWindows() ? result.toLower() : result;
	};
	return (check(path) == check(File::DefaultDownloadPath(session)));
}

void ResolveSettings(not_null<Main::Session*> session, Settings &settings) {
	if (settings.path.isEmpty()) {
		settings.path = File::DefaultDownloadPath(session);
		settings.forceSubPath = true;
	} else {
		settings.forceSubPath = IsDefaultPath(session, settings.path);
	}
	if (!settings.onlySinglePeer()) {
		settings.singlePeerFrom = settings.singlePeerTill = 0;
	} else {
		// For single peer exports, reset to clean defaults:
		// - No media types selected by default
		// - 8 MB size limit by default
		settings.media.types = MediaSettings::Types(0);
		settings.media.sizeLimit = 8 * 1024 * 1024;
	}
}

PanelController::PanelController(
	not_null<Main::Session*> session,
	not_null<Controller*> process)
: _session(session)
, _process(process)
, _settings(
	std::make_unique<Settings>(_session->local().readExportSettings()))
, _saveSettingsTimer([=] { saveSettings(); }) {
	ResolveSettings(session, *_settings);

	_process->state(
	) | rpl::on_next([=](State &&state) {
		updateState(std::move(state));
	}, _lifetime);
}

PanelController::~PanelController() {
	if (_saveSettingsTimer.isActive()) {
		saveSettings();
	}
	if (_panel) {
		_panel->hideLayer(anim::type::instant);
	}
}

void PanelController::activatePanel() {
	if (_panel) {
		_panel->showAndActivate();
	}
}

void PanelController::createPanel() {
	const auto singlePeer = _settings->onlySinglePeer();
	const auto singleTopic = _settings->onlySingleTopic();
	_panel = base::make_unique_q<Ui::SeparatePanel>(Ui::SeparatePanelArgs{
		.onAllSpaces = true,
	});
	_panel->setTitle((singleTopic
		? tr::lng_export_header_topic
		: singlePeer
		? tr::lng_export_header_chats
		: tr::lng_export_title)());
	_panel->setInnerSize(st::exportPanelSize);
	_panel->closeRequests(
	) | rpl::on_next([=] {
		LOG(("Export Info: Panel Hide By Close."));
		_panel->hideGetDuration();
	}, _panel->lifetime());
	_panelCloseEvents.fire(_panel->closeEvents());

	showSettings();
}

void PanelController::showSettings() {
	auto settings = base::make_unique_q<SettingsWidget>(
		_panel,
		_session,
		*_settings);
	const auto settingsRaw = settings.get();
	settingsRaw->setShowBoxCallback([=](object_ptr<Ui::BoxContent> box) {
		_panel->showBox(
			std::move(box),
			Ui::LayerOption::KeepOther,
			anim::type::normal);
	});

	settingsRaw->scanClicks(
	) | rpl::on_next([=] {
		if (settingsRaw->readData().media.types == MediaSettings::Types(0)) {
			return; // Do nothing if no file type selected
		}
		settingsRaw->setScanning(true);
		_panel->setTitle(tr::lng_export_scanning());
		_panel->setHideOnDeactivate(true);
		_process->runScan(*_settings, PrepareEnvironment(_session));
	}, settingsRaw->lifetime());

	settingsRaw->exportClicks(
	) | rpl::on_next([=]() {
		if (settingsRaw->readData().media.types == MediaSettings::Types(0)) {
			return; // Do nothing if no file type selected
		}
		_panel->setTitle(tr::lng_export_progress_title());
		showProgress();
		_process->startExport(*_settings, PrepareEnvironment(_session));
	}, settingsRaw->lifetime());

	settingsRaw->resumeClicks(
	) | rpl::on_next([=]() {
		if (settingsRaw->readData().media.types == MediaSettings::Types(0)) {
			return; // Do nothing if no file type selected
		}
		_panel->setTitle(tr::lng_export_progress_title());
		showProgress();
		_process->resumeExport(*_settings, PrepareEnvironment(_session));
	}, settingsRaw->lifetime());

	settingsRaw->cancelClicks(
	) | rpl::on_next([=] {
		const auto scanning = settingsRaw->isScanning();
		const auto hasResults = settingsRaw->hasScanResults();
		if (scanning) {
			settingsRaw->resetToDefault();
			_process->cancelExportFast();
		} else if (hasResults) {
			settingsRaw->resetToDefault();
			_process->clearResults();
		} else {
			LOG(("Export Info: Panel Hide By Cancel."));
			settingsRaw->resetToDefault(); // Reset on cancel as requested
			_panel->hideGetDuration();
		}
		_panel->setTitle(tr::lng_export_title());
	}, settingsRaw->lifetime());

	settingsRaw->scanInvalidated(
	) | rpl::on_next([=] {
		_process->clearResults();
	}, settingsRaw->lifetime());

	settingsRaw->changes(
	) | rpl::on_next([=](Settings &&settings) {
		*_settings = std::move(settings);
	}, settingsRaw->lifetime());

	// Check for existing export to show Resume button
	// We do this on the PanelController side because it has the correct path and peer info
	LOG(("Export Resume: Starting check - peerId=%1, path='%2', isSinglePeer=%3")
		.arg(_settings->singlePeerId).arg(_settings->path).arg(_settings->onlySinglePeer()));

	checkExistingExport([=](bool hasExisting, std::optional<Settings> restored) {
		LOG(("Export Resume: Check result - hasExisting=%1").arg(hasExisting ? "true" : "false"));
		crl::on_main([=] {
			if (_panel) {
				if (auto settingsWidget = dynamic_cast<SettingsWidget*>(_panel->inner())) {
					settingsWidget->setHasExistingExport(hasExisting);
					if (hasExisting && restored) {
						LOG(("Export Resume: Restoring persisted settings"));
						// Restore message types and size limit from previous session
						auto data = settingsWidget->readData();
						data.media.types = restored->media.types;
						data.media.sizeLimit = restored->media.sizeLimit;
						data.media.extensionFilterMode = restored->media.extensionFilterMode;
						data.media.extensionFilter = restored->media.extensionFilter;
						data.types = restored->types;
						data.fullChats = restored->fullChats;
						data.format = restored->format;

						// We need a way to update the widget's internal data and UI
						// SettingsWidget constructor calls setupContent, so we might
						// need to fire a change or similar.
						// For now, let's use resetToDefault style approach if possible,
						// or just modify _settings and hope for the best if it's already bound.
						*_settings = data;
						// Since SettingsWidget uses rpl to track _internal_data, 
						// we might need a method to set it.
						// I'll add a 'restoreSettings' method to SettingsWidget.
						settingsWidget->restoreSettings(data);
					}
				}
			}
		});
	});

	_panel->showInner(std::move(settings));
}

void PanelController::checkExistingExport(Fn<void(bool, std::optional<Settings>)> callback) const {
	if (!_settings->onlySinglePeer()) {
		callback(false, std::nullopt);
		return;
	}

	const auto targetPeerId = _settings->singlePeerId;
	if (targetPeerId == 0) {
		LOG(("Export Resume: No peer ID available for existing export check"));
		callback(false, std::nullopt);
		return;
	}

	auto downloadPath = _settings->path;
	if (downloadPath.isEmpty()) {
		LOG(("Export Resume: No download path available for existing export check"));
		callback(false, std::nullopt);
		return;
	}

	// Normalize path separators for Qt
	downloadPath = QDir::toNativeSeparators(downloadPath);

	LOG(("Export Resume: Checking for existing export in '%1' for peer ID %2")
		.arg(downloadPath).arg(targetPeerId));

	// Search for any ChatExport folder in the download directory containing this peer ID
	const auto rawId = std::abs(targetPeerId); // Raw ID without sign
	const QDir parentDir(downloadPath);
	const auto entries = parentDir.entryList(QStringList() << "ChatExport_*", QDir::Dirs | QDir::NoDotAndDotDot);
	for (const auto &entry : entries) {
		const auto fullIdStr = QString::number(targetPeerId);
		const auto rawIdStr = QString::number(rawId);

		if (entry.contains(fullIdStr) || entry.contains(rawIdStr)) {
			const auto folderPath = downloadPath + '/' + entry;
			LOG(("Export Resume: Found matching folder '%1'").arg(folderPath));

			// Check for progress.json
			const auto progressPath = ExportProgress::progressFilePath(folderPath);
			auto progress = ExportProgress::load(progressPath);
			if (progress) {
				LOG(("Export Resume: Found progress file, enabling resume"));
				callback(true, progress->settings);
				return;
			}

			// Also check for leftover .partial files from interrupted export
			const QDir folderDir(folderPath);
			const auto partialFiles = folderDir.entryList(QStringList() << "*.partial", QDir::Files | QDir::NoDotAndDotDot);
			if (!partialFiles.isEmpty()) {
				LOG(("Export Resume: Found %1 partial files, enabling resume").arg(partialFiles.size()));
				callback(true, std::nullopt);
				return;
			}
		}
	}

	LOG(("Export Resume: No existing export found"));
	callback(false, std::nullopt);
}
void PanelController::showError(const ApiErrorState &error) {
	LOG(("Export Info: API Error '%1'.").arg(error.data.type()));

	if (error.data.type() == u"TAKEOUT_INVALID"_q) {
		showError(tr::lng_export_invalid(tr::now));
	} else if (error.data.type().startsWith(u"TAKEOUT_INIT_DELAY_"_q)) {
		const auto seconds = std::max(base::StringViewMid(
			error.data.type(),
			u"TAKEOUT_INIT_DELAY_"_q.size()).toInt(), 1);
		const auto now = QDateTime::currentDateTime();
		const auto when = now.addSecs(seconds);
		const auto hours = seconds / 3600;
		const auto hoursText = [&] {
			if (hours <= 0) {
				return tr::lng_export_delay_less_than_hour(tr::now);
			}
			return tr::lng_hours(tr::now, lt_count, hours);
		}();
		showError(tr::lng_export_delay(
			tr::now,
			lt_hours,
			hoursText,
			lt_date,
			langDateTimeFull(when)));

		_settings->availableAt = base::unixtime::now() + seconds;
		_saveSettingsTimer.callOnce(kSaveSettingsTimeout);

		_session->data().suggestStartExport(_settings->availableAt);
	} else {
		showCriticalError("API Error happened :(\n"
			+ QString::number(error.data.code()) + ": " + error.data.type()
			+ "\n" + error.data.description());
	}
}

void PanelController::showError(const OutputErrorState &error) {
	showCriticalError("Disk Error happened :(\n"
		"Could not write path:\n" + error.path);
}

void PanelController::showCriticalError(const QString &text) {
	auto container = base::make_unique_q<Ui::PaddingWrap<Ui::FlatLabel>>(
		_panel.get(),
		object_ptr<Ui::FlatLabel>(
			_panel.get(),
			text,
			st::exportErrorLabel),
		style::margins(0, st::exportPanelSize.height() / 4, 0, 0));
	container->widthValue(
	) | rpl::on_next([label = container->entity()](int width) {
		label->resize(width, label->height());
	}, container->lifetime());

	_panel->showInner(std::move(container));
	_panel->setHideOnDeactivate(false);
}

void PanelController::showError(const QString &text) {
	auto box = Ui::MakeInformBox(text);
	const auto weak = base::make_weak(box.data());
	const auto hidden = _panel->isHidden();
	_panel->showBox(
		std::move(box),
		Ui::LayerOption::CloseOther,
		hidden ? anim::type::instant : anim::type::normal);
	weak->setCloseByEscape(false);
	weak->setCloseByOutsideClick(false);
	weak->boxClosing(
	) | rpl::on_next([=] {
		LOG(("Export Info: Panel Hide By Error: %1.").arg(text));
		_panel->hideGetDuration();
	}, weak->lifetime());
	if (hidden) {
		_panel->showAndActivate();
	}
	_panel->setHideOnDeactivate(false);
}

void PanelController::showProgress() {
	_settings->availableAt = 0;
	ClearSuggestStart(_session);

	_panel->setTitle(tr::lng_export_progress_title());

	_state = ProcessingState();
	auto progress = base::make_unique_q<ProgressWidget>(
		_panel.get(),
		rpl::single(
			ContentFromState(ProcessingState())
		) | rpl::then(progressState()));

	progress->skipFileClicks(
	) | rpl::on_next([=](uint64 randomId) {
		_process->skipFile(randomId);
	}, progress->lifetime());

	progress->cancelClicks(
	) | rpl::on_next([=] {
		stopWithConfirmation();
	}, progress->lifetime());

	progress->doneClicks(
	) | rpl::on_next([=] {
		if (const auto finished = std::get_if<FinishedState>(&_state)) {
			File::ShowInFolder(finished->path);
			LOG(("Export Info: Panel Hide By Done: %1."
				).arg(finished->path));
			_panel->hideGetDuration();
		}
	}, progress->lifetime());

	_panel->showInner(std::move(progress));
	_panel->setHideOnDeactivate(true);
}

void PanelController::stopWithConfirmation(Fn<void()> callback) {
	if (v::is<FinishedState>(_state) || v::is<PasswordCheckState>(_state)) {
		LOG(("Export Info: Stop Panel Without Confirmation."));
		stopExport();
		if (callback) {
			callback();
		}
		return;
	}
	const auto weak = std::make_shared<base::weak_qptr<Ui::GenericBox>>();
	auto stop = [=, callback = std::move(callback)]() mutable {
		if (const auto strong = weak->get()) {
			strong->closeBox();
		}
		if (auto saved = std::move(callback)) {
			LOG(("Export Info: Stop Panel With Confirmation."));
			stopExport();
			saved();
		} else {
			_process->cancelExportFast();
		}
	};
	const auto hidden = _panel->isHidden();
	const auto old = _confirmStopBox;
	auto box = Ui::MakeConfirmBox({
		.text = tr::lng_export_sure_stop(),
		.confirmed = std::move(stop),
		.confirmText = tr::lng_export_stop(),
		.confirmStyle = &st::attentionBoxButton,
	});
	_confirmStopBox = box.data();
	*weak = box.data();
	_panel->showBox(
		std::move(box),
		Ui::LayerOption::CloseOther,
		hidden ? anim::type::instant : anim::type::normal);
	if (hidden) {
		_panel->showAndActivate();
	}
	if (old) {
		old->closeBox();
	}
}

void PanelController::stopExport() {
	_stopRequested = true;
	_panel->showAndActivate();
	LOG(("Export Info: Panel Hide By Stop"));
	_panel->hideGetDuration();
}

bool PanelController::isScanning() const {
	if (const auto state = std::get_if<ProcessingState>(&_state)) {
		return state->isScanning;
	}
	return false;
}

rpl::producer<> PanelController::stopRequests() const {
	return _panelCloseEvents.events(
	) | rpl::flatten_latest(
	) | rpl::filter([=] {
		return !v::is<ProcessingState>(_state) || _stopRequested;
	});
}

void PanelController::fillParams(const PasswordCheckState &state) {
	_settings->singlePeer = state.singlePeer;
	_settings->singlePeerName = state.singlePeerName;
	_settings->singlePeerId = state.singlePeerId;

	// For single peer exports, always extract fresh from the current peer
	// (Old settings may have leftover values from a different chat)
	if (_settings->onlySinglePeer()) {
		_settings->singlePeerId = 0;
		_settings->singlePeerName.clear();
	}

	if (_settings->singlePeerId == 0) {
		state.singlePeer.match(
			[&](const MTPDinputPeerUser &data) {
				_settings->singlePeerId = static_cast<int64>(data.vuser_id().v);
			},
			[&](const MTPDinputPeerUserFromMessage &data) {
				_settings->singlePeerId = static_cast<int64>(data.vuser_id().v);
			},
			[&](const MTPDinputPeerChat &data) {
				_settings->singlePeerId = static_cast<int64>(data.vchat_id().v);
			},
			[&](const MTPDinputPeerChannel &data) {
				// Human-readable channel ID: -100xxxxxxxxxx
				_settings->singlePeerId = -(1000000000000LL + static_cast<int64>(data.vchannel_id().v));
			},
			[&](const MTPDinputPeerChannelFromMessage &data) {
				_settings->singlePeerId = -(1000000000000LL + static_cast<int64>(data.vchannel_id().v));
			},
			[&](const MTPDinputPeerSelf &data) {
				_settings->singlePeerId = _session->userPeerId().value;
			},
			[&](const MTPDinputPeerEmpty &data) {
				// No peer info available
			});

		// Get name from session peer data
		if (_settings->singlePeerId != 0) {
			const auto peerIdRaw = state.singlePeer.match(
				[&](const MTPDinputPeerUser &data) { return peerFromUser(data.vuser_id().v); },
				[&](const MTPDinputPeerUserFromMessage &data) { return peerFromUser(data.vuser_id().v); },
				[&](const MTPDinputPeerChat &data) { return peerFromChat(data.vchat_id().v); },
				[&](const MTPDinputPeerChannel &data) { return peerFromChannel(data.vchannel_id().v); },
				[&](const MTPDinputPeerChannelFromMessage &data) { return peerFromChannel(data.vchannel_id().v); },
				[&](const MTPDinputPeerSelf &data) { return _session->userPeerId(); },
				[&](const MTPDinputPeerEmpty &data) { return PeerId(0); });
			
			if (peerIdRaw) {
				const auto peer = _session->data().peer(peerIdRaw);
				if (peer) {
					_settings->singlePeerName = peer->name();
				}
			}
		}
		
		LOG(("Export Resume: Extracted peer ID=%1, name='%2' from state")
			.arg(_settings->singlePeerId).arg(_settings->singlePeerName));
	}
}

void PanelController::updateState(State &&state) {
	LOG(("Export State: updateState called, isPasswordCheck=%1").arg(v::is<PasswordCheckState>(state) ? "true" : "false"));
	if (const auto start = std::get_if<PasswordCheckState>(&state)) {
		fillParams(*start);
		LOG(("Export State: fillParams done, peerId=%1, peerName='%2'")
			.arg(_settings->singlePeerId).arg(_settings->singlePeerName));
	}
	if (!_panel) {
		createPanel();
	}
	_state = std::move(state);
	if (const auto apiError = std::get_if<ApiErrorState>(&_state)) {
		showError(*apiError);
	} else if (const auto error = std::get_if<OutputErrorState>(&_state)) {
		showError(*error);
	} else if (const auto scanDone = std::get_if<ScanDoneState>(&_state)) {
		if (_panel) {
			_panel->setTitle(tr::lng_export_title());
			showSettings();
			if (auto settings = dynamic_cast<SettingsWidget*>(_panel->inner())) {
				settings->setScanning(false);
				_panel->setHideOnDeactivate(false);
				settings->setScanResults(scanDone->stats);
			}
		}
	} else if (const auto processing = std::get_if<ProcessingState>(&_state)) {
		if (_panel) {
			if (auto settings = dynamic_cast<SettingsWidget*>(_panel->inner())) {
				if (processing->step == ProcessingState::Step::Scanning) {
					settings->setScanProgress(processing->itemIndex, processing->itemCount);
				}
			}
		}
	} else if (v::is<FinishedState>(_state)) {
		_stopRequested = false;
		_panel->setTitle(tr::lng_export_title());
		_panel->setHideOnDeactivate(false);
	} else if (v::is<CancelledState>(_state)) {
		LOG(("Export Info: Reset Panel After Cancel."));
		_stopRequested = false;
		_panel->setHideOnDeactivate(false);
		_panel->setTitle(tr::lng_export_title());
		showSettings();
		if (auto settings = dynamic_cast<SettingsWidget*>(_panel->inner())) {
			settings->setScanning(false);
			settings->clearScanResults();
		}
		_panel->showAndActivate();
	}
}

void PanelController::saveSettings() const {
	const auto check = [](const QString &value) {
		const auto result = value.endsWith('/')
			? value.mid(0, value.size() - 1)
			: value;
		return Platform::IsWindows() ? result.toLower() : result;
	};
	auto settings = *_settings;
	if (check(settings.path) == check(File::DefaultDownloadPath(_session))) {
		settings.path = QString();
	}
	_session->local().writeExportSettings(settings);
}

} // namespace View
} // namespace Export
