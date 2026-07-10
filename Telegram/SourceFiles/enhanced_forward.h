/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_peer_id.h"

class HistoryItem;
class PhotoData;
class DocumentData;

namespace Main {
class Session;
} // namespace Main

namespace EnhancedForward {

struct ItemFlags {
	bool msg = false;
	bool peer = false;
	bool restricted = false; // msg || peer
};

struct Split {
	std::vector<not_null<HistoryItem*>> restricted;
	std::vector<not_null<HistoryItem*>> normal;
};

[[nodiscard]] ItemFlags checkItem(not_null<HistoryItem*> item);
[[nodiscard]] Split classifyItems(
	const std::vector<not_null<HistoryItem*>> &items);
[[nodiscard]] bool checkMsgRestriction(not_null<HistoryItem*> item);

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

struct ForwardProgress {
	State state = State::Idle;
	int sent = 0;
	int total = 0;
	PeerId destPeer;
	int currentDownload = -1;
	int currentUpload = -1;
	ItemInfo downloadItem;
	ItemInfo uploadItem;
	float64 downloadProgress = 0;
	float64 uploadProgress = 0;
	qint64 downloadSpeed = 0;
	qint64 uploadSpeed = 0;
};

void startForwardSession(
	not_null<Main::Session*> session,
	const PeerId &peerId,
	int totalItems);

void markItemSent(
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

[[nodiscard]] bool isForwarding(const PeerId &id);

[[nodiscard]] bool isPaused(const PeerId &id);

[[nodiscard]] ForwardProgress currentProgress(const PeerId &id);

// Resume progress persistence (mirrors export's progress.json).
QString ProgressFilePath(const PeerId &peerId, const QString &dir);
void SaveProgress(
	const PeerId &peerId,
	const QString &dir,
	const QJsonObject &data);
[[nodiscard]] std::optional<QJsonObject> LoadProgress(
	const PeerId &peerId,
	const QString &dir);
void ClearProgress(const PeerId &peerId, const QString &dir);

} // namespace EnhancedForward
