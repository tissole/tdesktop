/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QByteArray>

class DocumentData;

namespace Main {
class Session;
} // namespace Main

namespace Data {

// Content fingerprint of a file, used for duplicate detection across
// downloads, uploads and forwards.
//
// Files at least kMinPartialHashSize bytes long are fingerprinted from two
// sampled chunks (one after a header skip, one at the very end) combined into
// a single 128-bit hash. Smaller files are fingerprinted from their whole
// contents. In both cases the result is exactly one hash.
constexpr auto kDedupHeadSkip = 256 * 1024;
constexpr auto kDedupChunk = 16 * 1024;
constexpr auto kDedupMinPartialHashSize = 320 * 1024;
constexpr auto kDedupSizeBucket = 1024 * 1024;
constexpr auto kDedupBlock = 1024 * 1024;
constexpr auto kDedupAlignment = 1 * 1024;

// Returns the 16-byte XXH3_128bits hash of a local file's fingerprint.
// Empty array if the file cannot be read or is empty.
[[nodiscard]] QByteArray FileFingerprint(const QString &path, int64 size);

// Returns the 16-byte XXH3_128bits hash of in-memory content.
[[nodiscard]] QByteArray ContentFingerprint(const QByteArray &content);

// Combines two sampled byte chunks (head and tail) into the same 128-bit
// fingerprint used for files. Shared by local and remote fingerprinting.
[[nodiscard]] QByteArray HashChunks(
	const QByteArray &head,
	const QByteArray &tail);

// Fetches two sampled chunks of a remote document via upload.getFile and
// computes the same 128-bit fingerprint used for local files. Reused by
// download dedup and forward dedup so both operate on identical content
// fingerprints. The result is passed to the callback (empty on failure).
void RemoteFileFingerprint(
	not_null<Main::Session*> session,
	not_null<DocumentData*> document,
	Fn<void(QByteArray)> done);

} // namespace Data
