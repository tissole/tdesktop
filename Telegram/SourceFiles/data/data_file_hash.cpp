/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_file_hash.h"

#include <QtCore/QFile>

#include <memory>

#include <xxhash.h>

#include "logs.h"
#include "data/data_document.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/facade.h"
#include "mtproto/mtp_instance.h"

namespace Data {

namespace {

[[nodiscard]] QByteArray HashState(XXH3_state_t *state) {
	auto hash = XXH3_128bits_digest(state);
	auto result = QByteArray();
	result.resize(sizeof(hash.low64) + sizeof(hash.high64));
	auto bytes = reinterpret_cast<uchar*>(result.data());
	memcpy(bytes, &hash.low64, sizeof(hash.low64));
	memcpy(bytes + sizeof(hash.low64), &hash.high64, sizeof(hash.high64));
	return result;
}

[[nodiscard]] QByteArray ReadHash(const QString &path, int64 offset, int64 count) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		LOG(("DEDUP: ReadHash open failed path=%1 offset=%2 count=%3 err=%4").arg(
			path).arg(offset).arg(count).arg(file.errorString()));
		return QByteArray();
	}
	const auto fileSize = file.size();
	if (!file.seek(offset)) {
		LOG(("DEDUP: ReadHash seek failed path=%1 offset=%2 size=%3").arg(
			path).arg(offset).arg(fileSize));
		return QByteArray();
	}
	const auto data = file.read(count);
	LOG(("DEDUP: ReadHash path=%1 offset=%2 requested=%3 got=%4 fileSize=%5").arg(
		path).arg(offset).arg(count).arg(data.size()).arg(fileSize));
	if (data.size() != count) {
		LOG(("DEDUP: ReadHash short read path=%1 offset=%2 count=%3 got=%4").arg(
			path).arg(offset).arg(count).arg(data.size()));
		return QByteArray();
	}
	return data;
}

[[nodiscard]] QByteArray ChunkFromRemote(const MTPupload_File &result) {
	if (result.type() != mtpc_upload_file) {
		return QByteArray();
	}
	const auto &data = result.c_upload_file();
	return data.vbytes().v;
}

// Two sample offsets (head and tail) used for the partial fingerprint.
// Both local and remote fingerprinting must use identical offsets so the
// resulting hash matches. Offsets are aligned to the server's 4KB boundary,
// and each sampled chunk is kept inside a single 1MB block (upload.getFile
// requirement).
void DedupSampleOffsets(
		int64 size,
		int64 &headOffset,
		int64 &tailOffset) {
	headOffset = std::min(int64(kDedupHeadSkip), size / 2 - kDedupChunk);
	headOffset = (headOffset / kDedupAlignment) * kDedupAlignment;
	const auto blockStart = ((size - kDedupChunk) / kDedupBlock) * kDedupBlock;
	tailOffset = blockStart + (kDedupBlock - kDedupChunk);
	if (tailOffset < headOffset + kDedupChunk) {
		tailOffset = size - kDedupChunk;
	}
	tailOffset = (tailOffset / kDedupAlignment) * kDedupAlignment;
}

} // namespace

QByteArray HashChunks(const QByteArray &head, const QByteArray &tail) {
	auto state = XXH3_state_t();
	XXH3_128bits_reset(&state);
	if (!head.isEmpty()) {
		XXH3_128bits_update(
			&state,
			head.constData(),
			static_cast<size_t>(head.size()));
	}
	if (!tail.isEmpty()) {
		XXH3_128bits_update(
			&state,
			tail.constData(),
			static_cast<size_t>(tail.size()));
	}
	return HashState(&state);
}

QByteArray FileFingerprint(const QString &path, int64 size) {
	if (size <= 0) {
		LOG(("DEDUP: FileFingerprint size<=0 path=%1 size=%2").arg(path).arg(
			size));
		return QByteArray();
	}
	if (size < kDedupMinPartialHashSize) {
		const auto r = ReadHash(path, 0, size);
		LOG(("DEDUP: FileFingerprint full path=%1 size=%2 result=%3").arg(
			path).arg(size).arg(QString::fromLatin1(r.toHex())));
		return r;
	}
	int64 headOffset = 0;
	int64 tailOffset = 0;
	DedupSampleOffsets(size, headOffset, tailOffset);
	const auto head = ReadHash(path, headOffset, kDedupChunk);
	const auto tail = ReadHash(path, tailOffset, kDedupChunk);
	LOG(("DEDUP: FileFingerprint path=%1 size=%2 headOff=%3 tailOff=%4 headLen=%5 tailLen=%6 result=%7").arg(
		path).arg(size).arg(headOffset).arg(tailOffset).arg(
		head.size()).arg(tail.size()).arg(
		QString::fromLatin1(HashChunks(head, tail).toHex())));
	if (head.isEmpty() || tail.isEmpty()) {
		LOG(("DEDUP: FileFingerprint empty head/tail path=%1 headEmpty=%2 tailEmpty=%3").arg(
			path).arg(Logs::b(head.isEmpty())).arg(Logs::b(tail.isEmpty())));
		return QByteArray();
	}
	return HashChunks(head, tail);
}

void RemoteFileFingerprint(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document,
		Fn<void(QByteArray)> done) {
	const auto size = document->size;
	if (size <= 0) {
		done(QByteArray());
		return;
	}
	const auto location = MTP_inputDocumentFileLocation(
		MTP_long(document->id),
		MTP_long(document->accessHash()),
		MTP_bytes(document->fileReference()),
		MTP_string());
	if (size < kDedupMinPartialHashSize) {
		done(QByteArray());
		return;
	}
	int64 headOffset = 0;
	int64 tailOffset = 0;
	DedupSampleOffsets(size, headOffset, tailOffset);
	const auto chunk = int64(kDedupChunk);
	LOG(("DEDUP: RemoteFileFingerprint size=%1 headOff=%2 tailOff=%3 chunk=%4").arg(
		size).arg(headOffset).arg(tailOffset).arg(chunk));

	const auto dcId = MTP::updaterDcId(document->getDC());
	const auto head = std::make_shared<QByteArray>();
	const auto tail = std::make_shared<QByteArray>();
	const auto pending = std::make_shared<int>(2);
	const auto doneHash = [head, tail, pending, done] {
		if (--*pending != 0) {
			return;
		}
		done(HashChunks(*head, *tail));
	};

	auto &mtp = session->account().mtp();
	mtp.send(
		MTPupload_GetFile(
			MTP_flags(MTPupload_GetFile::Flag::f_precise),
			location,
			MTP_long(headOffset),
			MTP_int(int(chunk))),
		[head, doneHash](const MTP::Response &response) {
			MTPupload_File result;
			auto from = response.reply.constData();
			if (!result.read(from, from + response.reply.size())) {
				return false;
			}
			*head = ChunkFromRemote(result);
			doneHash();
			return true;
		},
		[done](const MTP::Error &error, const MTP::Response &) {
			done(QByteArray());
			return true;
		},
		dcId);
	mtp.send(
		MTPupload_GetFile(
			MTP_flags(MTPupload_GetFile::Flag::f_precise),
			location,
			MTP_long(tailOffset),
			MTP_int(int(chunk))),
		[tail, doneHash](const MTP::Response &response) {
			MTPupload_File result;
			auto from = response.reply.constData();
			if (!result.read(from, from + response.reply.size())) {
				return false;
			}
			*tail = ChunkFromRemote(result);
			doneHash();
			return true;
		},
		[done](const MTP::Error &error, const MTP::Response &) {
			done(QByteArray());
			return true;
		},
		dcId);
}

} // namespace Data
