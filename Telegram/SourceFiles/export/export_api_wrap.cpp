/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/export_api_wrap.h"

#include "settings.h"
#include "data/data_dedup_db.h"
#include "data/data_file_hash.h"
#include "export/export_dedup.h"
#include "export/export_settings.h"
#include "export/output/export_output_stats.h"
#include "export/data/export_data_types.h"
#include "export/output/export_output_result.h"
#include "export/output/export_output_file.h"
#include "mtproto/mtproto_response.h"
#include <crl/crl_on_main.h>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include "base/concurrent_timer.h"
#include "base/bytes.h"
#include "base/options.h"
#include "base/random.h"
#include <set>
#include <deque>
#include <algorithm>

#include <QDateTime>

namespace Export {
namespace {

constexpr auto kUserpicsSliceLimit = 100;
constexpr auto kFileChunkSize = 1024 * 1024;
constexpr auto kFileRequestsCount = 2;
//constexpr auto kFileNextRequestDelay = crl::time(20);
constexpr auto kChatsSliceLimit = 100;
constexpr auto kMessagesSliceLimit = 100;
constexpr auto kSmallHashParallel = 6;
const auto kSmallStartSpacing = crl::time(30);
const auto kProbeStartSpacing = crl::time(40);
constexpr auto kTopPeerSliceLimit = 100;
constexpr auto kFileMaxSize = 4000 * int64(1024 * 1024);
constexpr auto kHydrateParallel = 4;
constexpr auto kWalkHashAttempts = 20;
constexpr auto kMaxEmojiPerRequest = 100;
constexpr auto kStoriesSliceLimit = 100;
constexpr auto kProfileMusicSliceLimit = 100;

[[nodiscard]] MediaSettings::Types scanFileTypes() {
	using Type = MediaSettings::Type;
	return Type::Photo | Type::Video | Type::VoiceMessage
		| Type::VideoMessage | Type::Sticker | Type::GIF | Type::File
		| Type::Audio;
}

[[nodiscard]] int SearchTotalSkippingUnions(
		const std::vector<MTPMessagesFilter> &filters,
		const std::vector<std::vector<int>> &counts) {
	// Measured: RoundVoice matches the union of Voice and
	// RoundVideo notes, so summing all three triple-counts. The
	// union is skipped: Voice and RoundVideo counts are exact on
	// their own, and any hypothetical RoundVoice-only note still
	// degrades gracefully (walks visit every filter, the max()
	// guard caps the bar).
	const auto hasVoice = ranges::any_of(
		filters,
		[](const auto &filter) {
			return filter.type() == mtpc_inputMessagesFilterVoice;
		});
	auto total = 0;
	for (auto i = 0; i != int(filters.size()); ++i) {
		const auto type = filters[i].type();
		if (hasVoice
			&& type == mtpc_inputMessagesFilterRoundVoice) {
			continue;
		}
		for (const auto count : counts[i]) {
			total += count;
		}
	}
	return total;
}

[[nodiscard]] std::vector<MTPMessagesFilter> ScanSearchFilters(
	MediaSettings::Types types) {
	using Type = MediaSettings::Type;
	const auto has = [&](Type type) {
		return ((types & type) == type);
	};
	if (has(Type::Text) || has(Type::FullHistory)) {
		return {};
	}
	auto result = std::vector<MTPMessagesFilter>();
	if (has(Type::Photo) && has(Type::Video)) {
		result.push_back(MTP_inputMessagesFilterPhotoVideo());
	} else {
		if (has(Type::Photo)) {
			result.push_back(MTP_inputMessagesFilterPhotos());
		}
		if (has(Type::Video)) {
			result.push_back(MTP_inputMessagesFilterVideo());
		}
	}
	if (has(Type::VoiceMessage)) {
		result.push_back(MTP_inputMessagesFilterVoice());
		result.push_back(MTP_inputMessagesFilterRoundVoice());
	}
	if (has(Type::VideoMessage)) {
		result.push_back(MTP_inputMessagesFilterRoundVideo());
	}
	if (has(Type::GIF)) {
		result.push_back(MTP_inputMessagesFilterGif());
	}
	if (has(Type::File) || has(Type::Sticker)) {
		result.push_back(MTP_inputMessagesFilterDocument());
	}
	if (has(Type::Audio)) {
		result.push_back(MTP_inputMessagesFilterMusic());
	}
	if (has(Type::Link)) {
		result.push_back(MTP_inputMessagesFilterUrl());
	}
	if (has(Type::Poll)) {
		result.push_back(MTP_inputMessagesFilterPoll());
	}
	return result;
}

Settings::Type SettingsFromDialogsType(Data::DialogInfo::Type type) {
	using DialogType = Data::DialogInfo::Type;
	switch (type) {
	case DialogType::Self:
	case DialogType::Personal:
		return Settings::Type::PersonalChats;
	case DialogType::Bot:
		return Settings::Type::BotChats;
	case DialogType::PrivateGroup:
	case DialogType::PrivateSupergroup:
		return Settings::Type::PrivateGroups;
	case DialogType::PublicSupergroup:
		return Settings::Type::PublicGroups;
	case DialogType::PrivateChannel:
		return Settings::Type::PrivateChannels;
	case DialogType::PublicChannel:
		return Settings::Type::PublicChannels;
	}
	return Settings::Type(0);
}

[[nodiscard]] uint64 LocationFileId(const Data::FileLocation &location) {
	return location.data.match(
		[](const MTPDinputDocumentFileLocation &data) {
			return uint64(data.vid().v);
		},
		[](const MTPDinputPhotoFileLocation &data) {
			return uint64(data.vid().v);
		},
		[](const MTPDinputPhotoLegacyFileLocation &data) {
			return uint64(data.vid().v);
		},
		[](const MTPDinputPeerPhotoFileLocation &data) {
			return uint64(data.vphoto_id().v);
		},
		[](const MTPDinputEncryptedFileLocation &data) {
			return uint64(data.vid().v);
		},
		[](const MTPDinputSecureFileLocation &data) {
			return uint64(data.vid().v);
		},
		[](const auto &) { return uint64(0); });
}

MediaSettings::Type DocumentMediaType(const Data::Document &document) {
	using Type = MediaSettings::Type;
	if (document.isSticker) {
		return Type::Sticker;
	} else if (document.isVideoMessage) {
		return Type::VideoMessage;
	} else if (document.isVoiceMessage) {
		return Type::VoiceMessage;
	} else if (document.isAnimated) {
		return Type::GIF;
	} else if (document.isVideoFile) {
		return Type::Video;
	} else if (document.isAudioFile) {
		return Type::Audio;
	}
	return Type::File;
}

MediaSettings::Type OrdinaryMediaType(const Data::Media &media) {
	return v::match(media.content, [](const Data::Document &document) {
		return DocumentMediaType(document);
	}, [](const Data::WebPage &) {
		return MediaSettings::Type::Link;
	}, [](const auto &) {
		return MediaSettings::Type::Photo;
	});
}

std::optional<MTPRichMessage> ExtractFullRichMessage(
		const MTPmessages_Messages &result,
		int32 messageId) {
	auto matches = 0;
	auto richMessage = std::optional<MTPRichMessage>();
	result.match([&](const MTPDmessages_messagesNotModified &) {
	}, [&](const auto &data) {
		for (const auto &entry : data.vmessages().v) {
			entry.match([&](const auto &message) {
				if (message.vid().v != messageId) {
					return;
				}
				++matches;
				if constexpr (MTPDmessage::Is<decltype(message)>()) {
					const auto candidate = message.vrich_message();
					if (candidate && !candidate->data().is_part()) {
						richMessage = *candidate;
					}
				}
			});
		}
	});
	if (matches != 1) {
		return std::nullopt;
	}
	return richMessage;
}

std::optional<Data::FileLocation> RefreshRichMessageFileReference(
		const Data::FileLocation &location,
		const Data::RichMessage &message) {
	auto refreshed = location;
	for (const auto &entry : message.photos) {
		if (Data::RefreshFileReference(
				refreshed,
				entry.second.image.file.location)) {
			return refreshed;
		}
	}
	for (const auto &entry : message.documents) {
		const auto &document = entry.second;
		if (Data::RefreshFileReference(
				refreshed,
				document.file.location)) {
			return refreshed;
		} else if (document.thumb.width > 0
			&& Data::RefreshFileReference(
				refreshed,
				document.thumb.file.location)) {
			return refreshed;
		}
	}
	return std::nullopt;
}

template <
	typename TextCallback,
	typename PhotoCallback,
	typename DocumentCallback>
struct RichMessageCallbacks {
	TextCallback &text;
	PhotoCallback &photo;
	DocumentCallback &document;
};

template <typename RichMessage, typename Callbacks>
void VisitRichPhoto(
		RichMessage &message,
		uint64 id,
		Callbacks &callbacks) {
	const auto i = message.photos.find(id);
	if (i != end(message.photos)) {
		callbacks.photo(i->second);
	}
}

template <typename RichMessage, typename Callbacks>
void VisitRichDocument(
		RichMessage &message,
		uint64 id,
		Callbacks &callbacks) {
	const auto i = message.documents.find(id);
	if (i != end(message.documents)) {
		callbacks.document(i->second);
	}
}

template <typename RichMessage, typename Text, typename Callbacks>
void VisitRichText(
		RichMessage &message,
		Text &text,
		Callbacks &callbacks) {
	callbacks.text(text);
	if (text.type == Data::RichText::Type::InlineImage) {
		VisitRichDocument(message, text.id, callbacks);
	}
	for (auto &child : text.children) {
		VisitRichText(message, child, callbacks);
	}
	for (auto &child : text.oldChildren) {
		VisitRichText(message, child, callbacks);
	}
}

template <typename RichMessage, typename Caption, typename Callbacks>
void VisitRichCaption(
		RichMessage &message,
		Caption &caption,
		Callbacks &callbacks) {
	VisitRichText(message, caption.text, callbacks);
	VisitRichText(message, caption.credit, callbacks);
}

template <typename RichMessage, typename Block, typename Callbacks>
void VisitRichBlock(
		RichMessage &message,
		Block &block,
		Callbacks &callbacks);

template <typename RichMessage, typename Blocks, typename Callbacks>
void VisitRichBlocks(
		RichMessage &message,
		Blocks &blocks,
		Callbacks &callbacks) {
	for (auto &block : blocks) {
		VisitRichBlock(message, block, callbacks);
	}
}

template <typename RichMessage, typename Block, typename Callbacks>
void VisitRichBlock(
		RichMessage &message,
		Block &block,
		Callbacks &callbacks) {
	using Kind = Data::RichBlock::Kind;
	using ListContent = Data::RichListItemContent;
	using QuoteContent = Data::RichQuoteContent;
	switch (block.kind) {
	case Kind::Heading:
	case Kind::Paragraph:
	case Kind::Footer:
	case Kind::Thinking:
	case Kind::AuthorDate:
	case Kind::Code:
		VisitRichText(message, block.text, callbacks);
		break;
	case Kind::List:
		for (auto &item : block.listItems) {
			switch (item.content) {
			case ListContent::Text:
				if (item.text) {
					VisitRichText(message, *item.text, callbacks);
				}
				break;
			case ListContent::Blocks:
				VisitRichBlocks(message, item.blocks, callbacks);
				break;
			}
		}
		break;
	case Kind::Quote:
		switch (block.quoteContent) {
		case QuoteContent::Text:
			VisitRichText(message, block.text, callbacks);
			break;
		case QuoteContent::Blocks:
			VisitRichBlocks(message, block.blocks, callbacks);
			break;
		}
		VisitRichText(message, block.quoteCaption, callbacks);
		break;
	case Kind::Photo:
		VisitRichPhoto(message, block.photoId, callbacks);
		VisitRichCaption(message, block.caption, callbacks);
		break;
	case Kind::Video:
	case Kind::Audio:
	case Kind::File:
		VisitRichDocument(message, block.documentId, callbacks);
		VisitRichCaption(message, block.caption, callbacks);
		break;
	case Kind::Cover:
		Expects(block.blocks.size() == 1);
		VisitRichBlock(message, block.blocks.front(), callbacks);
		break;
	case Kind::Embed:
		if (block.posterPhotoId) {
			VisitRichPhoto(message, *block.posterPhotoId, callbacks);
		}
		VisitRichCaption(message, block.caption, callbacks);
		break;
	case Kind::EmbedPost:
		VisitRichPhoto(message, block.authorPhotoId, callbacks);
		VisitRichBlocks(message, block.blocks, callbacks);
		VisitRichCaption(message, block.caption, callbacks);
		break;
	case Kind::Collage:
	case Kind::Slideshow:
		VisitRichBlocks(message, block.blocks, callbacks);
		VisitRichCaption(message, block.caption, callbacks);
		break;
	case Kind::Table:
		VisitRichText(message, block.text, callbacks);
		for (auto &row : block.tableRows) {
			for (auto &cell : row.cells) {
				if (cell.text) {
					VisitRichText(message, *cell.text, callbacks);
				}
			}
		}
		break;
	case Kind::Details:
		VisitRichText(message, block.text, callbacks);
		VisitRichBlocks(message, block.blocks, callbacks);
		break;
	case Kind::RelatedArticles:
		VisitRichText(message, block.text, callbacks);
		for (auto &article : block.relatedArticles) {
			if (article.photoId) {
				VisitRichPhoto(message, *article.photoId, callbacks);
			}
		}
		break;
	case Kind::Map:
	case Kind::InputMap:
		VisitRichCaption(message, block.caption, callbacks);
		break;
	case Kind::ButtonRow:
		for (auto &button : block.buttons) {
			VisitRichText(message, button, callbacks);
		}
		break;
	case Kind::Unsupported:
	case Kind::Divider:
	case Kind::Anchor:
	case Kind::Channel:
	case Kind::Math:
	case Kind::Unknown:
		break;
	}
}

template <
	typename RichMessage,
	typename TextCallback,
	typename PhotoCallback,
	typename DocumentCallback>
void VisitRichMessageImpl(
		RichMessage &message,
		TextCallback &textCallback,
		PhotoCallback &photoCallback,
		DocumentCallback &documentCallback) {
	auto callbacks = RichMessageCallbacks<
		TextCallback,
		PhotoCallback,
		DocumentCallback>{
		textCallback,
		photoCallback,
		documentCallback,
	};
	VisitRichBlocks(message, message.blocks, callbacks);
}

template <
	typename TextCallback,
	typename PhotoCallback,
	typename DocumentCallback>
void VisitRichMessage(
		const Data::RichMessage &message,
		TextCallback &&textCallback,
		PhotoCallback &&photoCallback,
		DocumentCallback &&documentCallback) {
	VisitRichMessageImpl(
		message,
		textCallback,
		photoCallback,
		documentCallback);
}

template <
	typename TextCallback,
	typename PhotoCallback,
	typename DocumentCallback>
void VisitRichMessage(
		Data::RichMessage &message,
		TextCallback &&textCallback,
		PhotoCallback &&photoCallback,
		DocumentCallback &&documentCallback) {
	VisitRichMessageImpl(
		message,
		textCallback,
		photoCallback,
		documentCallback);
}

} // namespace

struct ApiWrap::StartProcess {
	FnMut<void(StartInfo)> done;

	enum class Step {
		UserpicsCount,
		StoriesCount,
		ProfileMusicCount,
		SplitRanges,
		DialogsCount,
		LeftChannelsCount,
	};
	std::deque<Step> steps;
	int splitIndex = 0;
	StartInfo info;
};

struct ApiWrap::ContactsProcess {
	FnMut<void(Data::ContactsList&&)> done;

	Data::ContactsList result;

	int topPeersOffset = 0;
};

struct ApiWrap::UserpicsProcess {
	FnMut<bool(Data::UserpicsInfo&&)> start;
	Fn<bool(DownloadProgress)> fileProgress;
	Fn<bool(Data::UserpicsSlice&&)> handleSlice;
	FnMut<void()> finish;

	int processed = 0;
	std::optional<Data::UserpicsSlice> slice;
	uint64 maxId = 0;
	bool lastSlice = false;
	int fileIndex = 0;
};

struct ApiWrap::StoriesProcess {
	FnMut<bool(Data::StoriesInfo&&)> start;
	Fn<bool(DownloadProgress)> fileProgress;
	Fn<bool(Data::StoriesSlice&&)> handleSlice;
	FnMut<void()> finish;

	int processed = 0;
	std::optional<Data::StoriesSlice> slice;
	int offsetId = 0;
	bool lastSlice = false;
	int fileIndex = 0;
};

struct ApiWrap::ProfileMusicProcess {
	FnMut<bool(Data::ProfileMusicInfo&&)> start;
	Fn<bool(DownloadProgress)> fileProgress;
	Fn<bool(Data::ProfileMusicSlice&&)> handleSlice;
	FnMut<void()> finish;

	int processed = 0;
	std::optional<Data::ProfileMusicSlice> slice;
	int offsetId = 0;
	bool lastSlice = false;
	int fileIndex = 0;
};

struct ApiWrap::OtherDataProcess {
	Data::File file;
	FnMut<void(Data::File&&)> done;
};

struct ApiWrap::FileProcess {
	FileProcess(const QString &path, Output::Stats *stats);

	Output::File file;
	QString relativePath;

	Fn<bool(FileProgress)> progress;
	FnMut<void(const QString &relativePath)> done;

	uint64 randomId = 0;
	uint64 docId = 0;
	Data::FileLocation location;
	Data::FileOrigin origin;
	int64 offset = 0;
	int64 size = 0;

	struct Request {
		int64 offset = 0;
		QByteArray bytes;
	};
	std::deque<Request> requests;
	std::vector<std::pair<int64, mtpRequestId>> requestIds;
};

struct ApiWrap::FileProgress {
	int64 ready = 0;
	int64 total = 0;
};

struct ApiWrap::FilePolicy {
	const Data::Message *message = nullptr;
	MediaSettings::Type type = MediaSettings::Type();
	int64 controllingSize = 0;
	bool mainFile = false;
};

struct ApiWrap::MessageFileWork {
	Data::File *file = nullptr;
	int64 controllingSize = 0;
	MediaSettings::Type type = MediaSettings::Type();
	bool rich : 1 = false;
	bool main : 1 = false;
};

struct ApiWrap::ChatsProcess {
	Fn<bool(int count)> progress;
	FnMut<void(Data::DialogsInfo&&)> done;

	Data::DialogsInfo info;
	int processedCount = 0;
	std::map<PeerId, int> indexByPeer;
};

struct ApiWrap::LeftChannelsProcess : ChatsProcess {
	int fullCount = 0;
	int offset = 0;
	bool finished = false;
};

struct ApiWrap::DialogsProcess : ChatsProcess {
	int splitIndexPlusOne = 0;
	TimeId offsetDate = 0;
	int32 offsetId = 0;
	MTPInputPeer offsetPeer = MTP_inputPeerEmpty();
};

struct ApiWrap::AbstractMessagesProcess {
	Fn<bool(DownloadProgress)> fileProgress;
	Fn<bool(Data::MessagesSlice&&)> handleSlice;
	FnMut<void()> done;

	FnMut<void(MTPmessages_Messages&&)> requestDone;

	Data::ParseMediaContext context;
	std::optional<Data::MessagesSlice> slice;
	std::vector<MessageFileWork> messageFileWork;
	bool lastSlice = false;
	int hydrationIndex = 0;
	int hydrationPending = 0;
	int fileIndex = 0;
	int messageFileWorkIndex = 0;
	int messageFileWorkMessageIndex = -1;

	// Selected-message ordinal: counts messages that carry wanted
	// content (downloaded/linked files, wanted text/links/polls),
	// not walked messages. Used for progress only in export
	// file-filtered mode; scan keeps walked ordinals.
	int selectedDone = 0;
	bool messageContentSelected = false;
};

struct ApiWrap::ChatProcess : AbstractMessagesProcess {
	Data::DialogInfo info;

	FnMut<bool(const Data::DialogInfo &)> start;

	int localSplitIndex = 0;
	int32 fromId = 0; // First message id of the id range (0 = from start).
	int32 tillId = 0; // Last message id of the id range (0 = until now).

	// Single-pass oldest-first walk: cursor climbs by maximum raw
	// page id, output skips repeats, one counter climbs from zero.
	bool walkStarted = false;
	int32 walkCursor = 1;
	int32 writtenMax = 0;
	int32 committedMax = 0;

	// Next-page prefetch: fired while the current page's hash batch
	// drains, consumed in walk order. At most one outstanding list
	// request ever; emit order stays strictly oldest-first.
	std::optional<MTPmessages_Messages> pagePrefetch;
	bool pagePrefetchInflight = false;
	bool pagePrefetchWaited = false;
	int pagePrefetchSplit = -1;
	int pagePrefetchFilter = -1;

	std::vector<MTPMessagesFilter> scanFilters;
	std::vector<std::vector<int>> scanCounts;
	int scanFilterIndex = -1;
	int scanCountFilter = 0;
	int scanCountSplit = 0;
	int scanCountPending = 0;
	bool scanBySearch = false;
	crl::time scanNextAllowedAt = 0;

	// Exact selected-message total from filter probes (export
	// file-filtered mode, text excluded: text has no probe).
	// The walk still uses history counts for split skipping.
	bool hasSelectedTotal = false;
	int selectedTotal = 0;
};

struct ApiWrap::TopicProcess : AbstractMessagesProcess {
	PeerId peerId = 0;
	MTPInputPeer inputPeer;
	int32 topicRootId = 0;
	QString relativePath;

	FnMut<bool(int count)> start;

	bool walkStarted = false;
	int32 walkCursor = 1;
	int32 writtenMax = 0;
	int totalCount = 0;
	int processedCount = 0;

};


template <typename Request>
class ApiWrap::RequestBuilder {
public:
	using Original = MTP::ConcurrentSender::SpecificRequestBuilder<Request>;
	using Response = typename Request::ResponseType;

	RequestBuilder(
		Original &&builder,
		Fn<void(const MTP::Error&)> commonFailHandler);

	[[nodiscard]] RequestBuilder &done(FnMut<void()> &&handler);
	[[nodiscard]] RequestBuilder &done(
		FnMut<void(Response &&)> &&handler);
	[[nodiscard]] RequestBuilder &handleFloodErrors() noexcept;
	[[nodiscard]] RequestBuilder &fail(
		Fn<bool(const MTP::Error&)> &&handler);

	mtpRequestId send();

private:
	Original _builder;
	Fn<void(const MTP::Error&)> _commonFailHandler;

};

template <typename Request>
ApiWrap::RequestBuilder<Request>::RequestBuilder(
	Original &&builder,
	Fn<void(const MTP::Error&)> commonFailHandler)
: _builder(std::move(builder))
, _commonFailHandler(std::move(commonFailHandler)) {
}

template <typename Request>
auto ApiWrap::RequestBuilder<Request>::done(
	FnMut<void()> &&handler
) -> RequestBuilder& {
	if (handler) {
		[[maybe_unused]] auto &silence_warning = _builder.done(std::move(handler));
	}
	return *this;
}

template <typename Request>
auto ApiWrap::RequestBuilder<Request>::done(
	FnMut<void(Response &&)> &&handler
) -> RequestBuilder& {
	if (handler) {
		[[maybe_unused]] auto &silence_warning = _builder.done(std::move(handler));
	}
	return *this;
}

template <typename Request>
ApiWrap::RequestBuilder<Request> &ApiWrap::RequestBuilder<Request>::handleFloodErrors() noexcept {
	[[maybe_unused]] auto &silence_warning = _builder.handleFloodErrors();
	return *this;
}

template <typename Request>
auto ApiWrap::RequestBuilder<Request>::fail(
	Fn<bool(const MTP::Error &)> &&handler
) -> RequestBuilder& {
	if (handler) {
		[[maybe_unused]] auto &silence_warning = _builder.fail([
			common = base::take(_commonFailHandler),
			specific = std::move(handler)
		](const MTP::Error &error) {
			if (!specific(error)) {
				common(error);
			}
		});
	}
	return *this;
}

template <typename Request>
mtpRequestId ApiWrap::RequestBuilder<Request>::send() {
	return _commonFailHandler
		? _builder.fail(base::take(_commonFailHandler)).send()
		: _builder.send();
}

ApiWrap::FileProcess::FileProcess(const QString &path, Output::Stats *stats)
: file(path, stats) {
}

template <typename Request>
auto ApiWrap::mainRequest(Request &&request) {
	Expects(_takeoutId.has_value());

	auto original = std::move(_mtp.request(MTPInvokeWithTakeout<Request>(
		MTP_long(*_takeoutId),
		std::forward<Request>(request)
	)).toDC(MTP::ShiftDcId(0, MTP::kExportDcShift)));

	return RequestBuilder<MTPInvokeWithTakeout<Request>>(
		std::move(original),
		[=](const MTP::Error &result) { error(result); });
}

template <typename Request>
auto ApiWrap::splitRequest(int index, Request &&request) {
	Expects(index < _splits.size());

	//if (index == _splits.size() - 1) {
	//	return mainRequest(std::forward<Request>(request));
	//}
	return mainRequest(MTPInvokeWithMessagesRange<Request>(
		_splits[index],
		std::forward<Request>(request)));
}

auto ApiWrap::fileRequest(const Data::FileLocation &location, int64 offset) {
	Expects(location.dcId != 0
		|| location.data.type() == mtpc_inputTakeoutFileLocation);
	Expects(_takeoutId.has_value());
	Expects(_fileProcess->requestIds.size() < kFileRequestsCount);

	return std::move(_mtp.request(MTPInvokeWithTakeout<MTPupload_GetFile>(
		MTP_long(*_takeoutId),
		MTPupload_GetFile(
			MTP_flags(0),
			location.data,
			MTP_long(offset),
			MTP_int(kFileChunkSize))
	)).fail([=](const MTP::Error &result) {
		if (result.type() == u"OFFSET_INVALID"_q
			&& offset > 0
			&& _fileProcess
			&& filePartChunkFailed(offset)) {
			return;
		} else if (result.type() == u"TAKEOUT_FILE_EMPTY"_q
			&& _otherDataProcess != nullptr) {
			filePartDone(
				0,
				MTP_upload_file(
					MTP_storage_filePartial(),
					MTP_int(0),
					MTP_bytes()));
		} else if (result.type() == u"LOCATION_INVALID"_q
			|| result.type() == u"VERSION_INVALID"_q
			|| result.type() == u"LOCATION_NOT_AVAILABLE"_q) {
			filePartUnavailable();
		} else if (result.code() == 400
			&& result.type().startsWith(u"FILE_REFERENCE_"_q)) {
			// Serialize the refresh: drop sibling chunks, rewind,
			// then continue single-flight through the fresh reference.
			for (const auto &entry : base::take(_fileProcess->requestIds)) {
				_mtp.request(entry.second).cancel();
			}
			_fileProcess->requests.clear();
			_fileProcess->offset = offset;
			filePartRefreshReference(offset);
		} else if (result.type() == u"TAKEOUT_INVALID"_q
			&& _fileProcess) {
			// Dead takeout session (e.g. another client export opened
			// one): first failure rewinds and refreshes, siblings
			// landing mid-refresh are covered by the rewind.
			if (_takeoutInvalidPending) {
				return;
			}
			_takeoutInvalidPending = true;
			const auto gen = _dedupGen;
			const auto original = result;
			auto minOffset = offset;
			for (const auto &entry : _fileProcess->requestIds) {
				minOffset = std::min(minOffset, entry.first);
			}
			for (const auto &entry : base::take(_fileProcess->requestIds)) {
				_mtp.request(entry.second).cancel();
			}
			_fileProcess->requests.clear();
			_fileProcess->offset = minOffset;
			refreshTakeoutSession([=](uint64 fresh) {
				_takeoutInvalidPending = false;
				if (gen != _dedupGen || !_fileProcess) {
					return;
				}
				if (!fresh) {
					error(original);
					return;
				}
				loadFilePart();
			});
		} else {
			error(std::move(result));
		}
	}).toDC(MTP::ShiftDcId(location.dcId, MTP::kExportMediaDcShift)));
}

ApiWrap::ApiWrap(
	base::weak_qptr<MTP::Instance> weak,
	Fn<void(FnMut<void()>)> runner)
: _runner(runner)
, _mtp(weak, std::move(runner)) {
}

rpl::producer<MTP::Error> ApiWrap::errors() const {
	return _errors.events();
}

rpl::producer<Output::Result> ApiWrap::ioErrors() const {
	return _ioErrors.events();
}

void ApiWrap::startExport(
		const Settings &settings,
		Output::Stats *stats,
		FnMut<void(StartInfo)> done) {
	Expects(_settings == nullptr);
	Expects(_startProcess == nullptr);

	_settings = std::make_unique<Settings>(settings);
	_stats = stats;
	_startProcess = std::make_unique<StartProcess>();
	_startProcess->done = std::move(done);

	using Step = StartProcess::Step;
	if (_settings->types & Settings::Type::Userpics) {
		_startProcess->steps.push_back(Step::UserpicsCount);
	}
	if (_settings->types & Settings::Type::Stories) {
		_startProcess->steps.push_back(Step::StoriesCount);
	}
	if (_settings->types & Settings::Type::ProfileMusic) {
		_startProcess->steps.push_back(Step::ProfileMusicCount);
	}
	if (_settings->types & Settings::Type::AnyChatsMask) {
		_startProcess->steps.push_back(Step::SplitRanges);
		_startProcess->steps.push_back(Step::DialogsCount);
	}
	if (_settings->types & Settings::Type::GroupsChannelsMask) {
		if (!_settings->onlySinglePeer()) {
			_startProcess->steps.push_back(Step::LeftChannelsCount);
		}
	}
	startMainSession([=] {
		sendNextStartRequest();
	});
}

void ApiWrap::sendNextStartRequest() {
	Expects(_startProcess != nullptr);

	auto &steps = _startProcess->steps;
	if (steps.empty()) {
		finishStartProcess();
		return;
	}
	using Step = StartProcess::Step;
	const auto step = steps.front();
	steps.pop_front();
	switch (step) {
	case Step::UserpicsCount:
		return requestUserpicsCount();
	case Step::StoriesCount:
		return requestStoriesCount();
	case Step::ProfileMusicCount:
		return requestProfileMusicCount();
	case Step::SplitRanges:
		return requestSplitRanges();
	case Step::DialogsCount:
		return requestDialogsCount();
	case Step::LeftChannelsCount:
		return requestLeftChannelsCount();
	}
	Unexpected("Step in ApiWrap::sendNextStartRequest.");
}

void ApiWrap::requestUserpicsCount() {
	Expects(_startProcess != nullptr);

	mainRequest(MTPphotos_GetUserPhotos(
		_user,
		MTP_int(0),  // offset
		MTP_long(0), // max_id
		MTP_int(0)   // limit
	)).done([=](const MTPphotos_Photos &result) {
		Expects(_settings != nullptr);
		Expects(_startProcess != nullptr);

		_startProcess->info.userpicsCount = result.match(
		[](const MTPDphotos_photos &data) {
			return int(data.vphotos().v.size());
		}, [](const MTPDphotos_photosSlice &data) {
			return data.vcount().v;
		});

		sendNextStartRequest();
	}).send();
}

void ApiWrap::requestStoriesCount() {
	Expects(_startProcess != nullptr);

	mainRequest(MTPstories_GetStoriesArchive(
		MTP_inputPeerSelf(),
		MTP_int(0), // offset_id
		MTP_int(0) // limit
	)).done([=](const MTPstories_Stories &result) {
		Expects(_settings != nullptr);
		Expects(_startProcess != nullptr);

		_startProcess->info.storiesCount = result.data().vcount().v;

		sendNextStartRequest();
	}).send();
}

void ApiWrap::requestProfileMusicCount() {
	Expects(_startProcess != nullptr);

	mainRequest(MTPusers_GetSavedMusic(
		_user,
		MTP_int(0), // offset
		MTP_int(0), // limit
		MTP_long(0) // hash
	)).done([=](const MTPusers_SavedMusic &result) {
		Expects(_settings != nullptr);
		Expects(_startProcess != nullptr);

		const auto count = result.match(
		[](const MTPDusers_savedMusic &data) {
			return data.vcount().v;
		}, [](const MTPDusers_savedMusicNotModified &data) {
			return -1;
		});
		if (count < 0) {
			error("Unexpected messagesNotModified received.");
			return;
		}
		_startProcess->info.profileMusicCount = count;

		sendNextStartRequest();
	}).send();
}

void ApiWrap::requestSplitRanges() {
	Expects(_startProcess != nullptr);

	mainRequest(MTPmessages_GetSplitRanges(
	)).done([=](const MTPVector<MTPMessageRange> &result) {
		_splits = result.v;
		if (_splits.empty()) {
			_splits.push_back(MTP_messageRange(
				MTP_int(1),
				MTP_int(std::numeric_limits<int>::max())));
		}
		_startProcess->splitIndex = useOnlyLastSplit()
			? (_splits.size() - 1)
			: 0;

		sendNextStartRequest();
	}).send();
}

void ApiWrap::requestDialogsCount() {
	Expects(_startProcess != nullptr);

	if (_settings->onlySinglePeer()) {
		_startProcess->info.dialogsCount
			= (_settings->singlePeer.type() == mtpc_inputPeerChannel
				? 1
				: _splits.size());
		sendNextStartRequest();
		return;
	}

	const auto offsetDate = 0;
	const auto offsetId = 0;
	const auto offsetPeer = MTP_inputPeerEmpty();
	const auto limit = 1;
	const auto hash = uint64(0);
	splitRequest(_startProcess->splitIndex, MTPmessages_GetDialogs(
		MTP_flags(0),
		MTPint(), // folder_id
		MTP_int(offsetDate),
		MTP_int(offsetId),
		offsetPeer,
		MTP_int(limit),
		MTP_long(hash)
	)).done([=](const MTPmessages_Dialogs &result) {
		Expects(_settings != nullptr);
		Expects(_startProcess != nullptr);

		const auto count = result.match(
		[](const MTPDmessages_dialogs &data) {
			return int(data.vdialogs().v.size());
		}, [](const MTPDmessages_dialogsSlice &data) {
			return data.vcount().v;
		}, [](const MTPDmessages_dialogsNotModified &data) {
			return -1;
		});
		if (count < 0) {
			error("Unexpected dialogsNotModified received.");
			return;
		}
		_startProcess->info.dialogsCount += count;

		if (++_startProcess->splitIndex >= _splits.size()) {
			sendNextStartRequest();
		} else {
			requestDialogsCount();
		}
	}).send();
}

void ApiWrap::requestLeftChannelsCount() {
	Expects(_startProcess != nullptr);
	Expects(_leftChannelsProcess == nullptr);

	_leftChannelsProcess = std::make_unique<LeftChannelsProcess>();
	requestLeftChannelsSliceGeneric([=] {
		Expects(_startProcess != nullptr);
		Expects(_leftChannelsProcess != nullptr);

		_startProcess->info.dialogsCount
			+= _leftChannelsProcess->fullCount;
		sendNextStartRequest();
	});
}

void ApiWrap::finishStartProcess() {
	Expects(_startProcess != nullptr);

	const auto process = base::take(_startProcess);
	process->done(process->info);
}

bool ApiWrap::useOnlyLastSplit() const {
	return !(_settings->types & Settings::Type::NonChannelChatsMask);
}

void ApiWrap::requestLeftChannelsList(
		Fn<bool(int count)> progress,
		FnMut<void(Data::DialogsInfo&&)> done) {
	Expects(_leftChannelsProcess != nullptr);

	_leftChannelsProcess->progress = std::move(progress);
	_leftChannelsProcess->done = std::move(done);
	requestLeftChannelsSlice();
}

void ApiWrap::requestLeftChannelsSlice() {
	requestLeftChannelsSliceGeneric([=] {
		Expects(_leftChannelsProcess != nullptr);

		if (_leftChannelsProcess->finished) {
			const auto process = base::take(_leftChannelsProcess);
			process->done(std::move(process->info));
		} else {
			requestLeftChannelsSlice();
		}
	});
}

void ApiWrap::requestDialogsList(
		Fn<bool(int count)> progress,
		FnMut<void(Data::DialogsInfo&&)> done) {
	Expects(_dialogsProcess == nullptr);

	_dialogsProcess = std::make_unique<DialogsProcess>();
	_dialogsProcess->splitIndexPlusOne = _splits.size();
	_dialogsProcess->progress = std::move(progress);
	_dialogsProcess->done = std::move(done);

	requestDialogsSlice();
}

void ApiWrap::startMainSession(FnMut<void()> done) {
	// The takeout session is the shared session one, bound before
	// the start: export never inits or finishes its own (two live
	// sessions invalidate each other server-side).
	_mtp.request(MTPusers_GetUsers(
		MTP_vector<MTPInputUser>(1, MTP_inputUserSelf())
	)).done([=, done = std::move(done)](
			const MTPVector<MTPUser> &result) mutable {
		for (const auto &user : result.v) {
			user.match([&](const MTPDuser &data) {
				if (data.is_self()) {
					_selfId.emplace(data.vid());
				}
			}, [&](const MTPDuserEmpty&) {
			});
		}
		if (!_selfId) {
			error("Could not retrieve selfId.");
			return;
		}
		if (!_takeoutId) {
			error("Takeout session unavailable.");
			return;
		}
		done();
	}).fail([=](const MTP::Error &result) {
		error(result);
	}).send();
}

void ApiWrap::setSharedTakeoutId(uint64 id) {
	_takeoutId = id
		? std::optional<uint64>(id)
		: std::nullopt;
}

void ApiWrap::setTakeoutRefreshHook(Fn<void()> hook) {
	_takeoutRefreshHook = std::move(hook);
}

void ApiWrap::requestPersonalInfo(FnMut<void(Data::PersonalInfo&&)> done) {
	mainRequest(MTPusers_GetFullUser(
		_user
	)).done([=, done = std::move(done)](const MTPusers_UserFull &result) mutable {
		result.match([&](const MTPDusers_userFull &data) {
			if (!data.vusers().v.empty()) {
				done(Data::ParsePersonalInfo(data));
			} else {
				error("Bad user type.");
			}
		});
	}).send();
}

void ApiWrap::requestOtherData(
		const QString &suggestedPath,
		FnMut<void(Data::File&&)> done) {
	Expects(_otherDataProcess == nullptr);

	_otherDataProcess = std::make_unique<OtherDataProcess>();
	_otherDataProcess->done = std::move(done);
	_otherDataProcess->file.location.data = MTP_inputTakeoutFileLocation();
	_otherDataProcess->file.suggestedPath = suggestedPath;
	_dedupGen++;
	loadFile(
		_otherDataProcess->file,
		Data::FileOrigin(),
		[](FileProgress progress) { return true; },
		[=](const QString &result) { otherDataDone(result); });
}

void ApiWrap::otherDataDone(const QString &relativePath) {
	Expects(_otherDataProcess != nullptr);

	const auto process = base::take(_otherDataProcess);
	process->file.relativePath = relativePath;
	if (relativePath.isEmpty()) {
		process->file.skipReason = Data::File::SkipReason::Unavailable;
	}
	process->done(std::move(process->file));
}

void ApiWrap::requestUserpics(
		FnMut<bool(Data::UserpicsInfo&&)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::UserpicsSlice&&)> slice,
		FnMut<void()> finish) {
	Expects(_userpicsProcess == nullptr);

	_userpicsProcess = std::make_unique<UserpicsProcess>();
	_userpicsProcess->start = std::move(start);
	_userpicsProcess->fileProgress = std::move(progress);
	_userpicsProcess->handleSlice = std::move(slice);
	_userpicsProcess->finish = std::move(finish);
	_dedupGen++;

	mainRequest(MTPphotos_GetUserPhotos(
		_user,
		MTP_int(0), // offset
		MTP_long(_userpicsProcess->maxId),
		MTP_int(kUserpicsSliceLimit)
	)).done([=](const MTPphotos_Photos &result) mutable {
		Expects(_userpicsProcess != nullptr);

		auto startInfo = result.match(
		[](const MTPDphotos_photos &data) {
			return Data::UserpicsInfo{ int(data.vphotos().v.size()) };
		}, [](const MTPDphotos_photosSlice &data) {
			return Data::UserpicsInfo{ data.vcount().v };
		});
		if (!_userpicsProcess->start(std::move(startInfo))) {
			return;
		}

		handleUserpicsSlice(result);
	}).send();
}

void ApiWrap::handleUserpicsSlice(const MTPphotos_Photos &result) {
	Expects(_userpicsProcess != nullptr);

	result.match([&](const auto &data) {
		if constexpr (MTPDphotos_photos::Is<decltype(data)>()) {
			_userpicsProcess->lastSlice = true;
		}
		loadUserpicsFiles(Data::ParseUserpicsSlice(
			data.vphotos(),
			_userpicsProcess->processed));
	});
}

void ApiWrap::loadUserpicsFiles(Data::UserpicsSlice &&slice) {
	Expects(_userpicsProcess != nullptr);
	Expects(!_userpicsProcess->slice.has_value());

	if (slice.list.empty()) {
		_userpicsProcess->lastSlice = true;
	}
	++_sliceGen;
	_userpicsProcess->slice = std::move(slice);
	_userpicsProcess->fileIndex = 0;
	loadNextUserpic();
}

void ApiWrap::loadNextUserpic() {
	Expects(_userpicsProcess != nullptr);
	Expects(_userpicsProcess->slice.has_value());

	for (auto &list = _userpicsProcess->slice->list
		; _userpicsProcess->fileIndex < list.size()
		; ++_userpicsProcess->fileIndex) {
		const auto ready = processFileLoad(
			list[_userpicsProcess->fileIndex].image.file,
			Data::FileOrigin(),
			[=](FileProgress value) { return loadUserpicProgress(value); },
			[=](const QString &path) { loadUserpicDone(path); });
		if (!ready) {
			return;
		}
		noteMediaWritten(list[_userpicsProcess->fileIndex].image.file);
	}
	finishUserpicsSlice();
}

void ApiWrap::finishUserpicsSlice() {
	Expects(_userpicsProcess != nullptr);
	Expects(_userpicsProcess->slice.has_value());

	auto slice = *base::take(_userpicsProcess->slice);
	if (!slice.list.empty()) {
		_userpicsProcess->processed += slice.list.size();
		_userpicsProcess->maxId = slice.list.back().id;
		if (!_userpicsProcess->handleSlice(std::move(slice))) {
			return;
		}
	}
	if (_userpicsProcess->lastSlice) {
		finishUserpics();
		return;
	}

	mainRequest(MTPphotos_GetUserPhotos(
		_user,
		MTP_int(0), // offset
		MTP_long(_userpicsProcess->maxId),
		MTP_int(kUserpicsSliceLimit)
	)).done([=](const MTPphotos_Photos &result) {
		handleUserpicsSlice(result);
	}).send();
}

bool ApiWrap::loadUserpicProgress(FileProgress progress) {
	Expects(_fileProcess != nullptr);
	Expects(_userpicsProcess != nullptr);
	Expects(_userpicsProcess->slice.has_value());
	Expects((_userpicsProcess->fileIndex >= 0)
		&& (_userpicsProcess->fileIndex
			< _userpicsProcess->slice->list.size()));

	return _userpicsProcess->fileProgress(DownloadProgress{
		_fileProcess->randomId,
		_fileProcess->relativePath,
		_userpicsProcess->fileIndex,
		progress.ready,
		progress.total });
}

void ApiWrap::loadUserpicDone(const QString &relativePath) {
	Expects(_userpicsProcess != nullptr);
	Expects(_userpicsProcess->slice.has_value());
	Expects((_userpicsProcess->fileIndex >= 0)
		&& (_userpicsProcess->fileIndex
			< _userpicsProcess->slice->list.size()));

	const auto index = _userpicsProcess->fileIndex;
	auto &file = _userpicsProcess->slice->list[index].image.file;
	file.relativePath = relativePath;
	if (relativePath.isEmpty()) {
		file.skipReason = Data::File::SkipReason::Unavailable;
	} else {
		noteMediaWritten(file);
	}
	loadNextUserpic();
}

void ApiWrap::finishUserpics() {
	Expects(_userpicsProcess != nullptr);

	base::take(_userpicsProcess)->finish();
}

void ApiWrap::requestStories(
		FnMut<bool(Data::StoriesInfo&&)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::StoriesSlice&&)> slice,
		FnMut<void()> finish) {
	Expects(_storiesProcess == nullptr);

	_storiesProcess = std::make_unique<StoriesProcess>();
	_storiesProcess->start = std::move(start);
	_storiesProcess->fileProgress = std::move(progress);
	_storiesProcess->handleSlice = std::move(slice);
	_storiesProcess->finish = std::move(finish);
	_dedupGen++;

	mainRequest(MTPstories_GetStoriesArchive(
		MTP_inputPeerSelf(),
		MTP_int(_storiesProcess->offsetId),
		MTP_int(kStoriesSliceLimit)
	)).done([=](const MTPstories_Stories &result) mutable {
		Expects(_storiesProcess != nullptr);

		auto startInfo = Data::StoriesInfo{ result.data().vcount().v };
		if (!_storiesProcess->start(std::move(startInfo))) {
			return;
		}

		handleStoriesSlice(result);
	}).send();
}

void ApiWrap::handleStoriesSlice(const MTPstories_Stories &result) {
	Expects(_storiesProcess != nullptr);

	loadStoriesFiles(Data::ParseStoriesSlice(
		result.data().vstories(),
		_storiesProcess->processed));
}

void ApiWrap::loadStoriesFiles(Data::StoriesSlice &&slice) {
	Expects(_storiesProcess != nullptr);
	Expects(!_storiesProcess->slice.has_value());

	if (!slice.lastId) {
		_storiesProcess->lastSlice = true;
	}
	++_sliceGen;
	_storiesProcess->slice = std::move(slice);
	_storiesProcess->fileIndex = 0;
	loadNextStory();
}

void ApiWrap::loadNextStory() {
	Expects(_storiesProcess != nullptr);
	Expects(_storiesProcess->slice.has_value());

	for (auto &list = _storiesProcess->slice->list
		; _storiesProcess->fileIndex < list.size()
		; ++_storiesProcess->fileIndex) {
		auto &story = list[_storiesProcess->fileIndex];
		const auto origin = Data::FileOrigin{ .storyId = story.id };
		const auto ready = processFileLoad(
			story.file(),
			origin,
			[=](FileProgress value) { return loadStoryProgress(value); },
			[=](const QString &path) { loadStoryDone(path); });
		if (!ready) {
			return;
		}
		noteMediaWritten(story.file());
		const auto thumbProgress = [=](FileProgress value) {
			return loadStoryThumbProgress(value);
		};
		const auto thumbReady = processFileLoad(
			story.thumb().file,
			origin,
			thumbProgress,
			[=](const QString &path) { loadStoryThumbDone(path); },
			nullptr,
			&story);
		if (!thumbReady) {
			return;
		}
		noteMediaWritten(story.thumb().file);
	}
	finishStoriesSlice();
}

void ApiWrap::finishStoriesSlice() {
	Expects(_storiesProcess != nullptr);
	Expects(_storiesProcess->slice.has_value());

	auto slice = *base::take(_storiesProcess->slice);
	if (slice.lastId) {
		_storiesProcess->processed += slice.list.size();
		_storiesProcess->offsetId = slice.lastId;
		if (!_storiesProcess->handleSlice(std::move(slice))) {
			return;
		}
	}
	if (_storiesProcess->lastSlice) {
		finishStories();
		return;
	}

	mainRequest(MTPstories_GetStoriesArchive(
		MTP_inputPeerSelf(),
		MTP_int(_storiesProcess->offsetId),
		MTP_int(kStoriesSliceLimit)
	)).done([=](const MTPstories_Stories &result) {
		handleStoriesSlice(result);
	}).send();
}

bool ApiWrap::loadStoryProgress(FileProgress progress) {
	Expects(_fileProcess != nullptr);
	Expects(_storiesProcess != nullptr);
	Expects(_storiesProcess->slice.has_value());
	Expects((_storiesProcess->fileIndex >= 0)
		&& (_storiesProcess->fileIndex
			< _storiesProcess->slice->list.size()));

	return _storiesProcess->fileProgress(DownloadProgress{
		_fileProcess->randomId,
		_fileProcess->relativePath,
		_storiesProcess->fileIndex,
		progress.ready,
		progress.total });
}

void ApiWrap::loadStoryDone(const QString &relativePath) {
	Expects(_storiesProcess != nullptr);
	Expects(_storiesProcess->slice.has_value());
	Expects((_storiesProcess->fileIndex >= 0)
		&& (_storiesProcess->fileIndex
			< _storiesProcess->slice->list.size()));

	const auto index = _storiesProcess->fileIndex;
	auto &file = _storiesProcess->slice->list[index].file();
	file.relativePath = relativePath;
	if (relativePath.isEmpty()) {
		file.skipReason = Data::File::SkipReason::Unavailable;
	} else {
		noteMediaWritten(file);
	}
	loadNextStory();
}

bool ApiWrap::loadStoryThumbProgress(FileProgress progress) {
	return loadStoryProgress(progress);
}

void ApiWrap::loadStoryThumbDone(const QString &relativePath) {
	Expects(_storiesProcess != nullptr);
	Expects(_storiesProcess->slice.has_value());
	Expects((_storiesProcess->fileIndex >= 0)
		&& (_storiesProcess->fileIndex
			< _storiesProcess->slice->list.size()));

	const auto index = _storiesProcess->fileIndex;
	auto &file = _storiesProcess->slice->list[index].thumb().file;
	file.relativePath = relativePath;
	if (relativePath.isEmpty()) {
		file.skipReason = Data::File::SkipReason::Unavailable;
	} else {
		noteMediaWritten(file);
	}
	loadNextStory();
}

void ApiWrap::finishStories() {
	Expects(_storiesProcess != nullptr);

	base::take(_storiesProcess)->finish();
}

void ApiWrap::requestProfileMusic(
		FnMut<bool(Data::ProfileMusicInfo&&)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::ProfileMusicSlice&&)> slice,
		FnMut<void()> finish) {
	Expects(_profileMusicProcess == nullptr);

	_profileMusicProcess = std::make_unique<ProfileMusicProcess>();
	_profileMusicProcess->start = std::move(start);
	_profileMusicProcess->fileProgress = std::move(progress);
	_profileMusicProcess->handleSlice = std::move(slice);
	_profileMusicProcess->finish = std::move(finish);
	_dedupGen++;

	mainRequest(MTPusers_GetSavedMusic(
		_user,
		MTP_int(0), // offset
		MTP_int(kProfileMusicSliceLimit), // limit
		MTP_long(0) // hash
	)).done([=](const MTPusers_SavedMusic &result) mutable {
		Expects(_profileMusicProcess != nullptr);

		auto startInfo = result.match(
		[](const MTPDusers_savedMusic &data) {
			return Data::ProfileMusicInfo{ data.vcount().v };
		}, [](const MTPDusers_savedMusicNotModified &data) {
			return Data::ProfileMusicInfo{ 0 };
		});
		if (!_profileMusicProcess->start(std::move(startInfo))) {
			return;
		}

		handleProfileMusicSlice(result);
	}).send();
}

void ApiWrap::handleProfileMusicSlice(const MTPusers_SavedMusic &result) {
	Expects(_profileMusicProcess != nullptr);
	Expects(_selfId.has_value());

	auto context = Data::ParseMediaContext();
	context.selfPeerId = peerFromUser(*_selfId);

	auto slice = result.match([&](const MTPDusers_savedMusic &data) {
		if (data.vdocuments().v.size() < kProfileMusicSliceLimit) {
			_profileMusicProcess->lastSlice = true;
		}
		auto result = Data::MessagesSlice();
		for (const auto &doc : data.vdocuments().v) {
			auto message = Data::Message();
			message.id = ++_profileMusicProcess->processed;
			message.date = 0;
			message.media.content = Data::ParseDocument(
				context,
				doc,
				"profile_music/",
				0);
			result.list.push_back(std::move(message));
		}
		return result;
	}, [&](const MTPDusers_savedMusicNotModified &) {
		_profileMusicProcess->lastSlice = true;
		return Data::MessagesSlice();
	});

	auto profileSlice = Data::ProfileMusicSlice();
	profileSlice.list.reserve(slice.list.size());
	for (auto &message : slice.list) {
		if (v::is<Data::Document>(message.media.content)) {
			const auto &doc = v::get<Data::Document>(message.media.content);
			if (doc.isAudioFile) {
				profileSlice.list.push_back(std::move(message));
			}
		}
	}

	loadProfileMusicFiles(std::move(profileSlice));
}

void ApiWrap::loadProfileMusicFiles(Data::ProfileMusicSlice &&slice) {
	Expects(_profileMusicProcess != nullptr);
	Expects(!_profileMusicProcess->slice.has_value());

	if (slice.list.empty()) {
		_profileMusicProcess->lastSlice = true;
	}
	++_sliceGen;
	_profileMusicProcess->slice = std::move(slice);
	_profileMusicProcess->fileIndex = 0;
	loadNextProfileMusic();
}

void ApiWrap::loadNextProfileMusic() {
	Expects(_profileMusicProcess != nullptr);
	Expects(_profileMusicProcess->slice.has_value());

	for (auto &list = _profileMusicProcess->slice->list
		; _profileMusicProcess->fileIndex < list.size()
		; ++_profileMusicProcess->fileIndex) {
		auto &message = list[_profileMusicProcess->fileIndex];
		const auto origin = Data::FileOrigin{ .messageId = message.id };
		const auto ready = processFileLoad(
			message.file(),
			origin,
			[=](FileProgress value) { return loadProfileMusicProgress(value); },
			[=](const QString &path) { loadProfileMusicDone(path); },
			&message);
		if (!ready) {
			return;
		}
		noteMediaWritten(message.file());
		const auto thumbProgress = [=](FileProgress value) {
			return loadProfileMusicThumbProgress(value);
		};
		const auto thumbReady = processFileLoad(
			message.thumb().file,
			origin,
			thumbProgress,
			[=](const QString &path) { loadProfileMusicThumbDone(path); },
			&message);
		if (!thumbReady) {
			return;
		}
		noteMediaWritten(message.thumb().file);
	}
	finishProfileMusicSlice();
}

void ApiWrap::finishProfileMusicSlice() {
	Expects(_profileMusicProcess != nullptr);
	Expects(_profileMusicProcess->slice.has_value());

	auto slice = *base::take(_profileMusicProcess->slice);
	if (!slice.list.empty()) {
		_profileMusicProcess->processed += slice.list.size();
		_profileMusicProcess->offsetId = slice.list.back().id;
		if (!_profileMusicProcess->handleSlice(std::move(slice))) {
			return;
		}
	}
	if (_profileMusicProcess->lastSlice) {
		finishProfileMusic();
		return;
	}

	mainRequest(MTPusers_GetSavedMusic(
		_user,
		MTP_int(_profileMusicProcess->offsetId),
		MTP_int(kProfileMusicSliceLimit),
		MTP_long(0)
	)).done([=](const MTPusers_SavedMusic &result) {
		handleProfileMusicSlice(result);
	}).send();
}

bool ApiWrap::loadProfileMusicProgress(FileProgress progress) {
	Expects(_fileProcess != nullptr);
	Expects(_profileMusicProcess != nullptr);
	Expects(_profileMusicProcess->slice.has_value());
	Expects((_profileMusicProcess->fileIndex >= 0)
		&& (_profileMusicProcess->fileIndex
			< _profileMusicProcess->slice->list.size()));

	return _profileMusicProcess->fileProgress(DownloadProgress{
		_fileProcess->randomId,
		_fileProcess->relativePath,
		_profileMusicProcess->fileIndex,
		progress.ready,
		progress.total });
}

void ApiWrap::loadProfileMusicDone(const QString &relativePath) {
	Expects(_profileMusicProcess != nullptr);
	Expects(_profileMusicProcess->slice.has_value());
	Expects((_profileMusicProcess->fileIndex >= 0)
		&& (_profileMusicProcess->fileIndex
			< _profileMusicProcess->slice->list.size()));

	const auto index = _profileMusicProcess->fileIndex;
	auto &file = _profileMusicProcess->slice->list[index].file();
	file.relativePath = relativePath;
	finishFileRecord(&file);
	if (relativePath.isEmpty()) {
		file.skipReason = Data::File::SkipReason::Unavailable;
	}
	loadNextProfileMusic();
}

bool ApiWrap::loadProfileMusicThumbProgress(FileProgress progress) {
	return loadProfileMusicProgress(progress);
}

void ApiWrap::loadProfileMusicThumbDone(const QString &relativePath) {
	Expects(_profileMusicProcess != nullptr);
	Expects(_profileMusicProcess->slice.has_value());
	Expects((_profileMusicProcess->fileIndex >= 0)
		&& (_profileMusicProcess->fileIndex
			< _profileMusicProcess->slice->list.size()));

	const auto index = _profileMusicProcess->fileIndex;
	auto &file = _profileMusicProcess->slice->list[index].thumb().file;
	file.relativePath = relativePath;
	finishFileRecord(&file);
	if (relativePath.isEmpty()) {
		file.skipReason = Data::File::SkipReason::Unavailable;
	}
	loadNextProfileMusic();
}

void ApiWrap::finishProfileMusic() {
	Expects(_profileMusicProcess != nullptr);

	base::take(_profileMusicProcess)->finish();
}

void ApiWrap::requestContacts(FnMut<void(Data::ContactsList&&)> done) {
	Expects(_contactsProcess == nullptr);

	_contactsProcess = std::make_unique<ContactsProcess>();
	_contactsProcess->done = std::move(done);
	mainRequest(MTPcontacts_GetSaved(
	)).done([=](const MTPVector<MTPSavedContact> &result) {
		_contactsProcess->result = Data::ParseContactsList(result);

		const auto resolve = [=](int index, const auto &resolveNext) -> void {
			if (index == _contactsProcess->result.list.size()) {
				return requestTopPeersSlice();
			}
			const auto &contact = _contactsProcess->result.list[index];
			mainRequest(MTPcontacts_ResolvePhone(
				MTP_string(qs(contact.phoneNumber))
			)).done([=](const MTPcontacts_ResolvedPeer &result) {
				auto &contact = _contactsProcess->result.list[index];
				contact.userId = result.data().vpeer().match([&](
						const MTPDpeerUser &user) {
					return UserId(user.vuser_id());
				}, [](const auto &) {
					return UserId();
				});
				resolveNext(index + 1, resolveNext);
			}).fail([=](const MTP::Error &) {
				resolveNext(index + 1, resolveNext);
				return true;
			}).send();
		};

		if (base::options::lookup<bool>("show-peer-id-below-about").value()) {
			resolve(0, resolve);
		} else {
			requestTopPeersSlice();
		}

	}).send();
}

void ApiWrap::requestTopPeersSlice() {
	Expects(_contactsProcess != nullptr);

	using Flag = MTPcontacts_GetTopPeers::Flag;
	mainRequest(MTPcontacts_GetTopPeers(
		MTP_flags(Flag::f_correspondents
			| Flag::f_bots_inline
			| Flag::f_phone_calls),
		MTP_int(_contactsProcess->topPeersOffset),
		MTP_int(kTopPeerSliceLimit),
		MTP_long(0) // hash
	)).done([=](const MTPcontacts_TopPeers &result) {
		Expects(_contactsProcess != nullptr);

		if (!Data::AppendTopPeers(_contactsProcess->result, result)) {
			error("Unexpected data in ApiWrap::requestTopPeersSlice.");
			return;
		}

		const auto offset = _contactsProcess->topPeersOffset;
		const auto loaded = result.match(
		[](const MTPDcontacts_topPeersNotModified &data) {
			return true;
		}, [](const MTPDcontacts_topPeersDisabled &data) {
			return true;
		}, [&](const MTPDcontacts_topPeers &data) {
			for (const auto &category : data.vcategories().v) {
				const auto loaded = category.match(
				[&](const MTPDtopPeerCategoryPeers &data) {
					return offset + data.vpeers().v.size() >= data.vcount().v;
				});
				if (!loaded) {
					return false;
				}
			}
			return true;
		});

		if (loaded) {
			auto process = base::take(_contactsProcess);
			process->done(std::move(process->result));
		} else {
			_contactsProcess->topPeersOffset = std::max(std::max(
				_contactsProcess->result.correspondents.size(),
				_contactsProcess->result.inlineBots.size()),
				_contactsProcess->result.phoneCalls.size());
			requestTopPeersSlice();
		}
	}).send();
}

void ApiWrap::requestSessions(FnMut<void(Data::SessionsList&&)> done) {
	mainRequest(MTPaccount_GetAuthorizations(
	)).done([=, done = std::move(done)](
			const MTPaccount_Authorizations &result) mutable {
		auto list = Data::ParseSessionsList(result);
		mainRequest(MTPaccount_GetWebAuthorizations(
		)).done([=, done = std::move(done), list = std::move(list)](
				const MTPaccount_WebAuthorizations &result) mutable {
			list.webList = Data::ParseWebSessionsList(result).webList;
			done(std::move(list));
		}).send();
	}).send();
}

void ApiWrap::requestMessages(
		const Data::DialogInfo &info,
		FnMut<bool(const Data::DialogInfo &)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::MessagesSlice&&)> slice,
		FnMut<void()> done) {
	Expects(_chatProcess == nullptr);
	Expects(_selfId.has_value());

	_chatProcess = std::make_unique<ChatProcess>();
	_pauseRequested = false;
	_paused = false;
	_walkIdFloor = 0;
	_chatProcess->selectedDone = base::take(_resumeSelectedDone);
	_chatProcess->context.selfPeerId = peerFromUser(*_selfId);
	_chatProcess->info = info;
	_chatProcess->start = std::move(start);
	_chatProcess->fileProgress = std::move(progress);
	_chatProcess->handleSlice = std::move(slice);
	_chatProcess->done = std::move(done);

	// Kill stale async callbacks from previous chats: their file
	// references die with their slices while this process is alive.
	_dedupGen++;
	if (_scanMode) {
		_chatProcess->scanFilters = ScanSearchFilters(
			_settings->media.types);
	}
	// Seen ids are keyed by split plus message id: without a reset
	// per chat, a later chat would skip its own messages as repeats
	// of an earlier chat's. Within a chat the set persists across
	// sequential filter walks, deduping their overlap.
	_scanSeenMessages.clear();
	resolveDates();
}

void ApiWrap::resolveDates() {
	Expects(_chatProcess != nullptr);

	// Id bounds come from the id-range setting; dates additionally
	// resolve to id floors below. The walk trims the rest per
	// message client-side; walk pages carry no date bounds (server
	// date bounds fragment pagination with the count intact).
	if (_settings->useIdRange) {
		const auto fromId = _settings->singlePeerFromId.value_or(0);
		const auto tillId = _settings->singlePeerTillId.value_or(0);
		_chatProcess->fromId = (fromId > uint64(INT32_MAX))
			? INT32_MAX
			: int32(fromId);
		_chatProcess->tillId = (tillId > uint64(INT32_MAX))
			? INT32_MAX
			: int32(tillId);
	} else {
		_chatProcess->fromId = 0;
		_chatProcess->tillId = 0;
	}

	// Without a date range there is nothing to resolve in either
	// mode: date bounds resolve to id floors below, and the walk
	// trims the rest client-side.
	if (!_settings->singlePeerFrom && !_settings->singlePeerTill) {
		if (!_scanMode) {
			_chatProcess->localSplitIndex = 0;
		}
		requestMessagesCount(0);
		return;
	}
	if (!_scanMode) {
		_chatProcess->localSplitIndex = 0;
	}

	const auto resolveTill = [=] {
		if (!_chatProcess) {
			return;
		}
		if (!_settings->singlePeerTill) {
			requestMessagesCount(0);
			return;
		}
		const auto peer = _chatProcess->info.input;
		mainRequest(MTPmessages_GetHistory(
			peer,
			MTP_int(0),                           // offset_id
			MTP_int(*_settings->singlePeerTill),   // offset_date
			MTP_int(0),                           // add_offset
			MTP_int(1),                           // limit
			MTP_int(0),                           // max_id
			MTP_int(0),                           // min_id
			MTP_long(0)                           // hash
		)).done([=](const MTPmessages_Messages &result) {
			if (!_chatProcess) {
				return;
			}
			result.match([&](const MTPDmessages_messagesNotModified &data) {
			}, [&](const auto &data) {
				if (!data.vmessages().v.isEmpty()) {
					_chatProcess->tillId = data.vmessages().v[0].match(
						[](const auto &m) { return int32(m.vid().v); });
				}
			});
			requestMessagesCount(0);
		}).fail([=](const MTP::Error &error) {
			requestMessagesCount(0);
			return true;
		}).send();
	};

	if (!_settings->singlePeerFrom) {
		resolveTill();
		return;
	}
	const auto peer = _chatProcess->info.input;
	mainRequest(MTPmessages_GetHistory(
		peer,
		MTP_int(0),                             // offset_id
		MTP_int(*_settings->singlePeerFrom),     // offset_date
		MTP_int(0),                             // add_offset
		MTP_int(1),                             // limit
		MTP_int(0),                             // max_id
		MTP_int(0),                             // min_id
		MTP_long(0)                             // hash
	)).done([=](const MTPmessages_Messages &result) {
		if (!_chatProcess) {
			return;
		}
		result.match([&](const MTPDmessages_messagesNotModified &data) {
		}, [&](const auto &data) {
			if (!data.vmessages().v.isEmpty()) {
				const auto msg = data.vmessages().v[0];
				const auto id = msg.match([](const auto &m) {
					return int32(m.vid().v);
				});
				const auto date = msg.match([](const MTPDmessageEmpty &data) {
					return TimeId(0);
				}, [](const auto &m) {
					return TimeId(m.vdate().v);
				});
				// Exclude the boundary message only when it predates the range.
				_chatProcess->fromId = (date > 0 && date < *_settings->singlePeerFrom)
					? (id + 1)
					: id;
			}
		});
		resolveTill();
	}).fail([=](const MTP::Error &error) {
		resolveTill();
		return true;
	}).send();
}

void ApiWrap::requestMessagesCount(int localSplitIndex) {
	Expects(_chatProcess != nullptr);
	Expects(localSplitIndex < _chatProcess->info.splits.size());

	requestChatMessages(
		_chatProcess->info.splits[localSplitIndex],
		0, // offset_id
		0, // add_offset
		1, // limit
		[=](const MTPmessages_Messages &result) {
		Expects(_chatProcess != nullptr);

		const auto count = result.match(
			[](const MTPDmessages_messages &data) {
			return int(data.vmessages().v.size());
		}, [](const MTPDmessages_messagesSlice &data) {
			return data.vcount().v;
		}, [](const MTPDmessages_channelMessages &data) {
			return data.vcount().v;
		}, [](const MTPDmessages_messagesNotModified &data) {
			return -1;
		});
		if (count < 0) {
			error("Unexpected messagesNotModified received.");
			return;
		}
		const auto skipSplit = !Data::SingleMessageAfter(
			result,
			_settings->singlePeerFrom.value_or(0));
		if (skipSplit) {
			// No messages from the requested range, skip this split.
			messagesCountLoaded(localSplitIndex, 0);
			return;
		}
		checkFirstMessageDate(localSplitIndex, count);
	});
}

void ApiWrap::checkFirstMessageDate(int localSplitIndex, int count) {
	Expects(_chatProcess != nullptr);
	Expects(localSplitIndex < _chatProcess->info.splits.size());

	if (!_settings->singlePeerTill) {
		messagesCountLoaded(localSplitIndex, count);
		return;
	}

	// Request first message in this split to check if its' date < till.
	requestChatMessages(
		_chatProcess->info.splits[localSplitIndex],
		1, // offset_id
		-1, // add_offset
		1, // limit
		[=](const MTPmessages_Messages &result) {
		Expects(_chatProcess != nullptr);

		const auto skipSplit = !Data::SingleMessageBefore(
			result,
			_settings->singlePeerTill.value_or(0));
		messagesCountLoaded(localSplitIndex, skipSplit ? 0 : count);
	});
}

void ApiWrap::messagesCountLoaded(int localSplitIndex, int count) {
	Expects(_chatProcess != nullptr);
	Expects(localSplitIndex < _chatProcess->info.splits.size());

	_chatProcess->info.messagesCountPerSplit[localSplitIndex] = count;
	if (localSplitIndex + 1 < _chatProcess->info.splits.size()) {
		requestMessagesCount(localSplitIndex + 1);
	} else if (_updateMode) {
		_updateMode = false;
		auto total = 0;
		for (const auto splitCount :
				_chatProcess->info.messagesCountPerSplit) {
			total += splitCount;
		}
		if (_updateCheckHandler) {
			base::take(_updateCheckHandler)(
				total - _updateKnownTotal);
		}
		return;
	} else if (_scanMode
		&& !_chatProcess->scanFilters.empty()
		&& _chatProcess->scanCounts.empty()
		&& !_chatProcess->info.onlyMyMessages) {
		requestScanCount();
	} else if (!_scanMode
		&& _chatProcess->scanCounts.empty()
		&& !_chatProcess->info.onlyMyMessages
		&& startExportFilterCounts()) {
		return;
	} else if (_chatProcess->start(_chatProcess->info)) {
		requestMessagesSlice();
	}
}

// Fully indexed file-filtered exports (no text, no stickers:
// neither has a usable server index) ask the server for each
// filter's totals first: the sums are the exact selected-message
// denominator. Single indexed kinds walk search, several walk
// sequential per-filter search. Anything else walks history with
// walked/full-messages counters. Probes carry the date/id range
// so the totals match the filtered walk.
// Returns true when probes were fired (walk starts at join).
bool ApiWrap::startExportFilterCounts() {
	Expects(_chatProcess != nullptr);

	using Type = MediaSettings::Type;
	const auto types = _settings->media.types;
	// Text and stickers have no usable server index: selections
	// containing either walk history with walked/full-messages
	// counters. Server counts plus downloaded-selected counters
	// apply only when every selected kind is indexed.
	if (((types & Type::Text) == Type::Text)
		|| ((types & Type::Sticker) == Type::Sticker)
		|| skipMedia()) {
		return false;
	}
	const auto filters = ScanSearchFilters(types);
	if (filters.empty()) {
		return false;
	}
	const auto filterCount = int(filters.size());
	const auto splits = int(_chatProcess->info.splits.size());
	_chatProcess->scanFilters = filters;
	_chatProcess->scanCounts.assign(
		filterCount,
		std::vector<int>(splits, 0));
	_chatProcess->scanCountPending = filterCount * splits;
	if (!_chatProcess->scanCountPending) {
		return false;
	}
	// Fan out staggered like the scan probes: same method family
	// as the paced walk pages, completion order irrelevant.
	const auto gen = _dedupGen;
	const auto timer = std::make_shared<base::ConcurrentTimer>(_runner);
	const auto next = std::make_shared<int>(0);
	const auto total = _chatProcess->scanCountPending;
	const auto fire = std::make_shared<Fn<void()>>();
	*fire = [=, this] {
		if (!_chatProcess || gen != _dedupGen) {
			return;
		}
		if (*next >= total) {
			return;
		}
		const auto slot = (*next)++;
		fireExportCountSlot(slot / splits, slot % splits);
		if (*next < total) {
			const auto weakFire = std::weak_ptr<Fn<void()>>(fire);
			timer->setCallback([=] {
				if (gen == _dedupGen) {
					if (const auto strong = weakFire.lock()) {
						(*strong)();
					}
				}
			});
			timer->callOnce(kProbeStartSpacing);
		}
	};
	(*fire)();
	return true;
}

void ApiWrap::fireExportCountSlot(int filterIndex, int splitPosition) {
	Expects(_chatProcess != nullptr);

	const auto &process = *_chatProcess;
	Expects(filterIndex < int(process.scanFilters.size()));
	Expects(splitPosition < int(process.info.splits.size()));

	const auto splitsCount = int(_splits.size());
	const auto splitIndex = process.info.splits[splitPosition];
	const auto realPeerInput = (splitIndex >= 0)
		? process.info.input
		: process.info.migratedFromInput;
	const auto realSplitIndex = (splitIndex >= 0)
		? splitIndex
		: (splitsCount + splitIndex);
	const auto minId = (process.fromId > 0)
		? int32(process.fromId - 1)
		: int32(0);
	const auto maxId = (process.tillId > 0)
		? int32(process.tillId + 1)
		: int32(0);
	const auto from = _settings->singlePeerFrom
		? MTP_int(*_settings->singlePeerFrom)
		: MTP_int(0);
	const auto till = _settings->singlePeerTill
		? MTP_int(*_settings->singlePeerTill)
		: MTP_int(0);
	const auto gen = _dedupGen;
	const auto filter = process.scanFilters[filterIndex];
	splitRequest(realSplitIndex, MTPmessages_Search(
		MTP_flags(0),
		realPeerInput,
		MTP_string(),
		MTPInputPeer(),
		MTPInputPeer(),
		MTPVector<MTPReaction>(),
		MTPint(),
		filter,
		from,
		till,
		MTP_int(0),
		MTP_int(0),
		MTP_int(1),
		MTP_int(maxId),
		MTP_int(minId),
		MTP_long(0)
	)).done([=](const MTPmessages_Messages &result) {
		if (gen != _dedupGen || !_chatProcess) {
			return;
		}
		_chatProcess->scanCounts[filterIndex][splitPosition]
			= result.match(
			[](const MTPDmessages_messages &data) {
				return int(data.vmessages().v.size());
			}, [](const MTPDmessages_messagesSlice &data) {
				return data.vcount().v;
			}, [](const MTPDmessages_channelMessages &data) {
				return data.vcount().v;
			}, [](const MTPDmessages_messagesNotModified &data) {
				return 0;
			});
		if (--_chatProcess->scanCountPending > 0) {
			return;
		}
		decideExportSearch();
	}).fail([=](const MTP::Error &error) {
		if (gen != _dedupGen || !_chatProcess) {
			return true;
		}
		_chatProcess->scanCounts[filterIndex][splitPosition] = 0;
		if (--_chatProcess->scanCountPending > 0) {
			return true;
		}
		decideExportSearch();
		return true;
	}).send();
}

void ApiWrap::decideExportSearch() {
	Expects(_chatProcess != nullptr);

	const auto searchTotal = SearchTotalSkippingUnions(
		_chatProcess->scanFilters,
		_chatProcess->scanCounts);
	// The selected sums always feed the progress denominator. The
	// walk itself: a single indexed category always walks search
	// (serial pages, no flood risk), several indexed categories
	// walk search filter after filter. Measured: the server
	// excludes stickers from Document results (sticker-only search
	// completes 0/N while history finds them), so sticker
	// selections stay on history. Text and media-free likewise.
	// Scan (no media) keeps its own sparse rule in
	// decideScanMethod.
	_chatProcess->hasSelectedTotal = true;
	_chatProcess->selectedTotal = searchTotal;
	using Type = MediaSettings::Type;
	const auto sticker = ((_settings->media.types & Type::Sticker)
		== Type::Sticker);
	if (!sticker) {
		_chatProcess->scanBySearch = true;
		_chatProcess->scanFilterIndex = 0;
		for (auto i = 0; i != _chatProcess->info.splits.size(); ++i) {
			_chatProcess->info.messagesCountPerSplit[i]
				= _chatProcess->scanCounts[0][i];
		}
	}
	if (_chatProcess->start(_chatProcess->info)) {
		requestMessagesSlice();
	}
}

bool ApiWrap::hasSelectedTotal() const {
	return _chatProcess && _chatProcess->hasSelectedTotal;
}

int ApiWrap::selectedTotal() const {
	return _chatProcess ? _chatProcess->selectedTotal : 0;
}

int ApiWrap::chatSelectedDone() const {
	return _chatProcess ? _chatProcess->selectedDone : 0;
}

void ApiWrap::requestScanCount() {
	Expects(_chatProcess != nullptr);
	Expects(!_chatProcess->scanFilters.empty());

	// Count probes fan out over the whole filter-by-split grid at
	// once: every probe is a different filter shape writing its own
	// indexed slot, so completion order is irrelevant. Starts stay
	// staggered (same method family as the paced walk pages).
	const auto filters = int(_chatProcess->scanFilters.size());
	const auto splits = int(_chatProcess->info.splits.size());
	_chatProcess->scanCounts.assign(
		filters,
		std::vector<int>(splits, 0));
	_chatProcess->scanCountPending = filters * splits;
	if (!_chatProcess->scanCountPending) {
		decideScanMethod();
		return;
	}
	const auto gen = _dedupGen;
	const auto timer = std::make_shared<base::ConcurrentTimer>(_runner);
	const auto next = std::make_shared<int>(0);
	const auto total = _chatProcess->scanCountPending;
	const auto fire = std::make_shared<Fn<void()>>();
	*fire = [=, this] {
		if (!_chatProcess || gen != _dedupGen) {
			return;
		}
		if (*next >= total) {
			return;
		}
		const auto slot = (*next)++;
		fireScanCountSlot(slot / splits, slot % splits);
		if (*next < total) {
			const auto weakFire = std::weak_ptr<Fn<void()>>(fire);
			timer->setCallback([=] {
				if (gen == _dedupGen) {
					if (const auto strong = weakFire.lock()) {
						(*strong)();
					}
				}
			});
			timer->callOnce(kProbeStartSpacing);
		}
	};
	(*fire)();
}

void ApiWrap::fireScanCountSlot(int filterIndex, int splitPosition) {
	Expects(_chatProcess != nullptr);

	const auto &process = *_chatProcess;
	Expects(filterIndex < int(process.scanFilters.size()));
	Expects(splitPosition < int(process.info.splits.size()));

	const auto splitsCount = int(_splits.size());
	const auto splitIndex = process.info.splits[splitPosition];
	const auto realPeerInput = (splitIndex >= 0)
		? process.info.input
		: process.info.migratedFromInput;
	const auto realSplitIndex = (splitIndex >= 0)
		? splitIndex
		: (splitsCount + splitIndex);
	const auto minId = (process.fromId > 0)
		? int32(process.fromId - 1)
		: int32(0);
	const auto maxId = (process.tillId > 0)
		? int32(process.tillId + 1)
		: int32(0);
	const auto from = _settings->singlePeerFrom
		? MTP_int(*_settings->singlePeerFrom)
		: MTP_int(0);
	const auto till = _settings->singlePeerTill
		? MTP_int(*_settings->singlePeerTill)
		: MTP_int(0);
	const auto gen = _dedupGen;
	const auto noteDone = [=, this](int count) {
		if (gen != _dedupGen || !_chatProcess) {
			return;
		}
		_chatProcess->scanCounts[filterIndex][splitPosition] = count;
		if (--_chatProcess->scanCountPending <= 0) {
			decideScanMethod();
		}
	};
	splitRequest(realSplitIndex, MTPmessages_Search(
		MTP_flags(0),
		realPeerInput,
		MTP_string(),
		MTPInputPeer(),
		MTPInputPeer(),
		MTPVector<MTPReaction>(),
		MTPint(),
		process.scanFilters[filterIndex],
		from,
		till,
		MTP_int(0),
		MTP_int(0),
		MTP_int(1),
		MTP_int(maxId),
		MTP_int(minId),
		MTP_long(0)
	)).done([=](const MTPmessages_Messages &result) {
		const auto count = result.match(
			[](const MTPDmessages_messages &data) {
				return int(data.vmessages().v.size());
		}, [](const MTPDmessages_messagesSlice &data) {
				return data.vcount().v;
		}, [](const MTPDmessages_channelMessages &data) {
				return data.vcount().v;
		}, [](const MTPDmessages_messagesNotModified &data) {
				return 0;
			});
		noteDone(count);
	}).fail([=](const MTP::Error &error) {
		noteDone(0);
		return true;
	}).send();
}

void ApiWrap::decideScanMethod() {
	Expects(_chatProcess != nullptr);
	Expects(!_chatProcess->scanFilters.empty());

	auto historyTotal = 0;
	for (const auto count : _chatProcess->info.messagesCountPerSplit) {
		historyTotal += count;
	}
	const auto searchTotal = SearchTotalSkippingUnions(
		_chatProcess->scanFilters,
		_chatProcess->scanCounts);
	// Measured: the server excludes stickers from Document
	// results, so sticker selections walk history with all
	// messages as the denominator, like export.
	using Type = MediaSettings::Type;
	const auto sticker = ((_settings->media.types & Type::Sticker)
		== Type::Sticker);
	const auto useSearch = !sticker
		&& historyTotal > 0
		&& searchTotal * 2 <= historyTotal;
	LOG(("ExportDiag: TEMP scan history=%1 search=%2 useSearch=%3 "
		"splits=%4 filters=%5").arg(historyTotal).arg(searchTotal)
		.arg(useSearch).arg(int(_chatProcess->info.splits.size()))
		.arg(int(_chatProcess->scanFilters.size())));
	if (useSearch) {
		_chatProcess->scanBySearch = true;
		_chatProcess->scanFilterIndex = 0;
		const auto hasVoice = ranges::any_of(
			_chatProcess->scanFilters,
			[](const auto &filter) {
				return filter.type() == mtpc_inputMessagesFilterVoice;
			});
		for (auto i = 0; i != _chatProcess->info.splits.size(); ++i) {
			auto total = 0;
			for (auto f = 0;
				f != int(_chatProcess->scanFilters.size());
				++f) {
				const auto type = _chatProcess->scanFilters[f].type();
				if (hasVoice
					&& type == mtpc_inputMessagesFilterRoundVoice) {
					continue;
				}
				total += _chatProcess->scanCounts[f][i];
			}
			_chatProcess->info.messagesCountPerSplit[i] = total;
		}
		if (!_chatProcess->start(_chatProcess->info)) {
			return;
		}
		for (auto i = 0; i != _chatProcess->info.splits.size(); ++i) {
			_chatProcess->info.messagesCountPerSplit[i]
				= _chatProcess->scanCounts[0][i];
		}
		requestMessagesSlice();
		return;
	}
	if (_chatProcess->start(_chatProcess->info)) {
		requestMessagesSlice();
	}
}

bool ApiWrap::scanAdvanceFilter() {
	Expects(_chatProcess != nullptr);

	if (!_chatProcess->scanBySearch) {
		return false;
	}
	// RoundVoice results are a subset of the Voice walk: visiting
	// the filter re-fetches already-seen messages (id-deduped, finds
	// nothing) but its raw count lands in the finished denominator
	// (34 voice + 78 round = 112). Skip it when Voice is present.
	const auto hasVoice = ranges::any_of(
		_chatProcess->scanFilters,
		[](const auto &filter) {
			return filter.type() == mtpc_inputMessagesFilterVoice;
		});
	auto next = _chatProcess->scanFilterIndex + 1;
	while (next < int(_chatProcess->scanFilters.size())
		&& hasVoice
		&& _chatProcess->scanFilters[next].type()
			== mtpc_inputMessagesFilterRoundVoice) {
		++next;
	}
	if (next >= int(_chatProcess->scanFilters.size())) {
		return false;
	}
	_chatProcess->scanFilterIndex = next;
	_chatProcess->localSplitIndex = 0;
	_chatProcess->walkStarted = false;
	// Fresh runs restart at 1; updates keep their id floor so only
	// new messages are picked up in later filters too.
	_chatProcess->walkCursor = std::max(int32(1), _walkIdFloor);
	_chatProcess->writtenMax = std::max(int32(0), _walkIdFloor - 1);
	_chatProcess->committedMax = 0;
	_chatProcess->pagePrefetch.reset();
	_chatProcess->lastSlice = false;
	for (auto i = 0; i != _chatProcess->info.splits.size(); ++i) {
		_chatProcess->info.messagesCountPerSplit[i]
			= _chatProcess->scanCounts[next][i];
	}
	requestMessagesSlice();
	return true;
}

void ApiWrap::requestRangeTotal(Fn<void(int)> done) {
	if (!_chatProcess || !_takeoutId) {
		return;
	}
	// Filter probes already carry the date/id range: their sums
	// are the exact selected total, no advisory query needed.
	// Same for a scan walking search: its walked messages are the
	// selected ones. History walks keep the advisory walked total.
	if (_chatProcess->hasSelectedTotal) {
		done(_chatProcess->selectedTotal);
		return;
	}
	if (_chatProcess->scanBySearch
		&& !_chatProcess->scanCounts.empty()) {
		done(SearchTotalSkippingUnions(
			_chatProcess->scanFilters,
			_chatProcess->scanCounts));
		return;
	}
	const auto peer = _chatProcess->info.input;
	const auto takeout = *_takeoutId;
	const auto gen = _dedupGen;
	const auto from = _settings->singlePeerFrom
		? MTP_int(*_settings->singlePeerFrom)
		: MTP_int(0);
	const auto till = _settings->singlePeerTill
		? MTP_int(*_settings->singlePeerTill)
		: MTP_int(0);
	mainRequest(MTPmessages_Search(
		MTP_flags(0),
		peer,
		MTP_string(),
		MTPInputPeer(),
		MTPInputPeer(),
		MTPVector<MTPReaction>(),
		MTPint(),
		MTP_inputMessagesFilterEmpty(),
		from,
		till,
		MTP_int(0),
		MTP_int(0),
		MTP_int(1),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0)
	)).fail([=](const MTP::Error &error) {
		// Advisory query: never kill the run, keep the fallback total.
		return true;
	}).done([=](const MTPmessages_Messages &result) {
		if (gen != _dedupGen || !_chatProcess || _takeoutId != takeout) {
			return;
		}
		const auto count = result.match(
			[](const MTPDmessages_messages &data) {
				return int(data.vmessages().v.size());
			}, [](const MTPDmessages_messagesSlice &data) {
				return data.vcount().v;
			}, [](const MTPDmessages_channelMessages &data) {
				return data.vcount().v;
			}, [](const MTPDmessages_messagesNotModified &data) {
				return -1;
			});
		if (count < 0) {
			return;
		}
		done(count);
	}).send();
}

void ApiWrap::finishExport(FnMut<void()> done) {
	// Shared session takeout outlives the export: never finish it
	// here, just drop the local handle.
	_takeoutId = std::nullopt;
	clearExTmpOnly();
	done();
}

void ApiWrap::skipFile(uint64 randomId) {
	if (!_fileProcess || _fileProcess->randomId != randomId) {
		return;
	}
	LOG(("Export Info: File skipped."));
	Assert(!_fileProcess->requests.empty());
	for (const auto &entry : base::take(_fileProcess->requestIds)) {
		_mtp.request(entry.second).cancel();
	}
	base::take(_fileProcess)->done(QString());
}

bool ApiWrap::requestPause() {
	if (_scanMode || !_chatProcess || _paused) {
		return false;
	}
	_pauseRequested = true;
	return true;
}

bool ApiWrap::exportPaused() const {
	return _paused;
}

rpl::producer<bool> ApiWrap::pauseChanges() const {
	return _pauseChanges.events();
}

void ApiWrap::setPauseFlushHandler(
		Fn<Data::MessagesSlice(Data::MessagesSlice)> handler) {
	_pauseFlushHandler = std::move(handler);
}

void ApiWrap::resumeExport() {
	_pauseRequested = false;
	if (!_paused) {
		return;
	}
	_paused = false;
	_pauseChanges.fire(false);
	if (_fileProcess) {
		loadFilePart();
	} else if (_chatProcess && _chatProcess->slice.has_value()) {
		loadNextMessageFile();
	}
}

void ApiWrap::parkForPause() {
	if (_paused || !_chatProcess) {
		return;
	}
	_paused = true;
	if (_pauseFlushHandler && _chatProcess->slice.has_value()) {
		auto &slice = *_chatProcess->slice;
		auto &list = slice.list;
		const auto fileIndex = (_chatProcess->fileIndex > 0)
			? size_t(_chatProcess->fileIndex)
			: size_t(0);
		const auto done = (fileIndex < list.size())
			? fileIndex
			: list.size();
		_pauseFlushedPrefix = int(done);
		if (done > 0) {
			auto prefix = Data::MessagesSlice();
			prefix.list.reserve(done);
			for (auto i = size_t(0); i != done; ++i) {
				prefix.list.push_back(std::move(list[i]));
			}
			prefix.peers = slice.peers;
			auto written = _pauseFlushHandler(std::move(prefix));
			for (auto i = size_t(0);
				i != done && i != written.list.size();
				++i) {
				list[i] = std::move(written.list[i]);
			}
		}
	}
	commitExportProgress(_chatProcess->committedMax, u"paused"_q);
	_pauseChanges.fire(true);
}

void ApiWrap::cancelExportFast() {
	_dedupGen++;
	_decidingFiles.clear();
	_pauseRequested = false;
	_paused = false;
	_updateMode = false;
	_updateCheckHandler = nullptr;
	_takeoutInvalidPending = false;
	if (_fileProcess) {
		_fileProcess->file.close();
	}
	clearDedupRun();
	if (const auto db = dedupDb()) {
		for (const auto docId : base::take(_inflightDocs)) {
			Export::CancelFile(*db, docId);
		}
	} else {
		_inflightDocs.clear();
	}
	_pendingHash.clear();
}

void ApiWrap::requestSinglePeerDialog() {
	auto doneSinglePeer = [=](const auto &result) {
		appendSinglePeerDialogs(
			Data::ParseDialogsInfo(_settings->singlePeer, result));
	};
	const auto requestUser = [&](const MTPInputUser &data) {
		mainRequest(MTPusers_GetUsers(
			MTP_vector<MTPInputUser>(1, data)
		)).done(std::move(doneSinglePeer)).send();
	};
	_settings->singlePeer.match([&](const MTPDinputPeerUser &data) {
		requestUser(MTP_inputUser(data.vuser_id(), data.vaccess_hash()));
	}, [&](const MTPDinputPeerChat &data) {
		mainRequest(MTPmessages_GetChats(
			MTP_vector<MTPlong>(1, data.vchat_id())
		)).done(std::move(doneSinglePeer)).send();
	}, [&](const MTPDinputPeerChannel &data) {
		mainRequest(MTPchannels_GetChannels(
			MTP_vector<MTPInputChannel>(
				1,
				MTP_inputChannel(data.vchannel_id(), data.vaccess_hash()))
		)).done(std::move(doneSinglePeer)).send();
	}, [&](const MTPDinputPeerSelf &data) {
		requestUser(MTP_inputUserSelf());
	}, [&](const MTPDinputPeerUserFromMessage &data) {
		Unexpected("From message peer in ApiWrap::requestSinglePeerDialog.");
	}, [&](const MTPDinputPeerChannelFromMessage &data) {
		Unexpected("From message peer in ApiWrap::requestSinglePeerDialog.");
	}, [](const MTPDinputPeerEmpty &data) {
		Unexpected("Empty peer in ApiWrap::requestSinglePeerDialog.");
	});
}

mtpRequestId ApiWrap::requestSinglePeerMigrated(
		const Data::DialogInfo &info) {
	const auto input = info.input.match([&](
		const MTPDinputPeerChannel & data) {
		return MTP_inputChannel(
			data.vchannel_id(),
			data.vaccess_hash());
	}, [](auto&&) -> MTPinputChannel {
		Unexpected("Peer type in a supergroup.");
	});
	return mainRequest(MTPchannels_GetFullChannel(
		input
	)).done([=](const MTPmessages_ChatFull &result) {
		auto info = result.match([&](
				const MTPDmessages_chatFull &data) {
			const auto migratedChatId = data.vfull_chat().match([&](
					const MTPDchannelFull &data) {
				return data.vmigrated_from_chat_id().value_or_empty();
			}, [](auto &&other) -> BareId {
				return 0;
			});
			return migratedChatId
				? Data::ParseDialogsInfo(
					MTP_inputPeerChat(MTP_long(migratedChatId)),
					MTP_messages_chats(data.vchats()))
				: Data::DialogsInfo();
		});
		appendSinglePeerDialogs(std::move(info));
	}).send();
}

void ApiWrap::appendSinglePeerDialogs(Data::DialogsInfo &&info) {
	const auto isSupergroupType = [](Data::DialogInfo::Type type) {
		using Type = Data::DialogInfo::Type;
		return (type == Type::PrivateSupergroup)
			|| (type == Type::PublicSupergroup);
	};
	const auto isChannelType = [](Data::DialogInfo::Type type) {
		using Type = Data::DialogInfo::Type;
		return (type == Type::PrivateChannel)
			|| (type == Type::PublicChannel);
	};

	auto migratedRequestId = mtpRequestId(0);
	const auto last = _dialogsProcess->splitIndexPlusOne - 1;
	for (auto &info : info.chats) {
		if (isSupergroupType(info.type) && !migratedRequestId) {
			migratedRequestId = requestSinglePeerMigrated(info);
			continue;
		} else if (isChannelType(info.type) || info.isMonoforum) {
			continue;
		}
		for (auto i = last; i != 0; --i) {
			info.splits.push_back(i - 1);
			info.messagesCountPerSplit.push_back(0);
		}
	}

	if (!migratedRequestId) {
		_dialogsProcess->processedCount += info.chats.size();
	}
	appendDialogsSlice(std::move(info));

	if (migratedRequestId
		|| !_dialogsProcess->progress(_dialogsProcess->processedCount)) {
		return;
	}
	finishDialogsList();
}

void ApiWrap::requestDialogsSlice() {
	Expects(_dialogsProcess != nullptr);

	if (_settings->onlySinglePeer()) {
		requestSinglePeerDialog();
		return;
	}

	const auto splitIndex = _dialogsProcess->splitIndexPlusOne - 1;
	const auto hash = uint64(0);
	splitRequest(splitIndex, MTPmessages_GetDialogs(
		MTP_flags(0),
		MTPint(), // folder_id
		MTP_int(_dialogsProcess->offsetDate),
		MTP_int(_dialogsProcess->offsetId),
		_dialogsProcess->offsetPeer,
		MTP_int(kChatsSliceLimit),
		MTP_long(hash)
	)).done([=](const MTPmessages_Dialogs &result) {
		if (result.type() == mtpc_messages_dialogsNotModified) {
			error("Unexpected dialogsNotModified received.");
			return;
		}
		auto finished = result.match(
		[](const MTPDmessages_dialogs &data) {
			return true;
		}, [](const MTPDmessages_dialogsSlice &data) {
			return data.vdialogs().v.isEmpty();
		}, [](const MTPDmessages_dialogsNotModified &data) {
			return true;
		});

		auto info = Data::ParseDialogsInfo(result);
		_dialogsProcess->processedCount += info.chats.size();
		const auto last = info.chats.empty()
			? Data::DialogInfo()
			: info.chats.back();
		appendDialogsSlice(std::move(info));

		if (!_dialogsProcess->progress(_dialogsProcess->processedCount)) {
			return;
		}

		if (!finished && last.topMessageDate > 0) {
			_dialogsProcess->offsetId = last.topMessageId;
			_dialogsProcess->offsetDate = last.topMessageDate;
			_dialogsProcess->offsetPeer = last.input;
		} else if (!useOnlyLastSplit()
			&& --_dialogsProcess->splitIndexPlusOne > 0) {
			_dialogsProcess->offsetId = 0;
			_dialogsProcess->offsetDate = 0;
			_dialogsProcess->offsetPeer = MTP_inputPeerEmpty();
		} else {
			requestLeftChannelsIfNeeded();
			return;
		}
		requestDialogsSlice();
	}).send();
}

void ApiWrap::appendDialogsSlice(Data::DialogsInfo &&info) {
	Expects(_dialogsProcess != nullptr);
	Expects(_dialogsProcess->splitIndexPlusOne <= _splits.size());

	appendChatsSlice(
		*_dialogsProcess,
		_dialogsProcess->info.chats,
		std::move(info.chats),
		_dialogsProcess->splitIndexPlusOne - 1);
}

void ApiWrap::requestLeftChannelsIfNeeded() {
	if (_settings->types & Settings::Type::GroupsChannelsMask) {
		requestLeftChannelsList([=](int count) {
			Expects(_dialogsProcess != nullptr);

			return _dialogsProcess->progress(
				_dialogsProcess->processedCount + count);
		}, [=](Data::DialogsInfo &&result) {
			Expects(_dialogsProcess != nullptr);

			_dialogsProcess->info.left = std::move(result.left);
			finishDialogsList();
		});
	} else {
		finishDialogsList();
	}
}

void ApiWrap::finishDialogsList() {
	Expects(_dialogsProcess != nullptr);

	const auto process = base::take(_dialogsProcess);
	Data::FinalizeDialogsInfo(process->info, *_settings);
	process->done(std::move(process->info));
}

void ApiWrap::requestLeftChannelsSliceGeneric(FnMut<void()> done) {
	Expects(_leftChannelsProcess != nullptr);

	mainRequest(MTPchannels_GetLeftChannels(
		MTP_int(_leftChannelsProcess->offset)
	)).done([=, done = std::move(done)](
			const MTPmessages_Chats &result) mutable {
		Expects(_leftChannelsProcess != nullptr);

		appendLeftChannelsSlice(Data::ParseLeftChannelsInfo(result));

		const auto process = _leftChannelsProcess.get();
		process->offset += result.match(
		[](const auto &data) {
			return int(data.vchats().v.size());
		});

		process->fullCount = result.match(
		[](const MTPDmessages_chats &data) {
			return int(data.vchats().v.size());
		}, [](const MTPDmessages_chatsSlice &data) {
			return data.vcount().v;
		});

		process->finished = result.match(
		[](const MTPDmessages_chats &data) {
			return true;
		}, [](const MTPDmessages_chatsSlice &data) {
			return data.vchats().v.isEmpty();
		});

		if (process->progress) {
			if (!process->progress(process->info.left.size())) {
				return;
			}
		}

		done();
	}).send();
}

void ApiWrap::appendLeftChannelsSlice(Data::DialogsInfo &&info) {
	Expects(_leftChannelsProcess != nullptr);
	Expects(!_splits.empty());

	appendChatsSlice(
		*_leftChannelsProcess,
		_leftChannelsProcess->info.left,
		std::move(info.left),
		_splits.size() - 1);
}

void ApiWrap::appendChatsSlice(
		ChatsProcess &process,
		std::vector<Data::DialogInfo> &to,
		std::vector<Data::DialogInfo> &&from,
		int splitIndex) {
	Expects(_settings != nullptr);

	const auto types = _settings->types;
	const auto goodByTypes = [&](const Data::DialogInfo &info) {
		return ((types & SettingsFromDialogsType(info.type)) != 0);
	};
	auto filtered = ranges::views::all(
		from
	) | ranges::views::filter([&](const Data::DialogInfo &info) {
		if (goodByTypes(info)) {
			return true;
		} else if (info.migratedToChannelId
			&& (((types & Settings::Type::PublicGroups) != 0)
				|| ((types & Settings::Type::PrivateGroups) != 0))) {
			return true;
		}
		return false;
	});
	to.reserve(to.size() + from.size());
	for (auto &info : filtered) {
		const auto nextIndex = to.size();
		if (info.migratedToChannelId) {
			const auto toPeerId = PeerId(info.migratedToChannelId);
			const auto i = process.indexByPeer.find(toPeerId);
			if (i != process.indexByPeer.end()
				&& Data::AddMigrateFromSlice(
					to[i->second],
					info,
					splitIndex,
					int(_splits.size()))) {
				continue;
			} else if (!goodByTypes(info)) {
				continue;
			}
		}
		const auto &[i, ok] = process.indexByPeer.emplace(
			info.peerId,
			nextIndex);
		if (ok) {
			to.push_back(std::move(info));
		}
		to[i->second].splits.push_back(splitIndex);
		to[i->second].messagesCountPerSplit.push_back(0);
	}
}

void ApiWrap::consumeChatPage(MTPmessages_Messages result) {
	Expects(_chatProcess != nullptr);

	const auto cursor = _chatProcess->walkStarted
		? _chatProcess->walkCursor
		: ((_settings->useIdRange && _chatProcess->fromId > 0)
			? _chatProcess->fromId
			: int32(1));
	auto last = false;
	auto raw = 0;
	auto pageMax = int32(0);
	auto slice = Data::MessagesSlice();
	result.match([&](const MTPDmessages_messagesNotModified &data) {
		error("Unexpected messagesNotModified received.");
	}, [&](const auto &data) {
		if constexpr (MTPDmessages_messages::Is<decltype(data)>()) {
			last = true;
		}
		slice = Data::ParseMessagesSlice(
			_chatProcess->context,
			data.vmessages(),
			data.vusers(),
			data.vchats(),
			_chatProcess->info.relativePath);
		for (const auto &message : data.vmessages().v) {
			++raw;
			const auto id = message.match([](const auto &data) {
				return int32(data.vid().v);
			});
			if (id > pageMax) {
				pageMax = id;
			}
		}
	});
	if (!_chatProcess) {
		return;
	}
	// Ascending emit order regardless of wire order.
	ranges::sort(
		slice.list,
		ranges::less(),
		[](const Data::Message &message) { return message.id; });
	// Drop intra-response repeats first: the cross-page skip
	// below only sees the previous maximum.
	slice.list.erase(ranges::unique(
		slice.list,
		ranges::equal_to(),
		[](const Data::Message &message) { return message.id; }
	), end(slice.list));
	// Drop repeats and out-of-range ids; the cursor below still
	// advances over the raw page.
	slice.list.erase(std::remove_if(
		slice.list.begin(),
		slice.list.end(),
		[&](const Data::Message &message) {
			if (message.id <= _chatProcess->writtenMax) {
				return true;
			}
			if (_settings->useIdRange) {
				if (_chatProcess->fromId > 0
					&& message.id < _chatProcess->fromId) {
					return true;
				}
				if (_chatProcess->tillId > 0
					&& message.id > _chatProcess->tillId) {
					return true;
				}
			}
			return false;
		}), slice.list.end());
	// Order-proof cursor over raw ids: placeholders and filters
	// never steer it. A non-advancing cursor ends the split
	// instead of paging the same window forever.
	const auto next = (pageMax >= std::numeric_limits<int32>::max() - 1)
		? pageMax
		: (pageMax + 1);
	_chatProcess->walkStarted = true;
	if (pageMax > _chatProcess->writtenMax) {
		_chatProcess->writtenMax = pageMax;
	}
	if (!slice.list.empty()) {
		_chatProcess->walkCursor = next;
		startMessagesSlice(std::move(slice));
		return;
	}
	if (!last && raw >= kMessagesSliceLimit && next > cursor) {
		_chatProcess->walkCursor = next;
		requestMessagesSlice();
		return;
	}
	startMessagesSlice({});
}

void ApiWrap::firePagePrefetch() {
	Expects(_chatProcess != nullptr);

	// One outstanding list request max: never overlap a prefetch
	// with anything, and never fire twice. Search respects the
	// server page rate; history has no rate gate.
	if (_chatProcess->pagePrefetchInflight
		|| _chatProcess->pagePrefetch.has_value()
		|| !_chatProcess->walkStarted) {
		return;
	}
	if (_chatProcess->scanBySearch
		&& crl::now() < _chatProcess->scanNextAllowedAt) {
		return;
	}
	const auto gen = _dedupGen;
	const auto split = _chatProcess->localSplitIndex;
	const auto filter = _chatProcess->scanFilterIndex;
	const auto cursor = _chatProcess->walkCursor;
	_chatProcess->pagePrefetchInflight = true;
	_chatProcess->pagePrefetchSplit = split;
	_chatProcess->pagePrefetchFilter = filter;
	requestChatMessages(
		_chatProcess->info.splits[split],
		cursor,
		-kMessagesSliceLimit,
		kMessagesSliceLimit,
		[=](MTPmessages_Messages &&result) mutable {
		if (!_chatProcess || gen != _dedupGen) {
			return;
		}
		_chatProcess->pagePrefetchInflight = false;
		if (_chatProcess->localSplitIndex != split
			|| _chatProcess->scanFilterIndex != filter) {
			if (_chatProcess->pagePrefetchWaited) {
				_chatProcess->pagePrefetchWaited = false;
				requestMessagesSlice();
			}
			return;
		}
		_chatProcess->pagePrefetch = std::move(result);
		if (_chatProcess->pagePrefetchWaited) {
			_chatProcess->pagePrefetchWaited = false;
			requestMessagesSlice();
		}
	});
}

void ApiWrap::requestMessagesSlice() {
	Expects(_chatProcess != nullptr);

	// Prefetched page arrives in walk order: consume it instead of
	// firing, or park until it lands. Either way at most one list
	// request is ever outstanding.
	if (_chatProcess->pagePrefetch.has_value()) {
		auto prefetched = std::move(*_chatProcess->pagePrefetch);
		_chatProcess->pagePrefetch.reset();
		consumeChatPage(std::move(prefetched));
		return;
	}
	if (_chatProcess->pagePrefetchInflight) {
		_chatProcess->pagePrefetchWaited = true;
		return;
	}
	// Resume seeds the cursor past everything committed: pages at or
	// below the checkpoint are never re-fetched, and the written-max
	// filter drops any overlap at the edge.
	if (_resumeArmed && !_chatProcess->walkStarted) {
		_resumeArmed = false;
		if (_chatProcess->scanBySearch
			&& _chatProcess->scanFilters.size() > 1
			&& _resumeFilterRedo) {
			// Multi-filter resume redoes its persisted filter
			// from the start; dedup links already-saved files
			// instead of re-downloading them.
			_chatProcess->scanFilterIndex = std::min(
				_resumeFilterIndex,
				int(_chatProcess->scanFilters.size()) - 1);
			_chatProcess->localSplitIndex = 0;
			_chatProcess->walkStarted = true;
			_chatProcess->walkCursor = 1;
			_chatProcess->writtenMax = 0;
			for (auto i = 0;
				i != _chatProcess->info.splits.size();
				++i) {
				_chatProcess->info.messagesCountPerSplit[i]
					= _chatProcess->scanCounts[
						_chatProcess->scanFilterIndex][i];
			}
		} else {
			if (_resumeSplitIndex > 0
				&& _resumeSplitIndex
					< _chatProcess->info.splits.size()) {
				_chatProcess->localSplitIndex = _resumeSplitIndex;
			}
			_chatProcess->walkStarted = true;
			_chatProcess->walkCursor = _resumeLastId + 1;
			_chatProcess->writtenMax = _resumeLastId;
			// Filter advances restart here on updates, never at
			// 1: only new messages may be picked up.
			_walkIdFloor = _resumeLastId + 1;
		}
	}
	// Search pages go strictly serial at the server rate. Firing early
	// floods, and the flood wait costs more than the pause.
	if (_scanMode && _chatProcess->scanBySearch) {
		const auto wait = _chatProcess->scanNextAllowedAt - crl::now();
		if (wait > 0) {
			LOG(("Export Info: Server page rate, waiting %1ms.").arg(wait));
			const auto runner = _runner;
			crl::on_main([=] {
				QTimer::singleShot(int(wait), Qt::CoarseTimer, [=] {
					runner([=] {
						if (_chatProcess) {
							requestMessagesSlice();
						}
					});
				});
			});
			return;
		}
	}
	// Single-pass oldest-first walk for every list type: every page
	// is parsed, sorted ascending, deduped against everything
	// written, and emitted at once. One counter climbs from zero.
	const auto count = _chatProcess->info.messagesCountPerSplit[
		_chatProcess->localSplitIndex];
	LOG(("ExportDiag: TEMP walk split=%1 count=%2 bySearch=%3 floor=%4 "
		"from=%5 till=%6 scan=%7").arg(_chatProcess->localSplitIndex)
		.arg(count).arg(_chatProcess->scanBySearch)
		.arg(_chatProcess->fromId).arg(_chatProcess->tillId)
		.arg(_scanMode));
	if (!count) {
		startMessagesSlice({});
		return;
	}
	const auto cursor = _chatProcess->walkStarted
		? _chatProcess->walkCursor
		: ((_settings->useIdRange && _chatProcess->fromId > 0)
			? _chatProcess->fromId
			: int32(1));
	requestChatMessages(
		_chatProcess->info.splits[_chatProcess->localSplitIndex],
		cursor,
		-kMessagesSliceLimit,
		kMessagesSliceLimit,
		[=](MTPmessages_Messages &&result) mutable {
		Expects(_chatProcess != nullptr);

		consumeChatPage(std::move(result));
	});
}

void ApiWrap::requestChatMessages(
		int splitIndex,
		int offsetId,
		int addOffset,
		int limit,
		FnMut<void(MTPmessages_Messages&&)> done) {
	Expects(_chatProcess != nullptr);

	_chatProcess->requestDone = std::move(done);
	const auto doneHandler = [=](MTPmessages_Messages &&result) {
		// A fired prefetch can land after the walk took the process
		// at finish: drop it, the run is already complete.
		if (!_chatProcess) {
			return;
		}
		base::take(_chatProcess->requestDone)(std::move(result));
	};
	const auto gen = _dedupGen;
	const auto takeoutFail = [=](const MTP::Error &result) {
		if (result.type() != u"TAKEOUT_INVALID"_q) {
			return false;
		}
		refreshTakeoutSession([=](uint64 fresh) mutable {
			if (gen != _dedupGen || !_chatProcess) {
				return;
			}
			if (!fresh) {
				error(result);
				return;
			}
			requestChatMessages(
				splitIndex,
				offsetId,
				addOffset,
				limit,
				base::take(_chatProcess->requestDone));
		});
		return true;
	};
	const auto splitsCount = int(_splits.size());
	const auto realPeerInput = (splitIndex >= 0)
		? _chatProcess->info.input
		: _chatProcess->info.migratedFromInput;
	const auto outgoingInput = _chatProcess->info.isMonoforum
		? _chatProcess->info.monoforumBroadcastInput
		: MTP_inputPeerSelf();
	const auto realSplitIndex = (splitIndex >= 0)
		? splitIndex
		: (splitsCount + splitIndex);
	const auto minId = (_chatProcess->fromId > 0)
		? int32(_chatProcess->fromId - 1)
		: int32(0);
	const auto maxId = (_chatProcess->tillId > 0)
		? int32(_chatProcess->tillId + 1)
		: int32(0);
	// A first page with backward paging under any bound (id floor
	// or date range) returns an empty page with the count intact:
	// page forward instead. Later pages keep backward paging.
	const auto dateBounded = (_settings->singlePeerFrom
		|| _settings->singlePeerTill);
	const auto floored = ((minId > 0 && offsetId <= minId)
		|| (dateBounded && offsetId <= 1));
	const auto pageOffsetId = floored ? 0 : offsetId;
	const auto pageAddOffset = floored ? 0 : addOffset;
	if (_chatProcess->info.onlyMyMessages) {
		splitRequest(realSplitIndex, MTPmessages_Search(
			MTP_flags(MTPmessages_Search::Flag::f_from_id),
			realPeerInput,
			MTP_string(), // query
			outgoingInput,
			MTPInputPeer(), // saved_peer_id
			MTPVector<MTPReaction>(), // saved_reaction
			MTPint(), // top_msg_id
			MTP_inputMessagesFilterEmpty(),
			MTP_int(0), // min_date
			MTP_int(0), // max_date
			MTP_int(pageOffsetId),
			MTP_int(pageAddOffset),
			MTP_int(limit),
			MTP_int(maxId), // max_id
			MTP_int(minId), // min_id
			MTP_long(0) // hash
		)).fail(takeoutFail).done(doneHandler).send();
	} else if (_chatProcess->scanBySearch
		&& _chatProcess->scanFilterIndex >= 0
		&& _chatProcess->scanFilterIndex
			< int(_chatProcess->scanFilters.size())) {
		// No min/max_date on walk pages: server date bounds
		// fragment search pagination (short/empty pages with the
		// count intact). Dates filter client-side per message; the
		// id floor bounds the walk.
		splitRequest(realSplitIndex, MTPmessages_Search(
			MTP_flags(0),
			realPeerInput,
			MTP_string(), // query
			MTPInputPeer(), // from_id
			MTPInputPeer(), // saved_peer_id
			MTPVector<MTPReaction>(), // saved_reaction
			MTPint(), // top_msg_id
			_chatProcess->scanFilters[_chatProcess->scanFilterIndex],
			MTP_int(0), // min_date
			MTP_int(0), // max_date
			MTP_int(pageOffsetId),
			MTP_int(pageAddOffset),
			MTP_int(limit),
			MTP_int(maxId), // max_id
			MTP_int(minId), // min_id
			MTP_long(0) // hash
		)).done([=](MTPmessages_Messages &&result) mutable {
			// Honor the server page rate or the next pages flood.
			if (_chatProcess) {
				result.match([&](const MTPDmessages_messagesSlice &data) {
					if (const auto rate = data.vnext_rate()) {
						_chatProcess->scanNextAllowedAt = crl::now()
							+ rate->v * 1000;
					}
			}, [](const auto &) {});
			}
			doneHandler(std::move(result));
		}).fail(takeoutFail).send();
	} else {
		splitRequest(realSplitIndex, MTPmessages_GetHistory(
			realPeerInput,
			MTP_int(pageOffsetId),
			MTP_int(0), // offset_date
			MTP_int(pageAddOffset),
			MTP_int(limit),
			MTP_int(maxId), // max_id
			MTP_int(minId), // min_id
			MTP_long(0)  // hash
		)).fail([=](const MTP::Error &error) {
			Expects(_chatProcess != nullptr);

			if (takeoutFail(error)) {
				return true;
			}
			if (error.type() == u"CHANNEL_PRIVATE"_q) {
				if (realPeerInput.type() == mtpc_inputPeerChannel
					&& !_chatProcess->info.onlyMyMessages) {

					// Perhaps we just left / were kicked from channel.
					// Just switch to only my messages.
					_chatProcess->info.onlyMyMessages = true;
					requestChatMessages(
						splitIndex,
						offsetId,
						addOffset,
						limit,
						base::take(_chatProcess->requestDone));
					return true;
				}
			}
			return false;
		}).done(doneHandler).send();
	}
}



void ApiWrap::startMessagesSlice(Data::MessagesSlice &&slice) {
	Expects(bool(_chatProcess) != bool(_topicProcess));

	const auto process = _topicProcess
		? static_cast<AbstractMessagesProcess*>(_topicProcess.get())
		: static_cast<AbstractMessagesProcess*>(_chatProcess.get());
	Expects(!process->slice.has_value());

	if (slice.list.empty()) {
		process->lastSlice = true;
	}
	++_sliceGen;
	process->slice = std::move(slice);
	process->hydrationIndex = 0;
	process->hydrationPending = 0;
	process->fileIndex = 0;
	process->messageFileWork.clear();
	process->messageFileWorkIndex = 0;
	process->messageFileWorkMessageIndex = -1;

	resumeMessagesSlice();
}

void ApiWrap::resumeMessagesSlice() {
	Expects(bool(_chatProcess) != bool(_topicProcess));

	const auto topic = (_topicProcess != nullptr);
	const auto process = topic
		? static_cast<AbstractMessagesProcess*>(_topicProcess.get())
		: static_cast<AbstractMessagesProcess*>(_chatProcess.get());
	Expects(process->slice.has_value());

	// Scan needs only ids, sizes and hashes: skip hydration and emoji.
	// Hydration must still read complete: the walk asserts on it.
	if (_scanMode && (_chatProcess || _topicProcess)) {
		process->hydrationIndex = process->slice->list.size();
		process->hydrationPending = 0;
		beginSliceWalk(false);
		return;
	}
	const auto &list = process->slice->list;
	const auto parallel = skipMedia() ? kHydrateParallel : 1;
	while (process->hydrationIndex < list.size()
		&& process->hydrationPending < parallel) {
		const auto index = process->hydrationIndex;
		const auto &message = list[index];
		if (Data::SkipMessageByDate(message, *_settings)) {
			++process->hydrationIndex;
			continue;
		}
		if (!message.richMessage || !message.richMessage->part) {
			++process->hydrationIndex;
			continue;
		}
		const auto rawId = message.id;
		auto peer = topic
			? _topicProcess->inputPeer
			: _chatProcess->info.input;
		if (!topic) {
			const auto splitIndex = _chatProcess->info.splits[
				_chatProcess->localSplitIndex];
			if (splitIndex < 0) {
				peer = _chatProcess->info.migratedFromInput;
			}
		}
		const auto gen = _dedupGen;
		++process->hydrationIndex;
		++process->hydrationPending;
		mainRequest(MTPmessages_GetRichMessage(
			peer,
			MTP_int(rawId)
		)).handleFloodErrors().done([=](const MTPmessages_Messages &result) {
			hydrateMessageDone(topic, index, rawId, result, gen);
		}).send();
	}
	if (process->hydrationPending > 0) {
		return;
	}

	collectMessagesCustomEmoji(*process->slice);
	if (topic) {
		resolveTopicCustomEmoji();
	} else {
		resolveCustomEmoji();
	}
}

void ApiWrap::hydrateMessageDone(
		bool topic,
		int index,
		int32 rawId,
		const MTPmessages_Messages &result,
		int gen) {
	if (gen != _dedupGen) {
		return;
	}
	const auto richMessage = ExtractFullRichMessage(result, rawId);
	const auto process = topic
		? static_cast<AbstractMessagesProcess*>(_topicProcess.get())
		: static_cast<AbstractMessagesProcess*>(_chatProcess.get());
	const auto otherProcess = topic
		? static_cast<AbstractMessagesProcess*>(_chatProcess.get())
		: static_cast<AbstractMessagesProcess*>(_topicProcess.get());
	if (!richMessage
		|| !process
		|| otherProcess
		|| !process->slice) {
		error("Unexpected rich message hydration result.");
		return;
	}
	const auto &list = process->slice->list;
	if (index < 0 || index >= list.size()) {
		error("Unexpected rich message hydration result.");
		return;
	}
	const auto &message = list[index];
	if (message.id != rawId
		|| !message.richMessage
		|| !message.richMessage->part) {
		error("Unexpected rich message hydration result.");
		return;
	}
	const auto &relativePath = topic
		? _topicProcess->relativePath
		: _chatProcess->info.relativePath;
	auto parsed = Data::ParseRichMessage(
		process->context,
		*richMessage,
		relativePath,
		message.date);
	if (parsed.part
		|| !process->slice
		|| index < 0
		|| index >= process->slice->list.size()) {
		error("Unexpected rich message hydration result.");
		return;
	}
	auto &stored = process->slice->list[index];
	if (stored.id != rawId
		|| !stored.richMessage
		|| !stored.richMessage->part) {
		error("Unexpected rich message hydration result.");
		return;
	}
	stored.richMessage = std::move(parsed);
	--process->hydrationPending;
	resumeMessagesSlice();
}

void ApiWrap::collectMessagesCustomEmoji(const Data::MessagesSlice &slice) {
	const auto collect = [&](uint64 id) {
		if (id && !_resolvedCustomEmoji.contains(id)) {
			_unresolvedCustomEmoji.emplace(id);
		}
	};
	for (const auto &message : slice.list) {
		if (Data::SkipMessageByDate(message, *_settings)) {
			continue;
		}
		for (const auto &part : message.text) {
			if (part.type == Data::TextPart::Type::CustomEmoji) {
				collect(part.additional.toULongLong());
			}
		}
		for (const auto &reaction : message.reactions) {
			if (reaction.type == Data::Reaction::Type::CustomEmoji) {
				collect(reaction.documentId.toULongLong());
			}
		}
		if (message.richMessage) {
			VisitRichMessage(
				*message.richMessage,
				[&](const Data::RichText &text) {
					if (text.type == Data::RichText::Type::CustomEmoji) {
						collect(text.id);
					}
				},
				[](const Data::Photo &) {},
				[](const Data::Document &) {});
		}
	}
}

void ApiWrap::resolveCustomEmoji() {
	if (_unresolvedCustomEmoji.empty()) {
		beginSliceWalk(false);
		return;
	}
	const auto count = std::min(
		int(_unresolvedCustomEmoji.size()),
		kMaxEmojiPerRequest);
	auto v = QVector<MTPlong>();
	v.reserve(count);
	const auto till = end(_unresolvedCustomEmoji);
	const auto from = end(_unresolvedCustomEmoji) - count;
	for (auto i = from; i != till; ++i) {
		v.push_back(MTP_long(*i));
	}
	_unresolvedCustomEmoji.erase(from, till);
	const auto finalize = [=] {
		for (const auto &id : v) {
			if (_resolvedCustomEmoji.contains(id.v)) {
				continue;
			}
			_resolvedCustomEmoji.emplace(
				id.v,
				Data::Document{
					.file = {
						.skipReason = Data::File::SkipReason::Unavailable,
					},
				});
		}
		resolveCustomEmoji();
	};
	mainRequest(MTPmessages_GetCustomEmojiDocuments(
		MTP_vector<MTPlong>(v)
	)).fail([=](const MTP::Error &error) {
		LOG(("Export Error: Failed to get documents for emoji."));
		finalize();
		return true;
	}).done([=](const MTPVector<MTPDocument> &result) {
		for (const auto &entry : result.v) {
			auto document = Data::ParseDocument(
				_chatProcess->context,
				entry,
				_chatProcess->info.relativePath,
				TimeId());
			_resolvedCustomEmoji.emplace(document.id, std::move(document));
		}
		finalize();
	}).send();
}

Data::Message *ApiWrap::currentFileMessage() const {
	Expects(_chatProcess != nullptr);
	Expects(_chatProcess->slice.has_value());

	return &_chatProcess->slice->list[_chatProcess->fileIndex];
}

Data::FileOrigin ApiWrap::currentFileMessageOrigin() const {
	Expects(_chatProcess != nullptr);
	Expects(_chatProcess->slice.has_value());

	const auto splitIndex = _chatProcess->info.splits[
		_chatProcess->localSplitIndex];
	auto result = Data::FileOrigin();
	result.messageId = currentFileMessage()->id;
	result.split = (splitIndex >= 0)
		? splitIndex
		: (int(_splits.size()) + splitIndex);
	result.peer = (splitIndex >= 0)
		? _chatProcess->info.input
		: _chatProcess->info.migratedFromInput;
	return result;
}

std::optional<QByteArray> ApiWrap::getCustomEmoji(
		Data::Message &message,
		QByteArray &data) {
	if (const auto id = data.toULongLong()) {
		const auto i = _resolvedCustomEmoji.find(id);
		if (i == end(_resolvedCustomEmoji)) {
			return Data::TextPart::UnavailableEmoji();
		}
		auto &document = i->second;
		auto &file = document.file;
		const auto fileProgress = [=](FileProgress value) {
			if (_chatProcess) {
				return loadMessageEmojiProgress(value);
			} else if (_topicProcess) {
				return loadTopicEmojiProgress(value);
			}
			return true;
		};
		const auto policy = FilePolicy{
			.message = &message,
			.type = DocumentMediaType(document),
			.controllingSize = document.file.size,
		};
		const auto ready = processFileLoad(
			file,
			{ .customEmojiId = id },
			fileProgress,
			[=](const QString &path) { loadCustomEmojiDone(id, path); },
			policy);
		if (!ready) {
			return std::nullopt;
		}
		using SkipReason = Data::File::SkipReason;
		if (file.skipReason == SkipReason::Unavailable) {
			return Data::TextPart::UnavailableEmoji();
		} else if (file.skipReason == SkipReason::FileType
			|| file.skipReason == SkipReason::FileSize) {
			return QByteArray();
		} else {
			return file.relativePath.toUtf8();
		}
	}
	return data;
}

bool ApiWrap::messageCustomEmojiReady(Data::Message &message) {
	const auto resolve = [&](QByteArray &data) {
		auto result = getCustomEmoji(message, data);
		if (!result.has_value()) {
			return false;
		}
		data = base::take(*result);
		return true;
	};
	for (auto &part : message.text) {
		if (part.type == Data::TextPart::Type::CustomEmoji) {
			if (!resolve(part.additional)) {
				return false;
			}
		}
	}
	for (auto &reaction : message.reactions) {
		if (reaction.type == Data::Reaction::Type::CustomEmoji) {
			if (!resolve(reaction.documentId)) {
				return false;
			}
		}
	}
	if (!message.richMessage) {
		return true;
	}
	auto ready = true;
	VisitRichMessage(
		*message.richMessage,
		[&](Data::RichText &text) {
			if (ready
				&& text.type == Data::RichText::Type::CustomEmoji) {
				ready = resolve(text.customEmojiData);
			}
		},
		[](Data::Photo &) {},
		[](Data::Document &) {});
	return ready;
}

void ApiWrap::buildMessageFileWork(
		AbstractMessagesProcess &process,
		Data::Message &message) {
	Expects(process.messageFileWork.empty());
	Expects(process.messageFileWorkIndex == 0);
	Expects(process.messageFileWorkMessageIndex < 0);

	process.messageContentSelected = false;
	if (_stats) {
		_stats->incrementMessage();
		const auto selected = _settings->media.types;
		const auto countPolls = ((selected & MediaSettings::Type::Poll)
			== MediaSettings::Type::Poll)
			|| ((selected & MediaSettings::Type::FullHistory)
				== MediaSettings::Type::FullHistory);
		if (countPolls && v::match(message.media.content,
			[](const Data::Poll &) { return true; },
			[](const auto &) { return false; })) {
			_stats->incrementType(MediaSettings::Type::Poll, 0);
			process.messageContentSelected = true;
		}
		const auto countLinks = ((selected & MediaSettings::Type::Link)
			== MediaSettings::Type::Link)
			|| ((selected & MediaSettings::Type::FullHistory)
				== MediaSettings::Type::FullHistory);
		const auto countText = ((selected & MediaSettings::Type::Text)
			== MediaSettings::Type::Text)
			|| ((selected & MediaSettings::Type::FullHistory)
				== MediaSettings::Type::FullHistory);
		auto links = 0;
		auto dupLinks = 0;
		auto hasText = false;
		if (countText || countLinks) {
			for (const auto &part : message.text) {
				if (part.type == Data::TextPart::Type::Text
					&& !part.text.isEmpty()) {
					hasText = true;
					break;
				}
			}
		}
		if (countLinks) {
			for (const auto &link : message.links) {
				if (_knownLinks.contains(link)) {
					++dupLinks;
				} else {
					_knownLinks.insert(link);
					++links;
				}
				_linkUrls.emplace(link);
			}
			if (!links && !dupLinks) {
				if (const auto webpage = std::get_if<Data::WebPage>(
						&message.media.content)) {
					if (!webpage->url.isEmpty()) {
						if (_knownLinks.contains(webpage->url)) {
							++dupLinks;
						} else {
							_knownLinks.insert(webpage->url);
							++links;
						}
						_linkUrls.emplace(webpage->url);
					}
				}
			}
		}
		const auto hasMedia = v::match(message.media.content,
			[](v::null_t) { return false; },
			[](const auto &) { return true; });
		if (countText && hasText && !hasMedia) {
			_stats->incrementTextMessage();
			process.messageContentSelected = true;
		}
		if (countLinks && links + dupLinks > 0) {
			_stats->incrementLinkMessage(links + dupLinks);
			process.messageContentSelected = true;
		}
		if (countLinks && dupLinks > 0) {
			_stats->incrementLinkDuplicates(dupLinks);
		}
	}

	const auto append = [&process](
			Data::File *file,
			MediaSettings::Type type,
			int64 controllingSize,
			bool rich,
			bool main) {
		Expects(file != nullptr);
		process.messageFileWork.push_back(MessageFileWork{
			.file = file,
			.controllingSize = controllingSize,
			.type = type,
			.rich = rich,
			.main = main,
		});
	};

	auto ordinaryMain = static_cast<Data::File*>(nullptr);
	auto ordinaryDocument = static_cast<Data::Document*>(nullptr);
	auto ordinaryType = MediaSettings::Type::Photo;
	v::match(
		message.action.content,
		[&](Data::ActionChatEditPhoto &action) {
			ordinaryMain = &action.photo.image.file;
		},
		[&](Data::ActionSuggestProfilePhoto &action) {
			ordinaryMain = &action.photo.image.file;
		},
		[](auto &) {});
	v::match(
		message.media.content,
		[&](Data::Photo &photo) {
			if (!ordinaryMain) {
				ordinaryMain = &photo.image.file;
			}
		},
		[&](Data::Document &document) {
			ordinaryDocument = &document;
			if (!ordinaryMain) {
				ordinaryMain = &document.file;
				ordinaryType = DocumentMediaType(document);
			}
		},
		[&](Data::SharedContact &contact) {
			if (!ordinaryMain) {
				ordinaryMain = &contact.vcard;
			}
		},
		[](auto &) {});
	const auto ordinarySize = ordinaryMain ? ordinaryMain->size : 0;
	if (ordinaryMain) {
		append(ordinaryMain, ordinaryType, ordinarySize, false, true);
	}
	if (ordinaryDocument && ordinaryDocument->thumb.width > 0) {
		append(
			&ordinaryDocument->thumb.file,
			DocumentMediaType(*ordinaryDocument),
			ordinarySize,
			false,
			false);
	}

	if (message.richMessage) {
		auto richFiles = std::set<Data::File*>();
		const auto appendRich = [&richFiles, &append](
				Data::File &file,
				MediaSettings::Type type,
				int64 controllingSize,
				bool main) {
			if (richFiles.emplace(&file).second) {
				append(&file, type, controllingSize, true, main);
			}
		};
		VisitRichMessage(
			*message.richMessage,
			[](Data::RichText &) {},
			[&](Data::Photo &photo) {
				appendRich(
					photo.image.file,
					MediaSettings::Type::Photo,
					photo.image.file.size,
					false);
			},
			[&](Data::Document &document) {
				const auto type = DocumentMediaType(document);
				const auto controllingSize = document.file.size;
				appendRich(document.file, type, controllingSize, false);
				if (document.thumb.width > 0) {
					appendRich(document.thumb.file, type, controllingSize, false);
				}
			});
	}

	process.messageFileWorkMessageIndex = process.fileIndex;
}

void ApiWrap::loadNextMessageFile() {
	Expects(_chatProcess != nullptr);
	Expects(_chatProcess->slice.has_value());
	Expects(_chatProcess->hydrationIndex
		== _chatProcess->slice->list.size());

	auto &process = *_chatProcess;
	auto &list = process.slice->list;
	while (process.fileIndex < list.size()) {
		const auto index = process.fileIndex;
		auto &message = list[index];
		if (Data::SkipMessageByDate(message, *_settings)) {
			Expects(process.messageFileWork.empty());
			Expects(process.messageFileWorkMessageIndex < 0);
			if (message.id > process.committedMax) {
				process.committedMax = message.id;
			}
			++process.fileIndex;
			continue;
		}
		if (process.scanBySearch) {
			const auto split = process.info.splits[process.localSplitIndex];
			const auto key = (uint64(uint32(split)) << 32)
				| uint64(uint32(message.id));
			if (_scanSeenMessages.contains(key)) {
				if (message.id > process.committedMax) {
					process.committedMax = message.id;
				}
				++process.fileIndex;
				continue;
			}
		}
		if (!_scanMode
			&& !messageCustomEmojiReady(message)) {
			return;
		}
		if (process.messageFileWorkMessageIndex < 0) {
			if (_pauseRequested && !_paused) {
				parkForPause();
				return;
			}
			buildMessageFileWork(process, message);
		}
		Expects(process.messageFileWorkMessageIndex == index);
		while (process.messageFileWorkIndex
				< process.messageFileWork.size()) {
			const auto work = process.messageFileWork[
				process.messageFileWorkIndex];
			const auto target = work.file;
			Expects(target != nullptr);
			auto origin = currentFileMessageOrigin();
			origin.richMessage = work.rich;
			const auto policy = FilePolicy{
				.message = &message,
				.type = work.type,
				.controllingSize = work.controllingSize,
			.mainFile = work.main,
			};
			const auto ready = processFileLoad(
				*target,
				origin,
				[=](FileProgress value) {
					return loadMessageFileProgress(index, value);
				},
				[=](const QString &path) {
					loadMessageFileDone(index, target, path);
				},
				policy);
			if (!ready) {
				return;
			}
		Expects(!target->relativePath.isEmpty()
			|| target->skipReason != Data::File::SkipReason::None);
		++process.messageFileWorkIndex;
	}
	if (process.hasSelectedTotal) {
		auto fileWanted = false;
		for (const auto &work : process.messageFileWork) {
			const auto reason = work.file->skipReason;
			if (reason != Data::File::SkipReason::FileType
				&& reason != Data::File::SkipReason::FileSize
				&& reason != Data::File::SkipReason::DateLimits) {
				fileWanted = true;
				break;
			}
		}
		if (fileWanted || process.messageContentSelected) {
			++process.selectedDone;
		}
	}
	process.messageFileWork.clear();
	process.messageFileWorkIndex = 0;
	process.messageFileWorkMessageIndex = -1;
	if (process.scanBySearch) {
			const auto split = process.info.splits[process.localSplitIndex];
			_scanSeenMessages.emplace(
				(uint64(uint32(split)) << 32) | uint64(uint32(list[index].id)));
		}
		if (message.id > process.committedMax) {
			process.committedMax = message.id;
		}
		++process.fileIndex;
	}
	finishMessagesSlice();
}

void ApiWrap::finishMessagesSlice() {
	Expects(_chatProcess != nullptr);
	Expects(_chatProcess->slice.has_value());
	Expects(_chatProcess->messageFileWork.empty());
	Expects(_chatProcess->messageFileWorkIndex == 0);
	Expects(_chatProcess->messageFileWorkMessageIndex < 0);

	auto slice = *base::take(_chatProcess->slice);
	const auto flushed = _pauseFlushedPrefix;
	_pauseFlushedPrefix = 0;
	if (flushed > 0 && size_t(flushed) <= slice.list.size()) {
		slice.list.erase(
			slice.list.begin(),
			slice.list.begin() + flushed);
	}
	if (!slice.list.empty()) {
		if (_chatProcess->info.splits[_chatProcess->localSplitIndex] < 0) {
			slice = AdjustMigrateMessageIds(std::move(slice));
		}
		if (!_chatProcess->handleSlice(std::move(slice))) {
			return;
		}
		commitExportProgress(_chatProcess->committedMax, u"run"_q);
	}
	if (_chatProcess->lastSlice
		&& (++_chatProcess->localSplitIndex
			< _chatProcess->info.splits.size())) {
		_chatProcess->lastSlice = false;
		_chatProcess->walkStarted = false;
		_chatProcess->walkCursor = 1;
		_chatProcess->writtenMax = 0;
		_chatProcess->committedMax = 0;
		_chatProcess->pagePrefetch.reset();
	}
	if (!_chatProcess->lastSlice) {
		requestMessagesSlice();
	} else if (!scanAdvanceFilter()) {
		finishMessages();
	}
}

bool ApiWrap::loadMessageFileProgress(int index, FileProgress progress) {
	Expects(_fileProcess != nullptr);
	Expects(_chatProcess != nullptr);
	Expects(_chatProcess->slice.has_value());
	Expects(index >= 0 && index < _chatProcess->slice->list.size());
	Expects(_chatProcess->fileIndex == index);

	// Selected ordinal: the in-progress message is one past the
	// completed selected count. Skipped messages never trigger
	// byte progress, so they never consume ordinals.
	return _chatProcess->fileProgress(DownloadProgress{
		.randomId = _fileProcess->randomId,
		.path = _fileProcess->relativePath,
		.itemIndex = _chatProcess->hasSelectedTotal
			? (_chatProcess->selectedDone + 1)
			: index,
		.ready = progress.ready,
		.total = progress.total });
}

void ApiWrap::loadMessageFileDone(
		int index,
		Data::File *file,
		const QString &relativePath) {
	Expects(_chatProcess != nullptr);
	Expects(_chatProcess->slice.has_value());
	Expects(file != nullptr);
	Expects(index >= 0 && index < _chatProcess->slice->list.size());
	Expects(_chatProcess->fileIndex == index);
	Expects(_chatProcess->messageFileWorkMessageIndex == index);
	Expects(_chatProcess->messageFileWorkIndex >= 0);
	Expects(_chatProcess->messageFileWorkIndex
		< _chatProcess->messageFileWork.size());
	Expects(_chatProcess->messageFileWork[
		_chatProcess->messageFileWorkIndex].file == file);

	file->relativePath = relativePath;
	finishFileRecord(file);
	if (file->relativePath.isEmpty()
		&& file->skipReason == Data::File::SkipReason::None) {
		file->skipReason = Data::File::SkipReason::Unavailable;
	}
	loadNextMessageFile();
}

bool ApiWrap::loadMessageEmojiProgress(FileProgress progress) {
	Expects(_chatProcess != nullptr);

	return loadMessageFileProgress(_chatProcess->fileIndex, progress);
}

void ApiWrap::loadMessageEmojiDone(uint64 id, const QString &relativePath) {
	const auto i = _resolvedCustomEmoji.find(id);
	if (i != end(_resolvedCustomEmoji)) {
		i->second.file.relativePath = relativePath;
		if (relativePath.isEmpty()) {
			i->second.file.skipReason = Data::File::SkipReason::Unavailable;
		}
	}
	loadNextMessageFile();
}

bool ApiWrap::loadTopicEmojiProgress(FileProgress progress) {
	Expects(_topicProcess != nullptr);

	return loadTopicMessageFileProgress(
		_topicProcess->fileIndex,
		progress);
}

void ApiWrap::loadCustomEmojiDone(uint64 id, const QString &relativePath) {
	const auto i = _resolvedCustomEmoji.find(id);
	if (i != end(_resolvedCustomEmoji)) {
		i->second.file.relativePath = relativePath;
		if (relativePath.isEmpty()) {
			i->second.file.skipReason = Data::File::SkipReason::Unavailable;
		}
	}
	if (_chatProcess) {
		loadNextMessageFile();
	} else if (_topicProcess) {
		loadNextTopicMessageFile();
	}
}

void ApiWrap::finishMessages() {
	Expects(_chatProcess != nullptr);
	Expects(!_chatProcess->slice.has_value());
	_pauseRequested = false;
	_paused = false;

	commitExportProgress(_chatProcess->committedMax, u"done"_q);
	const auto process = base::take(_chatProcess);
	process->done();
}

void ApiWrap::requestTopicMessages(
		PeerId peerId,
		MTPInputPeer inputPeer,
		int32 topicRootId,
		FnMut<bool(int count)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::MessagesSlice&&)> slice,
		FnMut<void()> done) {
	Expects(_topicProcess == nullptr);
	Expects(_selfId.has_value());

	_topicProcess = std::make_unique<TopicProcess>();
	_topicProcess->context.selfPeerId = peerFromUser(*_selfId);
	_topicProcess->peerId = peerId;
	_topicProcess->inputPeer = inputPeer;
	_topicProcess->topicRootId = topicRootId;
	_topicProcess->relativePath = "chats/chat_"
		+ QString::number(peerId.value)
		+ "/topic_"
		+ QString::number(topicRootId)
		+ "/";
	_topicProcess->start = std::move(start);
	_topicProcess->fileProgress = std::move(progress);
	_topicProcess->handleSlice = std::move(slice);
	_topicProcess->done = std::move(done);

	_dedupGen++;

	mainRequest(MTPchannels_GetMessages(
		MTP_inputChannel(
			inputPeer.c_inputPeerChannel().vchannel_id(),
			inputPeer.c_inputPeerChannel().vaccess_hash()),
		MTP_vector<MTPInputMessage>(
			1,
			MTP_inputMessageID(MTP_int(topicRootId)))
	)).done([=](const MTPmessages_Messages &rootResult) {
		Expects(_topicProcess != nullptr);

		auto rootSlice = rootResult.match([&](
				const MTPDmessages_messagesNotModified &) {
			return Data::MessagesSlice();
		}, [&](const auto &data) {
			return Data::ParseMessagesSlice(
				_topicProcess->context,
				data.vmessages(),
				data.vusers(),
				data.vchats(),
				_topicProcess->relativePath);
		});

		auto rootSlicePtr = std::make_shared<Data::MessagesSlice>(
			std::move(rootSlice));

		requestTopicReplies(
			0,
			0,
			kMessagesSliceLimit,
			[=](const MTPmessages_Messages &result) {
				Expects(_topicProcess != nullptr);

				const auto count = result.match(
					[](const MTPDmessages_messages &data) {
					return int(data.vmessages().v.size());
				}, [](const MTPDmessages_messagesSlice &data) {
					return data.vcount().v;
				}, [](const MTPDmessages_channelMessages &data) {
					return data.vcount().v;
				}, [](const MTPDmessages_messagesNotModified &data) {
					return -1;
				});
				if (count < 0) {
					error("Unexpected messagesNotModified received.");
					return;
				}
				_topicProcess->totalCount = count;
				if (!_topicProcess->start(count)) {
					return;
				}

				if (!rootSlicePtr->list.empty()) {
					for (const auto &message : rootSlicePtr->list) {
						if (message.id > _topicProcess->writtenMax) {
							_topicProcess->writtenMax = message.id;
						}
					}
					startMessagesSlice(std::move(*rootSlicePtr));
					return;
				}

				requestTopicMessagesSlice();
			});
	}).send();
}

void ApiWrap::requestTopicMessagesSlice() {
	Expects(_topicProcess != nullptr);

	const auto cursor = _topicProcess->walkStarted
		? _topicProcess->walkCursor
		: int32(1);
	requestTopicReplies(
		cursor,
		-kMessagesSliceLimit,
		kMessagesSliceLimit,
		[=](const MTPmessages_Messages &result) {
		Expects(_topicProcess != nullptr);

		auto last = false;
		auto raw = 0;
		auto pageMax = int32(0);
		auto slice = Data::MessagesSlice();
		result.match([&](const MTPDmessages_messagesNotModified &data) {
			error("Unexpected messagesNotModified received.");
		}, [&](const auto &data) {
			if constexpr (MTPDmessages_messages::Is<decltype(data)>()) {
				last = true;
			}
			slice = Data::ParseMessagesSlice(
				_topicProcess->context,
				data.vmessages(),
				data.vusers(),
				data.vchats(),
				_topicProcess->relativePath);
			for (const auto &message : data.vmessages().v) {
				++raw;
				const auto id = message.match([](const auto &data) {
					return int32(data.vid().v);
				});
				if (id > pageMax) {
					pageMax = id;
				}
			}
		});
		if (!_topicProcess) {
			return;
		}
		ranges::sort(
			slice.list,
			ranges::less(),
			[](const Data::Message &message) { return message.id; });
		slice.list.erase(ranges::unique(
			slice.list,
			ranges::equal_to(),
			[](const Data::Message &message) { return message.id; }
		), end(slice.list));
		slice.list.erase(std::remove_if(
			slice.list.begin(),
			slice.list.end(),
			[&](const Data::Message &message) {
				return (message.id <= _topicProcess->writtenMax);
			}), slice.list.end());
		const auto next = (pageMax >= std::numeric_limits<int32>::max() - 1)
			? pageMax
			: (pageMax + 1);
		_topicProcess->walkStarted = true;
		if (pageMax > _topicProcess->writtenMax) {
			_topicProcess->writtenMax = pageMax;
		}
		if (!slice.list.empty()) {
			_topicProcess->walkCursor = next;
			loadTopicMessagesFiles(std::move(slice));
			return;
		}
		if (!last && raw >= kMessagesSliceLimit && next > cursor) {
			_topicProcess->walkCursor = next;
			requestTopicMessagesSlice();
			return;
		}
		loadTopicMessagesFiles(Data::MessagesSlice());
	});
}

void ApiWrap::requestTopicReplies(
		int offsetId,
		int addOffset,
		int limit,
		FnMut<void(MTPmessages_Messages&&)> done) {
	Expects(_topicProcess != nullptr);

	auto doneHandler = [=, done = std::move(done)](
			MTPmessages_Messages &&result) mutable {
		if (_topicProcess) {
			done(std::move(result));
		}
	};

	mainRequest(MTPmessages_GetReplies(
		_topicProcess->inputPeer,
		MTP_int(_topicProcess->topicRootId),
		MTP_int(offsetId),
		MTP_int(0),
		MTP_int(addOffset),
		MTP_int(limit),
		MTP_int(0),
		MTP_int(0),
		MTP_long(0)
	)).done(std::move(doneHandler)).send();
}

void ApiWrap::loadTopicMessagesFiles(Data::MessagesSlice &&slice) {
	Expects(_topicProcess != nullptr);
	Expects(!_topicProcess->slice.has_value());

	startMessagesSlice(std::move(slice));
}

void ApiWrap::resolveTopicCustomEmoji() {
	Expects(_topicProcess != nullptr);
	Expects(_topicProcess->slice.has_value());
	Expects(_topicProcess->hydrationIndex
		== _topicProcess->slice->list.size());

	if (_unresolvedCustomEmoji.empty()) {
		beginSliceWalk(true);
		return;
	}
	const auto count = std::min(
		int(_unresolvedCustomEmoji.size()),
		kMaxEmojiPerRequest);
	auto v = QVector<MTPlong>();
	v.reserve(count);
	const auto till = end(_unresolvedCustomEmoji);
	const auto from = end(_unresolvedCustomEmoji) - count;
	for (auto i = from; i != till; ++i) {
		v.push_back(MTP_long(*i));
	}
	_unresolvedCustomEmoji.erase(from, till);
	const auto finalize = [=] {
		for (const auto &id : v) {
			if (_resolvedCustomEmoji.contains(id.v)) {
				continue;
			}
			_resolvedCustomEmoji.emplace(
				id.v,
				Data::Document{
					.file = {
						.skipReason = Data::File::SkipReason::Unavailable,
					},
				});
		}
		resolveTopicCustomEmoji();
	};
	mainRequest(MTPmessages_GetCustomEmojiDocuments(
		MTP_vector<MTPlong>(v)
	)).fail([=](const MTP::Error &error) {
		LOG(("Export Error: Failed to get documents for emoji."));
		finalize();
		return true;
	}).done([=](const MTPVector<MTPDocument> &result) {
		for (const auto &entry : result.v) {
			auto document = Data::ParseDocument(
				_topicProcess->context,
				entry,
				_topicProcess->relativePath,
				TimeId());
			_resolvedCustomEmoji.emplace(document.id, std::move(document));
		}
		finalize();
	}).send();
}

void ApiWrap::loadNextTopicMessageFile() {
	Expects(_topicProcess != nullptr);
	Expects(_topicProcess->slice.has_value());
	Expects(_topicProcess->hydrationIndex
		== _topicProcess->slice->list.size());

	auto &process = *_topicProcess;
	auto &list = process.slice->list;
	while (process.fileIndex < list.size()) {
		const auto index = process.fileIndex;
		auto &message = list[index];
		if (Data::SkipMessageByDate(message, *_settings)) {
			Expects(process.messageFileWork.empty());
			Expects(process.messageFileWorkIndex == 0);
			Expects(process.messageFileWorkMessageIndex < 0);
			++process.fileIndex;
			continue;
		}
		if (!_scanMode && !messageCustomEmojiReady(message)) {
			return;
		}
		if (process.messageFileWorkMessageIndex < 0) {
			buildMessageFileWork(process, message);
		}
		Expects(process.messageFileWorkMessageIndex == index);
		while (process.messageFileWorkIndex
				< process.messageFileWork.size()) {
			const auto work = process.messageFileWork[
				process.messageFileWorkIndex];
			const auto target = work.file;
			Expects(target != nullptr);
			auto origin = Data::FileOrigin{
				.peer = process.inputPeer,
				.messageId = message.id,
			};
			origin.richMessage = work.rich;
			const auto policy = FilePolicy{
				.message = &message,
				.type = work.type,
				.controllingSize = work.controllingSize,
			.mainFile = work.main,
			};
			const auto ready = processFileLoad(
				*target,
				origin,
				[=](FileProgress value) {
					return loadTopicMessageFileProgress(index, value);
				},
				[=](const QString &path) {
					loadTopicMessageFileDone(index, target, path);
				},
				policy);
			if (!ready) {
				return;
			}
			Expects(!target->relativePath.isEmpty()
				|| target->skipReason != Data::File::SkipReason::None);
			++process.messageFileWorkIndex;
		}
		process.messageFileWork.clear();
		process.messageFileWorkIndex = 0;
		process.messageFileWorkMessageIndex = -1;
		++process.fileIndex;
	}
	finishTopicMessagesSlice();
}

void ApiWrap::finishTopicMessagesSlice() {
	Expects(_topicProcess != nullptr);
	Expects(_topicProcess->slice.has_value());
	Expects(_topicProcess->hydrationIndex
		== _topicProcess->slice->list.size());
	Expects(_topicProcess->messageFileWork.empty());
	Expects(_topicProcess->messageFileWorkIndex == 0);
	Expects(_topicProcess->messageFileWorkMessageIndex < 0);

	auto slice = *base::take(_topicProcess->slice);
	LOG(("Export Info: Topic slice done, messages: %1.").arg(slice.list.size()));
	if (!slice.list.empty()) {
		_topicProcess->processedCount += slice.list.size();
		if (!_topicProcess->handleSlice(std::move(slice))) {
			return;
		}
	}

	const auto reachedTotal = _topicProcess->totalCount > 0
		&& _topicProcess->processedCount >= _topicProcess->totalCount;

	if (!_topicProcess->lastSlice && !reachedTotal) {
		requestTopicMessagesSlice();
	} else {
		finishTopicMessages();
	}
}

bool ApiWrap::loadTopicMessageFileProgress(
		int index,
		FileProgress progress) {
	Expects(_fileProcess != nullptr);
	Expects(_topicProcess != nullptr);
	Expects(_topicProcess->slice.has_value());
	Expects(index >= 0 && index < _topicProcess->slice->list.size());
	Expects(_topicProcess->fileIndex == index);

	return _topicProcess->fileProgress(DownloadProgress{
		.randomId = _fileProcess->randomId,
		.path = _fileProcess->relativePath,
		.itemIndex = index,
		.ready = progress.ready,
		.total = progress.total });
}

void ApiWrap::loadTopicMessageFileDone(
		int index,
		Data::File *file,
		const QString &relativePath) {
	Expects(_topicProcess != nullptr);
	Expects(_topicProcess->slice.has_value());
	Expects(file != nullptr);
	Expects(index >= 0 && index < _topicProcess->slice->list.size());
	Expects(_topicProcess->fileIndex == index);
	Expects(_topicProcess->messageFileWorkMessageIndex == index);
	Expects(_topicProcess->messageFileWorkIndex >= 0);
	Expects(_topicProcess->messageFileWorkIndex
		< _topicProcess->messageFileWork.size());
	Expects(_topicProcess->messageFileWork[
		_topicProcess->messageFileWorkIndex].file == file);

	file->relativePath = relativePath;
	finishFileRecord(file);
	if (file->relativePath.isEmpty()
		&& file->skipReason == Data::File::SkipReason::None) {
		file->skipReason = Data::File::SkipReason::Unavailable;
	}
	loadNextTopicMessageFile();
}

void ApiWrap::finishTopicMessages() {
	Expects(_topicProcess != nullptr);
	Expects(!_topicProcess->slice.has_value());

	const auto process = base::take(_topicProcess);
	process->done();
}

void ApiWrap::setSessionId(uint64 sessionId) {
	_sessionId = sessionId;
}

void ApiWrap::setDedupDb(const QString &path) {
	_dedupDb = std::make_unique<::Data::DedupDb>(path, false);
}

void ApiWrap::setScanMode(bool scan) {
	_scanMode = scan;
}

std::vector<QString> ApiWrap::linkUrls() const {
	auto result = std::vector<QString>();
	result.reserve(_linkUrls.size());
	for (const auto &url : _linkUrls) {
		result.push_back(QString::fromUtf8(url));
	}
	std::sort(result.begin(), result.end());
	return result;
}

::Data::DedupDb *ApiWrap::dedupDb() const {
	return (_dedupDb && _dedupDb->isOpen()) ? _dedupDb.get() : nullptr;
}

PeerId ApiWrap::currentPeer() const {
	if (_chatProcess) {
		return _chatProcess->info.peerId;
	} else if (_topicProcess) {
		return _topicProcess->peerId;
	}
	return PeerId(0);
}

bool MainMediaId(
		const Data::Message &message,
		uint64 &docId,
		bool &isPhoto) {
	return v::match(message.media.content,
		[&](const Data::Photo &photo) {
			docId = photo.id;
			isPhoto = true;
			return true;
		},
		[&](const Data::Document &document) {
			docId = document.id;
			isPhoto = false;
			return true;
		},
		[](const auto &) {
			return false;
		});
}

bool ApiWrap::skipDuplicateById(Data::File &file, const FilePolicy &policy) {
	if (!policy.mainFile || !policy.message) {
		return false;
	}
	auto docId = uint64(0);
	auto isPhoto = false;
	if (!MainMediaId(*policy.message, docId, isPhoto) || !docId) {
		return false;
	}
	const auto db = dedupDb();
	if (!db) {
		return false;
	}
	const auto peer = currentPeer();
	_dedupPeers.emplace(peer);
	const auto known = GetEnhancedBool("prevent_export_duplicates")
		? db->containsDocId(::Data::DedupDb::Table::Downloads, docId)
		: db->containsExTmpDocId(_sessionId, peer, docId);
	if (!known) {
		return false;
	}
	file.skipReason = Data::File::SkipReason::Duplicate;
	return true;
}

void ApiWrap::recordFinishedContent(
		Data::File &file,
		const FilePolicy &policy) {
	if (!policy.mainFile || !policy.message || file.relativePath.isEmpty()) {
		return;
	}
	auto docId = uint64(0);
	auto isPhoto = false;
	if (!MainMediaId(*policy.message, docId, isPhoto) || !docId) {
		return;
	}
	const auto db = dedupDb();
	if (!db) {
		return;
	}
	const auto global = GetEnhancedBool("prevent_export_duplicates");
	const auto fullPath = _settings->path + file.relativePath;
	_dedupPeers.emplace(currentPeer());
	if (Export::FinishFile(
			*db,
			_sessionId,
			currentPeer(),
			docId,
			file.content,
			fullPath,
			file.size,
			QByteArray(),
			global,
			isPhoto)) {
		QFile::remove(fullPath);
		file.relativePath = QString();
		file.skipReason = Data::File::SkipReason::Duplicate;
		if (_stats) {
			_stats->incrementSkipped(policy.type, file.size);
		}
	} else if (_stats) {
		_stats->incrementMediaWritten(file.size);
	}
}

void ApiWrap::noteMediaWritten(const Data::File &file) {
	if (_stats && !file.relativePath.isEmpty()) {
		_stats->incrementMediaWritten(file.size);
	}
}

void ApiWrap::finishFileRecord(Data::File *file) {
	_decidingFiles.erase(file);
	const auto i = _pendingHash.find(file);
	if (i == end(_pendingHash)) {
		return;
	}
	const auto pending = i->second;
	_pendingHash.erase(i);
	_inflightDocs.remove(pending.docId);
	if (pending.docId == 0) {
		return;
	}
	const auto db = dedupDb();
	if (!db) {
		return;
	}
	const auto global = GetEnhancedBool("prevent_export_duplicates");
	if (file->relativePath.isEmpty()) {
		if (!pending.hash.isEmpty()) {
			Export::CancelFile(*db, pending.docId);
		}
		return;
	}
	const auto fullPath = _settings->path + file->relativePath;
	if (Export::FinishFile(
			*db,
			_sessionId,
			pending.peer,
			pending.docId,
			QByteArray(),
			fullPath,
			file->size,
			pending.hash,
			global,
			pending.photo)) {
		QFile::remove(fullPath);
		file->relativePath = QString();
		file->skipReason = Data::File::SkipReason::Duplicate;
		if (_stats) {
			_stats->incrementSkipped(pending.type, file->size);
		}
	} else if (_stats) {
		_stats->incrementMediaWritten(file->size);
	}
}

void ApiWrap::clearDedupRun() {
	const auto peers = base::take(_dedupPeers);
	const auto db = dedupDb();
	if (!db) {
		return;
	}
	for (const auto peer : peers) {
		db->clearExTmpRun(_sessionId, peer);
		db->removeExResume(_sessionId, peer);
	}
}

void ApiWrap::clearExTmpOnly() {
	const auto peers = base::take(_dedupPeers);
	const auto db = dedupDb();
	if (!db) {
		return;
	}
	for (const auto peer : peers) {
		db->clearExTmpRun(_sessionId, peer);
	}
}

void ApiWrap::setResumeCheckpoint(const ::Data::ExResumeRecord &record) {
	_resumeArmed = true;
	_resumeLastId = int32(record.lastId.bare);
	_resumeSplitIndex = record.splitIndex;
	_resumeSelectedDone = record.selectedDone;
	_resumeFilterIndex = record.filterIndex;
	// Only a true resume (paused/crashed run) redoes its filter
	// from the start; an update (done record) walks every filter
	// from the id cursor for new messages only.
	_resumeFilterRedo = (record.state != u"done"_q);
	_resumeDocId = record.docId;
	_resumePausedFile = record.pausedFile;
	_resumeFolder = _settings ? _settings->path : QString();
	if (_stats && !record.stats.isEmpty()) {
		_stats->restore(record.stats);
	}
}

void ApiWrap::setWriterStateGetter(Fn<Output::DialogState()> getter) {
	_writerStateGetter = std::move(getter);
}

void ApiWrap::refreshTakeoutSession(FnMut<void(uint64)> done) {
	// Circuit breaker: at most one shared re-ensure per 30s.
	// The session serializes inits; waiters fan in on the result.
	const auto now = crl::now();
	if (now - _takeoutRefreshedAt < crl::time(30000)) {
		done(0);
		return;
	}
	_takeoutRefreshedAt = now;
	_takeoutWaiters.push_back(std::move(done));
	if (_takeoutRefreshing) {
		return;
	}
	_takeoutRefreshing = true;
	_takeoutId = std::nullopt;
	if (_takeoutRefreshHook) {
		_takeoutRefreshHook();
		return;
	}
	_takeoutRefreshing = false;
	for (auto &waiter : base::take(_takeoutWaiters)) {
		waiter(0);
	}
}

void ApiWrap::takeoutRefreshDone(uint64 id) {
	_takeoutRefreshing = false;
	if (id) {
		_takeoutId = id;
	}
	for (auto &waiter : base::take(_takeoutWaiters)) {
		waiter(id);
	}
}

void ApiWrap::setUpdateCheck(int knownTotal, Fn<void(int newCount)> handler) {
	_updateMode = true;
	_updateKnownTotal = knownTotal;
	_updateCheckHandler = std::move(handler);
}

void ApiWrap::proceedUpdate() {
	_updateMode = false;
	_updateCheckHandler = nullptr;
	if (_chatProcess) {
		requestMessagesSlice();
	}
}

void ApiWrap::abortUpdate() {
	_updateMode = false;
	_updateCheckHandler = nullptr;
}

void ApiWrap::commitExportProgress(
		int32 committedMax,
		const QString &state) {
	// Resume scope: single-chat file exports. Scan and media-free
	// exports rebuild cheaply and never offer resume.
	if (_scanMode || !_chatProcess || !_settings || skipMedia()) {
		return;
	}
	const auto db = dedupDb();
	if (!db) {
		return;
	}
	const auto peer = currentPeer();
	if (!peer) {
		return;
	}
	// Flush-then-commit: the checkpoint must never outrun saved hashes.
	db->flushExTmp();
	auto total = 0;
	if (_chatProcess->hasSelectedTotal) {
		total = _chatProcess->selectedTotal;
	} else {
		for (const auto count : _chatProcess->info.messagesCountPerSplit) {
			total += count;
		}
	}
	auto skipped = int64(0);
	if (_stats) {
		for (const auto &group : Output::Stats::kGroupStats) {
			skipped += _stats->typeSkipped(group.type);
		}
	}
	auto record = ::Data::ExResumeRecord();
	record.sessionId = _sessionId;
	record.peerId = peer;
	record.splitIndex = _chatProcess->localSplitIndex;
	record.total = total;
	record.msgsDone = _stats ? int(_stats->messagesTotal()) : 0;
	record.skipped = int(skipped);
	record.selectedDone = _chatProcess->selectedDone;
	record.filterIndex = std::max(0, _chatProcess->scanFilterIndex);
	record.lastId = MsgId(committedMax);
	record.exportFolder = _settings->path;
	record.state = state;
	record.media = static_cast<uint32>(
		static_cast<std::underlying_type_t<MediaSettings::Type>>(
			_settings->media.types.value()));
	record.size = _settings->media.sizeLimit;
	record.exportFormat = static_cast<int>(_settings->format);
	record.fromDate = _settings->singlePeerFrom
		? int(*_settings->singlePeerFrom)
		: 0;
	record.tillDate = _settings->singlePeerTill
		? int(*_settings->singlePeerTill)
		: 0;
	record.useIdRange = _settings->useIdRange;
	record.fromId = _settings->singlePeerFromId.value_or(uint64(0));
	record.tillId = _settings->singlePeerTillId.value_or(uint64(0));
	record.stats = _stats ? _stats->serialize() : QByteArray();
	if (_fileProcess) {
		record.pausedFile = _fileProcess->relativePath;
		record.pausedBytes = _fileProcess->file.size();
		record.docId = _fileProcess->docId;
	}
	if (_writerStateGetter) {
		const auto state = _writerStateGetter();
		record.htmlIndex = state.messagesCount;
		record.dateIndex = state.dateMessageId;
		record.repliedIndex = state.lastIds;
		record.lastMsg = state.lastMessage;
	}
	db->insertExResume(record);
}

bool ApiWrap::skipMedia() const {
	return _settings && ((_settings->media.types & scanFileTypes()) == 0);
}

void ApiWrap::beginSliceWalk(bool topic) {
	const auto walk = [=, this] {
		if (topic) {
			loadNextTopicMessageFile();
		} else {
			loadNextMessageFile();
		}
	};
	const auto process = topic
		? static_cast<AbstractMessagesProcess*>(_topicProcess.get())
		: static_cast<AbstractMessagesProcess*>(_chatProcess.get());
	const auto db = dedupDb();
	if (!process || !process->slice || !db || !_takeoutId) {
		walk();
		return;
	}
	const auto scanSearch = _scanMode
		&& !topic
		&& _chatProcess
		&& _chatProcess->scanBySearch;
	const auto scanWalk = _scanMode && process;
	if (!scanWalk && !skipMedia() && !scanSearch) {
		walk();
		return;
	}
	if (!topic) {
		firePagePrefetch();
	}
	struct Target {
		uint64 docId = 0;
		Data::FileLocation location;
		int64 size = 0;
	};
	const auto peer = currentPeer();
	const auto global = GetEnhancedBool("prevent_export_duplicates");
	auto bigTargets = std::vector<Target>();
	auto smallTargets = std::vector<Target>();
	auto queuedDocs = base::flat_set<uint64>();
	for (const auto &message : process->slice->list) {
		if (int(bigTargets.size() + smallTargets.size())
			>= kMessagesSliceLimit) {
			break;
		}
		if (Data::SkipMessageByDate(message, *_settings)) {
			continue;
		}
		auto docId = uint64(0);
		auto location = Data::FileLocation();
		auto size = int64(0);
		auto fileType = MediaSettings::Type();
		v::match(message.media.content,
			[&](const Data::Photo &photo) {
				docId = photo.id;
				location = photo.image.file.location;
				size = photo.image.file.size;
				fileType = MediaSettings::Type::Photo;
			},
			[&](const Data::Document &document) {
				docId = document.id;
				location = document.file.location;
				size = document.file.size;
				fileType = DocumentMediaType(document);
			},
			[](const auto &) {});
		if (!docId || !location) {
			continue;
		}
		if (_scanMode) {
			const auto fullHistory = bool(_settings->media.types
				& MediaSettings::Type::FullHistory);
			const auto accepted = ((fileType & _settings->media.types)
				== fileType)
				|| (fullHistory
					&& (fileType & scanFileTypes()) == fileType);
			if (!accepted || size > _settings->media.sizeLimit) {
				continue;
			}
		}
		if (_knownFileHash.contains(docId)
			|| _knownFileContent.contains(docId)
			|| _hashFailedDocs.contains(docId)
			|| !queuedDocs.emplace(docId).second
			|| (global && db->containsDocId(
				::Data::DedupDb::Table::Downloads,
				docId))) {
			continue;
		}
		((size >= ::Data::kDedupMinPartialHashSize)
			? bigTargets
			: smallTargets).push_back({ docId, location, size });
	}
	if (bigTargets.empty() && smallTargets.empty()) {
		walk();
		return;
	}
	// Fetch the whole slice in parallel, then walk from memory.
	const auto gen = _dedupGen;
	if (scanWalk || skipMedia()) {
		const auto pending = std::make_shared<int>(
			int(bigTargets.size() + smallTargets.size()));
		const auto maybeWalk = [=, this] {
			if (--*pending > 0) {
				return;
			}
			if (gen != _dedupGen) {
				return;
			}
			walk();
		};
		for (const auto &target : bigTargets) {
			Export::FetchHash(
				_mtp,
				_runner,
				*_takeoutId,
				target.location,
				target.size,
				[=, this] {
					return (_chatProcess != nullptr)
						|| (_topicProcess != nullptr);
				},
			[=, this](QByteArray hash) {
				if (gen == _dedupGen) {
					if (!hash.isEmpty()) {
						_knownFileHash[target.docId] = hash;
					} else if (target.docId != 0) {
						_hashFailedDocs.emplace(target.docId);
					}
				}
				maybeWalk();
			},
				5,
				u"scan-big:%1:%2"_q.arg(target.size).arg(target.location.dcId),
				[=, this](FnMut<void(uint64)> done) {
					refreshTakeoutSession(std::move(done));
				});
		}
	// Small whole-file fetches run bounded-parallel with staggered
	// starts: every flood tag in six runs was small-lane, so only
	// this lane is spaced. Big chunks burst freely and never
	// flooded; spacing them serialized the drain and cost 40s.
	// Same-page doc dedupe above removes repeat-fetch bursts.
		const auto fireSmall = std::make_shared<Fn<void()>>();
		const auto smallActive = std::make_shared<int>(0);
		const auto smallIndex = std::make_shared<size_t>(0);
		const auto sharedSmall = std::make_shared<std::vector<Target>>(
			std::move(smallTargets));
		const auto smallTimer = std::make_shared<base::ConcurrentTimer>(
			_runner);
		const auto smallLastStart = std::make_shared<crl::time>(0);
		*fireSmall = [=, this] {
			while (*smallActive < kSmallHashParallel
				&& *smallIndex < sharedSmall->size()) {
				const auto now = crl::now();
				if (now - *smallLastStart < kSmallStartSpacing) {
					const auto weakFire = std::weak_ptr<Fn<void()>>(
						fireSmall);
					smallTimer->setCallback([=] {
						if (gen == _dedupGen) {
							if (const auto strong = weakFire.lock()) {
								(*strong)();
							}
						}
					});
					smallTimer->callOnce(kSmallStartSpacing
						- (now - *smallLastStart));
					return;
				}
				*smallLastStart = now;
				const auto target = (*sharedSmall)[*smallIndex];
				++*smallIndex;
				++*smallActive;
				Export::FetchFullFile(
					_mtp,
					_runner,
					*_takeoutId,
					target.location,
					target.size,
					[=, this] {
						return (_chatProcess != nullptr)
							|| (_topicProcess != nullptr);
					},
				[=, this](QByteArray content) {
					--*smallActive;
					if (gen == _dedupGen) {
						if (!content.isEmpty()) {
							_knownFileContent[target.docId] = content;
						} else if (target.docId != 0) {
							_hashFailedDocs.emplace(target.docId);
						}
					}
						if (*smallIndex < sharedSmall->size()) {
							(*fireSmall)();
						}
						maybeWalk();
					},
					5,
					u"scan-small:%1:%2"_q.arg(target.size).arg(target.location.dcId),
					[=, this](FnMut<void(uint64)> done) {
						refreshTakeoutSession(std::move(done));
					});
			}
		};
		if (!sharedSmall->empty()) {
			(*fireSmall)();
		}
		return;
	}
	walk();
}

bool ApiWrap::mainFileDuplicate(const FilePolicy &policy) {
	using SkipReason = Data::File::SkipReason;
	if (policy.mainFile || !policy.message) {
		return false;
	}
	const auto process = _chatProcess
		? static_cast<AbstractMessagesProcess*>(_chatProcess.get())
		: static_cast<AbstractMessagesProcess*>(_topicProcess.get());
	if (!process) {
		return false;
	}
	for (const auto &work : process->messageFileWork) {
		if (work.main
			&& work.file
			&& work.file->skipReason == SkipReason::Duplicate) {
			return true;
		}
	}
	return false;
}

Data::File *ApiWrap::walkParkedFile(const Data::File *file) const {
	const auto process = _topicProcess
		? static_cast<const AbstractMessagesProcess*>(
			_topicProcess.get())
		: (_chatProcess
			? static_cast<const AbstractMessagesProcess*>(
				_chatProcess.get())
			: nullptr);
	if (!process || !process->slice.has_value()) {
		return nullptr;
	}
	const auto &list = process->slice->list;
	if (process->fileIndex < 0
		|| process->fileIndex >= int(list.size())
		|| process->messageFileWorkMessageIndex != process->fileIndex
		|| process->messageFileWorkIndex < 0
		|| process->messageFileWorkIndex >= process->messageFileWork.size()
		|| process->messageFileWork[process->messageFileWorkIndex].file
			!= file) {
		return nullptr;
	}
	return process->messageFileWork[process->messageFileWorkIndex].file;
}

bool ApiWrap::decideFileScan(
		Data::File &file,
		const FilePolicy &policy,
		FnMut<void(QString)> done) {
	using SkipReason = Data::File::SkipReason;
	if (file.skipReason != SkipReason::None) {
		return true;
	}
	if (!_decidingFiles.emplace(&file).second) {
		return false;
	}
	Data::File *const filePtr = &file;
	const auto markType = [](Data::File &file) {
		file.skipReason = SkipReason::FileType;
	};
	if (!policy.mainFile || !policy.message) {
		markType(file);
		_decidingFiles.erase(filePtr);
		return true;
	}
	const auto fullHistory = bool(
		_settings->media.types & MediaSettings::Type::FullHistory);
	const auto fileTypes = scanFileTypes();
	const auto accepted = ((policy.type & _settings->media.types)
		== policy.type)
		|| (fullHistory && (policy.type & fileTypes) == policy.type);
	if (!accepted) {
		markType(file);
		_decidingFiles.erase(filePtr);
		return true;
	}
	if (policy.controllingSize > _settings->media.sizeLimit) {
		file.skipReason = SkipReason::FileSize;
		_decidingFiles.erase(filePtr);
		return true;
	}
	const auto type = policy.type;
	const auto controllingSize = policy.controllingSize;
	auto docId = uint64(0);
	auto isPhoto = false;
	const auto markNew = [this, type, controllingSize](Data::File &file) {
		file.skipReason = SkipReason::FileType;
		if (_stats) {
			_stats->incrementType(type, controllingSize);
		}
	};
	if (!MainMediaId(*policy.message, docId, isPhoto) || !docId) {
		markNew(file);
		_decidingFiles.erase(filePtr);
		return true;
	}
	const auto db = dedupDb();
	if (!db) {
		markNew(file);
		_decidingFiles.erase(filePtr);
		return true;
	}
	const auto peer = currentPeer();
	_dedupPeers.emplace(peer);
	const auto global = GetEnhancedBool("prevent_export_duplicates");
	const auto markDup = [this, type](Data::File &file) {
		file.skipReason = SkipReason::Duplicate;
		if (_stats) {
			_stats->incrementSkipped(type, file.size);
		}
	};
	if (global && db->containsDocId(
		::Data::DedupDb::Table::Downloads,
		docId)) {
		markDup(file);
		_decidingFiles.erase(filePtr);
		return true;
	}
	if (db->containsExTmpDocId(_sessionId, peer, docId)) {
		markDup(file);
		_decidingFiles.erase(filePtr);
		return true;
	}
	const auto decide = [=, this](Data::File &file, const QByteArray &hash) {
		if (!hash.isEmpty()) {
			const auto seen = db->containsExTmpHash(
				_sessionId,
				peer,
				hash);
			const auto known = seen || (global && db->containsHash(
				::Data::DedupDb::Table::Downloads,
				hash));
			if (!known) {
				db->insertExTmp(_sessionId, peer, docId, hash);
				markNew(file);
			} else {
				markDup(file);
			}
		} else {
			markNew(file);
		}
	};
	if (const auto i = _knownFileHash.find(docId);
		i != end(_knownFileHash) && !i->second.isEmpty()) {
		decide(file, i->second);
		_decidingFiles.erase(filePtr);
		return true;
	}
	if (const auto i = _knownFileContent.find(docId);
		i != end(_knownFileContent) && !i->second.isEmpty()) {
		decide(file, ::Data::ContentFingerprint(i->second));
		_decidingFiles.erase(filePtr);
		return true;
	}
	if (!file.content.isEmpty()) {
		const auto hash = ::Data::ContentFingerprint(file.content);
		if (!hash.isEmpty()) {
			_knownFileHash[docId] = hash;
		}
		decide(file, hash);
		_decidingFiles.erase(filePtr);
		return true;
	}
	if (_hashFailedDocs.contains(docId)) {
		decide(file, QByteArray());
		_decidingFiles.erase(filePtr);
		return true;
	}
	if (!file.location
		|| (file.location.dcId == 0
			&& file.location.data.type() != mtpc_inputTakeoutFileLocation)
		|| !_takeoutId) {
		markNew(file);
		_decidingFiles.erase(filePtr);
		return true;
	}
	const auto sharedDone = std::make_shared<FnMut<void(QString)>>(
		std::move(done));
	const auto gen = _dedupGen;
	const auto sliceGen = _sliceGen;
	if (file.size >= ::Data::kDedupMinPartialHashSize) {
		Export::FetchHash(
			_mtp,
			_runner,
			*_takeoutId,
			file.location,
			file.size,
			[=, this] {
				return (_chatProcess != nullptr)
					|| (_topicProcess != nullptr);
			},
			[=, this](QByteArray hash) mutable {
				_decidingFiles.erase(filePtr);
				if (gen != _dedupGen
					|| sliceGen != _sliceGen
					|| (!_chatProcess && !_topicProcess)) {
					return;
				}
				const auto live = walkParkedFile(filePtr);
				if (!live) {
					return;
				}
				if (!hash.isEmpty()) {
					_knownFileHash[docId] = hash;
				} else {
					_hashFailedDocs.emplace(docId);
				}
				decide(*live, hash);
				(*sharedDone)(QString());
			},
			kWalkHashAttempts,
			u"scan:%1:%2:%3:%4"_q.arg(int(type)).arg(file.size).arg(docId).arg(file.location.dcId),
			[=, this](FnMut<void(uint64)> done) {
				refreshTakeoutSession(std::move(done));
			});
		return false;
	}
	Export::FetchFullFile(
		_mtp,
		_runner,
		*_takeoutId,
		file.location,
		file.size,
		[=, this] {
			return (_chatProcess != nullptr)
				|| (_topicProcess != nullptr);
		},
		[=, this](QByteArray content) mutable {
			_decidingFiles.erase(filePtr);
			if (gen != _dedupGen
				|| sliceGen != _sliceGen
				|| (!_chatProcess && !_topicProcess)) {
				return;
			}
			const auto live = walkParkedFile(filePtr);
			if (!live) {
				return;
			}
			if (!content.isEmpty()) {
				_knownFileContent[docId] = content;
			} else {
				_hashFailedDocs.emplace(docId);
			}
			const auto hash = ::Data::ContentFingerprint(content);
			if (!hash.isEmpty()) {
				_knownFileHash[docId] = hash;
			}
			decide(*live, hash);
			(*sharedDone)(QString());
		},
		kWalkHashAttempts,
		u"scan:%1:%2:%3:%4"_q.arg(int(type)).arg(file.size).arg(docId).arg(file.location.dcId),
		[=, this](FnMut<void(uint64)> done) {
			refreshTakeoutSession(std::move(done));
		});
	return false;
}

bool ApiWrap::decideFileWithoutMedia(
		Data::File &file,
		const FilePolicy &policy,
		FnMut<void(QString)> done) {
	using SkipReason = Data::File::SkipReason;
	if (file.skipReason != SkipReason::None) {
		return true;
	}
	if (!_decidingFiles.emplace(&file).second) {
		return false;
	}
	Data::File *const filePtr = &file;
	const auto markType = [](Data::File &file) {
		file.skipReason = SkipReason::FileType;
	};
	if (!policy.mainFile
		|| !policy.message
		|| !(_settings->media.types & MediaSettings::Type::FullHistory)) {
		markType(file);
		_decidingFiles.erase(filePtr);
		return true;
	}
	const auto type = policy.type;
	const auto controllingSize = policy.controllingSize;
	auto docId = uint64(0);
	auto isPhoto = false;
	const auto markNew = [this, type, controllingSize](Data::File &file) {
		file.skipReason = SkipReason::FileType;
		if (_stats) {
			_stats->incrementType(type, controllingSize);
		}
	};
	if (!MainMediaId(*policy.message, docId, isPhoto) || !docId) {
		markNew(file);
		_decidingFiles.erase(filePtr);
		return true;
	}
	const auto db = dedupDb();
	if (!db) {
		markNew(file);
		_decidingFiles.erase(filePtr);
		return true;
	}
	const auto peer = currentPeer();
	_dedupPeers.emplace(peer);
	const auto global = GetEnhancedBool("prevent_export_duplicates");
	const auto markDup = [this, type](Data::File &file) {
		file.skipReason = SkipReason::Duplicate;
		if (_stats) {
			_stats->incrementSkipped(type, file.size);
		}
	};
	if (global && db->containsDocId(
		::Data::DedupDb::Table::Downloads,
		docId)) {
		markDup(file);
		_decidingFiles.erase(filePtr);
		return true;
	}
	const auto decide = [=, this](Data::File &file, const QByteArray &hash) {
		if (!hash.isEmpty()) {
			const auto seen = db->containsExTmpHash(
				_sessionId,
				peer,
				hash);
			const auto known = seen || (global && db->containsHash(
				::Data::DedupDb::Table::Downloads,
				hash));
			if (!known) {
				db->insertExTmp(_sessionId, peer, docId, hash);
				markNew(file);
			} else {
				markDup(file);
			}
		} else {
			markNew(file);
		}
	};
	if (const auto i = _knownFileHash.find(docId);
		i != end(_knownFileHash) && !i->second.isEmpty()) {
		decide(file, i->second);
		_decidingFiles.erase(filePtr);
		return true;
	}
	if (const auto i = _knownFileContent.find(docId);
		i != end(_knownFileContent) && !i->second.isEmpty()) {
		decide(file, ::Data::ContentFingerprint(i->second));
		_decidingFiles.erase(filePtr);
		return true;
	}
	if (!file.content.isEmpty()) {
		const auto hash = ::Data::ContentFingerprint(file.content);
		if (!hash.isEmpty()) {
			_knownFileHash[docId] = hash;
		}
		decide(file, hash);
		_decidingFiles.erase(filePtr);
		return true;
	}
	if (_hashFailedDocs.contains(docId)) {
		decide(file, QByteArray());
		_decidingFiles.erase(filePtr);
		return true;
	}
	if (!file.location
		|| (file.location.dcId == 0
			&& file.location.data.type() != mtpc_inputTakeoutFileLocation)
		|| !_takeoutId) {
		markNew(file);
		_decidingFiles.erase(filePtr);
		return true;
	}
	const auto sharedDone = std::make_shared<FnMut<void(QString)>>(
		std::move(done));
	const auto gen = _dedupGen;
	const auto sliceGen = _sliceGen;
	if (file.size >= ::Data::kDedupMinPartialHashSize) {
		Export::FetchHash(
			_mtp,
			_runner,
			*_takeoutId,
			file.location,
			file.size,
			[=, this] {
				return (_chatProcess != nullptr)
					|| (_topicProcess != nullptr);
			},
			[=, this](QByteArray hash) mutable {
				_decidingFiles.erase(filePtr);
				if (gen != _dedupGen
					|| sliceGen != _sliceGen
					|| (!_chatProcess && !_topicProcess)) {
					return;
				}
				const auto live = walkParkedFile(filePtr);
				if (!live) {
					return;
				}
				if (!hash.isEmpty()) {
					_knownFileHash[docId] = hash;
				} else {
					_hashFailedDocs.emplace(docId);
				}
				decide(*live, hash);
				(*sharedDone)(QString());
			},
			kWalkHashAttempts,
			u"walk:%1:%2:%3:%4"_q.arg(int(type)).arg(file.size).arg(docId).arg(file.location.dcId),
			[=, this](FnMut<void(uint64)> done) {
				refreshTakeoutSession(std::move(done));
			});
		return false;
	}
	Export::FetchFullFile(
		_mtp,
		_runner,
		*_takeoutId,
		file.location,
		file.size,
		[=, this] {
			return (_chatProcess != nullptr)
				|| (_topicProcess != nullptr);
		},
		[=, this](QByteArray content) mutable {
			_decidingFiles.erase(filePtr);
			if (gen != _dedupGen
				|| sliceGen != _sliceGen
				|| (!_chatProcess && !_topicProcess)) {
				return;
			}
			const auto live = walkParkedFile(filePtr);
			if (!live) {
				return;
			}
			if (!content.isEmpty()) {
				_knownFileContent[docId] = content;
			} else {
				_hashFailedDocs.emplace(docId);
			}
			const auto hash = ::Data::ContentFingerprint(content);
			if (!hash.isEmpty()) {
				_knownFileHash[docId] = hash;
			}
			decide(*live, hash);
			(*sharedDone)(QString());
		},
		kWalkHashAttempts,
		u"walk:%1:%2:%3:%4"_q.arg(int(type)).arg(file.size).arg(docId).arg(file.location.dcId),
		[=, this](FnMut<void(uint64)> done) {
			refreshTakeoutSession(std::move(done));
		});
	return false;
}

bool ApiWrap::decideFileWithMedia(
		Data::File &file,
		Data::FileOrigin origin,
		const FilePolicy &policy,
		Fn<bool(FileProgress)> progress,
		FnMut<void(QString)> done) {
	using SkipReason = Data::File::SkipReason;
	if (file.skipReason != SkipReason::None) {
		return true;
	}
	if (!_decidingFiles.emplace(&file).second) {
		return false;
	}
	Data::File *const filePtr = &file;
	const auto sharedProgress = std::make_shared<Fn<bool(FileProgress)>>(
		std::move(progress));
	const auto sharedDone = std::make_shared<FnMut<void(QString)>>(
		std::move(done));
	const auto startLoad = [=, this] {
		loadFile(file, origin, *sharedProgress, std::move(*sharedDone));
	};
	if (!policy.mainFile || !policy.message || !_takeoutId) {
		_decidingFiles.erase(&file);
		startLoad();
		return false;
	}
	auto docId = uint64(0);
	auto isPhoto = false;
	if (!MainMediaId(*policy.message, docId, isPhoto) || !docId) {
		_decidingFiles.erase(&file);
		startLoad();
		return false;
	}
	const auto peer = currentPeer();
	const auto global = GetEnhancedBool("prevent_export_duplicates");
	const auto db = dedupDb();
	if (!db) {
		_decidingFiles.erase(&file);
		startLoad();
		return false;
	}
	_dedupPeers.emplace(peer);
	const auto fileType = policy.type;
	if (const auto i = _knownFileHash.find(docId);
		i != end(_knownFileHash) && !i->second.isEmpty()) {
		const auto cached = Export::CheckHash(
			*db,
			_sessionId,
			peer,
			docId,
			i->second,
			global);
		if (cached.skip) {
			file.skipReason = SkipReason::Duplicate;
			if (_stats) {
				_stats->incrementSkipped(fileType, file.size);
			}
			_decidingFiles.erase(&file);
			return true;
		}
		_pendingHash[&file] = { docId, cached.hash, isPhoto, peer, fileType };
		if (global) {
			_inflightDocs.emplace(docId);
		}
		startLoad();
		return false;
	}
	const auto gen = _dedupGen;
	const auto sliceGen = _sliceGen;
	const auto sync = std::make_shared<bool>(true);
	const auto syncResult = std::make_shared<std::optional<Export::CheckResult>>();
	Export::CheckDuplicate(
		_mtp,
		_runner,
		*_takeoutId,
		*db,
		_sessionId,
		peer,
		docId,
		file.location,
		file.size,
		[=, this] {
			return (_chatProcess != nullptr)
				|| (_topicProcess != nullptr);
		},
		global,
		isPhoto,
		[=, this](Export::CheckResult result) mutable {
			_decidingFiles.erase(filePtr);
			if (*sync) {
				*syncResult = result;
				return;
			}
			if (!result.hash.isEmpty()) {
				_knownFileHash[docId] = result.hash;
			}
			if (gen != _dedupGen
				|| sliceGen != _sliceGen
				|| (!_chatProcess && !_topicProcess)) {
				return;
			}
			const auto live = walkParkedFile(filePtr);
			if (!live) {
				return;
			}
			if (result.skip) {
				live->skipReason = SkipReason::Duplicate;
				if (_stats) {
					_stats->incrementSkipped(fileType, live->size);
				}
				(*sharedDone)(QString());
				return;
			}
			_pendingHash[live] = { docId, result.hash, isPhoto, peer, fileType };
			if (!result.hash.isEmpty() && global) {
				_inflightDocs.emplace(docId);
			}
			startLoad();
		});
	*sync = false;
	if (!*syncResult) {
		return false;
	}
	if (!(*syncResult)->hash.isEmpty()) {
		_knownFileHash[docId] = (*syncResult)->hash;
	}
	if ((*syncResult)->skip) {
		file.skipReason = SkipReason::Duplicate;
		if (_stats) {
			_stats->incrementSkipped(policy.type, file.size);
		}
		return true;
	}
	_pendingHash[&file] = { docId, (*syncResult)->hash, isPhoto, peer, policy.type };
	if (!(*syncResult)->hash.isEmpty() && global) {
		_inflightDocs.emplace(docId);
	}
	startLoad();
	return false;
}

bool ApiWrap::processFileLoad(
		Data::File &file,
		const Data::FileOrigin &origin,
		Fn<bool(FileProgress)> progress,
		FnMut<void(QString)> done,
		const FilePolicy &policy) {
	using SkipReason = Data::File::SkipReason;

	Expects(_settings != nullptr);
	Expects(policy.message != nullptr);
	Expects(policy.type != MediaSettings::Type());

	if (!file.relativePath.isEmpty()
		|| file.skipReason != SkipReason::None) {
		return true;
	} else if (mainFileDuplicate(policy)) {
		file.skipReason = SkipReason::Duplicate;
		return true;
	} else if (Data::SkipMessageByDate(*policy.message, *_settings)) {
		file.skipReason = SkipReason::DateLimits;
		return true;
	} else if (!file.location && file.content.isEmpty()) {
		file.skipReason = SkipReason::Unavailable;
		return true;
	} else if (_scanMode) {
		return decideFileScan(file, policy, std::move(done));
	} else if ((_settings->media.types & policy.type) != policy.type) {
		return decideFileWithoutMedia(file, policy, std::move(done));
	} else if (policy.controllingSize > _settings->media.sizeLimit) {
		file.skipReason = SkipReason::FileSize;
		return true;
	}
	if (skipDuplicateById(file, policy)) {
		if (_stats) {
			_stats->incrementSkipped(policy.type, policy.controllingSize);
		}
		return true;
	}
	if (policy.mainFile && _stats) {
		_stats->incrementType(policy.type, policy.controllingSize);
	}
	if (writePreloadedFile(file, origin)) {
		if (!file.relativePath.isEmpty()) {
			recordFinishedContent(file, policy);
		}
		return !file.relativePath.isEmpty();
	}
	return decideFileWithMedia(
		file,
		origin,
		policy,
		std::move(progress),
		std::move(done));
}

bool ApiWrap::processFileLoad(
		Data::File &file,
		const Data::FileOrigin &origin,
		Fn<bool(FileProgress)> progress,
		FnMut<void(QString)> done,
		Data::Message *message,
		Data::Story *story) {
	using SkipReason = Data::File::SkipReason;

	if (!file.relativePath.isEmpty()
		|| file.skipReason != SkipReason::None) {
		return true;
	} else if (!file.location && file.content.isEmpty()) {
		file.skipReason = SkipReason::Unavailable;
		return true;
	} else if (writePreloadedFile(file, origin)) {
		return !file.relativePath.isEmpty();
	}

	const auto media = message
		? &message->media
		: story
		? &story->media
		: nullptr;
	const auto type = media
		? OrdinaryMediaType(*media)
		: MediaSettings::Type(0);

	const auto fullSize = message
		? message->file().size
		: story
		? story->file().size
		: file.size;
	if (message && Data::SkipMessageByDate(*message, *_settings)) {
		file.skipReason = SkipReason::DateLimits;
		return true;
	} else if (!story && (_settings->media.types & type) != type) {
		file.skipReason = SkipReason::FileType;
		return true;
	} else if (!story && fullSize > _settings->media.sizeLimit) {
		// Don't load thumbs for large files that we skip.
		file.skipReason = SkipReason::FileSize;
		return true;
	}
	loadFile(file, origin, std::move(progress), std::move(done));
	return false;
}

bool ApiWrap::writePreloadedFile(
		Data::File &file,
		const Data::FileOrigin &origin) {
	Expects(_settings != nullptr);

	using namespace Output;

	if (!file.content.isEmpty()) {
		const auto process = prepareFileProcess(file, origin);
		if (const auto result = process->file.writeBlock(file.content)) {
			file.relativePath = process->relativePath;
		} else {
			ioError(result);
		}
		return true;
	}
	return false;
}

void ApiWrap::loadFile(
		const Data::File &file,
		const Data::FileOrigin &origin,
		Fn<bool(FileProgress)> progress,
		FnMut<void(QString)> done) {
	Expects(_fileProcess == nullptr);
	Expects(file.location.dcId != 0
		|| file.location.data.type() == mtpc_inputTakeoutFileLocation);

	_fileProcess = prepareFileProcess(file, origin);
	_fileProcess->progress = std::move(progress);
	_fileProcess->done = std::move(done);

	if (_fileProcess->progress) {
		const auto progress = FileProgress{
			_fileProcess->file.size(),
			_fileProcess->size
		};
		if (!_fileProcess->progress(progress)) {
			return;
		}
	}

	loadFilePart();

	Ensures(!_fileProcess->requestIds.empty());
}

auto ApiWrap::prepareFileProcess(
	const Data::File &file,
	const Data::FileOrigin &origin)
-> std::unique_ptr<FileProcess> {
	Expects(_settings != nullptr);

	// Resume into an existing partial only with document identity:
	// generic names (AnimatedSticker.tgs and friends) collide
	// across different documents, and resuming into a foreign
	// prefix corrupts the file or dies past EOF. The checkpoint's
	// paused path is checked first (it may be renumbered, so the
	// suggested name is not compared), then this run's claimed
	// paths. Anything else renumbers fresh.
	const auto docId = LocationFileId(file.location);
	auto relativePath = file.suggestedPath;
	auto resumeOffset = int64(0);
	if (file.size > 0 && docId) {
		const auto stashed = _settings->path + _resumePausedFile;
		const auto stashedInfo = QFileInfo(stashed);
		if (_resumeDocId
			&& docId == _resumeDocId
			&& _settings->path == _resumeFolder
			&& stashedInfo.exists()
			&& stashedInfo.size() > 0
			&& stashedInfo.size() < file.size) {
			relativePath = _resumePausedFile;
			resumeOffset = stashedInfo.size();
			_resumeDocId = 0;
			_resumePausedFile.clear();
			_resumeFolder.clear();
		} else {
			const auto full = _settings->path + file.suggestedPath;
			const auto existing = QFileInfo(full);
			const auto sameRun = _preparedFileIds.contains(full)
				&& _preparedFileIds[full] == docId;
			if (existing.exists()
				&& existing.size() > 0
				&& existing.size() < file.size
				&& sameRun) {
				resumeOffset = existing.size();
			} else {
				relativePath = Output::File::PrepareRelativePath(
					_settings->path,
					file.suggestedPath);
			}
		}
	} else {
		relativePath = Output::File::PrepareRelativePath(
			_settings->path,
			file.suggestedPath);
	}
	auto result = std::make_unique<FileProcess>(
		_settings->path + relativePath,
		_stats);
	if (resumeOffset > 0) {
		result->file.resumeFrom(resumeOffset);
		result->offset = resumeOffset;
	}
	result->docId = docId;
	result->relativePath = relativePath;
	result->location = file.location;
	result->size = file.size;
	result->origin = origin;
	result->randomId = base::RandomValue<uint64>();
	if (docId) {
		_preparedFileIds[_settings->path + relativePath] = docId;
	}
	return result;
}

void ApiWrap::loadFilePart() {
	if (!_fileProcess) {
		return;
	}
	if (_pauseRequested) {
		return;
	}
	while (_fileProcess->requestIds.size() < kFileRequestsCount
		&& !(_fileProcess->size > 0
			&& _fileProcess->offset >= _fileProcess->size)) {
		const auto offset = _fileProcess->offset;
		_fileProcess->requests.push_back({ offset });
		_fileProcess->requestIds.emplace_back(offset, fileRequest(
			_fileProcess->location,
			_fileProcess->offset
		).done([=](const MTPupload_File &result) {
			if (!_fileProcess) {
				return;
			}
			auto &ids = _fileProcess->requestIds;
			ids.erase(std::remove_if(
				ids.begin(),
				ids.end(),
				[&](const auto &entry) { return (entry.first == offset); }
			), ids.end());
			filePartDone(offset, result);
		}).send());
		_fileProcess->offset += kFileChunkSize;
	}
}

void ApiWrap::filePartDone(int64 offset, const MTPupload_File &result) {
	Expects(_fileProcess != nullptr);
	Expects(!_fileProcess->requests.empty());

	if (result.type() == mtpc_upload_fileCdnRedirect) {
		error("Cdn redirect is not supported.");
		return;
	}
	const auto &data = result.c_upload_file();
	if (data.vbytes().v.isEmpty()) {
		if (_fileProcess->size > 0) {
			error("Empty bytes received in file part.");
			return;
		}
		const auto result = _fileProcess->file.writeBlock({});
		if (!result) {
			ioError(result);
			return;
		}
	} else {
		using Request = FileProcess::Request;
		auto &requests = _fileProcess->requests;
		const auto i = ranges::find(
			requests,
			offset,
			[](const Request &request) { return request.offset; });
		Assert(i != end(requests));

		i->bytes = data.vbytes().v;

		auto &file = _fileProcess->file;
		while (!requests.empty() && !requests.front().bytes.isEmpty()) {
			const auto &bytes = requests.front().bytes;
			if (const auto result = file.writeBlock(bytes); !result) {
				ioError(result);
				return;
			}
			requests.pop_front();
		}

		if (_fileProcess->progress) {
			_fileProcess->progress(FileProgress{
				file.size(),
				_fileProcess->size });
		}

		if (!requests.empty()
			|| !_fileProcess->size
			|| _fileProcess->size > _fileProcess->offset) {
			if (_pauseRequested
				&& !_paused
				&& requests.empty()
				&& (!_fileProcess->size
					|| _fileProcess->size > _fileProcess->offset)) {
				parkForPause();
				return;
			}
			loadFilePart();
			return;
		}
	}

	auto process = base::take(_fileProcess);
	process->done(process->relativePath);
}

bool ApiWrap::filePartChunkFailed(int64 offset) {
	if (!_fileProcess || offset <= 0) {
		return false;
	}
	// Stale prefix (same-name different document, or a server file
	// that shrank below its declared size): the existing file may
	// belong to another document, so keep it intact, restart under
	// a fresh name from zero. After the restart the offset is zero,
	// so a repeat failure still surfaces as fatal.
	auto &process = *_fileProcess;
	process.relativePath = Output::File::PrepareRelativePath(
		_settings->path,
		process.relativePath);
	process.file.repath(_settings->path + process.relativePath);
	process.offset = 0;
	process.requests.clear();
	for (const auto &entry : base::take(process.requestIds)) {
		_mtp.request(entry.second).cancel();
	}
	if (process.docId) {
		_preparedFileIds[_settings->path + process.relativePath]
			= process.docId;
	}
	loadFilePart();
	return true;
}

QString ApiWrap::filePartMediaFolder() const {
	if (_chatProcess) {
		return _chatProcess->info.relativePath;
	} else if (_topicProcess) {
		return _topicProcess->relativePath;
	}
	return QString();
}

void ApiWrap::filePartRetryReference(
		int64 offset,
		Data::FileLocation location) {
	Expects(_fileProcess != nullptr);
	Expects(_fileProcess->requestIds.empty());

	_fileProcess->location = std::move(location);
	_fileProcess->requestIds.push_back({ offset, fileRequest(
		_fileProcess->location,
		offset
	).done([=](const MTPupload_File &result) {
		_fileProcess->requestIds.clear();
		filePartDone(offset, result);
		}).send() });
}

void ApiWrap::filePartRefreshReference(int64 offset) {
	Expects(_fileProcess != nullptr);
	Expects(_fileProcess->requestIds.empty());

	const auto origin = _fileProcess->origin;
	if (origin.storyId) {
		_fileProcess->requestIds.push_back({ offset, mainRequest(MTPstories_GetStoriesByID(
			MTP_inputPeerSelf(),
			MTP_vector<MTPint>(1, MTP_int(origin.storyId))
		)).fail([=](const MTP::Error &error) {
			_fileProcess->requestIds.clear();
			filePartUnavailable();
			return true;
		}).done([=](const MTPstories_Stories &result) {
			_fileProcess->requestIds.clear();
			filePartExtractReference(offset, result);
		}).send() });
		return;
	}
	if (origin.customEmojiId) {
		const auto folder = filePartMediaFolder();
		_fileProcess->requestIds.push_back({ offset, mainRequest(
			MTPmessages_GetCustomEmojiDocuments(
				MTP_vector<MTPlong>(
					1,
					MTP_long(origin.customEmojiId)))
		).fail([=](const MTP::Error &) {
			_fileProcess->requestIds.clear();
			filePartUnavailable();
			return true;
		}).done([=](const MTPVector<MTPDocument> &result) {
			_fileProcess->requestIds.clear();
			filePartExtractCustomEmojiReference(
				offset,
				origin.customEmojiId,
				folder,
				result);
		}).send() });
		return;
	}
	if (origin.richMessage) {
		if (!origin.messageId) {
			filePartUnavailable();
			return;
		}
		const auto folder = filePartMediaFolder();
		_fileProcess->requestIds.push_back({ offset, mainRequest(MTPmessages_GetRichMessage(
			origin.peer,
			MTP_int(origin.messageId)
		)).fail([=](const MTP::Error &) {
			_fileProcess->requestIds.clear();
			filePartUnavailable();
			return true;
		}).done([=](const MTPmessages_Messages &result) {
			_fileProcess->requestIds.clear();
			filePartExtractRichReference(
				offset,
				origin,
				folder,
				result);
		}).send() });
		return;
	}
	if (!origin.messageId) {
		error("FILE_REFERENCE error for non-message file.");
		return;
	}
	if (origin.peer.type() == mtpc_inputPeerChannel
		|| origin.peer.type() == mtpc_inputPeerChannelFromMessage) {
		const auto channel = (origin.peer.type() == mtpc_inputPeerChannel)
			? MTP_inputChannel(
				origin.peer.c_inputPeerChannel().vchannel_id(),
				origin.peer.c_inputPeerChannel().vaccess_hash())
			: MTP_inputChannelFromMessage(
				origin.peer.c_inputPeerChannelFromMessage().vpeer(),
				origin.peer.c_inputPeerChannelFromMessage().vmsg_id(),
				origin.peer.c_inputPeerChannelFromMessage().vchannel_id());
		_fileProcess->requestIds.push_back({ offset, mainRequest(MTPchannels_GetMessages(
			channel,
			MTP_vector<MTPInputMessage>(
				1,
				MTP_inputMessageID(MTP_int(origin.messageId)))
		)).fail([=](const MTP::Error &error) {
			_fileProcess->requestIds.clear();
			filePartUnavailable();
			return true;
		}).done([=](const MTPmessages_Messages &result) {
			_fileProcess->requestIds.clear();
			filePartExtractReference(offset, result);
		}).send() });
	} else {
		_fileProcess->requestIds.push_back({ offset, splitRequest(
			origin.split,
			MTPmessages_GetMessages(
				MTP_vector<MTPInputMessage>(
					1,
					MTP_inputMessageID(MTP_int(origin.messageId)))
			)
		).fail([=](const MTP::Error &error) {
			_fileProcess->requestIds.clear();
			filePartUnavailable();
			return true;
		}).done([=](const MTPmessages_Messages &result) {
			_fileProcess->requestIds.clear();
			filePartExtractReference(offset, result);
		}).send() });
	}
}

void ApiWrap::filePartExtractCustomEmojiReference(
		int64 offset,
		uint64 customEmojiId,
		const QString &folder,
		const MTPVector<MTPDocument> &result) {
	Expects(_fileProcess != nullptr);
	Expects(_fileProcess->requestIds.empty());
	Expects(_selfId.has_value());

	auto context = Data::ParseMediaContext();
	context.selfPeerId = peerFromUser(*_selfId);
	auto document = std::optional<Data::Document>();
	auto matches = 0;
	for (const auto &entry : result.v) {
		auto parsed = Data::ParseDocument(
			context,
			entry,
			folder,
			TimeId());
		if (parsed.id != customEmojiId) {
			continue;
		}
		++matches;
		if (matches == 1) {
			document = std::move(parsed);
		}
	}
	if (matches != 1 || !document) {
		filePartUnavailable();
		return;
	}
	auto refreshed = _fileProcess->location;
	const auto mainRefreshed = Data::RefreshFileReference(
		refreshed,
		document->file.location);
	const auto thumbRefreshed = !mainRefreshed
		&& document->thumb.width > 0
		&& Data::RefreshFileReference(
			refreshed,
			document->thumb.file.location);
	if (!mainRefreshed && !thumbRefreshed) {
		filePartUnavailable();
		return;
	}
	filePartRetryReference(offset, std::move(refreshed));
}

void ApiWrap::filePartExtractRichReference(
		int64 offset,
		const Data::FileOrigin &origin,
		const QString &folder,
		const MTPmessages_Messages &result) {
	Expects(_fileProcess != nullptr);
	Expects(_fileProcess->requestIds.empty());
	Expects(_selfId.has_value());

	const auto richMessage = ExtractFullRichMessage(
		result,
		origin.messageId);
	if (!richMessage) {
		filePartUnavailable();
		return;
	}
	auto context = Data::ParseMediaContext();
	context.selfPeerId = peerFromUser(*_selfId);
	const auto parsed = Data::ParseRichMessage(
		context,
		*richMessage,
		folder,
		TimeId());
	if (parsed.part) {
		filePartUnavailable();
		return;
	}
	const auto refreshed = RefreshRichMessageFileReference(
		_fileProcess->location,
		parsed);
	if (!refreshed) {
		filePartUnavailable();
		return;
	}
	filePartRetryReference(offset, *refreshed);
}

void ApiWrap::filePartExtractReference(
		int64 offset,
		const MTPmessages_Messages &result) {
	Expects(_fileProcess != nullptr);
	Expects(_fileProcess->requestIds.empty());

	const auto folder = filePartMediaFolder();
	result.match([&](const MTPDmessages_messagesNotModified &data) {
		error("Unexpected messagesNotModified received.");
	}, [&](const auto &data) {
		Expects(_selfId.has_value());

		auto context = Data::ParseMediaContext();
		context.selfPeerId = peerFromUser(*_selfId);
		const auto messages = Data::ParseMessagesSlice(
			context,
			data.vmessages(),
			data.vusers(),
			data.vchats(),
			folder);
		for (const auto &message : messages.list) {
			if (message.id == _fileProcess->origin.messageId) {
				auto refreshed = _fileProcess->location;
				if (Data::RefreshFileReference(
						refreshed,
						message.file().location)
					|| Data::RefreshFileReference(
						refreshed,
						message.thumb().file.location)) {
					filePartRetryReference(
						offset,
						std::move(refreshed));
					return;
				}
			}
		}
		filePartUnavailable();
	});
}

void ApiWrap::filePartExtractReference(
		int64 offset,
		const MTPstories_Stories &result) {
	Expects(_fileProcess != nullptr);
	Expects(_fileProcess->requestIds.empty());

	const auto stories = Data::ParseStoriesSlice(
		result.data().vstories(),
		0);
	for (const auto &story : stories.list) {
		if (story.id == _fileProcess->origin.storyId) {
			auto refreshed = _fileProcess->location;
			if (Data::RefreshFileReference(
					refreshed,
					story.file().location)
				|| Data::RefreshFileReference(
					refreshed,
					story.thumb().file.location)) {
				filePartRetryReference(
					offset,
					std::move(refreshed));
				return;
			}
		}
	}
	filePartUnavailable();
}

void ApiWrap::filePartUnavailable() {
	Expects(_fileProcess != nullptr);
	Expects(!_fileProcess->requests.empty());

	LOG(("Export Error: File unavailable."));

	base::take(_fileProcess)->done(QString());
}

void ApiWrap::error(const MTP::Error &error) {
	_dedupGen++;
	_errors.fire_copy(error);
}

void ApiWrap::error(const QString &text) {
	error(MTP::Error(
		MTP_rpc_error(MTP_int(0), MTP_string("API_ERROR: " + text))));
}

void ApiWrap::ioError(const Output::Result &result) {
	_dedupGen++;
	_ioErrors.fire_copy(result);
}

ApiWrap::~ApiWrap() = default;

} // namespace Export