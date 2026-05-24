/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/export_global_dedup.h"

#include "export/output/export_output_stats.h"
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QDir>

namespace Export {

namespace {
	constexpr auto kBackupSuffix = QLatin1String(".backup");

	QString makeDocumentIdKey(uint64 docId) {
		return QLatin1String("i:") + QString::number(docId);
	}

	QString makeFingerprintKey(const QString &filename, int64 size) {
		return QLatin1String("s:") + filename + QLatin1String("_") + QString::number(size);
	}
}

GlobalDedupManager::GlobalDedupManager(Mode mode, const QString &exportPath)
: _mode(mode)
, _path(mode == Mode::Persistent ? globalDedupFilePath(exportPath) : QString()) {
	if (_mode == Mode::Persistent && !_path.isEmpty()) {
		QFile file(_path);
		if (!file.open(QIODevice::ReadOnly)) {
			return;
		}

		const auto data = file.readAll();
		const auto doc = QJsonDocument::fromJson(data);

		if (doc.isNull() || !doc.isObject()) {
			const auto backupPath = _path + kBackupSuffix;
			QFile backup(backupPath);
			if (backup.open(QIODevice::ReadOnly)) {
				const auto backupData = backup.readAll();
				const auto backupDoc = QJsonDocument::fromJson(backupData);
				if (!backupDoc.isNull() && backupDoc.isObject()) {
					buildFromJson(backupDoc.object());
					return;
				}
			}
			_lastError = u"Export failed: Deduplication database is corrupted. Please delete global_dedup.json and try again."_q;
			return;
		}

		buildFromJson(doc.object());
	}
}

QString GlobalDedupManager::globalDedupFilePath(const QString &exportPath) {
	QString path = exportPath;
	if (!path.endsWith('/')) {
		path += '/';
	}
	return path + QLatin1String("global_dedup.json");
}

QString GlobalDedupManager::documentIdKey(uint64 docId) {
	return makeDocumentIdKey(docId);
}

QString GlobalDedupManager::fingerprintKey(const QString &filename, int64 size) {
	return makeFingerprintKey(filename, size);
}

void GlobalDedupManager::buildFromJson(const QJsonObject &obj) {
	const auto entriesArray = obj.value(QLatin1String("entries")).toArray();
	for (const auto &val : entriesArray) {
		const auto key = val.toString();
		if (!key.isEmpty()) {
			_persistent.insert(key);
		}
	}
}

bool GlobalDedupManager::isKnown(
		uint64 docId,
		int64 size,
		const QString &name,
		MediaSettings::Type type) {
	if (_mode == Mode::Disabled) {
		return false;
	}

	const auto docKey = makeDocumentIdKey(docId);
	const auto fpKey = makeFingerprintKey(name, size);

	const bool alreadyInProgress = _inProgress.contains(docKey) || _inProgress.contains(fpKey);
	const bool alreadyKnown = (_mode == Mode::MemoryOnly && (_memoryDedup.contains(docKey) || _memoryDedup.contains(fpKey)))
		|| (_mode == Mode::Persistent && (_persistent.contains(docKey) || _persistent.contains(fpKey)));

	if (alreadyInProgress) {
		return true;
	}

	return alreadyKnown;
}

bool GlobalDedupManager::isKnownLink(const QString &url) {
	if (_mode == Mode::Disabled) {
		return false;
	}

	auto &item = _stats.byType[MediaSettings::Type::Link];
	item.totalCount.fetch_add(1, std::memory_order_relaxed);

	const auto linkKey = QLatin1String("l:") + url;
	
	if (_mode == Mode::MemoryOnly) {
		return _memoryDedup.contains(linkKey);
	} else if (_mode == Mode::Persistent) {
		return _persistent.contains(linkKey);
	}

	return false;
}

void GlobalDedupManager::markInProgress(
		uint64 docId,
		int64 size,
		const QString &name,
		MediaSettings::Type type) {
	if (_mode == Mode::Disabled) {
		return;
	}

	const auto docKey = makeDocumentIdKey(docId);
	const auto fpKey = makeFingerprintKey(name, size);

	if (_inProgress.insert(docKey).second) {
		_inProgressSizes[docKey] = size;
		_stats.inProgressCount.fetch_add(1, std::memory_order_relaxed);
	}
	if (_inProgress.insert(fpKey).second) {
		_inProgressSizes[fpKey] = size;
	}
}

void GlobalDedupManager::finalize(
		uint64 docId,
		int64 size,
		const QString &name,
		MediaSettings::Type type) {
	if (_mode == Mode::Disabled) {
		return;
	}

	const auto docKey = makeDocumentIdKey(docId);
	const auto fpKey = makeFingerprintKey(name, size);

	bool wasInProgress = false;
	if (_inProgress.erase(docKey)) {
		wasInProgress = true;
		_stats.inProgressCount.fetch_sub(1, std::memory_order_relaxed);
		_inProgressSizes.erase(docKey);
	}
	if (_inProgress.erase(fpKey)) {
		_inProgressSizes.erase(fpKey);
	}

	if (_mode == Mode::MemoryOnly) {
		_memoryDedup.insert(docKey);
		_memoryDedup.insert(fpKey);
	} else if (_mode == Mode::Persistent) {
		_persistent.insert(docKey);
		_persistent.insert(fpKey);
	}

	if (wasInProgress) {
		auto &item = _stats.byType[type];
		item.uniqueCount.fetch_add(1, std::memory_order_relaxed);
		item.uniqueSize.fetch_add(size, std::memory_order_relaxed);
		updateAggregateStats();
	}
}

void GlobalDedupManager::cancelInProgress(
		uint64 docId,
		int64 size,
		const QString &name,
		MediaSettings::Type type) {
	if (_mode == Mode::Disabled) {
		return;
	}

	const auto docKey = makeDocumentIdKey(docId);
	const auto fpKey = makeFingerprintKey(name, size);

	if (_inProgress.erase(docKey)) {
		_stats.inProgressCount--;
		_inProgressSizes.erase(docKey);
	}
	if (_inProgress.erase(fpKey)) {
		_inProgressSizes.erase(fpKey);
	}
}

void GlobalDedupManager::finalizeLink(const QString &url) {
	if (_mode == Mode::Disabled) {
		return;
	}

	const auto linkKey = QLatin1String("l:") + url;
	
	bool isNew = false;
	if (_mode == Mode::MemoryOnly) {
		isNew = _memoryDedup.insert(linkKey).second;
	} else if (_mode == Mode::Persistent) {
		isNew = _persistent.insert(linkKey).second;
	}

	if (isNew) {
		auto &item = _stats.byType[MediaSettings::Type::Link];
		item.uniqueCount.fetch_add(1, std::memory_order_relaxed);
		updateAggregateStats();
	}
}

base::flat_set<QString> GlobalDedupManager::getUniqueLinks() const {
	base::flat_set<QString> result;
	
	const auto &storage = (_mode == Mode::MemoryOnly) ? _memoryDedup : _persistent;
	
	for (const auto &key : storage) {
		if (key.startsWith(QLatin1String("l:"))) {
			result.insert(key.mid(2)); // Remove "l:" prefix
		}
	}
	
	return result;
}

void GlobalDedupManager::incrementTotalMessages() {
	_stats.totalMessages.fetch_add(1, std::memory_order_relaxed);
}

void GlobalDedupManager::incrementFilesWritten() {
	_stats.filesWritten.fetch_add(1, std::memory_order_relaxed);
}

void GlobalDedupManager::incrementBytesWritten(int64 bytes) {
	_stats.bytesWritten.fetch_add(bytes, std::memory_order_relaxed);
}

void GlobalDedupManager::setMessagesWithLinks(MediaSettings::Type type, int count) {
	_stats.byType[type].messagesWithLinks.store(count, std::memory_order_relaxed);
}

void GlobalDedupManager::incrementMessagesWithLinks(MediaSettings::Type type, int count) {
	_stats.byType[type].messagesWithLinks.fetch_add(count, std::memory_order_relaxed);
}

void GlobalDedupManager::incrementTotal(MediaSettings::Type type, int64 size) {
	auto &item = _stats.byType[type];
	item.totalCount.fetch_add(1, std::memory_order_relaxed);
	item.totalSize.fetch_add(size, std::memory_order_relaxed);
	updateAggregateStats();
}

void GlobalDedupManager::resetStats() {
	for (auto &[type, item] : _stats.byType) {
		item.totalCount.store(0, std::memory_order_relaxed);
		item.totalSize.store(0, std::memory_order_relaxed);
		item.uniqueCount.store(0, std::memory_order_relaxed);
		item.uniqueSize.store(0, std::memory_order_relaxed);
		item.messagesWithLinks.store(0, std::memory_order_relaxed);
	}
	_stats.totalMediaCount.store(0, std::memory_order_relaxed);
	_stats.totalMediaSize.store(0, std::memory_order_relaxed);
	_stats.uniqueMediaCount.store(0, std::memory_order_relaxed);
	_stats.uniqueMediaSize.store(0, std::memory_order_relaxed);
	_stats.totalMessages.store(0, std::memory_order_relaxed);
	_stats.inProgressCount.store(0, std::memory_order_relaxed);
	_stats.filesWritten.store(0, std::memory_order_relaxed);
	_stats.bytesWritten.store(0, std::memory_order_relaxed);
}

std::map<MediaSettings::Type, Output::StatItem> GlobalDedupManager::statsByType() const {
	std::map<MediaSettings::Type, Output::StatItem> result;
	
	for (const auto &[type, item] : _stats.byType) {
		Output::StatItem resultItem;
		resultItem.uniqueCount = static_cast<int>(item.uniqueCount.load(std::memory_order_relaxed));
		resultItem.uniqueSize = item.uniqueSize.load(std::memory_order_relaxed);
		resultItem.totalCount = static_cast<int>(item.totalCount.load(std::memory_order_relaxed));
		resultItem.totalSize = item.totalSize.load(std::memory_order_relaxed);
		resultItem.messagesWithLinks = item.messagesWithLinks.load(std::memory_order_relaxed);
		result[type] = resultItem;
	}
	
	return result;
}

int GlobalDedupManager::totalMessagesCount() const {
	return static_cast<int>(_stats.totalMessages.load(std::memory_order_relaxed));
}

void GlobalDedupManager::clearInProgress() {
	_inProgress.clear();
	_inProgressSizes.clear();
	_stats.inProgressCount.store(0, std::memory_order_relaxed);
}

bool GlobalDedupManager::save() {
	if (_mode != Mode::Persistent || _path.isEmpty()) {
		return true;
	}

	const auto dir = QFileInfo(_path).path();
	if (!QDir().mkpath(dir)) {
		return false;
	}

	const auto backupPath = _path + kBackupSuffix;
	QFile current(_path);
	if (current.exists()) {
		QFile::remove(backupPath);
		QFile::copy(_path, backupPath);
	}

	QJsonArray entries;
	for (const auto &entry : _persistent) {
		entries.append(entry);
	}

	QJsonObject root;
	root[QLatin1String("entries")] = entries;

	const auto tempPath = _path + QLatin1String(".tmp");
	QFile temp(tempPath);
	if (!temp.open(QIODevice::WriteOnly)) {
		return false;
	}

	const auto data = QJsonDocument(root).toJson();
	temp.write(data);
	temp.flush();

	QFile::remove(_path);
	if (!temp.rename(_path)) {
		QFile::remove(tempPath);
		return false;
	}

	return true;
}

GlobalDedupManager::Mode GlobalDedupManager::mode() const {
	return _mode;
}

QString GlobalDedupManager::lastError() const {
	return _lastError;
}

void GlobalDedupManager::updateAggregateStats() {
	int64 totalMedia = 0;
	int64 totalSize = 0;
	int64 uniqueMedia = 0;
	int64 uniqueSize = 0;

	for (const auto &[type, item] : _stats.byType) {
		if (type != MediaSettings::Type::Text && type != MediaSettings::Type::Link) {
			totalMedia += item.totalCount.load(std::memory_order_relaxed);
			totalSize += item.totalSize.load(std::memory_order_relaxed);
			uniqueMedia += item.uniqueCount.load(std::memory_order_relaxed);
			uniqueSize += item.uniqueSize.load(std::memory_order_relaxed);
		}
	}

	_stats.totalMediaCount.store(totalMedia, std::memory_order_relaxed);
	_stats.totalMediaSize.store(totalSize, std::memory_order_relaxed);
	_stats.uniqueMediaCount.store(uniqueMedia, std::memory_order_relaxed);
	_stats.uniqueMediaSize.store(uniqueSize, std::memory_order_relaxed);
}

} // namespace Export
