/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "export/export_settings.h"
#include "base/flat_set.h"

#include <QtCore/QString>
#include <QtCore/QJsonObject>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <vector>

namespace Export {
namespace Output {
struct StatItem;
} // namespace Output

// Unified deduplication and statistics manager
// Handles three modes:
// - Disabled: No dedup (text-only exports)
// - MemoryOnly: In-memory dedup for statistics (links/fullhistory)
// - Persistent: Disk-backed dedup with global_dedup.json (media exports)
class GlobalDedupManager {
public:
	enum class Mode {
		Disabled,      // Text-only exports (no dedup, no stats)
		MemoryOnly,    // Links/FullHistory (stats only, no persistence)
		Persistent     // Media exports (with global_dedup.json)
	};

	struct StatItem {
		std::atomic<int64> totalCount{0};
		std::atomic<int64> totalSize{0};
		std::atomic<int64> uniqueCount{0};
		std::atomic<int64> uniqueSize{0};
		std::atomic<int> messagesWithLinks{0};
	};

	struct Stats {
		std::map<MediaSettings::Type, StatItem> byType;

		std::atomic<int64> totalMediaCount{0};
		std::atomic<int64> totalMediaSize{0};
		std::atomic<int64> uniqueMediaCount{0};
		std::atomic<int64> uniqueMediaSize{0};

		std::atomic<int64> totalMessages{0};
		std::atomic<int64> inProgressCount{0};
		std::atomic<int64> filesWritten{0};
		std::atomic<int64> bytesWritten{0};

		[[nodiscard]] int64 duplicateMediaCount() const {
			return totalMediaCount.load(std::memory_order_relaxed) 
				- uniqueMediaCount.load(std::memory_order_relaxed);
		}
		[[nodiscard]] int64 duplicateMediaSize() const {
			return totalMediaSize.load(std::memory_order_relaxed) 
				- uniqueMediaSize.load(std::memory_order_relaxed);
		}
	};

	GlobalDedupManager() = default;
	explicit GlobalDedupManager(Mode mode, const QString &exportPath = QString());

	// Core dedup operations
	[[nodiscard]] bool isKnown(
		uint64 docId,
		int64 size,
		const QString &name,
		MediaSettings::Type type);
	
	// Special overload for links (no docId/size, just URL)
	[[nodiscard]] bool isKnownLink(const QString &url);
	
	void markInProgress(
		uint64 docId,
		int64 size,
		const QString &name,
		MediaSettings::Type type);
	void finalize(
		uint64 docId,
		int64 size,
		const QString &name,
		MediaSettings::Type type);
	
	// Special overload for links
	void finalizeLink(const QString &url);
	
	// Get all unique links (for writing to file)
	[[nodiscard]] base::flat_set<QString> getUniqueLinks() const;
	void cancelInProgress(
		uint64 docId,
		int64 size,
		const QString &name,
		MediaSettings::Type type);

	// Message and file tracking
	void incrementTotalMessages();
	void incrementFilesWritten();
	void incrementBytesWritten(int64 bytes);
	void setMessagesWithLinks(MediaSettings::Type type, int count);
	void incrementMessagesWithLinks(MediaSettings::Type type, int count = 1);
	void incrementTotal(MediaSettings::Type type, int64 size);

	// Statistics
	[[nodiscard]] std::map<MediaSettings::Type, Output::StatItem> statsByType() const;
	[[nodiscard]] int totalMessagesCount() const;
	void resetStats();

	// State management
	void clearInProgress();
	bool save();
	[[nodiscard]] Mode mode() const;
	[[nodiscard]] QString lastError() const;

	// Static helpers
	[[nodiscard]] static QString documentIdKey(uint64 docId);
	[[nodiscard]] static QString fingerprintKey(const QString &filename, int64 size);
	[[nodiscard]] static QString globalDedupFilePath(const QString &exportPath);

private:
	// Build internal structures from JSON
	void buildFromJson(const QJsonObject &obj);

	// Atomic save implementation
	bool atomicSave(const QString &path) const;

	// Update aggregate stats from per-type stats
	void updateAggregateStats();

	Mode _mode = Mode::Disabled;
	mutable QString _path;
	QString _lastError;
	Stats _stats;

	// In-progress tracking (all modes)
	std::unordered_set<QString> _inProgress;
	std::unordered_map<QString, int64> _inProgressSizes;

	// MemoryOnly mode: permanent memory storage
	std::unordered_set<QString> _memoryDedup;

	// Persistent mode: disk-backed storage
	std::unordered_set<QString> _persistent;
};

} // namespace Export
