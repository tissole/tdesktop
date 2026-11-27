/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_forward_dedup.h"

#include "core/application.h"
#include "data/data_dedup_db.h"
#include "data/data_document.h"
#include "data/data_download_manager.h"
#include "data/data_file_hash.h"
#include "data/data_media_types.h"
#include "data/data_photo.h"
#include "history/history.h"
#include "history/history_item.h"
#include "settings.h"

namespace Data::ForwardDedup {
namespace {

constexpr auto kMaxForwardHashRequests = 4;

// One fingerprint pass over a copy batch before the batch is sent, so the
// batch can be checked by hash and not only by document id. Every item ends
// up in `known`, with an empty hash when no sample is needed or the sample
// could not be fetched; such an item is still sent, only without that check.
struct HashResolveState {
	std::vector<not_null<HistoryItem*>> items;
	std::shared_ptr<std::map<const HistoryItem*, QByteArray>> known;
	Fn<void()> done;
	size_t next = 0;
	int inflight = 0;
};

void HashPump(const std::shared_ptr<HashResolveState> &state);

void HashStore(
		const std::shared_ptr<HashResolveState> &state,
		const HistoryItem *item,
		QByteArray hash) {
	state->known->emplace(item, std::move(hash));
	if (state->inflight > 0) {
		--state->inflight;
	}
	HashPump(state);
}

void HashPump(const std::shared_ptr<HashResolveState> &state) {
	while (state->inflight < kMaxForwardHashRequests
		&& state->next < state->items.size()) {
		const auto item = state->items[state->next++].get();
		const auto media = item->media();
		const auto doc = media ? media->document() : nullptr;
		const auto photo = media ? media->photo() : nullptr;
		if (!doc && !photo) {
			state->known->emplace(item, QByteArray());
			continue;
		}
		++state->inflight;
		const auto store = [state, item](QByteArray hash) {
			HashStore(state, item, std::move(hash));
		};
		const auto session = not_null<Main::Session*>(
			&item->history()->session());
		if (doc) {
			RemoteFileFingerprint(
				session,
				not_null<DocumentData*>(doc),
				store);
		} else {
			RemotePhotoFingerprint(
				session,
				not_null<PhotoData*>(photo),
				store);
		}
	}
	if (state->inflight == 0 && state->next >= state->items.size()) {
		const auto done = std::move(state->done);
		if (done) {
			done();
		}
	}
}

void ResolveHash(
		Main::Session *session,
		uint64 mediaId,
		DocumentData *doc,
		PeerId dstPeer,
		MsgId sentId) {
	if (!doc) {
		return;
	}
	RemoteFileFingerprint(
		not_null<Main::Session*>(session),
		not_null<DocumentData*>(doc),
		[=](QByteArray hash) {
			RecordHash(mediaId, false, hash, dstPeer, sentId);
		});
}

void ResolvePhotoHash(
		Main::Session *session,
		uint64 mediaId,
		PhotoData *photo,
		PeerId dstPeer,
		MsgId sentId) {
	if (!photo) {
		return;
	}
	RemotePhotoFingerprint(
		not_null<Main::Session*>(session),
		not_null<PhotoData*>(photo),
		[=](QByteArray hash) {
			RecordHash(mediaId, true, hash, dstPeer, sentId);
		});
}

} // namespace

Media MediaOf(HistoryItem *item) {
	const auto media = item ? item->media() : nullptr;
	const auto doc = media ? media->document() : nullptr;
	const auto photo = media ? media->photo() : nullptr;
	if (!doc && !photo) {
		return {};
	}
	return {
		.mediaId = doc ? uint64(doc->id) : uint64(photo->id),
		.doc = doc,
		.photo = photo,
	};
}

bool IsGlobalOn(const Context &context) {
	return context.globalOn && !context.destinationOnly;
}

Verdict CheckMedia(
		const Context &context,
		uint64 mediaId,
		bool isPhoto,
		bool seenInBatch) {
	if (!mediaId) {
		return Verdict::Allowed;
	}
	if (seenInBatch) {
		return Verdict::InBatch;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return Verdict::Allowed;
	}
	if (db.chatTableContainsDocId(
			context.destination,
			mediaId,
			isPhoto,
			false)) {
		return Verdict::DestinationDuplicate;
	}
	if (context.checkFwTmp
		&& db.containsFwTmpItem(
			context.sessionId,
			context.destination,
			mediaId,
			isPhoto)) {
		return Verdict::DestinationDuplicate;
	}
	if (!IsGlobalOn(context)) {
		return Verdict::Allowed;
	}
	if (db.chatTableContainsDocId(
			context.destination,
			mediaId,
			isPhoto,
			true)) {
		return Verdict::GlobalDuplicate;
	}
	if (context.run
		&& context.run->media.contains({ mediaId, isPhoto })) {
		return Verdict::Allowed;
	}
	if (db.containsDocId(DedupDb::Table::Uploads, mediaId, isPhoto)) {
		return Verdict::GlobalDuplicate;
	}
	return Verdict::Allowed;
}

Verdict CheckHash(
		const Context &context,
		const QByteArray &hash,
		bool seenInBatch) {
	if (hash.isEmpty()) {
		return Verdict::Allowed;
	}
	if (seenInBatch) {
		return Verdict::InBatch;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return Verdict::Allowed;
	}
	if (db.chatTableContainsHash(context.destination, hash, false)) {
		return Verdict::DestinationDuplicate;
	}
	if (context.checkFwTmp
		&& db.containsFwTmpHash(context.sessionId, context.destination, hash)) {
		return Verdict::DestinationDuplicate;
	}
	if (!IsGlobalOn(context)) {
		return Verdict::Allowed;
	}
	if (db.chatTableContainsHash(context.destination, hash, true)) {
		return Verdict::GlobalDuplicate;
	}
	if (context.run && context.run->hashes.contains(hash)) {
		return Verdict::Allowed;
	}
	if (db.seekDocumentId(DedupDb::Table::Uploads, hash, 0) != 0) {
		return Verdict::GlobalDuplicate;
	}
	return Verdict::Allowed;
}

void AddPending(uint64 mediaId, bool isPhoto, const QByteArray &hash) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		db.addPending(DedupDb::Table::Uploads, mediaId, hash, isPhoto);
	}
}

void RemovePending(uint64 mediaId, bool isPhoto) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (db.isOpen()) {
		db.removePending(DedupDb::Table::Uploads, mediaId, isPhoto);
	}
}

void ResolveHashes(
		PeerId dstPeer,
		const std::set<std::pair<uint64, bool>> &seen,
		const std::vector<not_null<HistoryItem*>> &items,
		std::shared_ptr<std::map<const HistoryItem*, QByteArray>> known,
		Fn<void()> done) {
	auto &db = Core::App().downloadManager().ensureDedupDb();
	const auto context = Context{
		.destination = dstPeer,
		.globalOn = GetEnhancedBool("prevent_forward_duplicates"),
	};
	auto missing = std::vector<not_null<HistoryItem*>>();
	for (const auto &item : items) {
		if (known->find(item.get()) != known->end()) {
			continue;
		}
		const auto media = MediaOf(item.get());
		if (!media.mediaId
			|| seen.contains({ media.mediaId, media.photo != nullptr })
			|| (db.isOpen()
				&& CheckMedia(
					context,
					media.mediaId,
					media.photo != nullptr,
					false) != Verdict::Allowed)) {
			known->emplace(item.get(), QByteArray());
			continue;
		}
		missing.push_back(item);
	}
	if (missing.empty()) {
		done();
		return;
	}
	const auto state = std::make_shared<HashResolveState>();
	state->items = std::move(missing);
	state->known = std::move(known);
	state->done = std::move(done);
	HashPump(state);
}

bool Skip(
		HistoryItem *item,
		PeerId dstPeer,
		std::set<std::pair<uint64, bool>> &seenInBatch,
		std::set<QByteArray> &seenHashes,
		const QByteArray &hash,
		bool &pendingAdded) {
	pendingAdded = false;
	const auto media = MediaOf(item);
	if (!media.mediaId) {
		return false;
	}
	const auto mediaId = media.mediaId;
	const auto isPhoto = (media.photo != nullptr);
	if (!seenInBatch.insert({ mediaId, isPhoto }).second) {
		return true;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return false;
	}
	const auto context = Context{
		.destination = dstPeer,
		.globalOn = GetEnhancedBool("prevent_forward_duplicates"),
	};
	if (CheckMedia(context, mediaId, isPhoto, false) != Verdict::Allowed) {
		return true;
	}
	if (!hash.isEmpty()) {
		if (!seenHashes.insert(hash).second) {
			return true;
		}
		if (CheckHash(context, hash, false) != Verdict::Allowed) {
			return true;
		}
	}
	if (IsGlobalOn(context)) {
		AddPending(mediaId, isPhoto, hash);
		pendingAdded = true;
	}
	return false;
}

void RecordHash(
		uint64 mediaId,
		bool isPhoto,
		const QByteArray &hash,
		PeerId dstPeer,
		MsgId sentId) {
	if (hash.isEmpty()) {
		return;
	}
	auto &db = Core::App().downloadManager().ensureDedupDb();
	if (!db.isOpen()) {
		return;
	}
	auto isDup = false;
	if (sentId && IsServerMsgId(sentId)) {
		isDup = db.recordUploadDestination(
			hash,
			mediaId,
			isPhoto,
			dstPeer,
			sentId,
			db.isChatScanned(dstPeer));
	} else {
		db.insert(DedupDb::Table::Uploads, {
			.hash = hash,
			.documentId = mediaId,
			.isPhoto = isPhoto,
			.status = u"f"_q,
		});
	}
	const auto dedupOn = GetEnhancedBool("prevent_forward_duplicates");
	if (!dedupOn || isDup) {
		return;
	}
	if (db.chatTableContainsHash(dstPeer, hash, dedupOn)) {
		return;
	}
	using Table = DedupDb::Table;
	if (db.containsDocId(Table::Uploads, mediaId, isPhoto)) {
		return;
	}
	if (db.containsHash(Table::Uploads, hash)) {
		if (db.containsFinishedHash(Table::Uploads, hash)) {
			db.insert(Table::Uploads, {
				.hash = hash,
				.documentId = mediaId,
				.isPhoto = isPhoto,
				.status = u"f"_q,
			});
		}
		return;
	}
	db.insert(Table::Uploads, {
		.hash = hash,
		.documentId = mediaId,
		.isPhoto = isPhoto,
		.status = u"f"_q,
	});
}

void Done(
		Main::Session *session,
		uint64 mediaId,
		DocumentData *doc,
		PhotoData *photo,
		const QByteArray &hash,
		PeerId dstPeer,
		MsgId sentId,
		bool pending) {
	if (pending) {
		RemovePending(mediaId, (photo != nullptr));
	}
	if (!hash.isEmpty()) {
		RecordHash(
			mediaId,
			(photo != nullptr),
			hash,
			dstPeer,
			sentId);
		return;
	}
	ResolveHash(session, mediaId, doc, dstPeer, sentId);
	ResolvePhotoHash(session, mediaId, photo, dstPeer, sentId);
}

void Fail(uint64 mediaId, bool isPhoto, bool pending) {
	if (pending) {
		RemovePending(mediaId, isPhoto);
	}
}

} // namespace Data::ForwardDedup
