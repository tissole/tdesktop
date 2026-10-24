/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QByteArray>

class DocumentData;
class PhotoData;

namespace Main {
class Session;
} // namespace Main

namespace Data {

// All 16 download shifts 16..31 are used for dedup fingerprints. Chunks are
// round-robin across the lanes, but every lane of one bare DC shares a single
// limiter and window (128 total, cwnd 96). That is the proven 16-slot layout:
// 143/s at 400-500 ms p95 before the server cut, 123-127/s at the 130 target.
constexpr auto kDedupChunkDcShift = 16;
constexpr auto kDedupChunkLaneCount = 16;

// Content fingerprint of a file, used for duplicate detection across
// downloads, uploads and forwards.
//
// Files at most kDedupMinPartialHashSize bytes long are fingerprinted from
// their whole contents. Larger files are fingerprinted from two 16 KB samples,
// one at the middle and one from the last 16 KB, never from the file header. The
// file size is the first input of every fingerprint, so equal hashes imply
// equal size and equal sampled content. In all cases the result is exactly
// one hash.
constexpr auto kDedupChunk = 16 * 1024;
constexpr auto kDedupHeadGap = 64 * 1024;
constexpr auto kDedupSampleGap = 32 * 1024;
constexpr auto kDedupMinPartialHashSize = 2 * kDedupHeadGap;
constexpr auto kDedupBlock = 1024 * 1024;
constexpr auto kDedupAlignment = 1 * 1024;

// Two sample offsets (middle, tail) for the partial fingerprint. Defined for
// sizes above kDedupMinPartialHashSize. Shared by local, remote and export
// fingerprinting so hashes match.
void DedupSampleOffsets(int64 size, int64 &middleOffset, int64 &tailOffset);

// Returns the 16-byte XXH3_128bits hash of a local file's fingerprint.
// Empty array if the file cannot be read or is empty.
[[nodiscard]] QByteArray FileFingerprint(const QString &path, int64 size);

// Returns the 16-byte XXH3_128bits hash of in-memory content, using the same
// whole-file or sampled plan as files of that size.
[[nodiscard]] QByteArray ContentFingerprint(const QByteArray &content);

// Combines the file size and two sampled byte chunks (middle and tail) into
// the same 128-bit fingerprint used for files. Shared by local and remote
// fingerprinting.
[[nodiscard]] QByteArray HashChunks(
	int64 size,
	const QByteArray &middle,
	const QByteArray &tail);

// Fetches the parts of a remote document that its fingerprint needs and
// computes the same 128-bit fingerprint used for local files. Reused by
// download dedup and forward dedup so both operate on identical content
// fingerprints. The result is passed to the callback (empty on failure).
// A chunk request rejected because of a dead file reference asks
// `onReferenceExpired` first: when it returns true the caller took the item
// over to re-resolve it and no result is reported for this attempt.
//
// A negative `dcShift` uses kDedupChunkDcShift. A non-negative value
// selects that MTP shift on the file's DC.
void RemoteFileFingerprint(
	not_null<Main::Session*> session,
	not_null<DocumentData*> document,
	Fn<void(QByteArray)> done,
	uint64 takeoutId = 0,
	Fn<bool()> onReferenceExpired = nullptr,
	int dcShift = -1);

// Same plan as RemoteFileFingerprint, using the photo's storage location.
// Empty hash on failure or on truncated reads.
void RemotePhotoFingerprint(
	not_null<Main::Session*> session,
	not_null<PhotoData*> photo,
	Fn<void(QByteArray)> done,
	uint64 takeoutId = 0,
	Fn<bool()> onReferenceExpired = nullptr,
	int dcShift = -1);

// Parses a server flood error ("FLOOD_WAIT_X", "FLOOD_PREMIUM_WAIT_X")
// into ordered wait seconds (0 when not a flood error).
[[nodiscard]] int FloodWaitSeconds(const QString &type);

} // namespace Data
