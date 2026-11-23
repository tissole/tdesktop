/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/unique_function.h"
#include "base/weak_ptr.h"
#include "data/data_msg_id.h"
#include "data/data_peer_id.h"
#include "mtproto/core_types.h"
#include "scheme.h"

#include <QByteArray>
#include <QString>
#include <vector>

namespace Main {
class Session;
} // namespace Main

class PeerData;

namespace BulkForward {

// One message created by the carrier copy, paired with its source message and
// the content fingerprint computed on the copy path.
struct CarrierItem {
	FullMsgId source;
	PeerId carrierPeer;
	MsgId carrierId;
	uint64 mediaId = 0;
	bool isPhoto = false;
	QByteArray hash;
};

// Forwards carrier copies to one destination chat with the author dropped.
// Only that destination's own records are consulted: the content was already
// approved once for the run, so the global uploads table must not skip it.
class CarrierForwarder final : public base::has_weak_ptr {
public:
	struct Result {
		int forwarded = 0;
		int skipped = 0;
		QString error;
		std::vector<CarrierItem> sent;
		std::vector<CarrierItem> fallback;
	};

	CarrierForwarder(
		not_null<Main::Session*> session,
		not_null<PeerData*> destination,
		MsgId topicRootId);
	~CarrierForwarder();

	// All items must come from `carrier`; one request carries all of them.
	void send(not_null<PeerData*> carrier, std::vector<CarrierItem> items);
	void cancel();

	Fn<void(Result &&result)> done;

private:
	void finish(Result &&result);
	void handleSuccess(const MTPUpdates &result);
	void handleFailure(QString error);

	const not_null<Main::Session*> _session;
	const not_null<PeerData*> _destination;
	const MsgId _topicRootId;
	std::vector<CarrierItem> _sent;
	mtpRequestId _requestId = 0;
	int _skipped = 0;
	bool _cancelled = false;

};

} // namespace BulkForward
