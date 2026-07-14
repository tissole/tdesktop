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

// Fetch message noforwards flag from server
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

	if (!finished) loop.exec();
	return result;
}

// Fetch peer noforwards flag from server
bool checkPeerRestriction(not_null<PeerData*> peer) {
	auto result = false;
	auto loop = QEventLoop();
	auto finished = false;
	const auto session = &peer->session();

	if (const auto channel = peer->asChannel()) {
		session->api().request(MTPchannels_GetChannels(
			MTP_vector<MTPInputChannel>(1, channel->inputChannel())
		)).done([&](const MTPmessages_Chats &data) {
			data.match([&](const auto &data) {
				session->data().processChats(data.vchats());
			});
			result = (channel->flags() & ChannelDataFlag::NoForwards);
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
			result = (user->flags() & UserDataFlag::NoForwardsPeerEnabled);
			finished = true;
			loop.quit();
		}).fail([&](const MTP::Error &) {
			finished = true;
			loop.quit();
		}).send();
	} else if (const auto chat = peer->asChat()) {
		session->api().request(MTPmessages_GetFullChat(
			chat->inputChat()
		)).done([&](const MTPmessages_ChatFull &data) {
			const auto &d = data.c_messages_chatFull();
			session->data().processUsers(d.vusers());
			session->data().processChats(d.vchats());
			result = (chat->flags() & ChatDataFlag::NoForwards);
			finished = true;
			loop.quit();
		}).fail([&](const MTP::Error &) {
			finished = true;
			loop.quit();
		}).send();
	}

	if (!finished) loop.exec();
	return result;
}

ItemFlags checkItem(not_null<HistoryItem*> item) {
	LOG(("ENHANCED_FWD: checkItem item=%1").arg(item->id.bare));
	const auto history = item->history();
	const auto peer = history->peer;
	const auto msg = !!(item->flags() & MessageFlag::NoForwards);
	auto peerFlag = false;
	if (const auto channel = peer->asChannel()) {
		peerFlag = !!(channel->flags() & ChannelDataFlag::NoForwards);
	} else if (const auto chat = peer->asChat()) {
		peerFlag = !!(chat->flags() & ChatDataFlag::NoForwards);
	} else if (const auto user = peer->asUser()) {
		peerFlag = !!(user->flags() & UserDataFlag::NoForwardsPeerEnabled);
	}
	LOG(("ENHANCED_FWD: checkItem msg=%1 peerFlag=%2 restricted=%3")
		.arg(Logs::b(msg))
		.arg(Logs::b(peerFlag))
		.arg(Logs::b(msg || peerFlag)));
	return { msg, peerFlag, (msg || peerFlag) };
}

Split classifyItems(
		const std::vector<not_null<HistoryItem*>> &items) {
	LOG(("ENHANCED_FWD: classifyItems count=%1").arg(items.size()));
	Split result;
	for (const auto &item : items) {
		const auto flags = checkItem(item);
		if (flags.restricted) {
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
		state.finishTimer = std::make_unique<base::Timer>([=] {
			auto &states = ActiveStates();
			states.erase(peerId);
			fireUpdate(session, peerId);
		});
		state.finishTimer->callOnce(2000);
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
				PeerId(obj["peer"].toVariant().toULongLong()),
				MsgId(obj["msg"].toVariant().toLongLong())));
		}
		const auto items = (*data)["items"].toArray();
		for (const auto &v : items) {
			const auto obj = v.toObject();
			job.uploadDone.push_back(obj["upload_done"].toBool(false));
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

} // namespace EnhancedForward