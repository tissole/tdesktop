/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "bulk_forward/bulk_forward_filter.h"
#include "data/data_file_hash.h"
#include "data/data_types.h"
#include "scheme.h"

#include <QString>

#include <optional>

namespace BulkForward {

enum class Kind : uchar {
	Photo,
	Video,
	Voice,
	RoundVideo,
	Sticker,
	Gif,
	File,
	Audio,
	Text,
	Link,
	Poll,
	Other,
};

struct SourceMessage {
	FullMsgId id;
	TimeId date = 0;
	qint64 size = 0;
	MessageGroupId groupId;
	Kind kind = Kind::Text;
	QString extension;
	uint64 mediaId = 0;
	// Describes the remote file for the dedup fingerprint sample, so the send
	// path does not have to register every scanned media in the session.
	Data::RemoteSample sample;
	bool isPhoto = false;
	bool isProtected = false;
};

// `peerProtected` is EnhancedForward::PeerNeedsEnhancedForward(peer): the
// chat itself is restricted from forwarding, or a forward already failed with
// a protected-content error. The per-message noforwards flag and an all-platform
// restriction reason are read from the message itself inside Classify().
[[nodiscard]] std::optional<SourceMessage> Classify(
	const MTPMessage &message,
	PeerId peer,
	bool peerProtected = false);

[[nodiscard]] bool Matches(
	const Filter &filter,
	const SourceMessage &message);
[[nodiscard]] bool MatchesContent(
	const Filter &filter,
	const SourceMessage &message);
[[nodiscard]] bool MatchesRange(
	const Filter &filter,
	const SourceMessage &message);

[[nodiscard]] MediaType MediaTypeOf(Kind kind);
[[nodiscard]] bool IsDocumentKind(Kind kind);

[[nodiscard]] QString ExtensionFromFilename(const QString &filename);

} // namespace BulkForward
