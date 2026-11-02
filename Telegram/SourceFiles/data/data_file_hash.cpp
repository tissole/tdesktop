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

int DedupChunkLaneIndex(int shiftedDcId) {
	const auto lane = MTP::GetDcIdShift(shiftedDcId) - kDedupChunkDcShift;
	return (lane >= 0 && lane < kDedupChunkLaneCount) ? lane : -1;
}

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

// Middle sample offset used for the partial fingerprint. Local and remote
// fingerprinting must use the identical offset so the resulting hash matches.
// The offset is aligned down to 1 KB, which upload.getFile requires even with
// the `precise` flag, and the sample is kept inside a single 1 MB block. The
// kDedupHeadGap clamp keeps the sample out of the common container header.
int64 DedupMiddleOffset(int64 size) {
	auto offset = (size / 2 / kDedupAlignment) * kDedupAlignment;
	offset = std::max<int64>(offset, kDedupHeadGap);
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
	return offset;
}

QByteArray HashChunk(
		int64 size,
		const QByteArray &middle) {
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
	const auto offset = DedupMiddleOffset(size);
	const auto middle = ReadHash(path, offset, kDedupChunk);
	if (middle.isEmpty()) {
		return QByteArray();
	}
	return HashChunk(size, middle);
}

QByteArray ContentFingerprint(const QByteArray &content) {
	if (content.isEmpty()) {
		return QByteArray();
	}
	const auto size = int64(content.size());
	if (size <= kDedupMinPartialHashSize) {
		return HashChunk(size, content);
	}
	const auto offset = DedupMiddleOffset(size);
	return HashChunk(size, content.mid(int(offset), kDedupChunk));
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
	// it re-queues the item once, and the result of this attempt is then
	// silently dropped instead of releasing its slot twice.
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

	const auto offset = DedupMiddleOffset(size);
	const auto chunk = int64(kDedupChunk);
	SendChunk(
		session,
		location,
		NextFingerprintLane(dcId),
		takeoutId,
		offset,
		int(chunk),
		[report, size, chunk](QByteArray bytes) {
			report((bytes.size() != chunk)
				? QByteArray()
				: HashChunk(size, bytes));
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
