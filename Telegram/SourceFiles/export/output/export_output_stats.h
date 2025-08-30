/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <atomic>

namespace Export {
namespace Output {

class Stats {
public:
	Stats() = default;
	Stats(const Stats &other);

	void incrementFiles();
	void incrementBytes(int count);
	void  incrementUserMediaFiles(); 

	int filesCount() const;
	int64 bytesCount() const;
	int   userMediaFilesCount() const;

private:
	std::atomic<int> _files;
	std::atomic<int64> _bytes;
	std::atomic<int>   _userMediaFiles; 

};

} // namespace Output
} // namespace Export
