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
	// Check date range validity.
	if (singlePeerFrom > 0 && singlePeerTill > 0 && singlePeerTill <= singlePeerFrom) {
		return false;
	}
	
	// Check ID range validity.
	if (useIdRange) {
		// When using ID range, From ID must be >= 1 if specified
		if (singlePeerFromId > 0 && singlePeerFromId < 1) {
			return false;
		}
		// To ID must be >= From ID if both are specified
		if (singlePeerFromId > 0 && singlePeerTillId > 0 && singlePeerTillId < singlePeerFromId) {
			return false;
		}
	}
	
	// Ensure only one export mode is active
	if (useIdRange && (singlePeerFrom > 0 || singlePeerTill > 0)) {
		return false;
	}
	
	if (!useIdRange && (singlePeerFromId > 0 || singlePeerTillId > 0)) {
		// If ID fields are set but useIdRange is false, that's invalid
		return false;
	}

	if (onlySinglePeer()) {
		if (media.types == MediaSettings::Types(0)) {
			return false;
		}
	} else {
		if (types == Types(0) && media.types == MediaSettings::Types(0)) {
			return false;
		}
	}

	if (!media.validate()) {
		return false;
	}
	return true;
};

} // namespace Export
