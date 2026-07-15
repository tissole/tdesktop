/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "enhanced_forward.h"

#include "apiwrap.h"
#include "rpl/rpl.h"
#include "base/debug_log.h"
#include "base/flat_map.h"
#include "base/timer.h"
#include "data/data_changes.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"

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
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>

// ADDITIONAL INTERNAL DATA TYPES REQUIRED FOR THE STAGE PIPELINE:
#include "data/data_document.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_file_origin.h"
#include "data/data_histories.h"
#include "data/business/data_shortcut_messages.h"
#include "ui/chat/attach/attach_prepare.h"
#include "storage/storage_account.h"

namespace EnhancedForward {
namespace {

struct SharedState {
	int total = 0;
	int sent = 0;
	bool cancelled = false;
	bool paused = false;
	bool finished = false;
	std::unique_ptr<base::Timer> finishTimer;
	PeerId destPeer;
	Fn<void()> cancelCallback;
	Fn<void()> pauseCallback;
	Fn<void()> resumeCallback;
	Fn<void()> saveCallback;
	int currentDownload = -1;
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

rpl::event_stream<PeerId> StateChanges;

void NotifyStateChanged(const PeerId &peer) {
	StateChanges.fire_copy(peer);
}

} // namespace

StateMap &ActiveStates() {
	static StateMap map;
	return map;
}

void fireUpdate(not_null<Main::Session*> session, const PeerId &peer) {
	session->changes().peerUpdated(
		session->data().peer(peer),
		Data::PeerUpdate::Flag::Slowmode);
	NotifyStateChanged(peer);
}

bool checkMsgRestriction(not_null<HistoryItem*> item) {
	const auto peer = item->history()->peer;
	auto result = false;
	auto loop = QEventLoop();
	auto finished = false;
	const auto session = &peer->session();

	if (const auto channel = peer->asChannel()) {
		session->api().request(MTPchannels_GetMessages(
			channel->inputChannel(),
			MTP_vector<MTPInputMessage>(1, MTP_inputMessageID(MTP_int(item->id.bare)))
		)).done([&](const MTPmessages_Messages &data) {
			data.match([&](const MTPDmessages_messages &data) {
				for (const auto &msg : data.vmessages().v) {
					msg.match([&](const MTPDmessage &m) {
						result = (m.vflags().v & (1U << 26));
					}, [&](const MTPDmessageService &m) {
						result = (m.vflags().v & (1U << 26));
					}, [](const auto &) {});
				}
				finished = true;
				loop.quit();
			}, [&](const MTPDmessages_messagesSlice &data) {
				for (const auto &msg : data.vmessages().v) {
					msg.match([&](const MTPDmessage &m) {
						result = (m.vflags().v & (1U << 26));
					}, [&](const MTPDmessageService &m) {
						result = (m.vflags().v & (1U << 26));
					}, [](const auto &) {});
				}
				finished = true;
				loop.quit();
			}, [&](const MTPDmessages_channelMessages &data) {
				for (const auto &msg : data.vmessages().v) {
					msg.match([&](const MTPDmessage &m) {
						result = (m.vflags().v & (1U << 26));
					}, [&](const MTPDmessageService &m) {
						result = (m.vflags().v & (1U << 26));
					}, [](const auto &) {});
				}
				finished = true;
				loop.quit();
			}, [&](const MTPDmessages_messagesNotModified &) {
				finished = true;
				loop.quit();
			}, [](const auto &) {
			});
		}).fail([&](const MTP::Error &) {
			finished = true;
			loop.quit();
		}).send();
	} else {
		session->api().request(MTPmessages_GetMessages(
			MTP_vector<MTPInputMessage>(1, MTP_inputMessageID(MTP_int(item->id.bare)))
		)).done([&](const MTPmessages_Messages &data) {
			data.match([&](const MTPDmessages_messages &data) {
				for (const auto &msg : data.vmessages().v) {
					msg.match([&](const MTPDmessage &m) {
						result = (m.vflags().v & (1U << 26));
					}, [](const auto &) {});
				}
				finished = true;
				loop.quit();
			}, [&](const MTPDmessages_messagesSlice &data) {
				for (const auto &msg : data.vmessages().v) {
					msg.match([&](const MTPDmessage &m) {
						result = (m.vflags().v & (1U << 26));
					}, [](const auto &) {});
				}
				finished = true;
				loop.quit();
			}, [](const auto &) {
			});
		}).fail([&](const MTP::Error &) {
			finished = true;
			loop.quit();
		}).send();
	}

	QTimer::singleShot(10000, [&] {
		if (!finished) { finished = true; loop.quit(); }
	});
	if (!finished) loop.exec();
	return result;
}

bool checkPeerRestriction(not_null<PeerData*> peer) {
	auto result = false;
	auto loop = QEventLoop();
	auto finished = false;
	const auto session = &peer->session();

	const auto scanMessages = [&](const MTPVector<MTPMessage> &msgs) {
		for (const auto &msg : msgs.v) {
			if (msg.type() != mtpc_message) continue;
			if (msg.c_message().vflags().v & (1U << 26)) {
				result = true;
			}
			break;
		}
	};
	const auto scanPeers = [&](const MTPVector<MTPChat> &chats) {
		for (const auto &c : chats.v) {
			if (c.type() == mtpc_channel) {
				if (c.c_channel().is_noforwards()) {
					result = true;
					break;
				}
			} else if (c.type() == mtpc_chat) {
				if (c.c_chat().is_noforwards()) {
					result = true;
					break;
				}
			}
		}
	};

	if (peer->asChannel() || peer->asChat()) {
		session->api().request(MTPmessages_GetHistory(
			peer->input(),
			MTP_int(0),
			MTP_int(0),
			MTP_int(0),
			MTP_int(10),
			MTP_int(0),
			MTP_int(0),
			MTP_long(0)
		)).done([&](const MTPmessages_Messages &data) {
			data.match([&](const MTPDmessages_messages &d) {
				scanMessages(d.vmessages());
				scanPeers(d.vchats());
			}, [&](const MTPDmessages_messagesSlice &d) {
				scanMessages(d.vmessages());
				scanPeers(d.vchats());
			}, [&](const MTPDmessages_channelMessages &d) {
				scanMessages(d.vmessages());
				scanPeers(d.vchats());
			}, [&](const MTPDmessages_messagesNotModified &) {
			}, [](const auto &) {
			});
			finished = true;
			loop.quit();
		}).fail([&](const MTP::Error &) {
			finished = true;
			loop.quit();
		}).send();
	} else if (const auto user = peer->asUser()) {
		session->api().request(MTPusers_GetFullUser(
			user->inputUser()
		)).done([&](const MTPusers_UserFull &data) {
			const auto &d = data.c_users_userFull();
			session->data().processUsers(d.vusers());
			session->data().processChats(d.vchats());
			result = (user->flags() & UserDataFlag::NoForwardsPeerEnabled)
				|| (user->flags() & UserDataFlag::NoForwardsMyEnabled);
			finished = true;
			loop.quit();
		}).fail([&](const MTP::Error &) {
			finished = true;
			loop.quit();
		}).send();
	}

	QTimer::singleShot(10000, [&] {
		if (!finished) { finished = true; loop.quit(); }
	});
	if (!finished) loop.exec();
	LOG(("ENHANCED_FWD: checkPeerRestriction result=%1")
		.arg(Logs::b(result)));
	return result;
}

Split classifyItems(
		const std::vector<not_null<HistoryItem*>> &items) {
	LOG(("ENHANCED_FWD: classifyItems count=%1").arg(items.size()));
	Split result;

	base::flat_map<PeerId, bool> peerRestricted;
	for (const auto &item : items) {
		const auto peerId = item->history()->peer->id;
		if (!peerRestricted.contains(peerId)) {
			peerRestricted.emplace(
				peerId,
				checkPeerRestriction(item->history()->peer));
		}
	}

	for (const auto &item : items) {
		const auto peerFlag = peerRestricted[item->history()->peer->id];
		LOG(("ENHANCED_FWD: checkItem item=%1 peerFlag=%2 restricted=%3")
			.arg(item->id.bare)
			.arg(Logs::b(peerFlag))
			.arg(Logs::b(peerFlag)));
		if (peerFlag) {
			result.restricted.push_back(item);
		} else {
			result.normal.push_back(item);
		}
	}
	LOG(("ENHANCED_FWD: classifyItems restricted=%1 normal=%2")
		.arg(result.restricted.size())
		.arg(result.normal.size()));
	return result;
}

void startForwardSession(
		not_null<Main::Session*> session,
		const PeerId &peerId,
		int totalItems,
		Fn<void()> saveCallback) {
	auto &states = ActiveStates();
	states.erase(peerId);

	auto &state = states[peerId];
	state.total = totalItems;
	state.sent = 0;
	state.cancelled = false;
	state.paused = false;
	state.finished = false;
	state.destPeer = peerId;
	state.items.resize(totalItems);
	state.saveCallback = std::move(saveCallback);

	fireUpdate(session, peerId);
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
	fireUpdate(session, peerId);
	if (state.saveCallback) {
		state.saveCallback();
	}

	if (state.sent >= state.total) {
		state.finished = true;
		fireUpdate(session, peerId);
		state.finishTimer = std::make_unique<base::Timer>([=] {
			auto &states = ActiveStates();
			states.erase(peerId);
			fireUpdate(session, peerId);
		});
		state.finishTimer->callOnce(3000);
	}
}

void cancelForward(
		const PeerId &id,
		not_null<Main::Session*> session) {
	auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return;

	it->second.cancelled = true;
	it->second.finished = true;
	if (it->second.cancelCallback) {
		it->second.cancelCallback();
	}
	fireUpdate(session, id);

	it->second.finishTimer = std::make_unique<base::Timer>([=] {
		auto &states = ActiveStates();
		states.erase(id);
		fireUpdate(session, id);
	});
	it->second.finishTimer->callOnce(3000);
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
	LOG(("ENHANCED_FWD: pauseForward peer=%1").arg(id.value));
	auto &states = ActiveStates();
	const auto it = states.find(id);
	if (it == states.end()) return;

	it->second.paused = true;
	if (it->second.pauseCallback) {
		it->second.pauseCallback();
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
		state.items[itemIndex].state = ItemState::Uploading;
		state.items[itemIndex].info = info;
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
		state.items[itemIndex].state = newState;
		state.items[itemIndex].info = info;
		state.items[itemIndex].progress = progress;
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
	result.destPeer = state.destPeer;
	result.currentDownload = state.currentDownload;
	result.currentUpload = state.currentUpload;
	result.downloadItem = state.downloadItem;
	result.uploadItem = state.uploadItem;
	result.downloadProgress = state.downloadProgress;
	result.uploadProgress = state.uploadProgress;
	result.downloadSpeed = state.downloadSpeed;
	result.uploadSpeed = state.uploadSpeed;
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

QString ProgressFilePath(
		const QString &bareName,
		const QString &dir) {
	return QDir(dir).absoluteFilePath(
		u"EF_%1.json"_q.arg(bareName));
}

QString ProgressFileBareName(const QString &srcName) {
	auto name = srcName;
	static const QRegularExpression bad(
		QRegularExpression::escape(u"\\/:*?\"<>|"_q));
	name.replace(bad, u"_"_q);
	name = name.trimmed();
	if (name.isEmpty()) {
		name = u"chat"_q;
	}
	return name;
}

void SaveProgress(
		const QString &path,
		const QJsonObject &data) {
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly)) {
		LOG(("ENHANCED_FWD: cannot write progress %1").arg(path));
		return;
	}
	f.write(QJsonDocument(data).toJson(QJsonDocument::Indented));
}

std::optional<QJsonObject> LoadProgress(const QString &path) {
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		return std::nullopt;
	}
	const auto doc = QJsonDocument::fromJson(f.readAll());
	if (!doc.isObject()) {
		return std::nullopt;
	}
	return doc.object();
}

void ClearProgress(const QString &path) {
	QFile(path).remove();
}

void CleanupPartialFiles(const QString &progressPath) {
	const auto data = LoadProgress(progressPath);
	if (data) {
		const auto items = (*data)["items"].toArray();
		for (const auto &v : items) {
			const auto obj = v.toObject();
			const auto filePath = obj["path"].toString();
			if (!filePath.isEmpty()) {
				QFile(filePath).remove();
			}
		}
	}
	QFile(progressPath).remove();
}

std::vector<SavedJob> GetUnfinishedJobs(const QString &dir) {
	std::vector<SavedJob> result;
	const auto files = QDir(dir).entryList(
		QStringList(u"EF_*.json"_q),
		QDir::Files);
	for (const auto &name : files) {
		const auto path = dir + name;
		const auto data = LoadProgress(path);
		if (!data) {
			continue;
		}
		const auto srcPeerVal = (*data)["src_peer"].toDouble();
		const auto dstPeerVal = (*data)["dst_peer"].toDouble();
		const auto srcId = PeerId(qulonglong(srcPeerVal));
		const auto dstId = PeerId(qulonglong(dstPeerVal));
		if (!srcId || !dstId) continue;
		const auto total = int((*data)["total"].toInt(0));
		const auto sent = int((*data)["sent"].toInt(0));
		if (total <= 0 || sent >= total) continue;
		SavedJob job;
		job.srcId = srcId;
		job.dstId = dstId;
		job.path = path;
		job.total = total;
		job.sent = sent;
		const auto msgs = (*data)["source_msgs"].toArray();
		for (const auto &v : msgs) {
			const auto obj = v.toObject();
			job.sourceMsgs.push_back(FullMsgId(
				srcId,
				MsgId(obj["msg"].toVariant().toLongLong())));
		}
		const auto items = (*data)["items"].toArray();
		for (const auto &v : items) {
			const auto obj = v.toObject();
			job.uploadDone.push_back(obj["upload_done"].toBool(false));
			job.fileId.push_back(obj["file_id"].toString().toULongLong());
			job.uploadedParts.push_back(int(obj["uploaded_parts"].toInt(0)));
		}
		result.push_back(std::move(job));
	}
	return result;
}

std::optional<SavedJob> GetUnfinishedJobByDst(
		const PeerId &dstId,
		const QString &dir) {
	for (const auto &job : GetUnfinishedJobs(dir)) {
		if (job.dstId == dstId) {
			return job;
		}
	}
	return std::nullopt;
}

rpl::producer<PeerId> stateChanges() {
	return StateChanges.events();
}

void Pipeline::Start(
		not_null<ApiWrap*> api,
		std::vector<not_null<HistoryItem*>> &&items,
		const Api::SendAction &action,
		Data::GroupingOptions groupOptions,
		std::shared_ptr<SavedJob> resumeJob) {
	auto pipeline = std::make_shared<Pipeline>(
		api,
		std::move(items),
		action,
		groupOptions,
		resumeJob);
	pipeline->run();
}

Pipeline::Pipeline(
		not_null<ApiWrap*> api,
		std::vector<not_null<HistoryItem*>> &&items,
		const Api::SendAction &action,
		Data::GroupingOptions groupOptions,
		std::shared_ptr<SavedJob> resumeJob)
: _api(api)
, _session(api->session())
, _action(action)
, _groupOptions(groupOptions) {
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
		_progressPath = resumeJob->path;
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
	}
}

Pipeline::~Pipeline() = default;

void Pipeline::run() {
	const auto self = shared_from_this();
	_uploadLifetime = std::make_shared<rpl::lifetime>();
	_dlLifetime = std::make_shared<rpl::lifetime>();
	_uploadIndex = std::make_shared<base::flat_map<FullMsgId, int>>();

	if (_progressPath.isEmpty()) {
		const auto srcPeer = _session.data().peer(_srcPeer);
		const auto srcName = srcPeer ? srcPeer->name() : u"chat"_q;
		_progressPath = EnhancedForward::ProgressFilePath(
			EnhancedForward::ProgressFileBareName(srcName),
			_downloadPath);
	}

	EnhancedForward::startForwardSession(
		&_session,
		_peerId,
		_n,
		[self] { self->saveProgress(); });

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
					_items[i].path = QDir(_downloadPath).absoluteFilePath(name);
					QDir().mkpath(QFileInfo(_items[i].path).absolutePath());
					_items[i].needsDownload = true;
				}
			} else if (const auto photo = media ? media->photo() : nullptr) {
				const auto v = photo->activeMediaView();
				const auto destPath = QDir(_downloadPath).absoluteFilePath(
					QString::number(i) + u"_"_q + QString::number(photo->id) + u".jpg"_q);
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
				_items[i].textOnly = true;
				_items[i].downloadDone = true;
				_items[i].uploadDone = true;
			}
		}
	}

	setupCallbacks();

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
	) | rpl::on_next([self](const Storage::UploadProgress &data) {
		self->onUploadProgress(data);
	}, *_uploadLifetime);

	_session.uploader().documentProgressInfo(
	) | rpl::on_next([self](const Storage::UploadProgress &data) {
		self->onUploadProgress(data);
	}, *_uploadLifetime);

	_session.data().documentLoadProgress(
	) | rpl::on_next([self](not_null<DocumentData*> doc) {
		for (auto i = 0; i < self->_n; i++) {
			self->checkItem(i);
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

	pumpDownloads();
	pumpUploads();
	sendNext();
}

void Pipeline::saveProgress() {
	QJsonObject root;
	root["dst_peer"] = qint64(_peerId.value);
	root["src_peer"] = qint64(_srcPeer.value);
	root["total"] = int(_items.size());
	root["sent"] = EnhancedForward::currentProgress(_peerId).sent;
	QJsonArray msgs;
	for (const auto &it : _items) {
		QJsonObject m;
		m["msg"] = qint64(it.sourceId.msg.bare);
		msgs.append(m);
	}
	root["source_msgs"] = msgs;
	QJsonArray items;
	for (auto i = 0; i < int(_items.size()); i++) {
		QJsonObject item;
		item["index"] = i;
		item["path"] = _items[i].path;
		item["downloaded"] = _items[i].downloadedBytes;
		item["download_done"] = bool(_items[i].downloadDone);
		item["upload_done"] = bool(_items[i].uploadDone);
		item["text_only"] = bool(_items[i].textOnly);
		item["file_id"] = QString::number(_items[i].fileId);
		item["part_size"] = qint64(_items[i].partSize);
		item["uploaded_parts"] = int(_items[i].uploadedParts);
		const auto srcItem = _session.data().message(_items[i].sourceId);
		if (srcItem) {
			const auto media = srcItem->media();
			if (const auto doc = media ? media->document() : nullptr) {
				item["name"] = doc->filename();
				item["size"] = qint64(doc->size);
			} else if (media && media->photo()) {
				item["name"] = u"photo.jpg"_q;
				item["size"] = qint64(0);
			} else {
				item["name"] = QFileInfo(_items[i].path).fileName();
				item["size"] = qint64(0);
			}
		}
		items.append(item);
	}
	root["items"] = items;
	const auto srcPeer = _session.data().peer(_srcPeer);
	const auto srcName = srcPeer ? srcPeer->name() : u"chat"_q;
	root["src_name"] = srcName;
	
	EnhancedForward::SaveProgress(_progressPath, root);
}

bool Pipeline::loadProgress() {
	const auto data = EnhancedForward::LoadProgress(_progressPath);
	if (!data) return false;
	_sent = int((*data)["sent"].toInt(0));
	const auto items = (*data)["items"].toArray();
	for (const auto &v : items) {
		const auto obj = v.toObject();
		const auto i = obj["index"].toInt(-1);
		if (i < 0 || i >= int(_items.size())) continue;
		_items[i].path = obj["path"].toString();
		_items[i].downloadedBytes = qint64(obj["downloaded"].toDouble());
		_items[i].textOnly = obj["text_only"].toBool();
		_items[i].downloadDone = obj["download_done"].toBool();
		_items[i].uploadDone = obj["upload_done"].toBool();
		_items[i].fileId = obj["file_id"].toString().toULongLong();
		_items[i].partSize = qint64(obj["part_size"].toDouble());
		_items[i].uploadedParts = int(obj["uploaded_parts"].toInt(0));
	}
	return true;
}

void Pipeline::setupCallbacks() {
	const auto self = shared_from_this();
	EnhancedForward::setCancelCallback(
		_peerId,
		&_session,
		[self] {
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
			}
			EnhancedForward::CleanupPartialFiles(self->_progressPath);
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
					self->_items[i].textOnly = true;
					self->_items[i].downloadDone = true;
					self->_items[i].uploadDone = true;
					continue;
				}
				const auto media = srcItem->media();
				if (!media || (!media->document() && !media->photo())) {
					continue;
				}
				self->_items[i].downloadStarted = false;
				self->_items[i].uploadStarted = false;
				if (!self->_items[i].downloadDone) {
					self->_items[i].needsDownload = true;
				}
				self->_items[i].uploadDone = false;
			}
			self->_downloadCursor = 0;
			self->_uploadCursor = 0;
			self->_downloadInFlight = false;
			self->_uploadInFlight = false;
			self->pumpDownloads();
			self->pumpUploads();
			self->sendNext();
		});
}

void Pipeline::sendNext() {
	if (EnhancedForward::currentProgress(_peerId).state == EnhancedForward::State::Cancelled) {
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
				const auto done = [this](const MTPUpdates &, const MTP::Response &) {
					EnhancedForward::markItemSent(&_session, _peerId);
				};
				const auto fail = [this, randomId](const MTP::Error &error, const MTP::Response &) {
					_api->sendMessageFail(error, _action.history->peer, randomId, FullMsgId());
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
						Api::SuggestToMTP(_action.options.suggest)),
					done,
					fail);
			} else {
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

	// Cleanup
	_session.data().sendHistoryChangeNotifications();
	_session.changes().historyUpdated(
		_action.history,
		(_action.options.scheduled
			? Data::HistoryUpdate::Flag::ScheduledSent
			: Data::HistoryUpdate::Flag::MessageSent));
	for (auto i = 0; i < _n; i++) {
		if (!_items[i].path.isEmpty() && _items[i].path.startsWith(_downloadPath)) {
			QFile::remove(_items[i].path);
		}
	}
	EnhancedForward::ClearProgress(_progressPath);
	if (_uploadLifetime) _uploadLifetime->destroy();
	if (_dlLifetime) _dlLifetime->destroy();
}

void Pipeline::pumpUploads() {
	if (_uploadInFlight) return;
	if (_uploadCursor >= _n) return;
	if (!_items[_uploadCursor].downloadDone) return;
	const auto i = _uploadCursor;
	_items[i].uploadStarted = true;
	_uploadInFlight = true;
	_uploadCursor++;
	startUploadForItem(i);
}

void Pipeline::startUploadForItem(int i) {
	LOG(("ENHANCED_FWD: startUploadForItem i=%1").arg(i));
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
	auto args = FileLoadTask::Args{
		.session = &_session,
		.filepath = item.path,
		.content = QByteArray(),
		.information = nullptr,
		.videoCover = nullptr,
		.type = item.isPhoto ? SendMediaType::Photo : SendMediaType::File,
		.to = to,
		.caption = {
			srcItem->originalText().text,
			TextUtilities::ConvertEntitiesToTextTags(srcItem->originalText().entities)
		},
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
				LOG(("ENHANCED_FWD: prep fail idx=%1").arg(i));
				item.textOnly = true;
				item.uploadDone = true;
				_uploadInFlight = false;
				pumpUploads();
				sendNext();
				return;
			} else {
				item.uploadDone = true;
				_uploadInFlight = false;
				pumpUploads();
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
			LOG(("ENHANCED_FWD: upload starting idx=%1 uploadedParts=%2 fileId=%3")
				.arg(i).arg(item.uploadedParts).arg(item.fileId));
			_session.uploader().upload(localMsgId, item.prepared, item.uploadedParts);

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
					albumIt->second->items.emplace_back(kEmptyTaskId);
					albumIt->second->items.back().msgId = localMsg->fullId();
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
				{ prepared ? prepared->filename : QString(), fileSize },
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
	EnhancedForward::updateUploadProgress(
		&_session, _peerId, idx,
		{ item.prepared ? item.prepared->filename : QString(),
		  item.prepared ? qint64(item.prepared->filesize) : qint64(0) },
		1.0);
	saveProgress();

	// The Uploader itself already calls sendUploadedPhoto/Document
	// via its internal subscription (file_upload.cpp lines 183/199).
	// We just track completion here.
	if (item.sentItem) {
		_session.data().requestItemRepaint(item.sentItem);
	}
	item.sent = true;
	EnhancedForward::markItemSent(&_session, _peerId);

	_uploadInFlight = false;
	pumpUploads();
	sendNext();
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
		return;
	}
	constexpr auto kMaxUploadRetries = 3;
	if (item.retries < kMaxUploadRetries) {
		item.retries++;
		item.uploadedParts = 0;
		item.fileId = base::RandomValue<uint64>();
		LOG(("ENHANCED_FWD: upload failed, fresh retry %1/%2 idx=%3")
			.arg(item.retries).arg(kMaxUploadRetries).arg(idx));
		item.uploadDone = false;
		_uploadInFlight = true;
		startUploadForItem(idx);
		return;
	}
	LOG(("ENHANCED_FWD: upload failed permanently idx=%1").arg(idx));
	_uploadInFlight = false;
	item.uploadDone = false;
	item.retries = 0;
	item.uploadedParts = 0;
	item.fileId = base::RandomValue<uint64>();
	const auto history = _session.data().history(_peerId);
	const auto fileName = item.prepared ? item.prepared->filename : u"file"_q;
	const auto text = tr::lng_enhanced_forward_upload_failed(
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
	EnhancedForward::markItemSent(&_session, _peerId);
	saveProgress();
	EnhancedForward::pauseForward(_peerId, &_session);
}

void Pipeline::onUploadProgress(const Storage::UploadProgress &data) {
	if (data.fullId.peer != _peerId) return;
	const auto it = _uploadIndex->find(data.fullId);
	if (it == _uploadIndex->end()) return;
	const auto idx = it->second;
	auto &item = _items[idx];
	if (item.uploadDone) return;
	const auto prepared = item.prepared;
	const auto filename = prepared ? prepared->filename : QString();
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
		if (newPct >= lastSaved + 10 || newPct >= 99) {
			item.lastSavedPct = newPct;
			saveProgress();
		}
	}
	EnhancedForward::updateUploadProgress(&_session, _peerId, idx, { filename, filesize }, p);
}

void Pipeline::checkItem(int i) {
	auto &item = _items[i];
	if (item.downloadDone) return;
	if (!item.downloadStarted) return;
	if (EnhancedForward::currentProgress(_peerId).state == EnhancedForward::State::Cancelled) return;
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
				return;
			}
		}
		item.downloadDone = true;
		item.downloadedBytes = fi.size();
		_downloadInFlight = false;
		EnhancedForward::updateDownloadProgress(
			&_session, _peerId, i,
			{ fi.fileName(), fi.size() },
			1.0);
		pumpUploads();
		pumpDownloads();
		return;
	}
	const auto srcItem = _session.data().message(item.sourceId);
	if (!srcItem) return;
	const auto media = srcItem->media();
	if (const auto doc = media ? media->document() : nullptr) {
		if (doc->loading()) {
			item.downloadedBytes = qint64(doc->loadOffset());
			EnhancedForward::updateDownloadProgress(
				&_session, _peerId, i,
				{ doc->filename(), doc->size },
				doc->progress());
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
				EnhancedForward::updateDownloadProgress(
					&_session, _peerId, i,
					{ QString::number(photo->id) + u".jpg"_q, 0 },
					1.0);
				_downloadInFlight = false;
				pumpUploads();
				pumpDownloads();
			} else {
				item.textOnly = true;
				item.downloadDone = true;
				item.uploadDone = true;
				sendNext();
			}
		} else if (failed) {
			item.textOnly = true;
			item.downloadDone = true;
			item.uploadDone = true;
			_downloadInFlight = false;
			sendNext();
			pumpDownloads();
		} else if (!loading) {
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
	while (_downloadCursor < _n
			&& (!_items[_downloadCursor].needsDownload || _items[_downloadCursor].downloadDone)) {
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
	if (doc) {
		doc->save(
			Data::FileOrigin(FullMsgId(srcItem->history()->peer->id, srcItem->id)),
			item.path,
			LoadFromCloudOrLocal,
			false,
			true);
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

} // namespace EnhancedForward