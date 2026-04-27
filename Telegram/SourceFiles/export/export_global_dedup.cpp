/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/export_global_dedup.h"

#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QDir>

namespace Export {

namespace {
	constexpr auto kCurrentVersion = 1;
	constexpr auto kBackupSuffix = QLatin1String(".backup");

	QString makeDocumentIdKey(uint64 docId) {
		return QLatin1String("i:") + QString::number(docId);
	}

	QString makeFingerprintKey(const QString &filename, int64 size) {
		return QLatin1String("s:") + filename + QLatin1String("_") + QString::number(size);
	}
}

GlobalDedupManager::GlobalDedupManager(const QString &exportPath)
: _path(globalDedupFilePath(exportPath)) {
}

QString GlobalDedupManager::filePath() const {
	return _path;
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

bool GlobalDedupManager::load(const QString &exportPath) {
	_path = globalDedupFilePath(exportPath);
	return load();
}

bool GlobalDedupManager::load() const {
	if (_path.isEmpty()) {
		return false;
	}

	QFile file(_path);
	if (!file.open(QIODevice::ReadOnly)) {
		// No existing file is OK - just empty state
		return true;
	}

	const auto data = file.readAll();
	const auto doc = QJsonDocument::fromJson(data);

	if (doc.isNull() || !doc.isObject()) {
		// Try backup file
		const auto backupPath = _path + kBackupSuffix;
		QFile backup(backupPath);
		if (backup.open(QIODevice::ReadOnly)) {
			const auto backupData = backup.readAll();
			const auto backupDoc = QJsonDocument::fromJson(backupData);
			if (!backupDoc.isNull() && backupDoc.isObject()) {
				const_cast<GlobalDedupManager*>(this)->buildFromJson(backupDoc.object());
				return true;
			}
		}
		return false;
	}

	const_cast<GlobalDedupManager*>(this)->buildFromJson(doc.object());
	return true;
}

void GlobalDedupManager::buildFromJson(const QJsonObject &obj) {
	const auto version = obj.value(QLatin1String("version")).toInt(0);
	if (version != kCurrentVersion) {
		// Future: handle version migration
		// For now, treat as empty
		if (version == 0) {
			return;
		}
	}

	const auto entriesObj = obj.value(QLatin1String("entries")).toObject();
	for (auto it = entriesObj.begin(); it != entriesObj.end(); ++it) {
		const auto key = it.key();
		if (key.startsWith(QLatin1String("i:"))) {
			// Document ID entry
			const auto docIdStr = key.mid(2);
			bool ok = false;
			const auto docId = docIdStr.toULongLong(&ok);
			if (ok) {
				_documentIds.insert(docId);
			}
		} else if (key.startsWith(QLatin1String("s:"))) {
			// Fingerprint entry
			_fingerprints.insert(key.mid(2));
		}
	}
}

bool GlobalDedupManager::save() const {
	if (_path.isEmpty()) {
		return false;
	}
	return atomicSave(_path);
}

bool GlobalDedupManager::atomicSave(const QString &path) const {
	// Ensure directory exists
	const auto dir = QFileInfo(path).path();
	if (!QDir().mkpath(dir)) {
		return false;
	}

	// Create backup before overwriting
	const auto backupPath = path + kBackupSuffix;
	QFile current(path);
	if (current.exists()) {
		QFile::remove(backupPath);
		QFile::copy(path, backupPath);
	}

	// Build JSON
	QJsonObject entries;
	for (const auto docId : _documentIds) {
		entries[makeDocumentIdKey(docId)] = 1;
	}
	for (const auto &fp : _fingerprints) {
		entries[QLatin1String("s:") + fp] = 1;
	}

	QJsonObject root;
	root[QLatin1String("version")] = kCurrentVersion;
	root[QLatin1String("entries")] = entries;

	// Write to temp file
	const auto tempPath = path + QLatin1String(".tmp");
	QFile temp(tempPath);
	if (!temp.open(QIODevice::WriteOnly)) {
		return false;
	}

	const auto data = QJsonDocument(root).toJson();
	temp.write(data);
	temp.flush();

	// Atomic rename: Rename temp to final. 
	// On Windows, rename() fails if destination exists, so we must remove it.
	// But we have the backup created above, so data is safe.
	QFile::remove(path);
	if (!temp.rename(path)) {
		QFile::remove(tempPath);
		return false;
	}

	return true;
}

bool GlobalDedupManager::hasDocumentId(uint64 docId) const {
	return _documentIds.find(docId) != _documentIds.end();
}

bool GlobalDedupManager::hasFingerprint(const QString &filename, int64 size) const {
	return hasFingerprint(makeFingerprintKey(filename, size));
}

bool GlobalDedupManager::hasFingerprint(const QString &fingerprint) const {
	return _fingerprints.find(fingerprint) != _fingerprints.end();
}

void GlobalDedupManager::addEntry(uint64 docId, const QString &filename, int64 size) {
	addDocumentId(docId);
	addFingerprint(filename, size);
}

void GlobalDedupManager::addDocumentId(uint64 docId) {
	_documentIds.insert(docId);
}

void GlobalDedupManager::addFingerprint(const QString &filename, int64 size) {
	addFingerprint(makeFingerprintKey(filename, size));
}

void GlobalDedupManager::addFingerprint(const QString &fingerprint) {
	_fingerprints.insert(fingerprint);
}

size_t GlobalDedupManager::entryCount() const {
	return _documentIds.size() + _fingerprints.size();
}

void GlobalDedupManager::clear() {
	_documentIds.clear();
	_fingerprints.clear();
}

} // namespace Export
