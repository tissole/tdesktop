/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_document.h"
#include "export/export_settings.h"

enum class DocumentMediaKind {
	Sticker,
	RoundVideo,
	Voice,
	GIF,
	Video,
	Music,
	File,
};

[[nodiscard]] inline DocumentMediaKind CategorizeDocument(
		const DocumentData *document) {
	if (!document) {
		return DocumentMediaKind::File;
	} else if (document->sticker()) {
		return DocumentMediaKind::Sticker;
	} else if (document->isVideoMessage()) {
		return DocumentMediaKind::RoundVideo;
	} else if (document->isVoiceMessage()) {
		return DocumentMediaKind::Voice;
	} else if (document->type == AnimatedDocument) {
		return DocumentMediaKind::GIF;
	} else if (document->type == VideoDocument) {
		return DocumentMediaKind::Video;
	} else if (document->isSong()) {
		// The server Music filter only indexes audio with music metadata.
		// Audio documents with no title and no performer (audiobooks,
		// podcasts, hash-named files) belong to Files, not Music.
		const auto song = document->song();
		if (song && (!song->title.isEmpty() || !song->performer.isEmpty())) {
			return DocumentMediaKind::Music;
		}
	}
	return DocumentMediaKind::File;
}

[[nodiscard]] inline Export::MediaSettings::Type DocumentMediaCategory(
		const DocumentData *document) {
	using Type = Export::MediaSettings::Type;
	switch (CategorizeDocument(document)) {
	case DocumentMediaKind::Sticker: return Type::Sticker;
	case DocumentMediaKind::RoundVideo: return Type::VideoMessage;
	case DocumentMediaKind::Voice: return Type::VoiceMessage;
	case DocumentMediaKind::GIF: return Type::GIF;
	case DocumentMediaKind::Video: return Type::Video;
	case DocumentMediaKind::Music: return Type::Audio;
	case DocumentMediaKind::File: return Type::File;
	}
	return Type::File;
}

