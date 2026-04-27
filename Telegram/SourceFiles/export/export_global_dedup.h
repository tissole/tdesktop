/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>
#include <QtCore/QJsonObject>
#include <unordered_set>

namespace Export {

// Cross-chat global deduplication manager
// Stores media fingerprints in {export_path}/global_dedup.json
// Format: {"version": 1, "entries": {"i:docId": 1, "s:filename_size": 1}}
class GlobalDedupManager {
public:
	GlobalDedupManager() = default;
	explicit GlobalDedupManager(const QString &exportPath);

	// Load from file
	bool load(const QString &exportPath);
	bool load() const; // Uses already set path

	// Save to file (atomic write)
	bool save() const;

	// Check if document ID exists
	[[nodiscard]] bool hasDocumentId(uint64 docId) const;

	// Check if size+name fingerprint exists
	[[nodiscard]] bool hasFingerprint(const QString &filename, int64 size) const;
	[[nodiscard]] bool hasFingerprint(const QString &fingerprint) const;

	// Add new entry (both document ID and fingerprint)
	void addEntry(uint64 docId, const QString &filename, int64 size);

	// Add just document ID
	void addDocumentId(uint64 docId);

	// Add just fingerprint
	void addFingerprint(const QString &filename, int64 size);
	void addFingerprint(const QString &fingerprint);

	// Get total entries count
	[[nodiscard]] size_t entryCount() const;

	// Clear all entries
	void clear();

	// Get file path
	[[nodiscard]] QString filePath() const;

	// Static helpers
	[[nodiscard]] static QString documentIdKey(uint64 docId);
	[[nodiscard]] static QString fingerprintKey(const QString &filename, int64 size);
	[[nodiscard]] static QString globalDedupFilePath(const QString &exportPath);

private:
	// Build internal structures from JSON
	void buildFromJson(const QJsonObject &obj);

	// Atomic save implementation
	bool atomicSave(const QString &path) const;

	mutable QString _path;
	std::unordered_set<uint64> _documentIds;
	std::unordered_set<QString> _fingerprints;
};

} // namespace Export
