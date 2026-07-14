/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "apiwrap.h"

#include <algorithm>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "api/api_authorizations.h"
#include "api/api_attached_stickers.h"
#include "api/api_blocked_peers.h"
#include "api/api_chat_links.h"
#include "api/api_chat_participants.h"
#include "api/api_cloud_password.h"
#include "api/api_hash.h"
#include "api/api_invite_links.h"
#include "api/api_media.h"
#include "api/api_peer_colors.h"
#include "api/api_peer_photo.h"
#include "api/api_polls.h"
#include "api/api_sending.h"
#include "api/api_text_entities.h"
#include "api/api_todo_lists.h"
#include "api/api_self_destruct.h"
#include "api/api_sensitive_content.h"
#include "api/api_global_privacy.h"
#include "api/api_reactions_notify_settings.h"
#include "api/api_updates.h"
#include "api/api_user_privacy.h"
#include "api/api_read_metrics.h"
#include "api/api_views.h"
#include "api/api_confirm_phone.h"
#include "api/api_unread_things.h"
#include "api/api_ringtones.h"
#include "api/api_compose_with_ai.h"
#include "api/api_transcribes.h"
#include "api/api_premium.h"
#include "api/api_user_names.h"
#include "api/api_websites.h"
#include "data/business/data_shortcut_messages.h"
#include "data/components/scheduled_messages.h"
#include "data/notify/data_notify_settings.h"
#include "data/data_changes.h"
#include "data/data_web_page.h"
#include "data/data_folder.h"
#include "data/data_forum_topic.h"
#include "data/data_forum.h"
#include "data/data_saved_messages.h"
#include "data/data_saved_music.h"
#include "data/data_saved_sublist.h"
#include "data/data_search_controller.h"
#include "data/data_session.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "enhanced_forward.h"
#include "core/file_utilities.h"
#include "data/data_document_media.h"
#include "data/data_photo_media.h"
#include "data/data_user.h"
#include "data/data_chat_filters.h"
#include "data/data_histories.h"
#include "data/data_history_messages.h"
#include "core/core_cloud_password.h"
#include "core/application.h"
#include "base/unixtime.h"
#include "base/random.h"
#include <QDir>
#include <QMimeDatabase>
#include <map>
#include <functional>
#include "logs.h"
#include "base/call_delayed.h"
#include "lang/lang_keys.h"
#include "mainwidget.h"
#include "boxes/add_contact_box.h"
#include "mtproto/mtproto_config.h"
#include "history/history.h"
#include "history/history_item_components.h"
#include "history/history_item_helpers.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "main/main_account.h"
#include "ui/boxes/confirm_box.h"
#include "boxes/sticker_set_box.h"
#include "boxes/premium_limits_box.h"
#include "window/notifications_manager.h"
#include "window/window_controller.h"
#include "window/window_lock_widgets.h"
#include "window/window_session_controller.h"
#include "inline_bots/inline_bot_result.h"
#include "chat_helpers/message_field.h"
#include "ui/item_text_options.h"
#include "ui/text/text_utilities.h"
#include "ui/chat/attach/attach_prepare.h"
#include "ui/toast/toast.h"
#include "support/support_helper.h"
#include "settings/sections/settings_premium.h"
#include "ui/image/image_prepare.h"
#include "storage/localimageloader.h"
#include "storage/download_manager_mtproto.h"
#include "storage/file_upload.h"
#include "storage/storage_account.h"

namespace {

// Save draft to the cloud with 1 sec extra delay.
constexpr auto kSaveCloudDraftTimeout = 1000;

constexpr auto kSmallDelayMs = 5;
constexpr auto kReadFeaturedSetsTimeout = crl::time(1000);
constexpr auto kFileLoaderQueueStopTimeout = crl::time(5000);

constexpr auto kStickersByEmojiInvalidateTimeout = crl::time(6 * 1000);
constexpr auto kNotifySettingSaveTimeout = crl::time(1000);
constexpr auto kDialogsFirstLoad = 20;
constexpr auto kDialogsPerPage = 500;
constexpr auto kStatsSessionKillTimeout = 10 * crl::time(1000);

using PhotoFileLocationId = Data::PhotoFileLocationId;
using DocumentFileLocationId = Data::DocumentFileLocationId;
using UpdatedFileReferences = Data::UpdatedFileReferences;

[[nodiscard]] std::shared_ptr<ChatHelpers::Show> ShowForPeer(
		not_null<PeerData*> peer) {
	if (const auto window = Core::App().windowFor(peer)) {
		if (const auto controller = window->sessionController()) {
			if (&controller->session() == &peer->session()) {
				return controller->uiShow();
			}
		}
	}
	return nullptr;
}

void ShowChannelsLimitBox(not_null<PeerData*> peer) {
	if (const auto window = Core::App().windowFor(peer)) {
		window->invokeForSessionController(
			&peer->session().account(),
			peer,
			[&](not_null<Window::SessionController*> controller) {
				controller->show(Box(ChannelsLimitBox, &peer->session()));
			});
	}
}

[[nodiscard]] FileLoadTo FileLoadTaskOptions(const Api::SendAction &action) {
	const auto peer = action.history->peer;
	return FileLoadTo(
		peer->id,
		action.options,
		action.replyTo,
		action.replaceMediaOf);
}

[[nodiscard]] QString FormatVideoTimestamp(TimeId seconds) {
	const auto minutes = seconds / 60;
	const auto hours = minutes / 60;
	return hours
		? u"%1h%2m%3s"_q.arg(hours).arg(minutes % 60).arg(seconds % 60)
		: minutes
		? u"%1m%2s"_q.arg(minutes).arg(seconds % 60)
		: QString::number(seconds);
}

} // namespace

namespace Api {

TimeId UnixtimeFromMsgId(mtpMsgId msgId) {
	return TimeId(msgId >> 32);
}

} // namespace Api

ApiWrap::ApiWrap(not_null<Main::Session*> session)
: MTP::Sender(&session->account().mtp())
, _session(session)
, _messageDataResolveDelayed([=] { resolveMessageDatas(); })
, _webPagesTimer([=] { resolveWebPages(); })
, _draftsSaveTimer([=] { saveDraftsToCloud(); })
, _featuredSetsReadTimer([=] { readFeaturedSets(); })
, _dialogsLoadState(std::make_unique<DialogsLoadState>())
, _fileLoader(std::make_unique<TaskQueue>(kFileLoaderQueueStopTimeout))
, _updateNotifyTimer([=] { sendNotifySettingsUpdates(); })
, _statsSessionKillTimer([=] { checkStatsSessions(); })
, _authorizations(std::make_unique<Api::Authorizations>(this))
, _attachedStickers(std::make_unique<Api::AttachedStickers>(this))
, _blockedPeers(std::make_unique<Api::BlockedPeers>(this))
, _cloudPassword(std::make_unique<Api::CloudPassword>(this))
, _selfDestruct(std::make_unique<Api::SelfDestruct>(this))
, _sensitiveContent(std::make_unique<Api::SensitiveContent>(this))
, _globalPrivacy(std::make_unique<Api::GlobalPrivacy>(this))
, _reactionsNotifySettings(
	std::make_unique<Api::ReactionsNotifySettings>(this))
, _userPrivacy(std::make_unique<Api::UserPrivacy>(this))
, _inviteLinks(std::make_unique<Api::InviteLinks>(this))
, _chatLinks(std::make_unique<Api::ChatLinks>(this))
, _views(std::make_unique<Api::ViewsManager>(this))
, _readMetrics(std::make_unique<Api::ReadMetrics>(this))
, _confirmPhone(std::make_unique<Api::ConfirmPhone>(this))
, _peerPhoto(std::make_unique<Api::PeerPhoto>(this))
, _polls(std::make_unique<Api::Polls>(this))
, _todoLists(std::make_unique<Api::TodoLists>(this))
, _chatParticipants(std::make_unique<Api::ChatParticipants>(this))
, _unreadThings(std::make_unique<Api::UnreadThings>(this))
, _ringtones(std::make_unique<Api::Ringtones>(this))
, _composeWithAi(std::make_unique<Api::ComposeWithAi>(this))
, _transcribes(std::make_unique<Api::Transcribes>(this))
, _premium(std::make_unique<Api::Premium>(this))
, _usernames(std::make_unique<Api::Usernames>(this))
, _websites(std::make_unique<Api::Websites>(this))
, _peerColors(std::make_unique<Api::PeerColors>(this)) {
	crl::on_main(session, [=] {
		// You can't use _session->lifetime() in the constructor,
		// only queued, because it is not constructed yet.
		_session->data().chatsFilters().changed(
		) | rpl::filter([=] {
			return _session->data().chatsFilters().archiveNeeded();
		}) | rpl::on_next([=] {
			requestMoreDialogsIfNeeded();
		}, _session->lifetime());

		_reactionsNotifySettings->reload();
		setupSupportMode();
	});
}

ApiWrap::~ApiWrap() = default;

void ApiWrap::ProcessRecentSelfForwards(
		not_null<Main::Session*> session,
		const MTPUpdates &updates,
		PeerId targetPeerId,
		PeerId fromPeerId) {
	auto newIds = MessageIdsList();
	updates.match([&](const MTPDupdates &data) {
		for (const auto &update : data.vupdates().v) {
			update.match([&](const MTPDupdateMessageID &d) {
				newIds.push_back(FullMsgId(targetPeerId, d.vid().v));
			}, [](const auto &) {});
		}
	}, [](const auto &) {});
	if (!newIds.empty()) {
		session->data().addRecentSelfForwards({
			.fromPeerId = fromPeerId,
			.ids = newIds,
		});
	}
}

Main::Session &ApiWrap::session() const {
	return *_session;
}

Storage::Account &ApiWrap::local() const {
	return _session->local();
}

Api::Updates &ApiWrap::updates() const {
	return _session->updates();
}

void ApiWrap::setupSupportMode() {
	if (!_session->supportMode()) {
		return;
	}

	_session->settings().supportChatsTimeSliceValue(
	) | rpl::on_next([=](int seconds) {
		_dialogsLoadTill = seconds ? std::max(base::unixtime::now() - seconds, 0) : 0;
		refreshDialogsLoadBlocked();
	}, _session->lifetime());
}

void ApiWrap::requestChangelog(
		const QString &sinceVersion,
		Fn<void(const MTPUpdates &result)> callback) {
	//request(MTPhelp_GetAppChangelog(
	//	MTP_string(sinceVersion)
	//)).done(
	//	callback
	//).send();
}

void ApiWrap::requestDeepLinkInfo(
		const QString &path,
		Fn<void(TextWithEntities message, bool updateRequired)> callback) {
	request(_deepLinkInfoRequestId).cancel();
	_deepLinkInfoRequestId = request(MTPhelp_GetDeepLinkInfo(
		MTP_string(path)
	)).done([=](const MTPhelp_DeepLinkInfo &result) {
		_deepLinkInfoRequestId = 0;
		if (result.type() == mtpc_help_deepLinkInfo) {
			const auto &data = result.c_help_deepLinkInfo();
			callback(TextWithEntities{
				qs(data.vmessage()),
				Api::EntitiesFromMTP(
					_session,
					data.ventities().value_or_empty())
			}, data.is_update_app());
		}
	}).fail([=] {
		_deepLinkInfoRequestId = 0;
	}).send();
}

void ApiWrap::requestTermsUpdate() {
	if (_termsUpdateRequestId) {
		return;
	}
	const auto now = crl::now();
	if (_termsUpdateSendAt && now < _termsUpdateSendAt) {
		base::call_delayed(_termsUpdateSendAt - now, _session, [=] {
			requestTermsUpdate();
		});
		return;
	}

	constexpr auto kTermsUpdateTimeoutMin = 10 * crl::time(1000);
	constexpr auto kTermsUpdateTimeoutMax = 86400 * crl::time(1000);

	_termsUpdateRequestId = request(MTPhelp_GetTermsOfServiceUpdate(
	)).done([=](const MTPhelp_TermsOfServiceUpdate &result) {
		_termsUpdateRequestId = 0;

		const auto requestNext = [&](auto &&data) {
			const auto timeout = (data.vexpires().v - base::unixtime::now());
			_termsUpdateSendAt = crl::now() + std::clamp(
				timeout * crl::time(1000),
				kTermsUpdateTimeoutMin,
				kTermsUpdateTimeoutMax);
			requestTermsUpdate();
		};
		switch (result.type()) {
		case mtpc_help_termsOfServiceUpdateEmpty: {
			const auto &data = result.c_help_termsOfServiceUpdateEmpty();
			requestNext(data);
		} break;
		case mtpc_help_termsOfServiceUpdate: {
			const auto &data = result.c_help_termsOfServiceUpdate();
			const auto &terms = data.vterms_of_service();
			const auto &fields = terms.c_help_termsOfService();
			session().lockByTerms(
				Window::TermsLock::FromMTP(_session, fields));
			requestNext(data);
		} break;
		default: Unexpected("Type in requestTermsUpdate().");
		}
	}).fail([=] {
		_termsUpdateRequestId = 0;
		_termsUpdateSendAt = crl::now() + kTermsUpdateTimeoutMin;
		requestTermsUpdate();
	}).send();
}

void ApiWrap::acceptTerms(bytes::const_span id) {
	request(MTPhelp_AcceptTermsOfService(
		MTP_dataJSON(MTP_bytes(id))
	)).done([=] {
		requestTermsUpdate();
	}).send();
}

void ApiWrap::checkChatInvite(
		const QString &hash,
		FnMut<void(const MTPChatInvite &)> done,
		Fn<void(const MTP::Error &)> fail) {
	request(base::take(_checkInviteRequestId)).cancel();
	_checkInviteRequestId = request(MTPmessages_CheckChatInvite(
		MTP_string(hash)
	)).done(std::move(done)).fail(std::move(fail)).handleFloodErrors().send();
}

void ApiWrap::checkFilterInvite(
		const QString &slug,
		FnMut<void(const MTPchatlists_ChatlistInvite &)> done,
		Fn<void(const MTP::Error &)> fail) {
	request(base::take(_checkFilterInviteRequestId)).cancel();
	_checkFilterInviteRequestId = request(
		MTPchatlists_CheckChatlistInvite(MTP_string(slug))
	).done(std::move(done)).fail(std::move(fail)).send();
}

void ApiWrap::savePinnedOrder(Data::Folder *folder) {
	const auto &order = _session->data().pinnedChatsOrder(folder);
	const auto input = [](Dialogs::Key key) {
		if (const auto history = key.history()) {
			return MTP_inputDialogPeer(history->peer->input());
		} else if (const auto folder = key.folder()) {
			return MTP_inputDialogPeerFolder(MTP_int(folder->id()));
		}
		Unexpected("Key type in pinnedDialogsOrder().");
	};
	auto peers = QVector<MTPInputDialogPeer>();
	peers.reserve(order.size());
	ranges::transform(
		order,
		ranges::back_inserter(peers),
		input);
	request(MTPmessages_ReorderPinnedDialogs(
		MTP_flags(MTPmessages_ReorderPinnedDialogs::Flag::f_force),
		MTP_int(folder ? folder->id() : 0),
		MTP_vector(peers)
	)).send();
}

void ApiWrap::savePinnedOrder(not_null<Data::Forum*> forum) {
	const auto &order = _session->data().pinnedChatsOrder(forum);
	const auto input = [](Dialogs::Key key) {
		if (const auto topic = key.topic()) {
			return MTP_int(topic->rootId().bare);
		}
		Unexpected("Key type in pinnedDialogsOrder().");
	};
	auto topics = QVector<MTPint>();
	topics.reserve(order.size());
	ranges::transform(
		order,
		ranges::back_inserter(topics),
		input);
	request(MTPmessages_ReorderPinnedForumTopics(
		MTP_flags(MTPmessages_ReorderPinnedForumTopics::Flag::f_force),
		forum->peer()->input(),
		MTP_vector(topics)
	)).done([=](const MTPUpdates &result) {
		applyUpdates(result);
	}).send();
}

void ApiWrap::savePinnedOrder(not_null<Data::SavedMessages*> saved) {
	if (saved->parentChat()) {
		return;
	}
	const auto &order = _session->data().pinnedChatsOrder(saved);
	const auto input = [](Dialogs::Key key) {
		if (const auto sublist = key.sublist()) {
			return MTP_inputDialogPeer(sublist->sublistPeer()->input());
		}
		Unexpected("Key type in pinnedDialogsOrder().");
	};
	auto peers = QVector<MTPInputDialogPeer>();
	peers.reserve(order.size());
	ranges::transform(
		order,
		ranges::back_inserter(peers),
		input);
	request(MTPmessages_ReorderPinnedSavedDialogs(
		MTP_flags(MTPmessages_ReorderPinnedSavedDialogs::Flag::f_force),
		MTP_vector(peers)
	)).send();
}

void ApiWrap::toggleHistoryArchived(
		not_null<History*> history,
		bool archived,
		Fn<void()> callback) {
	if (const auto already = _historyArchivedRequests.take(history)) {
		request(already->first).cancel();
	}
	const auto isPinned = history->isPinnedDialog(0);
	const auto archiveId = Data::Folder::kId;
	const auto requestId = request(MTPfolders_EditPeerFolders(
		MTP_vector<MTPInputFolderPeer>(
			1,
			MTP_inputFolderPeer(
				history->peer->input(),
				MTP_int(archived ? archiveId : 0)))
	)).done([=](const MTPUpdates &result) {
		applyUpdates(result);
		if (archived) {
			history->setFolder(_session->data().folder(archiveId));
		} else {
			if (GetEnhancedBool("hide_all_chats")) {
				if (const auto window = Core::App().activeWindow()) {
					if (const auto controller = window->sessionController()) {
						const auto filters = &_session->data().chatsFilters();
						const auto lookup_id = filters->lookupId(controller->session().premium() ? 0 : 1);
						controller->setActiveChatsFilter(lookup_id);
					}
				}
			} else {
				history->clearFolder();
			}
		}
		if (const auto data = _historyArchivedRequests.take(history)) {
			data->second();
		}
		if (isPinned) {
			_session->data().notifyPinnedDialogsOrderUpdated();
		}
	}).fail([=] {
		_historyArchivedRequests.remove(history);
	}).send();
	_historyArchivedRequests.emplace(history, requestId, callback);
}

void ApiWrap::sendMessageFail(
		const MTP::Error &error,
		not_null<PeerData*> peer,
		uint64 randomId,
		FullMsgId itemId) {
	sendMessageFail(error.type(), peer, randomId, itemId);
}

void ApiWrap::sendMessageFail(
		const QString &error,
		not_null<PeerData*> peer,
		uint64 randomId,
		FullMsgId itemId) {
	const auto show = ShowForPeer(peer);
	const auto paidStarsPrefix = u"ALLOW_PAYMENT_REQUIRED_"_q;
	if (show && error == u"PEER_FLOOD"_q) {
		show->showBox(
			Ui::MakeInformBox(
				PeerFloodErrorText(&session(), PeerFloodType::Send)),
			Ui::LayerOption::CloseOther);
	} else if (show && error == u"USER_BANNED_IN_CHANNEL"_q) {
		const auto link = tr::link(
			tr::lng_cant_more_info(tr::now),
			session().createInternalLinkFull(u"spambot"_q));
		show->showBox(
			Ui::MakeInformBox(
				tr::lng_error_public_groups_denied(
					tr::now,
					lt_more_info,
					link,
					tr::marked)),
			Ui::LayerOption::CloseOther);
	} else if (error.startsWith(u"SLOWMODE_WAIT_"_q)) {
		const auto chop = u"SLOWMODE_WAIT_"_q.size();
		const auto left = base::StringViewMid(error, chop).toInt();
		if (const auto channel = peer->asChannel()) {
			const auto seconds = channel->slowmodeSeconds();
			if (seconds >= left) {
				channel->growSlowmodeLastMessage(
					base::unixtime::now() - (left - seconds));
			} else {
				requestFullPeer(peer);
			}
		}
	} else if (error == u"SCHEDULE_STATUS_PRIVATE"_q) {
		auto &scheduled = _session->scheduledMessages();
		Assert(peer->isUser());
		if (const auto item = scheduled.lookupItem(peer->id, itemId.msg)) {
			scheduled.removeSending(item);
			if (show) {
				show->showBox(
					Ui::MakeInformBox(tr::lng_cant_do_this()),
					Ui::LayerOption::CloseOther);
			}
		}
	} else if (error == u"PREMIUM_ACCOUNT_REQUIRED"_q) {
		Settings::ShowPremium(&session(), "premium_stickers");
	} else if (error == u"SCHEDULE_TOO_MUCH"_q) {
		auto &scheduled = _session->scheduledMessages();
		if (const auto item = scheduled.lookupItem(peer->id, itemId.msg)) {
			scheduled.removeSending(item);
		}
		if (show) {
			show->showToast(tr::lng_error_schedule_limit(tr::now));
		}
	} else if (error.startsWith(paidStarsPrefix)) {
		if (show) {
			show->showToast(
				u"Payment requirements changed. Please, try again."_q);
		}
		if (const auto stars = error.mid(paidStarsPrefix.size()).toInt()) {
			if (const auto user = peer->asUser()) {
				user->setStarsPerMessage(stars);
			} else if (const auto channel = peer->asChannel()) {
				channel->setStarsPerMessage(stars);
			}
		}
		peer->updateFull();
	} else if (show) {
		show->showToast(error);
	}
	if (const auto item = _session->data().message(itemId)) {
		Assert(randomId != 0);
		_session->data().unregisterMessageRandomId(randomId);
		item->sendFailed();

		if (error == u"TOPIC_CLOSED"_q) {
			if (const auto topic = item->topic()) {
				topic->setClosed(true);
			}
		}
	}
}

void ApiWrap::requestMessageData(
		PeerData *peer,
		MsgId msgId,
		Fn<void()> done) {
	auto &requests = (peer && peer->isChannel())
		? _channelMessageDataRequests[peer->asChannel()][msgId]
		: _messageDataRequests[msgId];
	if (done) {
		requests.callbacks.push_back(std::move(done));
	}
	if (!requests.requestId) {
		_messageDataResolveDelayed.call();
	}
}

QVector<MTPInputMessage> ApiWrap::collectMessageIds(
		const MessageDataRequests &requests) {
	auto result = QVector<MTPInputMessage>();
	result.reserve(requests.size());
	for (const auto &[msgId, request] : requests) {
		if (request.requestId > 0) {
			continue;
		}
		result.push_back(MTP_inputMessageID(MTP_int(msgId)));
	}
	return result;
}

auto ApiWrap::messageDataRequests(ChannelData *channel, bool onlyExisting)
-> MessageDataRequests* {
	if (!channel) {
		return &_messageDataRequests;
	}
	const auto i = _channelMessageDataRequests.find(channel);
	if (i != end(_channelMessageDataRequests)) {
		return &i->second;
	} else if (onlyExisting) {
		return nullptr;
	}
	return &_channelMessageDataRequests.emplace(
		channel,
		MessageDataRequests()
	).first->second;
}

void ApiWrap::resolveMessageDatas() {
	if (_messageDataRequests.empty() && _channelMessageDataRequests.empty()) {
		return;
	}

	const auto ids = collectMessageIds(_messageDataRequests);
	if (!ids.isEmpty()) {
		const auto requestId = (_takeoutId && _takeoutPeerId != 0 && !peerIsChannel(_takeoutPeerId))
			? request(MTPInvokeWithTakeout<MTPmessages_GetMessages>(
				MTP_long(*_takeoutId),
				MTPmessages_GetMessages(
					MTP_vector<MTPInputMessage>(ids))
			)).done([=](
					const MTPmessages_Messages &result,
					mtpRequestId requestId) {
				_session->data().processExistingMessages(nullptr, result);
				finalizeMessageDataRequest(nullptr, requestId);
			}).fail([=](const MTP::Error &error, mtpRequestId requestId) {
				finalizeMessageDataRequest(nullptr, requestId);
			}).afterDelay(kSmallDelayMs).toDC(
				MTP::ShiftDcId(0, MTP::kExportDcShift)
			).send()
			: request(MTPmessages_GetMessages(
				MTP_vector<MTPInputMessage>(ids)
			)).done([=](
					const MTPmessages_Messages &result,
					mtpRequestId requestId) {
				_session->data().processExistingMessages(nullptr, result);
				finalizeMessageDataRequest(nullptr, requestId);
			}).fail([=](const MTP::Error &error, mtpRequestId requestId) {
				finalizeMessageDataRequest(nullptr, requestId);
			}).afterDelay(kSmallDelayMs).send();

		for (auto &[msgId, request] : _messageDataRequests) {
			if (request.requestId > 0) {
				continue;
			}
			request.requestId = requestId;
		}
	}
	for (auto j = _channelMessageDataRequests.begin(); j != _channelMessageDataRequests.cend();) {
		if (j->second.empty()) {
			j = _channelMessageDataRequests.erase(j);
			continue;
		}
		const auto ids = collectMessageIds(j->second);
		if (!ids.isEmpty()) {
			const auto channel = j->first;
			const auto requestId = (_takeoutId
				&& channel->id == _takeoutPeerId)
				? request(MTPInvokeWithTakeout<MTPchannels_GetMessages>(
					MTP_long(*_takeoutId),
					MTPchannels_GetMessages(
						channel->inputChannel(),
						MTP_vector<MTPInputMessage>(ids))
				)).done([=](
						const MTPmessages_Messages &result,
						mtpRequestId requestId) {
					_session->data().processExistingMessages(channel, result);
					finalizeMessageDataRequest(channel, requestId);
				}).fail([=](const MTP::Error &error, mtpRequestId requestId) {
					finalizeMessageDataRequest(channel, requestId);
				}).afterDelay(kSmallDelayMs).toDC(
					MTP::ShiftDcId(0, MTP::kExportDcShift)
				).send()
				: request(MTPchannels_GetMessages(
					channel->inputChannel(),
					MTP_vector<MTPInputMessage>(ids)
				)).done([=](
						const MTPmessages_Messages &result,
						mtpRequestId requestId) {
					_session->data().processExistingMessages(channel, result);
					finalizeMessageDataRequest(channel, requestId);
				}).fail([=](const MTP::Error &error, mtpRequestId requestId) {
					finalizeMessageDataRequest(channel, requestId);
				}).afterDelay(kSmallDelayMs).send();

			for (auto &[msgId, request] : j->second) {
				if (request.requestId > 0) {
					continue;
				}
				request.requestId = requestId;
			}
		}
		++j;
	}
}

void ApiWrap::finalizeMessageDataRequest(
		ChannelData *channel,
		mtpRequestId requestId) {
	auto requests = messageDataRequests(channel, true);
	if (!requests) {
		return;
	}
	auto callbacks = std::vector<Fn<void()>>();
	for (auto i = requests->begin(); i != requests->cend();) {
		if (i->second.requestId == requestId) {
			auto &list = i->second.callbacks;
			if (callbacks.empty()) {
				callbacks = std::move(list);
			} else {
				callbacks.insert(
					end(callbacks),
					std::make_move_iterator(begin(list)),
					std::make_move_iterator(end(list)));
			}
			i = requests->erase(i);
		} else {
			++i;
		}
	}
	if (channel && requests->empty()) {
		_channelMessageDataRequests.remove(channel);
	}
	for (const auto &callback : callbacks) {
		callback();
	}
}

void ApiWrap::exportMessageAsBase64(not_null<HistoryItem*> item, Fn<void(const QString&)> done, Fn<void()> fail) {
	auto ids = QVector<MTPInputMessage>{ MTP_inputMessageID(MTP_int(item->id)) };
	auto requestDone = [=](
		const MTPmessages_Messages& result,
		const MTP::Response& response) {
			auto buffer = response.reply;
			QByteArray byteArray(reinterpret_cast<const char*>(buffer.data()), buffer.size() * sizeof(mtpPrime));
			QString base64String = byteArray.toBase64(QByteArray::Base64UrlEncoding);
			done(base64String);
		};
	if (item->history()->peer->isChannel()) {
		request(MTPchannels_GetMessages(
			item->history()->peer->asChannel()->inputChannel(),
			MTP_vector<MTPInputMessage>(ids)
		)).done(requestDone).fail([=](const MTP::Error& error, mtpRequestId requestId) {
			fail();
		}).send();
	} else {
		request(MTPmessages_GetMessages(
			MTP_vector<MTPInputMessage>(ids)
		)).done(requestDone).fail([=](const MTP::Error& error, mtpRequestId requestId) {
			fail();
		}).send();
	}
}

QString ApiWrap::exportDirectMessageLink(
		not_null<HistoryItem*> item,
		bool inRepliesContext,
		bool forceNonPublicLink,
		std::optional<TimeId> videoTimestamp) {
	Expects(item->history()->peer->isChannel());

	const auto itemId = item->fullId();
	const auto channel = item->history()->peer->asChannel();
	const auto fallback = [&] {
		auto linkChannel = channel;
		auto linkItemId = item->id;
		auto linkCommentId = MsgId();
		auto linkThreadId = MsgId();
		auto linkThreadIsTopic = false;
		if (inRepliesContext) {
			linkThreadIsTopic = item->history()->isForum();
			const auto rootId = linkThreadIsTopic
				? item->topicRootId()
				: item->replyToTop();
			if (rootId) {
				const auto root = item->history()->owner().message(
					channel->id,
					rootId);
				const auto sender = root
					? root->discussionPostOriginalSender()
					: nullptr;
				if (sender && sender->hasUsername() && !forceNonPublicLink) {
					// Comment to a public channel.
					const auto forwarded = root->Get<HistoryMessageForwarded>();
					linkItemId = forwarded->savedFromMsgId;
					if (linkItemId) {
						linkChannel = sender;
						linkCommentId = item->id;
					} else {
						linkItemId = item->id;
					}
				} else {
					// Reply in a thread, maybe comment in a private channel.
					linkThreadId = rootId;
				}
			}
		}
		const auto base = (linkChannel->hasUsername() && !forceNonPublicLink)
			? linkChannel->username()
			: "c/" + QString::number(peerToChannel(linkChannel->id).bare);
		const auto post = QString::number(linkItemId.bare);
		const auto query = base
			+ '/'
			+ (linkCommentId
				? (post + "?comment=" + QString::number(linkCommentId.bare))
				: (linkThreadId && !linkThreadIsTopic)
				? (post + "?thread=" + QString::number(linkThreadId.bare))
				: linkThreadId
				? (QString::number(linkThreadId.bare) + '/' + post)
				: post);
		return session().createInternalLinkFull(query);
	};
	if (forceNonPublicLink) {
		return fallback();
	}
	const auto i = _unlikelyMessageLinks.find(itemId);
	const auto current = (i != end(_unlikelyMessageLinks))
		? i->second
		: fallback();
	request(MTPchannels_ExportMessageLink(
		MTP_flags(inRepliesContext
			? MTPchannels_ExportMessageLink::Flag::f_thread
			: MTPchannels_ExportMessageLink::Flag(0)),
		channel->inputChannel(),
		MTP_int(item->id)
	)).done([=](const MTPExportedMessageLink &result) {
		const auto link = qs(result.data().vlink());
		if (current != link) {
			_unlikelyMessageLinks.emplace_or_assign(itemId, link);
		}
	}).send();
	const auto addTimestamp = channel->hasUsername()
		&& !inRepliesContext
		&& videoTimestamp.has_value();
	const auto addedSeparator = (current.indexOf('?') >= 0) ? '&' : '?';
	const auto addedTimestamp = addTimestamp
		? (addedSeparator + u"t="_q + FormatVideoTimestamp(*videoTimestamp))
		: QString();
	return current + addedTimestamp;
}

QString ApiWrap::exportDirectStoryLink(not_null<Data::Story*> story) {
	const auto storyId = story->fullId();
	const auto peer = story->peer();
	const auto fallback = [&] {
		const auto base = peer->username();
		const auto id = story->call()
			? u"live"_q
			: QString::number(storyId.story);
		const auto query = base + "/s/" + id;
		return session().createInternalLinkFull(query);
	};
	const auto i = _unlikelyStoryLinks.find(storyId);
	const auto current = (i != end(_unlikelyStoryLinks))
		? i->second
		: fallback();
	request(MTPstories_ExportStoryLink(
		peer->input(),
		MTP_int(story->id())
	)).done([=](const MTPExportedStoryLink &result) {
		const auto link = qs(result.data().vlink());
		if (current != link) {
			_unlikelyStoryLinks.emplace_or_assign(storyId, link);
		}
	}).send();
	return current;
}

void ApiWrap::requestContacts() {
	if (_session->data().contactsLoaded().current() || _contactsRequestId) {
		return;
	}
	_contactsRequestId = request(MTPcontacts_GetContacts(
		MTP_long(0) // hash
	)).done([=](const MTPcontacts_Contacts &result) {
		_contactsRequestId = 0;
		if (result.type() == mtpc_contacts_contactsNotModified) {
			return;
		}
		Assert(result.type() == mtpc_contacts_contacts);
		const auto &d = result.c_contacts_contacts();
		_session->data().processUsers(d.vusers());
		for (const auto &contact : d.vcontacts().v) {
			if (contact.type() != mtpc_contact) continue;

			const auto userId = UserId(contact.c_contact().vuser_id());
			if (userId == _session->userId()) {
				_session->user()->setIsContact(true);
			}
		}
		_session->data().contactsLoaded() = true;
	}).fail([=] {
		_contactsRequestId = 0;
	}).send();
}

void ApiWrap::requestDialogs(Data::Folder *folder) {
	if (folder && !_foldersLoadState.contains(folder)) {
		_foldersLoadState.emplace(folder, DialogsLoadState());
	}
	requestMoreDialogs(folder);
}

void ApiWrap::requestMoreDialogs(Data::Folder *folder) {
	const auto state = dialogsLoadState(folder);
	if (!state) {
		return;
	} else if (state->requestId) {
		return;
	} else if (_dialogsLoadBlockedByDate.current()) {
		return;
	}

	const auto firstLoad = !state->offsetDate;
	const auto loadCount = firstLoad ? kDialogsFirstLoad : kDialogsPerPage;
	const auto flags = MTPmessages_GetDialogs::Flag::f_exclude_pinned
		| MTPmessages_GetDialogs::Flag::f_folder_id;
	const auto hash = uint64(0);
	state->requestId = request(MTPmessages_GetDialogs(
		MTP_flags(flags),
		MTP_int(folder ? folder->id() : 0),
		MTP_int(state->offsetDate),
		MTP_int(state->offsetId),
		(state->offsetPeer
			? state->offsetPeer->input()
			: MTP_inputPeerEmpty()),
		MTP_int(loadCount),
		MTP_long(hash)
	)).done([=](const MTPmessages_Dialogs &result) {
		const auto state = dialogsLoadState(folder);
		const auto count = result.match([](
				const MTPDmessages_dialogsNotModified &) {
			LOG(("API Error: not-modified received for requested dialogs."));
			return 0;
		}, [&](const MTPDmessages_dialogs &data) {
			if (state) {
				state->listReceived = true;
				dialogsLoadFinish(folder); // may kill 'state'.
			}
			return int(data.vdialogs().v.size());
		}, [&](const MTPDmessages_dialogsSlice &data) {
			updateDialogsOffset(
				folder,
				data.vdialogs().v,
				data.vmessages().v);
			return data.vcount().v;
		});
		result.match([](const MTPDmessages_dialogsNotModified & data) {
			LOG(("API Error: not-modified received for requested dialogs."));
		}, [&](const auto &data) {
			_session->data().processUsers(data.vusers());
			_session->data().processChats(data.vchats());
			_session->data().applyDialogs(
				folder,
				data.vmessages().v,
				data.vdialogs().v,
				count);
		});

		if (!folder
			&& (!_dialogsLoadState || !_dialogsLoadState->listReceived)) {
			refreshDialogsLoadBlocked();
		}
		requestMoreDialogsIfNeeded();
		_session->data().chatsListChanged(folder);
	}).fail([=] {
		dialogsLoadState(folder)->requestId = 0;
	}).send();

	if (!state->pinnedReceived) {
		requestPinnedDialogs(folder);
	}
	if (!folder) {
		refreshDialogsLoadBlocked();
	}
}

void ApiWrap::refreshDialogsLoadBlocked() {
	_dialogsLoadMayBlockByDate = _dialogsLoadState
		&& !_dialogsLoadState->listReceived
		&& (_dialogsLoadTill > 0);
	_dialogsLoadBlockedByDate = _dialogsLoadState
		&& !_dialogsLoadState->listReceived
		&& !_dialogsLoadState->requestId
		&& (_dialogsLoadTill > 0)
		&& (_dialogsLoadState->offsetDate > 0)
		&& (_dialogsLoadState->offsetDate <= _dialogsLoadTill);
}

void ApiWrap::requestMoreDialogsIfNeeded() {
	const auto dialogsReady = !_dialogsLoadState
		|| _dialogsLoadState->listReceived;
	if (_session->data().chatsFilters().loadNextExceptions(dialogsReady)) {
		return;
	} else if (_dialogsLoadState && !_dialogsLoadState->listReceived) {
		if (_dialogsLoadState->requestId) {
			return;
		}
		requestDialogs(nullptr);
	} else if (const auto folder = _session->data().folderLoaded(
			Data::Folder::kId)) {
		if (_session->data().chatsFilters().archiveNeeded()) {
			requestMoreDialogs(folder);
		}
	}
	requestContacts();
	_session->data().shortcutMessages().preloadShortcuts();
}

void ApiWrap::updateDialogsOffset(
		Data::Folder *folder,
		const QVector<MTPDialog> &dialogs,
		const QVector<MTPMessage> &messages) {
	auto lastDate = TimeId(0);
	auto lastPeer = PeerId(0);
	auto lastMsgId = MsgId(0);
	for (const auto &dialog : ranges::views::reverse(dialogs)) {
		dialog.match([&](const auto &dialog) {
			const auto peer = peerFromMTP(dialog.vpeer());
			const auto messageId = dialog.vtop_message().v;
			if (!peer || !messageId) {
				return;
			}
			if (!lastPeer) {
				lastPeer = peer;
			}
			if (!lastMsgId) {
				lastMsgId = messageId;
			}
			for (const auto &message : ranges::views::reverse(messages)) {
				if (IdFromMessage(message) == messageId
					&& PeerFromMessage(message) == peer) {
					if (const auto date = DateFromMessage(message)) {
						lastDate = date;
					}
					return;
				}
			}
		});
		if (lastDate) {
			break;
		}
	}
	if (const auto state = dialogsLoadState(folder)) {
		if (lastDate) {
			state->offsetDate = lastDate;
			state->offsetId = lastMsgId;
			state->offsetPeer = _session->data().peer(lastPeer);
			state->requestId = 0;
		} else {
			state->listReceived = true;
			dialogsLoadFinish(folder);
		}
	}
}

auto ApiWrap::dialogsLoadState(Data::Folder *folder) -> DialogsLoadState* {
	if (!folder) {
		return _dialogsLoadState.get();
	}
	const auto i = _foldersLoadState.find(folder);
	return (i != end(_foldersLoadState)) ? &i->second : nullptr;
}

void ApiWrap::dialogsLoadFinish(Data::Folder *folder) {
	const auto notify = [&] {
		Core::App().postponeCall(crl::guard(_session, [=] {
			_session->data().chatsListDone(folder);
		}));
	};
	const auto state = dialogsLoadState(folder);
	if (!state || !state->listReceived || !state->pinnedReceived) {
		return;
	}
	if (folder) {
		_foldersLoadState.remove(folder);
		notify();
	} else {
		_dialogsLoadState = nullptr;
		notify();
	}
}

void ApiWrap::requestPinnedDialogs(Data::Folder *folder) {
	const auto state = dialogsLoadState(folder);
	if (!state || state->pinnedReceived || state->pinnedRequestId) {
		return;
	}

	const auto finalize = [=] {
		if (const auto state = dialogsLoadState(folder)) {
			state->pinnedRequestId = 0;
			state->pinnedReceived = true;
			dialogsLoadFinish(folder);
		}
	};
	state->pinnedRequestId = request(MTPmessages_GetPinnedDialogs(
		MTP_int(folder ? folder->id() : 0)
	)).done([=](const MTPmessages_PeerDialogs &result) {
		finalize();
		result.match([&](const MTPDmessages_peerDialogs &data) {
			_session->data().processUsers(data.vusers());
			_session->data().processChats(data.vchats());
			_session->data().clearPinnedChats(folder);
			_session->data().applyDialogs(
				folder,
				data.vmessages().v,
				data.vdialogs().v);
			_session->data().chatsListChanged(folder);
			_session->data().notifyPinnedDialogsOrderUpdated();
		});
	}).fail([=] {
		finalize();
	}).send();
}

void ApiWrap::requestMoreBlockedByDateDialogs() {
	if (!_dialogsLoadState) {
		return;
	}
	const auto max = _session->settings().supportChatsTimeSlice();
	_dialogsLoadTill = _dialogsLoadState->offsetDate
		? (_dialogsLoadState->offsetDate - max)
		: (base::unixtime::now() - max);
	refreshDialogsLoadBlocked();
	requestDialogs();
}

rpl::producer<bool> ApiWrap::dialogsLoadMayBlockByDate() const {
	return _dialogsLoadMayBlockByDate.value();
}

rpl::producer<bool> ApiWrap::dialogsLoadBlockedByDate() const {
	return _dialogsLoadBlockedByDate.value();
}

void ApiWrap::requestWallPaper(
		const QString &slug,
		Fn<void(const Data::WallPaper &)> done,
		Fn<void()> fail) {
	if (_wallPaperSlug != slug) {
		_wallPaperSlug = slug;
		if (_wallPaperRequestId) {
			request(base::take(_wallPaperRequestId)).cancel();
		}
	}
	_wallPaperDone = std::move(done);
	_wallPaperFail = std::move(fail);
	if (_wallPaperRequestId) {
		return;
	}
	_wallPaperRequestId = request(MTPaccount_GetWallPaper(
		MTP_inputWallPaperSlug(MTP_string(slug))
	)).done([=](const MTPWallPaper &result) {
		_wallPaperRequestId = 0;
		_wallPaperSlug = QString();
		if (const auto paper = Data::WallPaper::Create(_session, result)) {
			if (const auto done = base::take(_wallPaperDone)) {
				done(*paper);
			}
		} else if (const auto fail = base::take(_wallPaperFail)) {
			fail();
		}
	}).fail([=](const MTP::Error &error) {
		_wallPaperRequestId = 0;
		_wallPaperSlug = QString();
		if (const auto fail = base::take(_wallPaperFail)) {
			fail();
		}
	}).send();
}

void ApiWrap::requestFullPeer(not_null<PeerData*> peer) {
	if (_fullPeerRequests.contains(peer)) {
		return;
	} else if (!peer->isUser() && !peer->barSettings().has_value()) {
		requestPeerSettings(peer);
	}

	const auto requestId = [&] {
		const auto failHandler = [=](const MTP::Error &error) {
			_fullPeerRequests.remove(peer);
			migrateFail(peer, error.type());
		};
		if (const auto user = peer->asUser()) {
			if (_session->supportMode()) {
				_session->supportHelper().refreshInfo(user);
			}
			return request(MTPusers_GetFullUser(
				user->inputUser()
			)).done([=](const MTPusers_UserFull &result) {
				result.match([&](const MTPDusers_userFull &data) {
					_session->data().processUsers(data.vusers());
					_session->data().processChats(data.vchats());
				});
				gotUserFull(user, result);
			}).fail(failHandler).send();
		} else if (const auto chat = peer->asChat()) {
			return request(MTPmessages_GetFullChat(
				chat->inputChat()
			)).done([=](const MTPmessages_ChatFull &result) {
				gotChatFull(peer, result);
			}).fail(failHandler).send();
		} else if (const auto channel = peer->asChannel()) {
			return request(MTPchannels_GetFullChannel(
				channel->inputChannel()
			)).done([=](const MTPmessages_ChatFull &result) {
				gotChatFull(peer, result);
				migrateDone(channel, channel);
			}).fail(failHandler).send();
		}
		Unexpected("Peer type in requestFullPeer.");
	}();
	_fullPeerRequests.emplace(peer, requestId);
}

void ApiWrap::processFullPeer(
		not_null<PeerData*> peer,
		const MTPmessages_ChatFull &result) {
	gotChatFull(peer, result);
}

void ApiWrap::gotChatFull(
		not_null<PeerData*> peer,
		const MTPmessages_ChatFull &result) {
	const auto &d = result.c_messages_chatFull();
	_session->data().applyMaximumChatVersions(d.vchats());

	_session->data().processUsers(d.vusers());
	_session->data().processChats(d.vchats());

	d.vfull_chat().match([&](const MTPDchatFull &data) {
		if (const auto chat = peer->asChat()) {
			Data::ApplyChatUpdate(chat, data);
		} else {
			LOG(("MTP Error: bad type in gotChatFull for channel: %1"
				).arg(d.vfull_chat().type()));
		}
	}, [&](const MTPDchannelFull &data) {
		if (const auto channel = peer->asChannel()) {
			Data::ApplyChannelUpdate(channel, data);
		} else {
			LOG(("MTP Error: bad type in gotChatFull for chat: %1"
				).arg(d.vfull_chat().type()));
		}
	});

	_fullPeerRequests.remove(peer);
	_session->changes().peerUpdated(
		peer,
		Data::PeerUpdate::Flag::FullInfo);
}

void ApiWrap::gotUserFull(
		not_null<UserData*> user,
		const MTPusers_UserFull &result) {
	result.match([&](const MTPDusers_userFull &data) {
		data.vfull_user().match([&](const MTPDuserFull &fields) {
			if (user == _session->user() && !_session->validateSelf(fields.vid().v)) {
				constexpr auto kRequestUserAgainTimeout = crl::time(10000);
				base::call_delayed(kRequestUserAgainTimeout, _session, [=] {
					requestFullPeer(user);
				});
				return;
			}
			Data::ApplyUserUpdate(user, fields);
		});
	});
	_fullPeerRequests.remove(user);
	_session->changes().peerUpdated(
		user,
		Data::PeerUpdate::Flag::FullInfo);
}

void ApiWrap::requestPeerSettings(not_null<PeerData*> peer) {
	if (!_requestedPeerSettings.emplace(peer).second) {
		return;
	} else if (peer->isMonoforum()) {
		peer->setBarSettings(PeerBarSettings());
		_requestedPeerSettings.erase(peer);
		return;
	}
	request(MTPmessages_GetPeerSettings(
		peer->input()
	)).done([=](const MTPmessages_PeerSettings &result) {
		result.match([&](const MTPDmessages_peerSettings &data) {
			_session->data().processUsers(data.vusers());
			_session->data().processChats(data.vchats());
			peer->setBarSettings(data.vsettings());
			_requestedPeerSettings.erase(peer);
		});
	}).fail([=] {
		peer->setBarSettings(PeerBarSettings());
		_requestedPeerSettings.erase(peer);
	}).send();
}

void ApiWrap::migrateChat(
		not_null<ChatData*> chat,
		FnMut<void(not_null<ChannelData*>)> done,
		Fn<void(const QString &)> fail) {
	const auto callback = [&] {
		return MigrateCallbacks{ std::move(done), std::move(fail) };
	};
	const auto i = _migrateCallbacks.find(chat);
	if (i != _migrateCallbacks.end()) {
		i->second.push_back(callback());
		return;
	}
	_migrateCallbacks.emplace(chat).first->second.push_back(callback());
	if (const auto channel = chat->migrateTo()) {
		session().changes().peerUpdated(
			chat,
			Data::PeerUpdate::Flag::Migration);
		crl::on_main([=] {
			migrateDone(chat, channel);
		});
	} else if (chat->isDeactivated()) {
		crl::on_main([=] {
			migrateFail(
				chat,
				MTP::Error::Local(
					"BAD_MIGRATION",
					"Chat is already deactivated").type());
		});
		return;
	} else if (!chat->amCreator()) {
		crl::on_main([=] {
			migrateFail(
				chat,
				MTP::Error::Local(
					"BAD_MIGRATION",
					"Current user is not the creator of that chat").type());
		});
		return;
	}

	request(MTPmessages_MigrateChat(
		chat->inputChat()
	)).done([=](const MTPUpdates &result) {
		applyUpdates(result);
		session().changes().sendNotifications();

		if (const auto channel = chat->migrateTo()) {
			if (auto handlers = _migrateCallbacks.take(chat)) {
				_migrateCallbacks.emplace(channel, std::move(*handlers));
			}
			requestFullPeer(channel);
		} else {
			migrateFail(
				chat,
				MTP::Error::Local("MIGRATION_FAIL", "No channel").type());
		}
	}).fail([=](const MTP::Error &error) {
		migrateFail(chat, error.type());
	}).send();
}

void ApiWrap::migrateDone(
		not_null<PeerData*> peer,
		not_null<ChannelData*> channel) {
	session().changes().sendNotifications();
	if (auto handlers = _migrateCallbacks.take(peer)) {
		for (auto &handler : *handlers) {
			if (handler.done) {
				handler.done(channel);
			}
		}
	}
}

void ApiWrap::migrateFail(not_null<PeerData*> peer, const QString &error) {
	if (error == u"CHANNELS_TOO_MUCH"_q) {
		ShowChannelsLimitBox(peer);
	}
	if (auto handlers = _migrateCallbacks.take(peer)) {
		for (auto &handler : *handlers) {
			if (handler.fail) {
				handler.fail(error);
			}
		}
	}
}

void ApiWrap::markContentsRead(
		const base::flat_set<not_null<HistoryItem*>> &items) {
	auto markedIds = QVector<MTPint>();
	auto channelMarkedIds = base::flat_map<
		not_null<ChannelData*>,
		QVector<MTPint>>();
	markedIds.reserve(items.size());
	for (const auto &item : items) {
		if (!item->markContentsRead(true) || !item->isRegular()) {
			continue;
		}
		if (const auto channel = item->history()->peer->asChannel()) {
			channelMarkedIds[channel].push_back(MTP_int(item->id));
		} else {
			markedIds.push_back(MTP_int(item->id));
		}
	}
	if (!markedIds.isEmpty()) {
		request(MTPmessages_ReadMessageContents(
			MTP_vector<MTPint>(markedIds)
		)).done([=](const MTPmessages_AffectedMessages &result) {
			applyAffectedMessages(result);
		}).send();
	}
	for (const auto &channelIds : channelMarkedIds) {
		request(MTPchannels_ReadMessageContents(
			channelIds.first->inputChannel(),
			MTP_vector<MTPint>(channelIds.second)
		)).send();
	}
}

void ApiWrap::markContentsRead(not_null<HistoryItem*> item) {
	if (!item->markContentsRead(true) || !item->isRegular()) {
		return;
	}
	const auto ids = MTP_vector<MTPint>(1, MTP_int(item->id));
	if (const auto channel = item->history()->peer->asChannel()) {
		request(MTPchannels_ReadMessageContents(
			channel->inputChannel(),
			ids
		)).send();
	} else {
		request(MTPmessages_ReadMessageContents(
			ids
		)).done([=](const MTPmessages_AffectedMessages &result) {
			applyAffectedMessages(result);
		}).send();
	}
}

void ApiWrap::deleteAllFromParticipant(
		not_null<ChannelData*> channel,
		not_null<PeerData*> from) {
	const auto history = _session->data().historyLoaded(channel);
	const auto ids = history
		? history->collectMessagesFromParticipantToDelete(from)
		: std::vector<MsgId>();
	for (const auto &msgId : ids) {
		if (const auto item = _session->data().message(channel->id, msgId)) {
			item->destroy();
		}
	}

	_session->data().sendHistoryChangeNotifications();

	deleteAllFromParticipantSend(channel, from);
}

void ApiWrap::deleteAllFromParticipantSend(
		not_null<ChannelData*> channel,
		not_null<PeerData*> from) {
	request(MTPchannels_DeleteParticipantHistory(
		channel->inputChannel(),
		from->input()
	)).done([=](const MTPmessages_AffectedHistory &result) {
		const auto offset = applyAffectedHistory(channel, result);
		if (offset > 0) {
			deleteAllFromParticipantSend(channel, from);
		} else if (const auto history = _session->data().historyLoaded(channel)) {
			history->requestChatListMessage();
		}
	}).send();
}

void ApiWrap::deleteSublistHistory(
		not_null<ChannelData*> channel,
		not_null<PeerData*> sublistPeer) {
	deleteSublistHistorySend(channel, sublistPeer);
}

void ApiWrap::deleteSublistHistorySend(
		not_null<ChannelData*> parentChat,
		not_null<PeerData*> sublistPeer) {
	request(MTPmessages_DeleteSavedHistory(
		MTP_flags(MTPmessages_DeleteSavedHistory::Flag::f_parent_peer),
		parentChat->input(),
		sublistPeer->input(),
		MTP_int(0), // max_id
		MTP_int(0), // min_date
		MTP_int(0) // max_date
	)).done([=](const MTPmessages_AffectedHistory &result) {
		const auto offset = applyAffectedHistory(parentChat, result);
		if (offset > 0) {
			deleteSublistHistorySend(parentChat, sublistPeer);
		} else if (const auto monoforum = parentChat->monoforum()) {
			monoforum->applySublistDeleted(sublistPeer);
		}
	}).send();
}

void ApiWrap::scheduleStickerSetRequest(uint64 setId, uint64 access) {
	if (!_stickerSetRequests.contains(setId)) {
		_stickerSetRequests.emplace(setId, StickerSetRequest{ access });
	}
}

void ApiWrap::requestStickerSets() {
	for (auto &[id, info] : _stickerSetRequests) {
		if (info.id) {
			continue;
		}
		info.id = request(MTPmessages_GetStickerSet(
			MTP_inputStickerSetID(
				MTP_long(id),
				MTP_long(info.accessHash)),
			MTP_int(0) // hash
		)).done([=, setId = id](const MTPmessages_StickerSet &result) {
			gotStickerSet(setId, result);
		}).fail([=, setId = id] {
			_stickerSetRequests.remove(setId);
		}).afterDelay(kSmallDelayMs).send();
	}
}

void ApiWrap::saveStickerSets(
		const Data::StickersSetsOrder &localOrder,
		const Data::StickersSetsOrder &localRemoved,
		Data::StickersType type) {
	auto &setDisenableRequests = (type == Data::StickersType::Emoji)
		? _customEmojiSetDisenableRequests
		: (type == Data::StickersType::Masks)
		? _maskSetDisenableRequests
		: _stickerSetDisenableRequests;
	const auto reorderRequestId = [=]() -> mtpRequestId & {
		return (type == Data::StickersType::Emoji)
			? _customEmojiReorderRequestId
			: (type == Data::StickersType::Masks)
			? _masksReorderRequestId
			: _stickersReorderRequestId;
	};
	for (auto requestId : base::take(setDisenableRequests)) {
		request(requestId).cancel();
	}
	request(base::take(reorderRequestId())).cancel();
	request(base::take(_stickersClearRecentRequestId)).cancel();
	request(base::take(_stickersClearRecentAttachedRequestId)).cancel();

	const auto stickersSaveOrder = [=] {
		if (localOrder.size() < 2) {
			return;
		}
		QVector<MTPlong> mtpOrder;
		mtpOrder.reserve(localOrder.size());
		for (const auto setId : std::as_const(localOrder)) {
			mtpOrder.push_back(MTP_long(setId));
		}

		using Flag = MTPmessages_ReorderStickerSets::Flag;
		const auto flags = (type == Data::StickersType::Emoji)
			? Flag::f_emojis
			: (type == Data::StickersType::Masks)
			? Flag::f_masks
			: Flag(0);
		reorderRequestId() = request(MTPmessages_ReorderStickerSets(
			MTP_flags(flags),
			MTP_vector<MTPlong>(mtpOrder)
		)).done([=] {
			reorderRequestId() = 0;
		}).fail([=] {
			reorderRequestId() = 0;
			if (type == Data::StickersType::Emoji) {
				_session->data().stickers().setLastEmojiUpdate(0);
				updateCustomEmoji();
			} else if (type == Data::StickersType::Masks) {
				_session->data().stickers().setLastMasksUpdate(0);
				updateMasks();
			} else {
				_session->data().stickers().setLastUpdate(0);
				updateStickers();
			}
		}).send();
	};

	const auto stickerSetDisenabled = [=](mtpRequestId requestId) {
		auto &setDisenableRequests = (type == Data::StickersType::Emoji)
			? _customEmojiSetDisenableRequests
			: (type == Data::StickersType::Masks)
			? _maskSetDisenableRequests
			: _stickerSetDisenableRequests;
		setDisenableRequests.remove(requestId);
		if (setDisenableRequests.empty()) {
			stickersSaveOrder();
		}
	};

	auto writeInstalled = true,
		writeRecent = false,
		writeCloudRecent = false,
		writeCloudRecentAttached = false,
		writeFaved = false,
		writeArchived = false;
	auto &recent = _session->data().stickers().getRecentPack();
	auto &sets = _session->data().stickers().setsRef();

	auto &order = (type == Data::StickersType::Emoji)
		? _session->data().stickers().emojiSetsOrder()
		: (type == Data::StickersType::Masks)
		? _session->data().stickers().maskSetsOrder()
		: _session->data().stickers().setsOrder();
	auto &orderRef = (type == Data::StickersType::Emoji)
		? _session->data().stickers().emojiSetsOrderRef()
		: (type == Data::StickersType::Masks)
		? _session->data().stickers().maskSetsOrderRef()
		: _session->data().stickers().setsOrderRef();

	using Flag = Data::StickersSetFlag;
	for (const auto removedSetId : localRemoved) {
		if ((removedSetId == Data::Stickers::CloudRecentSetId)
			|| (removedSetId == Data::Stickers::CloudRecentAttachedSetId)) {
			if (sets.remove(Data::Stickers::CloudRecentSetId) != 0) {
				writeCloudRecent = true;
			}
			if (sets.remove(Data::Stickers::CloudRecentAttachedSetId) != 0) {
				writeCloudRecentAttached = true;
			}
			if (sets.remove(Data::Stickers::CustomSetId)) {
				writeInstalled = true;
			}
			if (!recent.isEmpty()) {
				recent.clear();
				writeRecent = true;
			}

			const auto isAttached
				= (removedSetId == Data::Stickers::CloudRecentAttachedSetId);
			const auto flags = isAttached
				? MTPmessages_ClearRecentStickers::Flag::f_attached
				: MTPmessages_ClearRecentStickers::Flags(0);
			auto &requestId = isAttached
				? _stickersClearRecentAttachedRequestId
				: _stickersClearRecentRequestId;
			const auto finish = [=] {
				(isAttached
					? _stickersClearRecentAttachedRequestId
					: _stickersClearRecentRequestId) = 0;
			};
			requestId = request(MTPmessages_ClearRecentStickers(
				MTP_flags(flags)
			)).done(finish).fail(finish).send();
			continue;
		}

		auto it = sets.find(removedSetId);
		if (it != sets.cend()) {
			const auto set = it->second.get();
			for (auto i = recent.begin(); i != recent.cend();) {
				if (set->stickers.indexOf(i->first) >= 0) {
					i = recent.erase(i);
					writeRecent = true;
				} else {
					++i;
				}
			}
			const auto archived = !!(set->flags & Flag::Archived);
			if (!archived) {
				const auto featured = !!(set->flags & Flag::Featured);
				const auto special = !!(set->flags & Flag::Special);
				const auto emoji = !!(set->flags & Flag::Emoji);
				const auto locked = (set->locked > 0);
				const auto setId = set->mtpInput();

				auto requestId = request(MTPmessages_UninstallStickerSet(
					setId
				)).done([=](const MTPBool &result, mtpRequestId requestId) {
					stickerSetDisenabled(requestId);
				}).fail([=](const MTP::Error &error, mtpRequestId requestId) {
					stickerSetDisenabled(requestId);
				}).afterDelay(kSmallDelayMs).send();

				setDisenableRequests.insert(requestId);

				const auto removeIndex = order.indexOf(set->id);
				if (removeIndex >= 0) {
					orderRef.removeAt(removeIndex);
				}
				if (!featured && !special && !emoji && !locked) {
					sets.erase(it);
				} else {
					if (archived) {
						writeArchived = true;
					}
					set->flags &= ~(Flag::Installed | Flag::Archived);
					set->installDate = TimeId(0);
				}
			}
		}
	}

	// Clear all installed flags, set only for sets from order.
	for (auto &[id, set] : sets) {
		const auto archived = !!(set->flags & Flag::Archived);
		const auto thatType = !!(set->flags & Flag::Emoji)
			? Data::StickersType::Emoji
			: !!(set->flags & Flag::Masks)
			? Data::StickersType::Masks
			: Data::StickersType::Stickers;
		if (!archived && (type == thatType)) {
			set->flags &= ~Flag::Installed;
		}
	}

	orderRef.clear();
	for (const auto setId : std::as_const(localOrder)) {
		auto it = sets.find(setId);
		if (it == sets.cend()) {
			continue;
		}
		const auto set = it->second.get();
		const auto archived = !!(set->flags & Flag::Archived);
		if (archived && !localRemoved.contains(set->id)) {
			const auto mtpSetId = set->mtpInput();

			const auto requestId = request(MTPmessages_InstallStickerSet(
				mtpSetId,
				MTP_boolFalse()
			)).done([=](
					const MTPmessages_StickerSetInstallResult &result,
					mtpRequestId requestId) {
				stickerSetDisenabled(requestId);
			}).fail([=](
					const MTP::Error &error,
					mtpRequestId requestId) {
				stickerSetDisenabled(requestId);
			}).afterDelay(kSmallDelayMs).send();

			setDisenableRequests.insert(requestId);

			set->flags &= ~Flag::Archived;
			writeArchived = true;
		}
		orderRef.push_back(setId);
		set->flags |= Flag::Installed;
		if (!set->installDate) {
			set->installDate = base::unixtime::now();
		}
	}

	for (auto it = sets.begin(); it != sets.cend();) {
		const auto set = it->second.get();
		if ((set->flags & Flag::Featured)
			|| (set->flags & Flag::Installed)
			|| (set->flags & Flag::Archived)
			|| (set->flags & Flag::Special)
			|| (set->flags & Flag::Emoji)
			|| (set->locked > 0)) {
			++it;
		} else {
			it = sets.erase(it);
		}
	}

	auto &storage = local();
	if (writeInstalled) {
		if (type == Data::StickersType::Emoji) {
			storage.writeInstalledCustomEmoji();
		} else if (type == Data::StickersType::Masks) {
			storage.writeInstalledMasks();
		} else {
			storage.writeInstalledStickers();
		}
	}
	if (writeRecent) {
		session().saveSettings();
	}
	if (writeArchived) {
		if (type == Data::StickersType::Emoji) {
		} else if (type == Data::StickersType::Masks) {
			storage.writeArchivedMasks();
		} else {
			storage.writeArchivedStickers();
		}
	}
	if (writeCloudRecent) {
		storage.writeRecentStickers();
	}
	if (writeCloudRecentAttached) {
		storage.writeRecentMasks();
	}
	if (writeFaved) {
		storage.writeFavedStickers();
	}
	_session->data().stickers().notifyUpdated(type);

	if (setDisenableRequests.empty()) {
		stickersSaveOrder();
	} else {
		requestSendDelayed();
	}
}

void ApiWrap::joinChannel(not_null<ChannelData*> channel) {
	if (channel->amIn()) {
		session().changes().peerUpdated(
			channel,
			Data::PeerUpdate::Flag::ChannelAmIn);
	} else if (!_channelAmInRequests.contains(channel)) {
		const auto requestId = request(MTPchannels_JoinChannel(
			channel->inputChannel()
		)).done([=](const MTPUpdates &result) {
			_channelAmInRequests.remove(channel);
			applyUpdates(result);

			session().data().addRecentJoinChat({
				.fromPeerId = channel->id,
				.joinedPeerId = channel->id,
			});
		}).fail([=](const MTP::Error &error) {
			const auto &type = error.type();

			const auto show = ShowForPeer(channel);
			if (type == u"CHANNEL_PRIVATE"_q
				&& channel->invitePeekExpires()) {
				channel->privateErrorReceived();
			} else if (type == u"CHANNELS_TOO_MUCH"_q) {
				ShowChannelsLimitBox(channel);
			} else {
				const auto text = [&] {
					if (type == u"INVITE_REQUEST_SENT"_q) {
						return channel->isMegagroup()
							? tr::lng_group_request_sent(tr::now)
							: tr::lng_group_request_sent_channel(tr::now);
					} else if (type == u"CHANNEL_PRIVATE"_q
						|| type == u"CHANNEL_PUBLIC_GROUP_NA"_q
						|| type == u"USER_BANNED_IN_CHANNEL"_q) {
						return channel->isMegagroup()
							? tr::lng_group_not_accessible(tr::now)
							: tr::lng_channel_not_accessible(tr::now);
					} else if (type == u"USERS_TOO_MUCH"_q) {
						return tr::lng_group_full(tr::now);
					}
					return QString();
				}();
				if (show && !text.isEmpty()) {
					show->showToast(text, kJoinErrorDuration);
				}
			}
			_channelAmInRequests.remove(channel);
		}).send();

		_channelAmInRequests.emplace(channel, requestId);

		using Flag = ChannelDataFlag;
		chatParticipants().loadSimilarPeers(channel);
		channel->setFlags(channel->flags() | Flag::SimilarExpanded);
	}
}

void ApiWrap::leaveChannel(not_null<ChannelData*> channel) {
	if (!channel->amIn()) {
		session().changes().peerUpdated(
			channel,
			Data::PeerUpdate::Flag::ChannelAmIn);
	} else if (!_channelAmInRequests.contains(channel)) {
		auto requestId = request(MTPchannels_LeaveChannel(
			channel->inputChannel()
		)).done([=](const MTPUpdates &result) {
			_channelAmInRequests.remove(channel);
			applyUpdates(result);
		}).fail([=] {
			_channelAmInRequests.remove(channel);
		}).send();

		_channelAmInRequests.emplace(channel, requestId);
	}
}

void ApiWrap::requestNotifySettings(const MTPInputNotifyPeer &peer) {
	const auto bad = peer.match([](const MTPDinputNotifyUsers &) {
		return false;
	}, [](const MTPDinputNotifyChats &) {
		return false;
	}, [](const MTPDinputNotifyBroadcasts &) {
		return false;
	}, [&](const MTPDinputNotifyPeer &data) {
		if (data.vpeer().type() == mtpc_inputPeerEmpty) {
			LOG(("Api Error: Requesting settings for empty peer."));
			return true;
		}
		return false;
	}, [&](const MTPDinputNotifyForumTopic &data) {
		if (data.vpeer().type() == mtpc_inputPeerEmpty) {
			LOG(("Api Error: Requesting settings for empty peer topic."));
			return true;
		}
		return false;
	});
	if (bad) {
		return;
	}

	const auto peerFromInput = [&](const MTPInputPeer &inputPeer) {
		return inputPeer.match([&](const MTPDinputPeerSelf &) {
			return _session->userPeerId();
		}, [](const MTPDinputPeerEmpty &) {
			return PeerId(0);
		}, [](const MTPDinputPeerChannel &data) {
			return peerFromChannel(data.vchannel_id());
		}, [](const MTPDinputPeerChannelFromMessage &data) {
			return peerFromChannel(data.vchannel_id());
		}, [](const MTPDinputPeerChat &data) {
			return peerFromChat(data.vchat_id());
		}, [](const MTPDinputPeerUser &data) {
			return peerFromUser(data.vuser_id());
		}, [](const MTPDinputPeerUserFromMessage &data) {
			return peerFromUser(data.vuser_id());
		});
	};
	const auto key = peer.match([](const MTPDinputNotifyUsers &) {
		return NotifySettingsKey{ peerFromUser(1) };
	}, [](const MTPDinputNotifyChats &) {
		return NotifySettingsKey{ peerFromChat(1) };
	}, [](const MTPDinputNotifyBroadcasts &) {
		return NotifySettingsKey{ peerFromChannel(1) };
	}, [&](const MTPDinputNotifyPeer &data) {
		return NotifySettingsKey{ peerFromInput(data.vpeer()) };
	}, [&](const MTPDinputNotifyForumTopic &data) {
		return NotifySettingsKey{
			peerFromInput(data.vpeer()),
			data.vtop_msg_id().v,
		};
	});
	if (_notifySettingRequests.contains(key)) {
		return;
	}
	const auto requestId = request(MTPaccount_GetNotifySettings(
		peer
	)).done([=](const MTPPeerNotifySettings &result) {
		_session->data().notifySettings().apply(peer, result);
		_notifySettingRequests.remove(key);
	}).fail([=] {
		_session->data().notifySettings().apply(
			peer,
			MTP_peerNotifySettings(
				MTP_flags(0),
				MTPBool(),
				MTPBool(),
				MTPint(),
				MTPNotificationSound(),
				MTPNotificationSound(),
				MTPNotificationSound(),
				MTPBool(),
				MTPBool(),
				MTPNotificationSound(),
				MTPNotificationSound(),
				MTPNotificationSound()));
		_notifySettingRequests.erase(key);
	}).send();
	_notifySettingRequests.emplace(key, requestId);
}

void ApiWrap::updateNotifySettingsDelayed(
		not_null<const Data::Thread*> thread) {
	const auto topic = thread->asTopic();
	if (!topic) {
		return updateNotifySettingsDelayed(thread->peer());
	}
	if (_updateNotifyTopics.emplace(topic).second) {
		topic->destroyed(
		) | rpl::on_next([=] {
			_updateNotifyTopics.remove(topic);
		}, _updateNotifyQueueLifetime);
		_updateNotifyTimer.callOnce(kNotifySettingSaveTimeout);
	}
}

void ApiWrap::updateNotifySettingsDelayed(not_null<const PeerData*> peer) {
	if (_updateNotifyPeers.emplace(peer).second) {
		_updateNotifyTimer.callOnce(kNotifySettingSaveTimeout);
	}
}

void ApiWrap::updateNotifySettingsDelayed(Data::DefaultNotify type) {
	if (_updateNotifyDefaults.emplace(type).second) {
		_updateNotifyTimer.callOnce(kNotifySettingSaveTimeout);
	}
}

void ApiWrap::sendNotifySettingsUpdates() {
	_updateNotifyQueueLifetime.destroy();
	for (const auto &topic : base::take(_updateNotifyTopics)) {
		request(MTPaccount_UpdateNotifySettings(
			MTP_inputNotifyForumTopic(
				topic->peer()->input(),
				MTP_int(topic->rootId())),
			topic->notify().serialize()
		)).afterDelay(kSmallDelayMs).send();
	}
	for (const auto &peer : base::take(_updateNotifyPeers)) {
		request(MTPaccount_UpdateNotifySettings(
			MTP_inputNotifyPeer(peer->input()),
			peer->notify().serialize()
		)).afterDelay(kSmallDelayMs).send();
	}
	const auto &settings = session().data().notifySettings();
	for (const auto type : base::take(_updateNotifyDefaults)) {
		request(MTPaccount_UpdateNotifySettings(
			Data::DefaultNotifyToMTP(type),
			settings.defaultSettings(type).serialize()
		)).afterDelay(kSmallDelayMs).send();
	}
	session().mtp().sendAnything();
}

void ApiWrap::saveDraftToCloudDelayed(not_null<Data::Thread*> thread) {
	_draftsSaveRequestIds.emplace(base::make_weak(thread), 0);
	if (!_draftsSaveTimer.isActive()) {
		_draftsSaveTimer.callOnce(kSaveCloudDraftTimeout);
	}
}

void ApiWrap::updatePrivacyLastSeens() {
	const auto now = base::unixtime::now();
	if (!_session->premium()) {
		_session->data().enumerateUsers([&](not_null<UserData*> user) {
			if (user->isSelf()
				|| !user->isLoaded()
				|| user->lastseen().isHidden()) {
				return;
			}

			const auto till = user->lastseen().onlineTill();
			user->updateLastseen((till + 3 * 86400 >= now)
				? Data::LastseenStatus::Recently(true)
				: (till + 7 * 86400 >= now)
				? Data::LastseenStatus::WithinWeek(true)
				: (till + 30 * 86400 >= now)
				? Data::LastseenStatus::WithinMonth(true)
				: Data::LastseenStatus::LongAgo(true));
			session().changes().peerUpdated(
				user,
				Data::PeerUpdate::Flag::OnlineStatus);
			session().data().maybeStopWatchForOffline(user);
		});
	}

	if (_contactsStatusesRequestId) {
		request(_contactsStatusesRequestId).cancel();
	}
	_contactsStatusesRequestId = request(MTPcontacts_GetStatuses(
	)).done([=](const MTPVector<MTPContactStatus> &result) {
		_contactsStatusesRequestId = 0;
		for (const auto &status : result.v) {
			const auto &data = status.data();
			const auto userId = UserId(data.vuser_id());
			if (const auto user = _session->data().userLoaded(userId)) {
				const auto status = LastseenFromMTP(
					data.vstatus(),
					user->lastseen());
				if (user->updateLastseen(status)) {
					session().changes().peerUpdated(
						user,
						Data::PeerUpdate::Flag::OnlineStatus);
				}
			}
		}
	}).fail([this] {
		_contactsStatusesRequestId = 0;
	}).send();
}

void ApiWrap::clearHistory(not_null<PeerData*> peer, bool revoke) {
	deleteHistory(peer, true, revoke);
}

void ApiWrap::deleteConversation(not_null<PeerData*> peer, bool revoke) {
	if (const auto chat = peer->asChat()) {
		request(MTPmessages_DeleteChatUser(
			MTP_flags(0),
			chat->inputChat(),
			_session->user()->inputUser()
		)).done([=](const MTPUpdates &result) {
			applyUpdates(result);
			deleteHistory(peer, false, revoke);
		}).fail([=] {
			deleteHistory(peer, false, revoke);
		}).send();
	} else {
		deleteHistory(peer, false, revoke);
	}
}

void ApiWrap::deleteHistory(
		not_null<PeerData*> peer,
		bool justClear,
		bool revoke) {
	auto deleteTillId = MsgId(0);
	const auto history = _session->data().history(peer);
	if (justClear) {
		// In case of clear history we need to know the last server message.
		while (history->lastMessageKnown()) {
			const auto last = history->lastMessage();
			if (!last) {
				// History is empty.
				return;
			} else if (!last->isRegular()) {
				// Destroy client-side message locally.
				last->destroy();
			} else {
				break;
			}
		}
		if (!history->lastMessageKnown()) {
			history->owner().histories().requestDialogEntry(history, [=] {
				Expects(history->lastMessageKnown());

				deleteHistory(peer, justClear, revoke);
			});
			return;
		}
		deleteTillId = history->lastMessage()->id;
	}
	if (const auto channel = peer->asChannel()) {
		if (!justClear && !revoke) {
			channel->ptsSetWaitingForShortPoll(-1);
			leaveChannel(channel);
		} else {
			if (const auto migrated = peer->migrateFrom()) {
				deleteHistory(migrated, justClear, revoke);
			}
			if (deleteTillId || (!justClear && revoke)) {
				history->owner().histories().deleteAllMessages(
					history,
					deleteTillId,
					justClear,
					revoke);
			}
		}
	} else {
		history->owner().histories().deleteAllMessages(
			history,
			deleteTillId,
			justClear,
			revoke);
	}
	if (!justClear) {
		_session->data().deleteConversationLocally(peer);
	} else if (history) {
		history->clear(History::ClearType::ClearHistory);
	}
}

void ApiWrap::applyUpdates(
		const MTPUpdates &updates,
		uint64 sentMessageRandomId) const {
	this->updates().applyUpdates(updates, sentMessageRandomId);
}

int ApiWrap::applyAffectedHistory(
		PeerData *peer,
		const MTPmessages_AffectedHistory &result) const {
	const auto &data = result.c_messages_affectedHistory();
	if (const auto channel = peer ? peer->asChannel() : nullptr) {
		channel->ptsUpdateAndApply(data.vpts().v, data.vpts_count().v);
	} else {
		updates().updateAndApply(data.vpts().v, data.vpts_count().v);
	}
	return data.voffset().v;
}

void ApiWrap::applyAffectedMessages(
		not_null<PeerData*> peer,
		const MTPmessages_AffectedMessages &result) {
	const auto &data = result.c_messages_affectedMessages();
	if (const auto channel = peer->asChannel()) {
		channel->ptsUpdateAndApply(data.vpts().v, data.vpts_count().v);
	} else {
		applyAffectedMessages(result);
	}
}

void ApiWrap::applyAffectedMessages(
		const MTPmessages_AffectedMessages &result) const {
	const auto &data = result.c_messages_affectedMessages();
	updates().updateAndApply(data.vpts().v, data.vpts_count().v);
}

void ApiWrap::saveCurrentDraftToCloud() {
	Core::App().materializeLocalDrafts();
	for (const auto &controller : _session->windows()) {
		if (const auto thread = controller->activeChatCurrent().thread()) {
			const auto topic = thread->asTopic();
			if (topic && topic->creating()) {
				continue;
			}
			const auto history = thread->owningHistory();
			_session->local().writeDrafts(history);

			const auto topicRootId = thread->topicRootId();
			const auto monoforumPeerId = thread->monoforumPeerId();
			const auto localDraft = history->localDraft(
				topicRootId,
				monoforumPeerId);
			const auto cloudDraft = history->cloudDraft(
				topicRootId,
				monoforumPeerId);
			if (!Data::DraftsAreEqual(localDraft, cloudDraft)
				&& !_session->supportMode()) {
				saveDraftToCloudDelayed(thread);
			}
		}
	}
}

void ApiWrap::saveDraftsToCloud() {
	for (auto i = begin(_draftsSaveRequestIds); i != end(_draftsSaveRequestIds);) {
		const auto weak = i->first;
		const auto thread = weak.get();
		if (!thread) {
			i = _draftsSaveRequestIds.erase(i);
			continue;
		} else if (i->second) {
			++i;
			continue; // sent already
		}

		const auto history = thread->owningHistory();
		const auto topicRootId = thread->topicRootId();
		const auto monoforumPeerId = thread->monoforumPeerId();
		auto cloudDraft = history->cloudDraft(topicRootId, monoforumPeerId);
		auto localDraft = history->localDraft(topicRootId, monoforumPeerId);
		if (cloudDraft && cloudDraft->saveRequestId) {
			request(base::take(cloudDraft->saveRequestId)).cancel();
		}
		if (!_session->supportMode()) {
			cloudDraft = history->createCloudDraft(
				topicRootId,
				monoforumPeerId,
				localDraft);
		} else if (!cloudDraft) {
			cloudDraft = history->createCloudDraft(
				topicRootId,
				monoforumPeerId,
				nullptr);
		}

		auto flags = MTPmessages_SaveDraft::Flags(0);
		auto &textWithTags = cloudDraft->textWithTags;
		if (cloudDraft->webpage.removed) {
			flags |= MTPmessages_SaveDraft::Flag::f_no_webpage;
		} else if (!cloudDraft->webpage.url.isEmpty()) {
			flags |= MTPmessages_SaveDraft::Flag::f_media;
		}
		if (cloudDraft->reply.messageId
			|| cloudDraft->reply.topicRootId
			|| cloudDraft->reply.monoforumPeerId) {
			flags |= MTPmessages_SaveDraft::Flag::f_reply_to;
		}
		if (!textWithTags.tags.isEmpty()) {
			flags |= MTPmessages_SaveDraft::Flag::f_entities;
		}
		if (cloudDraft->suggest) {
			flags |= MTPmessages_SaveDraft::Flag::f_suggested_post;
		}
		auto entities = Api::EntitiesToMTP(
			_session,
			TextUtilities::ConvertTextTagsToEntities(textWithTags.tags),
			Api::ConvertOption::SkipLocal);

		history->startSavingCloudDraft(topicRootId, monoforumPeerId);
		cloudDraft->saveRequestId = request(MTPmessages_SaveDraft(
			MTP_flags(flags),
			ReplyToForMTP(history, cloudDraft->reply),
			history->peer->input(),
			MTP_string(textWithTags.text),
			entities,
			Data::WebPageForMTP(
				cloudDraft->webpage,
				textWithTags.text.isEmpty()),
			MTP_long(0), // effect
			Api::SuggestToMTP(cloudDraft->suggest)
		)).done([=](const MTPBool &result, const MTP::Response &response) {
			const auto requestId = response.requestId;
			history->finishSavingCloudDraft(
				topicRootId,
				monoforumPeerId,
				Api::UnixtimeFromMsgId(response.outerMsgId));
			const auto cloudDraft = history->cloudDraft(
				topicRootId,
				monoforumPeerId);
			if (cloudDraft) {
				if (cloudDraft->saveRequestId == requestId) {
					cloudDraft->saveRequestId = 0;
					history->draftSavedToCloud(topicRootId, monoforumPeerId);
				}
			}
			const auto i = _draftsSaveRequestIds.find(weak);
			if (i != _draftsSaveRequestIds.cend()
				&& i->second == requestId) {
				_draftsSaveRequestIds.erase(i);
				checkQuitPreventFinished();
			}
		}).fail([=](const MTP::Error &error, const MTP::Response &response) {
			const auto requestId = response.requestId;
			history->finishSavingCloudDraft(
				topicRootId,
				monoforumPeerId,
				Api::UnixtimeFromMsgId(response.outerMsgId));
			const auto cloudDraft = history->cloudDraft(
				topicRootId,
				monoforumPeerId);
			if (cloudDraft) {
				if (cloudDraft->saveRequestId == requestId) {
					history->clearCloudDraft(topicRootId, monoforumPeerId);
				}
			}
			const auto i = _draftsSaveRequestIds.find(weak);
			if (i != _draftsSaveRequestIds.cend()
				&& i->second == requestId) {
				_draftsSaveRequestIds.erase(i);
				checkQuitPreventFinished();
			}
		}).send();

		i->second = cloudDraft->saveRequestId;
		++i;
	}
}

bool ApiWrap::isQuitPrevent() {
	if (!_draftsSaveRequestIds.empty()) {
		LOG(("ApiWrap prevents quit, saving drafts..."));
		saveDraftsToCloud();
		return true;
	}
	const auto active = EnhancedForward::activeJobPeer();
	if (active.has_value() && !EnhancedForward::isPaused(*active)) {
		const auto session = _session;
		EnhancedForward::saveProgressForPeer(*active, session);
		return true;
	}
	return false;
}

void ApiWrap::checkQuitPreventFinished() {
	if (_draftsSaveRequestIds.empty()) {
		if (Core::Quitting()) {
			LOG(("ApiWrap doesn't prevent quit any more."));
		}
		Core::App().quitPreventFinished();
	}
}

void ApiWrap::registerModifyRequest(
		const QString &key,
		mtpRequestId requestId) {
	const auto i = _modifyRequests.find(key);
	if (i != end(_modifyRequests)) {
		request(i->second).cancel();
		i->second = requestId;
	} else {
		_modifyRequests.emplace(key, requestId);
	}
}

void ApiWrap::clearModifyRequest(const QString &key) {
	_modifyRequests.remove(key);
}

void ApiWrap::gotStickerSet(
		uint64 setId,
		const MTPmessages_StickerSet &result) {
	_stickerSetRequests.remove(setId);
	result.match([&](const MTPDmessages_stickerSet &data) {
		_session->data().stickers().feedSetFull(data);
	}, [](const MTPDmessages_stickerSetNotModified &) {
		LOG(("API Error: Unexpected messages.stickerSetNotModified."));
	});
}

void ApiWrap::requestWebPageDelayed(not_null<WebPageData*> page) {
	if (page->failed || !page->pendingTill) {
		return;
	}
	_webPagesPending.emplace(page, 0);
	auto left = (page->pendingTill - base::unixtime::now()) * 1000;
	if (!_webPagesTimer.isActive() || left <= _webPagesTimer.remainingTime()) {
		_webPagesTimer.callOnce((left < 0 ? 0 : left) + 1);
	}
}

void ApiWrap::clearWebPageRequest(not_null<WebPageData*> page) {
	_webPagesPending.remove(page);
	if (_webPagesPending.empty() && _webPagesTimer.isActive()) {
		_webPagesTimer.cancel();
	}
}

void ApiWrap::clearWebPageRequests() {
	_webPagesPending.clear();
	_webPagesTimer.cancel();
}

void ApiWrap::resolveWebPages() {
	auto ids = QVector<MTPInputMessage>(); // temp_req_id = -1
	using IndexAndMessageIds = QPair<int32, QVector<MTPInputMessage>>;
	using MessageIdsByChannel = base::flat_map<ChannelData*, IndexAndMessageIds>;
	MessageIdsByChannel idsByChannel; // temp_req_id = -index - 2

	ids.reserve(_webPagesPending.size());
	int32 t = base::unixtime::now(), m = INT_MAX;
	for (auto &[page, requestId] : _webPagesPending) {
		if (requestId > 0) {
			continue;
		}
		if (page->pendingTill <= t) {
			if (const auto item = _session->data().findWebPageItem(page)) {
				if (const auto channel = item->history()->peer->asChannel()) {
					auto channelMap = idsByChannel.find(channel);
					if (channelMap == idsByChannel.cend()) {
						channelMap = idsByChannel.emplace(
							channel,
							IndexAndMessageIds(
								idsByChannel.size(),
								QVector<MTPInputMessage>(
									1,
									MTP_inputMessageID(MTP_int(item->id))))).first;
					} else {
						channelMap->second.second.push_back(
							MTP_inputMessageID(MTP_int(item->id)));
					}
					requestId = -channelMap->second.first - 2;
				} else {
					ids.push_back(MTP_inputMessageID(MTP_int(item->id)));
					requestId = -1;
				}
			}
		} else {
			m = std::min(m, page->pendingTill - t);
		}
	}

	auto requestId = mtpRequestId(0);
	if (!ids.isEmpty()) {
		requestId = request(MTPmessages_GetMessages(
			MTP_vector<MTPInputMessage>(ids)
		)).done([=](
				const MTPmessages_Messages &result,
				mtpRequestId requestId) {
			gotWebPages(nullptr, result, requestId);
		}).afterDelay(kSmallDelayMs).send();
	}
	QVector<mtpRequestId> reqsByIndex(idsByChannel.size(), 0);
	for (auto i = idsByChannel.cbegin(), e = idsByChannel.cend(); i != e; ++i) {
		reqsByIndex[i->second.first] = request(MTPchannels_GetMessages(
			i->first->inputChannel(),
			MTP_vector<MTPInputMessage>(i->second.second)
		)).done([=, channel = i->first](
				const MTPmessages_Messages &result,
				mtpRequestId requestId) {
			gotWebPages(channel, result, requestId);
		}).afterDelay(kSmallDelayMs).send();
	}
	if (requestId || !reqsByIndex.isEmpty()) {
		for (auto &[page, pendingRequestId] : _webPagesPending) {
			if (pendingRequestId > 0) {
				continue;
			} else if (pendingRequestId < 0) {
				if (pendingRequestId == -1) {
					pendingRequestId = requestId;
				} else {
					pendingRequestId = reqsByIndex[-pendingRequestId - 2];
				}
			}
		}
	}
	if (m < INT_MAX) {
		_webPagesTimer.callOnce(std::min(m, 86400) * crl::time(1000));
	}
}

template <typename Request>
void ApiWrap::requestFileReference(
		Data::FileOrigin origin,
		FileReferencesHandler &&handler,
		Request &&data) {
	const auto i = _fileReferenceHandlers.find(origin);
	if (i != end(_fileReferenceHandlers)) {
		i->second.push_back(std::move(handler));
		return;
	}
	auto handlers = std::vector<FileReferencesHandler>();
	handlers.push_back(std::move(handler));
	_fileReferenceHandlers.emplace(origin, std::move(handlers));

	request(std::move(data)).done([=](const auto &result) {
		const auto parsed = Data::GetFileReferences(result);
		for (const auto &p : parsed.data) {
			// Unpack here the parsed pair by hand to workaround a GCC bug.
			// See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=87122
			const auto &origin = p.first;
			const auto &reference = p.second;
			const auto documentId = std::get_if<DocumentFileLocationId>(
				&origin);
			if (documentId) {
				_session->data().document(
					documentId->id
				)->refreshFileReference(reference);
			}
			const auto photoId = std::get_if<PhotoFileLocationId>(&origin);
			if (photoId) {
				_session->data().photo(
					photoId->id
				)->refreshFileReference(reference);
			}
		}
		const auto i = _fileReferenceHandlers.find(origin);
		Assert(i != end(_fileReferenceHandlers));
		auto handlers = std::move(i->second);
		_fileReferenceHandlers.erase(i);
		for (auto &handler : handlers) {
			handler(parsed);
		}
	}).fail([=] {
		const auto i = _fileReferenceHandlers.find(origin);
		Assert(i != end(_fileReferenceHandlers));
		auto handlers = std::move(i->second);
		_fileReferenceHandlers.erase(i);
		for (auto &handler : handlers) {
			handler(UpdatedFileReferences());
		}
	}).send();
}

void ApiWrap::refreshFileReference(
		Data::FileOrigin origin,
		not_null<Storage::DownloadMtprotoTask*> task,
		int requestId,
		const QByteArray &current) {
	return refreshFileReference(origin, crl::guard(task, [=](
			const UpdatedFileReferences &data) {
		task->refreshFileReferenceFrom(data, requestId, current);
	}));
}

void ApiWrap::refreshFileReference(
		Data::FileOrigin origin,
		FileReferencesHandler &&handler) {
	const auto fail = [&] {
		handler(UpdatedFileReferences());
	};
	const auto request = [&](
			auto &&data,
			Fn<void()> &&additional = nullptr) {
		requestFileReference(
			origin,
			std::move(handler),
			std::move(data));
		if (additional) {
			const auto i = _fileReferenceHandlers.find(origin);
			Assert(i != end(_fileReferenceHandlers));
			if (i->second.size() == 1) {
				i->second.push_back([=](auto&&) {
					additional();
				});
			}
		}
	};
	v::match(origin.data, [&](Data::FileOriginMessage data) {
		if (const auto item = _session->data().message(data)) {
			const auto media = item->media();
			const auto mediaStory = media ? media->storyId() : FullStoryId();
			const auto storyId = mediaStory
				? mediaStory
				: FullStoryId{
					(IsStoryMsgId(item->id)
						? item->history()->peer->id
						: PeerId()),
					(IsStoryMsgId(item->id)
						? StoryIdFromMsgId(item->id)
						: StoryId())
				};
			if (storyId) {
				request(MTPstories_GetStoriesByID(
					_session->data().peer(storyId.peer)->input(),
					MTP_vector<MTPint>(1, MTP_int(storyId.story))));
			} else if (item->isScheduled()) {
				const auto realId = _session->scheduledMessages().lookupId(
					item);
				request(MTPmessages_GetScheduledMessages(
					item->history()->peer->input(),
					MTP_vector<MTPint>(1, MTP_int(realId))));
			} else if (item->isSavedMusicItem()) {
				const auto user = item->history()->peer->asUser();
				const auto media = item->media();
				const auto document = media ? media->document() : nullptr;
				if (user && document) {
					request(MTPusers_GetSavedMusicByID(
						user->inputUser(),
						MTP_vector<MTPInputDocument>(1, document->mtpInput())));
				} else {
					fail();
				}
			} else if (item->isBusinessShortcut()) {
				const auto &shortcuts = _session->data().shortcutMessages();
				const auto realId = shortcuts.lookupId(item);
				request(MTPmessages_GetQuickReplyMessages(
					MTP_flags(MTPmessages_GetQuickReplyMessages::Flag::f_id),
					MTP_int(item->shortcutId()),
					MTP_vector<MTPint>(1, MTP_int(realId)),
					MTP_long(0)));
			} else if (const auto channel = item->history()->peer->asChannel()) {
				request(MTPchannels_GetMessages(
					channel->inputChannel(),
					MTP_vector<MTPInputMessage>(
						1,
						MTP_inputMessageID(MTP_int(item->id)))));
			} else {
				request(MTPmessages_GetMessages(
					MTP_vector<MTPInputMessage>(
						1,
						MTP_inputMessageID(MTP_int(item->id)))));
			}
		} else {
			fail();
		}
	}, [&](Data::FileOriginUserPhoto data) {
		if (const auto user = _session->data().user(data.userId)) {
			request(MTPphotos_GetUserPhotos(
				user->inputUser(),
				MTP_int(-1),
				MTP_long(data.photoId),
				MTP_int(1)));
		} else {
			fail();
		}
	}, [&](Data::FileOriginFullUser data) {
		if (const auto user = _session->data().user(data.userId)) {
			request(MTPusers_GetFullUser(user->inputUser()));
		} else {
			fail();
		}
	}, [&](Data::FileOriginPeerPhoto data) {
		const auto peer = _session->data().peer(data.peerId);
		if (const auto channel = peer->asChannel()) {
			request(MTPchannels_GetFullChannel(
				channel->inputChannel()));
		} else if (const auto chat = peer->asChat()) {
			request(MTPmessages_GetFullChat(chat->inputChat()));
		} else {
			fail();
		}
	}, [&](Data::FileOriginStickerSet data) {
		const auto isRecentAttached
			= (data.setId == Data::Stickers::CloudRecentAttachedSetId);
		if (data.setId == Data::Stickers::CloudRecentSetId
			|| data.setId == Data::Stickers::RecentSetId
			|| isRecentAttached) {
			auto done = [=] { crl::on_main(_session, [=] {
				if (isRecentAttached) {
					local().writeRecentMasks();
				} else {
					local().writeRecentStickers();
				}
			}); };
			request(MTPmessages_GetRecentStickers(
				MTP_flags(isRecentAttached
					? MTPmessages_GetRecentStickers::Flag::f_attached
					: MTPmessages_GetRecentStickers::Flags(0)),
				MTP_long(0)),
				std::move(done));
		} else if (data.setId == Data::Stickers::FavedSetId) {
			request(MTPmessages_GetFavedStickers(MTP_long(0)),
				[=] { crl::on_main(_session, [=] { local().writeFavedStickers(); }); });
		} else {
			request(MTPmessages_GetStickerSet(
				MTP_inputStickerSetID(
					MTP_long(data.setId),
					MTP_long(data.accessHash)),
				MTP_int(0)), // hash
				[=] { crl::on_main(_session, [=] {
					local().writeInstalledStickers();
					local().writeRecentStickers();
					local().writeFavedStickers();
				}); });
		}
	}, [&](Data::FileOriginSavedGifs data) {
		request(
			MTPmessages_GetSavedGifs(MTP_long(0)),
			[=] { crl::on_main(_session, [=] { local().writeSavedGifs(); }); });
	}, [&](Data::FileOriginWallpaper data) {
		const auto useSlug = data.ownerId
			&& (data.ownerId != session().userId())
			&& !data.slug.isEmpty();
		request(MTPaccount_GetWallPaper(useSlug
			? MTP_inputWallPaperSlug(MTP_string(data.slug))
			: MTP_inputWallPaper(
				MTP_long(data.paperId),
				MTP_long(data.accessHash))));
	}, [&](Data::FileOriginTheme data) {
		request(MTPaccount_GetTheme(
			MTP_string(Data::CloudThemes::Format()),
			MTP_inputTheme(
				MTP_long(data.themeId),
				MTP_long(data.accessHash))));
	}, [&](Data::FileOriginRingtones data) {
		request(MTPaccount_GetSavedRingtones(MTP_long(0)));
	}, [&](Data::FileOriginPremiumPreviews data) {
		request(MTPhelp_GetPremiumPromo());
	}, [&](Data::FileOriginWebPage data) {
		request(MTPmessages_GetWebPage(
			MTP_string(data.url),
			MTP_int(0)));
	}, [&](Data::FileOriginStory data) {
		request(MTPstories_GetStoriesByID(
			_session->data().peer(data.peer)->input(),
			MTP_vector<MTPint>(1, MTP_int(data.story))));
	}, [&](v::null_t) {
		fail();
	});
}

void ApiWrap::gotWebPages(
		ChannelData *channel,
		const MTPmessages_Messages &result,
		mtpRequestId req) {
	WebPageData::ApplyChanges(_session, channel, result);
	for (auto i = _webPagesPending.begin(); i != _webPagesPending.cend();) {
		if (i->second == req) {
			if (i->first->pendingTill > 0) {
				i->first->pendingTill = 0;
				i->first->failed = 1;
				_session->data().notifyWebPageUpdateDelayed(i->first);
			}
			i = _webPagesPending.erase(i);
		} else {
			++i;
		}
	}
	_session->data().sendWebPageGamePollTodoListNotifications();
}

void ApiWrap::updateStickers() {
	const auto now = crl::now();
	requestStickers(now);
	requestRecentStickers(now, false);
	requestFavedStickers(now);
	requestFeaturedStickers(now);
}

void ApiWrap::updateSavedGifs() {
	const auto now = crl::now();
	requestSavedGifs(now);
}

void ApiWrap::updateMasks() {
	const auto now = crl::now();
	requestMasks(now);
	requestRecentStickers(now, true);
}

void ApiWrap::updateCustomEmoji() {
	const auto now = crl::now();
	requestCustomEmoji(now);
	requestFeaturedEmoji(now);
}

void ApiWrap::requestSpecialStickersForce(
		bool faved,
		bool recent,
		bool attached) {
	if (faved) {
		requestFavedStickers(std::nullopt);
	} else if (recent || attached) {
		requestRecentStickers(std::nullopt, attached);
	}
}

void ApiWrap::setGroupStickerSet(
		not_null<ChannelData*> megagroup,
		const StickerSetIdentifier &set) {
	Expects(megagroup->mgInfo != nullptr);

	megagroup->mgInfo->stickerSet = set;
	request(MTPchannels_SetStickers(
		megagroup->inputChannel(),
		Data::InputStickerSet(set)
	)).send();
	_session->data().stickers().notifyUpdated(Data::StickersType::Stickers);
}

void ApiWrap::setGroupEmojiSet(
		not_null<ChannelData*> megagroup,
		const StickerSetIdentifier &set) {
	Expects(megagroup->mgInfo != nullptr);

	megagroup->mgInfo->emojiSet = set;
	request(MTPchannels_SetEmojiStickers(
		megagroup->inputChannel(),
		Data::InputStickerSet(set)
	)).send();
	_session->changes().peerUpdated(
		megagroup,
		Data::PeerUpdate::Flag::EmojiSet);
	_session->data().stickers().notifyUpdated(Data::StickersType::Emoji);
}

std::vector<not_null<DocumentData*>> *ApiWrap::stickersByEmoji(
		const QString &key) {
	const auto it = _stickersByEmoji.find(key);
	const auto sendRequest = [&] {
		if (it == _stickersByEmoji.end()) {
			return true;
		}
		const auto received = it->second.received;
		const auto now = crl::now();
		return (received > 0)
			&& (received + kStickersByEmojiInvalidateTimeout) <= now;
	}();
	if (sendRequest) {
		const auto hash = (it != _stickersByEmoji.end())
			? it->second.hash
			: uint64(0);
		request(MTPmessages_GetStickers(
			MTP_string(key),
			MTP_long(hash)
		)).done([=](const MTPmessages_Stickers &result) {
			if (result.type() == mtpc_messages_stickersNotModified) {
				return;
			}
			Assert(result.type() == mtpc_messages_stickers);
			const auto &data = result.c_messages_stickers();
			auto &entry = _stickersByEmoji[key];
			entry.list.clear();
			entry.list.reserve(data.vstickers().v.size());
			for (const auto &sticker : data.vstickers().v) {
				const auto document = _session->data().processDocument(
					sticker);
				if (document->sticker()) {
					entry.list.push_back(document);
				}
			}
			entry.hash = data.vhash().v;
			entry.received = crl::now();
			_session->data().stickers().notifyUpdated(
				Data::StickersType::Stickers);
		}).send();
	}
	if (it == _stickersByEmoji.end()) {
		_stickersByEmoji.emplace(key, StickersByEmoji());
	} else if (it->second.received > 0) {
		return &it->second.list;
	}
	return nullptr;
}

void ApiWrap::requestStickers(TimeId now) {
	if (!_session->data().stickers().updateNeeded(now)
		|| _stickersUpdateRequest) {
		return;
	}
	const auto done = [=](const MTPmessages_AllStickers &result) {
		_session->data().stickers().setLastUpdate(crl::now());
		_stickersUpdateRequest = 0;

		result.match([&](const MTPDmessages_allStickersNotModified&) {
		}, [&](const MTPDmessages_allStickers &data) {
			_session->data().stickers().setsReceived(
				data.vsets().v,
				data.vhash().v);
		});
	};
	_stickersUpdateRequest = request(MTPmessages_GetAllStickers(
		MTP_long(Api::CountStickersHash(_session, true))
	)).done(done).fail([=] {
		LOG(("App Fail: Failed to get stickers!"));
		done(MTP_messages_allStickersNotModified());
	}).send();
}

void ApiWrap::requestMasks(TimeId now) {
	if (!_session->data().stickers().masksUpdateNeeded(now)
		|| _masksUpdateRequest) {
		return;
	}
	const auto done = [=](const MTPmessages_AllStickers &result) {
		_session->data().stickers().setLastMasksUpdate(crl::now());
		_masksUpdateRequest = 0;

		result.match([&](const MTPDmessages_allStickersNotModified&) {
		}, [&](const MTPDmessages_allStickers &data) {
			_session->data().stickers().masksReceived(
				data.vsets().v,
				data.vhash().v);
		});
	};
	_masksUpdateRequest = request(MTPmessages_GetMaskStickers(
		MTP_long(Api::CountMasksHash(_session, true))
	)).done(done).fail([=] {
		LOG(("App Fail: Failed to get masks!"));
		done(MTP_messages_allStickersNotModified());
	}).send();
}

void ApiWrap::requestCustomEmoji(TimeId now) {
	if (!_session->data().stickers().emojiUpdateNeeded(now)
		|| _customEmojiUpdateRequest) {
		return;
	}
	const auto done = [=](const MTPmessages_AllStickers &result) {
		_session->data().stickers().setLastEmojiUpdate(crl::now());
		_customEmojiUpdateRequest = 0;

		result.match([&](const MTPDmessages_allStickersNotModified&) {
		}, [&](const MTPDmessages_allStickers &data) {
			_session->data().stickers().emojiReceived(
				data.vsets().v,
				data.vhash().v);
		});
	};
	_customEmojiUpdateRequest = request(MTPmessages_GetEmojiStickers(
		MTP_long(Api::CountCustomEmojiHash(_session, true))
	)).done(done).fail([=] {
		LOG(("App Fail: Failed to get custom emoji!"));
		done(MTP_messages_allStickersNotModified());
	}).send();
}

void ApiWrap::requestRecentStickers(
		std::optional<TimeId> now,
		bool attached) {
	const auto needed = !now
		? true
		: attached
		? _session->data().stickers().recentAttachedUpdateNeeded(*now)
		: _session->data().stickers().recentUpdateNeeded(*now);
	if (!needed) {
		return;
	}
	const auto requestId = [=]() -> mtpRequestId & {
		return attached
			? _recentAttachedStickersUpdateRequest
			: _recentStickersUpdateRequest;
	};
	if (requestId()) {
		return;
	}
	const auto finish = [=] {
		auto &stickers = _session->data().stickers();
		if (attached) {
			stickers.setLastRecentAttachedUpdate(crl::now());
		} else {
			stickers.setLastRecentUpdate(crl::now());
		}
		requestId() = 0;
	};
	const auto flags = attached
		? MTPmessages_getRecentStickers::Flag::f_attached
		: MTPmessages_getRecentStickers::Flags(0);
	requestId() = request(MTPmessages_GetRecentStickers(
		MTP_flags(flags),
		MTP_long(now ? Api::CountRecentStickersHash(_session, attached) : 0)
	)).done([=](const MTPmessages_RecentStickers &result) {
		finish();

		switch (result.type()) {
		case mtpc_messages_recentStickersNotModified: return;
		case mtpc_messages_recentStickers: {
			auto &d = result.c_messages_recentStickers();
			_session->data().stickers().specialSetReceived(
				attached
					? Data::Stickers::CloudRecentAttachedSetId
					: Data::Stickers::CloudRecentSetId,
				tr::lng_recent_stickers(tr::now),
				d.vstickers().v,
				d.vhash().v,
				d.vpacks().v,
				d.vdates().v);
		} return;
		default: Unexpected("Type in ApiWrap::recentStickersDone()");
		}
	}).fail([=] {
		finish();

		LOG(("App Fail: Failed to get recent stickers!"));
	}).send();
}

void ApiWrap::requestFavedStickers(std::optional<TimeId> now) {
	if (now) {
		if (!_session->data().stickers().favedUpdateNeeded(*now)
			|| _favedStickersUpdateRequest) {
			return;
		}
	}
	_favedStickersUpdateRequest = request(MTPmessages_GetFavedStickers(
		MTP_long(now ? Api::CountFavedStickersHash(_session) : 0)
	)).done([=](const MTPmessages_FavedStickers &result) {
		_session->data().stickers().setLastFavedUpdate(crl::now());
		_favedStickersUpdateRequest = 0;

		switch (result.type()) {
		case mtpc_messages_favedStickersNotModified: return;
		case mtpc_messages_favedStickers: {
			auto &d = result.c_messages_favedStickers();
			_session->data().stickers().specialSetReceived(
				Data::Stickers::FavedSetId,
				Lang::Hard::FavedSetTitle(),
				d.vstickers().v,
				d.vhash().v,
				d.vpacks().v);
		} return;
		default: Unexpected("Type in ApiWrap::favedStickersDone()");
		}
	}).fail([=] {
		_session->data().stickers().setLastFavedUpdate(crl::now());
		_favedStickersUpdateRequest = 0;

		LOG(("App Fail: Failed to get faved stickers!"));
	}).send();
}

void ApiWrap::requestFeaturedStickers(TimeId now) {
	if (!_session->data().stickers().featuredUpdateNeeded(now)
		|| _featuredStickersUpdateRequest) {
		return;
	}
	_featuredStickersUpdateRequest = request(MTPmessages_GetFeaturedStickers(
		MTP_long(Api::CountFeaturedStickersHash(_session))
	)).done([=](const MTPmessages_FeaturedStickers &result) {
		_featuredStickersUpdateRequest = 0;
		_session->data().stickers().featuredSetsReceived(result);
	}).fail([=] {
		_featuredStickersUpdateRequest = 0;
		_session->data().stickers().setLastFeaturedUpdate(crl::now());
		LOG(("App Fail: Failed to get featured stickers!"));
	}).send();
}

void ApiWrap::requestFeaturedEmoji(TimeId now) {
	if (!_session->data().stickers().featuredEmojiUpdateNeeded(now)
		|| _featuredEmojiUpdateRequest) {
		return;
	}
	_featuredEmojiUpdateRequest = request(
		MTPmessages_GetFeaturedEmojiStickers(
			MTP_long(Api::CountFeaturedStickersHash(_session)))
	).done([=](const MTPmessages_FeaturedStickers &result) {
		_featuredEmojiUpdateRequest = 0;
		_session->data().stickers().featuredEmojiSetsReceived(result);
	}).fail([=] {
		_featuredEmojiUpdateRequest = 0;
		_session->data().stickers().setLastFeaturedEmojiUpdate(crl::now());
		LOG(("App Fail: Failed to get featured emoji!"));
	}).send();
}

void ApiWrap::requestSavedGifs(TimeId now) {
	if (!_session->data().stickers().savedGifsUpdateNeeded(now)
		|| _savedGifsUpdateRequest) {
		return;
	}
	_savedGifsUpdateRequest = request(MTPmessages_GetSavedGifs(
		MTP_long(Api::CountSavedGifsHash(_session))
	)).done([=](const MTPmessages_SavedGifs &result) {
		_session->data().stickers().setLastSavedGifsUpdate(crl::now());
		_savedGifsUpdateRequest = 0;

		switch (result.type()) {
		case mtpc_messages_savedGifsNotModified: return;
		case mtpc_messages_savedGifs: {
			auto &d = result.c_messages_savedGifs();
			_session->data().stickers().gifsReceived(
				d.vgifs().v,
				d.vhash().v);
		} return;
		default: Unexpected("Type in ApiWrap::savedGifsDone()");
		}
	}).fail([=] {
		_session->data().stickers().setLastSavedGifsUpdate(crl::now());
		_savedGifsUpdateRequest = 0;

		LOG(("App Fail: Failed to get saved gifs!"));
	}).send();
}

void ApiWrap::readFeaturedSetDelayed(uint64 setId) {
	if (!_featuredSetsRead.contains(setId)) {
		_featuredSetsRead.insert(setId);
		_featuredSetsReadTimer.callOnce(kReadFeaturedSetsTimeout);
	}
}

void ApiWrap::readFeaturedSets() {
	const auto &sets = _session->data().stickers().sets();
	auto count = _session->data().stickers().featuredSetsUnreadCount();
	QVector<MTPlong> wrappedIds;
	wrappedIds.reserve(_featuredSetsRead.size());
	for (const auto setId : _featuredSetsRead) {
		const auto it = sets.find(setId);
		if (it != sets.cend()) {
			it->second->flags &= ~Data::StickersSetFlag::Unread;
			wrappedIds.append(MTP_long(setId));
			if (count) {
				--count;
			}
		}
	}
	_featuredSetsRead.clear();

	if (!wrappedIds.empty()) {
		auto requestData = MTPmessages_ReadFeaturedStickers(
			MTP_vector<MTPlong>(wrappedIds));
		request(std::move(requestData)).done([=] {
			local().writeFeaturedStickers();
			_session->data().stickers().notifyUpdated(
				Data::StickersType::Stickers);
		}).send();

		_session->data().stickers().setFeaturedSetsUnreadCount(count);
	}
}

void ApiWrap::resolveJumpToDate(
		Dialogs::Key chat,
		const QDate &date,
		Fn<void(not_null<PeerData*>, MsgId)> callback) {
	if (const auto peer = chat.peer()) {
		const auto topic = chat.topic();
		const auto sublist = chat.sublist();
		const auto rootId = topic ? topic->rootId() : MsgId();
		const auto monoforumPeerId = sublist
			? sublist->sublistPeer()->id
			: PeerId();
		resolveJumpToHistoryDate(
			peer,
			rootId,
			monoforumPeerId,
			date,
			std::move(callback));
	}
}

template <typename Callback>
void ApiWrap::requestMessageAfterDate(
	not_null<PeerData*> peer,
	MsgId topicRootId,
	PeerId monoforumPeerId,
	const QDate &date,
	Callback &&callback) {
	// API returns a message with date <= offset_date.
	// So we request a message with offset_date = desired_date - 1 and add_offset = -1.
	// This should give us the first message with date >= desired_date.
	const auto offsetId = 0;
	const auto offsetDate = static_cast<int>(date.startOfDay().toSecsSinceEpoch()) - 1;
	const auto addOffset = -1;
	const auto limit = 1;
	const auto maxId = 0;
	const auto minId = 0;
	const auto historyHash = uint64(0);

	auto send = [&](auto &&serialized) {
		request(std::move(serialized)).done([
			=,
			callback = std::forward<Callback>(callback)
		](const MTPmessages_Messages &result) {
			const auto handleMessages = [&](auto &messages) {
				_session->data().processUsers(messages.vusers());
				_session->data().processChats(messages.vchats());
				return &messages.vmessages().v;
			};
			const auto list = result.match([&](
					const MTPDmessages_messages &data) {
				peer->processTopics(data.vtopics());
				return handleMessages(data);
			}, [&](const MTPDmessages_messagesSlice &data) {
				peer->processTopics(data.vtopics());
				return handleMessages(data);
			}, [&](const MTPDmessages_channelMessages &data) {
				if (const auto channel = peer->asChannel()) {
					channel->ptsReceived(data.vpts().v);
				} else {
					LOG(("API Error: received messages.channelMessages when "
						"no channel was passed! (ApiWrap::jumpToDate)"));
				}
				peer->processTopics(data.vtopics());
				return handleMessages(data);
			}, [&](const MTPDmessages_messagesNotModified &) {
				LOG(("API Error: received messages.messagesNotModified! "
					"(ApiWrap::jumpToDate)"));
				return (const QVector<MTPMessage>*)nullptr;
			});
			if (list) {
				_session->data().processMessages(
					*list,
					NewMessageType::Existing);
				for (const auto &message : *list) {
					if (DateFromMessage(message) >= offsetDate) {
						callback(IdFromMessage(message));
						return;
					}
				}
			}
			callback(ShowAtUnreadMsgId);
		}).send();
	};
	if (topicRootId) {
		send(MTPmessages_GetReplies(
			peer->input(),
			MTP_int(topicRootId),
			MTP_int(offsetId),
			MTP_int(offsetDate),
			MTP_int(addOffset),
			MTP_int(limit),
			MTP_int(maxId),
			MTP_int(minId),
			MTP_long(historyHash)));
	} else if (monoforumPeerId) {
		send(MTPmessages_GetSavedHistory(
			MTP_flags(MTPmessages_GetSavedHistory::Flag::f_parent_peer),
			peer->input(),
			session().data().peer(monoforumPeerId)->input(),
			MTP_int(offsetId),
			MTP_int(offsetDate),
			MTP_int(addOffset),
			MTP_int(limit),
			MTP_int(maxId),
			MTP_int(minId),
			MTP_long(historyHash)));
	} else {
		send(MTPmessages_GetHistory(
			peer->input(),
			MTP_int(offsetId),
			MTP_int(offsetDate),
			MTP_int(addOffset),
			MTP_int(limit),
			MTP_int(maxId),
			MTP_int(minId),
			MTP_long(historyHash)));
	}
}

void ApiWrap::resolveJumpToHistoryDate(
		not_null<PeerData*> peer,
		MsgId topicRootId,
		PeerId monoforumPeerId,
		const QDate &date,
		Fn<void(not_null<PeerData*>, MsgId)> callback) {
	if (const auto channel = peer->migrateTo()) {
		return resolveJumpToHistoryDate(
			channel,
			topicRootId,
			monoforumPeerId,
			date,
			std::move(callback));
	}
	const auto jumpToDateInPeer = [=] {
		requestMessageAfterDate(
			peer,
			topicRootId,
			monoforumPeerId,
			date,
			[=](MsgId itemId) { callback(peer, itemId); });
	};
	const auto migrated = (topicRootId || monoforumPeerId)
		? nullptr
		: peer->migrateFrom();
	if (migrated) {
		requestMessageAfterDate(
			migrated,
			MsgId(),
			PeerId(),
			date,
			[=](MsgId itemId) {
				if (itemId) {
					callback(migrated, itemId);
				} else {
					jumpToDateInPeer();
				}
			});
	} else {
		jumpToDateInPeer();
	}
}

void ApiWrap::requestHistory(
		not_null<History*> history,
		MsgId messageId,
		SliceType slice) {
	const auto peer = history->peer;
	const auto key = HistoryRequest{
		peer,
		messageId,
		slice,
	};
	if (_historyRequests.contains(key)) {
		return;
	}

	const auto prepared = Api::PrepareHistoryRequest(peer, messageId, slice);
	auto &histories = history->owner().histories();
	const auto requestType = Data::Histories::RequestType::History;
	histories.sendRequest(history, requestType, [=](Fn<void()> finish) {
		return request(
			std::move(prepared)
		).done([=](const Api::HistoryRequestResult &result) {
			_historyRequests.remove(key);
			auto parsed = Api::ParseHistoryResult(
				peer,
				messageId,
				slice,
				result);
			history->messages().addSlice(
				std::move(parsed.messageIds),
				parsed.noSkipRange,
				parsed.fullCount);
			finish();
		}).fail([=] {
			_historyRequests.remove(key);
			finish();
		}).send();
	});
	_historyRequests.emplace(key);
}

void ApiWrap::requestSharedMedia(
		not_null<PeerData*> peer,
		MsgId topicRootId,
		PeerId monoforumPeerId,
		SharedMediaType type,
		MsgId messageId,
		SliceType slice) {
	const auto key = SharedMediaRequest{
		peer,
		topicRootId,
		monoforumPeerId,
		type,
		messageId,
		slice,
	};
	if (_sharedMediaRequests.contains(key)) {
		return;
	}

	const auto prepared = Api::PrepareSearchRequest(
		peer,
		topicRootId,
		monoforumPeerId,
		type,
		QString(),
		messageId,
		slice);
	if (!prepared) {
		return;
	}

	const auto history = _session->data().history(peer);
	auto &histories = history->owner().histories();
	const auto requestType = Data::Histories::RequestType::History;
	const auto takeout = (_takeoutId && _takeoutPeerId == peer->id)
		? _takeoutId : std::nullopt;
	histories.sendRequest(history, requestType, [=](Fn<void()> finish) {
		const auto sharedDone = [=](const Api::SearchRequestResult &result) {
			_sharedMediaRequests.remove(key);
			auto parsed = Api::ParseSearchResult(
				peer,
				type,
				messageId,
				slice,
				result);
			sharedMediaDone(
				peer,
				topicRootId,
				monoforumPeerId,
				type,
				std::move(parsed));
			finish();
		};
		const auto sharedFail = [=] {
			_sharedMediaRequests.remove(key);
			finish();
		};
		if (takeout) {
			return this->request(
				MTPInvokeWithTakeout<MTPmessages_Search>(
					MTP_long(*takeout),
					std::move(*prepared))
			).done(sharedDone).fail(sharedFail)
			.toDC(MTP::ShiftDcId(0, MTP::kExportDcShift)).send();
		} else {
			return this->request(std::move(*prepared)
			).done(sharedDone).fail(sharedFail).send();
		}
	});
	_sharedMediaRequests.emplace(key);
}

void ApiWrap::setTakeoutId(std::optional<uint64> id) {
	_takeoutId = id;
}
std::optional<uint64> ApiWrap::takeoutId() const {
	return _takeoutId;
}
void ApiWrap::setTakeoutBypass(bool bypass) {
	_takeoutBypass = bypass;
}
bool ApiWrap::takeoutBypass() const {
	return _takeoutBypass;
}
void ApiWrap::setTakeoutPeerId(PeerId peerId) {
	_takeoutPeerId = peerId;
}
PeerId ApiWrap::takeoutPeerId() const {
	return _takeoutPeerId;
}

void ApiWrap::sharedMediaDone(
		not_null<PeerData*> peer,
		MsgId topicRootId,
		PeerId monoforumPeerId,
		SharedMediaType type,
		Api::SearchResult &&parsed) {
	const auto topic = peer->forumTopicFor(topicRootId);
	const auto sublist = peer->monoforumSublistFor(monoforumPeerId);
	if ((topicRootId && !topic) || (monoforumPeerId && !sublist)) {
		return;
	}
	const auto hasMessages = !parsed.messageIds.empty();
	_session->storage().add(Storage::SharedMediaAddSlice(
		peer->id,
		topicRootId,
		monoforumPeerId,
		type,
		std::move(parsed.messageIds),
		parsed.noSkipRange,
		parsed.fullCount
	));
	if (type == SharedMediaType::Pinned && hasMessages) {
		peer->owner().history(peer)->setHasPinnedMessages(true);
		if (topic) {
			topic->setHasPinnedMessages(true);
		}
		if (sublist) {
			sublist->setHasPinnedMessages(true);
		}
	}
}

mtpRequestId ApiWrap::requestGlobalMedia(
		Storage::SharedMediaType type,
		const QString &query,
		int32 offsetRate,
		Data::MessagePosition offsetPosition,
		Fn<void(Api::GlobalMediaResult)> done) {
	auto prepared = Api::PrepareGlobalMediaRequest(
		_session,
		offsetRate,
		offsetPosition,
		type,
		query);
	if (!prepared) {
		done({});
		return 0;
	}
	return request(
		std::move(*prepared)
	).done([=](const Api::SearchRequestResult &result) {
		done(Api::ParseGlobalMediaResult(_session, result));
	}).fail([=] {
		done({});
	}).send();
}

void ApiWrap::sendAction(const SendAction &action) {
	if (!action.options.scheduled
		&& !action.options.shortcutId
		&& !action.replaceMediaOf) {
		const auto topicRootId = action.replyTo.topicRootId;
		const auto topic = topicRootId
			? action.history->peer->forumTopicFor(topicRootId)
			: nullptr;
		const auto monoforumPeerId = action.replyTo.monoforumPeerId;
		const auto sublist = monoforumPeerId
			? action.history->peer->monoforumSublistFor(monoforumPeerId)
			: nullptr;
		if (topic) {
			topic->readTillEnd();
		} else if (sublist) {
			sublist->readTillEnd();
		} else {
			_session->data().histories().readInbox(action.history);
		}
		action.history->getReadyFor(ShowAtTheEndMsgId);
	}
	_sendActions.fire_copy(action);
}

void ApiWrap::finishForwarding(const SendAction &action) {
	const auto history = action.history;
	const auto topicRootId = action.replyTo.topicRootId;
	const auto monoforumPeerId = action.replyTo.monoforumPeerId;
	auto toForward = history->resolveForwardDraft(
		topicRootId,
		monoforumPeerId);
	if (!toForward.items.empty()) {
		const auto error = GetErrorForSending(
			history->peer,
			{
				.topicRootId = topicRootId,
				.forward = &toForward.items,
			});
		if (error) {
			return;
		}

		history->setForwardDraft(topicRootId, monoforumPeerId, {});
		forwardMessages(std::move(toForward), action);
	}

	_session->data().sendHistoryChangeNotifications();
	if (!action.options.shortcutId) {
		_session->changes().historyUpdated(
			history,
			(action.options.scheduled
				? Data::HistoryUpdate::Flag::ScheduledSent
				: Data::HistoryUpdate::Flag::MessageSent));
	}
}

void ApiWrap::sendForwardWithRegrouping(
		Data::ResolvedForwardDraft &&draft,
		SendAction &&action,
		FnMut<void()> &&successCallback,
		bool regroupAll,
		bool separate) {
	
	Expects(!draft.items.empty());
	Expects(regroupAll || separate);
	
	if (draft.options == Data::ForwardOptions::Quoted) {
		draft.options = Data::ForwardOptions::UnquotedWithCaptions;
	}
	
	const auto peer = action.history->peer;
	const auto session = &peer->session();
	const auto history = action.history;
	const auto sendAs = action.options.sendAs;
	
	if (regroupAll) {
		// Send ALL items as a single album
		// Collect all media items
		auto mediaList = std::vector<MTPInputMedia>();
		auto captions = std::vector<TextWithEntities>();
		
		for (const auto &item : draft.items) {
			const auto media = item->media();
			if (!media) {
				// Text item - skip or handle separately
				continue;
			}
			
			// Get MTPInputMedia from existing media
			const auto inputMedia = [&]() -> MTPInputMedia {
				if (const auto photo = media->photo()) {
					return MTP_inputMediaPhoto(
						MTP_flags(0),
						photo->mtpInput(),
						MTPint(), // ttl_seconds
						MTPInputDocument()); // video
				} else if (const auto document = media->document()) {
					return MTP_inputMediaDocument(
						MTP_flags(0),
						document->mtpInput(),
						MTPInputPhoto(), // video_cover
						MTPint(), // ttl_seconds
						MTPint(), // video_timestamp
						MTPstring()); // query
				}
				return MTP_inputMediaEmpty();
			}();
			
			if (inputMedia.type() != mtpc_inputMediaEmpty) {
				mediaList.push_back(inputMedia);
				
				// Get caption (drop if UnquotedWithoutCaptions)
				if (draft.options != Data::ForwardOptions::UnquotedWithoutCaptions) {
					captions.push_back(item->originalText());
				} else {
					captions.push_back(TextWithEntities());
				}
			}
		}
		
		if (mediaList.empty()) {
			if (successCallback) successCallback();
			return;
		}
		
		// Send as single album using messages.sendMultiMedia
		// Each item keeps its own caption (unless UnquotedWithoutCaptions mode)
		
		using Flag = MTPmessages_SendMultiMedia::Flag;
		const auto flags = Flag(0)
			| (ShouldSendSilent(peer, action.options) ? Flag::f_silent : Flag(0))
			| (action.options.scheduled ? Flag::f_schedule_date : Flag(0))
			| (sendAs ? Flag::f_send_as : Flag(0))
			| (action.options.shortcutId ? Flag::f_quick_reply_shortcut : Flag(0))
			| (action.options.effectId ? Flag::f_effect : Flag(0))
			| (action.options.invertCaption ? Flag::f_invert_media : Flag(0));
		
		auto multiMedia = QVector<MTPInputSingleMedia>();
		multiMedia.reserve(mediaList.size());
		
		for (size_t i = 0; i < mediaList.size(); i++) {
			const auto randomId = base::RandomValue<uint64>();
			session->data().registerMessageRandomId(randomId, FullMsgId());
			
			// Each item gets its own caption (or empty if UnquotedWithoutCaptions)
			const auto &caption = (i < captions.size()) ? captions[i] : TextWithEntities();
			
			// Convert caption entities to MTP
			auto sentEntities = Api::EntitiesToMTP(
				session,
				caption.entities,
				Api::ConvertOption::SkipLocal);
			
			const auto entitiesFlags = !sentEntities.v.isEmpty()
				? MTPDinputSingleMedia::Flag::f_entities
				: MTPDinputSingleMedia::Flag(0);
			
			auto singleMedia = MTP_inputSingleMedia(
				MTP_flags(entitiesFlags),
				mediaList[i],
				MTP_long(randomId),
				MTP_string(caption.text),
				sentEntities);
			
			multiMedia.push_back(singleMedia);
		}
		
		auto callbackPtr = std::make_shared<FnMut<void()>>(std::move(successCallback));
		auto &histories = history->owner().histories();
		
		histories.sendPreparedMessage(
			history,
			FullReplyTo(),
			uint64(0),
			Data::Histories::PrepareMessage<MTPmessages_SendMultiMedia>(
				MTP_flags(flags),
				peer->input(),
				Data::Histories::ReplyToPlaceholder(),
				MTP_vector<MTPInputSingleMedia>(std::move(multiMedia)),
				MTP_int(action.options.scheduled),
				(sendAs ? sendAs->input() : MTP_inputPeerEmpty()),
				Data::ShortcutIdToMTP(session, action.options.shortcutId),
				MTP_long(action.options.effectId),
				MTP_long(0) // stars_paid
			), [callbackPtr](const MTPUpdates &result, const MTP::Response &response) {
				if (*callbackPtr) {
					(*callbackPtr)();
				}
			}, [callbackPtr](const MTP::Error &error, const MTP::Response &response) {
				if (*callbackPtr) {
					(*callbackPtr)();
				}
			});
} else if (separate) {
		// Send each item as a separate message
		const auto remaining = std::make_shared<int>(int(draft.items.size()));
		auto callbackPtr = std::make_shared<FnMut<void()>>(std::move(successCallback));
		auto checkComplete = std::make_shared<std::function<void()>>(
			[callbackPtr, remaining]() {
				if (--*remaining == 0 && *callbackPtr) {
					(*callbackPtr)();
				}
			});
		
		for (const auto &item : draft.items) {
			auto singleDraft = Data::ResolvedForwardDraft{
				.items = {item},
				.options = draft.options,
				.groupOptions = Data::GroupingOptions::GroupAsIs,
			};
			
			forwardMessages(
				std::move(singleDraft),
				action,
				[checkComplete] { (*checkComplete)(); });
		}
	}
}

void ApiWrap::forwardMessages(
		Data::ResolvedForwardDraft &&draft,
		SendAction action,
		FnMut<void()> &&successCallback,
		std::shared_ptr<EnhancedForward::SavedJob> resumeJob) {
	Expects(!draft.items.empty() || resumeJob);

	LOG(("ENHANCED_FWD: forwardMessages items=%1").arg(draft.items.size()));

	auto enhancedItems = std::vector<not_null<HistoryItem*>>();
	auto normalItems = decltype(draft.items)();
	enhancedItems.reserve(draft.items.size());
	normalItems.reserve(draft.items.size());
	for (const auto &item : draft.items) {
		if (EnhancedForward::checkItem(item).restricted) {
			enhancedItems.push_back(item);
		} else {
			normalItems.push_back(item);
		}
	}
	// draft.items are in selection order, not chronological. Sort both
	// lists by message id ascending so the pipeline (downloads, uploads
	// and sends) and the standard forward proceed oldest -> newest.
	const auto byId = [](const not_null<HistoryItem*> &a,
			const not_null<HistoryItem*> &b) {
		return a->id < b->id;
	};
	std::sort(enhancedItems.begin(), enhancedItems.end(), byId);
	std::sort(normalItems.begin(), normalItems.end(), byId);
	const auto enhancedNeeded = !enhancedItems.empty();
	LOG(("ENHANCED_FWD: enhancedNeeded=%1 enhanced=%2 normal=%3")
		.arg(Logs::b(enhancedNeeded))
		.arg(enhancedItems.size())
		.arg(normalItems.size()));

	struct SharedCallback {
		int requestsLeft = 0;
		FnMut<void()> callback;
	};
	const auto shared = successCallback
		? std::make_shared<SharedCallback>()
		: std::shared_ptr<SharedCallback>();
	if (successCallback) {
		shared->callback = std::move(successCallback);
	}

	if (enhancedNeeded) {
		const auto peerId = action.history->peer->id;
		LOG(("ENHANCED_FWD: enhanced peer=%1").arg(peerId.value));
		const auto downloadPath = File::DefaultDownloadPath(&session())
			+ "ForwardTemp/";
		QDir().mkpath(downloadPath);

		if (!enhancedItems.empty()) {
			const auto n = int(enhancedItems.size());
			LOG(("ENHANCED_FWD: creating Ctx"));
			struct Ctx {
				std::vector<FullMsgId> itemIds;
				// per-item state
				std::vector<bool> textOnly;     // no media, send text directly
				std::vector<bool> isPhoto;
				std::vector<bool> downloadDone; // download finished (or skipped)
				std::vector<bool> needsDownload; // needs a network download
				std::vector<bool> downloadStarted; // download kicked off
				std::vector<QString> paths;     // local file path after download
				std::vector<qint64> downloadedBytes; // bytes downloaded so far
				std::shared_ptr<base::flat_map<FullMsgId, int>> uploadIndex;
				std::vector<bool> uploadDone;   // upload to TG servers done
				std::vector<bool> uploadStarted; // upload to TG servers started
				std::vector<Api::RemoteFileInfo> uploadInfos;
				std::vector<std::shared_ptr<FilePrepareResult>> prepared;
				std::vector<FullMsgId> uploadIds; // FullMsgId used for upload tracking
				std::vector<uint64> uploadFileId; // client-chosen upload file_id
				std::vector<qint64> uploadPartSize; // bytes per part
				std::vector<int> uploadedParts; // parts acked by server
				std::vector<int> uploadRetries; // failed-upload retry count
				std::vector<int> lastSavedPct; // last saved % for periodic save
				// keep the photo media view alive so the loaded Large bytes
				// are not recycled by the chat view while we save them
				std::vector<std::shared_ptr<Data::PhotoMedia>> photoViews;
				// source groupId for album items (empty if not in album)
				std::vector<MessageGroupId> sourceGroup;
				// album tracking (keyed by SOURCE groupId -> SendingAlbum)
				base::flat_map<
					MessageGroupId,
					std::shared_ptr<SendingAlbum>> albums;
				// ordered sender state
				int current = 0; // next index to send
				// Limit concurrent UploadMedia+sendMedia so each finished
				// upload is sent promptly instead of queueing behind a
				// single in-flight request (which made everything appear
				// at the very end).
				int uploadMediaInFlight = 0;
				// One download and one upload at a time; the download of
				// the next item overlaps the upload of the previous one.
				// Both advance through the (oldest-first) index in order.
				bool downloadInFlight = false;
				bool uploadInFlight = false;
				std::vector<int> uploadQueue;
				// upload listeners lifetime (kept alive until all done)
				std::shared_ptr<rpl::lifetime> uploadLifetime;
				// download listeners lifetime (kept alive until all done)
				std::shared_ptr<rpl::lifetime> dlLifetime;
				QString downloadPath;
				QString progressPath;
				PeerId peerId;
				PeerId srcPeer;
				int sent = 0;
				std::shared_ptr<std::function<void()>> sendNextFn;
				std::function<void()> startNextDownloadFn;
				int downloadCursor = 0;
				int uploadCursor = 0;
				int pendingSend = -1;
			};
			const auto ctx = std::make_shared<Ctx>();
			ctx->itemIds.resize(n);
			ctx->textOnly.resize(n, false);
			ctx->isPhoto.resize(n, false);
			ctx->downloadDone.resize(n, false);
			ctx->needsDownload.resize(n, false);
			ctx->downloadStarted.resize(n, false);
			ctx->paths.resize(n);
			ctx->downloadedBytes.resize(n, 0);
			ctx->uploadDone.resize(n, false);
			ctx->uploadStarted.resize(n, false);
			ctx->uploadInfos.resize(n);
			ctx->prepared.resize(n);
			ctx->uploadIds.resize(n);
			ctx->uploadFileId.resize(n, 0);
			ctx->uploadPartSize.resize(n, 0);
			ctx->uploadedParts.resize(n, 0);
			ctx->uploadRetries.resize(n, 0);
			ctx->lastSavedPct.resize(n, -10);
			ctx->photoViews.resize(n);
			ctx->sourceGroup.resize(n);
			ctx->downloadPath = downloadPath;
			QDir().mkpath(downloadPath);
			ctx->peerId = peerId;
			ctx->srcPeer = !enhancedItems.empty()
				? enhancedItems.front()->history()->peer->id
				: peerId;
			for (auto i = 0; i < n; i++) {
				ctx->itemIds[i] = enhancedItems[i]->fullId();
			}

			const auto saveProgress = [=] {
				QJsonObject root;
				root["dst_peer"] = qint64(ctx->peerId.value);
				root["src_peer"] = qint64(ctx->srcPeer.value);
				root["total"] = int(ctx->itemIds.size());
				root["sent"] = int(ctx->sent);
				QJsonArray msgs;
				for (const auto &full : ctx->itemIds) {
					QJsonObject m;
					m["msg"] = qint64(full.msg.bare);
					msgs.append(m);
				}
				root["source_msgs"] = msgs;
				QJsonArray items;
				for (auto i = 0; i < int(ctx->itemIds.size()); i++) {
					QJsonObject item;
					item["index"] = i;
					item["path"] = ctx->paths[i];
					item["downloaded"] = ctx->downloadedBytes[i];
					item["download_done"] = bool(ctx->downloadDone[i]);
					item["upload_done"] = bool(ctx->uploadDone[i]);
					item["text_only"] = bool(ctx->textOnly[i]);
					item["file_id"] = QString::number(ctx->uploadFileId[i]);
					item["part_size"] = qint64(ctx->uploadPartSize[i]);
					item["uploaded_parts"] = int(ctx->uploadedParts[i]);
					const auto srcItem = session().data().message(ctx->itemIds[i]);
					if (srcItem) {
						const auto media = srcItem->media();
						if (const auto doc = media ? media->document() : nullptr) {
							item["name"] = doc->filename();
							item["size"] = qint64(doc->size);
						} else if (media && media->photo()) {
							item["name"] = u"photo.jpg"_q;
							item["size"] = qint64(0);
						} else {
							item["name"] = QFileInfo(ctx->paths[i]).fileName();
							item["size"] = qint64(0);
						}
					}
					items.append(item);
				}
				root["items"] = items;
				const auto srcPeer = session().data().peer(ctx->srcPeer);
				const auto srcName = srcPeer
					? srcPeer->name()
					: u"chat"_q;
				root["src_name"] = srcName;
				const auto path = EnhancedForward::ProgressFilePath(
					EnhancedForward::ProgressFileBareName(srcName),
					ctx->downloadPath);
				ctx->progressPath = path;
				EnhancedForward::SaveProgress(path, root);
			};

			LOG(("ENHANCED_FWD: startForwardSession n=%1").arg(n));
			EnhancedForward::startForwardSession(
				&session(),
				peerId,
				n,
				saveProgress);
			LOG(("ENHANCED_FWD: startForwardSession done"));

			// Resume: rebuild the in-flight state from the saved job so
			// the pipeline continues where it left off. Items whose
			// upload already finished are skipped; the rest re-upload
			// from their preserved local file (or re-download if the
			// local file is gone).
			const auto loadProgress = [=]() -> bool {
				const auto data = EnhancedForward::LoadProgress(
					ctx->progressPath);
				if (!data) return false;
				ctx->sent = int((*data)["sent"].toInt(0));
				const auto items = (*data)["items"].toArray();
				for (const auto &v : items) {
					const auto obj = v.toObject();
					const auto i = obj["index"].toInt(-1);
					if (i < 0 || i >= int(ctx->itemIds.size())) continue;
					ctx->paths[i] = obj["path"].toString();
					ctx->downloadedBytes[i] = qint64(obj["downloaded"].toDouble());
					ctx->textOnly[i] = obj["text_only"].toBool();
					ctx->downloadDone[i] = obj["download_done"].toBool();
					ctx->uploadDone[i] = obj["upload_done"].toBool();
					ctx->uploadFileId[i] = obj["file_id"].toString().toULongLong();
					ctx->uploadPartSize[i] = qint64(obj["part_size"].toDouble());
					ctx->uploadedParts[i] = int(obj["uploaded_parts"].toInt(0));
				}
				return true;
			};

			if (resumeJob) {
				enhancedItems.clear();
				for (const auto &full : resumeJob->sourceMsgs) {
					const auto item = session().data().message(full);
					if (item) {
						enhancedItems.push_back(item);
					}
				}
				ctx->sent = resumeJob->sent;
				LOG(("ENHANCED_FWD: resume job sent=%1 path=%2")
					.arg(ctx->sent).arg(resumeJob->path));
				// Restore progress state from JSON FIRST so paths and
				// upload state are available for the checks below.
				if (!resumeJob->path.isEmpty()) {
					ctx->progressPath = resumeJob->path;
				}
				loadProgress();
				LOG(("ENHANCED_FWD: after loadProgress, checking items"));
				for (auto i = 0; i < n; i++) {
					const auto jobUploadDone = (i < int(resumeJob->uploadDone.size()))
						? resumeJob->uploadDone[i]
						: false;
					LOG(("ENHANCED_FWD: resume item %1 jobUploadDone=%2 path=%3")
						.arg(i)
						.arg(Logs::b(jobUploadDone))
						.arg(ctx->paths[i]));
					if (jobUploadDone) {
						ctx->textOnly[i] = false;
						ctx->downloadDone[i] = true;
						ctx->needsDownload[i] = false;
						ctx->uploadDone[i] = true;
						LOG(("ENHANCED_FWD: resume item %1 -> uploadDone=TRUE (was done in job)").arg(i));
					} else {
						const auto local = QFileInfo(ctx->paths[i]);
						const auto fileExists = local.exists() && local.size() > 0;
						const auto jsonSaysDownloadDone = ctx->downloadDone[i];
						LOG(("ENHANCED_FWD: resume item %1 fileExists=%2 size=%3 jsonDownloadDone=%4")
							.arg(i)
							.arg(Logs::b(fileExists))
							.arg(local.size())
							.arg(Logs::b(jsonSaysDownloadDone)));
						if (jsonSaysDownloadDone && fileExists) {
							ctx->needsDownload[i] = false;
							ctx->uploadDone[i] = false;
							LOG(("ENHANCED_FWD: resume item %1 -> download done (JSON+file), needs upload").arg(i));
						} else {
							ctx->downloadDone[i] = false;
							ctx->needsDownload[i] = true;
							ctx->uploadDone[i] = false;
							LOG(("ENHANCED_FWD: resume item %1 -> needs download (JSON=%2, file=%3)")
								.arg(i)
								.arg(Logs::b(jsonSaysDownloadDone))
								.arg(Logs::b(fileExists)));
						}
					}
				}
			} else {
				loadProgress();
			}


			EnhancedForward::setCancelCallback(
				peerId,
				&session(),
				[ctx, session = &session(), peerId] {
					LOG(("ENHANCED_FWD: cancelCallback called for peer=%1 path=%2")
						.arg(peerId.value).arg(ctx->progressPath));
					for (auto i = 0; i < int(ctx->itemIds.size()); i++) {
						const auto item = ctx->itemIds[i];
						const auto msg = session->data().message(item);
						if (!msg) continue;
						const auto media = msg->media();
						if (const auto doc = media ? media->document() : nullptr) {
							doc->cancel();
						} else if (const auto photo = media ? media->photo() : nullptr) {
							photo->cancel();
						}
						if (ctx->uploadIds[i] != FullMsgId()) {
							session->uploader().cancel(ctx->uploadIds[i]);
							ctx->uploadIndex->erase(ctx->uploadIds[i]);
						}
					}
					EnhancedForward::CleanupPartialFiles(ctx->progressPath);
				});

			EnhancedForward::setPauseCallback(
				peerId,
				&session(),
				[ctx, session = &session(), saveProgress, peerId] {
					LOG(("ENHANCED_FWD: pauseCallback called for peer=%1").arg(peerId.value));
					for (auto i = 0; i < int(ctx->itemIds.size()); i++) {
						if (ctx->textOnly[i] || ctx->uploadDone[i]) continue;
						// Pause: abort any in-flight source download.
						// The partial .PART file stays on disk, so the
						// loader resumes it (not from the beginning) when
						// the download is re-triggered on resume.
						if (!ctx->downloadDone[i]) {
							const auto item =
								session->data().message(ctx->itemIds[i]);
							if (item) {
								const auto media = item->media();
								if (const auto doc = media
										? media->document()
										: nullptr) {
									doc->pause();
								} else if (const auto photo = media
										? media->photo()
										: nullptr) {
									photo->cancel();
								}
							}
						}
						// Pause the in-flight upload via the uploader
						// (its local source survives).
						if (ctx->uploadIds[i] != FullMsgId()) {
							session->uploader().pause(ctx->uploadIds[i]);
						}
					}
					ctx->downloadInFlight = false;
					ctx->uploadInFlight = false;
					saveProgress();
				});

			EnhancedForward::setResumeCallback(
				peerId,
				&session(),
				[ctx, session = &session(), loadProgress] {
					loadProgress();
					const auto savedUploadDone = ctx->uploadDone;
					session->uploader().unpause();
					for (auto i = 0; i < int(ctx->itemIds.size()); i++) {
						if (ctx->textOnly[i] || savedUploadDone[i]) continue;
						const auto srcItem =
							session->data().message(ctx->itemIds[i]);
						if (!srcItem) {
							ctx->textOnly[i] = true;
							ctx->downloadDone[i] = true;
							ctx->uploadDone[i] = true;
							continue;
						}
						const auto media = srcItem->media();
						if (!media
							|| (!media->document() && !media->photo())) {
							continue;
						}
						ctx->downloadStarted[i] = false;
						ctx->uploadStarted[i] = false;
						if (!ctx->downloadDone[i]) {
							ctx->needsDownload[i] = true;
						}
						ctx->uploadDone[i] = false;
					}
					ctx->downloadCursor = 0;
					ctx->uploadCursor = 0;
					ctx->downloadInFlight = false;
					ctx->uploadInFlight = false;
					if (ctx->startNextDownloadFn) {
						ctx->startNextDownloadFn();
					}
					if (ctx->sendNextFn) {
						(*ctx->sendNextFn)();
					}
				});

			// Close the share-box immediately (decoupled from network ops).
			if (shared && !shared->requestsLeft) {
				shared->callback();
				shared->requestsLeft = -1;
			}

			// --- count album items that will need media upload ---
			base::flat_map<MessageGroupId, int> albumItemCounts;
			for (auto i = 0; i < n; i++) {
				const auto srcItem =
					session().data().message(ctx->itemIds[i]);
				if (!srcItem) { ctx->textOnly[i] = true; continue; }
				const auto media = srcItem->media();
				if (!media) {
					ctx->textOnly[i] = true;
				} else if (media->photo()) {
					ctx->isPhoto[i] = true;
					if (const auto sg = srcItem->groupId()) {
						albumItemCounts[sg]++;
					}
				} else if (media->document()) {
					if (const auto sg = srcItem->groupId()) {
						albumItemCounts[sg]++;
					}
				} else {
					// unsupported media type -> treat as text
					ctx->textOnly[i] = true;
				}
			}

			// Pre-create SendingAlbum objects for every unique source album.
			// We do NOT create local messages; albums are purely server-side.
			// Grouping modes:
			// - GroupAsIs: keep source albums (default)
			// - RegroupAll: all media in one album
			// - Separate: no albums, each item separate
			const auto regroupAll = (draft.groupOptions == Data::GroupingOptions::RegroupAll);
			const auto separate = (draft.groupOptions == Data::GroupingOptions::Separate);
			MessageGroupId regroupAllId;
			if (regroupAll) {
				regroupAllId = MessageGroupId::FromRaw(
					action.history->peer->id,
					base::RandomValue<uint64>(),
					false);
			}
			for (auto i = 0; i < n; i++) {
				if (ctx->textOnly[i]) continue;
				const auto srcItem =
					session().data().message(ctx->itemIds[i]);
				if (!srcItem) { ctx->textOnly[i] = true; continue; }
				
				MessageGroupId sg;
				if (separate) {
					// Separate mode: no grouping
					sg = MessageGroupId();
				} else if (regroupAll) {
					// RegroupAll mode: all media in one album
					sg = regroupAllId;
				} else {
					// GroupAsIs mode: keep source albums
					sg = srcItem->groupId();
				}
				
				if (sg) {
					ctx->sourceGroup[i] = sg;
					if (ctx->albums.find(sg) == ctx->albums.end()) {
						auto album = std::make_shared<SendingAlbum>();
						album->options = action.options;
						album->expectedCount = [&] {
							if (regroupAll) {
								// Count all non-text items
								int count = 0;
								for (auto j = 0; j < n; j++) {
									if (!ctx->textOnly[j]) count++;
								}
								return count;
							} else {
								auto it = albumItemCounts.find(sg);
								return (it != albumItemCounts.end())
									? it->second : 1;
							}
						}();
						_sendingAlbums.emplace(album->groupId, album);
						ctx->albums.emplace(sg, std::move(album));
					}
				}
			}

			LOG(("ENHANCED_FWD: setting up sendNext"));

			// --------------------------------------------------------
			// sendNext: walks ctx->current forward in original order.
			// For each slot:
			//   textOnly -> send text message directly.
			//   media    -> wait until uploadDone[i], then:
			//     album item -> call sendAlbumWithUploaded (which
			//                   batches and fires SendMultiMedia when
			//                   the last member arrives)
			//     single    -> sendMedia directly
			// --------------------------------------------------------
			constexpr auto kMaxMediaInFlight = 4;
			const auto sendNext =
				std::make_shared<std::function<void()>>();
			ctx->sendNextFn = sendNext;
			*sendNext = [=]() -> void {
				LOG(("ENHANCED_FWD: sendNext current=%1/%2")
					.arg(ctx->current).arg(n));
				if (EnhancedForward::currentProgress(ctx->peerId).state
					== EnhancedForward::State::Cancelled) {
					if (ctx->uploadLifetime) {
						auto lt = std::move(ctx->uploadLifetime);
						crl::on_main([lt = std::move(lt)]() mutable {
							if (lt) lt->destroy();
						});
					}
					if (ctx->dlLifetime) {
						auto lt = std::move(ctx->dlLifetime);
						crl::on_main([lt = std::move(lt)]() mutable {
							if (lt) lt->destroy();
						});
					}
					_session->data().sendHistoryChangeNotifications();
					return;
				}
				if (EnhancedForward::isPaused(ctx->peerId)) {
					return;
				}
				while (ctx->current < n) {
					const auto i = ctx->current;
					LOG(("ENHANCED_FWD: sendNext i=%1 textOnly=%2")
						.arg(i).arg(Logs::b(ctx->textOnly[i])));
					const auto srcItem =
						session().data().message(ctx->itemIds[i]);
					LOG(("ENHANCED_FWD: sendNext srcItem=%1")
						.arg(Logs::b(srcItem != nullptr)));

					if (ctx->textOnly[i]) {
						// Text-only: send directly via messages.sendMessage.
						ctx->current++;
						if (srcItem) {
							const auto randomId =
								base::RandomValue<uint64>();
							const auto history = action.history;
							const auto peer = history->peer;
							auto caption = srcItem->originalText();
							TextUtilities::Trim(caption);
							auto sentEntities =
								Api::EntitiesToMTP(
									_session,
									caption.entities,
									Api::ConvertOption::SkipLocal);
							using SendFlag =
								MTPmessages_SendMessage::Flag;
							auto sendFlags = SendFlag(0)
								| (ShouldSendSilent(peer, action.options)
									? SendFlag::f_silent
									: SendFlag(0))
								| (!sentEntities.v.isEmpty()
									? SendFlag::f_entities
									: SendFlag(0))
								| (action.options.scheduled
									? SendFlag::f_schedule_date
									: SendFlag(0));
							if (action.replyTo) {
								sendFlags |= SendFlag::f_reply_to;
							}
							if (action.options.sendAs) {
								sendFlags |= SendFlag::f_send_as;
							}
							if (action.options.effectId) {
								sendFlags |= SendFlag::f_effect;
							}
							const auto done = [=](
									const MTPUpdates &,
									const MTP::Response &) {
								EnhancedForward::markItemSent(
									&session(), peerId);
							};
							const auto fail = [=](
									const MTP::Error &error,
									const MTP::Response &) {
								sendMessageFail(
									error, peer, randomId,
									FullMsgId());
								EnhancedForward::markItemSent(
									&session(), peerId);
							};
							_session->data().histories(
							).sendPreparedMessage(
								history,
								action.replyTo,
								randomId,
								Data::Histories::PrepareMessage<
									MTPmessages_SendMessage>(
									MTP_flags(sendFlags),
									peer->input(),
									Data::Histories::
										ReplyToPlaceholder(),
									MTP_string(caption.text),
									MTP_long(randomId),
									MTPReplyMarkup(),
									sentEntities,
									MTP_int(
										action.options.scheduled),
									MTP_int(
										action.options
											.scheduleRepeatPeriod),
									(action.options.sendAs
										? action.options.sendAs->input()
										: MTP_inputPeerEmpty()),
									Data::ShortcutIdToMTP(
										_session,
										action.options.shortcutId),
									MTP_long(
										action.options.effectId),
									MTP_long(0),
									Api::SuggestToMTP(
										action.options.suggest)),
								done,
								fail);
						} else {
							// item disappeared, count as sent
							EnhancedForward::markItemSent(
								&session(), peerId);
						}
						continue;
					}

					// Media item: wait until upload is done AND
					// no other UploadMedia/sendMedia is in-flight.
					LOG(("ENHANCED_FWD: sendNext media uploadDone=%1 inFlight=%2")
						.arg(Logs::b(ctx->uploadDone[i]))
						.arg(Logs::b(ctx->uploadMediaInFlight)));
					if (!ctx->uploadDone[i]) return;
					if (ctx->uploadMediaInFlight >= kMaxMediaInFlight) return;
					ctx->current++;
					LOG(("ENHANCED_FWD: sendNext media current=%1/%2")
						.arg(ctx->current).arg(n));

					if (!srcItem) {
						// source gone, skip
						EnhancedForward::markItemSent(&session(), peerId);
						continue;
					}

					if (const auto sg = ctx->sourceGroup[i]) {
						// Album item: use cached source groupId to find the
						// shared SendingAlbum created during pre-creation.
						const auto albumIt = ctx->albums.find(sg);
						if (albumIt == ctx->albums.end()) {
							// Should never happen; fall back to text-only.
							ctx->textOnly[i] = true;
							ctx->uploadDone[i] = true;
							(*sendNext)();
							return;
						}
						const auto &album = albumIt->second;
						const auto albumGroupId = MessageGroupId::FromRaw(
							peerId,
							album->groupId,
							action.options.scheduled);
						// Build a temporary local item so sendAlbumWithUploaded
						// can look up the album entry by msgId.
						const auto newId = FullMsgId(
							action.history->peer->id,
							_session->data().nextLocalMessageId());
						const auto caption = srcItem->originalText();
						auto flags = NewMessageFlags(action.history->peer);
						if (action.replyTo) {
							flags |= MessageFlag::HasReplyInfo;
						}
						FillMessagePostFlags(
							action, action.history->peer, flags);
						if (action.options.scheduled) {
							flags |= MessageFlag::IsOrWasScheduled;
						}
						const auto localMsg =
							action.history->addNewLocalMessage({
								.id = newId.msg,
								.flags = flags,
								.from = NewMessageFromId(action),
								.replyTo = action.replyTo,
								.date = NewMessageDate(action.options),
								.shortcutId =
									action.options.shortcutId,
								.starsPaid =
									action.options.starsApproved,
								.postAuthor =
									NewMessagePostAuthor(action),
								.groupedId = album->groupId,
								.effectId = action.options.effectId,
								.suggest = HistoryMessageSuggestInfo(
									action.options),
							}, caption, MTP_messageMediaEmpty());
						album->items.emplace_back(kEmptyTaskId);
						album->items.back().msgId = localMsg->fullId();

						// Build the server-side media reference.
						MTPInputMedia inputMedia;
						auto uploadInfo = ctx->uploadInfos[i];
						if (ctx->isPhoto[i]) {
							inputMedia = Api::PrepareUploadedPhoto(
								srcItem, std::move(uploadInfo));
						} else {
							inputMedia = Api::PrepareUploadedDocument(
								srcItem, std::move(uploadInfo));
						}
						if (inputMedia.type() == mtpc_inputMediaEmpty) {
							EnhancedForward::markItemSent(
								&session(), peerId);
							continue;
						}
						// uploadMedia -> get server file ref -> register in album
						const auto localMsgId = localMsg->fullId();
						const auto next = sendNext;
						ctx->uploadMediaInFlight++;
						request(MTPmessages_UploadMedia(
							MTP_flags(0),
							MTPstring(),
							action.history->peer->input(),
							inputMedia
						)).done([=](const MTPMessageMedia &result) {
							const auto li =
								_session->data().message(localMsgId);
							if (!li) {
								return;
							}
							MTPInputMedia srv;
							bool ok = false;
							if (result.type() == mtpc_messageMediaPhoto) {
								const auto &d = result.c_messageMediaPhoto();
								const auto ph = d.vphoto();
								if (ph && ph->type() == mtpc_photo) {
									const auto &f = ph->c_photo();
									srv = MTP_inputMediaPhoto(
										MTP_flags(0),
										MTP_inputPhoto(
											f.vid(),
											f.vaccess_hash(),
											f.vfile_reference()),
										MTP_int(0),
										MTPInputDocument());
									ok = true;
								}
							} else if (result.type()
									== mtpc_messageMediaDocument) {
								const auto &d =
									result.c_messageMediaDocument();
								const auto dc = d.vdocument();
								if (dc && dc->type() == mtpc_document) {
									const auto &f = dc->c_document();
									srv = MTP_inputMediaDocument(
										MTP_flags(0),
										MTP_inputDocument(
											f.vid(),
											f.vaccess_hash(),
											f.vfile_reference()),
										MTPInputPhoto(),
										MTP_int(0),
										MTP_int(0),
										MTPstring());
									ok = true;
								}
							}
							if (ok) {
								sendAlbumWithUploaded(li, albumGroupId, srv);
							}
							ctx->uploadMediaInFlight--;
							(*next)();
						}).fail([=](const MTP::Error &err) {
							ctx->uploadMediaInFlight--;
							EnhancedForward::markItemSent(
								&session(), peerId);
							(*next)();
						}).send();
						continue;
					} else {
						// Single media item: send directly.
						auto uploadInfo = ctx->uploadInfos[i];
						MTPInputMedia singleMedia;
						if (ctx->isPhoto[i]) {
							singleMedia = Api::PrepareUploadedPhoto(
								srcItem, std::move(uploadInfo));
						} else {
							singleMedia = Api::PrepareUploadedDocument(
								srcItem, std::move(uploadInfo));
						}
						if (singleMedia.type() == mtpc_inputMediaEmpty) {
							EnhancedForward::markItemSent(
								&session(), peerId);
							continue;
						}
						// Create local msg in DEST history so sendMedia
						// uses the dest peer, not the source peer.
						const auto singleNewId =
							_session->data().nextLocalMessageId();
						auto lflags =
							NewMessageFlags(action.history->peer);
						if (action.replyTo) {
							lflags |= MessageFlag::HasReplyInfo;
						}
						FillMessagePostFlags(
							action, action.history->peer, lflags);
						if (action.options.scheduled) {
							lflags |= MessageFlag::IsOrWasScheduled;
						}
						const auto localMsg =
							action.history->addNewLocalMessage({
								.id = singleNewId,
								.flags = lflags,
								.from = NewMessageFromId(action),
								.replyTo = action.replyTo,
								.date = NewMessageDate(action.options),
								.shortcutId =
									action.options.shortcutId,
								.starsPaid =
									action.options.starsApproved,
								.postAuthor =
									NewMessagePostAuthor(action),
								.effectId =
									action.options.effectId,
								.suggest =
									HistoryMessageSuggestInfo(
										action.options),
								}, srcItem->originalText(),
								MTP_messageMediaEmpty());
						const auto next = sendNext;
						ctx->uploadMediaInFlight++;
						sendMedia(localMsg, singleMedia,
							action.options,
							[=](bool success) {
								ctx->uploadMediaInFlight--;
								if (!success) {
									localMsg->destroy();
								}
							EnhancedForward::markItemSent(
								&session(), peerId);
							(*next)();
						});
					continue;
				}
				}
				// Notify UI to refresh now that all messages are sent.
				_session->data().sendHistoryChangeNotifications();
				_session->changes().historyUpdated(
					action.history,
					(action.options.scheduled
						? Data::HistoryUpdate::Flag::ScheduledSent
						: Data::HistoryUpdate::Flag::MessageSent));
				for (auto i = 0; i < int(ctx->paths.size()); i++) {
					if (!ctx->paths[i].isEmpty()
						&& ctx->paths[i].startsWith(ctx->downloadPath)) {
						QFile::remove(ctx->paths[i]);
					}
				}
				EnhancedForward::ClearProgress(ctx->progressPath);
				// Tear down upload and download listeners.
				if (ctx->uploadLifetime) {
					auto lt = std::move(ctx->uploadLifetime);
					crl::on_main([lt = std::move(lt)]() mutable {
						if (lt) lt->destroy();
					});
				}
				if (ctx->dlLifetime) {
					auto lt = std::move(ctx->dlLifetime);
					crl::on_main([lt = std::move(lt)]() mutable {
						if (lt) lt->destroy();
					});
				}
			};

			// --------------------------------------------------------
			// Per-item download + immediate upload pipeline.
			// As soon as a file is downloaded, we immediately:
			//   1. Create a FileLoadTask (file prep / thumbnail gen)
			//   2. Hand the prepared result to the uploader
			//   3. When upload completes, mark uploadDone[i] and
			//      call sendNext() to advance the ordered dispatcher.
			// This way downloads and uploads are fully parallel;
			// only the final SEND step is ordered.
			// --------------------------------------------------------

			// We install upload listeners once (shared across items).
			ctx->uploadLifetime = std::make_shared<rpl::lifetime>();

			ctx->uploadIndex = std::make_shared<
				base::flat_map<FullMsgId, int>>();

			// Forward-declared so pumpUploads can start an item's upload.
			const auto startUploadForItem =
				std::make_shared<std::function<void(int)>>();

			// Start the next upload (prep) if the single upload slot is
			// free and that file has finished downloading. The upload
			// itself (network) runs inside startUploadForItem's prep
			// callback; uploadInFlight stays true for prep+upload so only
			// one upload is ever in flight, preserving source order.
			const auto pumpUploads = [=]() {
				if (ctx->uploadInFlight) return;
				if (ctx->uploadCursor >= n) return;
				if (!ctx->downloadDone[ctx->uploadCursor]) return;
				const auto i = ctx->uploadCursor;
				ctx->uploadStarted[i] = true;
				ctx->uploadInFlight = true;
				ctx->uploadCursor++;
				(*startUploadForItem)(i);
			};

			// A helper: given a downloaded file at path[i], create a
			// FileLoadTask, run it, then hand result to the uploader.
			*startUploadForItem = [=](int i) {
				LOG(("ENHANCED_FWD: startUploadForItem i=%1").arg(i));
				const auto srcItem =
					session().data().message(ctx->itemIds[i]);
				LOG(("ENHANCED_FWD: startUploadForItem srcItem=%1 textOnly=%2")
					.arg(Logs::b(srcItem != nullptr))
					.arg(Logs::b(ctx->textOnly[i])));
				if (!srcItem || ctx->textOnly[i]) {
					ctx->uploadDone[i] = true;
					ctx->uploadInFlight = false;
					pumpUploads();
					(*sendNext)();
					return;
				}
				const auto to = FileLoadTaskOptions(action);
				const auto media = srcItem->media();
				const auto doc = media ? media->document() : nullptr;
				const auto caption = TextWithTags{
					srcItem->originalText().text };
				auto args = FileLoadTask::Args{
					.session = &session(),
					.filepath = ctx->paths[i],
					.content = QByteArray(),
					.information = nullptr,
					.videoCover = nullptr,
					.type = ctx->isPhoto[i]
						? SendMediaType::Photo
						: SendMediaType::File,
					.to = to,
					.caption = caption,
					.spoiler = false,
					.album = nullptr,
					.forceFile = false,
					.sendLargePhotos = ctx->isPhoto[i],
					.idOverride = 0,
					.displayName = {},
				};
				if (doc) {
					args.displayName = doc->filename();
					auto info = std::make_unique<
						Ui::PreparedFileInformation>();
					info->filemime = doc->mimeString();
					if (!doc->inlineThumbnailBytes().isEmpty()) {
						info->fileThumbnail = Images::FromInlineBytes(
							doc->inlineThumbnailBytes());
					}
					if (doc->duration() >= 0) {
						Ui::PreparedFileInformation::Song song;
						song.duration = doc->duration();
						const auto sd = doc->song();
						if (sd) {
							song.title = sd->title;
							song.performer = sd->performer;
						}
						args.information = std::make_unique<
							Ui::PreparedFileInformation>();
						*args.information = {};
						args.information->filemime = info->filemime;
						args.information->fileThumbnail =
							info->fileThumbnail;
						args.information->media = std::move(song);
					} else {
						args.information = std::move(info);
					}
				}
				const auto weakCtx = std::weak_ptr<Ctx>(ctx);
				class EnhancedFileTask final : public Task {
				public:
					EnhancedFileTask(
						FileLoadTask::Args &&args,
						Fn<void(std::shared_ptr<FilePrepareResult>)> &&cb)
					: _impl(std::make_unique<FileLoadTask>(
						std::move(args)))
					, _cb(std::move(cb)) {}
					void process() override { _impl->process(); }
					void finish() override {
						if (_cb) _cb(_impl->peekResult());
					}
				private:
					std::unique_ptr<FileLoadTask> _impl;
					Fn<void(std::shared_ptr<FilePrepareResult>)> _cb;
				};

				auto tasks = std::vector<std::unique_ptr<Task>>();
				tasks.push_back(std::make_unique<EnhancedFileTask>(
					std::move(args),
					[=, idx = i](
							std::shared_ptr<FilePrepareResult> result) {
					const auto s = weakCtx.lock();
					if (!s) return;
					const auto prepareState =
						EnhancedForward::currentProgress(
							s->peerId).state;
					if (result && result->filesize > 0) {
						s->prepared[idx] = std::move(result);
						// Keep a stable client-chosen file_id so a paused
						// transfer can be resumed across a restart (the
						// server keeps the partial upload keyed by it).
						// Persist it and continue from the last acked
						// part instead of re-uploading everything.
						if (s->uploadFileId[idx] == 0) {
							s->uploadFileId[idx]
								= base::RandomValue<uint64>();
						}
						s->prepared[idx]->fileId
							= s->uploadFileId[idx];
						// Guard against the source metadata reporting a
						// smaller size than the real local file: always
						// upload the full on-disk file.
						const auto path = s->paths[idx];
						if (!path.isEmpty()) {
							const auto realSize = QFile(path).size();
							if (realSize > s->prepared[idx]->filesize) {
								s->prepared[idx]->filesize = realSize;
								s->prepared[idx]->partssize = realSize;
							}
						}
						// Capture the actual part size used by the
						// uploader so uploadedParts can be computed.
						if (s->prepared[idx]->partssize > 0
							&& !s->prepared[idx]->fileparts.empty()) {
							s->uploadPartSize[idx] = s->prepared[idx]->partssize
								/ int(s->prepared[idx]->fileparts.size());
						}
					} else if (prepareState == EnhancedForward::State::Sending) {
						LOG(("ENHANCED_FWD: prep fail idx=%1 "
							"result=%2 filesize=%3")
							.arg(idx)
							.arg(Logs::b(result != nullptr))
							.arg(result ? result->filesize : -1));
						s->textOnly[idx] = true;
						s->uploadDone[idx] = true;
						s->uploadInFlight = false;
						pumpUploads();
						(*sendNext)();
						return;
					} else {
						// Paused / Cancelled / erased: do not send.
						s->uploadDone[idx] = true;
						s->uploadInFlight = false;
						pumpUploads();
						return;
					}
					const auto srcIt =
						session().data().message(s->itemIds[idx]);
					if (!srcIt) {
						s->textOnly[idx] = true;
						s->uploadDone[idx] = true;
						s->uploadInFlight = false;
						pumpUploads();
						(*sendNext)();
						return;
					}
					// Bare FullMsgId key for upload tracking only.
					// Do NOT create a real HistoryItem — the
					// Uploader's own photoReady listener calls
					// sendUploadedPhoto which would cause a
					// DUPLICATE message send.
					const auto uploadId = FullMsgId(
						peerId,
						session().data().nextLocalMessageId());
					s->uploadIds[idx] = uploadId;
					ctx->uploadIndex->emplace(uploadId, idx);
					const auto fileSize = s->prepared[idx]
						? qint64(s->prepared[idx]->filesize)
						: qint64(0);
					const auto partSize = s->uploadPartSize[idx];
					const auto totalParts = (partSize > 0 && fileSize > 0)
						? int((fileSize + partSize - 1) / partSize)
						: 0;
					const auto initialProgress = (totalParts > 0)
						? float64(s->uploadedParts[idx]) / float64(totalParts)
						: 0.0;
					EnhancedForward::updateUploadProgress(
						&session(), ctx->peerId, idx,
						{ s->prepared[idx]
							? s->prepared[idx]->filename
							: QString(),
						  fileSize },
						initialProgress);
					// Clear any stale uploading state left over from a
					// previous (paused/interrupted) upload of this file.
					if (const auto prepared = s->prepared[idx]) {
						if (prepared->type == SendMediaType::Photo) {
							if (const auto photo = session().data().photo(
									prepared->id)) {
								photo->uploadingData = nullptr;
							}
						} else if (const auto document = session().data(
								).document(prepared->id)) {
							document->uploadingData = nullptr;
						}
					}
					LOG(("ENHANCED_FWD: upload starting idx=%1 uploadedParts=%2 fileId=%3")
						.arg(idx)
						.arg(s->uploadedParts[idx])
						.arg(s->uploadFileId[idx]));
					session().uploader().upload(
						uploadId,
						s->prepared[idx],
						s->uploadedParts[idx]);
					}));
				_fileLoader->addTasks(std::move(tasks));
			};

			// Install upload completion listeners (shared for all items).
			ctx->uploadLifetime = std::make_shared<rpl::lifetime>();

			const auto onUploadDone = [=](
					const Storage::UploadedMedia &data) {
				if (data.fullId.peer != peerId) return;
				const auto it = ctx->uploadIndex->find(data.fullId);
				if (it == ctx->uploadIndex->end()) return;
				const auto idx = it->second;
			ctx->uploadInfos[idx] = std::move(data.info);
			ctx->uploadDone[idx] = true;
			ctx->uploadRetries[idx] = 0;
				EnhancedForward::updateUploadProgress(
					&session(), ctx->peerId, idx,
					{ ctx->prepared[idx]
						? ctx->prepared[idx]->filename
						: QString(),
					  ctx->prepared[idx]
						? qint64(ctx->prepared[idx]->filesize)
						: qint64(0) },
					1.0);
				saveProgress();
				(*sendNext)();
				ctx->uploadInFlight = false;
				pumpUploads();
			};

			const auto onUploadFail = [=](const FullMsgId &fullId) {
				if (fullId.peer != peerId) return;
				const auto it = ctx->uploadIndex->find(fullId);
				if (it == ctx->uploadIndex->end()) return;
				const auto idx = it->second;
			ctx->uploadInFlight = false;
			const auto state =
				EnhancedForward::currentProgress(ctx->peerId).state;
			if (state == EnhancedForward::State::Paused) {
				// Upload was cancelled only because forwarding is
				// paused. Keep the item as media so resume re-uploads
				// it instead of falling back to a caption-only text.
				ctx->uploadDone[idx] = false;
				return;
			}
			if (state != EnhancedForward::State::Sending) {
				// Cancelled (or finished-and-erased): do not send
				// anything, especially not a caption-only message.
				ctx->uploadDone[idx] = true;
				return;
			}
			// A real upload failure while still sending. Retry a few
			// times (this also covers a resume whose server-side
			// temp upload expired, e.g. after a day): each retry
			// starts a FRESH from-0 upload with a new file_id
			// instead of looping on the same broken resume point.
			constexpr auto kMaxUploadRetries = 3;
			if (ctx->uploadRetries[idx] < kMaxUploadRetries) {
				ctx->uploadRetries[idx]++;
				ctx->uploadedParts[idx] = 0;
				ctx->uploadFileId[idx]
					= base::RandomValue<uint64>();
				LOG(("ENHANCED_FWD: upload failed, fresh retry %1/%2 idx=%3")
					.arg(ctx->uploadRetries[idx])
					.arg(kMaxUploadRetries)
					.arg(idx));
				ctx->uploadDone[idx] = false;
				ctx->uploadInFlight = true;
				(*startUploadForItem)(idx);
				return;
			}
			// All retries exhausted — pause and notify the user.
			LOG(("ENHANCED_FWD: upload failed permanently idx=%1").arg(idx));
			ctx->uploadInFlight = false;
			ctx->uploadDone[idx] = false;
			// Reset retry state so resume starts a fresh upload.
			ctx->uploadRetries[idx] = 0;
			ctx->uploadedParts[idx] = 0;
			ctx->uploadFileId[idx] = base::RandomValue<uint64>();
			// Send a notification message to the destination chat.
			const auto history = session().data().history(ctx->peerId);
			const auto fileName = ctx->prepared[idx]
				? ctx->prepared[idx]->filename
				: u"file"_q;
			const auto text = tr::lng_enhanced_forward_upload_failed(
				tr::now,
				lt_file_name,
				fileName);
			const auto randomId = base::RandomValue<uint64>();
			const auto newId = FullMsgId(
				ctx->peerId,
				session().data().nextLocalMessageId());
			session().data().registerMessageRandomId(randomId, newId);
			history->addNewLocalMessage({
				.id = newId.msg,
				.flags = MessageFlags(),
				.from = NewMessageFromId(action),
				.date = NewMessageDate(action.options),
			}, TextWithEntities::Simple(text), MTP_messageMediaEmpty());
			EnhancedForward::markItemSent(_session, ctx->peerId);
			saveProgress();
			// Pause the forward — the user can resume later.
			EnhancedForward::pauseForward(ctx->peerId, _session);
		};
			session().uploader().photoReady(
			) | rpl::on_next(onUploadDone, *ctx->uploadLifetime);
			session().uploader().documentReady(
			) | rpl::on_next(onUploadDone, *ctx->uploadLifetime);
			session().uploader().photoFailed(
			) | rpl::on_next(onUploadFail, *ctx->uploadLifetime);
			session().uploader().documentFailed(
			) | rpl::on_next(onUploadFail, *ctx->uploadLifetime);

			const auto onUploadProgress = [=](
					const Storage::UploadProgress &data) {
				if (data.fullId.peer != peerId) return;
				const auto it = ctx->uploadIndex->find(data.fullId);
				if (it == ctx->uploadIndex->end()) return;
				const auto idx = it->second;
				if (ctx->uploadDone[idx]) return;
				const auto prepared = ctx->prepared[idx];
				const auto filename = prepared ? prepared->filename : QString();
				const auto filesize = prepared
					? qint64(prepared->filesize)
					: qint64(0);
				const auto size = (filesize > 0)
					? filesize
					: data.size;
				const auto p = (size > 0)
					? std::clamp(
						float64(data.offset) / float64(size),
						0.,
						1.)
					: 0.;
			// Track the last server-acked part so a paused
			// transfer can be resumed from here across a restart.
			if (data.partSize > 0) {
				ctx->uploadPartSize[idx] = data.partSize;
			}
			if (data.size > 0
				&& ctx->uploadPartSize[idx] > 0) {
				ctx->uploadedParts[idx] = int(
					data.offset / ctx->uploadPartSize[idx]);
					// Periodically save progress so resume survives
					// an app kill during upload.
					const auto newPct = (data.size > 0)
						? int(float64(data.offset) / float64(data.size) * 100)
						: 0;
					const auto lastSaved = ctx->lastSavedPct[idx];
					if (newPct >= lastSaved + 10 || newPct >= 99) {
						ctx->lastSavedPct[idx] = newPct;
						saveProgress();
					}
				}
				EnhancedForward::updateUploadProgress(
					&session(), ctx->peerId, idx,
					{ filename, filesize },
					p);
			};
			session().uploader().photoProgressInfo(
			) | rpl::on_next(onUploadProgress, *ctx->uploadLifetime);
			session().uploader().documentProgressInfo(
			) | rpl::on_next(onUploadProgress, *ctx->uploadLifetime);

			// Classify each item. Items that need a network download are
			// only marked here (needsDownload); the actual download is
			// started later, strictly one-at-a-time in index order, so
			// files complete in order and can be sent to the chat
			// incrementally instead of all at the end.
			LOG(("ENHANCED_FWD: classify items n=%1").arg(n));
			for (auto i = 0; i < n; i++) {
				LOG(("ENHANCED_FWD: classify i=%1 textOnly=%2")
					.arg(i).arg(Logs::b(ctx->textOnly[i])));
				if (ctx->textOnly[i]) {
					// text-only: no download/upload needed
					ctx->downloadDone[i] = true;
					ctx->uploadDone[i] = true;
					continue;
				}
				const auto item =
					session().data().message(ctx->itemIds[i]);
				if (!item) {
					ctx->textOnly[i] = true;
					ctx->downloadDone[i] = true;
					ctx->uploadDone[i] = true;
					continue;
				}
				const auto media = item->media();
				if (const auto doc = media ? media->document() : nullptr) {
					const auto filepath = doc->filepath(true);
					if (!filepath.isEmpty()) {
						// Already saved to a real file on disk: use it
						// in place (no copy, no download).
						ctx->paths[i] = filepath;
						ctx->downloadDone[i] = true;
						EnhancedForward::updateDownloadProgress(
							&session(), ctx->peerId, i,
							{ doc->filename(), doc->size },
							1.0);
					} else {
						auto name = doc->filename();
						if (name.isEmpty()) name = u"file"_q;
						name.replace(
							QRegularExpression("[:<>\"\\\\|?*]"), "_");
						ctx->paths[i] = QDir(ctx->downloadPath)
							.absoluteFilePath(name);
						QDir().mkpath(
							QFileInfo(ctx->paths[i]).absolutePath());
						ctx->needsDownload[i] = true;
					}
				} else if (const auto photo =
						media ? media->photo() : nullptr) {
					const auto v = photo->activeMediaView();
					const auto destPath = QDir(ctx->downloadPath)
						.absoluteFilePath(
							QString::number(i)
							+ u"_"_q
							+ QString::number(photo->id)
							+ u".jpg"_q);
					ctx->paths[i] = destPath;
					if (v && v->loaded() && v->saveToFile(destPath)) {
						ctx->downloadDone[i] = true;
						EnhancedForward::updateDownloadProgress(
							&session(), ctx->peerId, i,
							{ QString::number(photo->id) + u".jpg"_q,
							  0 },
							1.0);
					} else {
						ctx->needsDownload[i] = true;
					}
				} else {
					// Unsupported media type.
					ctx->textOnly[i] = true;
					ctx->downloadDone[i] = true;
					ctx->uploadDone[i] = true;
				}
			}
			LOG(("ENHANCED_FWD: classify done"));
			for (auto i = 0; i < n; i++) {
				LOG(("ENHANCED_FWD: post-classify item %1 uploadDone=%2 textOnly=%3 downloadDone=%4")
					.arg(i)
					.arg(Logs::b(ctx->uploadDone[i]))
					.arg(Logs::b(ctx->textOnly[i]))
					.arg(Logs::b(ctx->downloadDone[i])));
			}

			// Kick off uploads for already-cached items in source order.
			pumpUploads();

			{
				LOG(("ENHANCED_FWD: setting up download listeners"));
				// Always installed so pause/resume can restart downloads.
				ctx->dlLifetime = std::make_shared<rpl::lifetime>();

				// Forward declarations for the two mutually-recursive
				// drivers: pumpDownloads (one download at a time,
				// oldest-first) and checkItem (marks a finished download
				// and advances both the download and upload pipelines).
				const auto checkItem =
					std::make_shared<std::function<void(int)>>();
				const auto pumpDownloads =
					std::make_shared<std::function<void()>>();

				*checkItem = [=](int i) {
					if (ctx->downloadDone[i]) {
						return;
					}
					if (!ctx->downloadStarted[i]) {
						return;
					}
					if (EnhancedForward::currentProgress(ctx->peerId).state
							== EnhancedForward::State::Cancelled) {
						return;
					}
					// While paused, do not advance: let an in-flight
					// download finish to disk but do NOT start its
					// upload or the next download.
					if (EnhancedForward::isPaused(ctx->peerId)) {
						return;
					}
					// The downloaded file must exist on disk and be
					// complete for this item to be considered done.
					const auto fi = QFileInfo(ctx->paths[i]);
					if (fi.exists()) {
						const auto item =
							session().data().message(ctx->itemIds[i]);
						const auto media = item
							? item->media()
							: nullptr;
						if (const auto doc =
								media ? media->document() : nullptr) {
							if (doc->size > 0
								&& fi.size() < doc->size) {
								ctx->downloadedBytes[i] = fi.size();
								EnhancedForward::updateDownloadProgress(
									&session(), ctx->peerId, i,
									{ doc->filename(), doc->size },
									float64(fi.size())
										/ float64(doc->size));
								return;
							}
						}
						ctx->downloadDone[i] = true;
						ctx->downloadedBytes[i] = fi.size();
						ctx->downloadInFlight = false;
						EnhancedForward::updateDownloadProgress(
							&session(), ctx->peerId, i,
							{ fi.fileName(), fi.size() },
							1.0);
						// Immediately start this file's upload and the
						// next file's download (they overlap).
						pumpUploads();
						(*pumpDownloads)();
						return;
					}
					// File not ready yet: report progress from the
					// document / photo load state.
					const auto item =
						session().data().message(ctx->itemIds[i]);
					if (!item) {
						return;
					}
					const auto media = item->media();
					if (const auto doc =
							media ? media->document() : nullptr) {
						if (doc->loading()) {
							ctx->downloadedBytes[i] =
								qint64(doc->loadOffset());
							EnhancedForward::updateDownloadProgress(
								&session(), ctx->peerId, i,
								{ doc->filename(), doc->size },
								doc->progress());
						}
					} else if (const auto photo =
							media ? media->photo() : nullptr) {
						const auto v = ctx->photoViews[i]
							? ctx->photoViews[i]
							: photo->activeMediaView();
						const auto loaded = v && v->loaded();
						const auto failed = photo->failed(
							Data::PhotoSize::Large);
						const auto loading = photo->loading(
							Data::PhotoSize::Large);
						LOG(("ENHANCED_FWD: checkItem photo i=%1 loaded=%2 failed=%3 loading=%4")
							.arg(i)
							.arg(int(loaded))
							.arg(int(failed))
							.arg(int(loading)));
						if (loaded) {
							if (v->saveToFile(ctx->paths[i])) {
								ctx->downloadDone[i] = true;
								ctx->downloadedBytes[i] = QFile(ctx->paths[i]).size();
								EnhancedForward::updateDownloadProgress(
									&session(), ctx->peerId, i,
									{ QString::number(photo->id) + u".jpg"_q,
									  0 },
									1.0);
								ctx->downloadInFlight = false;
								pumpUploads();
								(*pumpDownloads)();
							} else {
								ctx->textOnly[i] = true;
								ctx->downloadDone[i] = true;
								ctx->uploadDone[i] = true;
								(*sendNext)();
							}
						} else if (failed) {
							ctx->textOnly[i] = true;
							ctx->downloadDone[i] = true;
							ctx->uploadDone[i] = true;
							ctx->downloadInFlight = false;
							(*sendNext)();
							(*pumpDownloads)();
						} else if (!loading) {
							// Load neither completed nor is in progress:
							// LoadCloudFile bailed out (e.g. invalid Large
							// location) so nothing will ever resolve this.
							LOG(("ENHANCED_FWD: photo i=%1 stuck, falling back to text-only")
								.arg(i));
							ctx->textOnly[i] = true;
							ctx->downloadDone[i] = true;
							ctx->uploadDone[i] = true;
							ctx->downloadInFlight = false;
							(*sendNext)();
							(*pumpDownloads)();
						}
					}
				};

				// Start the next download (one at a time, oldest-first).
				// After it finishes, checkItem kicks off its upload and
				// the following download together.
				*pumpDownloads = [=]() {
					// Skip items that do not need a download.
					while (ctx->downloadCursor < n
							&& (!ctx->needsDownload[ctx->downloadCursor]
								|| ctx->downloadDone[ctx->downloadCursor])) {
						ctx->downloadCursor++;
					}
					if (EnhancedForward::isPaused(ctx->peerId)) {
						return;
					}
					if (ctx->downloadInFlight) return;
					if (ctx->downloadCursor >= n) {
						return;
					}
					const auto i = ctx->downloadCursor;
					ctx->downloadStarted[i] = true;
					ctx->downloadInFlight = true;
					ctx->downloadCursor++;
					const auto item =
						session().data().message(ctx->itemIds[i]);
					const auto media = item
						? item->media()
						: nullptr;
					const auto doc = media
						? media->document()
						: nullptr;
					const auto photo = media
						? media->photo()
						: nullptr;
					if (doc) {
						doc->save(
							Data::FileOrigin(FullMsgId(
								item->history()->peer->id,
								item->id)),
							ctx->paths[i],
							LoadFromCloudOrLocal,
							false,
							true);
					} else if (photo) {
						ctx->photoViews[i] = photo->createMediaView();
						LOG(("ENHANCED_FWD: photo load i=%1 peer=%2 msg=%3 locValid=%4 loading=%5 failed=%6")
							.arg(i)
							.arg(quint64(item->history()->peer->id.value))
							.arg(quint64(item->id.bare))
							.arg(int(photo->location(Data::PhotoSize::Large).valid()))
							.arg(int(photo->loading(Data::PhotoSize::Large)))
							.arg(int(photo->failed(Data::PhotoSize::Large))));
						photo->load(
							Data::PhotoSize::Large,
							Data::FileOrigin(FullMsgId(
								item->history()->peer->id,
								item->id)));
					} else {
						ctx->textOnly[i] = true;
						ctx->downloadDone[i] = true;
						ctx->uploadDone[i] = true;
						ctx->downloadInFlight = false;
						(*sendNext)();
						(*pumpDownloads)();
						return;
					}
					// Already cached on disk: completes immediately.
					(*checkItem)(i);
					if (ctx->downloadDone[i]) {
						(*pumpDownloads)();
					}
				};

				ctx->startNextDownloadFn = [=]() {
					(*pumpDownloads)();
					pumpUploads();
				};

				// Watch for download completions and re-check the item.
				session().data().documentLoadProgress(
				) | rpl::on_next([=](not_null<DocumentData*> doc) {
					for (auto i = 0; i < n; i++) (*checkItem)(i);
				}, *ctx->dlLifetime);

				session().downloaderTaskFinished(
				) | rpl::on_next([=] {
					for (auto i = 0; i < n; i++) (*checkItem)(i);
				}, *ctx->dlLifetime);

				// Begin: download + upload advance together.
				LOG(("ENHANCED_FWD: starting pipeline"));
				(*pumpDownloads)();
				pumpUploads();
				LOG(("ENHANCED_FWD: pipeline kicked off"));
			}

			// Kick off the ordered sender (handles text-only items
			// and any items already marked uploadDone).
			LOG(("ENHANCED_FWD: calling sendNext"));
			(*sendNext)();
			LOG(("ENHANCED_FWD: sendNext returned"));

		}

		LOG(("ENHANCED_FWD: checking normalItems"));
		if (normalItems.empty()) {
			LOG(("ENHANCED_FWD: normalItems empty, returning"));
			return;
		}
		draft.items = std::move(normalItems);
	}

	auto &histories = _session->data().histories();
    
	const auto count = int(draft.items.size());
	const auto genClientSideMessage = action.generateLocal
		&& (count < 2)
		&& (draft.options == Data::ForwardOptions::Quoted);
	const auto history = action.history;
	const auto peer = history->peer;
    
	if (!action.options.scheduled && !action.options.shortcutId) {
		histories.readInbox(history);
	}
	const auto sendAs = action.options.sendAs;
	const auto silentPost = ShouldSendSilent(peer, action.options);
    
	using SendFlag = MTPmessages_ForwardMessages::Flag;
	auto flags = MessageFlags();
	auto sendFlags = SendFlag() | SendFlag();
	FillMessagePostFlags(action, peer, flags);
	if (silentPost) {
		sendFlags |= SendFlag::f_silent;
	}
	if (action.options.scheduled) {
		flags |= MessageFlag::IsOrWasScheduled;
		sendFlags |= SendFlag::f_schedule_date;
		if (action.options.scheduleRepeatPeriod) {
			sendFlags |= SendFlag::f_schedule_repeat_period;
		}
	}
	if (action.options.shortcutId) {
		flags |= MessageFlag::ShortcutMessage;
		sendFlags |= SendFlag::f_quick_reply_shortcut;
	}
	if (action.options.effectId) {
		sendFlags |= SendFlag::f_effect;
	}
	if (draft.options != Data::ForwardOptions::Quoted) {
		sendFlags |= SendFlag::f_drop_author;
	}
	if (draft.options == Data::ForwardOptions::UnquotedWithoutCaptions) {
		sendFlags |= SendFlag::f_drop_media_captions;
	}
	
	// Check if regrouping is requested (only meaningful when author is dropped)
	// If user selected a non-default grouping option, it always applies
	// and overrides Quoted (regrouping requires sending as new messages)
	const auto regroupAll = (draft.groupOptions == Data::GroupingOptions::RegroupAll);
	const auto separate = (draft.groupOptions == Data::GroupingOptions::Separate);
	const auto needsRegrouping = regroupAll || separate;
	
	if (needsRegrouping) {
		// Move the callback from shared (where it was stored at line 3670)
		// back out so sendForwardWithRegrouping can use it
		auto regroupCallback = shared
			? std::move(shared->callback)
			: FnMut<void()>();
		// Use separate send path for regrouping - send as new messages with controlled groupId
		sendForwardWithRegrouping(
			std::move(draft),
			std::move(action),
			std::move(regroupCallback),
			regroupAll,
			separate);
		return;
	}
	
	if (sendAs) {
		sendFlags |= SendFlag::f_send_as;
	}
	if (action.options.suggest) {
		sendFlags |= SendFlag::f_suggested_post;
	}
	const auto kGeneralId = Data::ForumTopic::kGeneralId;
	const auto topicRootId = action.replyTo.topicRootId;
	const auto topMsgId = (topicRootId == kGeneralId)
		? MsgId(0)
		: topicRootId;
	if (topMsgId) {
		sendFlags |= SendFlag::f_top_msg_id;
	}
	const auto monoforumPeerId = action.replyTo.monoforumPeerId;
	const auto monoforumPeer = monoforumPeerId
		? session().data().peer(monoforumPeerId).get()
		: nullptr;
	if (monoforumPeer || (action.options.suggest && action.replyTo)) {
		sendFlags |= SendFlag::f_reply_to;
	}
    
	constexpr auto kMaxForwardBatch = 100;
	auto forwardFrom = draft.items.front()->history()->peer;
	auto ids = QVector<MTPint>();
	auto randomIds = QVector<MTPlong>();
	auto localIds = std::shared_ptr<base::flat_map<uint64, FullMsgId>>();
    
	const auto sendAccumulated = [&] {
		if (shared) {
			++shared->requestsLeft;
		}
		const auto idsCopy = localIds;
		const auto scheduled = action.options.scheduled;
		const auto starsPaid = std::min(
			action.options.starsApproved,
			int(ids.size() * peer->starsPerMessageChecked()));
		auto oneFlags = sendFlags;
		if (starsPaid) {
			action.options.starsApproved -= starsPaid;
			oneFlags |= SendFlag::f_allow_paid_stars;
		}
		auto buildMessage = [=](
				not_null<History*> history,
				FullReplyTo replyTo)
			-> Data::Histories::PreparedMessage {
			const auto kGeneralId = Data::ForumTopic::kGeneralId;
			const auto realTopMsgId = (replyTo.topicRootId == kGeneralId)
				? MsgId(0)
				: replyTo.topicRootId;
			auto flags = oneFlags;
			if (realTopMsgId) {
				flags |= SendFlag::f_top_msg_id;
			} else {
				flags &= ~SendFlag::f_top_msg_id;
			}
			// Check NoForwards: if source is a NoForwards channel and takeout
			// is active, the existing _takeoutId branch below will wrap it.
			auto fwdMsg = MTPmessages_ForwardMessages(
				MTP_flags(flags),
				forwardFrom->input(),
				MTP_vector<MTPint>(ids),
				MTP_vector<MTPlong>(randomIds),
				history->peer->input(),
				MTP_int(realTopMsgId),
				(action.options.suggest
					? ReplyToForMTP(history, replyTo)
					: monoforumPeer
					? MTP_inputReplyToMonoForum(
						monoforumPeer->input())
					: MTPInputReplyTo()),
				MTP_int(action.options.scheduled),
				MTP_int(action.options.scheduleRepeatPeriod),
				(sendAs
					? sendAs->input()
					: MTP_inputPeerEmpty()),
				Data::ShortcutIdToMTP(
					&history->session(),
					action.options.shortcutId),
				MTP_long(action.options.effectId),
				MTPint(),
				MTP_long(starsPaid),
				Api::SuggestToMTP(action.options.suggest));
			return std::move(fwdMsg);
		};
		const auto doneCallback = [=](
				const MTPUpdates &result,
				const MTP::Response &) {
			if (!scheduled) {
				_session->api().updates().checkForSentToScheduled(result);
			}
			if (shared && !--shared->requestsLeft) {
				shared->callback();
			}
			if (peer->isSelf() && _session->premium()) {
				ProcessRecentSelfForwards(
					_session, result, peer->id, forwardFrom->id);
			}
		};
		const auto failCallback = [=](
				const MTP::Error &error,
				const MTP::Response &) {
			if (idsCopy) {
				for (const auto &[randomId, itemId] : *idsCopy) {
					_session->api().sendMessageFail(
						error, peer, randomId, itemId);
				}
			} else {
				_session->api().sendMessageFail(error, peer);
			}
		};
		ids.resize(0);
		randomIds.resize(0);
		localIds = nullptr;
	};
    
	ids.reserve(count);
	randomIds.reserve(count);
	for (const auto &item : draft.items) {
		const auto randomId = base::RandomValue<uint64>();
		if (genClientSideMessage) {
			const auto newId = FullMsgId(
				peer->id,
				_session->data().nextLocalMessageId());
			history->addNewLocalMessage({
				.id = newId.msg,
				.flags = flags,
				.from = NewMessageFromId(action),
				.replyTo = {
					.topicRootId = topMsgId,
					.monoforumPeerId = monoforumPeerId,
				},
				.date = NewMessageDate(action.options),
				.shortcutId = action.options.shortcutId,
				.starsPaid = action.options.starsApproved,
				.postAuthor = NewMessagePostAuthor(action),
				.suggest = HistoryMessageSuggestInfo(action.options),
			}, item);
			_session->data().registerMessageRandomId(randomId, newId);
			if (!localIds) {
				localIds = std::make_shared<base::flat_map<uint64, FullMsgId>>();
			}
			localIds->emplace(randomId, newId);
		}
		const auto newFrom = item->history()->peer;
		if (forwardFrom != newFrom) {
			sendAccumulated();
			forwardFrom = newFrom;
		}
		if (ids.size() >= kMaxForwardBatch) {
			sendAccumulated();
		}
		ids.push_back(MTP_int(item->id));
		randomIds.push_back(MTP_long(randomId));
	}
	sendAccumulated();
	_session->data().sendHistoryChangeNotifications();
}

void ApiWrap::startResumeForward(
		const PeerId &srcId,
		const PeerId &dstId,
		not_null<Main::Session*> session,
		const QString &path) {
	startResumeEnhancedForward(srcId, dstId, session, path);
}

void ApiWrap::startResumeEnhancedForward(
		const PeerId &srcId,
		const PeerId &dstId,
		not_null<Main::Session*> session,
		const QString &path) {
	LOG(("ENHANCED_FWD: startResumeEnhancedForward src=%1 dst=%2 path=%3")
		.arg(srcId.value).arg(dstId.value).arg(path));
	const auto progressPath = !path.isEmpty()
		? path
		: [&] {
			const auto dir = File::DefaultDownloadPath(session)
				+ "ForwardTemp/";
			const auto srcName = session->data().peer(srcId)->name();
			const auto bareName = EnhancedForward::ProgressFileBareName(srcName);
			return EnhancedForward::ProgressFilePath(bareName, dir);
		}();
	LOG(("ENHANCED_FWD: startResumeEnhancedForward progressPath=%1")
		.arg(progressPath));
	const auto data = EnhancedForward::LoadProgress(progressPath);
	if (!data) {
		LOG(("ENHANCED_FWD: startResumeEnhancedForward load failed"));
		return;
	}
	const auto total = int((*data)["total"].toInt(0));
	const auto sent = int((*data)["sent"].toInt(0));
	if (total <= 0 || sent >= total) {
		return;
	}
	// Guard against starting the same resume twice (e.g. the user
	// clicking "Resume" repeatedly while source messages are still
	// being fetched). Without this, each click would spin up its own
	// pipeline against the same peer.
	static auto Resuming = std::set<PeerId>();
	if (Resuming.contains(dstId)) {
		return;
	}
	const auto done = [=] {
		Resuming.erase(dstId);
	};
	auto job = std::make_shared<EnhancedForward::SavedJob>();
	job->srcId = srcId;
	job->dstId = dstId;
	job->total = total;
	job->sent = sent;
	job->path = progressPath;
	const auto srcPeerId = PeerId(
		qulonglong((*data)["src_peer"].toDouble()));
	const auto msgs = (*data)["source_msgs"].toArray();
	for (const auto &v : msgs) {
		const auto obj = v.toObject();
		job->sourceMsgs.push_back(FullMsgId(
			srcPeerId,
			MsgId(obj["msg"].toVariant().toLongLong())));
	}
	const auto items = (*data)["items"].toArray();
	for (const auto &v : items) {
		const auto obj = v.toObject();
		job->uploadDone.push_back(obj["upload_done"].toBool(false));
		job->fileId.push_back(
			obj["file_id"].toString().toULongLong());
		job->uploadedParts.push_back(
			int(obj["uploaded_parts"].toInt(0)));
	}

	const auto resume = [=] {
		auto resolved = std::vector<not_null<HistoryItem*>>();
		resolved.reserve(job->sourceMsgs.size());
		auto ok = true;
		for (const auto &full : job->sourceMsgs) {
			const auto item = session->data().message(full);
			if (item) {
				resolved.push_back(item);
			} else {
				LOG(("ENHANCED_FWD: resume source msg not found peer=%1 msg=%2")
					.arg(full.peer.value).arg(full.msg.bare));
				ok = false;
				break;
			}
		}
		if (!ok || resolved.empty()) return false;
		LOG(("ENHANCED_FWD: resume resolved %1 messages, starting forward")
			.arg(resolved.size()));
		Data::ResolvedForwardDraft draft;
		draft.items = std::move(resolved);
		SendAction action(session->data().history(dstId));
		forwardMessages(
			std::move(draft),
			action,
			nullptr,
			job);
		done();
		return true;
	};

	if (resume()) {
		return;
	}

	const auto fetchAndResume = [=] {
		auto ids = QVector<MTPInputMessage>();
		ids.reserve(job->sourceMsgs.size());
		for (const auto &full : job->sourceMsgs) {
			ids.push_back(MTP_inputMessageID(MTP_int(full.msg.bare)));
		}
		const auto srcPeer = session->data().peer(srcId);
		const auto channel = srcPeer
			? srcPeer->asChannel()
			: nullptr;
		auto afterFetch = [=] {
			struct RetryState {
				int attempts = 0;
			};
			const auto state = std::make_shared<RetryState>();
			const auto timer = std::make_shared<base::Timer>();
			timer->setCallback([=] {
				state->attempts++;
				if (resume() || state->attempts > 20) {
					timer->cancel();
					done();
				}
			});
			timer->callEach(crl::time(250));
		};
		if (channel) {
			request(MTPchannels_GetMessages(
				channel->inputChannel(),
				MTP_vector<MTPInputMessage>(ids)
			)).done([=](const MTPmessages_Messages &result) {
				session->data().processExistingMessages(channel, result);
				afterFetch();
			}).fail([=](const MTP::Error &) {
				afterFetch();
			}).send();
		} else {
			request(MTPmessages_GetMessages(
				MTP_vector<MTPInputMessage>(ids)
			)).done([=](const MTPmessages_Messages &result) {
				session->data().processExistingMessages(nullptr, result);
				afterFetch();
			}).fail([=](const MTP::Error &) {
				afterFetch();
			}).send();
		}
	};
	fetchAndResume();
}

void ApiWrap::shareContact(
		const QString &phone,
		const QString &firstName,
		const QString &lastName,
		const SendAction &action,
		Fn<void(bool)> done) {
	const auto userId = UserId(0);
	sendSharedContact(
		phone,
		firstName,
		lastName,
		userId,
		action,
		std::move(done));
}

void ApiWrap::shareContact(
		not_null<UserData*> user,
		const SendAction &action,
		Fn<void(bool)> done) {
	const auto userId = peerToUser(user->id);
	const auto phone = _session->data().findContactPhone(user);
	if (phone.isEmpty()) {
		if (done) {
			done(false);
		}
		return;
	}
	return sendSharedContact(
		phone,
		user->firstName,
		user->lastName,
		userId,
		action,
		std::move(done));
}

void ApiWrap::sendSharedContact(
		const QString &phone,
		const QString &firstName,
		const QString &lastName,
		UserId userId,
		const SendAction &action,
		Fn<void(bool)> done) {
	sendAction(action);

	const auto history = action.history;
	const auto peer = history->peer;

	const auto newId = FullMsgId(
		peer->id,
		_session->data().nextLocalMessageId());
	auto flags = NewMessageFlags(peer);
	if (action.replyTo) {
		flags |= MessageFlag::HasReplyInfo;
	}
	FillMessagePostFlags(action, peer, flags);
	if (action.options.scheduled) {
		flags |= MessageFlag::IsOrWasScheduled;
	}
	if (action.options.shortcutId) {
		flags |= MessageFlag::ShortcutMessage;
	}
	const auto item = history->addNewLocalMessage({
		.id = newId.msg,
		.flags = flags,
		.from = NewMessageFromId(action),
		.replyTo = action.replyTo,
		.date = NewMessageDate(action.options),
		.shortcutId = action.options.shortcutId,
		.starsPaid = action.options.starsApproved,
		.postAuthor = NewMessagePostAuthor(action),
		.effectId = action.options.effectId,
		.suggest = HistoryMessageSuggestInfo(action.options),
	}, TextWithEntities(), MTP_messageMediaContact(
		MTP_string(phone),
		MTP_string(firstName),
		MTP_string(lastName),
		MTP_string(), // vcard
		MTP_long(userId.bare)));

	const auto media = MTP_inputMediaContact(
		MTP_string(phone),
		MTP_string(firstName),
		MTP_string(lastName),
		MTP_string()); // vcard
	sendMedia(item, media, action.options, std::move(done));

	_session->data().sendHistoryChangeNotifications();
	_session->changes().historyUpdated(
		history,
		(action.options.scheduled
			? Data::HistoryUpdate::Flag::ScheduledSent
			: Data::HistoryUpdate::Flag::MessageSent));
}

void ApiWrap::sendVoiceMessage(
		QByteArray result,
		VoiceWaveform waveform,
		crl::time duration,
		bool video,
		const SendAction &action) {
	const auto caption = TextWithTags();
	const auto to = FileLoadTaskOptions(action);
	_fileLoader->addTask(
		std::make_unique<FileLoadTask>(FileLoadTask::VoiceArgs{
			.session = &session(),
			.voice = result,
			.duration = duration,
			.waveform = waveform,
			.video = video,
			.to = to,
			.caption = caption,
		}));
}

void ApiWrap::editMedia(
		Ui::PreparedList &&list,
		SendMediaType type,
		TextWithTags &&caption,
		const SendAction &action) {
	if (list.files.empty()) return;

	auto &file = list.files.front();
	auto to = FileLoadTaskOptions(action);
	const auto existing = to.replaceMediaOf
		? session().data().message(action.history->peer, to.replaceMediaOf)
		: nullptr;
	if (existing && existing->computeSuggestionActions()
		== SuggestionActions::AcceptAndDecline) {
		to.replyTo.messageId = {
			action.history->peer->id,
			to.replaceMediaOf
		};
		to.replyTo.monoforumPeerId = existing->sublistPeerId();
		to.replaceMediaOf = MsgId();
	}
	const auto forceFile = (type == SendMediaType::File)
		&& (file.type == Ui::PreparedFile::Type::Video);
	_fileLoader->addTask(std::make_unique<FileLoadTask>(FileLoadTask::Args{
		.session = &session(),
		.filepath = file.path,
		.content = file.content,
		.information = std::move(file.information),
		.videoCover = (file.videoCover
			? std::make_unique<FileLoadTask>(FileLoadTask::Args{
				.session = &session(),
				.filepath = file.videoCover->path,
				.content = file.videoCover->content,
				.information = std::move(file.videoCover->information),
				.videoCover = nullptr,
				.type = SendMediaType::Photo,
				.to = to,
				.caption = TextWithTags(),
				.spoiler = false,
				.album = nullptr,
				.forceFile = false,
				.sendLargePhotos = false,
				.idOverride = 0,
			})
			: nullptr),
		.type = type,
		.to = to,
		.caption = caption,
		.spoiler = file.spoiler,
		.album = nullptr,
		.forceFile = forceFile,
		.sendLargePhotos = file.sendLargePhotos,
		.idOverride = 0,
		.displayName = file.displayName,
	}));
}

void ApiWrap::sendFiles(
		Ui::PreparedList &&list,
		SendMediaType type,
		std::shared_ptr<SendingAlbum> album,
		const SendAction &action) {
	const auto to = FileLoadTaskOptions(action);
	if (album) {
		album->options = to.options;
	}
	auto tasks = std::vector<std::unique_ptr<Task>>();
	tasks.reserve(list.files.size());
	for (auto &file : list.files) {
		const auto uploadWithType = !album
			? type
			: (file.type == Ui::PreparedFile::Type::Photo
				&& type != SendMediaType::File)
			? SendMediaType::Photo
			: SendMediaType::File;
		const auto forceFile = (type == SendMediaType::File)
			&& (file.type == Ui::PreparedFile::Type::Video);
		tasks.push_back(std::make_unique<FileLoadTask>(FileLoadTask::Args{
			.session = &session(),
			.filepath = file.path,
			.content = file.content,
			.information = std::move(file.information),
			.videoCover = (file.videoCover
				? std::make_unique<FileLoadTask>(FileLoadTask::Args{
					.session = &session(),
					.filepath = file.videoCover->path,
					.content = file.videoCover->content,
					.information = std::move(file.videoCover->information),
					.videoCover = nullptr,
					.type = SendMediaType::Photo,
					.to = to,
					.caption = TextWithTags(),
					.spoiler = false,
					.album = nullptr,
					.forceFile = false,
					.sendLargePhotos = false,
					.idOverride = 0,
				})
				: nullptr),
			.type = uploadWithType,
			.to = to,
			.caption = std::move(file.caption),
			.spoiler = file.spoiler,
			.album = album,
			.forceFile = forceFile,
			.sendLargePhotos = file.sendLargePhotos,
			.idOverride = 0,
			.displayName = file.displayName,
		}));
	}
	if (album) {
		_sendingAlbums.emplace(album->groupId, album);
		album->items.reserve(tasks.size());
		for (const auto &task : tasks) {
			album->items.emplace_back(task->id());
		}
	}
	_fileLoader->addTasks(std::move(tasks));
}

void ApiWrap::sendFile(
		const QByteArray &fileContent,
		SendMediaType type,
		const SendAction &action) {
	const auto to = FileLoadTaskOptions(action);
	auto caption = TextWithTags();
	const auto spoiler = false;
	_fileLoader->addTask(std::make_unique<FileLoadTask>(FileLoadTask::Args{
		.session = &session(),
		.filepath = QString(),
		.content = fileContent,
		.information = nullptr,
		.videoCover = nullptr,
		.type = type,
		.to = to,
		.caption = caption,
		.spoiler = spoiler,
		.album = nullptr,
		.forceFile = false,
		.idOverride = 0
	}));
}

void ApiWrap::sendUploadedPhoto(
		FullMsgId localId,
		Api::RemoteFileInfo info,
		Api::SendOptions options) {
	if (const auto item = _session->data().message(localId)) {
		const auto media = Api::PrepareUploadedPhoto(item, std::move(info));
		if (const auto groupId = item->groupId()) {
			uploadAlbumMedia(item, groupId, media);
		} else {
			sendMedia(item, media, options);
		}
	}
}

void ApiWrap::sendUploadedDocument(
		FullMsgId localId,
		Api::RemoteFileInfo info,
		Api::SendOptions options) {
	if (const auto item = _session->data().message(localId)) {
		if (!item->media() || !item->media()->document()) {
			return;
		}
		const auto media = Api::PrepareUploadedDocument(
			item,
			std::move(info));
		const auto groupId = item->groupId();
		if (groupId) {
			uploadAlbumMedia(item, groupId, media);
		} else {
			sendMedia(item, media, options);
		}
	}
}

void ApiWrap::cancelLocalItem(not_null<HistoryItem*> item) {
	Expects(item->isSending());

	if (const auto groupId = item->groupId()) {
		sendAlbumWithCancelled(item, groupId);
	}
}

void ApiWrap::sendShortcutMessages(
		not_null<PeerData*> peer,
		BusinessShortcutId id) {
	auto ids = QVector<MTPint>();
	auto randomIds = QVector<MTPlong>();
	request(MTPmessages_SendQuickReplyMessages(
		peer->input(),
		MTP_int(id),
		MTP_vector<MTPint>(ids),
		MTP_vector<MTPlong>(randomIds)
	)).done([=](const MTPUpdates &result) {
		applyUpdates(result);
	}).fail([=](const MTP::Error &error) {
	}).send();
}

void ApiWrap::sendMessage(
		MessageToSend &&message,
		std::optional<MsgId> localMessageId) {
	const auto history = message.action.history;
	const auto peer = history->peer;
	auto &textWithTags = message.textWithTags;

	auto action = message.action;
	action.generateLocal = true;
	sendAction(action);

	const auto clearCloudDraft = action.clearDraft;
	const auto draftTopicRootId = action.replyTo.topicRootId;
	const auto draftMonoforumPeerId = action.replyTo.monoforumPeerId;
	const auto replyTo = action.replyTo.messageId
		? peer->owner().message(action.replyTo.messageId)
		: nullptr;
	const auto topicRootId = draftTopicRootId
		? draftTopicRootId
		: replyTo
		? replyTo->topicRootId()
		: Data::ForumTopic::kGeneralId;
	const auto topic = peer->forumTopicFor(topicRootId);
	if (!(topic ? Data::CanSendTexts(topic) : Data::CanSendTexts(peer))
		|| Api::SendDice(message)) {
		return;
	}
	local().saveRecentSentHashtags(textWithTags.text);

	auto sending = TextWithEntities();
	auto left = TextWithEntities {
		textWithTags.text,
		TextUtilities::ConvertTextTagsToEntities(textWithTags.tags)
	};
	auto prepareFlags = Ui::ItemTextOptions(
		history,
		_session->user()).flags;
	TextUtilities::PrepareForSending(left, prepareFlags);

	HistoryItem *lastMessage = nullptr;

	auto &histories = history->owner().histories();

	const auto exactWebPage = !message.webPage.url.isEmpty();
	auto isFirst = true;
	while (TextUtilities::CutPart(sending, left, MaxMessageSize)
		|| (isFirst && exactWebPage)) {
		TextUtilities::Trim(left);
		const auto isLast = left.empty();

		auto newId = FullMsgId(
			peer->id,
			localMessageId
				? std::exchange(localMessageId, std::nullopt).value()
				: _session->data().nextLocalMessageId());
		auto randomId = base::RandomValue<uint64>();

		TextUtilities::Trim(sending);

		_session->data().registerMessageRandomId(randomId, newId);
		_session->data().registerMessageSentData(
			randomId,
			peer->id,
			sending.text);

		MTPstring msgText(MTP_string(sending.text));
		auto flags = NewMessageFlags(peer);
		auto sendFlags = MTPmessages_SendMessage::Flags(0);
		auto mediaFlags = MTPmessages_SendMedia::Flags(0);
		if (action.replyTo) {
			flags |= MessageFlag::HasReplyInfo;
			sendFlags |= MTPmessages_SendMessage::Flag::f_reply_to;
			mediaFlags |= MTPmessages_SendMedia::Flag::f_reply_to;
		}
		const auto ignoreWebPage = message.webPage.removed
			|| (exactWebPage && !isLast);
		const auto manualWebPage = exactWebPage
			&& !ignoreWebPage
			&& (message.webPage.manual || (isLast && !isFirst));
		MTPMessageMedia media = MTP_messageMediaEmpty();
		if (ignoreWebPage) {
			sendFlags |= MTPmessages_SendMessage::Flag::f_no_webpage;
		} else if (exactWebPage) {
			using PageFlag = MTPDmessageMediaWebPage::Flag;
			using PendingFlag = MTPDwebPagePending::Flag;
			const auto &fields = message.webPage;
			const auto page = _session->data().webpage(fields.id);
			media = MTP_messageMediaWebPage(
				MTP_flags(PageFlag()
					| (manualWebPage ? PageFlag::f_manual : PageFlag())
					| (fields.forceLargeMedia
						? PageFlag::f_force_large_media
						: PageFlag())
					| (fields.forceSmallMedia
						? PageFlag::f_force_small_media
						: PageFlag())),
				MTP_webPagePending(
					MTP_flags(PendingFlag::f_url),
					MTP_long(fields.id),
					MTP_string(fields.url),
					MTP_int(page->pendingTill)));
		}
		const auto silentPost = ShouldSendSilent(peer, action.options);
		FillMessagePostFlags(action, peer, flags);
		if ((exactWebPage && !ignoreWebPage && message.webPage.invert)
			|| action.options.invertCaption) {
			flags |= MessageFlag::InvertMedia;
			sendFlags |= MTPmessages_SendMessage::Flag::f_invert_media;
			mediaFlags |= MTPmessages_SendMedia::Flag::f_invert_media;
		}
		if (silentPost) {
			sendFlags |= MTPmessages_SendMessage::Flag::f_silent;
			mediaFlags |= MTPmessages_SendMedia::Flag::f_silent;
		}
		const auto sentEntities = Api::EntitiesToMTP(
			_session,
			sending.entities,
			Api::ConvertOption::SkipLocal);
		if (!sentEntities.v.isEmpty()) {
			sendFlags |= MTPmessages_SendMessage::Flag::f_entities;
			mediaFlags |= MTPmessages_SendMedia::Flag::f_entities;
		}
		if (clearCloudDraft) {
			sendFlags |= MTPmessages_SendMessage::Flag::f_clear_draft;
			mediaFlags |= MTPmessages_SendMedia::Flag::f_clear_draft;
			history->clearCloudDraft(draftTopicRootId, draftMonoforumPeerId);
			history->startSavingCloudDraft(
				draftTopicRootId,
				draftMonoforumPeerId);
		}
		const auto sendAs = action.options.sendAs;
		if (sendAs) {
			sendFlags |= MTPmessages_SendMessage::Flag::f_send_as;
			mediaFlags |= MTPmessages_SendMedia::Flag::f_send_as;
		}
		if (action.options.scheduled) {
			flags |= MessageFlag::IsOrWasScheduled;
			sendFlags |= MTPmessages_SendMessage::Flag::f_schedule_date;
			mediaFlags |= MTPmessages_SendMedia::Flag::f_schedule_date;
			if (action.options.scheduleRepeatPeriod) {
				sendFlags |= MTPmessages_SendMessage::Flag::f_schedule_repeat_period;
				mediaFlags |= MTPmessages_SendMedia::Flag::f_schedule_repeat_period;
			}
		}
		if (action.options.shortcutId) {
			flags |= MessageFlag::ShortcutMessage;
			sendFlags |= MTPmessages_SendMessage::Flag::f_quick_reply_shortcut;
			mediaFlags |= MTPmessages_SendMedia::Flag::f_quick_reply_shortcut;
		}
		if (action.options.effectId) {
			sendFlags |= MTPmessages_SendMessage::Flag::f_effect;
			mediaFlags |= MTPmessages_SendMedia::Flag::f_effect;
		}
		if (action.options.suggest) {
			sendFlags |= MTPmessages_SendMessage::Flag::f_suggested_post;
			mediaFlags |= MTPmessages_SendMedia::Flag::f_suggested_post;
		}
		const auto starsPaid = std::min(
			peer->starsPerMessageChecked(),
			action.options.starsApproved);
		if (starsPaid) {
			action.options.starsApproved -= starsPaid;
			sendFlags |= MTPmessages_SendMessage::Flag::f_allow_paid_stars;
			mediaFlags |= MTPmessages_SendMedia::Flag::f_allow_paid_stars;
		}
		lastMessage = history->addNewLocalMessage({
			.id = newId.msg,
			.flags = flags,
			.from = NewMessageFromId(action),
			.replyTo = action.replyTo,
			.date = NewMessageDate(action.options),
			.scheduleRepeatPeriod = action.options.scheduleRepeatPeriod,
			.shortcutId = action.options.shortcutId,
			.starsPaid = starsPaid,
			.postAuthor = NewMessagePostAuthor(action),
			.effectId = action.options.effectId,
			.suggest = HistoryMessageSuggestInfo(action.options),
		}, sending, media);
		const auto done = [=](
				const MTPUpdates &result,
				const MTP::Response &response) {
			if (clearCloudDraft) {
				history->finishSavingCloudDraft(
					draftTopicRootId,
					draftMonoforumPeerId,
					Api::UnixtimeFromMsgId(response.outerMsgId));
			}
		};
		const auto fail = [=](
				const MTP::Error &error,
				const MTP::Response &response) {
			if (error.type() == u"MESSAGE_EMPTY"_q) {
				lastMessage->destroy();
			} else {
				sendMessageFail(error, peer, randomId, newId);
			}
			if (clearCloudDraft) {
				history->finishSavingCloudDraft(
					draftTopicRootId,
					draftMonoforumPeerId,
					Api::UnixtimeFromMsgId(response.outerMsgId));
			}
		};
		const auto mtpShortcut = Data::ShortcutIdToMTP(
			_session,
			action.options.shortcutId);
		if (exactWebPage
			&& !ignoreWebPage
			&& (manualWebPage || sending.empty())) {
			histories.sendPreparedMessage(
				history,
				action.replyTo,
				randomId,
				Data::Histories::PrepareMessage<MTPmessages_SendMedia>(
					MTP_flags(mediaFlags),
					peer->input(),
					Data::Histories::ReplyToPlaceholder(),
					Data::WebPageForMTP(message.webPage, true),
					msgText,
					MTP_long(randomId),
					MTPReplyMarkup(),
					sentEntities,
					MTP_int(action.options.scheduled),
					MTP_int(action.options.scheduleRepeatPeriod),
					(sendAs ? sendAs->input() : MTP_inputPeerEmpty()),
					mtpShortcut,
					MTP_long(action.options.effectId),
					MTP_long(starsPaid),
					Api::SuggestToMTP(action.options.suggest)
				), done, fail);
		} else {
			histories.sendPreparedMessage(
				history,
				action.replyTo,
				randomId,
				Data::Histories::PrepareMessage<MTPmessages_SendMessage>(
					MTP_flags(sendFlags),
					peer->input(),
					Data::Histories::ReplyToPlaceholder(),
					msgText,
					MTP_long(randomId),
					MTPReplyMarkup(),
					sentEntities,
					MTP_int(action.options.scheduled),
					MTP_int(action.options.scheduleRepeatPeriod),
					(sendAs ? sendAs->input() : MTP_inputPeerEmpty()),
					mtpShortcut,
					MTP_long(action.options.effectId),
					MTP_long(starsPaid),
					Api::SuggestToMTP(action.options.suggest)
				), done, fail);
		}
		isFirst = false;
	}

	finishForwarding(action);
}

void ApiWrap::sendBotStart(
		std::shared_ptr<Ui::Show> show,
		not_null<UserData*> bot,
		PeerData *chat,
		const QString &startTokenForChat) {
	Expects(bot->isBot());

	if (chat && chat->isChannel() && !chat->isMegagroup()) {
		ShowAddParticipantsError(show, "USER_BOT", chat, bot);
		return;
	}

	auto &info = bot->botInfo;
	const auto token = chat ? startTokenForChat : info->startToken;
	if (token.isEmpty()) {
		auto message = MessageToSend(
			Api::SendAction(_session->data().history(chat
				? chat
				: bot.get())));
		message.textWithTags = { u"/start"_q, TextWithTags::Tags() };
		if (chat) {
			message.textWithTags.text += '@' + bot->username();
		}
		sendMessage(std::move(message));
		return;
	}
	const auto randomId = base::RandomValue<uint64>();
	if (!chat) {
		info->startToken = QString();
	}
	request(MTPmessages_StartBot(
		bot->inputUser(),
		chat ? chat->input() : MTP_inputPeerEmpty(),
		MTP_long(randomId),
		MTP_string(token)
	)).done([=](const MTPUpdates &result) {
		applyUpdates(result);
	}).fail([=](const MTP::Error &error) {
		if (chat) {
			const auto type = error.type();
			ShowAddParticipantsError(show, type, chat, bot);
		}
	}).send();
}

void ApiWrap::sendInlineResult(
		not_null<UserData*> bot,
		not_null<InlineBots::Result*> data,
		SendAction action,
		std::optional<MsgId> localMessageId,
		Fn<void(bool)> done) {
	sendAction(action);

	const auto history = action.history;
	const auto peer = history->peer;
	const auto newId = FullMsgId(
		peer->id,
		localMessageId
			? (*localMessageId)
			: _session->data().nextLocalMessageId());
	const auto randomId = base::RandomValue<uint64>();
	const auto topicRootId = action.replyTo.messageId
		? action.replyTo.topicRootId
		: 0;
	const auto monoforumPeerId = action.replyTo.monoforumPeerId;

	using SendFlag = MTPmessages_SendInlineBotResult::Flag;
	auto flags = NewMessageFlags(peer);
	auto sendFlags = SendFlag::f_clear_draft | SendFlag();
	if (action.replyTo) {
		flags |= MessageFlag::HasReplyInfo;
		sendFlags |= SendFlag::f_reply_to;
	}
	const auto silentPost = ShouldSendSilent(peer, action.options);
	FillMessagePostFlags(action, peer, flags);
	if (silentPost) {
		sendFlags |= SendFlag::f_silent;
	}
	if (action.options.scheduled) {
		flags |= MessageFlag::IsOrWasScheduled;
		sendFlags |= SendFlag::f_schedule_date;
	}
	if (action.options.shortcutId) {
		flags |= MessageFlag::ShortcutMessage;
		sendFlags |= SendFlag::f_quick_reply_shortcut;
	}
	if (action.options.hideViaBot) {
		sendFlags |= SendFlag::f_hide_via;
	}
	const auto starsPaid = std::min(
		peer->starsPerMessageChecked(),
		action.options.starsApproved);
	if (starsPaid) {
		action.options.starsApproved -= starsPaid;
		sendFlags |= SendFlag::f_allow_paid_stars;
	}

	const auto sendAs = action.options.sendAs;
	if (sendAs) {
		sendFlags |= MTPmessages_SendInlineBotResult::Flag::f_send_as;
	}
	_session->data().registerMessageRandomId(randomId, newId);

	data->addToHistory(history, {
		.id = newId.msg,
		.flags = flags,
		.from = NewMessageFromId(action),
		.replyTo = action.replyTo,
		.date = NewMessageDate(action.options),
		.shortcutId = action.options.shortcutId,
		.starsPaid = starsPaid,
		.viaBotId = ((bot && !action.options.hideViaBot)
			? peerToUser(bot->id)
			: UserId()),
		.postAuthor = NewMessagePostAuthor(action),
	});

	history->clearCloudDraft(topicRootId, monoforumPeerId);
	history->startSavingCloudDraft(topicRootId, monoforumPeerId);

	auto &histories = history->owner().histories();
	histories.sendPreparedMessage(
		history,
		action.replyTo,
		randomId,
		Data::Histories::PrepareMessage<MTPmessages_SendInlineBotResult>(
			MTP_flags(sendFlags),
			peer->input(),
			Data::Histories::ReplyToPlaceholder(),
			MTP_long(randomId),
			MTP_long(data->getQueryId()),
			MTP_string(data->getId()),
			MTP_int(action.options.scheduled),
			(sendAs ? sendAs->input() : MTP_inputPeerEmpty()),
			Data::ShortcutIdToMTP(_session, action.options.shortcutId),
			MTP_long(starsPaid)
		), [=](const MTPUpdates &result, const MTP::Response &response) {
		history->finishSavingCloudDraft(
			topicRootId,
			monoforumPeerId,
			Api::UnixtimeFromMsgId(response.outerMsgId));
		if (done) {
			done(true);
		}
	}, [=](const MTP::Error &error, const MTP::Response &response) {
		sendMessageFail(error, peer, randomId, newId);
		history->finishSavingCloudDraft(
			topicRootId,
			monoforumPeerId,
			Api::UnixtimeFromMsgId(response.outerMsgId));
		if (done) {
			done(false);
		}
	});
	finishForwarding(action);
}

void ApiWrap::uploadAlbumMedia(
		not_null<HistoryItem*> item,
		const MessageGroupId &groupId,
		const MTPInputMedia &media) {
	const auto localId = item->fullId();
	const auto failed = [=] {

	};
	request(MTPmessages_UploadMedia(
		MTP_flags(0),
		MTPstring(), // business_connection_id
		item->history()->peer->input(),
		media
	)).done([=](const MTPMessageMedia &result) {
		const auto item = _session->data().message(localId);
		if (!item) {
			failed();
			return;
		}
		auto spoiler = false;
		if (const auto media = item->media()) {
			spoiler = media->hasSpoiler();
			if (const auto photo = media->photo()) {
				photo->setWaitingForAlbum();
			} else if (const auto document = media->document()) {
				document->setWaitingForAlbum();
			}
		}

		switch (result.type()) {
		case mtpc_messageMediaPhoto: {
			const auto &data = result.c_messageMediaPhoto();
			const auto photo = data.vphoto();
			if (!photo || photo->type() != mtpc_photo) {
				failed();
				return;
			}
			const auto &fields = photo->c_photo();
			using Flag = MTPDinputMediaPhoto::Flag;
			const auto flags = Flag()
				| (data.vttl_seconds() ? Flag::f_ttl_seconds : Flag())
				| (spoiler ? Flag::f_spoiler : Flag());
			const auto media = MTP_inputMediaPhoto(
				MTP_flags(flags),
				MTP_inputPhoto(
					fields.vid(),
					fields.vaccess_hash(),
					fields.vfile_reference()),
				MTP_int(data.vttl_seconds().value_or_empty()),
				MTPInputDocument()); // video
			sendAlbumWithUploaded(item, groupId, media);
		} break;

		case mtpc_messageMediaDocument: {
			const auto &data = result.c_messageMediaDocument();
			const auto document = data.vdocument();
			if (!document || document->type() != mtpc_document) {
				failed();
				return;
			}
			const auto &fields = document->c_document();
			const auto mtpCover = data.vvideo_cover();
			const auto cover = (mtpCover && mtpCover->type() == mtpc_photo)
				? &(mtpCover->c_photo())
				: (const MTPDphoto*)nullptr;
			using Flag = MTPDinputMediaDocument::Flag;
			const auto flags = Flag()
				| (data.vttl_seconds() ? Flag::f_ttl_seconds : Flag())
				| (spoiler ? Flag::f_spoiler : Flag())
				| (data.vvideo_timestamp() ? Flag::f_video_timestamp : Flag())
				| (cover ? Flag::f_video_cover : Flag());
			const auto media = MTP_inputMediaDocument(
				MTP_flags(flags),
				MTP_inputDocument(
					fields.vid(),
					fields.vaccess_hash(),
					fields.vfile_reference()),
				(cover
					? MTP_inputPhoto(
						cover->vid(),
						cover->vaccess_hash(),
						cover->vfile_reference())
					: MTPInputPhoto()),
				MTP_int(data.vvideo_timestamp().value_or_empty()),
				MTP_int(data.vttl_seconds().value_or_empty()),
				MTPstring()); // query
			sendAlbumWithUploaded(item, groupId, media);
		} break;
		}
	}).fail([=] {
		failed();
	}).send();
}

void ApiWrap::sendMedia(
		not_null<HistoryItem*> item,
		const MTPInputMedia &media,
		Api::SendOptions options,
		Fn<void(bool)> done) {
	const auto randomId = base::RandomValue<uint64>();
	_session->data().registerMessageRandomId(randomId, item->fullId());

	sendMediaWithRandomId(item, media, options, randomId, std::move(done));
}

void ApiWrap::sendMediaWithRandomId(
		not_null<HistoryItem*> item,
		const MTPInputMedia &media,
		Api::SendOptions options,
		uint64 randomId,
		Fn<void(bool)> done) {
	const auto history = item->history();
	const auto replyTo = item->replyTo();
	const auto peer = history->peer;

	auto caption = item->originalText();
	TextUtilities::Trim(caption);
	auto sentEntities = Api::EntitiesToMTP(
		_session,
		caption.entities,
		Api::ConvertOption::SkipLocal);

	const auto updateRecentStickers = Api::HasAttachedStickers(media);
	const auto starsPaid = std::min(
		peer->starsPerMessageChecked(),
		options.starsApproved);
	if (starsPaid) {
		options.starsApproved -= starsPaid;
	}

	using Flag = MTPmessages_SendMedia::Flag;
	const auto flags = Flag(0)
		| (replyTo ? Flag::f_reply_to : Flag(0))
		| (ShouldSendSilent(history->peer, options)
			? Flag::f_silent
			: Flag(0))
		| (!sentEntities.v.isEmpty() ? Flag::f_entities : Flag(0))
		| (options.scheduled ? Flag::f_schedule_date : Flag(0))
		| ((options.scheduled && options.scheduleRepeatPeriod)
			? Flag::f_schedule_repeat_period
			: Flag(0))
		| (options.sendAs ? Flag::f_send_as : Flag(0))
		| (options.shortcutId ? Flag::f_quick_reply_shortcut : Flag(0))
		| (options.effectId ? Flag::f_effect : Flag(0))
		| (options.suggest ? Flag::f_suggested_post : Flag(0))
		| (options.invertCaption ? Flag::f_invert_media : Flag(0))
		| (starsPaid ? Flag::f_allow_paid_stars : Flag(0));

	auto &histories = history->owner().histories();
	const auto itemId = item->fullId();
	histories.sendPreparedMessage(
		history,
		replyTo,
		randomId,
		Data::Histories::PrepareMessage<MTPmessages_SendMedia>(
			MTP_flags(flags),
			peer->input(),
			Data::Histories::ReplyToPlaceholder(),
			(options.price
				? MTPInputMedia(MTP_inputMediaPaidMedia(
					MTP_flags(0),
					MTP_long(options.price),
					MTP_vector<MTPInputMedia>(1, media),
					MTPstring()))
				: media),
			MTP_string(caption.text),
			MTP_long(randomId),
			MTPReplyMarkup(),
			sentEntities,
			MTP_int(options.scheduled),
			MTP_int(options.scheduleRepeatPeriod),
			(options.sendAs ? options.sendAs->input() : MTP_inputPeerEmpty()),
			Data::ShortcutIdToMTP(_session, options.shortcutId),
			MTP_long(options.effectId),
			MTP_long(starsPaid),
			Api::SuggestToMTP(options.suggest)
		), [=](const MTPUpdates &result, const MTP::Response &response) {
		if (done) done(true);
		if (updateRecentStickers) {
			requestRecentStickers(std::nullopt, true);
		}
	}, [=](const MTP::Error &error, const MTP::Response &response) {
		if (done) done(false);
		sendMessageFail(error, peer, randomId, itemId);
	});
}

void ApiWrap::sendMultiPaidMedia(
		not_null<HistoryItem*> item,
		not_null<SendingAlbum*> album,
		Fn<void(bool)> done) {
	Expects(album->options.price > 0);

	const auto groupId = album->groupId;
	auto &options = album->options;
	const auto randomId = album->items.front().randomId;
	auto medias = album->items | ranges::view::transform([](
			const SendingAlbum::Item &part) {
		Assert(part.media.has_value());
		return MTPInputMedia(part.media->data().vmedia());
	}) | ranges::to<QVector<MTPInputMedia>>();

	const auto history = item->history();
	const auto replyTo = item->replyTo();
	const auto peer = history->peer;

	auto caption = item->originalText();
	TextUtilities::Trim(caption);
	auto sentEntities = Api::EntitiesToMTP(
		_session,
		caption.entities,
		Api::ConvertOption::SkipLocal);
	const auto starsPaid = std::min(
		peer->starsPerMessageChecked(),
		options.starsApproved);
	if (starsPaid) {
		options.starsApproved -= starsPaid;
	}

	using Flag = MTPmessages_SendMedia::Flag;
	const auto flags = Flag(0)
		| (replyTo ? Flag::f_reply_to : Flag(0))
		| (ShouldSendSilent(history->peer, options)
			? Flag::f_silent
			: Flag(0))
		| (!sentEntities.v.isEmpty() ? Flag::f_entities : Flag(0))
		| (options.scheduled ? Flag::f_schedule_date : Flag(0))
		| (options.scheduleRepeatPeriod
			? Flag::f_schedule_repeat_period
			: Flag(0))
		| (options.sendAs ? Flag::f_send_as : Flag(0))
		| (options.shortcutId ? Flag::f_quick_reply_shortcut : Flag(0))
		| (options.effectId ? Flag::f_effect : Flag(0))
		| (options.suggest ? Flag::f_suggested_post : Flag(0))
		| (options.invertCaption ? Flag::f_invert_media : Flag(0))
		| (starsPaid ? Flag::f_allow_paid_stars : Flag(0));

	auto &histories = history->owner().histories();
	const auto itemId = item->fullId();
	album->sent = true;
	histories.sendPreparedMessage(
		history,
		replyTo,
		randomId,
		Data::Histories::PrepareMessage<MTPmessages_SendMedia>(
			MTP_flags(flags),
			peer->input(),
			Data::Histories::ReplyToPlaceholder(),
			MTP_inputMediaPaidMedia(
				MTP_flags(0),
				MTP_long(options.price),
				MTP_vector<MTPInputMedia>(std::move(medias)),
				MTPstring()),
			MTP_string(caption.text),
			MTP_long(randomId),
			MTPReplyMarkup(),
			sentEntities,
			MTP_int(options.scheduled),
			MTP_int(options.scheduleRepeatPeriod),
			(options.sendAs ? options.sendAs->input() : MTP_inputPeerEmpty()),
			Data::ShortcutIdToMTP(_session, options.shortcutId),
			MTP_long(options.effectId),
			MTP_long(starsPaid),
			Api::SuggestToMTP(options.suggest)
		), [=](const MTPUpdates &result, const MTP::Response &response) {
		if (const auto album = _sendingAlbums.take(groupId)) {
			const auto copy = (*album)->items;
			for (const auto &part : copy) {
				if (const auto item = history->owner().message(part.msgId)) {
					item->destroy();
				}
			}
		}
		if (done) done(true);
	}, [=](const MTP::Error &error, const MTP::Response &response) {
		if (done) done(false);
		sendMessageFail(error, peer, randomId, itemId);
	});
}

void ApiWrap::sendAlbumWithUploaded(
		not_null<HistoryItem*> item,
		const MessageGroupId &groupId,
		const MTPInputMedia &media) {
	LOG(("sendAlbumWithUploaded: item=%1, groupId=%2"
		).arg(item->id.bare
		).arg(groupId.value));
	const auto localId = item->fullId();
	const auto randomId = base::RandomValue<uint64>();
	_session->data().registerMessageRandomId(randomId, localId);

	const auto albumIt = _sendingAlbums.find(groupId.raw());
	Assert(albumIt != _sendingAlbums.end());
	const auto &album = albumIt->second;
	LOG(("sendAlbumWithUploaded: filling media for album, items=%1"
		).arg(album->items.size()));
	album->fillMedia(item, media, randomId);
	sendAlbumIfReady(album.get());
}

void ApiWrap::sendAlbumWithCancelled(
		not_null<HistoryItem*> item,
		const MessageGroupId &groupId) {
	const auto albumIt = _sendingAlbums.find(groupId.raw());
	if (albumIt == _sendingAlbums.end()) {
		// Sometimes we destroy item being sent already after the album
		// was sent successfully. For example the message could be loaded
		// from server (by messages.getHistory or updateNewMessage) and
		// added to history and after that updateMessageID was received with
		// the same message id, in this case we destroy a detached local
		// item and sendAlbumWithCancelled is called for already sent album.
		return;
	}
	const auto &album = albumIt->second;
	album->removeItem(item);
	sendAlbumIfReady(album.get());
}

void ApiWrap::sendAlbumIfReady(not_null<SendingAlbum*> album) {
	LOG(("sendAlbumIfReady: album=%1, items=%2, expected=%3, sent=%4"
		).arg(album->groupId
		).arg(album->items.size()
		).arg(album->expectedCount
		).arg(album->sent ? 1 : 0));
	if (album->sent) {
		LOG(("sendAlbumIfReady: album already sent, returning"));
		return;
	}
	const auto groupId = album->groupId;
	if (album->items.empty()) {
		LOG(("sendAlbumIfReady: album items empty, removing"));
		_sendingAlbums.remove(groupId);
		return;
	}
	auto sample = (HistoryItem*)nullptr;
	auto medias = QVector<MTPInputSingleMedia>();
	medias.reserve(album->items.size());
	for (const auto &item : album->items) {
		if (!item.media) {
			LOG(("sendAlbumIfReady: item %1 has no media, waiting").arg(item.msgId.msg.bare));
			return;
		} else if (!sample) {
			sample = _session->data().message(item.msgId);
		}
		medias.push_back(*item.media);
	}
	if (album->items.size() != album->expectedCount) {
		LOG(("sendAlbumIfReady: waiting for more items, have=%1, expected=%2"
			).arg(album->items.size()
			).arg(album->expectedCount));
		return;
	}
	LOG(("sendAlbumIfReady: all items ready, sending album with %1 items").arg(medias.size()));
	if (!sample) {
		_sendingAlbums.remove(groupId);
		return;
	} else if (album->options.price > 0) {
		sendMultiPaidMedia(sample, album);
		return;
	} else if (medias.size() < 2) {
		const auto &single = medias.front().data();
		album->sent = true;
		const auto historyPeer = sample->history()->peer;
		const auto albumSample = sample;
		sendMediaWithRandomId(
			albumSample,
			single.vmedia(),
			album->options,
			single.vrandom_id().v,
			[=](bool) {
				_session->data().groups().refreshMessage(
					albumSample);
				EnhancedForward::markItemSent(
					&session(),
					historyPeer->id);
			});
		_sendingAlbums.remove(groupId);
		return;
	}
	const auto history = sample->history();
	const auto replyTo = sample->replyTo();
	const auto sendAs = album->options.sendAs;
	const auto starsPaid = std::min(
		history->peer->starsPerMessageChecked() * int(medias.size()),
		album->options.starsApproved);
	if (starsPaid) {
		album->options.starsApproved -= starsPaid;
	}
	using Flag = MTPmessages_SendMultiMedia::Flag;
	const auto flags = Flag(0)
		| (replyTo ? Flag::f_reply_to : Flag(0))
		| (ShouldSendSilent(history->peer, album->options)
			? Flag::f_silent
			: Flag(0))
		| (album->options.scheduled ? Flag::f_schedule_date : Flag(0))
		//| (album->options.scheduleRepeatPeriod
		//	? Flag::f_schedule_repeat_period
		//	: Flag(0))
		| (sendAs ? Flag::f_send_as : Flag(0))
		| (album->options.shortcutId
			? Flag::f_quick_reply_shortcut
			: Flag(0))
		| (album->options.effectId ? Flag::f_effect : Flag(0))
		| (album->options.invertCaption ? Flag::f_invert_media : Flag(0))
		| (starsPaid ? Flag::f_allow_paid_stars : Flag(0));
	auto &histories = history->owner().histories();
	const auto peer = history->peer;
	album->sent = true;
	// Capture message pointers before sending (setRealId changes ids).
	auto albumMsgs = std::make_shared<std::vector<
		not_null<HistoryItem*>>>();
	for (const auto &item : album->items) {
		if (const auto msg = _session->data().message(
				item.msgId)) {
			albumMsgs->push_back(msg);
		}
	}
	histories.sendPreparedMessage(
		history,
		replyTo,
		uint64(0), // randomId
		Data::Histories::PrepareMessage<MTPmessages_SendMultiMedia>(
			MTP_flags(flags),
			peer->input(),
			Data::Histories::ReplyToPlaceholder(),
			MTP_vector<MTPInputSingleMedia>(medias),
			MTP_int(album->options.scheduled),
			//MTP_int(album->options.scheduleRepeatPeriod),
			(sendAs ? sendAs->input() : MTP_inputPeerEmpty()),
			Data::ShortcutIdToMTP(_session, album->options.shortcutId),
			MTP_long(album->options.effectId),
			MTP_long(starsPaid)
		), [=](const MTPUpdates &result, const MTP::Response &response) {
		for (const auto &msg : *albumMsgs) {
			_session->data().groups().refreshMessage(msg);
			EnhancedForward::markItemSent(&session(), peer->id);
		}
		_sendingAlbums.remove(groupId);
	}, [=](const MTP::Error &error,
			const MTP::Response &response) {
		if (const auto album = _sendingAlbums.take(groupId)) {
			for (const auto &item : (*album)->items) {
				sendMessageFail(error, peer, item.randomId, item.msgId);
			}
		} else {
			sendMessageFail(error, peer);
		}
	});
}

void ApiWrap::reloadContactSignupSilent() {
	if (_contactSignupSilentRequestId) {
		return;
	}
	const auto requestId = request(MTPaccount_GetContactSignUpNotification(
	)).done([=](const MTPBool &result) {
		_contactSignupSilentRequestId = 0;
		const auto silent = mtpIsTrue(result);
		_contactSignupSilent = silent;
		_contactSignupSilentChanges.fire_copy(silent);
	}).fail([=] {
		_contactSignupSilentRequestId = 0;
	}).send();
	_contactSignupSilentRequestId = requestId;
}

rpl::producer<bool> ApiWrap::contactSignupSilent() const {
	return _contactSignupSilent
		? _contactSignupSilentChanges.events_starting_with_copy(
			*_contactSignupSilent)
		: (_contactSignupSilentChanges.events() | rpl::type_erased);
}

std::optional<bool> ApiWrap::contactSignupSilentCurrent() const {
	return _contactSignupSilent;
}

void ApiWrap::saveContactSignupSilent(bool silent) {
	request(base::take(_contactSignupSilentRequestId)).cancel();

	const auto requestId = request(MTPaccount_SetContactSignUpNotification(
		MTP_bool(silent)
	)).done([=] {
		_contactSignupSilentRequestId = 0;
		_contactSignupSilent = silent;
		_contactSignupSilentChanges.fire_copy(silent);
	}).fail([=] {
		_contactSignupSilentRequestId = 0;
	}).send();
	_contactSignupSilentRequestId = requestId;
}

auto ApiWrap::botCommonGroups(not_null<UserData*> bot) const
-> std::optional<std::vector<not_null<PeerData*>>> {
	const auto i = _botCommonGroups.find(bot);
	return (i != end(_botCommonGroups))
		? i->second
		: std::optional<std::vector<not_null<PeerData*>>>();
}

void ApiWrap::requestBotCommonGroups(
		not_null<UserData*> bot,
		Fn<void()> done) {
	if (_botCommonGroupsRequests.contains(bot)) {
		return;
	}
	_botCommonGroupsRequests.emplace(bot, done);
	const auto finish = [=](std::vector<not_null<PeerData*>> list) {
		_botCommonGroups.emplace(bot, std::move(list));
		if (const auto callback = _botCommonGroupsRequests.take(bot)) {
			(*callback)();
		}
	};
	const auto limit = 100;
	request(MTPmessages_GetCommonChats(
		bot->inputUser(),
		MTP_long(0), // max_id
		MTP_int(limit)
	)).done([=](const MTPmessages_Chats &result) {
		const auto chats = result.match([](const auto &data) {
			return &data.vchats().v;
		});
		auto &owner = session().data();
		auto list = std::vector<not_null<PeerData*>>();
		list.reserve(chats->size());
		for (const auto &chat : *chats) {
			if (const auto peer = owner.processChat(chat)) {
				list.push_back(peer);
			}
		}
		finish(std::move(list));
	}).fail([=] {
		finish({});
	}).send();
}

void ApiWrap::saveSelfBio(const QString &text) {
	if (_bio.requestId) {
		if (text != _bio.requestedText) {
			request(_bio.requestId).cancel();
		} else {
			return;
		}
	}
	_bio.requestedText = text;
	_bio.requestId = request(MTPaccount_UpdateProfile(
		MTP_flags(MTPaccount_UpdateProfile::Flag::f_about),
		MTPstring(),
		MTPstring(),
		MTP_string(text)
	)).done([=](const MTPUser &result) {
		_bio.requestId = 0;

		_session->data().processUser(result);
		_session->user()->setAbout(_bio.requestedText);
	}).fail([=] {
		_bio.requestId = 0;
	}).send();
}

void ApiWrap::registerStatsRequest(MTP::DcId dcId, mtpRequestId id) {
	_statsRequests[dcId].emplace(id);
}

void ApiWrap::unregisterStatsRequest(MTP::DcId dcId, mtpRequestId id) {
	const auto i = _statsRequests.find(dcId);
	Assert(i != end(_statsRequests));
	const auto removed = i->second.remove(id);
	Assert(removed);
	if (i->second.empty()) {
		_statsSessionKillTimer.callOnce(kStatsSessionKillTimeout);
	}
}

void ApiWrap::checkStatsSessions() {
	for (auto i = begin(_statsRequests); i != end(_statsRequests);) {
		if (i->second.empty()) {
			instance().killSession(
				MTP::ShiftDcId(i->first, MTP::kStatsDcShift));
			i = _statsRequests.erase(i);
		} else {
			++i;
		}
	}
}

Api::Authorizations &ApiWrap::authorizations() {
	return *_authorizations;
}

Api::AttachedStickers &ApiWrap::attachedStickers() {
	return *_attachedStickers;
}

Api::BlockedPeers &ApiWrap::blockedPeers() {
	return *_blockedPeers;
}

Api::CloudPassword &ApiWrap::cloudPassword() {
	return *_cloudPassword;
}

Api::SelfDestruct &ApiWrap::selfDestruct() {
	return *_selfDestruct;
}

Api::SensitiveContent &ApiWrap::sensitiveContent() {
	return *_sensitiveContent;
}

Api::GlobalPrivacy &ApiWrap::globalPrivacy() {
	return *_globalPrivacy;
}

Api::ReactionsNotifySettings &ApiWrap::reactionsNotifySettings() {
	return *_reactionsNotifySettings;
}

Api::UserPrivacy &ApiWrap::userPrivacy() {
	return *_userPrivacy;
}

Api::InviteLinks &ApiWrap::inviteLinks() {
	return *_inviteLinks;
}

Api::ChatLinks &ApiWrap::chatLinks() {
	return *_chatLinks;
}

Api::ViewsManager &ApiWrap::views() {
	return *_views;
}

Api::ReadMetrics &ApiWrap::readMetrics() {
	return *_readMetrics;
}

Api::ConfirmPhone &ApiWrap::confirmPhone() {
	return *_confirmPhone;
}

Api::PeerPhoto &ApiWrap::peerPhoto() {
	return *_peerPhoto;
}

Api::Polls &ApiWrap::polls() {
	return *_polls;
}

Api::TodoLists &ApiWrap::todoLists() {
	return *_todoLists;
}

Api::ChatParticipants &ApiWrap::chatParticipants() {
	return *_chatParticipants;
}

Api::UnreadThings &ApiWrap::unreadThings() {
	return *_unreadThings;
}

Api::Ringtones &ApiWrap::ringtones() {
	return *_ringtones;
}

Api::ComposeWithAi &ApiWrap::composeWithAi() {
	return *_composeWithAi;
}

Api::Transcribes &ApiWrap::transcribes() {
	return *_transcribes;
}

Api::Premium &ApiWrap::premium() {
	return *_premium;
}

Api::Usernames &ApiWrap::usernames() {
	return *_usernames;
}

Api::Websites &ApiWrap::websites() {
	return *_websites;
}

Api::PeerColors &ApiWrap::peerColors() {
	return *_peerColors;
}
