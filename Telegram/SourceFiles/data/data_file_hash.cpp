/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_file_hash.h"

#include <QtCore/QFile>
#include <variant>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>

#include <xxhash.h>

#include "data/data_document.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/file_chunk_fetcher.h"
#include "main/main_session.h"
#include "mtproto/facade.h"
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

// Every chunk request of the app is admitted, retried and delivered by the
// shared fetcher; this wrapper only adapts a location, an offset and a limit.
void SendChunk(
		not_null<Main::Session*> session,
		const MTPInputFileLocation &location,
		MTP::ShiftedDcId dcId,
		uint64 takeoutId,
		int64 offset,
		int limit,
		Fn<void(QByteArray)> onBytes,
		Fn<bool()> onReferenceExpired = nullptr) {
	session->data().chunkFetcher().fetch(
		ChunkRequest{ location, offset, limit, takeoutId, dcId },
		[=](ChunkResult result) {
			if (result.kind == ChunkResult::Kind::Reference) {
				if (onReferenceExpired && onReferenceExpired()) {
					return;
				}
				onBytes(QByteArray());
			} else if (result.kind == ChunkResult::Kind::Done) {
				onBytes(std::move(result.bytes));
			} else {
				onBytes(QByteArray());
			}
		});
}

} // namespace

int FloodWaitSeconds(const QString &type) {
	if (!type.startsWith(u"FLOOD_"_q) || !type.contains(u"WAIT"_q)) {
		return 0;
	}
	return std::max(type.mid(type.lastIndexOf(u"_"_q) + 1).toInt(), 0);
}

// Two sample offsets (middle and tail) used for the partial fingerprint.
// Both local and remote fingerprinting must use identical offsets so the
// resulting hash matches. Both offsets are aligned down to 1 KB, which
// upload.getFile requires even with the `precise` flag, and each sample is
// kept inside a single 1 MB block. The tail is aligned down from
// size - kDedupChunk, so at most 1 KB at EOF normally stays outside the
// sample (a block adjustment can move it up to 16 KB earlier).
void DedupSampleOffsets(
		int64 size,
		int64 &middleOffset,
		int64 &tailOffset) {
	middleOffset = (size / 2 / kDedupAlignment) * kDedupAlignment;
	middleOffset = std::max<int64>(middleOffset, kDedupHeadGap);
	tailOffset = std::max<int64>(0, size - kDedupChunk);
	tailOffset = (tailOffset / kDedupAlignment) * kDedupAlignment;
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
	ensureWithinBlock(middleOffset);
	ensureWithinBlock(tailOffset);
}

QByteArray HashChunks(
		int64 size,
		const QByteArray &middle,
		const QByteArray &tail) {
	auto state = XXH3_state_t();
	XXH3_128bits_reset(&state);
	auto sizeBytes = std::array<uchar, 8>();
	auto value = static_cast<uint64>(size);
	for (auto i = 0; i != 8; ++i) {
		sizeBytes[i] = uchar(value >> (56 - i * 8));
	}
	XXH3_128bits_update(&state, sizeBytes.data(), sizeBytes.size());
	if (!middle.isEmpty()) {
		XXH3_128bits_update(
			&state,
			middle.constData(),
			static_cast<size_t>(middle.size()));
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
	if (size <= kDedupMinPartialHashSize) {
		const auto content = ReadHash(path, 0, size);
		if (content.isEmpty()) {
			return QByteArray();
		}
		return ContentFingerprint(content);
	}
	int64 middleOffset = 0;
	int64 tailOffset = 0;
	DedupSampleOffsets(size, middleOffset, tailOffset);
	const auto middle = ReadHash(path, middleOffset, kDedupChunk);
	const auto tail = ReadHash(path, tailOffset, kDedupChunk);
	if (middle.isEmpty() || tail.isEmpty()) {
		return QByteArray();
	}
	return HashChunks(size, middle, tail);
}

QByteArray ContentFingerprint(const QByteArray &content) {
	if (content.isEmpty()) {
		return QByteArray();
	}
	const auto size = int64(content.size());
	if (size <= kDedupMinPartialHashSize) {
		return HashChunks(size, content, QByteArray());
	}
	int64 middleOffset = 0;
	int64 tailOffset = 0;
	DedupSampleOffsets(size, middleOffset, tailOffset);
	return HashChunks(
		size,
		content.mid(int(middleOffset), kDedupChunk),
		content.mid(int(tailOffset), kDedupChunk));
}

namespace {

[[nodiscard]] MTP::ShiftedDcId FingerprintDcId(int dcId, int dcShift) {
	return (dcShift < 0)
		? MTP::ShiftDcId(dcId, kDedupChunkDcShift)
		: MTP::ShiftDcId(dcId, dcShift);
}

[[nodiscard]] MTP::ShiftedDcId NextFingerprintLane(
		MTP::ShiftedDcId dcId) {
	const auto shift = MTP::GetDcIdShift(dcId);
	if (shift < MTP::kBaseDownloadDcShift) {
		return dcId;
	}
	static auto next = std::atomic<int>(0);
	const auto lane = next.fetch_add(1, std::memory_order_relaxed)
		% kDedupChunkLaneCount;
	return MTP::ShiftDcId(
		MTP::BareDcId(dcId),
		kDedupChunkDcShift + lane);
}

void RemoteFingerprint(
		not_null<Main::Session*> session,
		const MTPInputFileLocation &location,
		MTP::ShiftedDcId dcId,
		int64 size,
		Fn<void(QByteArray)> done,
		uint64 takeoutId,
		Fn<bool()> onReferenceExpired) {
	const auto abandoned = std::make_shared<bool>(false);
	const auto report = [done, abandoned](QByteArray hash) {
		if (!*abandoned) {
			done(std::move(hash));
		}
	};
	// Only the first dead file reference of an attempt reaches the owner:
	// it re-queues the item once, and the remaining chunks of this attempt
	// are then silently dropped instead of releasing its slot twice.
	const auto expired = [onReferenceExpired, abandoned] {
		if (*abandoned) {
			return true;
		}
		if (!onReferenceExpired || !onReferenceExpired()) {
			return false;
		}
		*abandoned = true;
		return true;
	};
	if (size <= kDedupMinPartialHashSize) {
		const auto limit = ((size + kDedupAlignment - 1) / kDedupAlignment)
			* kDedupAlignment;
		SendChunk(
			session,
			location,
			NextFingerprintLane(dcId),
			takeoutId,
			0,
			int(limit),
			[report, size](QByteArray content) {
				report((content.size() != size)
					? QByteArray()
					: ContentFingerprint(content));
			},
			expired);
		return;
	}

	int64 middleOffset = 0;
	int64 tailOffset = 0;
	DedupSampleOffsets(size, middleOffset, tailOffset);
	const auto chunk = int64(kDedupChunk);
	const auto middle = std::make_shared<QByteArray>();
	const auto tail = std::make_shared<QByteArray>();
	const auto pending = std::make_shared<int>(2);
	// 'failed' makes sure 'done' fires exactly once no matter which of the
	// two requests fails: every completion path funnels through doneHash(),
	// and doneHash() reports only when both requests have settled.
	const auto failed = std::make_shared<bool>(false);
	const auto doneHash = [size, middle, tail, pending, failed, report] {
		if (--*pending != 0) {
			return;
		}
		report(*failed
			? QByteArray()
			: HashChunks(size, *middle, *tail));
	};
	SendChunk(
		session,
		location,
		NextFingerprintLane(dcId),
		takeoutId,
		middleOffset,
		int(chunk),
		[middle, failed, doneHash, chunk](QByteArray bytes) {
			if (bytes.size() != chunk) {
				*failed = true;
			} else {
				*middle = std::move(bytes);
			}
			doneHash();
		},
		expired);
	SendChunk(
		session,
		location,
		NextFingerprintLane(dcId),
		takeoutId,
		tailOffset,
		int(chunk),
		[tail, failed, doneHash, chunk](QByteArray bytes) {
			if (bytes.size() != chunk) {
				*failed = true;
			} else {
				*tail = std::move(bytes);
			}
			doneHash();
		},
		expired);
}

} // namespace

void RemoteFileFingerprint(
		not_null<Main::Session*> session,
		not_null<DocumentData*> document,
		Fn<void(QByteArray)> done,
		uint64 takeoutId,
		Fn<bool()> onReferenceExpired,
		int dcShift) {
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
	const auto dcId = FingerprintDcId(document->getDC(), dcShift);
	RemoteFingerprint(
		session,
		location,
		dcId,
		size,
		std::move(done),
		takeoutId,
		std::move(onReferenceExpired));
}

void RemotePhotoFingerprint(
		not_null<Main::Session*> session,
		not_null<PhotoData*> photo,
		Fn<void(QByteArray)> done,
		uint64 takeoutId,
		Fn<bool()> onReferenceExpired,
		int dcShift) {
	const auto &imageLocation = photo->location(Data::PhotoSize::Large);
	const auto storage = std::get_if<StorageFileLocation>(
		&imageLocation.file().data);
	if (!storage || !storage->valid()) {
		done(QByteArray());
		return;
	}
	auto size = int64(photo->imageByteSize(Data::PhotoSize::Large));
	if (!size) {
		size = photo->videoByteSize(Data::PhotoSize::Large);
	}
	if (size <= 0) {
		done(QByteArray());
		return;
	}
	const auto dcId = FingerprintDcId(storage->dcId(), dcShift);
	const auto location = storage->tl(session->userId());
	RemoteFingerprint(
		session,
		location,
		dcId,
		size,
		std::move(done),
		takeoutId,
		std::move(onReferenceExpired));
}

} // namespace Data
