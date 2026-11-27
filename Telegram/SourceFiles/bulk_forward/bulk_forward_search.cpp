/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk_forward/bulk_forward_search.h"

namespace BulkForward {

std::vector<SearchPiece> AllSearchPieces() {
	return {
		SearchPiece::PhotoVideo,
		SearchPiece::Voice,
		SearchPiece::Round,
		SearchPiece::Gif,
		SearchPiece::Document,
		SearchPiece::Music,
		SearchPiece::Url,
		SearchPiece::Poll,
		SearchPiece::Contacts,
		SearchPiece::Geo,
	};
}

std::vector<SearchPiece> SelectedSearchPieces(MediaTypes types) {
	auto result = std::vector<SearchPiece>();
	const auto photo = (types & MediaType::Photo).value() != 0;
	const auto video = (types & MediaType::Video).value() != 0;
	if (photo && video) {
		result.push_back(SearchPiece::PhotoVideo);
	} else if (photo) {
		result.push_back(SearchPiece::Photo);
	} else if (video) {
		result.push_back(SearchPiece::Video);
	}
	if ((types & MediaType::Voice).value()) {
		result.push_back(SearchPiece::Voice);
	}
	if ((types & MediaType::RoundVideo).value()) {
		result.push_back(SearchPiece::Round);
	}
	if ((types & MediaType::Gif).value()) {
		result.push_back(SearchPiece::Gif);
	}
	if ((types & MediaType::File).value()) {
		result.push_back(SearchPiece::Document);
	}
	if ((types & MediaType::Audio).value()) {
		result.push_back(SearchPiece::Music);
	}
	if ((types & MediaType::Link).value()) {
		result.push_back(SearchPiece::Url);
	}
	if ((types & MediaType::Poll).value()) {
		result.push_back(SearchPiece::Poll);
	}
	if ((types & MediaType::Other).value()) {
		result.push_back(SearchPiece::Contacts);
		result.push_back(SearchPiece::Geo);
	}
	return result;
}

MTPMessagesFilter SearchFilterFor(SearchPiece piece) {
	switch (piece) {
	case SearchPiece::PhotoVideo:
		return MTPMessagesFilter(MTP_inputMessagesFilterPhotoVideo());
	case SearchPiece::Photo:
		return MTPMessagesFilter(MTP_inputMessagesFilterPhotos());
	case SearchPiece::Video:
		return MTPMessagesFilter(MTP_inputMessagesFilterVideo());
	case SearchPiece::Voice:
		return MTPMessagesFilter(MTP_inputMessagesFilterVoice());
	case SearchPiece::Round:
		return MTPMessagesFilter(MTP_inputMessagesFilterRoundVideo());
	case SearchPiece::Gif:
		return MTPMessagesFilter(MTP_inputMessagesFilterGif());
	case SearchPiece::Document:
		return MTPMessagesFilter(MTP_inputMessagesFilterDocument());
	case SearchPiece::Music:
		return MTPMessagesFilter(MTP_inputMessagesFilterMusic());
	case SearchPiece::Url:
		return MTPMessagesFilter(MTP_inputMessagesFilterUrl());
	case SearchPiece::Poll:
		return MTPMessagesFilter(MTP_inputMessagesFilterPoll());
	case SearchPiece::Contacts:
		return MTPMessagesFilter(MTP_inputMessagesFilterContacts());
	case SearchPiece::Geo:
		return MTPMessagesFilter(MTP_inputMessagesFilterGeo());
	}
	return MTPMessagesFilter(MTP_inputMessagesFilterEmpty());
}

std::optional<SearchPiece> SingleSearchPiece(MediaTypes types) {
	// Text and stickers have no index of their own, so a selection that
	// includes them must walk the history even if it has one media type.
	if ((types & MediaType::Text).value()
		|| (types & MediaType::Sticker).value()) {
		return std::nullopt;
	}
	const auto pieces = SelectedSearchPieces(types);
	if (pieces.size() != 1) {
		return std::nullopt;
	}
	return pieces.front();
}

} // namespace BulkForward
