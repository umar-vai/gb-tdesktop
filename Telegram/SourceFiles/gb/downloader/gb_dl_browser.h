/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"
#include "gb/downloader/gb_dl_tasks.h"

#include <QtCore/QJsonObject>

class QTcpServer;
class QTcpSocket;

namespace GB::Downloader {

class Manager;

// The local endpoint the GB browser extension talks to. It listens on
// 127.0.0.1 only, hands its pairing token to browser extensions only (web
// pages cannot fake the Origin header) and turns what the extension sends
// (a page, a captured HLS/DASH stream with the player's headers and
// cookies, or a file the browser started) into downloads.
class BrowserServer final : public base::has_weak_ptr {
public:
	explicit BrowserServer(not_null<Manager*> manager);
	~BrowserServer();

	[[nodiscard]] static BrowserServer *Instance();

	[[nodiscard]] bool running() const;
	[[nodiscard]] int port() const;
	[[nodiscard]] bool extensionConnected() const;

	// Opens the page in the user's browser; the extension reports the
	// streams its player loads. Returns 0 when there is no extension.
	[[nodiscard]] uint64 resolve(
		const QString &url,
		Fn<void(BrowserReport)> done);
	void cancelResolve(uint64 id);

	// Writes the extension into the GB data folder and returns that folder.
	[[nodiscard]] static QString ExportExtension(QString *error);

private:
	struct Request {
		QByteArray method;
		QByteArray path;
		QByteArray origin;
		QByteArray host;
		QByteArray token;
		QByteArray body;
	};
	struct Connection {
		QByteArray buffer;
		bool handled = false;
	};
	struct Job {
		QString key;
		Fn<void(BrowserReport)> done;
	};
	struct Cached {
		QString infoJsonPath;
		crl::time when = 0;
	};

	void start();
	void accept();
	void read(not_null<QTcpSocket*> socket);
	void dropped(not_null<QTcpSocket*> socket);
	void handle(not_null<QTcpSocket*> socket, const Request &request);
	void send(
		not_null<QTcpSocket*> socket,
		int status,
		const QByteArray &body,
		const QByteArray &origin,
		bool preflight = false);
	void sendJson(
		not_null<QTcpSocket*> socket,
		int status,
		const QJsonObject &payload,
		const QByteArray &origin);
	[[nodiscard]] QJsonObject download(const QJsonObject &data);
	void analyze(
		not_null<QTcpSocket*> socket,
		const QJsonObject &data,
		const QByteArray &origin);
	[[nodiscard]] bool resolved(const QJsonObject &data);
	void finishJob(uint64 id, BrowserReport report);
	[[nodiscard]] QString cachedInfo(const QString &url);
	void rememberInfo(const QString &url, const QString &path);

	const not_null<Manager*> _manager;
	std::unique_ptr<QTcpServer> _server;
	QString _token;
	int _port = 0;
	crl::time _seenAt = 0;
	std::map<QTcpSocket*, Connection> _connections;
	std::map<QTcpSocket*, std::unique_ptr<Analyzer>> _analyzers;
	std::map<uint64, Job> _jobs;
	std::map<QString, Cached> _analyzed;
	uint64 _nextJob = 0;

};

} // namespace GB::Downloader
