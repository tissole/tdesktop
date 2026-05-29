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
#include <QProcess>
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

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtCore/QXmlStreamReader>
#include <mupdf/fitz.h>
#include <QtGui/QImageWriter>

#include "base/zlib_help.h"

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
        u"audio/aiff"_q,
		u"audio/als"_q,
		u"audio/ape"_q,
		u"audio/atrac"_q,
        u"audio/basic"_q,
		u"audio/dsd"_q,
        u"audio/eac3"_q,
		u"audio/flac"_q,
		u"audio/m4a"_q,
		u"audio/m4b"_q,
		u"audio/mp3"_q,
		u"audio/mp4"_q,
		u"audio/musepack"_q,
		u"audio/ogg"_q,
		u"audio/opus"_q,
		u"audio/vnd.dts"_q,
		u"audio/vnd.dts.hd"_q,
		u"audio/vnd.rn-realaudio"_q,
		u"audio/vnd.wave"_q,
		u"audio/vorbis"_q,
		u"audio/wav"_q,
        u"audio/wave"_q,
		u"audio/webm"_q,
        u"audio/x-aiff"_q,
		u"audio/x-caf"_q,
		u"audio/x-dff"_q,
		u"audio/x-dsd"_q,
        u"audio/x-dop"_q,
        u"audio/x-flac"_q
		u"audio/x-dsf"_q,
		u"audio/x-dsdiff"_q,
		u"audio/x-m4a"_q,
		u"audio/x-matroska"_q,
        u"audio/x-m4b"_q,
        u"audio/x-monkeys-audio"_q,
		u"audio/x-ms-wma"_q,
        u"audio/x-musepack"_q,
		u"audio/x-pn-realaudio"_q,
		u"audio/x-tak"_q,
		u"audio/x-tta"_q,
        u"audio/x-wav"_q,
		u"audio/x-wavpack"_q,
	};
	static const auto extensions = {
		u".aac"_q,
		u".ac4"_q,
		u".aif"_q,
		u".aifc"_q,
		u".aiff"_q,
        u".aff"_q,
		u".alac"_q,
		u".als"_q,
		u".ape"_q,
		u".atrac"_q,
        u".au"_q,
		u".caf"_q,
		u".dff"_q,
		u".dsf"_q,
		u".dts"_q,
		u".dtshd"_q,
		u".f4a"_q,
		u".f4b"_q,
		u".flac"_q,
		u".m4a"_q,
		u".m4b"_q,
		u".m4r"_q,
		u".mka"_q,
		u".mp+"_q,
		u".mp2"_q,
		u".mp3"_q,
		u".mpc"_q,
		u".mpp"_q,
		u".oga"_q,
		u".ogg"_q,
		u".ogx"_q,
		u".opus"_q,
		u".ra"_q,
		u".ram"_q,
		u".spx"_q,
		u".tak"_q,
		u".tta"_q,
		u".wav"_q,
		u".webma"_q,
		u".wma"_q,
		u".wsd"_q,
		u".wv"_q,
        u".snd"_q,
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
		u"application/mxf"_q,
		u"application/vnd.adobe.flash.movie"_q,
		u"application/vnd.rn-realmedia-vbr"_q,
		u"application/x-shockwave-flash"_q,
		u"video/asf"_q,
		u"video/avi"_q,
		u"video/dvd"_q,
		u"video/mp2t"_q,
		u"video/mp4"_q,
		u"video/mpeg"_q,
		u"video/msvideo"_q,
		u"video/ogg"_q,
		u"video/quicktime"_q,
		u"video/vnd.dvb.file"_q,
		u"video/vnd.rn-realvideo"_q,
		u"video/webm"_q,
		u"video/wmv"_q,
		u"video/x-flv"_q,
		u"video/x-m4v"_q,
		u"video/x-matroska"_q,
		u"video/x-ms-asf"_q,
		u"video/x-ms-wm"_q,
		u"video/x-ms-wmv"_q,
		u"video/x-ms-wmv"_q,
		u"video/x-msvideo"_q,
		u"video/x-pn-realvideo"_q,
        u"video/x-quicktime"_q,
	};
	static const auto extensions = {
		u".asf"_q,
		u".asx"_q,
		u".avi"_q,
		u".f4p"_q,
		u".f4v"_q,
		u".flv"_q,
		u".m2ts"_q,
		u".m2v"_q,
		u".m4v"_q,
		u".mkv"_q,
		u".mov"_q,
		u".mp4"_q,
		u".mts"_q,
		u".mxf"_q,
		u".ogm"_q,
		u".ogv"_q,
		u".rm"_q,
		u".rmvb"_q,
		u".rv"_q,
		u".swf"_q,
		u".ts"_q,
		u".vob"_q,
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
		u"application/epub+zip"_q,
		u"application/vnd.openxmlformats-officedocument.wordprocessingml.document"_q,
		u"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"_q,
		u"application/vnd.openxmlformats-officedocument.presentationml.presentation"_q,
		u"application/x-cbr"_q,
		u"application/vnd.rar"_q,
	};
	static const auto extensions = {
		u".pdf"_q,
		u".epub"_q,
		u".cbz"_q,
		u".cbr"_q,
		u".mobi"_q,
		u".prc"_q,
		u".docx"_q,
		u".xlsx"_q,
		u".pptx"_q,
		u".fb2"_q,
		u".txt"_q,
		u".text"_q,
		u".html"_q,
		u".htm"_q,
		u".xhtml"_q,
	};
	if (!filepath.isEmpty()
		&& !CheckMimeOrExtensions(
			filepath,
			result->filemime,
			mimes,
			extensions)) {
		return false;
	}

	auto tryRenderViaMuPDF = [&] {
		LOG(("MuPDF thumb: trying %1").arg(filepath));
		auto ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
		if (!ctx) {
			LOG(("MuPDF thumb: fz_new_context failed"));
			return QImage();
		}
		fz_register_document_handlers(ctx);
		fz_document *doc = nullptr;
		fz_var(doc);
		auto ok = false;
		if (filepath.endsWith(u".html"_q) || filepath.endsWith(u".htm"_q) || filepath.endsWith(u".xhtml"_q)) {
			QFile f(filepath);
			if (f.open(QIODevice::ReadOnly)) {
				auto html = f.readAll();
				f.close();
				auto sanitized = QByteArray();
				sanitized.reserve(html.size());
				int tableDepth = 0;
				for (int i = 0; i < html.size();) {
					if (html[i] == '<') {
						int end = i + 1;
						bool isClosing = (end < html.size() && html[end] == '/');
						if (isClosing) ++end;
						int tagStart = end;
						while (end < html.size() && html[end] != '>' && html[end] != ' ' && html[end] != '\t' && html[end] != '\n' && html[end] != '\r')
							++end;
						auto tagName = html.mid(tagStart, end - tagStart).toLower();
						auto isTableTag = (tagName == "table" || tagName == "tr" || tagName == "td"
							|| tagName == "th" || tagName == "tbody" || tagName == "thead"
							|| tagName == "tfoot" || tagName == "caption" || tagName == "colgroup"
							|| tagName == "col");
						if (isTableTag) {
							if (!isClosing && tagName == "table") ++tableDepth;
							if (isClosing && tagName == "table") --tableDepth;
							sanitized.append(isClosing ? "</div>" : "<div");
							if (!isClosing) {
								auto attrsEnd = i + 1;
								while (attrsEnd < html.size() && html[attrsEnd] != '>')
									++attrsEnd;
								if (attrsEnd < html.size()) {
									auto afterName = tagStart + tagName.size();
									sanitized.append(html.mid(afterName, attrsEnd - afterName));
									sanitized.append(">");
									i = attrsEnd + 1;
									continue;
								}
							}
							sanitized.append(">");
							i = (html[i] == '>') ? i + 2 : end + 1;
							while (i < html.size() && html[i] != '>') ++i;
							if (i < html.size()) ++i;
							continue;
						}
					}
					sanitized.append(html[i]);
					++i;
				}
				LOG(("MuPDF thumb: HTML sanitized, tableDepth=%1, size=%2→%3")
					.arg(tableDepth).arg(html.size()).arg(sanitized.size()));
				fz_try(ctx) {
					doc = fz_open_document_with_stream(
						ctx, "html", fz_open_memory(ctx,
							(const unsigned char*)sanitized.constData(),
							sanitized.size()));
					if (doc) ok = true;
				} fz_catch(ctx) {
					LOG(("MuPDF thumb: fz_open_document sanitized error: %1")
						.arg(QString::fromUtf8(fz_caught_message(ctx))));
				}
				if (!ok) {
					LOG(("MuPDF thumb: sanitized open failed, falling back to raw"));
					fz_try(ctx) {
						doc = fz_open_document_with_stream(
							ctx, "html", fz_open_memory(ctx,
								(const unsigned char*)html.constData(),
								html.size()));
						if (doc) ok = true;
					} fz_catch(ctx) {
						LOG(("MuPDF thumb: raw fallback also failed: %1")
							.arg(QString::fromUtf8(fz_caught_message(ctx))));
					}
				}
			} else {
				LOG(("MuPDF thumb: failed to open HTML file for sanitization"));
				fz_try(ctx) {
					doc = fz_open_document(ctx, filepath.toUtf8().constData());
					if (doc) ok = true;
				} fz_catch(ctx) {
					LOG(("MuPDF thumb: fz_open_document error: %1")
						.arg(QString::fromUtf8(fz_caught_message(ctx))));
				}
			}
		} else {
			fz_try(ctx) {
				doc = fz_open_document(ctx, filepath.toUtf8().constData());
				if (doc) ok = true;
			} fz_catch(ctx) {
				LOG(("MuPDF thumb: fz_open_document error: %1")
					.arg(QString::fromUtf8(fz_caught_message(ctx))));
			}
		}
		if (!ok) {
			LOG(("MuPDF thumb: fz_open_document failed"));
			fz_drop_context(ctx);
			return QImage();
		}
		if (fz_is_document_reflowable(ctx, doc)) {
			LOG(("MuPDF thumb: reflowable, laying out at 320x400 em=12"));
			fz_try(ctx) {
				fz_layout_document(ctx, doc, 320, 400, 12);
			} fz_catch(ctx) {
				LOG(("MuPDF thumb: fz_layout_document failed: %1")
					.arg(QString::fromUtf8(fz_caught_message(ctx))));
				fz_drop_document(ctx, doc);
				fz_drop_context(ctx);
				return QImage();
			}
		} else {
			LOG(("MuPDF thumb: not reflowable"));
		}
		auto pageCount = 0;
		fz_try(ctx) {
			pageCount = fz_count_pages(ctx, doc);
		} fz_catch(ctx) {
			LOG(("MuPDF thumb: fz_count_pages failed: %1")
				.arg(QString::fromUtf8(fz_caught_message(ctx))));
			fz_drop_document(ctx, doc);
			fz_drop_context(ctx);
			return QImage();
		}
		auto bestColorImage = QImage();
		auto bestFallbackImage = QImage();
		auto bestFallbackScore = 0;
		auto lastResortImage = QImage();
		const auto maxPages = std::min(pageCount, 50);
		fz_pixmap *pix = nullptr;
		fz_page *page = nullptr;
		fz_var(pix);
		fz_var(page);
		auto renderPage = [&](int i) {
			LOG(("MuPDF thumb: rendering page %1").arg(i));
			fz_try(ctx) {
				page = fz_load_page(ctx, doc, i);
				auto bounds = fz_bound_page(ctx, page);
				LOG(("MuPDF thumb: page %1 bounds=(%2,%3)-(%4,%5)")
					.arg(i).arg(bounds.x0).arg(bounds.y0)
					.arg(bounds.x1).arg(bounds.y1));
				const auto pageW = bounds.x1 - bounds.x0;
				auto scale = 320.0f / (pageW > 1 ? pageW : 320);
				if (scale > 3.0f) scale = 3.0f;
				const auto ctm = fz_scale(scale, scale);
				auto r = fz_transform_rect(bounds, ctm);
				auto bbox = fz_round_rect(r);
				LOG(("MuPDF thumb: page %1 scale=%2 bbox=%3,%4 %5x%6")
					.arg(i).arg(scale)
					.arg(bbox.x0).arg(bbox.y0)
					.arg(bbox.x1 - bbox.x0)
					.arg(bbox.y1 - bbox.y0));
				pix = fz_new_pixmap_with_bbox(
					ctx, fz_device_rgb(ctx), bbox, NULL, 0);
				LOG(("MuPDF thumb: page %1 pixmap %2x%3 stride=%4")
					.arg(i).arg(pix->w).arg(pix->h).arg(pix->stride));
				fz_clear_pixmap_with_value(ctx, pix, 0xFF);
				auto dev = fz_new_draw_device(ctx, ctm, pix);
				fz_run_page(ctx, page, dev, fz_identity, NULL);
				fz_close_device(ctx, dev);
				fz_drop_device(ctx, dev);
				fz_drop_page(ctx, page);
				page = nullptr;
			} fz_catch(ctx) {
				LOG(("MuPDF thumb: page %1 render failed: %2")
					.arg(i)
					.arg(QString::fromUtf8(fz_caught_message(ctx))));
				fz_drop_page(ctx, page);
				page = nullptr;
				fz_drop_pixmap(ctx, pix);
				pix = nullptr;
			}
		};
		for (auto i = 0; i < maxPages; ++i) {
			renderPage(i);
			if (!pix) continue;
			QImage image(pix->w, pix->h, QImage::Format_RGB888);
			for (auto y = 0; y < pix->h; ++y) {
				auto src = pix->samples + y * pix->stride;
				auto dst = reinterpret_cast<uchar*>(image.scanLine(y));
				for (auto x = 0; x < pix->w; ++x) {
					dst[x * 3 + 0] = src[0];
					dst[x * 3 + 1] = src[1];
					dst[x * 3 + 2] = src[2];
					src += 3;
				}
			}
			{
				auto minR = 255, minG = 255, minB = 255;
				auto maxR = 0, maxG = 0, maxB = 0;
				auto nonWhite = 0;
				for (auto y = 0; y < pix->h; ++y) {
					auto src = pix->samples + y * pix->stride;
					for (auto x = 0; x < pix->w; ++x) {
						if (src[0] < minR) minR = src[0];
						if (src[1] < minG) minG = src[1];
						if (src[2] < minB) minB = src[2];
						if (src[0] > maxR) maxR = src[0];
						if (src[1] > maxG) maxG = src[1];
						if (src[2] > maxB) maxB = src[2];
						if (src[0] < 255 || src[1] < 255 || src[2] < 255)
							++nonWhite;
						src += 3;
					}
				}
			LOG(("MuPDF thumb: page %1 pix stats: "
				"minRGB=(%2,%3,%4) maxRGB=(%5,%6,%7) "
				"nonWhite=%8/%9")
				.arg(i).arg(minR).arg(minG).arg(minB)
				.arg(maxR).arg(maxG).arg(maxB)
				.arg(nonWhite).arg(pix->w * pix->h));
            }    
			{
				constexpr auto kHBands = 10;
				const auto bandH = pix->h / kHBands;
				for (auto b = 0; b < kHBands; ++b) {
					auto bandNonWhite = 0;
					const auto y0 = b * bandH;
					const auto y1 = (b + 1) * bandH;
					for (auto y = y0; y < y1; ++y) {
						auto s = pix->samples + y * pix->stride;
						for (auto x = 0; x < pix->w; ++x) {
							if (s[0] < 255 || s[1] < 255 || s[2] < 255)
								++bandNonWhite;
							s += 3;
						}
					}
					LOG(("MuPDF thumb: page %1 hband[%2] y=%3-%4 nonWhite=%5")
						.arg(i).arg(b).arg(y0).arg(y1 - 1).arg(bandNonWhite));
				}
			}
			{
				constexpr auto kVBands = 10;
				const auto bandW = pix->w / kVBands;
				for (auto b = 0; b < kVBands; ++b) {
					auto bandNonWhite = 0;
					const auto x0 = b * bandW;
					const auto x1 = (b + 1) * bandW;
					for (auto y = 0; y < pix->h; ++y) {
						auto s = pix->samples + y * pix->stride;
						for (auto x = 0; x < pix->w; ++x) {
							if (x >= x0 && x < x1
								&& (s[0] < 255 || s[1] < 255 || s[2] < 255))
								++bandNonWhite;
							s += 3;
						}
					}
					LOG(("MuPDF thumb: page %1 vband[%2] x=%3-%4 nonWhite=%5")
						.arg(i).arg(b).arg(x0).arg(x1 - 1).arg(bandNonWhite));
				}
			}

		if (i == 0 && !image.save("D:/mupdf_thumb_debug.png")) {
			LOG(("MuPDF thumb: FAILED to save debug image"));
		} else if (i == 0) {
			LOG(("MuPDF thumb: saved debug image to D:/mupdf_thumb_debug.png"));
		}
		fz_drop_pixmap(ctx, pix);
		pix = nullptr;
		if (image.isNull()) continue;
		if (lastResortImage.isNull())
			lastResortImage = image;
		const auto sample = image.scaled(
			16, 12,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
		auto hasColor = false;
		auto allSame = true;
		auto contentScore = 0;
		const auto p0 = sample.pixel(0, 0);
		const auto firstR = qRed(p0), firstG = qGreen(p0), firstB = qBlue(p0);
		for (auto y = 0; y < 12; ++y) {
			for (auto x = 0; x < 16; ++x) {
				const auto p = sample.pixel(x, y);
				const auto r = qRed(p), g = qGreen(p), b = qBlue(p);
				if (r != g || r != b)
					hasColor = true;
				if (allSame && (x > 0 || y > 0))
					if (r != firstR || g != firstG || b != firstB)
						allSame = false;
				if (r < 255 || g < 255 || b < 255)
					++contentScore;
			}
		}
		{
			auto colorCount = 0;
			auto firstColoredR = 0, firstColoredG = 0, firstColoredB = 0;
			for (auto y = 0; y < 12 && !colorCount; ++y)
				for (auto x = 0; x < 16 && !colorCount; ++x) {
					const auto p = sample.pixel(x, y);
					const auto r = qRed(p), g = qGreen(p), b = qBlue(p);
					if (r != g || r != b) {
						firstColoredR = r;
						firstColoredG = g;
						firstColoredB = b;
						++colorCount;
					}
				}
			LOG(("MuPDF thumb: page %1 sample firstRGB=(%2,%3,%4) "
				"hasColor=%5 allSame=%6 score=%7 "
				"firstColored=(%8,%9,%10)")
				.arg(i).arg(firstR).arg(firstG).arg(firstB)
				.arg(hasColor).arg(allSame).arg(contentScore)
				.arg(firstColoredR).arg(firstColoredG).arg(firstColoredB));
		}
		if (hasColor && bestColorImage.isNull()) {
			LOG(("MuPDF thumb: page %1 hasColor").arg(i));
			bestColorImage = std::move(image);
			break;
		}
		if (allSame) continue;
		if (contentScore > bestFallbackScore) {
			bestFallbackScore = contentScore;
			bestFallbackImage = std::move(image);
		}
		constexpr auto kGoodEnough = 48;
		if (contentScore >= kGoodEnough) {
			LOG(("MuPDF thumb: page %1 good enough (%2)")
				.arg(i).arg(contentScore));
			break;
		}
		}
	if (fz_is_document_reflowable(ctx, doc)) {
		LOG(("MuPDF thumb: diagnostic 740pt relayout"));
		fz_try(ctx) {
			fz_layout_document(ctx, doc, 740, 400, 12);
		} fz_catch(ctx) {
			LOG(("MuPDF thumb: 740pt layout failed: %1")
				.arg(QString::fromUtf8(fz_caught_message(ctx))));
		}
		if (fz_count_pages(ctx, doc) > 0) {
			const auto savedImage = lastResortImage;
			renderPage(0);
			if (pix) {
				QImage img(pix->w, pix->h, QImage::Format_RGB888);
				for (auto y = 0; y < pix->h; ++y) {
					auto s = pix->samples + y * pix->stride;
					auto d = reinterpret_cast<uchar*>(img.scanLine(y));
					for (auto x = 0; x < pix->w; ++x) {
						d[x * 3 + 0] = s[0];
						d[x * 3 + 1] = s[1];
						d[x * 3 + 2] = s[2];
						s += 3;
					}
				}
				auto w = pix->w, h = pix->h;
				auto minR = 255, minG = 255, minB = 255;
				auto maxR = 0, maxG = 0, maxB = 0, nonWhite = 0;
				for (auto y = 0; y < h; ++y) {
					auto s = pix->samples + y * pix->stride;
					for (auto x = 0; x < w; ++x) {
						if (s[0] < minR) minR = s[0];
						if (s[1] < minG) minG = s[1];
						if (s[2] < minB) minB = s[2];
						if (s[0] > maxR) maxR = s[0];
						if (s[1] > maxG) maxG = s[1];
						if (s[2] > maxB) maxB = s[2];
						if (s[0] < 255 || s[1] < 255 || s[2] < 255)
							++nonWhite;
						s += 3;
					}
				}
				LOG(("MuPDF thumb: 740pt pix stats: "
					"minRGB=(%1,%2,%3) maxRGB=(%4,%5,%6) "
					"nonWhite=%7/%8")
					.arg(minR).arg(minG).arg(minB)
					.arg(maxR).arg(maxG).arg(maxB)
					.arg(nonWhite).arg(w * h));
				{
					constexpr auto kH = 10;
					const auto hh = h / kH;
					for (auto b = 0; b < kH; ++b) {
						auto nw = 0;
						const auto y0 = b * hh;
						const auto y1 = (b + 1) * hh;
						for (auto y = y0; y < y1; ++y) {
							auto s = pix->samples + y * pix->stride;
							for (auto x = 0; x < w; ++x) {
								if (s[0] < 255 || s[1] < 255 || s[2] < 255)
									++nw;
								s += 3;
							}
						}
						LOG(("MuPDF thumb: 740pt hband[%1] y=%2-%3 nonWhite=%4")
							.arg(b).arg(y0).arg(y1 - 1).arg(nw));
					}
				}
				{
					constexpr auto kV = 10;
					const auto vw = w / kV;
					for (auto b = 0; b < kV; ++b) {
						auto nw = 0;
						const auto x0 = b * vw;
						const auto x1 = (b + 1) * vw;
						for (auto y = 0; y < h; ++y) {
							auto s = pix->samples + y * pix->stride;
							for (auto x = 0; x < w; ++x) {
								if (x >= x0 && x < x1
									&& (s[0] < 255 || s[1] < 255 || s[2] < 255))
									++nw;
								s += 3;
							}
						}
						LOG(("MuPDF thumb: 740pt vband[%1] x=%2-%3 nonWhite=%4")
							.arg(b).arg(x0).arg(x1 - 1).arg(nw));
					}
				}
				if (!img.save("D:/mupdf_thumb_debug_740.png")) {
					LOG(("MuPDF thumb: FAILED to save 740pt debug"));
				} else {
					LOG(("MuPDF thumb: saved 740pt debug to D:/mupdf_thumb_debug_740.png"));
				}
				if (!img.isNull())
					lastResortImage = std::move(img);
				fz_drop_pixmap(ctx, pix);
				pix = nullptr;
			}
		}
	}
	fz_drop_document(ctx, doc);
	fz_drop_context(ctx);
	if (!bestColorImage.isNull()) return std::move(bestColorImage);
	if (!bestFallbackImage.isNull()) {
		LOG(("MuPDF thumb: best fallback score=%1").arg(bestFallbackScore));
		return std::move(bestFallbackImage);
	}
	LOG(("MuPDF thumb: returning lastResortImage, size=%1x%2")
		.arg(lastResortImage.width()).arg(lastResortImage.height()));
	return lastResortImage;
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

		const auto isEpub = filepath.endsWith(u".epub"_q, Qt::CaseInsensitive);
		if (isEpub) {
			auto container = zip.readFileContent(
				"META-INF/container.xml",
				zlib::kCaseInsensitive,
				64 * 1024);
			if (!container.isEmpty()) {
				auto opfPath = QString();
				auto r = QXmlStreamReader(container);
				while (!r.atEnd() && !r.hasError()) {
					if (r.readNext() == QXmlStreamReader::StartElement
						&& r.name().toString().compare(
							u"rootfile"_q,
							Qt::CaseInsensitive) == 0) {
						opfPath = r.attributes().value(
							u"full-path"_q).toString();
						break;
					}
				}
				if (!opfPath.isEmpty()) {
					auto opf = zip.readFileContent(
						opfPath.toUtf8().constData(),
						zlib::kCaseSensitive,
						256 * 1024);
					if (!opf.isEmpty()) {
						auto epub3Href = QString();
						auto epub2Id = QString();
						auto items = QMap<QString, QString>();
						auto r2 = QXmlStreamReader(opf);
						while (!r2.atEnd() && !r2.hasError()) {
							r2.readNext();
							if (r2.isStartElement()) {
								const auto tag = r2.name().toString().toLower();
								if (tag == u"item") {
									const auto id = r2.attributes().value(
										u"id"_q).toString();
									const auto href = r2.attributes().value(
										u"href"_q).toString();
									const auto props = r2.attributes().value(
										u"properties"_q).toString();
									if (!id.isEmpty() && !href.isEmpty()) {
										items[id] = href;
									}
									if (!props.isEmpty()
										&& props.split(u' ').contains(
											u"cover-image"_q)) {
										epub3Href = href;
									}
								} else if (tag == u"meta") {
									const auto name = r2.attributes().value(
										u"name"_q).toString();
									const auto content = r2.attributes().value(
										u"content"_q).toString();
									if (name.compare(
										u"cover"_q,
										Qt::CaseInsensitive) == 0) {
										epub2Id = content;
									}
								}
							}
						}
						const auto coverHref = !epub3Href.isEmpty()
							? epub3Href
							: (!epub2Id.isEmpty() && items.contains(epub2Id)
								? items[epub2Id]
								: QString());
						if (!coverHref.isEmpty()) {
							auto resolvePath = [](
									const QString &opfDir,
									const QString &href) {
								if (href.startsWith(u'/'))
									return href.mid(1);
								auto result = opfDir + href;
								auto parts = QStringList();
								for (const auto &p : result.split(u'/')) {
									if (p == u"..") {
										if (!parts.isEmpty())
											parts.removeLast();
									} else if (!p.isEmpty() && p != u".") {
										parts.push_back(p);
									}
								}
								return parts.join(u'/');
							};
							const auto opfDir = opfPath.contains(u'/')
								? opfPath.left(
									opfPath.lastIndexOf(u'/') + 1)
								: QString();
							const auto resolved = resolvePath(
								opfDir,
								coverHref);
							constexpr auto kCoverMax = 10 * 1024 * 1024;
							auto imageData = zip.readFileContent(
								resolved.toUtf8().constData(),
								zlib::kCaseSensitive,
								kCoverMax);
							if (!imageData.isEmpty()) {
								auto image = QImage::fromData(imageData);
								if (!image.isNull()) return image;
							}
						}
					}
				}
			}
			zip.clearError();
		}

		if (zip.goToFirstFile() != UNZ_OK) {
			return QImage();
		}
		auto candidateNames = std::vector<QString>();
		do {
			const auto name = zip.getCurrentFileName();
			const auto lower = name.toLower();
			if (lower.endsWith(u".jpg"_q)
				|| lower.endsWith(u".jpeg"_q)
				|| lower.endsWith(u".png"_q)
				|| lower.endsWith(u".gif"_q)
				|| lower.endsWith(u".bmp"_q)
				|| lower.endsWith(u".webp"_q)
				|| lower.endsWith(u".tiff"_q)
				|| lower.endsWith(u".tif"_q)) {
				candidateNames.push_back(name);
			}
		} while (zip.goToNextFile() == UNZ_OK);

		if (candidateNames.empty()) return QImage();

		auto naturalCmp = [](const QString &a, const QString &b) {
			auto i = 0, j = 0;
			while (i < a.size() && j < b.size()) {
				if (a[i].isDigit() && b[j].isDigit()) {
					auto ai = i;
					while (ai < a.size() && a[ai].isDigit()) ++ai;
					auto bj = j;
					while (bj < b.size() && b[bj].isDigit()) ++bj;
					auto aNum = a.mid(i, ai - i).toULongLong();
					auto bNum = b.mid(j, bj - j).toULongLong();
					if (aNum != bNum) return aNum < bNum;
					i = ai;
					j = bj;
				} else {
					if (a[i].toLower() != b[j].toLower())
						return a[i].toLower() < b[j].toLower();
					++i;
					++j;
				}
			}
			return a.size() < b.size();
		};
		std::sort(
			candidateNames.begin(),
			candidateNames.end(),
			naturalCmp);

		auto isCoverName = [](const QString &name) -> bool {
			const auto lower = name.toLower();
			const auto base = lower.contains(u'/')
				? lower.mid(lower.lastIndexOf(u'/') + 1)
				: lower;
			const auto dot = base.lastIndexOf(u'.');
			const auto stem = (dot >= 0) ? base.left(dot) : base;
			return (stem == u"cover"
				|| stem == u"folder"
				|| stem == u"thumb"
				|| stem == u"thumbnail"
				|| stem == u"front");
		};
		for (const auto &name : candidateNames) {
			if (isCoverName(name)) {
				constexpr auto kMaxSize = 10 * 1024 * 1024;
				auto content = zip.readFileContent(
					name.toUtf8().constData(),
					zlib::kCaseSensitive,
					kMaxSize);
				auto image = QImage::fromData(content);
				if (!image.isNull()) return image;
			}
		}
		constexpr auto kMaxSize = 10 * 1024 * 1024;
		auto content = zip.readFileContent(
			candidateNames.front().toUtf8().constData(),
			zlib::kCaseSensitive,
			kMaxSize);
		return QImage::fromData(content);
	};

	auto tryExtractMobiCover = [&] {
		auto file = QFile(filepath);
		if (!file.open(QIODevice::ReadOnly)) {
			return QImage();
		}
		const auto bytes = file.readAll();
		file.close();
		if (bytes.size() < 86) return QImage();

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

		if (memcmp(bytes.constData() + 60, "BOOK", 4)
			|| memcmp(bytes.constData() + 64, "MOBI", 4)) {
			return QImage();
		}

		const auto numRecords = read16(76);
		if (numRecords < 1) return QImage();
		const auto rec0Offset = read32(78);

		// Determine first resource (image) record index.
		auto firstResource = (uint32)-1;
		if (rec0Offset + 0x6C + 4 <= bytes.size()
			&& memcmp(bytes.constData() + rec0Offset + 16, "MOBI", 4) == 0) {
			const auto mobiLen = read32(rec0Offset + 20);
			if (mobiLen >= 0x6C - 16 + 4) {
				const auto fr = read32(rec0Offset + 0x6C);
				if (fr != 0xFFFFFFFF) firstResource = fr;
			}
		}
		if (firstResource == (uint32)-1) {
			firstResource = read16(rec0Offset + 8) + 1;
		}

		// EXTH records can contain cover data (type 201 = cover offset, 202 = cover data).
		const auto rec0End = (numRecords > 1)
			? read32(78 + 8)
			: (int)bytes.size();
		for (auto i = rec0Offset; (i < rec0End - 12) && i < bytes.size(); ++i) {
			if (memcmp(bytes.constData() + i, "EXTH", 4) != 0) continue;
			const auto exthLen = read32(i + 4);
			if (exthLen < 12 || exthLen > (rec0End - i)) continue;
			const auto exthCount = read32(i + 8);
			auto pos = i + 12;
			for (auto j = 0u; j < exthCount; ++j) {
				if (pos + 8 > bytes.size()) break;
				const auto type = read32(pos);
				const auto length = read32(pos + 4);
				if (length < 8 || pos + length > bytes.size()) break;
				const auto data = bytes.mid(pos + 8, length - 8);
				pos += length;

				if (type == 202 && data.size() > 8) {
					auto image = QImage::fromData(data);
					if (image.isNull()) image = QImage::fromData(data.mid(8));
					if (!image.isNull()) return image;
				}
				if ((type == 201 && data.size() >= 4)
					|| (type == 202 && data.size() == 4)) {
					const auto coverIdx = read32(pos - length + 8);
					if (firstResource == (uint32)-1) continue;
					const auto absoluteIdx = firstResource + coverIdx;
					if (absoluteIdx >= (uint32)numRecords) continue;
					const auto off = read32(78 + absoluteIdx * 8);
					const auto end = (absoluteIdx + 1 < (uint32)numRecords)
						? read32(78 + (absoluteIdx + 1) * 8)
						: (int)bytes.size();
				if (off < (int)bytes.size() && end > off) {
					if (off + 4 <= bytes.size()) {
						const auto *m = (const unsigned char *)bytes.constData() + off;
						const auto isMarker
							= (m[0] == 'F' && m[1] == 'L' && m[2] == 'I' && m[3] == 'S')
							|| (m[0] == 'F' && m[1] == 'C' && m[2] == 'I' && m[3] == 'S')
							|| (m[0] == 'S' && m[1] == 'R' && m[2] == 'C' && m[3] == 'S')
							|| (m[0] == 'R' && m[1] == 'E' && m[2] == 'S' && m[3] == 'C')
							|| (m[0] == 'B' && m[1] == 'O' && m[2] == 'U' && m[3] == 'N')
							|| (m[0] == 'F' && m[1] == 'D' && m[2] == 'S' && m[3] == 'T')
							|| (m[0] == 'D' && m[1] == 'A' && m[2] == 'T' && m[3] == 'P')
							|| (m[0] == 'A' && m[1] == 'U' && m[2] == 'D' && m[3] == 'I')
							|| (m[0] == 'V' && m[1] == 'I' && m[2] == 'D' && m[3] == 'E')
							|| (m[0] == 0xE9 && m[1] == 0x8E && m[2] == 0x0D && m[3] == 0x0A);
						if (isMarker) continue;
					}
					auto image = QImage::fromData(bytes.mid(off, end - off));
					if (!image.isNull()) return image;
				}
				}
			}
			break;
		}

		// Scan all records for image data.
		for (auto idx = 0; idx < numRecords; ++idx) {
			const auto off = read32(78 + idx * 8);
			const auto end = (idx + 1 < numRecords)
				? read32(78 + (idx + 1) * 8)
				: (int)bytes.size();
			if (off + 2 > (int)bytes.size() || end <= off) continue;
			const auto b = (const unsigned char *)bytes.constData() + off;
			const auto isJpeg = (b[0] == 0xFF && b[1] == 0xD8);
			const auto isPng = (b[0] == 0x89 && b[1] == 0x50);
			const auto isGif = (b[0] == 'G' && b[1] == 'I');
			if (!isJpeg && !isPng && !isGif) continue;
			auto image = QImage::fromData(bytes.mid(off, end - off));
			if (!image.isNull()
				&& image.width() >= 100
				&& image.height() >= 100) {
				return image;
			}
		}
		return QImage();
	};

	auto tryExtractRarCover = [&] {
		auto list = QProcess();
		list.start(u"unrar"_q, {
			u"lb"_q, u"-c-"_q, u"-p-"_q, filepath
		});
		list.waitForFinished(10000);
		if (list.exitCode() != 0) return QImage();
		const auto lines = QString::fromUtf8(
			list.readAllStandardOutput()).split(u'\n', Qt::SkipEmptyParts);

		auto candidates = std::vector<QString>();
		for (const auto &line : lines) {
			const auto name = line.trimmed();
			const auto lower = name.toLower();
			if (lower.endsWith(u".jpg"_q)
				|| lower.endsWith(u".jpeg"_q)
				|| lower.endsWith(u".png"_q)
				|| lower.endsWith(u".gif"_q)
				|| lower.endsWith(u".webp"_q)) {
				candidates.push_back(name);
			}
		}
		if (candidates.empty()) return QImage();

		std::sort(candidates.begin(), candidates.end());
		for (const auto &name : candidates) {
			auto extract = QProcess();
			extract.start(u"unrar"_q, {
				u"p"_q, u"-c-"_q, u"-p-"_q, u"-inul"_q, filepath, name
			});
			extract.waitForFinished(10000);
			if (extract.exitCode() != 0) continue;
			auto image = QImage::fromData(
				extract.readAllStandardOutput());
			if (!image.isNull()
				&& image.width() >= 100
				&& image.height() >= 100) {
				return image;
			}
		}
		return QImage();
	};

	auto image = [&]() -> QImage {
		if (filepath.endsWith(u".pdf"_q, Qt::CaseInsensitive)) {
			return tryRenderViaMuPDF();
		}
		if (filepath.endsWith(u".epub"_q, Qt::CaseInsensitive)
			|| filepath.endsWith(u".cbz"_q, Qt::CaseInsensitive)) {
			auto result = tryExtractZipCover();
			if (!result.isNull()) return result;
		}
		if (filepath.endsWith(u".mobi"_q, Qt::CaseInsensitive)
			|| filepath.endsWith(u".prc"_q, Qt::CaseInsensitive)) {
			auto result = tryExtractMobiCover();
			if (!result.isNull()) return result;
		}
		if (filepath.endsWith(u".cbr"_q, Qt::CaseInsensitive)) {
			auto result = tryExtractRarCover();
			if (!result.isNull()) return result;
		}
		return tryRenderViaMuPDF();
	}();
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

	if (filename.endsWith(u".htm"_q, Qt::CaseInsensitive)) {
		attributes[0] = MTP_documentAttributeFilename(MTP_string(
			QString(filename).chopped(4) + u"[htm].xhtml"_q));
	} else if (filename.endsWith(u".html"_q, Qt::CaseInsensitive)) {
		attributes[0] = MTP_documentAttributeFilename(MTP_string(
			QString(filename).chopped(5) + u"[html].xhtml"_q));
	}

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
				if (filename.endsWith(u".webm"_q, Qt::CaseInsensitive)) {
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
