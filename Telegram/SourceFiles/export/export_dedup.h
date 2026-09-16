/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QByteArray>
#include <QString>

#include "data/data_peer_id.h"
#include "export/data/export_data_types.h"

namespace MTP {
class ConcurrentSender;
} // namespace MTP

namespace Data {
class DedupDb;
} // namespace Data

namespace Export {

// Result of the pre-download duplicate decision.
struct CheckResult {
	bool skip = false;
	QByteArray hash;
	bool decided = false;
};

// Fetches the same 2-chunk hash downloads use, over takeout.
void FetchHash(
	MTP::ConcurrentSender &mtp,
	Fn<void(FnMut<void()>)> runner,
	uint64 takeoutId,
	const Data::FileLocation &location,
	int64 size,
	Fn<bool()> alive,
	Fn<void(QByteArray)> done,
	int maxAttempts = 5,
	const QString &tag = QString());

// Fetches a whole small file (or photo) into memory over takeout.
// Sequential precise 1MB chunks; calls done exactly once, empty on failure.
void FetchFullFile(
	MTP::ConcurrentSender &mtp,
	Fn<void(FnMut<void()>)> runner,
	uint64 takeoutId,
	const Data::FileLocation &location,
	int64 size,
	Fn<bool()> alive,
	Fn<void(QByteArray)> done,
	int maxAttempts = 5,
	const QString &tag = QString());
// Same decision tree as download dedup: document id first, then the
// 2-chunk remote hash for big files. Small files and photos are decided
// after download (decided == false). With global set the downloads table
// is used, otherwise the run temp table. Always calls done exactly once.
// Decides by an already known hash, no network. Lets callers reuse one
// fetched hash for repeated ids instead of fetching it again.
CheckResult CheckHash(
	::Data::DedupDb &db,
	uint64 sessionId,
	PeerId peerId,
	uint64 docId,
	const QByteArray &hash,
	bool global);

void CheckDuplicate(
	MTP::ConcurrentSender &mtp,
	Fn<void(FnMut<void()>)> runner,
	uint64 takeoutId,
	::Data::DedupDb &db,
	uint64 sessionId,
	PeerId peerId,
	uint64 docId,
	const Data::FileLocation &location,
	int64 size,
	Fn<bool()> alive,
	bool global,
	bool isPhoto,
	Fn<void(CheckResult)> done);

// Records a fully written file. Returns true when the bytes duplicate
// content already known (caller deletes the file and labels the message).
// A non-empty preHash means the decision was made before download.
[[nodiscard]] bool FinishFile(
	::Data::DedupDb &db,
	uint64 sessionId,
	PeerId peerId,
	uint64 docId,
	const QByteArray &content,
	const QString &path,
	int64 size,
	const QByteArray &preHash,
	bool global,
	bool isPhoto);

// Drops the in-progress mark of a canceled file (global mode only).
void CancelFile(
	::Data::DedupDb &db,
	uint64 docId);

} // namespace Export
