/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/export_settings.h"

#include "export/output/export_output_abstract.h"

namespace Export {
namespace {

constexpr auto kMaxFileSize = 4000 * int64(1024 * 1024);

} // namespace

bool MediaSettings::validate() const {
	if ((types | Type::AllMask) != Type::AllMask) {
		return false;
	} else if (sizeLimit < 0 || sizeLimit > kMaxFileSize) {
		return false;
	}
	// Extension filter only valid when eligible types are selected
	if (extensionFilterMode != ExtFilterMode::None) {
		const auto eligible = Type::Video | Type::Audio | Type::File;
		if (!(types & eligible)) {
			return false;
		}
	}
	return true;
}

bool Settings::validate() const {
	// Ensure exactly one range mode is active
	if (useDateRange == useIdRange) {
		return false; // Both true or both false is invalid
	}
	
	// Check date range validity (if both values are specified)
	if (singlePeerFrom.has_value() && singlePeerTill.has_value() 
		&& *singlePeerTill <= *singlePeerFrom) {
		return false;
	}
	
	// Check ID range validity (if both values are specified)
	if (useIdRange) {
		// When using ID range, From ID must be >= 1 if specified
		if (singlePeerFromId.has_value() && *singlePeerFromId < 1) {
			return false;
		}
		// To ID must be >= From ID if both are specified
		if (singlePeerFromId.has_value() && singlePeerTillId.has_value() 
			&& *singlePeerTillId < *singlePeerFromId) {
			return false;
		}
	}
	
	// If date values are set, useDateRange should be true
	if (hasDateRange() && !useDateRange) {
		return false;
	}
	
	// If ID values are set, useIdRange should be true
	if (hasIdRange() && !useIdRange) {
		return false;
	}

	if (!media.validate()) {
		return false;
	}
	return true;
};

} // namespace Export
