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

// G&B Drive (docs/FEED-V2-PLAN.md §4): shared files live in the user's own
// private Telegram channel. Everything here is automatic; the user only
// picks a file to share or presses Download.

struct UploadProgress {
	int64 sent = 0;
	int64 total = 0;
};

// The largest file Telegram takes from this account (2 GB, 4 GB Premium).
[[nodiscard]] int64 DriveFileLimit(
	not_null<Window::SessionController*> controller);

// Makes sure the drive exists (creates the channel, makes the bot an admin,
// archives and mutes it), uploads the file there with Telegram's uploader
// and returns the backend's cloud file (id, name, size...) once the bot has
// seen it.
void UploadToDrive(
	not_null<Window::SessionController*> controller,
	const QString &path,
	Fn<void(UploadProgress)> progress,
	Fn<void(QJsonObject file)> done,
	Fn<void(QString error)> fail);

// Download button of a file post: the bot copies the file into our chat
// with it, and Telegram's own downloader saves it to Downloads\GB (shown in
// Telegram's download bar).
void DownloadCloudFile(
	not_null<Window::SessionController*> controller,
	const QJsonObject &cloudFile,
	int64 postId);

// Joins the project's G&B Library channels (shared files are copied there
// once, so downloads come straight from Telegram) and tucks them away in
// the archive. Silent; called after signing in to the Feed.
void SyncLibraries(not_null<Window::SessionController*> controller);

// Project admins only: creates a new G&B Library channel, makes the bot an
// admin there and registers its invite link with the backend.
void CreateLibrary(not_null<Window::SessionController*> controller);

} // namespace GB::Feed
