/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_document.h"
#include "export/export_settings.h"

[[nodiscard]] inline Export::MediaSettings::Type DocumentMediaCategory(
		const DocumentData *document) {
	using Type = Export::MediaSettings::Type;
	if (!document) {
		return Type::File;
	} else if (document->sticker()) {
		return Type::Sticker;
	} else if (document->isVideoMessage()) {
		return Type::VideoMessage;
	} else if (document->isVoiceMessage()) {
		return Type::VoiceMessage;
	} else if (document->isAnimation()) {
		return Type::GIF;
	} else if (document->isVideoFile()) {
		return Type::Video;
	} else if (document->isAudioFile()) {
		return Type::Audio;
	}
	return Type::File;
}

