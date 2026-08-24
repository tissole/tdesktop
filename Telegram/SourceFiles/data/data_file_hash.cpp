/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_file_hash.h"

#include <QtCore/QFile>
#include <variant>

#include <memory>

#include <xxhash.h>

#include "logs.h"
#include "data/data_document.h"
#include "data/data_photo.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_session.h"
#include "mtproto/facade.h"
#include "mtproto/mtp_instance.h"
#include "ui/image/image_location.h"

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
		return QByteArray();
	}
	if (!file.seek(offset)) {
		return QByteArray();
	}
	const auto data = file.read(count);
	if (data.size() != count) {
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
	headOffset = std::min<int64>(kDedupHeadSkip, (size / 2) - kDedupChunk);
	headOffset = std::max<int64>(0, headOffset);
	headOffset = (headOffset / kDedupAlignment) * kDedupAlignment;
	tailOffset = size - kDedupChunk;
	tailOffset = std::max<int64>(0, tailOffset);
	tailOffset = (tailOffset / kDedupAlignment) * kDedupAlignment;
	if (tailOffset < headOffset + kDedupChunk) {
		tailOffset = headOffset + kDedupChunk;
	}
	if (tailOffset + kDedupChunk > size) {
		tailOffset = std::max<int64>(headOffset + kDedupChunk, size - kDedupChunk);
	}
	const auto ensureWithinBlock = [](int64 &offset) {
		const auto blockOffset = offset % kDedupBlock;
		if (blockOffset + kDedupChunk > kDedupBlock) {
			offset -= kDedupAlignment;
			offset = (offset / kDedupAlignment) * kDedupAlignment;
			const auto newBlockOffset = offset % kDedupBlock;
			if (newBlockOffset + kDedupChunk > kDedupBlock) {
				offset = (offset / kDedupBlock) * kDedupBlock
					+ kDedupBlock - kDedupChunk;
			}
		}
	};
	ensureWithinBlock(headOffset);
	ensureWithinBlock(tailOffset);
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
		return QByteArray();
	}
	if (size < kDedupMinPartialHashSize) {
		const auto r = ReadHash(path, 0, size);
		if (r.isEmpty()) {
			return QByteArray();
		}
		return HashChunks(r, QByteArray());
	}
	int64 headOffset = 0;
	int64 tailOffset = 0;
	DedupSampleOffsets(size, headOffset, tailOffset);
	const auto head = ReadHash(path, headOffset, kDedupChunk);
	const auto tail = ReadHash(path, tailOffset, kDedupChunk);
	if (head.isEmpty() || tail.isEmpty()) {
		return QByteArray();
	}
	return HashChunks(head, tail);
}

QByteArray ContentFingerprint(const QByteArray &content) {
	if (content.isEmpty()) {
		return QByteArray();
	}
	auto state = XXH3_state_t();
	XXH3_128bits_reset(&state);
	XXH3_128bits_update(
		&state,
		content.constData(),
		static_cast<size_t>(content.size()));
	return HashState(&state);
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
	const auto dcId = MTP::updaterDcId(document->getDC());

	if (size < kDedupMinPartialHashSize) {
		const auto limit = ((size + 1023) / 1024) * 1024;
		const auto content = std::make_shared<QByteArray>();
		auto &mtp = session->account().mtp();
		mtp.send(
			MTPupload_GetFile(
				MTP_flags(MTPupload_GetFile::Flag::f_precise),
				location,
				MTP_long(0),
				MTP_int(int(limit))),
		[content, done](const MTP::Response &response) {
			auto from = response.reply.constData();
			if (!from || response.reply.isEmpty()) {
				done(QByteArray());
				return true;
			}
			auto result = MTPupload_File();
			if (!result.read(from, from + response.reply.size())) {
				done(QByteArray());
				return true;
			}
			if (result.type() != mtpc_upload_file) {
				done(QByteArray());
				return true;
			}
			*content = result.c_upload_file().vbytes().v;
			done(ContentFingerprint(*content));
			return true;
		},
		[done](const MTP::Error &, const MTP::Response &) {
			done(QByteArray());
			return true;
		},
		dcId);
		return;
	}

	int64 headOffset = 0;
	int64 tailOffset = 0;
	DedupSampleOffsets(size, headOffset, tailOffset);
	const auto chunk = int64(kDedupChunk);

	const auto head = std::make_shared<QByteArray>();
	const auto tail = std::make_shared<QByteArray>();
	const auto pending = std::make_shared<int>(2);
	// 'failed' makes sure 'done' fires exactly once no matter which/how many
	// of the two requests fail: every completion path (success, MTP error,
	// or an unparsable response) always funnels through doneHash(), and
	// doneHash() itself only calls 'done' once 'pending' has been decremented
	// by both requests. Previously, an MTP error called 'done' directly
	// without going through 'pending', so if both requests failed, 'done'
	// was invoked twice; and an unparsable response returned without calling
	// doneHash() at all, so 'pending' never reached 0 and 'done' was never
	// called, leaving the caller (and e.g. checkDuplicate's in-flight
	// counter) stuck waiting forever.
	const auto failed = std::make_shared<bool>(false);
	const auto doneHash = [head, tail, pending, failed, done] {
		if (--*pending != 0) {
			return;
		}
		done(*failed ? QByteArray() : HashChunks(*head, *tail));
	};

	auto &mtp = session->account().mtp();
	mtp.send(
		MTPupload_GetFile(
			MTP_flags(MTPupload_GetFile::Flag::f_precise),
			location,
			MTP_long(headOffset),
			MTP_int(int(chunk))),
		[head, failed, doneHash](const MTP::Response &response) {
			MTPupload_File result;
			auto from = response.reply.constData();
			if (!result.read(from, from + response.reply.size())) {
				*failed = true;
				doneHash();
				return true;
			}
			*head = ChunkFromRemote(result);
			doneHash();
			return true;
		},
		[failed, doneHash](const MTP::Error &error, const MTP::Response &) {
			*failed = true;
			doneHash();
			return true;
		},
		dcId);
	mtp.send(
		MTPupload_GetFile(
			MTP_flags(MTPupload_GetFile::Flag::f_precise),
			location,
			MTP_long(tailOffset),
			MTP_int(int(chunk))),
		[tail, failed, doneHash](const MTP::Response &response) {
			MTPupload_File result;
			auto from = response.reply.constData();
			if (!result.read(from, from + response.reply.size())) {
				*failed = true;
				doneHash();
				return true;
			}
			*tail = ChunkFromRemote(result);
			doneHash();
			return true;
		},
		[failed, doneHash](const MTP::Error &error, const MTP::Response &) {
			*failed = true;
			doneHash();
			return true;
		},
		dcId);
}

void RemotePhotoFingerprint(
		not_null<Main::Session*> session,
		not_null<PhotoData*> photo,
		Fn<void(QByteArray)> done) {
	const auto &imageLocation = photo->location(Data::PhotoSize::Large);
	const auto storage = std::get_if<StorageFileLocation>(
		&imageLocation.file().data);
	if (!storage || !storage->valid()) {
		done(QByteArray());
		return;
	}
	const auto dcId = MTP::updaterDcId(storage->dcId());
	const auto location = storage->tl(session->userId());

	constexpr auto kChunk = 1024 * 1024;
	constexpr auto kMaxTotal = 16 * 1024 * 1024;

	auto accumulated = std::make_shared<QByteArray>();
	auto offset = std::make_shared<int64>(0);
	auto step = std::make_shared<Fn<void()>>();
	*step = [=, done = std::move(done)]() mutable {
		auto &mtp = session->account().mtp();
		mtp.send(
			MTPupload_GetFile(
				MTP_flags(MTPupload_GetFile::Flag::f_precise),
				location,
				MTP_long(*offset),
				MTP_int(kChunk)),
			[=](const MTP::Response &response) {
				auto result = MTPupload_File();
				auto from = response.reply.constData();
				if (!from
					|| response.reply.isEmpty()
					|| !result.read(from, from + response.reply.size())
					|| result.type() != mtpc_upload_file) {
					*step = nullptr;
					done(QByteArray());
					return true;
				}
				const auto bytes = ChunkFromRemote(result);
				if (bytes.isEmpty()) {
					*step = nullptr;
					done(QByteArray());
					return true;
				}
				accumulated->append(bytes);
				if (bytes.size() < kChunk
					|| accumulated->size() >= kMaxTotal) {
					*step = nullptr;
					done(ContentFingerprint(*accumulated));
					return true;
				}
				*offset += int64(bytes.size());
				(*step)();
				return true;
			},
			[=](const MTP::Error &, const MTP::Response &) {
				*step = nullptr;
				done(QByteArray());
				return true;
			},
			dcId);
	};
	(*step)();
}

} // namespace Data
