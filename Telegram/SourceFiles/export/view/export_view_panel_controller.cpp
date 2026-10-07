/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/view/export_view_panel_controller.h"

#include "export/view/export_view_settings.h"
#include "export/view/export_view_progress.h"
#include "export/export_manager.h"
#include "data/data_session.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "data/data_peer_id.h"
#include "history/history.h"
#include "base/base_file_utilities.h"
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
#include "main/main_account.h"
#include "apiwrap.h"
#include <crl/crl_on_main.h>
#include "data/data_session.h"
#include "data/data_download_manager.h"
#include "base/platform/base_platform_info.h"
#include "base/unixtime.h"
#include "base/qt/qt_common_adapters.h"
#include "boxes/abstract_box.h" // Ui::show().
#include <ui/toast/toast.h>
#include "ui/widgets/checkbox.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_export.h"
#include "styles/style_layers.h"

#include <QtGui/QGuiApplication>
#include <QApplication>

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

// One chat may own several export folders (fresh exports with different
// scopes fork numbered copies). This box lists them with checkboxes: All
// checks everything, Clear unchecks, Save runs the checked ones each in
// its own folder, Cancel backs out.
class FolderPickerBox : public Ui::BoxContent {
public:
	FolderPickerBox(
		QWidget*,
		QString title,
		QStringList folders,
		FnMut<void(std::vector<int> &&indices)> save);

protected:
	void prepare() override;

private:
	QString _title;
	QStringList _folders;
	FnMut<void(std::vector<int> &&indices)> _save;
	std::vector<QPointer<Ui::Checkbox>> _checks;

};

FolderPickerBox::FolderPickerBox(
		QWidget*,
		QString title,
		QStringList folders,
		FnMut<void(std::vector<int> &&indices)> save)
: _title(std::move(title))
, _folders(std::move(folders))
, _save(std::move(save)) {
}

void FolderPickerBox::prepare() {
	setTitle(rpl::single(_title));
	const auto inner = setInnerWidget(
		object_ptr<Ui::VerticalLayout>(this));
	for (const auto &folder : _folders) {
		_checks.push_back(inner->add(
			object_ptr<Ui::Checkbox>(inner, folder, true),
			st::boxPadding));
	}
	addButton(tr::lng_export_select_all(), [=] {
		for (const auto check : _checks) {
			if (check) {
				check->setChecked(true);
			}
		}
	});
	addButton(tr::lng_export_clear_selection(), [=] {
		for (const auto check : _checks) {
			if (check) {
				check->setChecked(false);
			}
		}
	});
	addButton(tr::lng_settings_save(), [=] {
		auto indices = std::vector<int>();
		for (auto i = 0; i != int(_checks.size()); ++i) {
			if (_checks[i] && _checks[i]->checked()) {
				indices.push_back(i);
			}
		}
		closeBox();
		_save(std::move(indices));
	});
	addButton(tr::lng_cancel(), [=] { closeBox(); });
}

// A user-pointed folder is accepted as the moved tree when it holds the
// expected content: chat files for a single folder, lists/ for a root.
[[nodiscard]] bool LooksLikeExportFolder(
		const QString &path,
		bool isGlobal) {
	const auto dir = QDir(path);
	if (!dir.exists()) {
		return false;
	}
	if (isGlobal) {
		return QDir(dir.absoluteFilePath(u"lists"_q)).exists();
	}
	return !dir.entryList({ u"messages.*"_q }, QDir::Files).isEmpty()
		|| !dir.entryList({ u"result.json"_q }, QDir::Files).isEmpty()
		|| QDir(dir.absoluteFilePath(u"media"_q)).exists();
}

// Points every stored absolute row path under the old folder at the new
// one (both normalised with trailing slash), after the user located the
// moved tree on disk.
void RebaseStoredFolder(
		not_null<Main::Session*> session,
		int mode,
		const QString &from,
		const QString &to) {
	if (session->account().maybeSession() != session) {
		return;
	}
	auto db = ::Data::DedupDb(
		Core::App().downloadManager().dedupDbPath(),
		false);
	if (!db.isOpen()) {
		return;
	}
	const auto oldPrefix = from.endsWith('/') ? from : (from + '/');
	const auto newPrefix = to.endsWith('/') ? to : (to + '/');
	db.rebaseExResumeFolder(session->uniqueId(), mode, oldPrefix, newPrefix);
}

} // namespace

void CenterPanel(not_null<Ui::SeparatePanel*> panel) {
	if (panel->isHidden()) {
		return;
	}
	const auto active = QApplication::activeWindow();
	const auto screen = active
		? active->screen()
		: QGuiApplication::primaryScreen();
	const auto available = screen ? screen->availableGeometry() : QRect();
	const auto parentGeometry = (active && active->isVisible())
		? active->geometry()
		: available;
	auto geometry = QRect(QPoint(), panel->size());
	geometry.moveCenter(parentGeometry.center());
	if (!available.isNull()) {
		if (geometry.width() <= available.width()) {
			if (geometry.left() < available.left()) {
				geometry.moveLeft(available.left());
			}
			if (geometry.right() > available.right()) {
				geometry.moveRight(available.right());
			}
		}
		if (geometry.height() <= available.height()) {
			if (geometry.top() < available.top()) {
				geometry.moveTop(available.top());
			}
			if (geometry.bottom() > available.bottom()) {
				geometry.moveBottom(available.bottom());
			}
		}
	}
	panel->move(geometry.topLeft());
}

[[nodiscard]] QString SinglePeerFolder(
		not_null<Main::Session*> session,
		const Settings &settings) {
	if (!settings.onlySinglePeer()) {
		return QString();
	}
	const auto peerId = settings.singlePeer.match([](
			const MTPDinputPeerUser &data) {
		return peerFromUser(data.vuser_id().v);
	}, [](const MTPDinputPeerUserFromMessage &data) {
		return peerFromUser(data.vuser_id().v);
	}, [](const MTPDinputPeerChat &data) {
		return peerFromChat(data.vchat_id().v);
	}, [](const MTPDinputPeerChannel &data) {
		return peerFromChannel(data.vchannel_id().v);
	}, [](const MTPDinputPeerChannelFromMessage &data) {
		return peerFromChannel(data.vchannel_id().v);
	}, [&](const MTPDinputPeerSelf &data) {
		return session->userPeerId();
	}, [](const MTPDinputPeerEmpty &data) {
		return PeerId(0);
	});
	if (!peerId) {
		return QString();
	}
	const auto peer = session->data().peerLoaded(peerId);
	const auto name = peer
		? base::FileNameFromUserString(peer->name())
		: QString();
	const auto bare = peerId.value & PeerId::kChatTypeMask;
	const auto id = u"EX_"_q + QString::number(bare);
	if (name.isEmpty()) {
		return id;
	}
	return id + '_' + name;
}

[[nodiscard]] PeerId SinglePeerId(
		not_null<Main::Session*> session,
		const Settings &settings) {
	if (!settings.onlySinglePeer()) {
		return PeerId(0);
	}
	return settings.singlePeer.match([](
			const MTPDinputPeerUser &data) {
		return peerFromUser(data.vuser_id().v);
	}, [](
			const MTPDinputPeerUserFromMessage &data) {
		return peerFromUser(data.vuser_id().v);
	}, [](
			const MTPDinputPeerChat &data) {
		return peerFromChat(data.vchat_id().v);
	}, [](
			const MTPDinputPeerChannel &data) {
		return peerFromChannel(data.vchannel_id().v);
	}, [](
			const MTPDinputPeerChannelFromMessage &data) {
		return peerFromChannel(data.vchannel_id().v);
	}, [&](
			const MTPDinputPeerSelf &data) {
		return session->userPeerId();
	}, [](
			const MTPDinputPeerEmpty &data) {
		return PeerId(0);
	});
}

Environment PrepareEnvironment(not_null<Main::Session*> session) {	auto result = Environment();
	result.accountId = session->userId().bare;
	result.accountName = base::FileNameFromUserString(session->user()->name());
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

// Scope fields only: identity (peer, path, selection) never counts.
[[nodiscard]] bool ScopesEqual(const Settings &a, const Settings &b) {
	return a.media.types == b.media.types
		&& a.media.sizeLimit == b.media.sizeLimit
		&& a.format == b.format
		&& a.singlePeerFrom == b.singlePeerFrom
		&& a.singlePeerTill == b.singlePeerTill
		&& a.useIdRange == b.useIdRange
		&& a.singlePeerFromId == b.singlePeerFromId
		&& a.singlePeerTillId == b.singlePeerTillId
		&& a.types == b.types
		&& a.fullChats == b.fullChats
		&& a.media.extensionFilterMode == b.media.extensionFilterMode
		&& a.media.extensionFilter == b.media.extensionFilter;
}

// Renames a stale single-chat folder (EX_<id>_<name>) to the current title
// and rebases its stored row path, so resume/update continue into the
// renamed folder. Dated fallback folders are never touched. Returns the
// folder to run into.
[[nodiscard]] QString RebaseSingleFolderForContinue(
		not_null<Main::Session*> session,
		const QString &storedFolder,
		const QString &freshFolder) {
	if (session->account().maybeSession() != session) {
		return storedFolder;
	}
	if (storedFolder.isEmpty() || freshFolder.isEmpty()) {
		return storedFolder;
	}
	auto parent = QDir(storedFolder);
	const auto current = parent.dirName();
	if (current == freshFolder) {
		return storedFolder;
	}
	if (!freshFolder.startsWith(u"EX_"_q)) {
		return storedFolder;
	}
	const auto tail = freshFolder.mid(3);
	const auto cut = tail.indexOf('_');
	const auto stem = u"EX_"_q + (cut >= 0 ? tail.left(cut) : tail);
	if (current != stem && !current.startsWith(stem + '_')) {
		return storedFolder;
	}
	parent.cdUp();
	const auto from = parent.absoluteFilePath(current);
	const auto to = parent.absoluteFilePath(freshFolder);
	if (QDir(to).exists() || !QDir().rename(from, to)) {
		return storedFolder;
	}
	auto db = ::Data::DedupDb(
		Core::App().downloadManager().dedupDbPath(),
		false);
	if (db.isOpen()) {
		db.rebaseExResumeFolder(
			session->uniqueId(),
			0,
			from + '/',
			to + '/');
	}
	return to;
}

// Renames a stale per-account global root to the current display name and
// rebases the stored absolute row paths, so resume/update continue into the
// renamed tree with the marker still done. Returns the root to run into.
[[nodiscard]] QString RebaseGlobalRootForContinue(
		not_null<Main::Session*> session,
		const QString &storedRoot) {
	if (session->account().maybeSession() != session) {
		return storedRoot;
	}
	auto db = ::Data::DedupDb(
		Core::App().downloadManager().dedupDbPath(),
		false);
	if (!db.isOpen()) {
		return storedRoot;
	}
	const auto env = PrepareEnvironment(session);
	// Legacy or foreign tree (no id stem): reuse byte-for-byte, never touch.
	const auto stem = u"EX_Global_"_q
		+ QString::number(env.accountId);
	const auto dirName = QDir(storedRoot).dirName();
	if (dirName != stem && !dirName.startsWith(stem + '_')) {
		return storedRoot;
	}
	auto parent = QDir(storedRoot);
	parent.cdUp();
	auto oldRoot = QString();
	const auto root = ResolveGlobalRoot(
		parent.absolutePath(),
		env.accountId,
		env.accountName,
		&oldRoot);
	if (!oldRoot.isEmpty()) {
		db.rebaseExResumeFolder(session->uniqueId(), 1, oldRoot, root);
	}
	return root;
}

void PanelController::resolveMissingFolder(
		QString missingPath,
		bool isGlobal,
		FnMut<void(QString &&folder)> proceed) {
	auto sharedProceed = std::make_shared<FnMut<void(QString &&folder)>>(
		std::move(proceed));
	_panel->showBox(
		Ui::MakeConfirmBox({
			.text = tr::lng_export_folder_missing(
				tr::now,
				lt_path,
				missingPath),
			.confirmed = [=](Fn<void()> close) {
				close();
			FileDialog::GetFolder(
				_panel.get(),
				tr::lng_export_folder(tr::now),
					QString(),
					[=](QString &&result) mutable {
						if (result.isEmpty()) {
							return;
						}
						if (!LooksLikeExportFolder(result, isGlobal)) {
							Ui::Toast::Show(
								tr::lng_export_folder_invalid(tr::now));
							resolveMissingFolder(
								missingPath,
								isGlobal,
								std::move(*sharedProceed));
							return;
						}
						(*sharedProceed)(std::move(result));
					});
			},
			// New folder: recreate from the stored path and continue.
			.cancelled = [=](Fn<void()> close) {
				close();
				(*sharedProceed)(QString());
			},
			.confirmText = tr::lng_export_folder_locate(tr::now),
			.cancelText = tr::lng_export_folder_new(tr::now),
		}),
		Ui::LayerOption::KeepOther,
		anim::type::normal);
}

void PanelController::showFolderPicker(
		std::vector<::Data::ExResumeRecord> rows,
		bool update,
		FnMut<void(std::vector<int> &&indices)> done) {
	auto names = QStringList();
	for (const auto &row : rows) {
		names.push_back(QDir(row.exportFolder).dirName());
	}
	const auto title = update
		? tr::lng_export_choose_update_folder(tr::now)
		: tr::lng_export_choose_resume_folder(tr::now);
	_panel->showBox(
		Box<FolderPickerBox>(title, names, std::move(done)),
		Ui::LayerOption::KeepOther,
		anim::type::normal);
}

void PanelController::startSingleResume(
		Settings settings,
		::Data::ExResumeRecord row) {
	auto resumeSettings = std::move(settings);
	// A renamed chat resumes into the renamed folder: the stored row
	// path is rebased first.
	const auto folder = RebaseSingleFolderForContinue(
		_session,
		row.exportFolder,
		SinglePeerFolder(_session, resumeSettings));
	row.exportFolder = folder;
	resumeSettings.path = folder;
	const auto missing = !QDir(row.exportFolder).exists();
	auto start = [=](QString located) mutable {
		if (!located.isEmpty()) {
			// The user pointed at the moved tree: rebase the stored
			// path and resume there instead of recreating.
			const auto root = located.endsWith('/')
				? located
				: (located + '/');
			RebaseStoredFolder(_session, 0, row.exportFolder, root);
			row.exportFolder = root;
			resumeSettings.path = root;
		}
		const auto gen = _startGen;
		const auto sizeLimit = resumeSettings.media.sizeLimit;
		ensureSharedTakeout([=](uint64 id) {
			if (gen != _startGen) {
				return;
			}
			showProgress();
			_session->api().setTakeoutBorrowed(true);
			_process->setSessionId(_session->uniqueId());
			_process->setDedupDb(
				Core::App().downloadManager().dedupDbPath());
			_process->setSharedTakeoutId(id);
			_process->startResumeExport(
				resumeSettings,
				PrepareEnvironment(_session),
				row);
		}, sizeLimit);
	};
	if (!missing) {
		start(QString());
		return;
	}
	resolveMissingFolder(row.exportFolder, false, std::move(start));
}

void PanelController::startSingleUpdate(
		Settings settings,
		::Data::ExResumeRecord row) {
	if (!updateCoversWholeChat(row)) {
		return;
	}
	// A renamed chat updates into the renamed folder: the stored row
	// path is rebased first.
	const auto folder = RebaseSingleFolderForContinue(
		_session,
		row.exportFolder,
		SinglePeerFolder(_session, settings));
	row.exportFolder = folder;
	const auto folderMissing = !QDir(row.exportFolder).exists();
	auto updateSettings = std::move(settings);
	applyRowSettings(updateSettings, row);
	auto newFolderName = QString();
	if (folderMissing) {
		const auto rowDir = QDir(row.exportFolder);
		newFolderName = rowDir.dirName();
		auto parentPath = rowDir.absolutePath();
		parentPath.chop(newFolderName.length());
		updateSettings.path = parentPath;
		updateSettings.forceSubPath = false;
	}
	auto run = [=](
			Settings runSettings,
			::Data::ExResumeRecord runRow,
			QString runFolder) mutable {
		const auto gen = _startGen;
		const auto sizeLimit = runSettings.media.sizeLimit;
		ensureSharedTakeout([=](uint64 id) {
			if (gen != _startGen) {
				return;
			}
			_process->setSessionId(_session->uniqueId());
			_process->setDedupDb(
				Core::App().downloadManager().dedupDbPath());
			_process->setSharedTakeoutId(id);
			_session->api().setTakeoutBorrowed(true);
			_process->startUpdateExport(
				runSettings,
				PrepareEnvironment(_session),
				runRow,
				runFolder);
		}, sizeLimit);
	};
	if (!folderMissing) {
		run(updateSettings, row, newFolderName);
		return;
	}
	auto start = [=](QString located) mutable {
		if (!located.isEmpty()) {
			// The user pointed at the moved folder: rebase the stored
			// path and update there instead of recreating.
			const auto root = located.endsWith('/')
				? located
				: (located + '/');
			RebaseStoredFolder(_session, 0, row.exportFolder, root);
			row.exportFolder = root;
			updateSettings.path = root;
			newFolderName = QString();
		}
		run(updateSettings, row, newFolderName);
	};
	resolveMissingFolder(row.exportFolder, false, std::move(start));
}

void PanelController::startNextSingleRun() {
	if (_pendingRuns.empty()) {
		return;
	}
	auto next = std::move(_pendingRuns.front());
	_pendingRuns.erase(_pendingRuns.begin());
	if (next.update) {
		startSingleUpdate(std::move(next.settings), std::move(next.row));
	} else {
		startSingleResume(std::move(next.settings), std::move(next.row));
	}
}

base::weak_qptr<Ui::BoxContent> SuggestStart(not_null<Main::Session*> session) {
	if (Core::App().passcodeLocked()) {
		return {};
	}
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
}

PanelController::PanelController(
	not_null<Main::Session*> session,
	not_null<Controller*> process)
: _session(session)
, _account(&session->account())
, _process(process)
, _settings(
	std::make_unique<Settings>(_session->local().readExportSettings()))
, _saveSettingsTimer([=] { saveSettings(); })
, _mtp(&_session->mtp()) {
	ResolveSettings(session, *_settings);

	_process->state(
	) | rpl::on_next([=](State &&state) {
		updateState(std::move(state));
	}, _lifetime);

	// The takeout session is the shared session one: export binds
	// its id before every start and re-ensures it through the
	// session when the server invalidates it mid-run. Export never
	// inits or finishes its own session (two live sessions
	// invalidate each other server-side).
	_process->setTakeoutRefreshHook([=] {
		const auto alive = _alive;
		const auto account = _account;
		const auto session = _session.get();
		crl::on_main([=] {
			// Mid-run refresh: no new size opinion, keep pending.
			if (!*alive || account->maybeSession() != session) {
				return;
			}
			ensureSharedTakeout([=](uint64 id) {
				_process->takeoutRefreshDone(id);
			}, 0);
		});
	});
}

void PanelController::ensureSharedTakeout(
		FnMut<void(uint64)> done,
		int64 fileMaxSize) {
	// FnMut is move-only, Fn needs copyable: share ownership.
	const auto sharedDone = std::make_shared<FnMut<void(uint64)>>(
		std::move(done));
	// This continuation is queued in ApiWrap and can fire on a late
	// response after teardown: never touch a dead panel or session.
	const auto alive = _alive;
	const auto account = _account;
	const auto session = _session.get();
	_session->api().ensureTakeout(
		not_null<PeerData*>(static_cast<PeerData*>(_session->user().get())),
		[=](bool ok) {
			if (!*alive || account->maybeSession() != session) {
				return;
			}
			const auto id = ok
				? _session->api().takeoutId().value_or(uint64(0))
				: uint64(0);
			(*sharedDone)(id);
		},
		fileMaxSize);
}

void PanelController::finishExportTakeout() {
	// End-of-run rule: clear the borrow, then finish unless a
	// restricted view is open (the user is inside it and still
	// needs the session). A finished-away run leaves nothing.
	_session->api().setTakeoutBorrowed(false);
	if (_session->api().takeoutMayFinish()) {
		_session->api().finishTakeout();
	}
}

void PanelController::validateIdRange(FnMut<void()> proceed) {
	if (!_settings->useIdRange) {
		proceed();
		return;
	}
	const auto fromOpt = _settings->singlePeerFromId;
	const auto tillOpt = _settings->singlePeerTillId;
	if ((fromOpt && !*fromOpt) || (tillOpt && !*tillOpt)) {
		Ui::Toast::Show(tr::lng_export_id_zero(tr::now));
		return;
	}
	const auto from = fromOpt.value_or(uint64(0));
	const auto till = tillOpt.value_or(uint64(0));
	if (from && till && from > till) {
		Ui::Toast::Show(tr::lng_export_id_from_above_to(tr::now));
		return;
	}
	if (!_settings->onlySinglePeer()) {
		proceed();
		return;
	}
	const auto peerId = SinglePeerId(_session, *_settings);
	const auto peer = peerId
		? _session->data().peerLoaded(peerId)
		: nullptr;
	if (!peer) {
		proceed();
		return;
	}
	if (_rangeRequestId) {
		_mtp.request(_rangeRequestId).cancel();
		_rangeRequestId = 0;
	}
	struct Bounds {
		int first = 0;
		int last = 0;
		int count = 0;
	};
	const auto parseHead = [](const MTPmessages_Messages &result) {
		auto head = Bounds();
		const auto readFirst = [&](const auto &data) {
			const auto &list = data.vmessages().v;
			if (!list.isEmpty()) {
				head.first = list[0].match([](const auto &m) {
					return int(m.vid().v);
				});
			}
		};
		result.match([&](const MTPDmessages_messages &data) {
			readFirst(data);
			head.count = int(data.vmessages().v.size());
		}, [&](const MTPDmessages_messagesSlice &data) {
			readFirst(data);
			head.count = data.vcount().v;
		}, [&](const MTPDmessages_channelMessages &data) {
			readFirst(data);
			head.count = data.vcount().v;
		}, [&](const MTPDmessages_messagesNotModified &data) {
		});
		return head;
	};
	const auto sharedProceed = std::make_shared<FnMut<void()>>(
		std::move(proceed));
	const auto check = [=, this](int first, int last) {
		_rangeRequestId = 0;
		if (last <= 0) {
			(*sharedProceed)();
			return;
		}
		if (from && from > uint64(last)) {
			Ui::Toast::Show(tr::lng_export_id_from_past_end(
				tr::now,
				lt_last,
				QString::number(last)));
		} else if (till && till > uint64(last)) {
			Ui::Toast::Show(tr::lng_export_id_to_past_end(
				tr::now,
				lt_last,
				QString::number(last)));
		} else if (first > 0 && from && from < uint64(first)) {
			Ui::Toast::Show(tr::lng_export_id_from_before_start(
				tr::now,
				lt_first,
				QString::number(first)));
		} else if (first > 0 && till && till < uint64(first)) {
			Ui::Toast::Show(tr::lng_export_id_to_before_start(
				tr::now,
				lt_first,
				QString::number(first)));
		} else {
			(*sharedProceed)();
		}
	};
	const auto topic = _settings->onlySingleTopic();
	const auto input = peer->input();
	const auto rootId = _settings->singleTopicRootId;
	const auto requestNewest = [=, this]() -> mtpRequestId {
		if (topic) {
			return _mtp.request(MTPmessages_GetReplies(
				input,
				MTP_int(rootId),
				MTP_int(0),
				MTP_int(0),
				MTP_int(0),
				MTP_int(1),
				MTP_int(0),
				MTP_int(0),
				MTP_long(0)
			)).done([=, this](const MTPmessages_Messages &result) {
				const auto head = parseHead(result);
				if (!head.first) {
					_rangeRequestId = 0;
					(*sharedProceed)();
					return;
				}
				if (head.count <= 1) {
					check(head.first, head.first);
					return;
				}
				_rangeRequestId = _mtp.request(MTPmessages_GetReplies(
					input,
					MTP_int(rootId),
					MTP_int(0),
					MTP_int(0),
					MTP_int(1 - head.count),
					MTP_int(1),
					MTP_int(0),
					MTP_int(0),
					MTP_long(0)
				)).done([=, this](const MTPmessages_Messages &oldest) {
					auto first = parseHead(oldest).first;
					auto last = head.first;
					if (first > last) {
						std::swap(first, last);
					}
					check(first, last);
				}).fail([=, this](const MTP::Error &error) {
					check(0, head.first);
					return true;
				}).send();
			}).fail([=, this](const MTP::Error &error) {
				_rangeRequestId = 0;
				(*sharedProceed)();
				return true;
			}).send();
		}
		return _mtp.request(MTPmessages_GetHistory(
			input,
			MTP_int(0),
			MTP_int(0),
			MTP_int(0),
			MTP_int(1),
			MTP_int(0),
			MTP_int(0),
			MTP_long(0)
		)).done([=, this](const MTPmessages_Messages &result) {
			const auto head = parseHead(result);
			if (!head.first) {
				_rangeRequestId = 0;
				(*sharedProceed)();
				return;
			}
			if (head.count <= 1) {
				check(head.first, head.first);
				return;
			}
			_rangeRequestId = _mtp.request(MTPmessages_GetHistory(
				input,
				MTP_int(0),
				MTP_int(0),
				MTP_int(1 - head.count),
				MTP_int(1),
				MTP_int(0),
				MTP_int(0),
				MTP_long(0)
			)).done([=, this](const MTPmessages_Messages &oldest) {
				auto first = parseHead(oldest).first;
				auto last = head.first;
				if (first > last) {
					std::swap(first, last);
				}
				check(first, last);
			}).fail([=, this](const MTP::Error &error) {
				check(0, head.first);
				return true;
			}).send();
		}).fail([=, this](const MTP::Error &error) {
			_rangeRequestId = 0;
			(*sharedProceed)();
			return true;
		}).send();
	};
	_rangeRequestId = requestNewest();
}

PanelController::~PanelController() {
	*_alive = false;
	if (_rangeRequestId) {
		_mtp.request(_rangeRequestId).cancel();
	}
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
	_panel->closeRequests(
	) | rpl::on_next([=] {
		LOG(("Export Info: Panel Hide By Close."));
		_panel->hideGetDuration();
	}, _panel->lifetime());
	_panelCloseEvents.fire(_panel->closeEvents());

	showSettings();
}

void PanelController::refreshResumeRow() {
	_resumeRows.clear();
	const auto peerId = SinglePeerId(_session, *_settings);
	if (!peerId) {
		return;
	}
	auto db = ::Data::DedupDb(
		Core::App().downloadManager().dedupDbPath(),
		false);
	if (!db.isOpen()) {
		return;
	}
	const auto sessionId = _session->uniqueId();
	for (auto &row : db.loadExResume(sessionId, 0)) {
		if (row.mode == 0 && row.peerId == peerId) {
			_resumeRows.push_back(std::move(row));
		}
	}
	// Stable order: the picker lists folders alphabetically.
	std::sort(
		begin(_resumeRows),
		end(_resumeRows),
		[](const auto &a, const auto &b) {
			return a.exportFolder < b.exportFolder;
		});
}

void PanelController::requestChatList(
		Settings snapshot,
		FnMut<void(Data::DialogsInfo&&)> done) {
	if (_chatListCache) {
		if (done) {
			done(Data::DialogsInfo(*_chatListCache));
		}
		return;
	}
	if (done) {
		_chatListWaiters.push_back(std::move(done));
	}
	if (_chatListLoading) {
		return;
	}
	_chatListLoading = true;
	const auto gen = _startGen;
	const auto sizeLimit = snapshot.media.sizeLimit;
	ensureSharedTakeout([=](uint64 id) mutable {
		if (gen != _startGen) {
			return;
		}
		_session->api().setTakeoutBorrowed(true);
		_process->setSessionId(_session->uniqueId());
		_process->setDedupDb(
			Core::App().downloadManager().dedupDbPath());
		_process->setSharedTakeoutId(id);
		_process->requestChatList(
			std::move(snapshot),
			[=](Data::DialogsInfo &&info) mutable {
				crl::on_main([=, info = std::move(info)]() mutable {
					_session->api().setTakeoutBorrowed(false);
					_chatListLoading = false;
					if (info.chats.empty() && info.left.empty()) {
						Ui::Toast::Show(
							tr::lng_export_chat_list_failed(tr::now));
					} else {
						_chatListCache = info;
					}
					for (auto &waiter : base::take(_chatListWaiters)) {
						waiter(Data::DialogsInfo(info));
					}
				});
			});
	}, sizeLimit);
}

void PanelController::refreshGlobalRow() {
	_globalRow = std::nullopt;
	_globalHasUnfinished = false;
	if (_settings->onlySinglePeer()) {
		return;
	}
	auto db = ::Data::DedupDb(
		Core::App().downloadManager().dedupDbPath(),
		false);
	if (!db.isOpen()) {
		return;
	}
	const auto sessionId = _session->uniqueId();
	auto unfinished = false;
	for (auto &row : db.loadExResume(sessionId, 1)) {
		// No disk check here (same as single): a missing folder is handled
		// at click time by the Locate / New folder choice.
		if (row.mode == 1 && !row.peerId) {
			_globalRow = std::move(row);
		} else if (row.mode == 1 && row.state != u"done"_q) {
			unfinished = true;
		}
	}
	// A done marker with unfinished rows (interrupted update or crash
	// between runs) still offers resume for the stranded chats.
	_globalHasUnfinished = unfinished
		&& (!_globalRow || _globalRow->state == u"done"_q);
}

void PanelController::applyMarkerSettings(
		Settings &settings,
		const ::Data::ExResumeRecord &row) {
	applyRowSettings(settings, row);
}

void PanelController::applyRowSettings(
		Settings &settings,
		const ::Data::ExResumeRecord &row) {
	settings.media.types = MediaSettings::Types::from_raw(
		int(row.media));
	settings.media.sizeLimit = row.size;
	settings.format = static_cast<Output::Format>(row.exportFormat);
	settings.singlePeerFrom = row.fromDate
		? std::make_optional(TimeId(row.fromDate))
		: std::nullopt;
	settings.singlePeerTill = row.tillDate
		? std::make_optional(TimeId(row.tillDate))
		: std::nullopt;
	settings.useIdRange = row.useIdRange;
	settings.singlePeerFromId = row.fromId
		? std::make_optional(uint64(row.fromId))
		: std::nullopt;
	settings.singlePeerTillId = row.tillId
		? std::make_optional(uint64(row.tillId))
		: std::nullopt;
	settings.types = Settings::Types::from_raw(row.chatTypes);
	settings.fullChats = Settings::Types::from_raw(row.fullChats);
	const auto extMode = row.extFilterMode;
	settings.media.extensionFilterMode = (extMode >= 0 && extMode <= 2)
		? static_cast<MediaSettings::ExtFilterMode>(extMode)
		: MediaSettings::ExtFilterMode::None;
	settings.media.extensionFilter = row.extFilter.isEmpty()
		? QStringList()
		: row.extFilter.split(u',', Qt::SkipEmptyParts);
	settings.path = row.exportFolder;
}

bool PanelController::updateCoversWholeChat(
		const ::Data::ExResumeRecord &row) const {
	return row.coversWholeChat();
}

void PanelController::showSettings() {
	refreshResumeRow();
	refreshGlobalRow();
	_paused = false;
	_pausePending = false;
	// One chat may own several folders now: buttons follow ANY row.
	const auto anySinglePaused = [&] {
		for (const auto &row : _resumeRows) {
			if (row.state != u"done"_q) {
				return true;
			}
		}
		return false;
	}();
	const auto anySingleUpdatable = [&] {
		for (const auto &row : _resumeRows) {
			if (row.state == u"done"_q && updateCoversWholeChat(row)) {
				return true;
			}
		}
		return false;
	}();
	const auto globalPausedRow = (_globalRow.has_value()
		&& _globalRow->state != u"done"_q)
		|| _globalHasUnfinished;
	const auto globalDoneRow = _globalRow.has_value()
		&& _globalRow->state == u"done"_q;
	// The panel never implies scope: every scope checkbox opens unchecked,
	// so the user consciously ticks what each export contains. Resume and
	// Update visibly read nothing from the panel (they restore row
	// snapshots); any ticked option belongs to a fresh Export.
	// NOTE: fullChats is intentionally kept: its sub-checkboxes hide while
	// their main type is off, and they are inverted (checked means only-my),
	// so zeroing it would flip their opt-in defaults.
	_settings->types = Settings::Types(0);
	_settings->media.types = MediaSettings::Types(0);
	_settings->media.extensionFilterMode
		= MediaSettings::ExtFilterMode::None;
	_settings->media.extensionFilter.clear();
	_scopeShown = *_settings;
	auto settings = base::make_unique_q<SettingsWidget>(
		_panel,
		_session,
		*_settings);
	settings->setResumeUpdateEnabled(
		anySinglePaused || globalPausedRow,
		(anySingleUpdatable && _settings->onlySinglePeer()) || globalDoneRow);
	settings->setStartEnabled(!globalPausedRow);
	settings->setOptionsEnabled(!globalPausedRow);
	settings->setShowBoxCallback([=](object_ptr<Ui::BoxContent> box) {
		_panel->showBox(
			std::move(box),
			Ui::LayerOption::KeepOther,
			anim::type::normal);
	});
	settings->setChatListRequestCallback([=](
			Settings snapshot,
			FnMut<void(Data::DialogsInfo&&)> done) mutable {
		requestChatList(std::move(snapshot), std::move(done));
	});
	if (!_settings->onlySinglePeer()
		&& !_chatListCache
		&& !_chatListLoading
		&& _session->api().takeoutId().has_value()) {
		requestChatList(*_settings, [](Data::DialogsInfo&&) {});
	}

	settings->startClicks(
	) | rpl::on_next([=]() {
		if (_chatListLoading) {
			Ui::Toast::Show(tr::lng_export_loading_chats(tr::now));
			return;
		}
		if (_settings->onlySinglePeer()
			&& _settings->media.types == MediaSettings::Types(0)) {
			Ui::Toast::Show(tr::lng_export_nothing_selected(tr::now));
			return;
		}
		validateIdRange([=]() mutable {
			const auto gen = _startGen;
			const auto folder = SinglePeerFolder(_session, *_settings);
			const auto sizeLimit = _settings->media.sizeLimit;
			ensureSharedTakeout([=](uint64 id) {
				if (gen != _startGen) {
					return;
				}
				showProgress();
				_session->api().setTakeoutBorrowed(true);
				_process->setSessionId(_session->uniqueId());
				_process->setDedupDb(
					Core::App().downloadManager().dedupDbPath());
				_process->setSharedTakeoutId(id);
				if (_settings->chatSelectionActive && _chatListCache) {
					_process->setCachedDialogs(*_chatListCache);
				}
				_process->startExport(
					*_settings,
					PrepareEnvironment(_session),
					folder);
			}, sizeLimit);
		});
	}, settings->lifetime());

	settings->scanClicks(
	) | rpl::on_next([=]() {
		if (_chatListLoading) {
			Ui::Toast::Show(tr::lng_export_loading_chats(tr::now));
			return;
		}
		using Type = MediaSettings::Type;
		const auto fileTypes = Type::Photo | Type::Video
			| Type::VoiceMessage | Type::VideoMessage | Type::Sticker
			| Type::GIF | Type::File | Type::Audio;
		// Text has no server filter and nothing to hash: it walks
		// history and counts into stats.txt only (text messages when
		// ticked, plus whatever else is ticked), no HTML/JSON, and
		// file-less messages skip hashing and dedup entirely.
		const auto hasFiles = ((_settings->media.types & fileTypes)
			!= MediaSettings::Types(0));
		// Links and polls have server filters and counting but
		// nothing to hash: like text they walk into stats.txt only.
		const auto hasCountables = ((_settings->media.types
			& (Type::Text | Type::Link | Type::Poll))
			!= MediaSettings::Types(0));
		if (!hasFiles && !hasCountables) {
			Ui::Toast::Show(tr::lng_export_nothing_selected(tr::now));
			return;
		}
		validateIdRange([=]() mutable {
			const auto gen = _startGen;
			const auto folder = SinglePeerFolder(_session, *_settings);
			const auto sizeLimit = _settings->media.sizeLimit;
			ensureSharedTakeout([=](uint64 id) {
				if (gen != _startGen) {
					return;
				}
				showProgress(true);
				_session->api().setTakeoutBorrowed(true);
				_process->setSessionId(_session->uniqueId());
				_process->setDedupDb(
					Core::App().downloadManager().dedupDbPath());
				_process->setSharedTakeoutId(id);
				if (_settings->chatSelectionActive && _chatListCache) {
					_process->setCachedDialogs(*_chatListCache);
				}
				_process->startScan(
					*_settings,
					PrepareEnvironment(_session),
					folder);
			}, sizeLimit);
		});
	}, settings->lifetime());

	settings->cancelClicks(
	) | rpl::on_next([=] {
		LOG(("Export Info: Panel Hide By Cancel."));
		_panel->hideGetDuration();
	}, settings->lifetime());

	settings->resumeClicks(
	) | rpl::on_next([=]() {
		if (!_settings->onlySinglePeer()
			&& _globalRow
			&& _globalRow->state != u"done"_q) {
			auto resumeSettings = *_settings;
			applyMarkerSettings(resumeSettings, *_globalRow);
			auto marker = *_globalRow;
			if (!ScopesEqual(*_settings, _scopeShown)) {
				Ui::Toast::Show(tr::lng_export_resume_blocked(tr::now));
				return;
			}
			// A renamed account resumes into the renamed tree, marker
			// still valid: the stored absolute paths are rebased first.
			const auto root = RebaseGlobalRootForContinue(
				_session,
				marker.exportFolder);
			marker.exportFolder = root;
			resumeSettings.path = root;
			const auto missing = !QDir(marker.exportFolder).exists();
			auto start = [=](QString located) mutable {
				if (!located.isEmpty()) {
					// The user pointed at the moved tree: rebase the
					// stored paths and resume there instead of recreating.
					const auto moved = located.endsWith('/')
						? located
						: (located + '/');
					RebaseStoredFolder(
						_session,
						1,
						marker.exportFolder,
						moved);
					marker.exportFolder = moved;
					resumeSettings.path = moved;
				}
				const auto gen = _startGen;
				const auto sizeLimit = resumeSettings.media.sizeLimit;
				ensureSharedTakeout([=](uint64 id) {
					if (gen != _startGen) {
						return;
					}
					auto db = ::Data::DedupDb(
						Core::App().downloadManager().dedupDbPath(),
						false);
					const auto rows = db.isOpen()
						? db.loadExResume(_session->uniqueId(), 1)
						: std::vector<::Data::ExResumeRecord>();
					showProgress();
					_session->api().setTakeoutBorrowed(true);
					_process->setSessionId(_session->uniqueId());
					_process->setDedupDb(
						Core::App().downloadManager().dedupDbPath());
					_process->setSharedTakeoutId(id);
					_process->startResumeExportGlobal(
						resumeSettings,
						PrepareEnvironment(_session),
						marker,
						rows);
				}, sizeLimit);
			};
			if (!missing) {
				start(QString());
				return;
			}
			resolveMissingFolder(marker.exportFolder, true, std::move(start));
			return;
		}
		auto candidates = std::vector<::Data::ExResumeRecord>();
		for (const auto &row : _resumeRows) {
			if (row.state != u"done"_q) {
				candidates.push_back(row);
			}
		}
		if (candidates.empty()) {
			return;
		}
		if (!ScopesEqual(*_settings, _scopeShown)) {
			Ui::Toast::Show(tr::lng_export_resume_blocked(tr::now));
			return;
		}
		if (candidates.size() == 1) {
			auto snapshot = *_settings;
			applyRowSettings(snapshot, candidates.front());
			startSingleResume(std::move(snapshot), candidates.front());
			return;
		}
		const auto live = *_settings;
		showFolderPicker(candidates, false, [=](
				std::vector<int> &&indices) mutable {
			_pendingRuns.clear();
			for (const auto i : indices) {
				if (i >= 0 && i < int(candidates.size())) {
					auto snapshot = live;
					applyRowSettings(snapshot, candidates[i]);
					_pendingRuns.push_back(SingleRun{
						std::move(snapshot),
						candidates[i],
						false,
					});
				}
			}
			startNextSingleRun();
		});
		return;
	}, settings->lifetime());

	_process->setUpdateConfirmHandler([=](
			int anyNew,
			int selectedNew,
			FnMut<void(bool)> proceed) mutable {
		if (!_settings->onlySinglePeer()) {
			if (selectedNew <= 0) {
				Ui::Toast::Show(tr::lng_export_up_to_date(tr::now));
				proceed(false);
				return;
			}
			const auto sharedProceed = std::make_shared<FnMut<void(bool)>>(
				std::move(proceed));
			const auto snapshotChats = _globalRow
				? int(_globalRow->dialogIds.size())
				: 0;
			_panel->showBox(
				Ui::MakeConfirmBox({
					.text = tr::lng_export_update_confirm_global(
						tr::now,
						lt_amount,
						QString::number(anyNew),
						lt_chats,
						QString::number(selectedNew),
						lt_skipped,
						QString::number(
							std::max(snapshotChats - selectedNew, 0))),
					.confirmed = [=](Fn<void()> close) {
						close();
						showProgress();
						(*sharedProceed)(true);
					},
					.cancelled = [=](Fn<void()> close) {
						close();
						(*sharedProceed)(false);
					},
				}),
				Ui::LayerOption::KeepOther,
				anim::type::normal);
			return;
		}
		if (selectedNew <= 0) {
			Ui::Toast::Show(anyNew > 0
				? tr::lng_export_update_none_selected(tr::now)
				: tr::lng_export_up_to_date(tr::now));
			proceed(false);
			return;
		}
		const auto sharedProceed = std::make_shared<FnMut<void(bool)>>(
			std::move(proceed));
		_panel->showBox(
			Ui::MakeConfirmBox({
				.text = tr::lng_export_update_confirm(
					tr::now,
					lt_amount,
					QString::number(selectedNew)),
				.confirmed = [=](Fn<void()> close) {
					close();
					showProgress();
					(*sharedProceed)(true);
				},
				.cancelled = [=](Fn<void()> close) {
					close();
					(*sharedProceed)(false);
				},
			}),
			Ui::LayerOption::KeepOther,
			anim::type::normal);
	});

	settings->updateClicks(
	) | rpl::on_next([=]() {
		if (!_settings->onlySinglePeer()) {
			if (!_globalRow || _globalRow->state != u"done"_q) {
				return;
			}
			auto updateSettings = *_settings;
			applyMarkerSettings(updateSettings, *_globalRow);
			auto marker = *_globalRow;
			if (!ScopesEqual(*_settings, _scopeShown)) {
				Ui::Toast::Show(tr::lng_export_update_blocked(tr::now));
				return;
			}
			// A renamed account updates into the renamed tree, marker
			// still done: the stored absolute paths are rebased first.
			const auto root = RebaseGlobalRootForContinue(
				_session,
				marker.exportFolder);
			marker.exportFolder = root;
			updateSettings.path = root;
			const auto missing = !QDir(marker.exportFolder).exists();
			auto start = [=](QString located) mutable {
				if (!located.isEmpty()) {
					// The user pointed at the moved tree: rebase the
					// stored paths and update there instead of recreating.
					const auto moved = located.endsWith('/')
						? located
						: (located + '/');
					RebaseStoredFolder(
						_session,
						1,
						marker.exportFolder,
						moved);
					marker.exportFolder = moved;
					updateSettings.path = moved;
				}
				const auto gen = _startGen;
				const auto sizeLimit = updateSettings.media.sizeLimit;
				ensureSharedTakeout([=](uint64 id) {
					if (gen != _startGen) {
						return;
					}
					auto db = ::Data::DedupDb(
						Core::App().downloadManager().dedupDbPath(),
						false);
					const auto rows = db.isOpen()
						? db.loadExResume(_session->uniqueId(), 1)
						: std::vector<::Data::ExResumeRecord>();
					_process->setSessionId(_session->uniqueId());
					_process->setDedupDb(
						Core::App().downloadManager().dedupDbPath());
					_process->setSharedTakeoutId(id);
					_session->api().setTakeoutBorrowed(true);
					_process->startUpdateExportGlobal(
						updateSettings,
						PrepareEnvironment(_session),
						marker,
						rows);
				}, sizeLimit);
			};
			if (!missing) {
				start(QString());
				return;
			}
			resolveMissingFolder(marker.exportFolder, true, std::move(start));
			return;
		}
		auto candidates = std::vector<::Data::ExResumeRecord>();
		for (const auto &row : _resumeRows) {
			if (row.state == u"done"_q && updateCoversWholeChat(row)) {
				candidates.push_back(row);
			}
		}
		if (candidates.empty()) {
			return;
		}
		if (!ScopesEqual(*_settings, _scopeShown)) {
			Ui::Toast::Show(tr::lng_export_update_blocked(tr::now));
			return;
		}
		if (candidates.size() == 1) {
			startSingleUpdate(*_settings, candidates.front());
			return;
		}
		const auto live = *_settings;
		showFolderPicker(candidates, true, [=](
				std::vector<int> &&indices) mutable {
			_pendingRuns.clear();
			for (const auto i : indices) {
				if (i >= 0 && i < int(candidates.size())) {
					_pendingRuns.push_back(SingleRun{
						live,
						candidates[i],
						true,
					});
				}
			}
			startNextSingleRun();
		});
		return;
	}, settings->lifetime());

	settings->changes(
	) | rpl::on_next([=](Settings &&settings) {
		*_settings = std::move(settings);
	}, settings->lifetime());

	auto size = st::exportPanelSize;
	const auto screen = QGuiApplication::primaryScreen();
	const auto fixedHeight = _settings->onlySinglePeer() ? 780 : 1000;
	size.setHeight(screen
		? std::min(screen->availableGeometry().height() - 40, fixedHeight)
		: fixedHeight);
	settings->resize(size.width(), size.height());
	_panel->setInnerSize(size);
	_panel->showInner(std::move(settings));
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

bool PanelController::isExportRunning() const {
	// A requested pause counts immediately: the park lands
	// asynchronously on the export queue, and treating it as
	// running until then reopens the quit box (double-click).
	return _running && !_scanning && !_paused && !_pausePending;
}

void PanelController::pauseRunningExport() {
	if (isExportRunning()) {
		_pausePending = true;
		_process->requestPause();
	}
}

void PanelController::cancelRunningExport() {
	++_startGen;
	_pendingRuns.clear();
	finishExportTakeout();
	refreshResumeRow();
	refreshGlobalRow();
	_paused = false;
	_pausePending = false;
	_running = false;
	_process->cancelExportFast();
	_process->closeDialogFiles();
	// Cancel abandons unfinished work: drop every non-done single row with
	// its folder (done folders are never touched). The api side already
	// removed the active run's own row above.
	if (_settings->onlySinglePeer()) {
		auto db = ::Data::DedupDb(
			Core::App().downloadManager().dedupDbPath(),
			false);
		const auto sessionId = _session->uniqueId();
		for (const auto &row : _resumeRows) {
			if (row.state == u"done"_q || row.exportFolder.isEmpty()) {
				continue;
			}
			if (db.isOpen()) {
				db.removeExResumeFolder(
					sessionId,
					row.peerId,
					0,
					row.exportFolder);
			}
			QDir(row.exportFolder).removeRecursively();
		}
	}
	_resumeRows.clear();
	// A global cancel clears checkpoints but keeps the account tree:
	// its folders belong to finished chats.
}

void PanelController::showProgress(bool scanning) {
	_settings->availableAt = 0;
	ClearSuggestStart(_session);
	_paused = false;
	_pausePending = false;
	_running = true;
	_scanning = scanning;

	_panel->setTitle(scanning
		? tr::lng_export_scanning()
		: tr::lng_export_progress_title());

	auto progress = base::make_unique_q<ProgressWidget>(
		_panel.get(),
		rpl::single(
			ContentFromState(_settings.get(), ProcessingState())
		) | rpl::then(progressState()));

	progress->skipFileClicks(
	) | rpl::on_next([=](uint64 randomId) {
		_process->skipFile(randomId);
	}, progress->lifetime());

	progress->cancelClicks(
	) | rpl::on_next([=] {
		stopWithConfirmation();
	}, progress->lifetime());

	progress->pauseToggleClicks(
	) | rpl::on_next([=] {
		if (_paused) {
			_pausePending = false;
			_process->resumeExport();
		} else {
			_pausePending = true;
			_process->requestPause();
		}
	}, progress->lifetime());

	// The button is only offered while a park point is reachable: from
	// the first chat until the run ends. A press while hidden is
	// impossible by construction, so a pending pause is never lost.
	_process->canPauseChanges(
	) | rpl::on_next([=](bool can) {
		if (const auto widget = _progress.data()) {
			widget->setPauseEnabled(can);
		}
	}, progress->lifetime());

	_process->pauseChanges(
	) | rpl::on_next([=](bool paused) {
		_paused = paused;
		if (!paused) {
			_pausePending = false;
		}
		if (const auto widget = _progress.data()) {
			widget->setPaused(paused);
		}
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

	_progress = progress.get();
	_panel->showInner(std::move(progress));
	_panel->setHideOnDeactivate(true);
}

void PanelController::stopWithConfirmation(Fn<void()> callback) {
	if (!v::is<ProcessingState>(_state)) {
		LOG(("Export Info: Stop Panel Without Confirmation."));
		stopExport();
		if (callback) {
			callback();
		}
		return;
	}
	auto stop = [=, callback = std::move(callback)]() mutable {
		if (auto saved = std::move(callback)) {
			LOG(("Export Info: Stop Panel With Confirmation."));
			stopExport();
			saved();
			return;
		}
		cancelRunningExport();
	};
	const auto hidden = _panel->isHidden();
	const auto old = _confirmStopBox;
	refreshResumeRow();
	const auto deleteFolder = [&] {
		for (const auto &row : _resumeRows) {
			if (row.state != u"done"_q && !row.exportFolder.isEmpty()) {
				return true;
			}
		}
		return false;
	}();
	auto box = Ui::MakeConfirmBox({
		.text = deleteFolder
			? tr::lng_export_cancel_delete()
			: _scanning
			? tr::lng_export_sure_stop_scan()
			: tr::lng_export_sure_stop(),
		.confirmed = std::move(stop),
		.confirmText = tr::lng_box_yes(tr::now),
		.cancelText = tr::lng_box_no(tr::now),
		.confirmStyle = &st::attentionBoxButton,
	});
	_confirmStopBox = box.data();
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

rpl::producer<> PanelController::stopRequests() const {
	return _panelCloseEvents.events(
	) | rpl::flatten_latest(
	) | rpl::filter([=] {
		return !v::is<ProcessingState>(_state) || _stopRequested;
	});
}

void PanelController::fillParams(const PasswordCheckState &state) {
	_settings->singlePeer = state.singlePeer;
}

void PanelController::updateState(State &&state) {
	if (const auto start = std::get_if<PasswordCheckState>(&state)) {
		fillParams(*start);
	}
	if (!_panel) {
		createPanel();
	}
	_state = std::move(state);
	if (v::is<ProcessingState>(_state)
		&& !_running
		&& !v::is<PasswordCheckState>(_state)) {
		return;
	}
	if (const auto apiError = std::get_if<ApiErrorState>(&_state)) {
		if (!_running) {
			_session->api().setTakeoutBorrowed(false);
			_chatListLoading = false;
			Ui::Toast::Show(apiError->data.type());
			return;
		}
		_pendingRuns.clear();
		finishExportTakeout();
		showError(*apiError);
	} else if (const auto error = std::get_if<OutputErrorState>(&_state)) {
		_pendingRuns.clear();
		finishExportTakeout();
		showError(*error);
	} else if (const auto finished = std::get_if<FinishedState>(&_state)) {
		_running = false;
		_paused = false;
		_pausePending = false;
		finishExportTakeout();
		// Multi-folder chain: next queued single run instead of the finish
		// screen; the last run falls through to it normally.
		if (!_pendingRuns.empty()) {
			startNextSingleRun();
			return;
		}
		_panel->setTitle(tr::lng_export_title());
		_panel->setHideOnDeactivate(false);
		// Keep total-row space: exact sizing overlaps the button
		// while stale progress rows are still in the layout.
		auto rows = 2;
		if (finished->skippedFiles > 0) {
			++rows;
		}
		for (const auto i : Output::Stats::kDisplayOrder) {
			if (finished->groupFiles[i] || finished->groupSkipped[i]) {
				++rows;
			}
		}
		if (finished->linkMessages > 0) {
			++rows;
		}
		for (auto i = 0; i != Output::Stats::kGroups; ++i) {
			if (Output::Stats::kGroupStats[i].type
				== MediaSettings::Type::Poll
				&& finished->groupFiles[i]) {
				++rows;
			}
		}
		if (finished->textMessages > 0) {
			++rows;
		}
		if (!finished->failedChats.empty()) {
			++rows;
		}
		const auto unit = st::exportProgressRowHeight
			+ st::exportProgressRowPadding.top()
			+ st::exportProgressRowPadding.bottom();
		const auto skip = int(st::exportProgressRowSkip);
		const auto wanted = 150 + rows * unit + (rows - 1) * skip;
		const auto screen = QGuiApplication::primaryScreen();
		const auto cap = screen
			? (screen->availableGeometry().height() - 80)
			: 800;
		if (const auto inner = _panel->inner()) {
			const auto height = std::min(wanted, cap);
			if (inner->height() != height) {
				_panel->setInnerSize({ inner->width(), height });
				CenterPanel(_panel.get());
			}
			if (!_progress.isNull() && inner == _progress.get()) {
				const auto progress = _progress.get();
				progress->heightValue(
				) | rpl::on_next([=] {
					if (!v::is<FinishedState>(_state)) {
						return;
					}
					const auto overflow = progress->scrollOverflow();
					if (overflow <= 0) {
						return;
					}
					if (const auto innerNow = _panel->inner()) {
						const auto grown = std::min(
							innerNow->height() + overflow,
							cap);
						if (grown != innerNow->height()) {
							_panel->setInnerSize(
								{ innerNow->width(), grown });
							CenterPanel(_panel.get());
						}
					}
				}, _lifetime);
			}
		}
	} else if (v::is<CancelledState>(_state)) {
		LOG(("Export Info: Stop Panel After Cancel."));
		_pendingRuns.clear();
		_running = false;
		_paused = false;
		_pausePending = false;
		stopExport();
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