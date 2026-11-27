/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtGui/QImage>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace GB::Feed {

// GB's own backend (Cloudflare Worker, backend/ in the main repo).
// GB_API_URL in the environment overrides it for local testing.
[[nodiscard]] QString ApiBase();

struct Unread {
	int notifications = 0;
	int friendRequests = 0;
	int approvedRequests = 0;

	friend inline bool operator==(const Unread &, const Unread &) = default;
};

using Done = Fn<void(const QJsonObject &)>;
using Fail = Fn<void(const QString &)>;

// Session tokens are kept per Telegram account in
// %LocalAppData%\GB\feed\sessions.json.
class Api final {
public:
	[[nodiscard]] static Api &Instance();

	// Picks the GB token that belongs to this Telegram account.
	void setAccount(not_null<Main::Session*> session);
	[[nodiscard]] bool signedIn() const {
		return !_token.isEmpty();
	}
	[[nodiscard]] uint64 myId() const {
		return _account;
	}
	[[nodiscard]] rpl::producer<bool> signedInValue() const {
		return _signedIn.value();
	}

	// Makes this Telegram account start @GnBfeedBot with a one-time code;
	// the bot confirms the account to our backend, which returns a token.
	void signIn(
		not_null<Window::SessionController*> controller,
		Fn<void()> done,
		Fail fail);
	void signOut();

	void get(const QString &path, Done done, Fail fail = nullptr);
	void send(
		const QString &method,
		const QString &path,
		const QJsonObject &body,
		Done done,
		Fail fail = nullptr);
	void uploadImage(const QString &path, Done done, Fail fail);

	// Images from our R2 (avatars, post pictures), cached in memory.
	void loadImage(const QString &url, Fn<void(QImage)> done);

	[[nodiscard]] rpl::producer<Unread> unreadValue() const {
		return _unread.value();
	}
	[[nodiscard]] Unread unread() const {
		return _unread.current();
	}
	void refreshUnread();

	// Fired after a change that should reload visible lists
	// (new post, deleted post, etc.).
	[[nodiscard]] rpl::producer<> changes() const {
		return _changes.events();
	}
	void notifyChanged() {
		_changes.fire({});
	}

private:
	Api();

	void request(
		const QString &method,
		const QString &path,
		const QByteArray &body,
		const QString &contentType,
		Done done,
		Fail fail);
	void saveToken();
	void pollLogin(
		const QString &nonce,
		int attempt,
		Fn<void()> done,
		Fail fail);

	std::unique_ptr<QNetworkAccessManager> _network;
	std::unique_ptr<QTimer> _unreadTimer;
	QString _token;
	uint64 _account = 0;
	rpl::variable<bool> _signedIn = false;
	rpl::variable<Unread> _unread;
	rpl::event_stream<> _changes;
	base::flat_map<QString, QImage> _images;
	base::flat_map<QString, std::vector<Fn<void(QImage)>>> _imageWaiters;
	bool _signingIn = false;

};

[[nodiscard]] QString ErrorText(const QString &code);
[[nodiscard]] QString RelativeTime(int64 unixtime);

} // namespace GB::Feed
