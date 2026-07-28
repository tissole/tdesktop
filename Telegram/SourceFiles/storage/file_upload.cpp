/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/file_upload.h"

#include "api/api_editing.h"
#include "api/api_send_progress.h"

#include "base/random.h"
#include "base/unixtime.h"
#include "storage/localimageloader.h"
#include "storage/file_download.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_channel.h"
#include "data/data_changes.h"
#include "ui/image/image_location_factory.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "history/history_item.h"
#include "history/history.h"
#include "history/history_item_helpers.h"
#include "core/file_location.h"
#include "core/mime_type.h"
#include "core/application.h"
#include "platform/platform_file_utilities.h"
#include "window/window_controller.h"
#include "main/main_session.h"
#include "main/main_account.h"
#include "storage/storage_account.h"
#include "apiwrap.h"
#include "lang/lang_keys.h"
#include "styles/style_layers.h"

#include "storage/serialize_common.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QFileInfo>

namespace Storage {
namespace {

// max 1mb uploaded at the same time in each session
constexpr auto kMaxUploadPerSession = 1024 * 1024;

constexpr auto kDocumentMaxPartsCountDefault = 4000;

// 32kb for tiny document ( < 1mb )
constexpr auto kDocumentUploadPartSize0 = 32 * 1024;

// 64kb for little document ( <= 32mb )
constexpr auto kDocumentUploadPartSize1 = 64 * 1024;

// 128kb for small document ( <= 375mb )
constexpr auto kDocumentUploadPartSize2 = 128 * 1024;

// 256kb for medium document ( <= 750mb )
constexpr auto kDocumentUploadPartSize3 = 256 * 1024;

// 512kb for large document ( <= 1500mb )
constexpr auto kDocumentUploadPartSize4 = 512 * 1024;

// One part each half second, if not uploaded faster.
constexpr auto kUploadRequestInterval = crl::time(250);

// How much time without upload causes additional session kill.
constexpr auto kKillSessionTimeout = 15 * crl::time(1000);

// How much wait after session kill before killing another one.
constexpr auto kWaitForNormalizeTimeout = 8 * crl::time(1000);

constexpr auto kMaxSessionsCount = 8;
constexpr auto kFastRequestThreshold = 1 * crl::time(1000);
constexpr auto kSlowRequestThreshold = 8 * crl::time(1000);

// Request is 'fast' if it was done in less than 1s and
// (it-s size + queued before size) >= 512kb.
constexpr auto kAcceptAsFastIfTotalAtLeast = 512 * 1024;

[[nodiscard]] const char *ThumbnailFormat(const QString &mime) {
	return Core::IsMimeSticker(mime) ? "WEBP" : "JPG";
}

} // namespace

struct Uploader::Entry {
	Entry(
		FullMsgId itemId,
		const std::shared_ptr<FilePrepareResult> &file,
		int startPartsSent = 0);

	void setDocSize(int64 size);
	bool setPartSize(int partSize);

	// const, but non-const for the move-assignment in the
	FullMsgId itemId;
	std::shared_ptr<FilePrepareResult> file;
	not_null<std::vector<QByteArray>*> parts;
	uint64 partsOfId = 0;

	int64 sentSize = 0;
	ushort partsSent = 0;
	ushort partsWaiting = 0;

	HashMd5 md5Hash;

	std::unique_ptr<QFile> docFile;
	int64 docSize = 0;
	int64 docSentSize = 0;
	int docPartSize = 0;
	ushort docPartsSent = 0;
	ushort docPartsCount = 0;
	ushort docPartsWaiting = 0;
};

struct Uploader::Request {
	FullMsgId itemId;
	crl::time sent = 0;
	QByteArray bytes;
	int queued = 0;
	ushort part = 0;
	uchar dcIndex = 0;
	bool docPart = false;
	bool bigPart = false;
	bool nonPremiumDelayed = false;
};

Uploader::Entry::Entry(
	FullMsgId itemId,
	const std::shared_ptr<FilePrepareResult> &file,
	int startPartsSent)
: itemId(itemId)
, file(file)
, parts((file->type == SendMediaType::Photo
	|| file->type == SendMediaType::Secure)
		? &file->fileparts
		: &file->thumbparts)
, partsOfId((file->fileId != 0)
		? file->fileId
		: ((file->type == SendMediaType::Photo
			|| file->type == SendMediaType::Secure)
		? file->id
		: file->thumbId)) {
	if (file->type == SendMediaType::File
		|| file->type == SendMediaType::ThemeFile
		|| file->type == SendMediaType::Audio
		|| file->type == SendMediaType::Round) {
		setDocSize(file->filesize);
		docPartsSent = ushort(startPartsSent);
	}
	partsSent = ushort(startPartsSent);
	if (docPartsSent > 0 && docPartSize > 0) {
		docSentSize = int64(docPartsSent) * int64(docPartSize);
	}
}

void Uploader::Entry::setDocSize(int64 size) {
	docSize = size;
	if (GetEnhancedInt("net_speed_boost") == 3) {
		setPartSize(kDocumentUploadPartSize4);
	} else if (GetEnhancedInt("net_speed_boost") == 2) {
		setPartSize(kDocumentUploadPartSize3);
	} else if (GetEnhancedInt("net_speed_boost") == 1) {
		setPartSize(kDocumentUploadPartSize2);
	} else {
		constexpr auto limit0 = 1024 * 1024;
		constexpr auto limit1 = 32 * limit0;
		if (docSize >= limit0 || !setPartSize(kDocumentUploadPartSize0)) {
			if (docSize > limit1 || !setPartSize(kDocumentUploadPartSize1)) {
				if (!setPartSize(kDocumentUploadPartSize2)) {
					if (!setPartSize(kDocumentUploadPartSize3)) {
						setPartSize(kDocumentUploadPartSize4);
					}
				}
			}
		}
	}
}

bool Uploader::Entry::setPartSize(int partSize) {
	docPartSize = partSize;
	docPartsCount = (docSize + docPartSize - 1) / docPartSize;
	return (docPartsCount <= kDocumentMaxPartsCountDefault);
}

Uploader::Uploader(not_null<ApiWrap*> api)
: _api(api)
, _nextTimer([=] { maybeSend(); })
, _stopSessionsTimer([=] { stopSessions(); }) {
	const auto session = &_api->session();
	photoReady(
	) | rpl::on_next([=](UploadedMedia &&data) {
		if (data.edit) {
			const auto item = session->data().message(data.fullId);
			Api::EditMessageWithUploadedPhoto(
				item,
				std::move(data.info),
				data.options);
		} else {
			_api->sendUploadedPhoto(
				data.fullId,
				std::move(data.info),
				data.options);
		}
	}, _lifetime);

	documentReady(
	) | rpl::on_next([=](UploadedMedia &&data) {
		if (data.edit) {
			const auto item = session->data().message(data.fullId);
			Api::EditMessageWithUploadedDocument(
				item,
				std::move(data.info),
				data.options);
		} else {
			_api->sendUploadedDocument(
				data.fullId,
				std::move(data.info),
				data.options);
		}
	}, _lifetime);

	photoProgress(
	) | rpl::on_next([=](const FullMsgId &fullId) {
		processPhotoProgress(fullId);
	}, _lifetime);

	photoFailed(
	) | rpl::on_next([=](const FullMsgId &fullId) {
		processPhotoFailed(fullId);
	}, _lifetime);

	documentProgress(
	) | rpl::on_next([=](const FullMsgId &fullId) {
		processDocumentProgress(fullId);
	}, _lifetime);

	documentFailed(
	) | rpl::on_next([=](const FullMsgId &fullId) {
		processDocumentFailed(fullId);
	}, _lifetime);

	_api->instance().nonPremiumDelayedRequests(
	) | rpl::on_next([=](mtpRequestId id) {
		const auto i = _requests.find(id);
		if (i != end(_requests)) {
			i->second.nonPremiumDelayed = true;
		}
	}, _lifetime);

}

void Uploader::finishInit() {
	const auto session = &_api->session();
	loadFinishedUploadsFromAccount();
	session->account().local().updateUploads(serializeFinishedUploads());
	session->data().itemIdChanged(
	) | rpl::on_next([=](const Data::Session::IdChange &change) {
		const auto oldFullId = FullMsgId(change.newId.peer, change.oldId);
		_finishedUploads.remove(oldFullId);
		_finishedUploads.emplace(change.newId);
		for (auto &info : _finishedUploadsList) {
			if (info.itemId == oldFullId) {
				info.itemId = change.newId;
				break;
			}
		}
		session->account().local().updateUploads(serializeFinishedUploads());
	}, _lifetime);
}

void Uploader::processPhotoProgress(FullMsgId itemId) {
	if (const auto item = session().data().message(itemId)) {
		sendProgressUpdate(item, Api::SendProgressType::UploadPhoto);
	}
}

void Uploader::processDocumentProgress(FullMsgId itemId) {
	if (const auto item = session().data().message(itemId)) {
		const auto media = item->media();
		const auto document = media ? media->document() : nullptr;
		const auto sendAction = (document && document->isVoiceMessage())
			? Api::SendProgressType::UploadVoice
			: (document && document->isVideoMessage())
			? Api::SendProgressType::UploadRound
			: Api::SendProgressType::UploadFile;
		const auto progress = (document && document->uploading())
			? ((document->uploadingData->offset * 100)
				/ document->uploadingData->size)
			: 0;
		sendProgressUpdate(item, sendAction, progress);
	}
}

void Uploader::processPhotoFailed(FullMsgId itemId) {
	if (const auto item = session().data().message(itemId)) {
		sendProgressUpdate(item, Api::SendProgressType::UploadPhoto, -1);
	}
}

void Uploader::processDocumentFailed(FullMsgId itemId) {
	if (const auto item = session().data().message(itemId)) {
		const auto media = item->media();
		const auto document = media ? media->document() : nullptr;
		const auto sendAction = (document && document->isVoiceMessage())
			? Api::SendProgressType::UploadVoice
			: (document && document->isVideoMessage())
			? Api::SendProgressType::UploadRound
			: Api::SendProgressType::UploadFile;
		sendProgressUpdate(item, sendAction, -1);
	}
}

void Uploader::sendProgressUpdate(
		not_null<HistoryItem*> item,
		Api::SendProgressType type,
		int progress) {
	const auto history = item->history();
	auto &manager = _api->session().sendProgressManager();
	manager.update(history, type, progress);
	if (const auto replyTo = item->replyToTop()) {
		if (history->peer->isMegagroup()) {
			manager.update(history, replyTo, type, progress);
		}
	} else if (history->isForum()) {
		manager.update(history, item->topicRootId(), type, progress);
	}
	_api->session().data().requestItemRepaint(item);
}

Uploader::~Uploader() {
	clear();
}

Main::Session &Uploader::session() const {
	return _api->session();
}

FullMsgId Uploader::currentUploadId() const {
	return _queue.empty() ? FullMsgId() : _queue.front().itemId;
}

void Uploader::upload(
		FullMsgId itemId,
		const std::shared_ptr<FilePrepareResult> &file,
		int resumeFromParts) {
	if (_queue.empty() && !_pausedId && _paused) {
		_paused = false;
	}
	if (file->type == SendMediaType::Photo) {
		const auto photo = session().data().processPhoto(
			file->photo,
			file->photoThumbs);
		photo->uploadingData = std::make_unique<Data::UploadState>(
			file->partssize);
	} else if (file->type == SendMediaType::File
		|| file->type == SendMediaType::ThemeFile
		|| file->type == SendMediaType::Audio
		|| file->type == SendMediaType::Round) {
		const auto document = file->thumb.isNull()
			? session().data().processDocument(file->document)
			: session().data().processDocument(
				file->document,
				Images::FromImageInMemory(
					file->thumb,
					ThumbnailFormat(file->filemime),
					file->thumbbytes));
		LOG(("Uploader::upload: document type=%1, file->thumb isNull=%2, thumb size=%3x%4"
			).arg(int(file->type)
			).arg(file->thumb.isNull() ? "yes" : "no"
			).arg(file->thumb.width()
			).arg(file->thumb.height()));
		document->uploadingData = std::make_unique<Data::UploadState>(
			document->size);
		if (const auto active = document->activeMediaView()) {
			if (!file->goodThumbnail.isNull()) {
				active->setGoodThumbnail(std::move(file->goodThumbnail));
			}
			if (!file->thumb.isNull()) {
				active->setThumbnail(file->thumb);
				const auto &thumb = active->thumbnail();
				LOG(("Uploader::upload: setThumbnail called, active thumbnail now isNull=%1"
					).arg(!thumb || thumb->isNull() ? "yes" : "no"));
			}
		}
		if (!file->goodThumbnailBytes.isEmpty()) {
			document->owner().cache().putIfEmpty(
				document->goodThumbnailCacheKey(),
				Storage::Cache::Database::TaggedValue(
					std::move(file->goodThumbnailBytes),
					Data::kImageCacheTag));
		}
		if (!file->content.isEmpty()) {
			document->setDataAndCache(file->content);
		}
		if (!file->filepath.isEmpty()
			&& !file->filepath.contains("ForwardTemp")) {
			document->setLocation(Core::FileLocation(file->filepath));
		}
		if (file->type == SendMediaType::ThemeFile) {
			document->checkWallPaperProperties();
		}
		if (file->videoCover) {
			session().data().processPhoto(
				file->videoCover->photo,
				file->videoCover->photoThumbs);
		}
	}
	_queue.push_back(Entry(itemId, file, resumeFromParts));
	saveResumeState();
	_uploadListChanges.fire(rpl::empty_value{});
	if (!_nextTimer.isActive()) {
		maybeSend();
	}
}

void Uploader::failed(FullMsgId itemId) {
	const auto i = ranges::find(_queue, itemId, &Entry::itemId);
	QString failedFilePath;
	if (i != end(_queue)) {
		const auto entry = std::move(*i);
		_queue.erase(i);
		_uploadListChanges.fire(rpl::empty_value{});
		failedFilePath = entry.file ? entry.file->filepath : QString();
		notifyFailed(entry);
	} else if (const auto coverId = _videoIdToCoverId.take(itemId)) {
		if (const auto video = _videoWaitingCover.take(*coverId)) {
			const auto document = session().data().document(video->id);
			if (document->uploading()) {
				document->status = FileUploadFailed;
			}
			_documentFailed.fire_copy(video->fullId);
		}
		failed(*coverId);
	} else if (const auto video = _videoWaitingCover.take(itemId)) {
		_videoIdToCoverId.remove(video->fullId);
		const auto document = session().data().document(video->id);
		if (document->uploading()) {
			document->status = FileUploadFailed;
		}
		_documentFailed.fire_copy(video->fullId);
	}
	if (!failedFilePath.isEmpty()) {
		clearResumeState(itemId.peer, failedFilePath);
	}
	cancelRequests(itemId);
	maybeFinishFront();

	crl::on_main(this, [=] {
		maybeSend();
	});
}

void Uploader::notifyFailed(const Entry &entry) {
	const auto type = entry.file->type;
	if (type == SendMediaType::Photo) {
		_photoFailed.fire_copy(entry.itemId);
	} else if (type == SendMediaType::File
		|| type == SendMediaType::ThemeFile
		|| type == SendMediaType::Audio
		|| type == SendMediaType::Round) {
		const auto document = session().data().document(entry.file->id);
		if (document->uploading()) {
			document->status = FileUploadFailed;
		}
		_documentFailed.fire_copy(entry.itemId);
	} else if (type == SendMediaType::Secure) {
		_secureFailed.fire_copy(entry.itemId);
	} else {
		Unexpected("Type in Uploader::failed.");
	}
}

void Uploader::stopSessions() {
	if (ranges::any_of(_sentPerDcIndex, rpl::mappers::_1 != 0)) {
		_stopSessionsTimer.callOnce(kKillSessionTimeout);
	} else {
		for (auto i = 0; i != int(_sentPerDcIndex.size()); ++i) {
			_api->instance().stopSession(MTP::uploadDcId(i));
		}
		_sentPerDcIndex.clear();
		_dcIndicesWithFastRequests.clear();
	}
}

QByteArray Uploader::readDocPart(not_null<Entry*> entry) {
	const auto checked = [&](QByteArray result) {
		if ((entry->file->type == SendMediaType::File
			|| entry->file->type == SendMediaType::ThemeFile
			|| entry->file->type == SendMediaType::Audio
			|| entry->file->type == SendMediaType::Round)
			&& entry->docSize <= kUseBigFilesFrom) {
			entry->md5Hash.feed(result.data(), result.size());
		}
		if (result.isEmpty()
			|| (result.size() > entry->docPartSize)
			|| ((result.size() < entry->docPartSize
				&& entry->docPartsSent + 1 != entry->docPartsCount))) {
			return QByteArray();
		}
		return result;
	};
	auto &content = entry->file->content;
	if (!content.isEmpty()) {
		const auto offset = entry->docPartsSent * entry->docPartSize;
		return checked(content.mid(offset, entry->docPartSize));
	} else if (!entry->docFile) {
		const auto filepath = entry->file->filepath;
		entry->docFile = std::make_unique<QFile>(filepath);
		if (!entry->docFile->open(QIODevice::ReadOnly)) {
			return QByteArray();
		}
		// A resumed upload opens the doc at the byte offset of the
		// first part we still need to (re)send.
		if (entry->docPartsSent > 0) {
			entry->docFile->seek(
				int64(entry->docPartsSent) * entry->docPartSize);
			entry->docSentSize =
				int64(entry->docPartsSent) * entry->docPartSize;
		}
	}
	return checked(entry->docFile->read(entry->docPartSize));
}

bool Uploader::canAddDcIndex() const {
	const auto count = int(_sentPerDcIndex.size());
	return (count < kMaxSessionsCount)
		&& (count == int(_dcIndicesWithFastRequests.size()));
}

std::optional<uchar> Uploader::chooseDcIndexForNextRequest(
		const base::flat_set<uchar> &used) {
	for (auto i = 0, count = int(_sentPerDcIndex.size()); i != count; ++i) {
		if (!_sentPerDcIndex[i] && !used.contains(i)) {
			return i;
		}
	}
	if (canAddDcIndex()) {
		const auto result = int(_sentPerDcIndex.size());
		_sentPerDcIndex.push_back(0);
		_dcIndicesWithFastRequests.clear();
		_latestDcIndexAdded = crl::now();

		DEBUG_LOG(("Uploader: Added dc index %1.").arg(result));
		return result;
	}
	auto result = std::optional<int>();
	for (auto i = 0, count = int(_sentPerDcIndex.size()); i != count; ++i) {
		if (!used.contains(i)
			&& (!result.has_value()
				|| _sentPerDcIndex[i] < _sentPerDcIndex[*result])) {
			result = i;
		}
	}
	return result;
}

Uploader::Entry *Uploader::chooseEntryForNextRequest() {
	if (!_pendingFromRemovedDcIndices.empty()) {
		const auto itemId = _pendingFromRemovedDcIndices.front().itemId;
		const auto i = ranges::find(_queue, itemId, &Entry::itemId);
		Assert(i != end(_queue));
		return &*i;
	}

	for (auto i = begin(_queue); i != end(_queue); ++i) {
		if (i->partsSent < i->parts->size()
			|| i->docPartsSent < i->docPartsCount) {
			return &*i;
		}
	}
	return nullptr;
}

auto Uploader::sendPart(not_null<Entry*> entry, uchar dcIndex)
-> SendResult {
	return !_pendingFromRemovedDcIndices.empty()
		? sendPendingPart(entry, dcIndex)
		: (entry->partsSent < entry->parts->size())
		? sendSlicedPart(entry, dcIndex)
		: sendDocPart(entry, dcIndex);
}

template <typename Prepared>
void Uploader::sendPreparedRequest(Prepared &&prepared, Request &&request) {
	auto &sentInSession = _sentPerDcIndex[request.dcIndex];
	const auto queued = sentInSession;
	sentInSession += int(request.bytes.size());

	const auto requestId = _api->request(
		std::move(prepared)
	).done([=](const MTPBool &result, mtpRequestId requestId) {
		partLoaded(result, requestId);
	}).fail([=](const MTP::Error &error, mtpRequestId requestId) {
		partFailed(error, requestId);
	}).toDC(MTP::uploadDcId(request.dcIndex)).send();

	request.sent = crl::now();
	request.queued = queued;
	_requests.emplace(requestId, std::move(request));
}

auto Uploader::sendPendingPart(not_null<Entry*> entry, uchar dcIndex)
-> SendResult {
	Expects(!_pendingFromRemovedDcIndices.empty());
	Expects(_pendingFromRemovedDcIndices.front().itemId == entry->itemId);

	auto request = std::move(_pendingFromRemovedDcIndices.front());
	_pendingFromRemovedDcIndices.erase(begin(_pendingFromRemovedDcIndices));

	const auto part = request.part;
	const auto bytes = request.bytes;
	request.dcIndex = dcIndex;
	if (request.bigPart) {
		sendPreparedRequest(MTPupload_SaveBigFilePart(
			MTP_long(entry->file->id),
			MTP_int(part),
			MTP_int(entry->docPartsCount),
			MTP_bytes(bytes)
		), std::move(request));
	} else {
		const auto id = request.docPart ? entry->file->id : entry->partsOfId;
		sendPreparedRequest(MTPupload_SaveFilePart(
			MTP_long(id),
			MTP_int(part),
			MTP_bytes(bytes)
		), std::move(request));
	}
	return SendResult::Success;
}

auto Uploader::sendDocPart(not_null<Entry*> entry, uchar dcIndex)
-> SendResult {
	const auto itemId = entry->itemId;
	const auto alreadySent = _sentPerDcIndex[dcIndex];
	const auto willProbablyBeSent = entry->docPartSize;
	if (alreadySent + willProbablyBeSent > kMaxUploadPerSession) {
		return SendResult::DcIndexFull;
	}

	Assert(entry->docPartsSent < entry->docPartsCount);

	const auto partBytes = readDocPart(entry);
	if (partBytes.isEmpty()) {
		failed(itemId);
		return SendResult::Failed;
	}
	const auto part = entry->docPartsSent++;
	++entry->docPartsWaiting;

	const auto send = [&](auto &&request, bool big) {
		sendPreparedRequest(std::move(request), {
			.itemId = itemId,
			.bytes = partBytes,
			.part = part,
			.dcIndex = dcIndex,
			.docPart = true,
			.bigPart = big,
		});
	};
	if (entry->docSize > kUseBigFilesFrom) {
		send(MTPupload_SaveBigFilePart(
			MTP_long(entry->file->id),
			MTP_int(part),
			MTP_int(entry->docPartsCount),
			MTP_bytes(partBytes)
		), true);
	} else {
		send(MTPupload_SaveFilePart(
			MTP_long(entry->file->id),
			MTP_int(part),
			MTP_bytes(partBytes)
		), false);
	}
	return SendResult::Success;
}

auto Uploader::sendSlicedPart(not_null<Entry*> entry, uchar dcIndex)
-> SendResult {
	const auto itemId = entry->itemId;
	const auto alreadySent = _sentPerDcIndex[dcIndex];
	const auto willBeSent = entry->parts->at(entry->partsSent).size();
	if (alreadySent + willBeSent >= kMaxUploadPerSession) {
		return SendResult::DcIndexFull;
	}

	++entry->partsWaiting;
	const auto index = entry->partsSent++;
	const auto partBytes = entry->parts->at(index);
	sendPreparedRequest(MTPupload_SaveFilePart(
		MTP_long(entry->partsOfId),
		MTP_int(index),
		MTP_bytes(partBytes)
	), {
		.itemId = itemId,
		.bytes = partBytes,
		.dcIndex = dcIndex,
	});
	return SendResult::Success;
}

void Uploader::maybeSend() {
	const auto stopping = _stopSessionsTimer.isActive();
	if (_queue.empty()) {
		if (!stopping) {
			_stopSessionsTimer.callOnce(kKillSessionTimeout);
		}
		_pausedId = FullMsgId();
		return;
	} else if (_pausedId) {
		return;
	} else if (stopping) {
		_stopSessionsTimer.cancel();
	}

	auto usedDcIndices = base::flat_set<uchar>();
	while (true) {
		const auto maybeDcIndex = chooseDcIndexForNextRequest(usedDcIndices);
		if (!maybeDcIndex.has_value()) {
			break;
		}
		const auto dcIndex = *maybeDcIndex;
		while (true) {
			const auto entry = chooseEntryForNextRequest();
			if (!entry) {
				return;
			}
			const auto result = sendPart(entry, dcIndex);
			if (result == SendResult::DcIndexFull) {
				return;
			} else if (result == SendResult::Success) {
				break;
			}
			// If this entry failed, we try the next one.
		}
		if (_sentPerDcIndex[dcIndex] >= kAcceptAsFastIfTotalAtLeast) {
			usedDcIndices.emplace(dcIndex);
		}
	}
	if (usedDcIndices.empty()) {
		_nextTimer.cancel();
	} else {
		_nextTimer.callOnce(kUploadRequestInterval);
	}
}

void Uploader::cancel(FullMsgId itemId) {
	failed(itemId);
}

void Uploader::cancelAll() {
	while (!_queue.empty()) {
		failed(_queue.front().itemId);
	}
	clear();
	unpause();
	_paused = false;
	notifyListChanged();
}

void Uploader::pause(FullMsgId itemId) {
	_pausedId = itemId;
}

void Uploader::unpause() {
	_pausedId = FullMsgId();
	maybeSend();
}

void Uploader::cancelRequests(FullMsgId itemId) {
	for (auto i = begin(_requests); i != end(_requests);) {
		if (i->second.itemId == itemId) {
			const auto bytes = int(i->second.bytes.size());
			_sentPerDcIndex[i->second.dcIndex] -= bytes;
			_api->request(i->first).cancel();
			i = _requests.erase(i);
		} else {
			++i;
		}
	}
	_pendingFromRemovedDcIndices.erase(ranges::remove(
		_pendingFromRemovedDcIndices,
		itemId,
		&Request::itemId
	), end(_pendingFromRemovedDcIndices));
}

void Uploader::cancelAllRequests() {
	for (const auto &[requestId, request] : base::take(_requests)) {
		_api->request(requestId).cancel();
	}
	ranges::fill(_sentPerDcIndex, 0);
}

void Uploader::clear() {
	_queue.clear();
	cancelAllRequests();
	stopSessions();
	_stopSessionsTimer.cancel();
}

Uploader::Request Uploader::finishRequest(mtpRequestId requestId) {
	const auto taken = _requests.take(requestId);
	Assert(taken.has_value());

	_sentPerDcIndex[taken->dcIndex] -= int(taken->bytes.size());
	return *taken;
}

void Uploader::partLoaded(const MTPBool &result, mtpRequestId requestId) {
	const auto request = finishRequest(requestId);

	const auto bytes = int(request.bytes.size());
	const auto itemId = request.itemId;

	if (mtpIsFalse(result)) { // failed to upload current file
		failed(itemId);
		return;
	}

	const auto i = ranges::find(_queue, itemId, &Entry::itemId);
	Assert(i != end(_queue));
	auto &entry = *i;

	const auto now = crl::now();
	const auto duration = now - request.sent;
	const auto fast = (duration < kFastRequestThreshold);
	const auto slowish = !fast;
	const auto slow = (duration >= kSlowRequestThreshold);

	if (slowish) {
		_dcIndicesWithFastRequests.clear();
		if (slow) {
			const auto elapsed = (now - _latestDcIndexRemoved);
			const auto remove = (elapsed >= kWaitForNormalizeTimeout);
			if (remove && _sentPerDcIndex.size() > 1) {
				DEBUG_LOG(("Uploader: Slow request, removing dc index."));
				removeDcIndex();
				_latestDcIndexRemoved = now;
			} else {
				DEBUG_LOG(("Uploader: Slow request, clear fast records."));
			}
		} else {
			DEBUG_LOG(("Uploader: Slow-ish request, clear fast records."));
		}
	} else if (request.sent > _latestDcIndexAdded
		&& (request.queued + bytes >= kAcceptAsFastIfTotalAtLeast)) {
		if (_dcIndicesWithFastRequests.emplace(request.dcIndex).second) {
			DEBUG_LOG(("Uploader: Mark %1 of %2 as fast."
				).arg(request.dcIndex
				).arg(_sentPerDcIndex.size()));
		}
	}

	if (request.docPart) {
		--entry.docPartsWaiting;
		entry.docSentSize += bytes;
	} else {
		--entry.partsWaiting;
		entry.sentSize += bytes;
	}
	saveResumeState();

	auto totalSent = int64(0);
	auto totalSize = int64(0);
	for (const auto &e : _queue) {
		totalSent += e.docSentSize + e.sentSize;
		totalSize += e.file ? e.file->filesize : 0;
	}
	_uploadProgress = UploadProgress{
		.fullId = itemId,
		.offset = totalSent,
		.size = totalSize,
		.partSize = 0,
	};

	if (entry.file->type == SendMediaType::Photo) {
			const auto photo = session().data().photo(entry.file->id);
			if (photo->uploading()) {
				photo->uploadingData->size = entry.file->partssize;
				photo->uploadingData->offset = entry.sentSize;
			}
			_photoProgress.fire_copy(itemId);
			_photoProgressInfo.fire_copy(UploadProgress{
				itemId,
				entry.sentSize,
				entry.file->partssize });
		} else if (entry.file->type == SendMediaType::File
			|| entry.file->type == SendMediaType::ThemeFile
			|| entry.file->type == SendMediaType::Audio
			|| entry.file->type == SendMediaType::Round) {
			const auto document = session().data().document(entry.file->id);
			if (document->uploading()) {
				document->uploadingData->offset = std::min(
					document->uploadingData->size,
					entry.docSentSize);
			}
			_documentProgress.fire_copy(itemId);
			_documentProgressInfo.fire_copy(UploadProgress{
				itemId,
				entry.docSentSize,
				document->size,
				entry.docPartSize });
		} else if (entry.file->type == SendMediaType::Secure) {
		_secureProgress.fire_copy({
			.fullId = itemId,
			.offset = entry.sentSize,
			.size = entry.file->partssize,
		});
	}
	if (request.nonPremiumDelayed) {
		_nonPremiumDelays.fire_copy(itemId);
	}

	if (!_queue.empty() && itemId == _queue.front().itemId) {
		maybeFinishFront();
	}
	maybeSend();
}

void Uploader::removeDcIndex() {
	Expects(_sentPerDcIndex.size() > 1);

	const auto dcIndex = int(_sentPerDcIndex.size()) - 1;
	for (auto i = begin(_requests); i != end(_requests);) {
		if (i->second.dcIndex == dcIndex) {
			const auto bytes = int(i->second.bytes.size());
			_sentPerDcIndex[dcIndex] -= bytes;
			_api->request(i->first).cancel();
			_pendingFromRemovedDcIndices.push_back(std::move(i->second));
			i = _requests.erase(i);
		} else {
			++i;
		}
	}
	Assert(_sentPerDcIndex.back() == 0);
	_sentPerDcIndex.pop_back();
	_dcIndicesWithFastRequests.remove(dcIndex);
	_api->instance().stopSession(MTP::uploadDcId(dcIndex));
	DEBUG_LOG(("Uploader: Removed dc index %1.").arg(dcIndex));
}

void Uploader::maybeFinishFront() {
	while (!_queue.empty()) {
		const auto &entry = _queue.front();
		if (entry.partsSent >= entry.parts->size()
			&& entry.docPartsSent >= entry.docPartsCount
			&& !entry.partsWaiting
			&& !entry.docPartsWaiting) {
			finishFront();
		} else {
			break;
		}
	}
}

void Uploader::finishFront() {
	Expects(!_queue.empty());

	auto entry = std::move(_queue.front());
	_queue.erase(_queue.begin());
	_uploadListChanges.fire(rpl::empty_value{});
	if (entry.file) {
		clearResumeState(entry.itemId.peer, entry.file->filepath);
	}
	_finishedUploads.emplace(entry.itemId);
	if (entry.file) {
		auto info = FinishedUpload{
			.itemId = entry.itemId,
			.filename = entry.file->filepath,
			.started = int64(base::unixtime::now()) * 1000,
		};
		_finishedUploadsList.push_back(std::move(info));
		_finishedUploadAdded.fire_copy(entry.itemId);
		session().account().local().updateUploads(serializeFinishedUploads());
	}

	const auto options = entry.file
		? entry.file->to.options
		: Api::SendOptions();
	const auto edit = entry.file &&
		entry.file->to.replaceMediaOf;
	const auto attachedStickers = entry.file
		? entry.file->attachedStickers
		: std::vector<MTPInputDocument>();
	if (entry.file->type == SendMediaType::Photo) {
		const auto photo = session().data().photo(entry.file->id);
		if (photo && photo->uploadingData) {
			photo->uploadingData = nullptr;
		}
		auto photoFilename = entry.file->filename;
		if (!photoFilename.endsWith(u".jpg"_q, Qt::CaseInsensitive)) {
			// Server has some extensions checking for inputMediaUploadedPhoto,
			// so force the extension to be .jpg anyway. It doesn't matter,
			// because the filename from inputFile is not used anywhere.
			photoFilename += u".jpg"_q;
		}
		const auto md5 = entry.file->filemd5;
		const auto file = MTP_inputFile(
			MTP_long(entry.partsOfId),
			MTP_int(entry.parts->size()),
			MTP_string(photoFilename),
			MTP_bytes(md5));
		auto ready = UploadedMedia{
			.id = entry.file->id,
			.fullId = entry.itemId,
			.info = {
				.file = file,
				.attachedStickers = attachedStickers,
			},
			.options = options,
			.edit = edit,
		};
		const auto i = _videoWaitingCover.find(entry.itemId);
		if (i != end(_videoWaitingCover)) {
			uploadCoverAsPhoto(i->second.fullId, std::move(ready));
		} else {
			_photoReady.fire(std::move(ready));
		}
		if (entry.file->filepath.contains("ForwardTemp")
			&& !entry.file->filepath.isEmpty()) {
			if (QFileInfo(entry.file->filepath).exists()) {
				QFile(entry.file->filepath).remove();
			}
		}
	} else if (entry.file->type == SendMediaType::File
		|| entry.file->type == SendMediaType::ThemeFile
		|| entry.file->type == SendMediaType::Audio
		|| entry.file->type == SendMediaType::Round) {
		const auto document = session().data().document(entry.file->id);
		if (document && document->uploadingData) {
			document->uploadingData = nullptr;
		}
		QByteArray docMd5(32, Qt::Uninitialized);
		hashMd5Hex(entry.md5Hash.result(), docMd5.data());

		const auto file = (entry.docSize > kUseBigFilesFrom)
			? MTP_inputFileBig(
				MTP_long(entry.file->id),
				MTP_int(entry.docPartsCount),
				MTP_string(entry.file->filename))
			: MTP_inputFile(
				MTP_long(entry.file->id),
				MTP_int(entry.docPartsCount),
				MTP_string(entry.file->filename),
				MTP_bytes(docMd5));
		const auto thumb = [&]() -> std::optional<MTPInputFile> {
			if (entry.parts->empty()) {
				return std::nullopt;
			}
			const auto thumbFilename = entry.file->thumbname;
			const auto thumbMd5 = entry.file->thumbmd5;
			return MTP_inputFile(
				MTP_long(entry.partsOfId),
				MTP_int(entry.parts->size()),
				MTP_string(thumbFilename),
				MTP_bytes(thumbMd5));
		}();
		auto ready = UploadedMedia{
			.id = entry.file->id,
			.fullId = entry.itemId,
			.info = {
				.file = file,
				.thumb = thumb,
				.attachedStickers = attachedStickers,
				.forceFile = entry.file->forceFile,
			},
			.options = options,
			.edit = edit,
		};
		if (entry.file->videoCover) {
			uploadVideoCover(std::move(ready), entry.file->videoCover);
		} else {
			_documentReady.fire(std::move(ready));
			if (entry.file->filepath.contains("ForwardTemp")
				&& !entry.file->filepath.isEmpty()) {
				entry.docFile.reset();
				LOG(("Uploader: delete check - path='%1', exists=%2"
					).arg(entry.file->filepath)
					.arg(QFileInfo(entry.file->filepath).exists()));
				if (QFileInfo(entry.file->filepath).exists()) {
					if (QFile(entry.file->filepath).remove()) {
						LOG(("Uploader: successfully deleted %1").arg(entry.file->filepath));
					} else {
						LOG(("Uploader: FAILED to delete %1, error=%2"
							).arg(entry.file->filepath)
							.arg(QString::number(int(GetLastError()))));
					}
				}
			}
		}
	} else if (entry.file->type == SendMediaType::Secure) {
		_secureReady.fire({
			entry.itemId,
			entry.file->id,
			int(entry.parts->size()),
		});
	}
}

void Uploader::partFailed(const MTP::Error &error, mtpRequestId requestId) {
	const auto request = finishRequest(requestId);
	const auto type = error.type();
	if (type.startsWith("FILE_PART_")) {
		const auto i = ranges::find(
			_queue,
			request.itemId,
			&Entry::itemId);
		if (i != end(_queue)) {
			i->partsSent = 0;
			i->docPartsSent = 0;
			i->sentSize = 0;
			i->docSentSize = 0;
			i->docFile = nullptr;
			saveResumeState();
			_uploadListChanges.fire(rpl::empty_value{});
			LOG(("Uploader: restarting upload from part 0 due to %1"
				).arg(type));
			if (!_nextTimer.isActive()) {
				maybeSend();
			}
			return;
		}
	}
	failed(request.itemId);
}

void Uploader::uploadVideoCover(
		UploadedMedia &&video,
		std::shared_ptr<FilePrepareResult> videoCover) {
	const auto coverId = FullMsgId(
		videoCover->to.peer,
		session().data().nextLocalMessageId());
	_videoIdToCoverId.emplace(video.fullId, coverId);
	_videoWaitingCover.emplace(coverId, std::move(video));

	upload(coverId, videoCover);
}

void Uploader::uploadCoverAsPhoto(
		FullMsgId videoId,
		UploadedMedia &&cover) {
	const auto coverId = cover.fullId;
	_api->request(MTPmessages_UploadMedia(
		MTP_flags(0),
		MTPstring(), // business_connection_id
		session().data().peer(videoId.peer)->input(),
		MTP_inputMediaUploadedPhoto(
			MTP_flags(0),
			cover.info.file,
			MTP_vector<MTPInputDocument>(0),
			MTP_int(0),
			MTPInputDocument()) // video
	)).done([=](const MTPMessageMedia &result) {
		result.match([&](const MTPDmessageMediaPhoto &data) {
			const auto photo = data.vphoto();
			if (!photo || photo->type() != mtpc_photo) {
				failed(coverId);
				return;
			}
			const auto &fields = photo->c_photo();
			if (const auto coverId = _videoIdToCoverId.take(videoId)) {
				if (auto video = _videoWaitingCover.take(*coverId)) {
					video->info.videoCover = MTP_inputPhoto(
						fields.vid(),
						fields.vaccess_hash(),
						fields.vfile_reference());
					_documentReady.fire(std::move(*video));
				}
			}
		}, [&](const auto &) {
			failed(coverId);
		});
	}).fail([=] {
		failed(coverId);
	}).send();
}

QString Uploader::resumeFilePath(PeerId peerId) const {
	const auto path = DownloadRootPath(&session());
	if (path.isEmpty()) {
		return QString();
	}
	const auto name = u"UL_"_q + QString::number(peerId.value);
	const auto root = path.endsWith('/') ? path : (path + '/');
	return root + name + u".json"_q;
}

Fn<std::optional<QByteArray>()> Uploader::serializeFinishedUploads() {
	return [this, weak = base::make_weak(this)]()
		-> std::optional<QByteArray> {
		const auto strong = weak.get();
		if (!strong) {
			return std::nullopt;
		}
		auto result = QByteArray();
		const auto count = _finishedUploadsList.size();
		const auto constant = sizeof(quint64) // itemId.peer
			+ sizeof(qint64) // itemId.msg
			+ sizeof(qint64); // started
		auto size = sizeof(qint32) // count
			+ count * constant;
		for (const auto &upload : _finishedUploadsList) {
			size += Serialize::stringSize(upload.filename);
		}
		result.reserve(size);

		auto stream = QDataStream(&result, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		stream << qint32(count);
		for (const auto &upload : _finishedUploadsList) {
			stream
				<< quint64(upload.itemId.peer.value)
				<< qint64(upload.itemId.msg.bare)
				<< qint64(upload.started)
				<< upload.filename;
		}
		stream.device()->close();

		return result;
	};
}

void Uploader::loadFinishedUploadsFromAccount() {
	const auto serialized = session().account().local().uploadsSerialized();
	if (serialized.isEmpty()) {
		return;
	}

	QDataStream stream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);

	auto count = qint32();
	stream >> count;
	if (stream.status() != QDataStream::Ok || count < 0 || count > 99'999) {
		return;
	}
	for (auto i = 0; i != count; ++i) {
		auto peerIdValue = quint64();
		auto msgIdBare = qint64();
		auto started = qint64();
		auto filename = QString();
		stream
			>> peerIdValue
			>> msgIdBare
			>> started
			>> filename;
		if (stream.status() != QDataStream::Ok) {
			return;
		}
		const auto itemId = FullMsgId(PeerId(peerIdValue), MsgId(msgIdBare));
		_finishedUploads.emplace(itemId);
		_finishedUploadsList.push_back({
			.itemId = itemId,
			.filename = filename,
			.started = started,
		});
	}
}

void Uploader::saveResumeState() {
	base::flat_map<PeerId, QJsonArray> byPeer;
	for (const auto &entry : _queue) {
		if (!entry.file) {
			continue;
		}
		auto obj = QJsonObject();
		obj[u"file_path"_q] = entry.file->filepath;
		obj[u"file_size"_q] = qlonglong(entry.file->filesize);
		obj[u"parts_sent"_q] = int(entry.partsSent - entry.partsWaiting);
		obj[u"doc_parts_sent"_q] = int(entry.docPartsSent - entry.docPartsWaiting);
		obj[u"sent_size"_q] = qlonglong(entry.docSentSize);
		obj[u"peer_id"_q] = QString::number(entry.itemId.peer.value);
		obj[u"file_id"_q] = QString::number(entry.file->id);
		const auto topicRootId = entry.file->to.replyTo.topicRootId;
		LOG(("RESUME_SAVE: peer=%1 fileId=%2 topicRootId=%3"
			).arg(entry.itemId.peer.value
			).arg(entry.file->id
			).arg(topicRootId.bare));
		if (topicRootId) {
			obj[u"topic_root_id"_q] = QString::number(topicRootId.bare);
		}
		byPeer[entry.itemId.peer].append(obj);
	}
	for (const auto &[peerId, entries] : byPeer) {
		const auto path = resumeFilePath(peerId);
		if (path.isEmpty()) {
			continue;
		}
		QDir().mkpath(QFileInfo(path).absolutePath());
		auto doc = QJsonObject();
		doc[u"entries"_q] = entries;
		doc[u"paused"_q] = _paused;
		auto file = QFile(path);
		if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			file.write(QJsonDocument(doc).toJson(QJsonDocument::Compact));
		}
	}
}

void Uploader::clearResumeState(PeerId peerId, const QString &filePath) {
	const auto path = resumeFilePath(peerId);
	if (path.isEmpty() || !QFile(path).exists()) {
		return;
	}
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto doc = QJsonDocument::fromJson(file.readAll()).object();
	auto entries = doc[u"entries"_q].toArray();
	auto newEntries = QJsonArray();
	for (const auto &val : entries) {
		const auto obj = val.toObject();
		if (obj[u"file_path"_q].toString() == filePath) {
			continue;
		}
		newEntries.append(obj);
	}
	auto newDoc = QJsonObject();
	newDoc[u"entries"_q] = newEntries;
	file.close();
	if (newEntries.empty()) {
		(void)QFile(path).remove();
	} else {
		(void)file.open(QIODevice::WriteOnly | QIODevice::Truncate);
		file.write(QJsonDocument(newDoc).toJson(QJsonDocument::Compact));
	}
}

bool Uploader::hasUnfinishedResume() const {
	const auto root = DownloadRootPath(&session());
	if (root.isEmpty()) {
		return false;
	}
	const auto dir = QDir(root);
	const auto files = dir.entryList(
		QStringList(u"UL_*"_q),
		QDir::Files,
		QDir::Name);
	for (const auto &file : files) {
		const auto path = dir.absoluteFilePath(file);
		auto f = QFile(path);
		if (!f.open(QIODevice::ReadOnly)) {
			continue;
		}
		const auto doc = QJsonDocument::fromJson(f.readAll()).object();
		if (!doc[u"entries"_q].toArray().empty()) {
			return true;
		}
	}
	return false;
}

int Uploader::pendingResumeCount() const {
	const auto root = DownloadRootPath(&session());
	if (root.isEmpty()) {
		return 0;
	}
	const auto dir = QDir(root);
	const auto files = dir.entryList(
		QStringList(u"UL_*"_q),
		QDir::Files,
		QDir::Name);
	auto count = 0;
	for (const auto &file : files) {
		const auto path = dir.absoluteFilePath(file);
		auto f = QFile(path);
		if (!f.open(QIODevice::ReadOnly)) {
			continue;
		}
		const auto doc = QJsonDocument::fromJson(f.readAll()).object();
		const auto entries = doc[u"entries"_q].toArray();
		for (const auto &val : entries) {
			const auto obj = val.toObject();
			const auto filePath = obj[u"file_path"_q].toString();
			if (QFileInfo::exists(filePath)) {
				++count;
			}
		}
	}
	return count;
}

void Uploader::showResumeUnfinished() {
	const auto root = DownloadRootPath(&session());
	if (root.isEmpty()) {
		return;
	}
	const auto dir = QDir(root);
	const auto files = dir.entryList(
		QStringList(u"UL_*"_q),
		QDir::Files,
		QDir::Name);
	auto toResume = QVector<Uploader::ResumeEntry>();
	for (const auto &file : files) {
		const auto path = dir.absoluteFilePath(file);
		auto f = QFile(path);
		if (!f.open(QIODevice::ReadOnly)) {
			continue;
		}
		const auto doc = QJsonDocument::fromJson(f.readAll()).object();
		const auto entries = doc[u"entries"_q].toArray();
		for (const auto &val : entries) {
			const auto obj = val.toObject();
			auto entry = Uploader::ResumeEntry();
			entry.filePath = obj[u"file_path"_q].toString();
			entry.fileSize = obj[u"file_size"_q].toVariant().toLongLong();
			const auto partsSent = ushort(obj[u"parts_sent"_q].toVariant().toUInt());
			const auto docPartsSent = ushort(obj[u"doc_parts_sent"_q].toVariant().toUInt());
			entry.partsSent = std::max(partsSent, docPartsSent);
			entry.sentSize = obj[u"sent_size"_q].toVariant().toLongLong();
			entry.peerId = PeerId(PeerIdHelper(obj[u"peer_id"_q].toVariant().toULongLong()));
			const auto fileIdVal = obj[u"file_id"_q];
			if (fileIdVal.isString()) {
				entry.fileId = fileIdVal.toString().toULongLong();
			} else {
				entry.fileId = 0;
				entry.partsSent = 0;
			}
			const auto topicRootIdVal = obj[u"topic_root_id"_q];
			if (topicRootIdVal.isString()) {
				entry.topicRootId = MsgId(topicRootIdVal.toString().toLongLong());
			}
			if (QFileInfo::exists(entry.filePath)) {
				toResume.push_back(entry);
				LOG(("RESUME_LOAD: peer=%1 file=%2 topicRootId=%3"
					).arg(entry.peerId.value
					).arg(entry.filePath
					).arg(entry.topicRootId.bare));
			}
		}
	}
	if (toResume.empty()) {
		return;
	}
	const auto window = Core::App().windowFor(
		not_null(&session().account()));
	if (!window) {
		return;
	}
	const auto resumeAll = [=]() {
		for (const auto &entry : toResume) {
			const auto newId = FullMsgId(
				entry.peerId,
				session().data().nextLocalMessageId());
			const auto fileId = entry.fileId
				? entry.fileId
				: base::RandomValue<uint64>();
			auto file = MakePreparedFile(FilePrepareDescriptor{
				kEmptyTaskId,
				fileId,
				SendMediaType::File,
				FileLoadTo(
					entry.peerId,
					Api::SendOptions(),
					FullReplyTo{ .topicRootId = entry.topicRootId },
					MsgId()),
			});
			file->filepath = entry.filePath;
			file->filesize = entry.fileSize;
			file->filename = QFileInfo(entry.filePath).fileName();
			file->filemime = Core::MimeTypeForFile(
				QFileInfo(entry.filePath)).name();
			const auto now = int(base::unixtime::now());
			const auto docProto = MTP_document(
				MTP_flags(0),
				MTP_long(fileId),
				MTP_long(0),
				MTP_bytes(),
				MTP_int(now),
				MTP_string(file->filemime),
				MTP_long(file->filesize),
				MTP_vector<MTPPhotoSize>(),
				MTPVector<MTPVideoSize>(),
				MTP_int(0),
				MTP_vector<MTPDocumentAttribute>({
					MTP_documentAttributeFilename(
						MTP_string(file->filename)),
				}));
			file->document = MTPDocument(docProto);

		const auto peer = session().data().peer(entry.peerId);
		if (peer) {
			const auto history = session().data().history(peer);
			const auto doc = session().data().processDocument(
				file->document);
			doc->uploadingData = std::make_unique<Data::UploadState>(
				doc->size);
			auto flags = NewMessageFlags(peer);
		const auto channel = peer->asBroadcast();
		if (channel) {
			flags |= MessageFlag::Post;
			flags |= MessageFlag::HasViews;
			if (channel->addsSignature()) {
				flags |= MessageFlag::HasPostAuthor;
			}
		}
			if (!peer->amAnonymous()) {
				flags |= MessageFlag::HasFromId;
			}
			if (entry.topicRootId) {
				flags |= MessageFlag::HasReplyInfo;
			}
			const auto media = MTP_messageMediaDocument(
				MTP_flags(MTPDmessageMediaDocument::Flag::f_document),
				file->document,
				MTPVector<MTPDocument>(),
				MTPPhoto(),
				MTPint(),
				MTPint());
			LOG(("RESUME_MSG: peer=%1 topicRootId=%2 creating local msg"
				).arg(entry.peerId.value
				).arg(entry.topicRootId.bare));
			history->addNewLocalMessage({
				.id = newId.msg,
				.flags = flags,
				.from = session().userPeerId(),
				.replyTo = FullReplyTo{
					.messageId = FullMsgId(
						entry.peerId,
						entry.topicRootId),
					.topicRootId = entry.topicRootId,
				},
				.date = base::unixtime::now(),
			}, TextWithEntities{}, media);
			session().data().sendHistoryChangeNotifications();
			session().changes().historyUpdated(
				history,
				Data::HistoryUpdate::Flag::MessageSent);
		}
		upload(newId, file, entry.partsSent);
		if (peer) {
			const auto newDoc = session().data().document(file->id);
			if (newDoc && newDoc->uploading()) {
				newDoc->uploadingData->offset = std::min(
					newDoc->uploadingData->size,
					entry.sentSize);
			}
		}
		LOG(("Uploader: resuming upload for %1").arg(entry.filePath));
	}
	const auto root = DownloadRootPath(&session());
		if (!root.isEmpty()) {
			const auto dir = QDir(root);
			const auto files = dir.entryList(
				QStringList(u"UL_*"_q),
				QDir::Files,
				QDir::Name);
			for (const auto &file : files) {
				(void)QFile(dir.absoluteFilePath(file)).remove();
			}
		}
	};
	auto box = Box([=](not_null<Ui::GenericBox*> box) {
		box->setCloseByOutsideClick(false);
		box->setCloseByEscape(false);
		auto text = tr::lng_upload_resume_multiple(
			tr::now,
			lt_count, int(toResume.size()));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box.get(),
			text,
			st::boxLabel));
		box->addButton(tr::lng_upload_resume_cancel(), [=] {
			box->closeBox();
			auto cancelBox = Box([=](not_null<Ui::GenericBox*> cancelBox) {
				cancelBox->setCloseByOutsideClick(false);
				cancelBox->setCloseByEscape(false);
				cancelBox->addRow(object_ptr<Ui::FlatLabel>(
					cancelBox.get(),
					tr::lng_upload_cancel_confirm(tr::now),
					st::boxLabel));
				cancelBox->addButton(tr::lng_upload_cancel_yes(), [=] {
					cancelBox->closeBox();
					crl::on_main([=] {
						const auto root = DownloadRootPath(&session());
						if (!root.isEmpty()) {
							const auto dir = QDir(root);
							const auto files = dir.entryList(
								QStringList(u"UL_*"_q),
								QDir::Files,
								QDir::Name);
							for (const auto &file : files) {
								(void)QFile(dir.absoluteFilePath(file)).remove();
							}
						}
						notifyListChanged();
					});
				});
				cancelBox->addButton(tr::lng_upload_cancel_no(), [=] {
					cancelBox->closeBox();
					crl::on_main([=] { notifyListChanged(); });
				});
			});
			window->show(std::move(cancelBox));
		}, st::attentionBoxButton);
		box->addButton(tr::lng_upload_resume_later(), [=] {
			box->closeBox();
			crl::on_main([=] {
				_paused = true;
				_pausedId = FullMsgId(
					PeerId(1),
					session().data().nextLocalMessageId());
				resumeEntriesFromDisk();
			});
		});
		box->addButton(tr::lng_upload_resume_yes(), [=] {
			box->closeBox();
			crl::on_main([=] {
				resumeAll();
				notifyListChanged();
			});
		});
	});
	window->show(std::move(box));
	window->activate();
}

void Uploader::showQuitUnfinished(not_null<Window::Controller*> window, Fn<void()> quit) {
	if (!anyUploads()) {
		if (quit) {
			quit();
		}
		return;
	}
	auto count = 0;
	for (const auto &entry : _queue) {
		if (entry.file && !entry.file->filepath.isEmpty()) {
			++count;
		}
	}
	if (!count) {
		if (quit) {
			quit();
		}
		return;
	}
	auto box = Box([=](not_null<Ui::GenericBox*> box) {
		box->setCloseByOutsideClick(false);
		box->setCloseByEscape(false);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box.get(),
			tr::lng_upload_quit_unfinished(
				tr::now,
				lt_count, count),
			st::boxLabel));
		box->addButton(tr::lng_upload_quit_cancel(), [=] {
			box->closeBox();
			auto confirmBox = Box([=](not_null<Ui::GenericBox*> confirmBox) {
				confirmBox->setCloseByOutsideClick(false);
				confirmBox->setCloseByEscape(false);
				confirmBox->addRow(object_ptr<Ui::FlatLabel>(
					confirmBox.get(),
					tr::lng_upload_cancel_confirm(tr::now),
					st::boxLabel));
				confirmBox->addButton(tr::lng_upload_cancel_yes(), [=] {
					confirmBox->closeBox();
					crl::on_main([=] {
						cancelAll();
						const auto root = DownloadRootPath(&session());
						if (!root.isEmpty()) {
							const auto dir = QDir(root);
							const auto files = dir.entryList(
								QStringList(u"UL_*"_q),
								QDir::Files,
								QDir::Name);
							for (const auto &file : files) {
								(void)QFile(dir.absoluteFilePath(file)).remove();
							}
						}
						if (quit) {
							quit();
						}
					});
				});
				confirmBox->addButton(tr::lng_upload_cancel_no(), [=] {
					confirmBox->closeBox();
				});
			});
			window->show(std::move(confirmBox));
		}, st::attentionBoxButton);
		box->addButton(tr::lng_upload_quit_pause(), [=] {
			box->closeBox();
			pauseAllUploads();
			Core::Quit();
		});
		box->addButton(tr::lng_upload_quit_continue(), [=] {
			box->closeBox();
		});
	});
	window->show(std::move(box));
	window->activate();
}

rpl::producer<> Uploader::loadingListChanges() const {
	return _uploadListChanges.events();
}

void Uploader::notifyListChanged() {
	_uploadListChanges.fire(rpl::empty_value{});
}

rpl::producer<> Uploader::finishedUploadsCleared() const {
	return _finishedUploadsCleared.events();
}

rpl::producer<UploadProgress> Uploader::uploadProgressValue() const {
	return _uploadProgress.value();
}

bool Uploader::anyUploads() const {
	return !_queue.empty();
}

bool Uploader::anyUploadsPaused() const {
	return !!_pausedId;
}

bool Uploader::isPaused() const {
	return _paused;
}

int Uploader::queueSize() const {
	return int(_queue.size());
}

bool Uploader::wasUploaded(FullMsgId itemId) const {
	return _finishedUploads.contains(itemId);
}

int Uploader::anyFinishedUploads() const {
	return int(_finishedUploads.size());
}

void Uploader::clearFinishedUploads() {
	_finishedUploads.clear();
	_finishedUploadsList.clear();
	_finishedUploadsCleared.fire(rpl::empty_value{});
	notifyListChanged();
	session().account().local().updateUploads(serializeFinishedUploads());
}

rpl::producer<FullMsgId> Uploader::finishedUploadAdded() const {
	return _finishedUploadAdded.events();
}

rpl::producer<FullMsgId> Uploader::finishedUploadRemoved() const {
	return _finishedUploadRemoved.events();
}

bool Uploader::allFinished() const {
	return _queue.empty() && !_finishedUploads.empty();
}

const std::vector<Uploader::FinishedUpload> &Uploader::finishedUploadList() const {
	return _finishedUploadsList;
}

void Uploader::removeFinishedUpload(FullMsgId itemId) {
	_finishedUploads.erase(itemId);
	_finishedUploadsList.erase(
		ranges::remove(_finishedUploadsList, itemId, &FinishedUpload::itemId),
		end(_finishedUploadsList));
	_finishedUploadRemoved.fire_copy(itemId);
	notifyListChanged();
	session().account().local().updateUploads(serializeFinishedUploads());
}

void Uploader::deleteFinishedUpload(FullMsgId itemId) {
	QString filenameToRemove;
	for (const auto &entry : _finishedUploadsList) {
		if (entry.itemId == itemId) {
			filenameToRemove = entry.filename;
			break;
		}
	}
	if (!filenameToRemove.isEmpty()
		&& QFileInfo::exists(filenameToRemove)) {
		Platform::File::MoveToTrash(filenameToRemove);
	}
	removeFinishedUpload(itemId);
}

void Uploader::deleteAllFinishedUploads() {
	for (const auto &entry : _finishedUploadsList) {
		if (!entry.filename.isEmpty()
			&& QFileInfo::exists(entry.filename)) {
			Platform::File::MoveToTrash(entry.filename);
		}
	}
	clearFinishedUploads();
}

std::vector<Uploader::UiUploadInfo> Uploader::activeUploads() const {
	auto result = std::vector<UiUploadInfo>();
	result.reserve(_queue.size());
	for (const auto &entry : _queue) {
		auto info = UiUploadInfo();
		info.itemId = entry.itemId;
		if (entry.file) {
			info.filename = entry.file->filepath;
			info.total = entry.file->filesize;
			info.offset = entry.docSentSize + entry.sentSize;
		}
		info.paused = (entry.itemId == _pausedId);
		result.push_back(std::move(info));
	}
	return result;
}

std::vector<Uploader::UiPendingUpload> Uploader::pendingUploads() const {
	auto result = std::vector<UiPendingUpload>();
	const auto root = DownloadRootPath(&session());
	if (root.isEmpty()) {
		return result;
	}
	const auto dir = QDir(root);
	const auto files = dir.entryList(
		QStringList(u"UL_*"_q),
		QDir::Files,
		QDir::Name);
	for (const auto &file : files) {
		const auto path = dir.absoluteFilePath(file);
		auto f = QFile(path);
		if (!f.open(QIODevice::ReadOnly)) {
			continue;
		}
		const auto doc = QJsonDocument::fromJson(f.readAll()).object();
		const auto entries = doc[u"entries"_q].toArray();
		for (const auto &val : entries) {
			const auto obj = val.toObject();
			const auto filePath = obj[u"file_path"_q].toString();
			if (!QFileInfo::exists(filePath)) {
				continue;
			}
			auto info = UiPendingUpload();
			info.filename = QFileInfo(filePath).fileName();
			info.total = obj[u"file_size"_q].toVariant().toLongLong();
			info.sent = obj[u"sent_size"_q].toVariant().toLongLong();
			info.itemId = FullMsgId(
				PeerId(PeerIdHelper(obj[u"peer_id"_q].toVariant().toULongLong())),
				MsgId(0));
			result.push_back(std::move(info));
		}
	}
	return result;
}

QString Uploader::firstUploadName() const {
	if (_queue.empty()) {
		return QString();
	}
	const auto &file = _queue.front().file;
	if (!file) {
		return QString();
	}
	return QFileInfo(file->filepath).fileName();
}

QString Uploader::firstPendingUploadName() const {
	const auto root = DownloadRootPath(&session());
	if (root.isEmpty()) {
		return QString();
	}
	const auto dir = QDir(root);
	const auto files = dir.entryList(
		QStringList(u"UL_*"_q),
		QDir::Files,
		QDir::Name);
	for (const auto &file : files) {
		const auto path = dir.absoluteFilePath(file);
		auto f = QFile(path);
		if (!f.open(QIODevice::ReadOnly)) {
			continue;
		}
		const auto doc = QJsonDocument::fromJson(f.readAll()).object();
		const auto entries = doc[u"entries"_q].toArray();
		for (const auto &val : entries) {
			const auto obj = val.toObject();
			const auto filePath = obj[u"file_path"_q].toString();
			if (QFileInfo::exists(filePath)) {
				return QFileInfo(filePath).fileName();
			}
		}
	}
	return QString();
}

void Uploader::pauseAllUploads() {
	for (const auto &entry : _queue) {
		pause(entry.itemId);
	}
	_paused = true;
	saveResumeState();
	notifyListChanged();
}

void Uploader::resumeAllUploads() {
	_paused = false;
	saveResumeState();
	if (_queue.empty()) {
		resumeEntriesFromDisk();
	} else {
		unpause();
	}
}

void Uploader::resumeEntriesFromDisk() {
	const auto root = DownloadRootPath(&session());
	if (root.isEmpty()) {
		return;
	}
	const auto dir = QDir(root);
	const auto files = dir.entryList(
		QStringList(u"UL_*"_q),
		QDir::Files,
		QDir::Name);
	auto toResume = QVector<Uploader::ResumeEntry>();
	for (const auto &file : files) {
		const auto path = dir.absoluteFilePath(file);
		auto f = QFile(path);
		if (!f.open(QIODevice::ReadOnly)) {
			continue;
		}
		const auto doc = QJsonDocument::fromJson(f.readAll()).object();
		const auto entries = doc[u"entries"_q].toArray();
		for (const auto &val : entries) {
			const auto obj = val.toObject();
			auto entry = Uploader::ResumeEntry();
			entry.filePath = obj[u"file_path"_q].toString();
			entry.fileSize = obj[u"file_size"_q].toVariant().toLongLong();
			const auto partsSent = ushort(obj[u"parts_sent"_q].toVariant().toUInt());
			const auto docPartsSent = ushort(obj[u"doc_parts_sent"_q].toVariant().toUInt());
			entry.partsSent = std::max(partsSent, docPartsSent);
			entry.sentSize = obj[u"sent_size"_q].toVariant().toLongLong();
			entry.peerId = PeerId(PeerIdHelper(obj[u"peer_id"_q].toVariant().toULongLong()));
			const auto fileIdVal = obj[u"file_id"_q];
			if (fileIdVal.isString()) {
				entry.fileId = fileIdVal.toString().toULongLong();
			} else {
				entry.fileId = 0;
				entry.partsSent = 0;
			}
			const auto topicRootIdVal = obj[u"topic_root_id"_q];
			if (topicRootIdVal.isString()) {
				entry.topicRootId = MsgId(topicRootIdVal.toString().toLongLong());
			}
			if (QFileInfo::exists(entry.filePath)) {
				toResume.push_back(entry);
				LOG(("RESUME_LOAD: peer=%1 file=%2 topicRootId=%3"
					).arg(entry.peerId.value
					).arg(entry.filePath
					).arg(entry.topicRootId.bare));
			}
		}
	}
	if (toResume.empty()) {
		return;
	}
	for (const auto &entry : toResume) {
		const auto newId = FullMsgId(
			entry.peerId,
			session().data().nextLocalMessageId());
		const auto fileId = entry.fileId
			? entry.fileId
			: base::RandomValue<uint64>();
		auto file = MakePreparedFile(FilePrepareDescriptor{
			kEmptyTaskId,
			fileId,
			SendMediaType::File,
			FileLoadTo(
				entry.peerId,
				Api::SendOptions(),
				FullReplyTo{ .topicRootId = entry.topicRootId },
				MsgId()),
		});
		file->filepath = entry.filePath;
		file->filesize = entry.fileSize;
		file->filename = QFileInfo(entry.filePath).fileName();
		file->filemime = Core::MimeTypeForFile(
				QFileInfo(entry.filePath)).name();
		const auto now = int(base::unixtime::now());
		const auto docProto = MTP_document(
			MTP_flags(0),
			MTP_long(fileId),
			MTP_long(0),
			MTP_bytes(),
			MTP_int(now),
			MTP_string(file->filemime),
			MTP_long(file->filesize),
			MTP_vector<MTPPhotoSize>(),
			MTPVector<MTPVideoSize>(),
			MTP_int(0),
			MTP_vector<MTPDocumentAttribute>({
				MTP_documentAttributeFilename(
					MTP_string(file->filename)),
			}));
		file->document = MTPDocument(docProto);

		const auto peer = session().data().peer(entry.peerId);
		if (peer) {
			const auto history = session().data().history(peer);
			const auto doc = session().data().processDocument(
				file->document);
			doc->uploadingData = std::make_unique<Data::UploadState>(
				doc->size);
			auto flags = NewMessageFlags(peer);
		const auto channel = peer->asBroadcast();
		if (channel) {
			flags |= MessageFlag::Post;
			flags |= MessageFlag::HasViews;
			if (channel->addsSignature()) {
				flags |= MessageFlag::HasPostAuthor;
			}
		}
			if (!peer->amAnonymous()) {
				flags |= MessageFlag::HasFromId;
			}
			if (entry.topicRootId) {
				flags |= MessageFlag::HasReplyInfo;
			}
			const auto media = MTP_messageMediaDocument(
				MTP_flags(MTPDmessageMediaDocument::Flag::f_document),
				file->document,
				MTPVector<MTPDocument>(),
				MTPPhoto(),
				MTPint(),
				MTPint());
		LOG(("RESUME_MSG(disk): peer=%1 topicRootId=%2 creating local msg"
			).arg(entry.peerId.value
			).arg(entry.topicRootId.bare));
		history->addNewLocalMessage({
			.id = newId.msg,
			.flags = flags,
			.from = session().userPeerId(),
			.replyTo = FullReplyTo{
				.messageId = FullMsgId(
					entry.peerId,
					entry.topicRootId),
				.topicRootId = entry.topicRootId,
			},
			.date = base::unixtime::now(),
		}, TextWithEntities{}, media);
		session().data().sendHistoryChangeNotifications();
		session().changes().historyUpdated(
			history,
			Data::HistoryUpdate::Flag::MessageSent);
		}
		upload(newId, file, entry.partsSent);
		if (!_queue.empty() && entry.sentSize > 0) {
			_queue.back().docSentSize = entry.sentSize;
		}
		if (peer) {
			const auto newDoc = session().data().document(file->id);
			if (newDoc && newDoc->uploading()) {
				newDoc->uploadingData->offset = std::min(
					newDoc->uploadingData->size,
					entry.sentSize);
			}
		}
		LOG(("Uploader: resuming upload for %1").arg(entry.filePath));
	}
	if (!_paused) {
		for (const auto &file : files) {
			(void)QFile(dir.absoluteFilePath(file)).remove();
		}
	}
	auto totalSent = int64(0);
	auto totalSize = int64(0);
	for (const auto &e : _queue) {
		totalSent += e.docSentSize + e.sentSize;
		totalSize += e.file ? e.file->filesize : 0;
	}
	_uploadProgress = UploadProgress{
		.fullId = FullMsgId(),
		.offset = totalSent,
		.size = totalSize,
		.partSize = 0,
	};
	notifyListChanged();
}

} // namespace Storage
