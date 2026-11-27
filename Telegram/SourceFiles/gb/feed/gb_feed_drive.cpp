/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/feed/gb_feed_drive.h"

#include "api/api_chat_invite.h"
#include "api/api_common.h"
#include "api/api_invite_links.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "base/timer.h"
#include "core/application.h"
#include "data/data_channel.h"
#include "data/data_document.h"
#include "data/data_download_manager.h"
#include "data/data_file_origin.h"
#include "data/data_histories.h"
#include "data/data_media_types.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/notify/data_notify_settings.h"
#include "gb/downloader/gb_dl_settings.h"
#include "gb/feed/gb_feed_api.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "storage/localimageloader.h"
#include "storage/storage_media_prepare.h"
#include "ui/chat/attach/attach_prepare.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>

namespace GB::Feed {
namespace {

constexpr auto kDriveTitle = "G&B Drive";
constexpr auto kPollDelay = crl::time(1500);
constexpr auto kPollAttempts = 40;
constexpr auto kFetchTimeout = crl::time(60 * 1000);

using Done = Fn<void()>;
using Fail = Fn<void(QString)>;

// Bot API ids of channels are -100<bare id>.
[[nodiscard]] ChannelId ChannelFromBotApi(int64 chatId) {
	return ChannelId(BareId(-chatId - 1000000000000LL));
}

[[nodiscard]] int64 BotApiFromChannel(not_null<ChannelData*> channel) {
	return -1000000000000LL - int64(peerToChannel(channel->id).bare);
}

void ResolveBot(
		not_null<Main::Session*> session,
		const QString &name,
		Fn<void(not_null<UserData*>)> done,
		Fail fail) {
	if (const auto peer = session->data().peerByUsername(name)) {
		if (const auto user = peer->asUser()) {
			done(user);
			return;
		}
	}
	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(name),
		MTP_string()
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		session->data().processUsers(result.data().vusers());
		session->data().processChats(result.data().vchats());
		const auto peer = session->data().peerLoaded(
			peerFromMTP(result.data().vpeer()));
		if (const auto user = peer ? peer->asUser() : nullptr) {
			done(user);
		} else {
			fail(u"bot_not_found"_q);
		}
	}).fail([=](const MTP::Error &error) {
		fail(u"bot_not_found"_q);
	}).send();
}

// Keeps a GB service chat out of the way: archived and muted forever.
void Tuck(not_null<PeerData*> peer) {
	const auto session = &peer->session();
	session->data().notifySettings().update(
		peer,
		Data::MuteValue{ .forever = true });
	const auto history = session->data().history(peer);
	if (!history->folder()) {
		session->api().toggleHistoryArchived(history, true, [] {});
	}
}

// Waits until the backend reports the drive as ready (the bot's
// my_chat_member update arrived).
void WaitDriveReady(
		not_null<ChannelData*> channel,
		int attempt,
		Done done,
		Fail fail) {
	if (attempt >= kPollAttempts) {
		fail(u"drive_timeout"_q);
		return;
	}
	Api::Instance().get(u"/v1/drive"_q, [=](const QJsonObject &r) {
		const auto drive = r.value(u"drive"_q).toObject();
		const auto chatId = drive.value(u"chat_id"_q).toVariant().toLongLong();
		if (drive.value(u"ready"_q).toBool()
			&& chatId == BotApiFromChannel(channel)) {
			done();
		} else {
			base::call_delayed(kPollDelay, [=] {
				WaitDriveReady(channel, attempt + 1, done, fail);
			});
		}
	}, fail);
}

void MakeBotAdmin(
		not_null<ChannelData*> channel,
		const QString &botName,
		Done done,
		Fail fail) {
	const auto session = &channel->session();
	ResolveBot(session, botName, [=](not_null<UserData*> bot) {
		// The bot only has to read the channel; give it the most harmless
		// right (Telegram needs at least one for an admin).
		using Flag = MTPchannels_editAdmin::Flag;
		session->api().request(MTPchannels_EditAdmin(
			MTP_flags(Flag::f_rank),
			channel->inputChannel(),
			bot->inputUser(),
			MTP_chatAdminRights(
				MTP_flags(MTPDchatAdminRights::Flag::f_manage_call)),
			MTP_string(QString())
		)).done([=](const MTPUpdates &result) {
			session->api().applyUpdates(result);
			WaitDriveReady(channel, 0, done, fail);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	}, fail);
}

void CreateDrive(
		not_null<Main::Session*> session,
		const QString &botName,
		Fn<void(not_null<ChannelData*>)> done,
		Fail fail) {
	using Flag = MTPchannels_CreateChannel::Flag;
	session->api().request(MTPchannels_CreateChannel(
		MTP_flags(Flag::f_broadcast),
		MTP_string(QString::fromUtf8(kDriveTitle)),
		MTP_string(u"Files you share on G&B Community live here, in your "
			"own Telegram. Deleting a file here removes it from the Feed. "
			"Make Internet Free Again."_q),
		MTPInputGeoPoint(),
		MTPstring(),
		MTP_int(0)
	)).done([=](const MTPUpdates &result) {
		session->api().applyUpdates(result);
		auto channel = (ChannelData*)nullptr;
		const auto find = [&](const QVector<MTPChat> &chats) {
			for (const auto &chat : chats) {
				if (chat.type() == mtpc_channel) {
					channel = session->data().channel(
						chat.c_channel().vid());
					return;
				}
			}
		};
		result.match([&](const MTPDupdates &data) {
			find(data.vchats().v);
		}, [&](const MTPDupdatesCombined &data) {
			find(data.vchats().v);
		}, [](const auto &) {});
		if (!channel) {
			fail(u"drive_create_failed"_q);
			return;
		}
		const auto created = not_null<ChannelData*>(channel);
		Tuck(created);
		MakeBotAdmin(created, botName, [=] { done(created); }, fail);
	}).fail([=](const MTP::Error &error) {
		fail(error.type());
	}).send();
}

void EnsureDrive(
		not_null<Window::SessionController*> controller,
		Fn<void(not_null<ChannelData*>)> done,
		Fail fail) {
	const auto session = &controller->session();
	Api::Instance().get(u"/v1/drive"_q, [=](const QJsonObject &r) {
		const auto bot = r.value(u"bot"_q).toString();
		const auto drive = r.value(u"drive"_q).toObject();
		const auto chatId = drive.value(u"chat_id"_q).toVariant().toLongLong();
		const auto channel = chatId
			? session->data().channelLoaded(ChannelFromBotApi(chatId))
			: nullptr;
		if (channel && channel->amCreator()) {
			if (drive.value(u"ready"_q).toBool()) {
				done(channel);
			} else {
				// The bot was removed: make it an admin again.
				MakeBotAdmin(channel, bot, [=] { done(channel); }, fail);
			}
			return;
		}
		CreateDrive(session, bot, done, fail);
	}, fail);
}

void WaitCloudFile(MsgId messageId, int attempt, Fn<void(QJsonObject)> done, Fail fail) {
	if (attempt >= kPollAttempts) {
		fail(u"drive_timeout"_q);
		return;
	}
	const auto path = u"/v1/drive/files?message_id=%1"_q.arg(messageId.bare);
	Api::Instance().get(path, [=](const QJsonObject &r) {
		const auto files = r.value(u"files"_q).toArray();
		if (!files.isEmpty()) {
			done(files.first().toObject());
		} else {
			base::call_delayed(kPollDelay, [=] {
				WaitCloudFile(messageId, attempt + 1, done, fail);
			});
		}
	}, fail);
}

[[nodiscard]] QString UniquePath(const QString &folder, const QString &name) {
	QDir().mkpath(folder);
	const auto info = QFileInfo(name);
	const auto base = info.completeBaseName().isEmpty()
		? u"file"_q
		: info.completeBaseName();
	const auto suffix = info.suffix().isEmpty()
		? QString()
		: ('.' + info.suffix());
	auto result = QDir(folder).filePath(base + suffix);
	for (auto i = 2; QFileInfo::exists(result); ++i) {
		result = QDir(folder).filePath(u"%1 (%2)%3"_q.arg(base).arg(i).arg(suffix));
	}
	return result;
}

[[nodiscard]] QString RandomTag() {
	const auto alphabet = u"abcdefghijklmnopqrstuvwxyz0123456789"_q;
	auto result = QString();
	for (auto i = 0; i != 20; ++i) {
		result += alphabet[base::RandomIndex(alphabet.size())];
	}
	return result;
}

void SaveItemDocument(
		base::weak_ptr<Window::SessionController> weak,
		not_null<HistoryItem*> item,
		const QString &name) {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document) {
		if (const auto strong = weak.get()) {
			strong->showToast(u"This file is not available."_q);
		}
		return;
	}
	const auto path = UniquePath(
		Downloader::DefaultDownloadDir(),
		name.isEmpty() ? document->filename() : name);
	document->save(Data::FileOrigin(item->fullId()), path);
	Core::App().downloadManager().addLoading({
		.item = item,
		.document = document,
	});
	if (const auto strong = weak.get()) {
		strong->showToast(u"Downloading \"%1\"..."_q.arg(name));
	}
}

void JoinInvite(
		not_null<Main::Session*> session,
		const QString &link,
		Done done) {
	const auto hash = link.mid(link.indexOf('+') + 1);
	if (hash.isEmpty()) {
		return;
	}
	session->api().request(MTPmessages_ImportChatInvite(
		MTP_string(hash)
	)).done([=](const MTPmessages_ChatInviteJoinResult &result) {
		::Api::ProcessChatInviteJoinResult(
			session,
			nullptr,
			result,
			[=](const MTPUpdates &updates) {
				session->api().applyUpdates(updates);
				if (done) {
					done();
				}
			});
	}).fail([=](const MTP::Error &error) {
		// USER_ALREADY_PARTICIPANT and the like: nothing to do.
		if (done) {
			done();
		}
	}).send();
}

void DownloadFromLibrary(
		not_null<Window::SessionController*> controller,
		int64 chatId,
		MsgId msgId,
		const QString &name,
		bool joined = false) {
	const auto session = &controller->session();
	const auto weak = base::make_weak(controller);
	const auto channel = session->data().channelLoaded(
		ChannelFromBotApi(chatId));
	if (!channel || !channel->amIn()) {
		if (joined) {
			controller->showToast(u"Could not open the G&B Library."_q);
			return;
		}
		// Join the libraries first (normally done at sign-in), then retry.
		Api::Instance().get(u"/v1/library"_q, [=](const QJsonObject &r) {
			const auto libraries = r.value(u"libraries"_q).toArray();
			for (const auto value : libraries) {
				const auto library = value.toObject();
				if (library.value(u"chat_id"_q).toVariant().toLongLong() == chatId) {
					JoinInvite(session, library.value(u"invite_link"_q).toString(), [=] {
						if (const auto strong = weak.get()) {
							DownloadFromLibrary(strong, chatId, msgId, name, true);
						}
					});
					return;
				}
			}
			if (const auto strong = weak.get()) {
				strong->showToast(u"Could not open the G&B Library."_q);
			}
		});
		return;
	}
	if (const auto item = session->data().message(channel->id, msgId)) {
		SaveItemDocument(weak, item, name);
		return;
	}
	session->api().requestMessageData(channel, msgId, [=] {
		if (const auto item = session->data().message(channel->id, msgId)) {
			SaveItemDocument(weak, item, name);
		} else if (const auto strong = weak.get()) {
			strong->showToast(u"The owner removed this file."_q);
		}
	});
}

} // namespace

void SyncLibraries(not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	Api::Instance().get(u"/v1/library"_q, [=](const QJsonObject &r) {
		for (const auto value : r.value(u"libraries"_q).toArray()) {
			const auto library = value.toObject();
			const auto chatId = library.value(u"chat_id"_q).toVariant().toLongLong();
			const auto channel = session->data().channelLoaded(
				ChannelFromBotApi(chatId));
			if (channel && channel->amIn()) {
				continue;
			}
			JoinInvite(session, library.value(u"invite_link"_q).toString(), [=] {
				if (const auto joined = session->data().channelLoaded(
						ChannelFromBotApi(chatId))) {
					Tuck(joined);
				}
			});
		}
	});
}

void CreateLibrary(not_null<Window::SessionController*> controller) {
	const auto session = &controller->session();
	const auto weak = base::make_weak(controller);
	const auto toast = [=](const QString &text) {
		if (const auto strong = weak.get()) {
			strong->showToast(text);
		}
	};
	Api::Instance().get(u"/v1/library"_q, [=](const QJsonObject &r) {
		const auto title = r.value(u"title"_q).toString()
			+ u" "_q
			+ QString::number(r.value(u"libraries"_q).toArray().size() + 1);
		Api::Instance().get(u"/v1/drive"_q, [=](const QJsonObject &d) {
			const auto bot = d.value(u"bot"_q).toString();
			using Flag = MTPchannels_CreateChannel::Flag;
			session->api().request(MTPchannels_CreateChannel(
				MTP_flags(Flag::f_broadcast),
				MTP_string(title),
				MTP_string(u"Shared files of G&B Community. "
					"Make Internet Free Again."_q),
				MTPInputGeoPoint(),
				MTPstring(),
				MTP_int(0)
			)).done([=](const MTPUpdates &result) {
				session->api().applyUpdates(result);
				auto channel = (ChannelData*)nullptr;
				result.match([&](const MTPDupdates &data) {
					for (const auto &chat : data.vchats().v) {
						if (chat.type() == mtpc_channel) {
							channel = session->data().channel(chat.c_channel().vid());
						}
					}
				}, [](const auto &) {});
				if (!channel) {
					toast(u"Could not create the library."_q);
					return;
				}
				const auto library = not_null<ChannelData*>(channel);
				Tuck(library);
				ResolveBot(session, bot, [=](not_null<UserData*> user) {
					// The bot posts copies and deletes them with the post.
					using Rights = MTPDchatAdminRights::Flag;
					session->api().request(MTPchannels_EditAdmin(
						MTP_flags(MTPchannels_editAdmin::Flag::f_rank),
						library->inputChannel(),
						user->inputUser(),
						MTP_chatAdminRights(MTP_flags(Rights::f_post_messages
							| Rights::f_edit_messages
							| Rights::f_delete_messages)),
						MTP_string(QString())
					)).done([=](const MTPUpdates &updates) {
						session->api().applyUpdates(updates);
						session->api().inviteLinks().create({
							.peer = library,
							.done = [=](::Api::InviteLink link) {
								auto body = QJsonObject();
								body.insert(u"chat_id"_q, double(BotApiFromChannel(library)));
								body.insert(u"invite_link"_q, link.link);
								// The bot's update may still be on its way.
								const auto put = std::make_shared<Fn<void(int)>>();
								*put = [=](int attempt) {
									Api::Instance().send(u"PUT"_q, u"/v1/library"_q, body, [=](const QJsonObject &) {
										toast(u"G&B Library is ready."_q);
									}, [=](const QString &error) {
										if (attempt < 10) {
											base::call_delayed(kPollDelay, [=] { (*put)(attempt + 1); });
										} else {
											toast(ErrorText(error));
										}
									});
								};
								(*put)(0);
							},
						});
					}).fail([=](const MTP::Error &error) {
						toast(error.type());
					}).send();
				}, toast);
			}).fail([=](const MTP::Error &error) {
				toast(error.type());
			}).send();
		});
	});
}

int64 DriveFileLimit(not_null<Window::SessionController*> controller) {
	return controller->session().premium()
		? kFileSizePremiumLimit
		: kFileSizeLimit;
}

void UploadToDrive(
		not_null<Window::SessionController*> controller,
		const QString &path,
		Fn<void(UploadProgress)> progress,
		Fn<void(QJsonObject file)> done,
		Fn<void(QString error)> fail) {
	const auto session = &controller->session();
	const auto size = QFileInfo(path).size();
	if (size <= 0) {
		fail(u"file_empty"_q);
		return;
	} else if (size > DriveFileLimit(controller)) {
		fail(u"file_too_big"_q);
		return;
	}
	EnsureDrive(controller, [=](not_null<ChannelData*> channel) {
		const auto history = session->data().history(channel);
		auto list = Storage::PrepareMediaList(
			QStringList{ path },
			st::sendMediaPreviewSize,
			session->premium());
		if (list.error != Ui::PreparedList::Error::None
			|| list.files.empty()) {
			fail(u"file_unreadable"_q);
			return;
		}

		// Follow the message we are about to send: its local id changes to
		// the real one when the upload finishes.
		struct State {
			rpl::lifetime lifetime;
			HistoryItem *item = nullptr;
			FullMsgId localId;
			base::Timer timer;
		};
		const auto state = std::make_shared<State>();
		const auto finish = [=] {
			state->timer.cancel();
			state->lifetime.destroy();
		};
		session->data().newItemAdded(
		) | rpl::filter([=](not_null<HistoryItem*> item) {
			return !state->item
				&& (item->history() == history)
				&& item->out();
		}) | rpl::on_next([=](not_null<HistoryItem*> item) {
			state->item = item;
			state->localId = item->fullId();
		}, state->lifetime);
		session->data().itemIdChanged(
		) | rpl::filter([=](const Data::Session::IdChange &change) {
			return state->item
				&& (change.newId.peer == state->localId.peer)
				&& (change.oldId == state->localId.msg);
		}) | rpl::on_next([=](const Data::Session::IdChange &change) {
			const auto id = change.newId.msg;
			finish();
			WaitCloudFile(id, 0, done, fail);
		}, state->lifetime);
		state->timer.setCallback([=] {
			const auto item = state->item
				? session->data().message(state->localId)
				: nullptr;
			const auto media = item ? item->media() : nullptr;
			const auto document = media ? media->document() : nullptr;
			if (document && document->uploadingData && progress) {
				progress({
					.sent = document->uploadingData->offset,
					.total = document->uploadingData->size,
				});
			}
		});
		state->timer.callEach(500);

		session->api().sendFiles(
			std::move(list),
			SendMediaType::File,
			nullptr,
			::Api::SendAction(history));
	}, fail);
}

void DownloadCloudFile(
		not_null<Window::SessionController*> controller,
		const QJsonObject &cloudFile,
		int64 postId) {
	const auto session = &controller->session();
	const auto fileId = cloudFile.value(u"id"_q).toVariant().toLongLong();
	const auto name = cloudFile.value(u"name"_q).toString();
	if (cloudFile.value(u"removed"_q).toBool() || !fileId) {
		controller->showToast(u"The owner removed this file."_q);
		return;
	}
	const auto tag = RandomTag();
	const auto marker = u"#gnb_"_q + tag;

	struct State {
		rpl::lifetime lifetime;
		bool done = false;
	};
	const auto state = std::make_shared<State>();
	const auto weak = base::make_weak(controller);
	session->data().newItemAdded(
	) | rpl::filter([=](not_null<HistoryItem*> item) {
		return !state->done
			&& item->history()->peer->isUser()
			&& item->history()->peer->asUser()->isBot()
			&& item->originalText().text.contains(marker);
	}) | rpl::on_next([=](not_null<HistoryItem*> item) {
		const auto media = item->media();
		const auto document = media ? media->document() : nullptr;
		if (!document) {
			return;
		}
		state->done = true;
		const auto path = UniquePath(
			Downloader::DefaultDownloadDir(),
			name.isEmpty() ? document->filename() : name);
		document->save(Data::FileOrigin(item->fullId()), path);
		Core::App().downloadManager().addLoading({
			.item = item,
			.document = document,
		});
		Tuck(item->history()->peer);
		if (const auto strong = weak.get()) {
			strong->showToast(u"Downloading \"%1\"..."_q.arg(name));
		}
		crl::on_main([=] { state->lifetime.destroy(); });
	}, state->lifetime);
	base::call_delayed(kFetchTimeout, [=] {
		if (!state->done) {
			state->lifetime.destroy();
		}
	});

	auto body = QJsonObject();
	body.insert(u"tag"_q, tag);
	if (postId) {
		body.insert(u"post_id"_q, double(postId));
	}
	controller->showToast(u"Getting \"%1\" from Telegram..."_q.arg(name));
	Api::Instance().send(
		u"POST"_q,
		u"/v1/files/%1/fetch"_q.arg(fileId),
		body,
		[=](const QJsonObject &r) {
			const auto library = r.value(u"library"_q).toObject();
			if (library.isEmpty()) {
				return; // The bot copies it to us; newItemAdded picks it up.
			}
			state->done = true;
			state->lifetime.destroy();
			if (const auto strong = weak.get()) {
				DownloadFromLibrary(
					strong,
					library.value(u"chat_id"_q).toVariant().toLongLong(),
					MsgId(library.value(u"message_id"_q).toVariant().toLongLong()),
					name);
			}
		},
		[=](const QString &error) {
			state->done = true;
			state->lifetime.destroy();
			if (const auto strong = weak.get()) {
				strong->showToast((error == u"file_removed"_q)
					? u"The owner removed this file."_q
					: ErrorText(error));
			}
		});
}

} // namespace GB::Feed
