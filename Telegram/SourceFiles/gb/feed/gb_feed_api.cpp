/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/feed/gb_feed_api.h"

#include "apiwrap.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QStandardPaths>
#include <QtCore/QSysInfo>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace GB::Feed {
namespace {

constexpr auto kUnreadInterval = 60 * 1000;
constexpr auto kLoginPollMs = 1500;
constexpr auto kLoginAttempts = 60;
constexpr auto kMaxImages = 300;

[[nodiscard]] QString DataFolder() {
	const auto result = QStandardPaths::writableLocation(
		QStandardPaths::GenericDataLocation) + u"/GB/feed"_q;
	QDir().mkpath(result);
	return result;
}

[[nodiscard]] QString SessionsPath() {
	return DataFolder() + u"/sessions.json"_q;
}

[[nodiscard]] QJsonObject ReadSessions() {
	auto file = QFile(SessionsPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	return QJsonDocument::fromJson(file.readAll()).object();
}

} // namespace

QString ApiBase() {
	const auto custom = qEnvironmentVariable("GB_API_URL");
	return custom.isEmpty()
		? u"https://gb-api.muhammadabdullahfahad5.workers.dev"_q
		: custom;
}

Api &Api::Instance() {
	static auto instance = Api();
	return instance;
}

Api::Api()
: _network(std::make_unique<QNetworkAccessManager>())
, _unreadTimer(std::make_unique<QTimer>()) {
	QObject::connect(_unreadTimer.get(), &QTimer::timeout, [=] {
		refreshUnread();
	});
	_unreadTimer->start(kUnreadInterval);
	QObject::connect(qApp, &QCoreApplication::aboutToQuit, [=] {
		_unreadTimer->stop();
		_network = nullptr;
	});
}

void Api::setAccount(not_null<Main::Session*> session) {
	const auto id = session->userId().bare;
	if (_account == id) {
		return;
	}
	_account = id;
	_token = ReadSessions().value(QString::number(id)).toString();
	_unread = Unread();
	_signedIn = signedIn();
	refreshUnread();
}

void Api::saveToken() {
	auto sessions = ReadSessions();
	const auto key = QString::number(_account);
	if (_token.isEmpty()) {
		sessions.remove(key);
	} else {
		sessions.insert(key, _token);
	}
	auto file = QFile(SessionsPath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(sessions).toJson(QJsonDocument::Compact));
	}
}

void Api::signIn(
		not_null<Window::SessionController*> controller,
		Fn<void()> done,
		Fail fail) {
	if (_signingIn) {
		return;
	}
	const auto session = &controller->session();
	setAccount(session);
	_signingIn = true;
	const auto failed = [=](const QString &error) {
		_signingIn = false;
		if (fail) {
			fail(error);
		}
	};
	auto body = QJsonObject();
	body.insert(u"device"_q, QSysInfo::machineHostName().left(64));
	send(u"POST"_q, u"/v1/auth/start"_q, body, [=](const QJsonObject &r) {
		const auto nonce = r.value(u"nonce"_q).toString();
		const auto botName = r.value(u"bot"_q).toString();
		const auto param = r.value(u"start_param"_q).toString();
		if (nonce.isEmpty() || botName.isEmpty()) {
			failed(u"bad_response"_q);
			return;
		}
		const auto start = [=](not_null<UserData*> bot) {
			session->api().request(MTPmessages_StartBot(
				bot->inputUser(),
				MTP_inputPeerEmpty(),
				MTP_long(base::RandomValue<uint64>()),
				MTP_string(param)
			)).done([=](const MTPUpdates &result) {
				session->api().applyUpdates(result);
				pollLogin(nonce, 0, done, failed);
			}).fail([=](const MTP::Error &error) {
				failed(error.type());
			}).send();
		};
		if (const auto peer = session->data().peerByUsername(botName)) {
			if (const auto user = peer->asUser()) {
				start(user);
				return;
			}
		}
		session->api().request(MTPcontacts_ResolveUsername(
			MTP_flags(0),
			MTP_string(botName),
			MTP_string()
		)).done([=](const MTPcontacts_ResolvedPeer &result) {
			session->data().processUsers(result.data().vusers());
			session->data().processChats(result.data().vchats());
			const auto peer = session->data().peerLoaded(
				peerFromMTP(result.data().vpeer()));
			const auto user = peer ? peer->asUser() : nullptr;
			if (!user) {
				failed(u"bot_not_found"_q);
				return;
			}
			start(user);
		}).fail([=](const MTP::Error &error) {
			failed(u"bot_not_found"_q);
		}).send();
	}, failed);
}

void Api::pollLogin(
		const QString &nonce,
		int attempt,
		Fn<void()> done,
		Fail fail) {
	if (attempt >= kLoginAttempts) {
		fail(u"login_timeout"_q);
		return;
	}
	const auto path = u"/v1/auth/poll?nonce="_q
		+ QString::fromLatin1(QUrl::toPercentEncoding(nonce));
	get(path, [=](const QJsonObject &r) {
		if (r.value(u"status"_q).toString() == u"ok"_q) {
			const auto userId = uint64(
				r.value(u"user_id"_q).toVariant().toULongLong());
			if (userId != _account) {
				fail(u"account_mismatch"_q);
				return;
			}
			_token = r.value(u"token"_q).toString();
			saveToken();
			_signingIn = false;
			_signedIn = true;
			refreshUnread();
			if (done) {
				done();
			}
			return;
		}
		QTimer::singleShot(kLoginPollMs, [=] {
			pollLogin(nonce, attempt + 1, done, fail);
		});
	}, fail);
}

void Api::signOut() {
	if (!_token.isEmpty()) {
		send(u"POST"_q, u"/v1/auth/logout"_q, {}, nullptr, nullptr);
	}
	_token = QString();
	saveToken();
	_unread = Unread();
	_signedIn = false;
}

void Api::get(const QString &path, Done done, Fail fail) {
	request(u"GET"_q, path, QByteArray(), QString(), done, fail);
}

void Api::send(
		const QString &method,
		const QString &path,
		const QJsonObject &body,
		Done done,
		Fail fail) {
	request(
		method,
		path,
		QJsonDocument(body).toJson(QJsonDocument::Compact),
		u"application/json"_q,
		done,
		fail);
}

void Api::uploadImage(const QString &path, Done done, Fail fail) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024) {
		if (fail) {
			fail(u"too_large"_q);
		}
		return;
	}
	request(
		u"POST"_q,
		u"/v1/media"_q,
		file.readAll(),
		u"application/octet-stream"_q,
		done,
		fail);
}

void Api::request(
		const QString &method,
		const QString &path,
		const QByteArray &body,
		const QString &contentType,
		Done done,
		Fail fail) {
	if (!_network) {
		return;
	}
	auto request = QNetworkRequest(QUrl(ApiBase() + path));
	if (!_token.isEmpty()) {
		request.setRawHeader("Authorization", ("Bearer " + _token).toLatin1());
	}
	if (!contentType.isEmpty()) {
		request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
	}
	request.setTransferTimeout(30000);
	const auto reply = _network->sendCustomRequest(
		request,
		method.toLatin1(),
		body);
	const auto token = _token;
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto data = reply->readAll();
		const auto code = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto object = QJsonDocument::fromJson(data).object();
		if (code >= 200 && code < 300) {
			if (done) {
				done(object);
			}
			return;
		}
		auto error = object.value(u"error"_q).toString();
		if (error.isEmpty()) {
			error = code ? (u"HTTP "_q + QString::number(code)) : u"network"_q;
		}
		if (code == 401 && !token.isEmpty() && token == _token) {
			// The session was revoked on the server.
			_token = QString();
			saveToken();
			_signedIn = false;
		}
		if (fail) {
			fail(error);
		}
	});
}

void Api::loadImage(const QString &url, Fn<void(QImage)> done) {
	if (url.isEmpty() || !_network) {
		return;
	}
	const auto i = _images.find(url);
	if (i != end(_images)) {
		done(i->second);
		return;
	}
	auto &waiters = _imageWaiters[url];
	waiters.push_back(std::move(done));
	if (waiters.size() > 1) {
		return;
	}
	auto request = QNetworkRequest(QUrl(url));
	request.setTransferTimeout(30000);
	const auto reply = _network->get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		auto image = QImage::fromData(reply->readAll());
		if (_images.size() >= kMaxImages) {
			_images.clear();
		}
		if (!image.isNull()) {
			_images.emplace(url, image);
		}
		auto list = base::take(_imageWaiters[url]);
		_imageWaiters.remove(url);
		if (!image.isNull()) {
			for (const auto &callback : list) {
				callback(image);
			}
		}
	});
}

void Api::refreshUnread() {
	if (!signedIn()) {
		return;
	}
	get(u"/v1/me/unread"_q, [=](const QJsonObject &r) {
		_unread = Unread{
			.notifications = r.value(u"notifications"_q).toInt(),
			.friendRequests = r.value(u"friend_requests"_q).toInt(),
			.approvedRequests = r.value(u"approved_requests"_q).toInt(),
		};
	});
}

QString ErrorText(const QString &code) {
	static const auto map = base::flat_map<QString, QString>{
		{ u"network"_q, u"Can't reach the GB server. Check your internet."_q },
		{ u"unauthorized"_q, u"Your GB session expired. Sign in again."_q },
		{ u"banned"_q, u"This account is suspended from GB."_q },
		{ u"bot_not_found"_q, u"The GB sign-in bot was not found."_q },
		{ u"login_timeout"_q, u"Sign-in took too long. Try again."_q },
		{ u"account_mismatch"_q, u"Signed in with a different account."_q },
		{ u"empty_post"_q, u"Write something first."_q },
		{ u"missing_file_name"_q, u"Enter the file name."_q },
		{ u"too_large"_q, u"The image is larger than 8 MB."_q },
		{ u"unsupported_type"_q, u"Only JPG, PNG, WebP and GIF images."_q },
		{ u"handle_taken"_q, u"That handle is already taken."_q },
		{ u"not_allowed"_q, u"You can't do that here."_q },
		{ u"post_not_found"_q, u"This post is no longer available."_q },
		{ u"user_not_found"_q, u"This user is not on GB."_q },
		{ u"blocked"_q, u"You can't interact with this user."_q },
		{ u"bad_options"_q, u"A poll needs 2 to 10 options."_q },
		{ u"bad_telegram_link"_q, u"Use a https://t.me/... link."_q },
	};
	const auto i = map.find(code);
	return (i != end(map)) ? i->second : (u"Error: "_q + code);
}

QString RelativeTime(int64 unixtime) {
	const auto diff = int64(base::unixtime::now()) - unixtime;
	if (diff < 60) {
		return u"just now"_q;
	} else if (diff < 3600) {
		return QString::number(diff / 60) + u"m"_q;
	} else if (diff < 86400) {
		return QString::number(diff / 3600) + u"h"_q;
	} else if (diff < 7 * 86400) {
		return QString::number(diff / 86400) + u"d"_q;
	}
	return base::unixtime::parse(TimeId(unixtime)).date().toString(
		u"d MMM yyyy"_q);
}

} // namespace GB::Feed
