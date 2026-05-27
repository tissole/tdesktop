/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "storage/localimageloader.h"

#include "api/api_text_entities.h"
#include "api/api_sending.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "base/unixtime.h"
#include "base/random.h"
#include "editor/scene/scene_item_sticker.h"
#include "editor/scene/scene.h"
#include "media/audio/media_audio.h"
#include "media/clip/media_clip_reader.h"
#include "mtproto/facade.h"
#include "lottie/lottie_animation.h"
#include "history/history.h"
#include "history/history_item.h"
#include "boxes/abstract_box.h"
#include "boxes/send_files_box.h"
#include "boxes/premium_limits_box.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/image/image_prepare.h"
#include "lang/lang_keys.h"
#include "storage/file_download.h"
#include "storage/storage_media_prepare.h"
#include "window/themes/window_theme_preview.h"
#include "mainwidget.h"
#include "mainwindow.h"
#include "main/main_session.h"
#include "base/zlib_help.h"

#include <QtCore/QBuffer>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtGui/QImageWriter>

#include <cstdlib>

namespace {

constexpr auto kThumbnailQuality = 100;
constexpr auto kThumbnailSize = 320;
constexpr auto kPhotoUploadPartSize = 32 * 1024;
constexpr auto kRecompressAfterBpp = 16;

using Ui::ValidateThumbDimensions;

struct PreparedFileThumbnail {
	uint64 id = 0;
	QString name;
	QImage image;
	QByteArray bytes;
	MTPPhotoSize mtpSize = MTP_photoSizeEmpty(MTP_string());
};

[[nodiscard]] PreparedFileThumbnail PrepareFileThumbnail(QImage &&original) {
	const auto width = original.width();
	const auto height = original.height();
	if (!ValidateThumbDimensions(width, height)) {
		return {};
	}
	auto result = PreparedFileThumbnail();
	result.id = base::RandomValue<uint64>();
	const auto scaled = (width > kThumbnailSize || height > kThumbnailSize);
	const auto scaledWidth = [&] {
		return (width > height)
			? kThumbnailSize
			: int(base::SafeRound(kThumbnailSize * width / float64(height)));
	};
	const auto scaledHeight = [&] {
		return (width > height)
			? int(base::SafeRound(kThumbnailSize * height / float64(width)))
			: kThumbnailSize;
	};
	result.image = scaled
		? original.scaled(
			scaledWidth(),
			scaledHeight(),
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation)
		: std::move(original);
	result.mtpSize = MTP_photoSize(
		MTP_string(),
		MTP_int(result.image.width()),
		MTP_int(result.image.height()),
		MTP_int(0));
	return result;
}

[[nodiscard]] bool FileThumbnailUploadRequired(
		const QString &filemime,
		int64 filesize) {
	constexpr auto kThumbnailUploadBySize = 5 * int64(1024 * 1024);
	const auto kThumbnailKnownMimes = {
		"image/jpeg",
		"image/gif",
		"image/png",
		"image/webp",
		"video/mp4",
	};
	return (filesize > kThumbnailUploadBySize)
		|| (ranges::find(kThumbnailKnownMimes, filemime.toLower())
			== end(kThumbnailKnownMimes));
}

[[nodiscard]] PreparedFileThumbnail FinalizeFileThumbnail(
		PreparedFileThumbnail &&prepared,
		const QString &filemime,
		int64 filesize,
		bool isSticker) {
	prepared.name = isSticker ? u"thumb.webp"_q : u"thumb.jpg"_q;
	if (FileThumbnailUploadRequired(filemime, filesize)) {
		const auto format = isSticker ? "WEBP" : "JPG";
		auto buffer = QBuffer(&prepared.bytes);
		prepared.image.save(&buffer, format, kThumbnailQuality);
	}
	return std::move(prepared);
}

[[nodiscard]] auto FindAlbumItem(
		std::vector<SendingAlbum::Item> &items,
		not_null<HistoryItem*> item) {
	const auto result = ranges::find(
		items,
		item->fullId(),
		&SendingAlbum::Item::msgId);

	Ensures(result != end(items));
	return result;
}

[[nodiscard]] MTPInputSingleMedia PrepareAlbumItemMedia(
		not_null<HistoryItem*> item,
		const MTPInputMedia &media,
		uint64 randomId) {
	auto caption = item->originalText();
	TextUtilities::Trim(caption);
	auto sentEntities = Api::EntitiesToMTP(
		&item->history()->session(),
		caption.entities,
		Api::ConvertOption::SkipLocal);
	const auto flags = !sentEntities.v.isEmpty()
		? MTPDinputSingleMedia::Flag::f_entities
		: MTPDinputSingleMedia::Flag(0);

	return MTP_inputSingleMedia(
		MTP_flags(flags),
		media,
		MTP_long(randomId),
		MTP_string(caption.text),
		sentEntities);
}

[[nodiscard]] std::vector<not_null<DocumentData*>> ExtractStickersFromScene(
		not_null<const Ui::PreparedFileInformation::Image*> info) {
	const auto allItems = info->modifications.paint->items();

	return ranges::views::all(
		allItems
	) | ranges::views::filter([](const Editor::Scene::ItemPtr &i) {
		return i->isVisible() && (i->type() == Editor::ItemSticker::Type);
	}) | ranges::views::transform([](const Editor::Scene::ItemPtr &i) {
		return static_cast<Editor::ItemSticker*>(i.get())->sticker();
	}) | ranges::to_vector;
}

[[nodiscard]] QByteArray ComputePhotoJpegBytes(
		QImage &full,
		const QByteArray &bytes,
		const QByteArray &format) {
	if (!bytes.isEmpty()
		&& (bytes.size()
			<= full.width() * full.height() * kRecompressAfterBpp / 8)
		&& (format == u"jpeg"_q)) {
		if (!Images::IsProgressiveJpeg(bytes)) {
			if (const auto result = Images::MakeProgressiveJpeg(bytes)
				; !result.isEmpty()) {
				return result;
			}
		} else {
			return bytes;
		}
	}

	auto result = QByteArray();
	QBuffer buffer(&result);
	QImageWriter writer(&buffer, "JPEG");
	writer.setQuality(100);
	writer.setProgressiveScanWrite(true);
	writer.write(full);
	buffer.close();

	return result;
}

} // namespace

int PhotoSideLimit(bool large) {
	return large ? 2560 : 1280;
}

int PhotoSideLimit() {
	return PhotoSideLimit(
		Core::App().settings().sendFilesWay().sendLargePhotos());
}

TaskQueue::TaskQueue(crl::time stopTimeoutMs) {
	if (stopTimeoutMs > 0) {
		_stopTimer = new QTimer(this);
		connect(_stopTimer, SIGNAL(timeout()), this, SLOT(stop()));
		_stopTimer->setSingleShot(true);
		_stopTimer->setInterval(int(stopTimeoutMs));
	}
}

TaskId TaskQueue::addTask(std::unique_ptr<Task> &&task) {
	const auto result = task->id();
	{
		QMutexLocker lock(&_tasksToProcessMutex);
		_tasksToProcess.push_back(std::move(task));
	}

	wakeThread();

	return result;
}

void TaskQueue::addTasks(std::vector<std::unique_ptr<Task>> &&tasks) {
	{
		QMutexLocker lock(&_tasksToProcessMutex);
		for (auto &task : tasks) {
			_tasksToProcess.push_back(std::move(task));
		}
	}

	wakeThread();
}

void TaskQueue::wakeThread() {
	if (!_thread) {
		_thread = new QThread();

		_worker = new TaskQueueWorker(this);
		_worker->moveToThread(_thread);

		connect(this, SIGNAL(taskAdded()), _worker, SLOT(onTaskAdded()));
		connect(_worker, SIGNAL(taskProcessed()), this, SLOT(onTaskProcessed()));

		_thread->start();
	}
	if (_stopTimer) _stopTimer->stop();
	taskAdded();
}

void TaskQueue::cancelTask(TaskId id) {
	const auto removeFrom = [&](std::deque<std::unique_ptr<Task>> &queue) {
		const auto proj = [](const std::unique_ptr<Task> &task) {
			return task->id();
		};
		auto i = ranges::find(queue, id, proj);
		if (i != queue.end()) {
			queue.erase(i);
		}
	};
	{
		QMutexLocker lock(&_tasksToProcessMutex);
		removeFrom(_tasksToProcess);
		if (_taskInProcessId == id) {
			_taskInProcessId = TaskId();
		}
	}
	QMutexLocker lock(&_tasksToFinishMutex);
	removeFrom(_tasksToFinish);
}

void TaskQueue::onTaskProcessed() {
	do {
		auto task = std::unique_ptr<Task>();
		{
			QMutexLocker lock(&_tasksToFinishMutex);
			if (_tasksToFinish.empty()) break;
			task = std::move(_tasksToFinish.front());
			_tasksToFinish.pop_front();
		}
		task->finish();
	} while (true);

	if (_stopTimer) {
		QMutexLocker lock(&_tasksToProcessMutex);
		if (_tasksToProcess.empty() && !_taskInProcessId) {
			_stopTimer->start();
		}
	}
}

void TaskQueue::stop() {
	if (_thread) {
		_thread->requestInterruption();
		_thread->quit();
		LOG(("Waiting for taskThread to finish"));
		_thread->wait();
		delete base::take(_worker);
		delete base::take(_thread);
	}
	_tasksToProcess.clear();
	_tasksToFinish.clear();
	_taskInProcessId = TaskId();
}

TaskQueue::~TaskQueue() {
	stop();
	delete _stopTimer;
}

void TaskQueueWorker::onTaskAdded() {
	if (_inTaskAdded) return;
	_inTaskAdded = true;

	bool someTasksLeft = false;
	do {
		auto task = std::unique_ptr<Task>();
		{
			QMutexLocker lock(&_queue->_tasksToProcessMutex);
			if (!_queue->_tasksToProcess.empty()) {
				task = std::move(_queue->_tasksToProcess.front());
				_queue->_tasksToProcess.pop_front();
				_queue->_taskInProcessId = task->id();
			}
		}

		if (task) {
			task->process();
			bool emitTaskProcessed = false;
			{
				QMutexLocker lockToProcess(&_queue->_tasksToProcessMutex);
				if (_queue->_taskInProcessId == task->id()) {
					_queue->_taskInProcessId = TaskId();
					someTasksLeft = !_queue->_tasksToProcess.empty();

					QMutexLocker lockToFinish(&_queue->_tasksToFinishMutex);
					emitTaskProcessed = _queue->_tasksToFinish.empty();
					_queue->_tasksToFinish.push_back(std::move(task));
				}
			}
			if (emitTaskProcessed) {
				taskProcessed();
			}
		}
		QCoreApplication::processEvents();
	} while (someTasksLeft && !thread()->isInterruptionRequested());

	_inTaskAdded = false;
}

SendingAlbum::SendingAlbum() : groupId(base::RandomValue<uint64>()) {
}

void SendingAlbum::fillMedia(
		not_null<HistoryItem*> item,
		const MTPInputMedia &media,
		uint64 randomId) {
	const auto i = FindAlbumItem(items, item);
	Assert(!i->media);

	i->randomId = randomId;
	i->media = PrepareAlbumItemMedia(item, media, randomId);
}

void SendingAlbum::refreshMediaCaption(not_null<HistoryItem*> item) {
	const auto i = FindAlbumItem(items, item);
	if (!i->media) {
		return;
	}
	i->media = i->media->match([&](const MTPDinputSingleMedia &data) {
		return PrepareAlbumItemMedia(
			item,
			data.vmedia(),
			data.vrandom_id().v);
	});
}

void SendingAlbum::removeItem(not_null<HistoryItem*> item) {
	const auto localId = item->fullId();
	const auto i = ranges::find(items, localId, &Item::msgId);
	const auto moveCaption = (items.size() > 1) && (i == begin(items));
	Assert(i != end(items));
	items.erase(i);
	if (moveCaption) {
		auto caption = item->originalText();
		const auto firstId = items.front().msgId;
		if (const auto first = item->history()->owner().message(firstId)) {
			// We don't need to finishEdition() here, because the whole
			// album will be rebuilt after one item was removed from it.
			auto firstCaption = first->originalText();
			first->setText(firstCaption.text.isEmpty()
				? std::move(caption)
				: firstCaption.append('\n').append(std::move(caption)));
			refreshMediaCaption(first);
		}
	}
}

SendingAlbum::Item::Item(TaskId taskId)
: taskId(taskId) {
}

FilePrepareResult::FilePrepareResult(FilePrepareDescriptor &&descriptor)
: taskId(descriptor.taskId)
, id(descriptor.id)
, to(std::move(descriptor.to))
, album(std::move(descriptor.album))
, type(descriptor.type)
, caption(std::move(descriptor.caption))
, spoiler(descriptor.spoiler) {
}

void FilePrepareResult::setFileData(const QByteArray &filedata) {
	if (filedata.isEmpty()) {
		partssize = 0;
	} else {
		partssize = filedata.size();
		fileparts.reserve(
			(partssize + kPhotoUploadPartSize - 1) / kPhotoUploadPartSize);
		for (int32 i = 0, part = 0; i < partssize; i += kPhotoUploadPartSize, ++part) {
			fileparts.push_back(filedata.mid(i, kPhotoUploadPartSize));
		}
		filemd5.resize(32);
		hashMd5Hex(filedata.constData(), filedata.size(), filemd5.data());
	}
}

void FilePrepareResult::setThumbData(const QByteArray &thumbdata) {
	if (!thumbdata.isEmpty()) {
		thumbbytes = thumbdata;
		int32 size = thumbdata.size();
		thumbparts.reserve(
			(size + kPhotoUploadPartSize - 1) / kPhotoUploadPartSize);
		for (int32 i = 0, part = 0; i < size; i += kPhotoUploadPartSize, ++part) {
			thumbparts.push_back(thumbdata.mid(i, kPhotoUploadPartSize));
		}
		thumbmd5.resize(32);
		hashMd5Hex(thumbdata.constData(), thumbdata.size(), thumbmd5.data());
	}
}

std::shared_ptr<FilePrepareResult> MakePreparedFile(
		FilePrepareDescriptor &&descriptor) {
	return std::make_shared<FilePrepareResult>(std::move(descriptor));
}

FileLoadTask::FileLoadTask(Args &&args)
: _id(args.idOverride ? args.idOverride : base::RandomValue<uint64>())
, _session(args.session)
, _dcId(args.session->mainDcId())
, _to(std::move(args.to))
, _album(std::move(args.album))
, _filepath(std::move(args.filepath))
, _displayName(std::move(args.displayName))
, _content(std::move(args.content))
, _videoCover(std::move(args.videoCover))
, _information(std::move(args.information))
, _type(args.type)
, _caption(std::move(args.caption))
, _spoiler(args.spoiler)
, _forceFile(args.forceFile)
, _sendLargePhotos(args.sendLargePhotos) {
	Expects(_to.options.scheduled
		|| _to.options.shortcutId
		|| !_to.replaceMediaOf
		|| IsServerMsgId(_to.replaceMediaOf));
}

FileLoadTask::FileLoadTask(VoiceArgs &&args)
: _id(base::RandomValue<uint64>())
, _session(args.session)
, _dcId(args.session->mainDcId())
, _to(std::move(args.to))
, _content(std::move(args.voice))
, _duration(args.duration)
, _waveform(std::move(args.waveform))
, _type(args.video ? SendMediaType::Round : SendMediaType::Audio)
, _caption(std::move(args.caption)) {
}

FileLoadTask::~FileLoadTask() = default;

auto FileLoadTask::ReadMediaInformation(
	const QString &filepath,
	const QByteArray &content,
	const QString &filemime)
-> std::unique_ptr<Ui::PreparedFileInformation> {
	auto result = std::make_unique<Ui::PreparedFileInformation>();
	result->filemime = filemime;

	if (CheckForSong(filepath, content, result)) {
		return result;
	} else if (CheckForVideo(filepath, content, result)) {
		return result;
	} else if (CheckForImage(filepath, content, result)) {
		return result;
	} else if (CheckForDocument(filepath, content, result)) {
		return result;
	}
	if (v::is<v::null_t>(result->media)) {
		auto check = Media::Player::PrepareForSending(
			filepath,
			content);
		auto &song = v::get<Ui::PreparedFileInformation::Song>(
			check.media);
		if (song.duration >= 0) {
			if (!ValidateThumbDimensions(
				song.cover.width(),
				song.cover.height())) {
				song.cover = QImage();
			}
			result->media = std::move(song);
			result->filemime = u"audio/mp4"_q;
		}
	}
	return result;
}

template <typename Mimes, typename Extensions>
bool FileLoadTask::CheckMimeOrExtensions(
		const QString &filepath,
		const QString &filemime,
		Mimes &mimes,
		Extensions &extensions) {
	if (std::find(std::begin(mimes), std::end(mimes), filemime) != std::end(mimes)) {
		return true;
	}
	if (std::find_if(std::begin(extensions), std::end(extensions), [&filepath](auto &extension) {
		return filepath.endsWith(extension, Qt::CaseInsensitive);
	}) != std::end(extensions)) {
		return true;
	}
	return false;
}

bool FileLoadTask::CheckForSong(
		const QString &filepath,
		const QByteArray &content,
		std::unique_ptr<Ui::PreparedFileInformation> &result) {
	static const auto mimes = {
		u"audio/aac"_q,
        u"audio/ac3"_q,
		u"audio/ac4"_q,
		u"audio/als"_q,
        u"audio/ape"_q,
		u"audio/atrac"_q,
		u"audio/amr-wb"_q,
		u"audio/amr"_q,		
		u"audio/flac"_q,
		u"audio/gsm"_q,
		u"audio/mp3"_q,
		u"audio/mp4"_q,
		u"audio/m4a"_q,
		u"audio/m4b"_q,
		u"audio/midi"_q,
		u"audio/x-monkeys-audio"_q,
		u"audio/x-musepack"_q,
		u"audio/musepack"_q,		
		u"audio/ogg"_q,
		u"audio/opus"_q,
		u"audio/vnd.rn-realaudio"_q,
		u"audio/x-pn-realaudio"_q,
		u"audio/x-aiff"_q,
		u"audio/aiff"_q,
		u"audio/x-caf"_q,
		u"audio/x-dff"_q,
		u"audio/x-dsd"_q,
		u"audio/x-dsf"_q,
        u"audio/x-matroska"_q,
        u"audio/x-m4a"_q,
		u"audio/x-ms-wma"_q,
		u"audio/x-tta"_q,
		u"audio/x-wavpack"_q,
		u"audio/x-tak"_q,
        u"audio/vorbis"_q,
        u"audio/vnd.dts"_q,
        u"audio/vnd.dts.hd"_q,
		u"audio/wav"_q,
        u"audio/webm"_q,
        u"audio/x-wav"_q,
        u"audio/vnd.wave"_q,
        u"audio/wave"_q,
	};
	static const auto extensions = {
		u".aac"_q,
		u".aiff"_q,
		u".aifc"_q,
		u".aif"_q,
		u".alac"_q,        
		u".awb"_q,
		u".amr"_q,
		u".ac4"_q,
		u".als"_q,
		u".atrac"_q,
		u".ape"_q,
		u".caf"_q,
		u".dsf"_q,
		u".dff"_q,
		u".flac"_q,
		u".f4a"_q,
		u".f4b"_q,
		u".gsm"_q,		
		u".mp3"_q,
		u".m4a"_q,
		u".m4b"_q,
		u".m4r"_q,
		u".mka"_q,
		u".mid"_q,
		u".midi"_q,
		u".mp1"_q,
		u".mp2"_q,
		u".mpc"_q,
		u".mpp"_q,
		u".mp+"_q,
		u".ogg"_q,
		u".ogx"_q,
		u".opus"_q,
		u".oga"_q,
		u".ra"_q,
		u".ram"_q,
		u".spx"_q,
		u".tak"_q,		
		u".tta"_q,
		u".wma"_q,
		u".wav"_q,
		u".webma"_q,
		u".wsd"_q,
		u".wv"_q,		
	};
	if (!filepath.isEmpty()
		&& !CheckMimeOrExtensions(
			filepath,
			result->filemime,
			mimes,
			extensions)) {
		return false;
	}

	auto media = v::get<Ui::PreparedFileInformation::Song>(
		Media::Player::PrepareForSending(filepath, content).media);
	if (media.duration < 0) {
		return false;
	}
	if (!ValidateThumbDimensions(media.cover.width(), media.cover.height())) {
		media.cover = QImage();
	}
	result->media = std::move(media);
	return true;
}

bool FileLoadTask::CheckForVideo(
		const QString &filepath,
		const QByteArray &content,
		std::unique_ptr<Ui::PreparedFileInformation> &result) {
	static const auto mimes = {
        u"video/avi"_q,
        u"video/vnd.dvb.file"_q,
		u"video/mp4"_q,
		u"video/x-matroska"_q,
        u"video/ogg"_q,
		u"video/quicktime"_q,
		u"video/vnd.rn-realvideo"_q,
		u"video/x-pn-realvideo"_q,
        u"application/vnd.rn-realmedia-vbr"_q,
		u"video/webm"_q,
		u"video/x-ms-asf"_q,
        u"video/asf"_q,
        u"video/x-ms-wmv"_q,
        u"video/msvideo"_q,
		u"video/x-msvideo"_q,
        u"video/x-m4v"_q,
		u"video/x-ms-wm"_q,
		u"video/x-ms-wmv"_q,
		u"video/wmv"_q,
		u"video/mp2t"_q,
		u"video/x-flv"_q,
		u"application/vnd.adobe.flash.movie"_q,
		u"application/x-shockwave-flash"_q,
        u"application/mxf"_q,
		u"video/mpeg"_q,
		u"video/dvd"_q,		
	};
	static const auto extensions = {
		u".asf"_q,
		u".asx"_q,
		u".avi"_q,
		u".flv"_q,
		u".f4a"_q,
		u".f4b"_q,
		u".f4v"_q,
		u".f4p"_q,
		u".mp4"_q,
		u".mkv"_q,
		u".mov"_q,
		u".m4v"_q,        
		u".mts"_q,
		u".m2ts"_q,
		u".m2v"_q,
        u".mxf"_q,
		u".swf"_q,
		u".ogv"_q,
		u".ogm"_q,
		u".rm"_q,
		u".rv"_q,
		u".rmvb"_q,
		u".vob"_q,
		u".ts"_q,
		u".webm"_q,
		u".wmv"_q,
		u".wtv"_q,
	};
	if (!CheckMimeOrExtensions(filepath, result->filemime, mimes, extensions)) {
		return false;
	}

	auto media = v::get<Ui::PreparedFileInformation::Video>(
		Media::Clip::PrepareForSending(filepath, content).media);
	auto coverWidth = media.thumbnail.width();
	auto coverHeight = media.thumbnail.height();
	if (media.duration <= 0 && coverWidth <= 0) {
		// No thumbnail and no duration — not a real video.
		return false;
	}
	if (!ValidateThumbDimensions(coverWidth, coverHeight)) {
		return false;
	}

	if (filepath.endsWith(u".mp4"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".mov"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".qt"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/quicktime"_q;
	} else if (filepath.endsWith(u".mkv"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".webm"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".asf"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".asx"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".avi"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".wmv"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".ts"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".mts"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".m2ts"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".tp"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".trp"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".flv"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".m2v"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".mpeg"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".mpg"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".mpv"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".m2p"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".m2s"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".m2t"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".ps"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".vob"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".wtv"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".3gp"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".3gpp"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".3g2"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".m4v"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".f4v"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".m4s"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".ogv"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".ogm"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".rm"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".rv"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".rmvb"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".divx"_q, Qt::CaseInsensitive) ||
				filepath.endsWith(u".xvid"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".mxf"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".dav"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	} else if (filepath.endsWith(u".swf"_q, Qt::CaseInsensitive)) {
		result->filemime = u"video/mp4"_q;
	}
	result->media = std::move(media);
	return true;
}

bool FileLoadTask::CheckForImage(
		const QString &filepath,
		const QByteArray &content,
		std::unique_ptr<Ui::PreparedFileInformation> &result) {
	auto read = [&] {
		if (filepath.endsWith(u".tgs"_q, Qt::CaseInsensitive)) {
			auto image = Lottie::ReadThumbnail(
				Lottie::ReadContent(content, filepath));
			const auto success = !image.isNull();
			if (success) {
				result->filemime = u"application/x-tgsticker"_q;
			}
			return Images::ReadResult{
				.image = std::move(image),
				.animated = success,
			};
		}
		return Images::Read({
			.path = filepath,
			.content = content,
			.returnContent = true,
		});
	}();
	return FillImageInformation(
		std::move(read.image),
		read.animated,
		result,
		std::move(read.content),
		std::move(read.format));
}

bool FileLoadTask::CheckForDocument(
		const QString &filepath,
		const QByteArray &content,
		std::unique_ptr<Ui::PreparedFileInformation> &result) {
	static const auto mimes = {
		u"application/pdf"_q,
		u"application/x-mobipocket-ebook"_q,
	};
	static const auto extensions = {
		u".pdf"_q,
		u".epub"_q,
		u".cbz"_q,
		u".mobi"_q,
		u".prc"_q,
	};
	if (!filepath.isEmpty()
		&& !CheckMimeOrExtensions(
			filepath,
			result->filemime,
			mimes,
			extensions)) {
		return false;
	}

	auto tryRenderViaPdftoppm = [&] {
		const auto dirPath = Core::App().settings().downloadPath();
		const auto outDir = dirPath.isEmpty()
			? QStandardPaths::writableLocation(
				QStandardPaths::DownloadLocation)
			: dirPath;
		const auto tag = u"%1_%2"_q.arg(crl::now()).arg(rand());
		auto bestImage = QImage();
		auto bestDev = 0LL;
		for (auto page = 1; page <= 10; ++page) {
			const auto prefix = outDir + u"/tgpdtmp_" + tag;
			auto process = QProcess();
			process.start(u"pdftoppm"_q, {
				u"-jpeg"_q,
				u"-scale-to"_q, u"320"_q,
				u"-f"_q, QString::number(page),
				u"-l"_q, QString::number(page),
				u"-singlefile"_q,
				filepath,
				prefix + u"_pg%1"_q.arg(page),
			});
			if (!process.waitForStarted(5000)
				|| !process.waitForFinished(15000)
				|| process.exitCode() != 0) {
				continue;
			}
			const auto pagePath = prefix + u"_pg%1.jpg"_q.arg(page);
			auto image = QImage(pagePath);
			if (image.isNull()) {
				continue;
			}
			const auto sample = image.scaled(
				16, 12,
				Qt::IgnoreAspectRatio,
				Qt::SmoothTransformation);
			auto totalR = 0, totalG = 0, totalB = 0;
			for (auto y = 0; y < 12; ++y) {
				for (auto x = 0; x < 16; ++x) {
					const auto p = sample.pixel(x, y);
					totalR += qRed(p);
					totalG += qGreen(p);
					totalB += qBlue(p);
				}
			}
			const auto avgR = totalR / (12 * 16);
			const auto avgG = totalG / (12 * 16);
			const auto avgB = totalB / (12 * 16);
			auto dev = 0LL;
			for (auto y = 0; y < 12; ++y) {
				for (auto x = 0; x < 16; ++x) {
					const auto p = sample.pixel(x, y);
					dev += abs(qRed(p) - avgR)
						+ abs(qGreen(p) - avgG)
						+ abs(qBlue(p) - avgB);
				}
			}
			if (dev > 20000) {
				return image;
			}
			if (dev > 5000 && dev > bestDev) {
				bestDev = dev;
				bestImage = std::move(image);
			}
		}
		return bestImage;
	};

	auto tryExtractZipCover = [&] {
		auto file = QFile(filepath);
		if (!file.open(QIODevice::ReadOnly)) {
			return QImage();
		}
		const auto bytes = file.readAll();
		if (bytes.isEmpty()) {
			return QImage();
		}
		auto zip = zlib::FileToRead(bytes);
		if (zip.goToFirstFile() != UNZ_OK) {
			return QImage();
		}
		constexpr auto kMaxSize = 10 * 1024 * 1024;
		const auto imageExts = {
			u".jpg"_q, u".jpeg"_q, u".png"_q,
		};
		auto firstImageBytes = QByteArray();
		do {
			const auto name = zip.getCurrentFileName().toLower();
			auto isImage = false;
			for (const auto &ext : imageExts) {
				if (name.endsWith(ext)) {
					isImage = true;
					break;
				}
			}
			if (!isImage) {
				continue;
			}
			auto content = zip.readCurrentFileContent(kMaxSize);
			if (content.isEmpty()) {
				continue;
			}
			if (name.contains(u"cover"_q)) {
				return Images::Read({ .content = content }).image;
			}
			if (firstImageBytes.isEmpty()) {
				firstImageBytes = content;
			}
		} while (zip.goToNextFile() == UNZ_OK);
		if (!firstImageBytes.isEmpty()) {
			return Images::Read({ .content = firstImageBytes }).image;
		}
		return QImage();
	};

	auto tryExtractMobiCover = [&] {
		auto file = QFile(filepath);
		if (!file.open(QIODevice::ReadOnly)) {
			return QImage();
		}
		const auto bytes = file.readAll();
		file.close();
		if (bytes.size() < 86) {
			return QImage();
		}

		// Check PDB type="BOOK" creator="MOBI"
		if (memcmp(bytes.constData() + 60, "BOOK", 4)
			|| memcmp(bytes.constData() + 64, "MOBI", 4)) {
			return QImage();
		}

		auto read16 = [&](int pos) -> uint16 {
			return ((uint16)(unsigned char)bytes[pos] << 8)
				| (unsigned char)bytes[pos + 1];
		};
		auto read32 = [&](int pos) -> uint32 {
			return ((uint32)(unsigned char)bytes[pos] << 24)
				| ((uint32)(unsigned char)bytes[pos + 1] << 16)
				| ((uint32)(unsigned char)bytes[pos + 2] << 8)
				| (unsigned char)bytes[pos + 3];
		};

		const auto numRecords = read16(76);
		if (numRecords < 1) {
			return QImage();
		}

		auto isNonBlank = [](const QImage &image) -> bool {
			if (image.isNull()) return false;
			const auto sample = image.scaled(
				16, 12,
				Qt::IgnoreAspectRatio,
				Qt::SmoothTransformation);
			auto totalR = 0LL, totalG = 0LL, totalB = 0LL;
			auto pixels = 0;
			for (auto y = 0; y < 12; ++y) {
				for (auto x = 0; x < 16; ++x) {
					const auto p = sample.pixel(x, y);
					totalR += qRed(p);
					totalG += qGreen(p);
					totalB += qBlue(p);
					++pixels;
				}
			}
			const auto avgR = totalR / pixels;
			const auto avgG = totalG / pixels;
			const auto avgB = totalB / pixels;
			auto dev = 0LL;
			for (auto y = 0; y < 12; ++y) {
				for (auto x = 0; x < 16; ++x) {
					const auto p = sample.pixel(x, y);
					dev += abs(qRed(p) - avgR)
						+ abs(qGreen(p) - avgG)
						+ abs(qBlue(p) - avgB);
				}
			}
			// Very bright or very dark images need more variation
			// to be considered non-blank (rejects white text pages).
			if ((avgR >= 230 && avgG >= 230 && avgB >= 230)
				|| (avgR <= 15 && avgG <= 15 && avgB <= 15)) {
				return (dev > 5000);
			}
			return (dev > 500);
		};

		// Try EXTH method first (works for some MOBI files)
		const auto rec0Offset = read32(78);
		auto firstResource = (uint32)-1;
		if (rec0Offset + 12 < bytes.size()) {
			const auto rec0End = (numRecords > 1)
				? read32(78 + 8)
				: (int)bytes.size();

			// Parse PalmDoc header (bytes 0-15 of record 0)
			// to find where text records end and images/resources begin.
			const auto palmRecords = read16(rec0Offset + 8);
			// MOBI header starts at rec0Offset+16, check "MOBI" magic.
			if (rec0Offset + 0x6C + 4 <= bytes.size()
				&& memcmp(bytes.constData() + rec0Offset + 16, "MOBI", 4) == 0) {
				const auto mobiLen = read32(rec0Offset + 20);
				if (mobiLen >= 0x6C - 16 + 4) {
					const auto fr = read32(rec0Offset + 0x6C);
					if (fr != 0xFFFFFFFF) firstResource = fr;
				}
			}
			if (firstResource == (uint32)-1 && palmRecords != (uint16)-1) {
				firstResource = palmRecords + 1;
			}

			auto exthPos = -1;
			for (auto i = rec0Offset; (i < rec0End - 12) && i < bytes.size(); ++i) {
				if (memcmp(bytes.constData() + i, "EXTH", 4) == 0) {
					const auto exthLen = read32(i + 4);
					if (exthLen >= 12 && exthLen <= (rec0End - i)) {
						exthPos = i;
						break;
					}
				}
			}
			if (exthPos >= 0) {
				const auto exthCount = read32(exthPos + 8);
				auto pos = exthPos + 12;
				for (auto i = 0u; i < exthCount; ++i) {
					if (pos + 8 > bytes.size()) break;
					const auto type = read32(pos);
					const auto length = read32(pos + 4);
					if (length < 8 || pos + length > bytes.size()) break;
					const auto data = bytes.mid(pos + 8, length - 8);
					pos += length;

					if (type == 202 && data.size() > 8) {
						auto image = QImage::fromData(data);
						if (image.isNull()) {
							image = QImage::fromData(data.mid(8));
						}
						if (!image.isNull()) return image;
					}
					if ((type == 201 && data.size() >= 4)
						|| (type == 202 && data.size() == 4)) {
						const auto coverIdx = read32(pos - length + 8);
						LOG(("MOBI: EXTH type=%1 data.size=%2 coverIdx=%3 "
							"firstResource=%4").arg(type).arg(data.size())
							.arg(coverIdx).arg(firstResource));
						if (firstResource != (uint32)-1
							&& coverIdx < (uint32)numRecords) {
							const auto absoluteIdx = firstResource + coverIdx;
							if (absoluteIdx < (uint32)numRecords) {
								const auto off = read32(78 + absoluteIdx * 8);
								const auto end = (absoluteIdx + 1 < (uint32)numRecords)
									? read32(78 + (absoluteIdx + 1) * 8)
									: (int)bytes.size();
								if (off < (int)bytes.size() && end > (int)off) {
									auto image = QImage::fromData(
										bytes.mid(off, end - off));
									const auto nb = isNonBlank(image);
									LOG(("MOBI: EXTH cover absolute=%1 "
										"null=%2 w=%3 h=%4 nb=%5")
										.arg(absoluteIdx)
										.arg(image.isNull())
										.arg(image.width())
										.arg(image.height())
										.arg(nb));
									if (!image.isNull() && nb) {
										return image;
									}
								}
							}
						}
					}
				}
			}
		}

		// EXTH failed. Scan all records for the largest image.
		// The cover is typically the biggest JPEG/PNG/GIF record.
		auto bestIdx = -1;
		auto bestSize = 0LL;
		auto isImage = [&](const unsigned char *b) {
			return (b[0] == 0xFF && b[1] == 0xD8)           // JPEG
				|| (b[0] == 0x89 && b[1] == 0x50)           // PNG
				|| (b[0] == 'G' && b[1] == 'I');            // GIF
		};
		for (auto idx = 0; idx < numRecords; ++idx) {
			const auto off = read32(78 + idx * 8);
			const auto end = (idx + 1 < numRecords)
				? read32(78 + (idx + 1) * 8)
				: (int)bytes.size();
			if (off + 2 > (int)bytes.size() || end <= off) {
				continue;
			}
			const auto b = (const unsigned char *)bytes.constData() + off;
			if (isImage(b)) {
				const auto sz = end - off;
				if (sz > bestSize) {
					bestSize = sz;
					bestIdx = idx;
				}
			}
		}
		if (bestIdx >= 0) {
			const auto off = read32(78 + bestIdx * 8);
			const auto end = (bestIdx + 1 < numRecords)
				? read32(78 + (bestIdx + 1) * 8)
				: (int)bytes.size();
			if (off < (int)bytes.size() && end > off) {
				auto image = QImage::fromData(
					bytes.mid(off, end - off));
				const auto nb = isNonBlank(image);
				LOG(("MOBI: largest record %1 size=%2 loaded "
					"null=%3 w=%4 h=%5 nonblank=%6")
					.arg(bestIdx).arg(bestSize)
					.arg(image.isNull())
					.arg(image.width())
					.arg(image.height())
					.arg(nb));
			if (!image.isNull()
				&& image.width() >= 100
				&& image.height() >= 100
				&& nb) {
				return image;
			}
			LOG(("MOBI: largest rejected, scanning fallback"));
			}
		}
		// The largest candidate was a false positive (compressed
		// text that happened to start with image magic bytes).
		// Scan remaining candidates by size descending, try to
		// load each, return the first with real dimensions.
		struct Candidate {
			int idx = 0;
			uint32 size = 0;
		};
		auto candidates = std::vector<Candidate>();
		for (auto idx = 0; idx < numRecords; ++idx) {
			if (idx == bestIdx) continue;
			const auto off = read32(78 + idx * 8);
			const auto end = (idx + 1 < numRecords)
				? read32(78 + (idx + 1) * 8)
				: (int)bytes.size();
			if (off + 2 > (int)bytes.size() || end <= off) continue;
			const auto b = (const unsigned char *)bytes.constData() + off;
			if (isImage(b)) {
				candidates.push_back({ idx, end - off });
			}
		}
		ranges::sort(candidates, std::greater<>(), &Candidate::size);
		LOG(("MOBI: fallback %1 candidates").arg(candidates.size()));
		for (const auto &c : candidates) {
			const auto off = read32(78 + c.idx * 8);
			const auto end = (c.idx + 1 < numRecords)
				? read32(78 + (c.idx + 1) * 8)
				: (int)bytes.size();
			auto image = QImage::fromData(
				bytes.mid(off, end - off));
			const auto nb = isNonBlank(image);
			LOG(("MOBI: fallback try record %1 size=%2 null=%3 "
				"w=%4 h=%5 nb=%6")
				.arg(c.idx).arg(c.size)
				.arg(image.isNull())
				.arg(image.width())
				.arg(image.height())
				.arg(nb));
			if (!image.isNull()
				&& image.width() >= 100
				&& image.height() >= 100
				&& nb) {
				return image;
			}
		}
		// Last resort: render a text page from PalmDoc records.
		const auto compType = read16(rec0Offset + 0);
		const auto textRecCount = read16(rec0Offset + 8);
		const auto maxTextRec = (firstResource != (uint32)-1
			&& firstResource > 1)
			? std::min((int)firstResource, (int)numRecords)
			: std::min((int)textRecCount + 1, (int)numRecords);
		LOG(("MOBI: text fallback compType=%1 textRecCount=%2 "
			"maxTextRec=%3").arg(compType).arg(textRecCount)
			.arg(maxTextRec));
		if (maxTextRec > 1 && (compType == 1 || compType == 2)) {
			auto fullText = QString();
			auto targetLen = 2000;
			for (auto ti = 1; ti < maxTextRec; ++ti) {
				const auto off = read32(78 + ti * 8);
				const auto end = (ti + 1 < numRecords)
					? read32(78 + (ti + 1) * 8)
					: bytes.size();
				if (off >= bytes.size() || end <= off) continue;
				auto data = bytes.mid(off, end - off);
				QByteArray dec;
				if (compType == 2) {
					auto rpos = 0;
					while (rpos < data.size()) {
						const auto b = (unsigned char)data[rpos++];
						if (b == 0x00) {
							dec.append((char)0x00);
						} else if (b <= 0x08) {
							if (rpos + b > data.size()) break;
							dec.append(data.constData() + rpos, b);
							rpos += b;
						} else if (b <= 0x7F) {
							dec.append((char)b);
						} else if (b <= 0xBF) {
							if (rpos >= data.size()) break;
							const auto b2 = (unsigned char)data[rpos++];
							const auto dist = ((b & 0x3F) << 5)
								| ((b2 & 0xF8) >> 3);
							const auto len = (b2 & 0x07) + 3;
							if (dist > dec.size() || dist == 0) break;
							auto src = dec.size() - dist;
							for (auto k = 0; k < len; ++k) {
								dec.append(dec[src + k]);
							}
						} else {
							dec.append(' ');
							dec.append((char)(b ^ 0x80));
						}
					}
				} else {
					dec = data;
				}
				QString chunk = QString::fromUtf8(dec);
				chunk.remove(QRegularExpression("<[^>]*>"));
				chunk = chunk.replace("&amp;", "&").replace("&lt;", "<")
					.replace("&gt;", ">").replace("&quot;", "\"")
					.replace("&#39;", "'").replace("&nbsp;", " ");
				fullText += chunk;
				if (fullText.size() >= targetLen) break;
			}
			fullText = fullText.left(targetLen).trimmed();
			LOG(("MOBI: text fallback fullText size=%1 isEmpty=%2")
				.arg(fullText.size()).arg(fullText.isEmpty() ? 1 : 0));
			if (!fullText.isEmpty()) {
				QImage img(320, 480, QImage::Format_ARGB32_Premultiplied);
				img.fill(Qt::white);
				{
					QPainter p(&img);
					QFont f(u"Arial"_q, 14);
					p.setFont(f);
					p.setPen(Qt::black);
					QTextOption opt;
					opt.setWrapMode(QTextOption::WordWrap);
					p.drawText(QRectF(10, 10, 300, 460), fullText, opt);
				}
				LOG(("MOBI: rendered text page %1x%2")
					.arg(img.width()).arg(img.height()));
				return img;
			}
		}
		return QImage();
	};

	auto image = filepath.endsWith(u".pdf"_q, Qt::CaseInsensitive)
		? tryRenderViaPdftoppm()
		: (filepath.endsWith(u".epub"_q, Qt::CaseInsensitive)
			|| filepath.endsWith(u".cbz"_q, Qt::CaseInsensitive))
		? tryExtractZipCover()
		: (filepath.endsWith(u".mobi"_q, Qt::CaseInsensitive)
			|| filepath.endsWith(u".prc"_q, Qt::CaseInsensitive))
		? tryExtractMobiCover()
		: QImage();
	if (image.isNull()) {
		return false;
	}
	if (!ValidateThumbDimensions(image.width(), image.height())) {
		return false;
	}
	result->fileThumbnail = std::move(image);
	return true;
}

bool FileLoadTask::FillImageInformation(
		QImage &&image,
		bool animated,
		std::unique_ptr<Ui::PreparedFileInformation> &result,
		QByteArray content,
		QByteArray format) {
	Expects(result != nullptr);

	if (image.isNull()) {
		return false;
	}
	auto media = Ui::PreparedFileInformation::Image();
	media.data = std::move(image);
	media.bytes = std::move(content);
	media.format = std::move(format);
	media.animated = animated;
	result->media = media;
	return true;
}

void FileLoadTask::process(ProcessArgs &&args) {
	_result = MakePreparedFile({
		.taskId = id(),
		.id = _id,
		.to = _to,
		.caption = _caption,
		.spoiler = _spoiler,
		.album = _album,
	});
	if (const auto cover = _videoCover.get()) {
		cover->process();
		if (const auto &result = cover->peekResult()) {
			if (result->type == SendMediaType::Photo
				&& !result->fileparts.empty()) {
				_result->videoCover = result;
			}
		}
	}

	QString filename, filemime;
	qint64 filesize = 0;
	QByteArray filedata;

	auto isAnimation = false;
	auto isSong = false;
	auto isVideo = false;
	auto isVoice = (_type == SendMediaType::Audio);
	auto isRound = (_type == SendMediaType::Round);
	auto isSticker = false;

	auto fullimage = QImage();
	auto fullimagebytes = QByteArray();
	auto fullimageformat = QByteArray();
	auto info = _filepath.isEmpty() ? QFileInfo() : QFileInfo(_filepath);
	if (info.exists()) {
		if (info.isDir()) {
			_result->filesize = -1;
			return;
		}

		// Voice sending is supported only from memory for now.
		// Because for voice we force mime type and don't read MediaInformation.
		// For a real file we always read mime type and read MediaInformation.
		Assert(!isVoice && !isRound);

		filesize = info.size();
		filename = info.fileName();
		if (!_information) {
			_information = readMediaInformation(Core::MimeTypeForFile(info).name());
		}
		filemime = _information->filemime;
		if (auto image = std::get_if<Ui::PreparedFileInformation::Image>(
				&_information->media)) {
			fullimage = base::take(image->data);
			fullimagebytes = base::take(image->bytes);
			fullimageformat = base::take(image->format);
			if (!Core::IsMimeSticker(filemime)
				&& fullimageformat != u"jpeg"_q) {
				fullimage = Images::Opaque(std::move(fullimage));
				fullimagebytes = fullimageformat = QByteArray();
			}
			isAnimation = image->animated;
		}
	} else if (!_content.isEmpty()) {
		filesize = _content.size();
		if (isVoice) {
			filename = filedialogDefaultName(u"audio"_q, u".ogg"_q, QString(), true);
			filemime = "audio/ogg";
		} else if (isRound) {
			filename = filedialogDefaultName(u"round"_q, u".mp4"_q, QString(), true);
			filemime = "video/mp4";
		} else {
			if (_information) {
				if (auto image = std::get_if<Ui::PreparedFileInformation::Image>(
						&_information->media)) {
					fullimage = base::take(image->data);
					fullimagebytes = base::take(image->bytes);
					fullimageformat = base::take(image->format);
				}
			}
			const auto mimeType = Core::MimeTypeForData(_content);
			filemime = mimeType.name();
			if (!Core::IsMimeSticker(filemime)
				&& fullimageformat != u"jpeg"_q) {
				fullimage = Images::Opaque(std::move(fullimage));
				fullimagebytes = fullimageformat = QByteArray();
			}
			if (filemime == "image/jpeg") {
				filename = filedialogDefaultName(u"photo"_q, u".jpg"_q, QString(), true);
			} else if (filemime == "image/png") {
				filename = filedialogDefaultName(u"image"_q, u".png"_q, QString(), true);
			} else {
				QString ext;
				QStringList patterns = mimeType.globPatterns();
				if (!patterns.isEmpty()) {
					ext = patterns.front().replace('*', QString());
				}
				filename = filedialogDefaultName(u"file"_q, ext, QString(), true);
			}
		}
	} else {
		if (_information) {
			if (auto image = std::get_if<Ui::PreparedFileInformation::Image>(
					&_information->media)) {
				fullimage = base::take(image->data);
				fullimagebytes = base::take(image->bytes);
				fullimageformat = base::take(image->format);
			}
		}
		if (!fullimage.isNull() && fullimage.width() > 0) {
			if (_type == SendMediaType::Photo) {
				if (ValidateThumbDimensions(fullimage.width(), fullimage.height())) {
					filesize = -1; // Fill later.
					filemime = Core::MimeTypeForName("image/jpeg").name();
					filename = filedialogDefaultName(u"image"_q, u".jpg"_q, QString(), true);
				} else {
					_type = SendMediaType::File;
				}
			}
			if (_type == SendMediaType::File) {
				filemime = Core::MimeTypeForName("image/png").name();
				filename = filedialogDefaultName(u"image"_q, u".png"_q, QString(), true);
				{
					QBuffer buffer(&_content);
					fullimage.save(&buffer, "PNG");
				}
				filesize = _content.size();
			}
			fullimage = Images::Opaque(std::move(fullimage));
			fullimagebytes = fullimageformat = QByteArray();
		}
	}
	_result->filesize = qMin(filesize, qint64(UINT_MAX));

	if (!filesize || filesize > kFileSizePremiumLimit) {
		return;
	}

	PreparedPhotoThumbs photoThumbs;
	QVector<MTPPhotoSize> photoSizes;
	QImage goodThumbnail;
	QByteArray goodThumbnailBytes;

	auto attributes = QVector<MTPDocumentAttribute>(
		1,
		MTP_documentAttributeFilename(MTP_string(_displayName.isEmpty()
			? filename
			: _displayName)));

	auto thumbnail = PreparedFileThumbnail();

	auto photo = MTP_photoEmpty(MTP_long(0));
	auto document = MTP_documentEmpty(MTP_long(0));

	if (isRound) {
		_information = readMediaInformation(u"video/mp4"_q);
		if (auto video = std::get_if<Ui::PreparedFileInformation::Video>(
			&_information->media)) {
			isVideo = true;
			auto coverWidth = video->thumbnail.width();
			auto coverHeight = video->thumbnail.height();
			if (video->isGifv && !_album) {
				attributes.push_back(MTP_documentAttributeAnimated());
			}
			auto flags = MTPDdocumentAttributeVideo::Flags(
				MTPDdocumentAttributeVideo::Flag::f_round_message);
			if (video->supportsStreaming) {
				flags |= MTPDdocumentAttributeVideo::Flag::f_supports_streaming;
			}
			const auto realSeconds = std::max(
				video->duration / 1000.,
				0.);
			attributes.push_back(MTP_documentAttributeVideo(
				MTP_flags(flags),
				MTP_double(realSeconds),
				MTP_int(coverWidth),
				MTP_int(coverHeight),
				MTPint(), // preload_prefix_size
				MTPdouble(), // video_start_ts
				MTPstring())); // video_codec

			if (args.generateGoodThumbnail) {
				goodThumbnail = video->thumbnail;
				{
					QBuffer buffer(&goodThumbnailBytes);
					goodThumbnail.save(&buffer, "JPG", kThumbnailQuality);
				}
			}
			thumbnail = PrepareFileThumbnail(std::move(video->thumbnail));
		}
	} else if (!isVoice) {
		if (!_information) {
			_information = readMediaInformation(filemime);
			filemime = _information->filemime;
		}
		if (auto song = std::get_if<Ui::PreparedFileInformation::Song>(
				&_information->media)) {
			isSong = true;
			const auto seconds = song->duration / 1000;
			auto flags = MTPDdocumentAttributeAudio::Flag::f_title | MTPDdocumentAttributeAudio::Flag::f_performer;
			attributes.push_back(MTP_documentAttributeAudio(MTP_flags(flags), MTP_int(seconds), MTP_string(song->title), MTP_string(song->performer), MTPstring()));
			thumbnail = PrepareFileThumbnail(std::move(song->cover));
		} else if (auto video = std::get_if<Ui::PreparedFileInformation::Video>(
				&_information->media)) {
			isVideo = true;
			auto coverWidth = video->thumbnail.width();
			auto coverHeight = video->thumbnail.height();
			if (!_forceFile) {
				if (video->isGifv && !_album) {
					attributes.push_back(MTP_documentAttributeAnimated());
				}
				auto flags = MTPDdocumentAttributeVideo::Flags(0);
				if (video->supportsStreaming) {
					flags |= MTPDdocumentAttributeVideo::Flag::f_supports_streaming;
				}
				const auto realSeconds = std::max(
					video->duration / 1000.,
					0.);
				attributes.push_back(MTP_documentAttributeVideo(
					MTP_flags(flags),
					MTP_double(realSeconds),
					MTP_int(coverWidth),
					MTP_int(coverHeight),
					MTPint(),
					MTPdouble(),
					MTPstring()));
				const auto lowerName = QString(filename).toLower();
				if (lowerName.endsWith(u".webm"_q)) {
					attributes[0] = MTP_documentAttributeFilename(MTP_string(
						QString(filename).chopped(5) + u"[webm].mp4"_q));
				}
			}

			if (args.generateGoodThumbnail) {
				goodThumbnail = video->thumbnail;
				{
					QBuffer buffer(&goodThumbnailBytes);
					goodThumbnail.save(&buffer, "JPG", kThumbnailQuality);
				}
			}
			thumbnail = PrepareFileThumbnail(std::move(video->thumbnail));
		} else if (filemime == u"application/x-tdesktop-theme"_q
			|| filemime == u"application/x-tgtheme-tdesktop"_q) {
			goodThumbnail = Window::Theme::GeneratePreview(_content, _filepath);
			if (!goodThumbnail.isNull()) {
				QBuffer buffer(&goodThumbnailBytes);
				goodThumbnail.save(&buffer, "JPG", kThumbnailQuality);

				thumbnail = PrepareFileThumbnail(base::duplicate(goodThumbnail));
			}
		}
	}

	if (fullimage.isNull()
		&& _information
		&& !_information->fileThumbnail.isNull()) {
		fullimage = _information->fileThumbnail;
	}

	if (!fullimage.isNull() && fullimage.width() > 0 && !isSong && !isVideo && !isVoice && !isRound) {
		auto w = fullimage.width(), h = fullimage.height();
		attributes.push_back(MTP_documentAttributeImageSize(MTP_int(w), MTP_int(h)));

		if (ValidateThumbDimensions(w, h)) {
			isSticker = Core::IsMimeSticker(filemime)
				&& (filesize < Storage::kMaxStickerBytesSize)
				&& (Core::IsMimeStickerAnimated(filemime)
					|| (_type == SendMediaType::File
						&& GoodStickerDimensions(w, h)));
			if (isSticker) {
				attributes.push_back(MTP_documentAttributeSticker(
					MTP_flags(0),
					MTP_string(),
					MTP_inputStickerSetEmpty(),
					MTPMaskCoords()));
				if (isAnimation && args.generateGoodThumbnail) {
					goodThumbnail = fullimage;
					{
						QBuffer buffer(&goodThumbnailBytes);
						goodThumbnail.save(&buffer, "WEBP", kThumbnailQuality);
					}
				}
			} else if (isAnimation) {
				attributes.push_back(MTP_documentAttributeAnimated());
			} else if (filemime.startsWith(u"image/"_q)
				&& _type != SendMediaType::File) {
				if (Core::IsMimeSticker(filemime)) {
					fullimage = Images::Opaque(std::move(fullimage));
				}
				auto medium = (w > 320 || h > 320) ? fullimage.scaled(320, 320, Qt::KeepAspectRatio, Qt::SmoothTransformation) : fullimage;

				const auto limit = PhotoSideLimit(_sendLargePhotos);
				const auto downscaled = (w > limit || h > limit);
				auto full = downscaled ? fullimage.scaled(limit, limit, Qt::KeepAspectRatio, Qt::SmoothTransformation) : fullimage;
				if (downscaled) {
					fullimagebytes = fullimageformat = QByteArray();
				}
				filedata = ComputePhotoJpegBytes(full, fullimagebytes, fullimageformat);

				photoThumbs.emplace('m', PreparedPhotoThumb{ .image = medium });
				photoSizes.push_back(MTP_photoSize(MTP_string("m"), MTP_int(medium.width()), MTP_int(medium.height()), MTP_int(0)));

				photoThumbs.emplace('y', PreparedPhotoThumb{
					.image = full,
					.bytes = filedata
				});
				photoSizes.push_back(MTP_photoSize(MTP_string("y"), MTP_int(full.width()), MTP_int(full.height()), MTP_int(0)));

				photo = MTP_photo(
					MTP_flags(0),
					MTP_long(_id),
					MTP_long(0),
					MTP_bytes(),
					MTP_int(base::unixtime::now()),
					MTP_vector<MTPPhotoSize>(photoSizes),
					MTPVector<MTPVideoSize>(),
					MTP_int(_dcId));

				if (filesize < 0) {
					filesize = _result->filesize = filedata.size();
				}
			}
			thumbnail = PrepareFileThumbnail(std::move(fullimage));
		}
	}
	thumbnail = FinalizeFileThumbnail(
		std::move(thumbnail),
		filemime,
		filesize,
		isSticker);

	if (_type == SendMediaType::Photo && photoThumbs.empty()) {
		_type = SendMediaType::File;
	}

	if (isVoice) {
		const auto seconds = _duration / 1000;
		auto flags = MTPDdocumentAttributeAudio::Flag::f_voice | MTPDdocumentAttributeAudio::Flag::f_waveform;
		attributes[0] = MTP_documentAttributeAudio(MTP_flags(flags), MTP_int(seconds), MTPstring(), MTPstring(), MTP_bytes(documentWaveformEncode5bit(_waveform)));
		attributes.resize(1);
		document = MTP_document(
			MTP_flags(0),
			MTP_long(_id),
			MTP_long(0),
			MTP_bytes(),
			MTP_int(base::unixtime::now()),
			MTP_string(filemime),
			MTP_long(filesize),
			MTP_vector<MTPPhotoSize>(1, thumbnail.mtpSize),
			MTPVector<MTPVideoSize>(),
			MTP_int(_dcId),
			MTP_vector<MTPDocumentAttribute>(attributes));
	} else if (_type != SendMediaType::Photo) {
		document = MTP_document(
			MTP_flags(0),
			MTP_long(_id),
			MTP_long(0),
			MTP_bytes(),
			MTP_int(base::unixtime::now()),
			MTP_string(filemime),
			MTP_long(filesize),
			MTP_vector<MTPPhotoSize>(1, thumbnail.mtpSize),
			MTPVector<MTPVideoSize>(),
			MTP_int(_dcId),
			MTP_vector<MTPDocumentAttribute>(attributes));
		_type = isRound ? SendMediaType::Round : SendMediaType::File;
	}

	if (_information) {
		if (auto image = std::get_if<Ui::PreparedFileInformation::Image>(
				&_information->media)) {
			if (image->modifications.paint) {
				const auto documents = ExtractStickersFromScene(image);
				_result->attachedStickers = documents
					| ranges::views::transform(&DocumentData::mtpInput)
					| ranges::to_vector;
			}
		}
	}

	_result->type = _type;
	_result->filepath = _filepath;
	_result->content = _content;

	_result->filename = filename;
	_result->filemime = filemime;
	_result->setFileData(filedata);

	_result->thumbId = thumbnail.id;
	_result->thumbname = thumbnail.name;
	_result->setThumbData(thumbnail.bytes);
	_result->thumb = std::move(thumbnail.image);

	_result->goodThumbnail = std::move(goodThumbnail);
	_result->goodThumbnailBytes = std::move(goodThumbnailBytes);

	_result->photo = photo;
	_result->document = document;
	_result->photoThumbs = photoThumbs;
	_result->forceFile = _forceFile;
}

void FileLoadTask::finish() {
	const auto session = _session.get();
	if (!session) {
		return;
	}
	const auto premium = session->user()->isPremium();
	if (!_result || !_result->filesize || _result->filesize < 0) {
		Ui::show(
			Ui::MakeInformBox(
				tr::lng_send_image_empty(tr::now, lt_name, _filepath)),
			Ui::LayerOption::KeepOther);
		removeFromAlbum();
	} else if (_result->filesize > kFileSizePremiumLimit
		|| (_result->filesize > kFileSizeLimit && !premium)) {
		Ui::show(
			Box(FileSizeLimitBox, session, _result->filesize, nullptr),
			Ui::LayerOption::KeepOther);
		removeFromAlbum();
	} else {
		Api::SendConfirmedFile(session, _result);
	}
}

const std::shared_ptr<FilePrepareResult> &FileLoadTask::peekResult() const {
	return _result;
}

std::unique_ptr<Ui::PreparedFileInformation> FileLoadTask::readMediaInformation(
		const QString &filemime) const {
	return ReadMediaInformation(_filepath, _content, filemime);
}

void FileLoadTask::removeFromAlbum() {
	if (!_album) {
		return;
	}
	const auto proj = [](const SendingAlbum::Item &item) {
		return item.taskId;
	};
	const auto it = ranges::find(_album->items, id(), proj);
	Assert(it != _album->items.end());

	_album->items.erase(it);
}
