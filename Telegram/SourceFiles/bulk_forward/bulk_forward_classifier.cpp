/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "bulk_forward/bulk_forward_classifier.h"

#include "mtproto/core_types.h"
#include "scheme.h"

#include <algorithm>

namespace BulkForward {
namespace {

[[nodiscard]] qint64 LargestPhotoSize(const MTPDphoto &photo) {
	auto result = qint64(0);
	for (const auto &size : photo.vsizes().v) {
		size.match([&](const MTPDphotoSize &data) {
			result = std::max(result, qint64(data.vsize().v));
		}, [&](const MTPDphotoSizeProgressive &data) {
			for (const auto &value : data.vsizes().v) {
				result = std::max(result, qint64(value.v));
			}
		}, [&](const auto &) {
		});
	}
	return result;
}

// Mirrors how a photo's large size is chosen for PhotoData: the largest
// progressive size wins, otherwise the best level letter. The fingerprint has
// to sample the same file the download and upload paths would sample.
[[nodiscard]] Data::RemoteSample PhotoSampleOf(const MTPDphoto &photo) {
	const auto &sizes = photo.vsizes().v;
	const auto progressive = [&]() -> const MTPPhotoSize* {
		const auto largest = [&]() -> const MTPPhotoSize* {
			auto result = static_cast<const MTPPhotoSize*>(nullptr);
			auto bestArea = int64(0);
			for (const auto &size : sizes) {
				const auto area = size.match(
					[](const MTPDphotoSizeProgressive &data) {
						return int64(data.vw().v) * data.vh().v;
					},
					[](const auto &) {
						return int64(0);
					});
				if (!result || area > bestArea) {
					result = &size;
					bestArea = area;
				}
			}
			return result;
		}();
		return (largest && largest->type() == mtpc_photoSizeProgressive)
			? largest
			: nullptr;
	}();
	auto chosen = progressive;
	if (!chosen) {
		const auto levels = QByteArray("ydxcwmbsa");
		auto bestIndex = int(levels.size());
		for (const auto &size : sizes) {
			const auto letter = size.match([](const MTPDphotoSizeEmpty &) {
				return char(0);
			}, [](const auto &data) {
				const auto &type = data.vtype().v;
				return type.isEmpty() ? char(0) : type[0];
			});
			const auto index = int(levels.indexOf(letter));
			if (index >= 0 && index < bestIndex) {
				bestIndex = index;
				chosen = &size;
			}
		}
	}
	if (!chosen) {
		return {};
	}
	auto result = Data::RemoteSample();
	chosen->match([&](const MTPDphotoSize &data) {
		result.sizeType = data.vtype().v;
		result.size = int64(data.vsize().v);
	}, [&](const MTPDphotoSizeProgressive &data) {
		result.sizeType = data.vtype().v;
		result.size = data.vsizes().v.isEmpty()
			? 0
			: int64(data.vsizes().v.back().v);
	}, [&](const auto &) {
	});
	if (result.size <= 0 || result.sizeType.isEmpty()) {
		return {};
	}
	result.id = uint64(photo.vid().v);
	result.accessHash = uint64(photo.vaccess_hash().v);
	result.fileReference = photo.vfile_reference().v;
	result.dcId = photo.vdc_id().v;
	result.isPhoto = true;
	return result;
}

[[nodiscard]] bool MatchesType(MediaTypes types, Kind kind) {
	return (types & MediaTypeOf(kind)).value() != 0;
}

[[nodiscard]] bool MatchesSize(
		const Filter &filter,
		const SourceMessage &message) {
	if (message.size <= 0) {
		return true;
	}
	if (filter.minSize > 0 && message.size < filter.minSize) {
		return false;
	}
	if (filter.maxSize > 0 && message.size > filter.maxSize) {
		return false;
	}
	return true;
}

[[nodiscard]] bool MatchesExtension(
		const Filter &filter,
		const SourceMessage &message) {
	if (filter.extensionMode == ExtensionMode::None) {
		return true;
	} else if (!IsDocumentKind(message.kind)) {
		return true;
	}
	const auto listed = [&] {
		for (const auto &value : filter.extensions) {
			if (NormalizeExtension(value) == message.extension) {
				return true;
			}
		}
		return false;
	}();
	return (filter.extensionMode == ExtensionMode::Whitelist)
		? listed
		: !listed;
}

} // namespace

MediaType MediaTypeOf(Kind kind) {
	switch (kind) {
	case Kind::Photo: return MediaType::Photo;
	case Kind::Video: return MediaType::Video;
	case Kind::Voice: return MediaType::Voice;
	case Kind::RoundVideo: return MediaType::RoundVideo;
	case Kind::Sticker: return MediaType::Sticker;
	case Kind::Gif: return MediaType::Gif;
	case Kind::File: return MediaType::File;
	case Kind::Audio: return MediaType::Audio;
	case Kind::Text: return MediaType::Text;
	case Kind::Link: return MediaType::Link;
	case Kind::Poll: return MediaType::Poll;
	case Kind::Other: return MediaType::Other;
	}
	return MediaType::Other;
}

bool IsDocumentKind(Kind kind) {
	switch (kind) {
	case Kind::Video:
	case Kind::Voice:
	case Kind::RoundVideo:
	case Kind::Sticker:
	case Kind::Gif:
	case Kind::File:
	case Kind::Audio:
		return true;
	case Kind::Photo:
	case Kind::Text:
	case Kind::Link:
	case Kind::Poll:
	case Kind::Other:
		return false;
	}
	return false;
}

QString ExtensionFromFilename(const QString &filename) {
	const auto dot = filename.lastIndexOf(u'.');
	if (dot < 0 || dot + 1 >= filename.size()) {
		return {};
	}
	return filename.mid(dot + 1).trimmed().toLower();
}

[[nodiscard]] bool HasAllPlatformRestriction(
		const MTPVector<MTPRestrictionReason> *list) {
	if (!list) {
		return false;
	}
	for (const auto &reason : list->v) {
		const auto allPlatforms = reason.match([](
				const MTPDrestrictionReason &data) {
			return (data.vplatform().v == u"all"_q);
		}, [](const auto &) {
			return false;
		});
		if (allPlatforms) {
			return true;
		}
	}
	return false;
}

std::optional<SourceMessage> Classify(
		const MTPMessage &message,
		PeerId peer,
		bool peerProtected) {
	return message.match([&](
			const MTPDmessage &data) -> std::optional<SourceMessage> {
		const auto id = data.vid().v;
		if (id <= 0) {
			return std::nullopt;
		}
		auto result = SourceMessage();
		result.id = FullMsgId(peer, MsgId(id));
		result.date = TimeId(data.vdate().v);
		result.isProtected = peerProtected
			|| data.is_noforwards()
			|| HasAllPlatformRestriction(data.vrestriction_reason());

		if (const auto grouped = data.vgrouped_id()) {
			result.groupId = MessageGroupId::FromRaw(
				peer,
				uint64(grouped->v),
				false);
		}

		const auto media = data.vmedia();
		if (!media) {
			result.kind = Kind::Text;
			return result;
		}
		media->match([&](const MTPDmessageMediaPhoto &mediaData) {
			result.kind = Kind::Photo;
			if (const auto photo = mediaData.vphoto()) {
				photo->match([&](const MTPDphoto &photoData) {
					result.mediaId = uint64(photoData.vid().v);
					result.isPhoto = true;
					result.size = LargestPhotoSize(photoData);
					result.sample = PhotoSampleOf(photoData);
				}, [&](const MTPDphotoEmpty &) {
				});
			}
		}, [&](const MTPDmessageMediaDocument &mediaData) {
			result.kind = Kind::File;
			if (const auto document = mediaData.vdocument()) {
				document->match([&](const MTPDdocument &documentData) {
					result.mediaId = uint64(documentData.vid().v);
					result.size = std::max(
						qint64(documentData.vsize().v),
						qint64(0));
					result.sample = {
						.id = uint64(documentData.vid().v),
						.accessHash = uint64(documentData.vaccess_hash().v),
						.fileReference = documentData.vfile_reference().v,
						.dcId = int(documentData.vdc_id().v),
						.size = result.size,
						.isPhoto = false,
					};

					auto hasSticker = false;
					auto hasAnimated = false;
					auto hasVideo = false;
					auto hasRound = false;
					auto hasAudio = false;
					auto hasVoice = false;
					for (const auto &attribute
						: documentData.vattributes().v) {
						attribute.match([&](
								const MTPDdocumentAttributeSticker &) {
							hasSticker = true;
						}, [&](const MTPDdocumentAttributeAnimated &) {
							hasAnimated = true;
						}, [&](const MTPDdocumentAttributeVideo &video) {
							if (video.is_round_message()) {
								hasRound = true;
							} else {
								hasVideo = true;
							}
						}, [&](const MTPDdocumentAttributeAudio &audio) {
							if (audio.is_voice()) {
								hasVoice = true;
							} else {
								hasAudio = true;
							}
						}, [&](const MTPDdocumentAttributeFilename &file) {
							result.extension = ExtensionFromFilename(
								qs(file.vfile_name()));
						}, [&](const auto &) {
						});
					}

					const auto mime = qs(documentData.vmime_type());
					if (hasSticker) {
						result.kind = Kind::Sticker;
					} else if (hasRound || mediaData.is_round()) {
						result.kind = Kind::RoundVideo;
					} else if (hasVoice || mediaData.is_voice()) {
						result.kind = Kind::Voice;
					} else if (hasAnimated) {
						result.kind = Kind::Gif;
					} else if (hasVideo
						|| mediaData.is_video()
						|| mime.startsWith(u"video/"_q)) {
						result.kind = Kind::Video;
					} else if (hasAudio || mime.startsWith(u"audio/"_q)) {
						result.kind = Kind::Audio;
					} else {
						result.kind = Kind::File;
					}
				}, [&](const MTPDdocumentEmpty &) {
				});
			}
		}, [&](const MTPDmessageMediaWebPage &) {
			result.kind = Kind::Link;
		}, [&](const MTPDmessageMediaPoll &) {
			result.kind = Kind::Poll;
		}, [&](const auto &) {
			result.kind = Kind::Other;
		});
		return result;
	}, [](const auto &) -> std::optional<SourceMessage> {
		return std::nullopt;
	});
}

bool MatchesRange(const Filter &filter, const SourceMessage &message) {
	switch (filter.scope) {
	case Scope::All:
		return true;
	case Scope::DateRange:
		if (filter.fromDate && message.date < filter.fromDate) {
			return false;
		}
		if (filter.tillDate && message.date > filter.tillDate) {
			return false;
		}
		return true;
	case Scope::IdRange:
		if (filter.fromId && message.id.msg < filter.fromId) {
			return false;
		}
		if (filter.tillId && message.id.msg > filter.tillId) {
			return false;
		}
		return true;
	case Scope::Selection:
		return true;
	}
	return false;
}

bool MatchesContent(const Filter &filter, const SourceMessage &message) {
	return MatchesType(filter.types, message.kind)
		&& MatchesSize(filter, message)
		&& MatchesExtension(filter, message);
}

bool Matches(const Filter &filter, const SourceMessage &message) {
	if (!MatchesRange(filter, message)) {
		return false;
	} else if (!MatchesContent(filter, message)) {
		return false;
	} else if (filter.scope == Scope::Selection) {
		return std::find(
			begin(filter.selection),
			end(filter.selection),
			message.id) != end(filter.selection);
	}
	return true;
}

} // namespace BulkForward
