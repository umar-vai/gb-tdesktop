/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QJsonObject>

namespace Window {
class SessionController;
} // namespace Window

namespace GB::Feed {

struct ComposeArgs {
	QString kind; // text, need, have, poll; empty = text
	int64 communityId = 0;
	QJsonObject shared; // repost of this post
	QJsonObject edit; // edit this own post
};

void ShowComposeBox(
	not_null<Window::SessionController*> controller,
	ComposeArgs args = {});
void ShowCommentsBox(
	not_null<Window::SessionController*> controller,
	const QJsonObject &post);
void ShowPostBox(
	not_null<Window::SessionController*> controller,
	int64 postId);
void ShowProfileBox(
	not_null<Window::SessionController*> controller,
	uint64 userId);
void ShowEditProfileBox(not_null<Window::SessionController*> controller);
void ShowNotificationsBox(not_null<Window::SessionController*> controller);
void ShowRequestsBox(not_null<Window::SessionController*> controller);
void ShowFriendRequestsBox(not_null<Window::SessionController*> controller);
void ShowSuggestionsBox(not_null<Window::SessionController*> controller);
void ShowPostsBox(
	not_null<Window::SessionController*> controller,
	const QString &title,
	const QString &path,
	const QString &emptyText);
void ShowPeopleBox(
	not_null<Window::SessionController*> controller,
	const QString &title,
	const QString &path);
void ShowReactionsBox(
	not_null<Window::SessionController*> controller,
	int64 postId);
void ShowCommunitiesBox(not_null<Window::SessionController*> controller);
void ShowCommunityBox(
	not_null<Window::SessionController*> controller,
	int64 communityId);
void ShowCreateCommunityBox(not_null<Window::SessionController*> controller);
void ShowSearchBox(
	not_null<Window::SessionController*> controller,
	const QString &query);
void ShowReportBox(
	not_null<Window::SessionController*> controller,
	const QString &targetType,
	int64 targetId);

// Approve a file request: pick the file, offer it with p2pd and hand the
// gb:// invite to the backend, which passes it to the requester.
void ApproveFileRequest(
	not_null<Window::SessionController*> controller,
	int64 requestId,
	const QString &fileName,
	Fn<void()> done);

// Opens a Telegram chat with this user (@username) when possible.
void OpenTelegramChat(
	not_null<Window::SessionController*> controller,
	const QJsonObject &user);

// Called once per window: binds the GB account to the Telegram session,
// shows a toast for new notifications and offers approved files.
void WatchFeed(not_null<Window::SessionController*> controller);

} // namespace GB::Feed
