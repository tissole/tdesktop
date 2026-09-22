/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/export_dedup.h"

#include <QString>
#include <algorithm>

#include "base/concurrent_timer.h"
#include "data/data_dedup_db.h"
#include "mtproto/mtproto_response.h"
#include "data/data_file_hash.h"
#include "export/data/export_data_types.h"
#include "mtproto/mtproto_concurrent_sender.h"

namespace Export {
namespace {

using Table = ::Data::DedupDb::Table;

void MarkDone(
		::Data::DedupDb &db,
		uint64 sessionId,
		PeerId peerId,
		uint64 docId,
		const QByteArray &hash,
		bool global) {
	if (global) {
		db.insert(Table::Downloads, {
			.hash = hash,
			.documentId = docId,
			.status = u"f"_q,
		});
	} else {
		db.insertExTmp(sessionId, peerId, docId, hash);
	}
}

} // namespace

void FetchHash(
		MTP::ConcurrentSender &mtp,
		Fn<void(FnMut<void()>)> runner,
		uint64 takeoutId,
		const Data::FileLocation &location,
		int64 size,
		Fn<bool()> alive,
		Fn<void(QByteArray)> done,
		int maxAttempts,
		const QString &tag,
		Fn<void(FnMut<void(uint64 freshId)>)> refreshTakeout) {
	int64 headOffset = 0;
	int64 tailOffset = 0;
	::Data::DedupSampleOffsets(size, headOffset, tailOffset);
	const auto currentTakeout = std::make_shared<uint64>(takeoutId);
	const auto head = std::make_shared<QByteArray>();
	const auto tail = std::make_shared<QByteArray>();
	const auto pending = std::make_shared<int>(2);
	const auto failed = std::make_shared<bool>(false);
	const auto doneHash = [head, tail, pending, failed, done] {
		if (--*pending != 0) {
			return;
		}
		done(*failed ? QByteArray() : ::Data::HashChunks(*head, *tail));
	};
	const auto fetch = [&](int64 offset, std::shared_ptr<QByteArray> target) {
		const auto gen = std::make_shared<int>(0);
		const auto settled = std::make_shared<bool>(false);
		const auto armedFor = std::make_shared<int>(0);
		const auto refreshed = std::make_shared<bool>(false);
		const auto timer = std::make_shared<base::ConcurrentTimer>(runner);
		const auto waitFor = [](int attempt) {
			return 2000 + std::min(attempt * 500, 4000) + (attempt * 137) % 300;
		};
		const auto sendChunk = std::make_shared<Fn<void()>>();
		const auto giveUp = [=] {
			if (*settled) {
				return;
			}
			*settled = true;
			timer->cancel();
			LOG(("ExportDiag: giveup tag=%1 attempts=%2").arg(tag).arg(*gen));
			*failed = true;
			doneHash();
		};
		timer->setCallback([=] {
			if (*settled || *gen != *armedFor) {
				return;
			}
			if (!alive()) {
				*settled = true;
				timer->cancel();
				return;
			}
			if (*gen >= maxAttempts) {
				giveUp();
				return;
			}
			(*sendChunk)();
		});
		const auto armTimeout = [=](int my) {
			*armedFor = my;
			timer->callOnce(crl::time(waitFor(my)));
		};
		*sendChunk = [=, &mtp] {
			if (*settled) {
				return;
			} else if (!alive()) {
				*settled = true;
				timer->cancel();
				return;
			}
			const auto my = ++*gen;
			mtp.request(MTPInvokeWithTakeout<MTPupload_GetFile>(
				MTP_long(*currentTakeout),
				MTPupload_GetFile(
					MTP_flags(MTPupload_GetFile::Flag::f_precise),
					location.data,
					MTP_long(offset),
					MTP_int(int(::Data::kDedupChunk))))
			).done([=](const MTPupload_File &result) {
				if (*settled || my != *gen) {
					return;
				}
				*settled = true;
				timer->cancel();
				if (*gen > 1) {
					LOG(("ExportDiag: rehealed tag=%1 attempt=%2")
						.arg(tag)
						.arg(*gen));
				}
				if (result.type() != mtpc_upload_file) {
					*failed = true;
				} else {
					*target = result.c_upload_file().vbytes().v;
				}
				doneHash();
			}).fail([=](const MTP::Error &error) {
				if (*settled || my != *gen) {
					return;
				} else if (!alive()) {
					*settled = true;
					timer->cancel();
					return;
				}
				if (error.type() == u"TAKEOUT_INVALID"_q
					&& refreshTakeout
					&& !*refreshed) {
					*refreshed = true;
					refreshTakeout([=](uint64 fresh) mutable {
						if (*settled || *gen != my) {
							return;
						}
						if (fresh) {
							*currentTakeout = fresh;
							*gen = 0;
						}
						(*sendChunk)();
					});
					return;
				}
				if (my == 1 && !MTP::IsFloodError(error)) {
					(*sendChunk)();
				} else if (my >= maxAttempts) {
					giveUp();
				} else {
					if (MTP::IsFloodError(error)) {
						LOG(("ExportDiag: flood tag=%1 wait=%2s attempt=%3/%4")
							.arg(tag)
							.arg(error.type().split(u"_"_q).last().toInt())
							.arg(my)
							.arg(maxAttempts));
					}
					armTimeout(my);
				}
			}).toDC(MTP::ShiftDcId(location.dcId, MTP::kExportMediaDcShift)).handleFloodErrors().send();
			armTimeout(my);
		};
		(*sendChunk)();
	};
	fetch(headOffset, head);
	fetch(tailOffset, tail);
}

void FetchFullFile(
	MTP::ConcurrentSender &mtp,
	Fn<void(FnMut<void()>)> runner,
	uint64 takeoutId,
	const Data::FileLocation &location,
	int64 size,
	Fn<bool()> alive,
	Fn<void(QByteArray)> done,
	int maxAttempts,
	const QString &tag,
	Fn<void(FnMut<void(uint64 freshId)>)> refreshTakeout) {
	constexpr auto kChunk = 1024 * 1024;
	const auto currentTakeout = std::make_shared<uint64>(takeoutId);
	const auto content = std::make_shared<QByteArray>();
	const auto fetchNext = std::make_shared<Fn<void()>>();
	*fetchNext = [=, &mtp] {
		if (size > 0 && content->size() >= size) {
			done(*content);
			return;
		}
		const auto refreshed = std::make_shared<bool>(false);
		const auto gen = std::make_shared<int>(0);
		const auto settled = std::make_shared<bool>(false);
		const auto waitFor = [](int attempt) {
			return 2000 + std::min(attempt * 500, 4000) + (attempt * 137) % 300;
		};
		const auto sendChunk = std::make_shared<Fn<void()>>();
		const auto armedFor = std::make_shared<int>(0);
		const auto timer = std::make_shared<base::ConcurrentTimer>(runner);
		const auto giveUp = [=] {
			if (*settled) {
				return;
			}
			*settled = true;
			timer->cancel();
			LOG(("ExportDiag: giveup tag=%1 attempts=%2").arg(tag).arg(*gen));
			done(QByteArray());
		};
		timer->setCallback([=] {
			if (*settled || *gen != *armedFor) {
				return;
			}
			if (!alive()) {
				*settled = true;
				timer->cancel();
				return;
			}
			if (*gen >= maxAttempts) {
				giveUp();
				return;
			}
			(*sendChunk)();
		});
		const auto armTimeout = [=](int my) {
			*armedFor = my;
			timer->callOnce(crl::time(waitFor(my)));
		};
		*sendChunk = [=, &mtp] {
			if (*settled) {
				return;
			} else if (!alive()) {
				*settled = true;
				timer->cancel();
				return;
			}
			const auto my = ++*gen;
			mtp.request(MTPInvokeWithTakeout<MTPupload_GetFile>(
				MTP_long(*currentTakeout),
				MTPupload_GetFile(
					MTP_flags(MTPupload_GetFile::Flag::f_precise),
					location.data,
					MTP_long(int64(content->size())),
					MTP_int(kChunk)))
			).done([=](const MTPupload_File &result) {
				if (*settled || my != *gen) {
					return;
				}
				*settled = true;
				timer->cancel();
				if (*gen > 1) {
					LOG(("ExportDiag: rehealed tag=%1 attempt=%2")
						.arg(tag)
						.arg(*gen));
				}
				if (result.type() != mtpc_upload_file) {
					done(QByteArray());
					return;
				}
				const auto bytes = result.c_upload_file().vbytes().v;
				content->append(bytes);
				if (bytes.size() < kChunk) {
					done(*content);
					return;
				}
				(*fetchNext)();
			}).fail([=](const MTP::Error &error) {
				if (*settled || my != *gen) {
					return;
				} else if (!alive()) {
					*settled = true;
					timer->cancel();
					return;
				}
				if (error.type() == u"TAKEOUT_INVALID"_q
					&& refreshTakeout
					&& !*refreshed) {
					*refreshed = true;
					refreshTakeout([=](uint64 fresh) mutable {
						if (*settled || *gen != my) {
							return;
						}
						if (fresh) {
							*currentTakeout = fresh;
							*gen = 0;
						}
						(*sendChunk)();
					});
					return;
				}
				if (my == 1 && !MTP::IsFloodError(error)) {
					(*sendChunk)();
				} else if (my >= maxAttempts) {
					giveUp();
				} else {
					if (MTP::IsFloodError(error)) {
						LOG(("ExportDiag: flood tag=%1 wait=%2s attempt=%3/%4")
							.arg(tag)
							.arg(error.type().split(u"_"_q).last().toInt())
							.arg(my)
							.arg(maxAttempts));
					}
					armTimeout(my);
				}
			}).toDC(MTP::ShiftDcId(location.dcId, MTP::kExportMediaDcShift)).handleFloodErrors().send();
			armTimeout(my);
		};
		(*sendChunk)();
	};
	(*fetchNext)();
}

void CheckDuplicate(
		MTP::ConcurrentSender &mtp,
		Fn<void(FnMut<void()>)> runner,
		uint64 takeoutId,
		::Data::DedupDb &db,
		uint64 sessionId,
		PeerId peerId,
		uint64 docId,
		const Data::FileLocation &location,
		int64 size,
		Fn<bool()> alive,
		bool global,
		bool isPhoto,
		Fn<void(CheckResult)> done) {
	if (!db.isOpen() || !docId) {
		done(CheckResult());
		return;
	}
	if (global
		? db.containsDocId(Table::Downloads, docId)
		: db.containsExTmpDocId(sessionId, peerId, docId)) {
		done(CheckResult{ .skip = true });
		return;
	}
	if (isPhoto || size < ::Data::kDedupMinPartialHashSize || !location) {
		done(CheckResult());
		return;
	}
	FetchHash(mtp, runner, takeoutId, location, size, alive, [=, dbPtr = &db](QByteArray hash) {
		if (hash.isEmpty()) {
			done(CheckResult());
			return;
		}
		done(CheckHash(*dbPtr, sessionId, peerId, docId, hash, global));
	});
}

CheckResult CheckHash(
		::Data::DedupDb &db,
		uint64 sessionId,
		PeerId peerId,
		uint64 docId,
		const QByteArray &hash,
		bool global) {
	const auto known = global
		? db.containsHash(Table::Downloads, hash)
		: db.containsExTmpHash(sessionId, peerId, hash);
	if (!known) {
		if (global) {
			db.insert(Table::Downloads, {
				.hash = hash,
				.documentId = docId,
				.status = u"u"_q,
			});
		}
		return CheckResult{ .hash = hash, .decided = true };
	}
	// Alias rows must reference finished content only: one based on an
	// in-flight row would survive its cancel and block this content.
	if (global
		? db.containsFinishedHash(Table::Downloads, hash)
		: true) {
		MarkDone(db, sessionId, peerId, docId, hash, global);
	}
	return CheckResult{ .skip = true, .hash = hash, .decided = true };
}

bool FinishFile(
		::Data::DedupDb &db,
		uint64 sessionId,
		PeerId peerId,
		uint64 docId,
		const QByteArray &content,
		const QString &path,
		int64 size,
		const QByteArray &preHash,
		bool global,
		bool isPhoto) {
	if (!db.isOpen() || !docId) {
		return false;
	}
	if (!preHash.isEmpty()) {
		// Big file: the decision was made before download.
		if (global) {
			db.updateDedupStatus(Table::Downloads, preHash, u"f"_q);
		} else {
			db.insertExTmp(sessionId, peerId, docId, preHash);
		}
		return false;
	}
	const auto hash = !content.isEmpty()
		? ::Data::ContentFingerprint(content)
		: ::Data::FileFingerprint(path, size);
	if (hash.isEmpty()) {
		return false;
	}
	if (global) {
		// A match against our own in-progress row is this file
		// recognizing itself, not a duplicate: exclude it like downloads.
		const auto other = db.seekDocumentId(Table::Downloads, hash, docId);
		const auto photoDup = isPhoto
			&& db.containsDocId(Table::Downloads, docId);
		if (other == 0 && !photoDup) {
			MarkDone(db, sessionId, peerId, docId, hash, global);
			return false;
		}
	} else if (!db.containsExTmpHash(sessionId, peerId, hash)
		&& !(isPhoto && db.containsExTmpDocId(sessionId, peerId, docId))) {
		MarkDone(db, sessionId, peerId, docId, hash, global);
		return false;
	}
	MarkDone(db, sessionId, peerId, docId, hash, global);
	return true;
}

void CancelFile(
		::Data::DedupDb &db,
		uint64 docId) {
	if (!db.isOpen() || !docId) {
		return;
	}
	db.removeByDocumentId(Table::Downloads, docId, u"u"_q);
}

} // namespace Export
