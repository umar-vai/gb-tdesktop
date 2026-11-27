/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/downloader/gb_dl_browser.h"

#include "base/call_delayed.h"
#include "base/random.h"
#include "core/version.h"
#include "gb/downloader/gb_dl_capture.h"
#include "gb/downloader/gb_dl_manager.h"

#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

namespace GB::Downloader {
namespace {

constexpr auto kFirstPort = 47740;
constexpr auto kPortCount = 10;
constexpr auto kMaxHeader = 16 * 1024;
constexpr auto kMaxBody = 512 * 1024;
constexpr auto kResolveTimeout = 45 * crl::time(1000);
constexpr auto kCacheLifetime = 10 * 60 * crl::time(1000);
constexpr auto kMaxAnalyzers = 4;
constexpr auto kMaxCookies = 400;
constexpr auto kMaxStreams = 8;

BrowserServer *GlobalServer = nullptr;

[[nodiscard]] bool FromExtension(const QByteArray &origin) {
	return origin.startsWith("chrome-extension://")
		|| origin.startsWith("moz-extension://")
		|| origin.startsWith("edge-extension://")
		|| origin.startsWith("extension://");
}

[[nodiscard]] bool OriginAllowed(const QByteArray &origin) {
	return origin.isEmpty() || FromExtension(origin);
}

// A page on another site cannot reach us under its own name (DNS rebinding):
// only requests addressed to the loopback host are served.
[[nodiscard]] bool HostAllowed(const QByteArray &host) {
	const auto name = host.left(host.lastIndexOf(':') > host.lastIndexOf(']')
		? host.lastIndexOf(':')
		: host.size()).toLower();
	return name.isEmpty()
		|| name == "127.0.0.1"
		|| name == "localhost"
		|| name == "[::1]";
}

[[nodiscard]] bool SameSecret(const QByteArray &a, const QByteArray &b) {
	if (a.isEmpty() || a.size() != b.size()) {
		return false;
	}
	auto difference = 0;
	for (auto i = 0; i != a.size(); ++i) {
		difference |= (a[i] ^ b[i]);
	}
	return !difference;
}

[[nodiscard]] QString StatusText(int status) {
	switch (status) {
	case 200: return u"OK"_q;
	case 204: return u"No Content"_q;
	case 400: return u"Bad Request"_q;
	case 401: return u"Unauthorized"_q;
	case 403: return u"Forbidden"_q;
	case 404: return u"Not Found"_q;
	case 413: return u"Payload Too Large"_q;
	case 429: return u"Too Many Requests"_q;
	}
	return u"Internal Server Error"_q;
}

[[nodiscard]] QString TokenPath() {
	return DataPath() + u"/browser-token.txt"_q;
}

[[nodiscard]] QString LoadToken() {
	auto file = QFile(TokenPath());
	if (file.open(QIODevice::ReadOnly)) {
		const auto token = QString::fromLatin1(file.readAll()).trimmed();
		static const auto valid = QRegularExpression(u"^[0-9a-f]{32}$"_q);
		if (valid.match(token).hasMatch()) {
			return token;
		}
		LOG(("GB Browser: token file unreadable, making a new one."));
	} else if (file.exists()) {
		LOG(("GB Browser: could not open the token file: %1").arg(
			file.errorString()));
	}
	auto token = QString();
	for (auto i = 0; i != 2; ++i) {
		token += u"%1"_q.arg(base::RandomValue<uint64>(), 16, 16, QChar('0'));
	}
	file.close();
	if (file.open(QIODevice::WriteOnly)) {
		file.write(token.toLatin1());
	}
	return token;
}

[[nodiscard]] bool SafeHeaderValue(const QString &value) {
	return !value.isEmpty()
		&& value.size() <= 4096
		&& !value.contains('\r')
		&& !value.contains('\n')
		&& !value.contains(QChar(0));
}

[[nodiscard]] QString WebUrl(const QJsonValue &value) {
	const auto text = value.toString().trimmed();
	return IsValidWebUrl(text) ? text : QString();
}

[[nodiscard]] QString HeaderFrom(const QJsonObject &headers, QStringView name) {
	for (auto i = headers.begin(); i != headers.end(); ++i) {
		if (i.key().compare(name, Qt::CaseInsensitive) == 0) {
			return i.value().toString().trimmed();
		}
	}
	return QString();
}

// The headers a CDN checks: Referer, Origin and User-Agent. The cookies go
// to a cookies.txt file, which yt-dlp scopes to their own domains.
[[nodiscard]] BrowserContext ContextFrom(
		const QString &url,
		const QString &referer,
		const QJsonObject &headers,
		const QJsonArray &cookies) {
	auto result = BrowserContext();
	const auto add = [&](const QString &name, const QString &value) {
		if (SafeHeaderValue(value)) {
			result.headers.push_back({ name, value });
		}
	};
	const auto explicitReferer = HeaderFrom(headers, u"Referer");
	add(u"Referer"_q, IsValidWebUrl(referer)
		? referer
		: IsValidWebUrl(explicitReferer)
		? explicitReferer
		: QString());
	const auto origin = HeaderFrom(headers, u"Origin");
	if (origin.startsWith(u"http://"_q) || origin.startsWith(u"https://"_q)) {
		add(u"Origin"_q, origin);
	}
	add(u"User-Agent"_q, HeaderFrom(headers, u"User-Agent"));

	auto list = std::vector<BrowserCookie>();
	for (const auto &value : cookies) {
		if (int(list.size()) >= kMaxCookies) {
			break;
		}
		const auto cookie = value.toObject();
		list.push_back({
			.domain = cookie.value(u"domain"_q).toString(),
			.name = cookie.value(u"name"_q).toString(),
			.value = cookie.value(u"value"_q).toString(),
			.path = cookie.value(u"path"_q).toString(),
			.hostOnly = cookie.value(u"hostOnly"_q).toBool(),
			.secure = cookie.value(u"secure"_q).toBool(),
			.httpOnly = cookie.value(u"httpOnly"_q).toBool(),
			.expires = int64(cookie.value(u"expirationDate"_q).toDouble()),
		});
	}
	const auto header = HeaderFrom(headers, u"Cookie");
	if (SafeHeaderValue(header)) {
		const auto more = CookiesFromHeader(url, header);
		list.insert(end(list), begin(more), end(more));
	}
	if (!list.empty()) {
		result.cookiesFile = WriteCookieFile(list);
	}
	return result;
}

[[nodiscard]] QString CacheKey(const QString &url) {
	return url.trimmed().split('#').front();
}

[[nodiscard]] QString CleanTitle(const QString &title) {
	const auto text = title.simplified().left(180);
	return IsPlaceholderTitle(text) ? QString() : text;
}

[[nodiscard]] QString QualityFrom(const QString &value) {
	static const auto height = QRegularExpression(u"^\\d{3,4}p$"_q);
	const auto text = value.trimmed();
	if (text.isEmpty()) {
		return QString();
	} else if (height.match(text).hasMatch()) {
		return text;
	}
	return BestQuality();
}

} // namespace

BrowserServer::BrowserServer(not_null<Manager*> manager)
: _manager(manager)
, _token(LoadToken()) {
	GlobalServer = this;
	start();
}

BrowserServer::~BrowserServer() {
	if (GlobalServer == this) {
		GlobalServer = nullptr;
	}
	_jobs.clear();
	_analyzers.clear();
	for (const auto &[url, cached] : base::take(_analyzed)) {
		QFile::remove(cached.infoJsonPath);
	}
	if (_server) {
		_server->close();
		_server = nullptr;
	}
}

BrowserServer *BrowserServer::Instance() {
	return GlobalServer;
}

bool BrowserServer::running() const {
	return _server && _server->isListening();
}

int BrowserServer::port() const {
	return _port;
}

bool BrowserServer::extensionConnected() const {
	return running() && (_seenAt > 0);
}

void BrowserServer::start() {
	_server = std::make_unique<QTcpServer>();
	for (auto port = kFirstPort; port != kFirstPort + kPortCount; ++port) {
		if (_server->listen(QHostAddress::LocalHost, quint16(port))) {
			_port = port;
			break;
		}
	}
	if (!_port) {
		_server = nullptr;
		return;
	}
	QObject::connect(_server.get(), &QTcpServer::newConnection, [=] {
		accept();
	});
}

void BrowserServer::accept() {
	while (_server && _server->hasPendingConnections()) {
		const auto socket = _server->nextPendingConnection();
		if (!socket) {
			break;
		}
		_connections.emplace(socket, Connection());
		const auto weak = base::make_weak(this);
		QObject::connect(socket, &QTcpSocket::readyRead, [=] {
			if (weak) {
				read(socket);
			}
		});
		QObject::connect(socket, &QTcpSocket::disconnected, [=] {
			if (weak) {
				dropped(socket);
			}
			socket->deleteLater();
		});
		if (socket->bytesAvailable() > 0) {
			read(socket);
		}
	}
}

void BrowserServer::dropped(not_null<QTcpSocket*> socket) {
	_connections.erase(socket);
	if (const auto i = _analyzers.find(socket); i != end(_analyzers)) {
		auto analyzer = std::move(i->second);
		_analyzers.erase(i);
		analyzer->cancel();
		base::call_delayed(0, [moved = std::shared_ptr<Analyzer>(
			analyzer.release())] {});
	}
}

void BrowserServer::read(not_null<QTcpSocket*> socket) {
	const auto i = _connections.find(socket);
	if (i == end(_connections)) {
		socket->readAll();
		return;
	}
	auto &connection = i->second;
	connection.buffer += socket->readAll();
	if (connection.handled) {
		connection.buffer.clear();
		return;
	}
	const auto headerEnd = connection.buffer.indexOf("\r\n\r\n");
	if (headerEnd < 0) {
		if (connection.buffer.size() > kMaxHeader) {
			connection.handled = true;
			send(socket, 413, R"({"ok":false,"error":"bad size"})", {});
		}
		return;
	}
	auto request = Request();
	auto length = qsizetype(0);
	const auto lines = connection.buffer.left(headerEnd).split('\n');
	const auto first = lines.value(0).trimmed().split(' ');
	request.method = first.value(0).toUpper();
	request.path = first.value(1).split('?').front();
	for (auto j = 1; j < lines.size(); ++j) {
		const auto &line = lines[j];
		const auto colon = line.indexOf(':');
		if (colon <= 0) {
			continue;
		}
		const auto name = line.left(colon).trimmed().toLower();
		const auto value = line.mid(colon + 1).trimmed();
		if (name == "content-length") {
			length = value.toLongLong();
		} else if (name == "origin") {
			request.origin = value;
		} else if (name == "host") {
			request.host = value;
		} else if (name == "x-gb-token") {
			request.token = value;
		}
	}
	if (length < 0 || length > kMaxBody) {
		connection.handled = true;
		send(socket, 413, R"({"ok":false,"error":"bad size"})", {});
		return;
	}
	const auto bodyStart = headerEnd + 4;
	if (connection.buffer.size() < bodyStart + length) {
		return;
	}
	request.body = connection.buffer.mid(bodyStart, length);
	connection.handled = true;
	connection.buffer.clear();
	handle(socket, request);
}

void BrowserServer::handle(
		not_null<QTcpSocket*> socket,
		const Request &request) {
	const auto origin = request.origin;
	const auto allowed = OriginAllowed(origin) && HostAllowed(request.host);
	if (request.path != "/ping") {
		LOG(("GB Browser: %1 %2 from %3 (%4)").arg(
			QString::fromLatin1(request.method),
			QString::fromLatin1(request.path),
			QString::fromLatin1(origin),
			allowed ? u"allowed"_q : u"refused"_q));
	}
	const auto echo = (allowed && !origin.isEmpty()) ? origin : QByteArray();
	const auto fail = [&](int status, const QString &error) {
		sendJson(socket, status, QJsonObject{
			{ u"ok"_q, false },
			{ u"error"_q, error },
		}, echo);
	};
	if (request.method == "OPTIONS") {
		send(socket, allowed ? 204 : 403, QByteArray(), echo, true);
		return;
	} else if (!allowed) {
		fail(403, u"forbidden"_q);
		return;
	}
	const auto extension = FromExtension(origin);
	if (request.method == "GET") {
		if (request.path == "/ping") {
			if (extension) {
				_seenAt = crl::now();
			}
			sendJson(socket, 200, QJsonObject{
				{ u"ok"_q, true },
				{ u"app"_q, u"GB"_q },
				{ u"version"_q, QString::fromLatin1(AppVersionStr) },
				{ u"protocol"_q, 1 },
				{ u"capture"_q, _manager->settings().catchBrowserDownloads },
				{ u"engine"_q, _manager->engineAvailable() },
			}, echo);
		} else {
			fail(404, u"not found"_q);
		}
		return;
	} else if (request.method != "POST") {
		fail(404, u"not found"_q);
		return;
	}
	const auto path = request.path;
	if (path == "/pair") {
		// Self-pairing: only a browser extension can read the token. It is a
		// POST because Chrome leaves the Origin header off an extension's GET
		// requests, and a web page cannot send a POST as chrome-extension://.
		if (!extension) {
			fail(403, u"forbidden"_q);
			return;
		}
		_seenAt = crl::now();
		sendJson(socket, 200, QJsonObject{
			{ u"ok"_q, true },
			{ u"token"_q, _token },
		}, echo);
		return;
	} else if (path != "/hello"
		&& path != "/download"
		&& path != "/analyze"
		&& path != "/resolved") {
		fail(404, u"not found"_q);
		return;
	} else if (!SameSecret(request.token, _token.toLatin1())) {
		fail(401, u"bad token"_q);
		return;
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(request.body, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		fail(400, u"bad json"_q);
		return;
	}
	const auto data = document.object();
	_seenAt = crl::now();
	if (path == "/hello") {
		sendJson(socket, 200, QJsonObject{
			{ u"ok"_q, true },
			{ u"version"_q, QString::fromLatin1(AppVersionStr) },
		}, echo);
	} else if (path == "/resolved") {
		const auto accepted = resolved(data);
		sendJson(socket, accepted ? 200 : 404, QJsonObject{
			{ u"ok"_q, accepted },
		}, echo);
	} else if (WebUrl(data.value(u"url"_q)).isEmpty()) {
		fail(400, u"bad url"_q);
	} else if (path == "/analyze") {
		analyze(socket, data, echo);
	} else {
		const auto result = download(data);
		sendJson(
			socket,
			result.value(u"ok"_q).toBool() ? 200 : 500,
			result,
			echo);
	}
}

void BrowserServer::send(
		not_null<QTcpSocket*> socket,
		int status,
		const QByteArray &body,
		const QByteArray &origin,
		bool preflight) {
	if (socket->state() != QAbstractSocket::ConnectedState) {
		return;
	}
	auto head = QByteArray("HTTP/1.1 ")
		+ QByteArray::number(status)
		+ ' '
		+ StatusText(status).toLatin1()
		+ "\r\n";
	if (!origin.isEmpty()) {
		head += "Access-Control-Allow-Origin: " + origin + "\r\n";
		head += "Vary: Origin\r\n";
		if (preflight) {
			head += "Access-Control-Allow-Headers: Content-Type, X-GB-Token\r\n";
			head += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
			head += "Access-Control-Allow-Private-Network: true\r\n";
		}
	}
	if (!body.isEmpty()) {
		head += "Content-Type: application/json\r\n";
	}
	head += "Cache-Control: no-store\r\n";
	head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
	head += "Connection: close\r\n\r\n";
	socket->write(head + body);
	socket->disconnectFromHost();
}

void BrowserServer::sendJson(
		not_null<QTcpSocket*> socket,
		int status,
		const QJsonObject &payload,
		const QByteArray &origin) {
	send(
		socket,
		status,
		QJsonDocument(payload).toJson(QJsonDocument::Compact),
		origin);
}

QJsonObject BrowserServer::download(const QJsonObject &data) {
	const auto url = WebUrl(data.value(u"url"_q));
	const auto referer = WebUrl(data.value(u"referer"_q));
	auto pageUrl = WebUrl(data.value(u"pageUrl"_q));
	const auto kind = data.value(u"kind"_q).toString();
	const auto headers = data.value(u"headers"_q).toObject();
	auto request = AddRequest();
	request.url = url;
	request.mode = (kind == u"file"_q)
		? Mode::File
		: (data.value(u"mode"_q).toString().compare(
			u"audio"_q,
			Qt::CaseInsensitive) == 0)
		? Mode::Audio
		: Mode::Video;
	if (request.mode == Mode::File) {
		request.title = CleanFilename(
			data.value(u"fileName"_q).toString(),
			QString());
		request.fileSize = int64(data.value(u"fileSize"_q).toDouble());
		// A file the browser started: its page stays its own.
		if (pageUrl.isEmpty()) {
			pageUrl = referer;
		}
	} else {
		request.title = CleanTitle(data.value(u"title"_q).toString());
		request.quality = QualityFrom(data.value(u"quality"_q).toString());
	}
	request.pageUrl = pageUrl.isEmpty() ? url : pageUrl;
	request.context = ContextFrom(
		url,
		referer,
		headers,
		data.value(u"cookies"_q).toArray());
	request.fromBrowser = true;
	if (request.mode != Mode::File) {
		request.infoJsonPath = cachedInfo(url);
	}
	LOG(("GB Browser: download %1 (%2, %3)").arg(
		url.left(200),
		ModeKey(request.mode),
		request.quality));
	if (_manager->settings().confirmBrowserDownloads) {
		// Its own window on top of the browser, like IDM's, not a box
		// inside the Telegram window.
		ShowCaptureWindow(std::move(request));
		return QJsonObject{ { u"ok"_q, true }, { u"asked"_q, true } };
	}
	const auto id = _manager->add(std::move(request));
	return QJsonObject{
		{ u"ok"_q, id != 0 },
		{ u"id"_q, QString::number(id) },
	};
}

void BrowserServer::analyze(
		not_null<QTcpSocket*> socket,
		const QJsonObject &data,
		const QByteArray &origin) {
	if (int(_analyzers.size()) >= kMaxAnalyzers) {
		sendJson(socket, 429, QJsonObject{
			{ u"ok"_q, false },
			{ u"error"_q, u"GB is busy reading other videos."_q },
		}, origin);
		return;
	}
	const auto url = WebUrl(data.value(u"url"_q));
	auto context = ContextFrom(
		url,
		QString(),
		QJsonObject(),
		data.value(u"cookies"_q).toArray());
	const auto cookies = context.cookiesFile;
	const auto weak = base::make_weak(this);
	const auto raw = socket.get();
	auto analyzer = std::make_unique<Analyzer>(
		_manager->settings(),
		url,
		nullptr,
		[=](AnalyzeResult result) {
			RemoveCookieFile(cookies);
			if (!weak) {
				QFile::remove(result.infoJsonPath);
				return;
			}
			if (const auto i = _analyzers.find(raw); i != end(_analyzers)) {
				base::call_delayed(0, [moved = std::shared_ptr<Analyzer>(
					i->second.release())] {});
				_analyzers.erase(i);
			}
			auto qualities = QJsonArray();
			for (const auto &quality : result.qualities) {
				if (quality != BestQuality()) {
					qualities.push_back(quality);
				}
			}
			if (result.ok && !result.infoJsonPath.isEmpty()) {
				rememberInfo(url, result.infoJsonPath);
			}
			if (!_connections.contains(raw)) {
				return;
			}
			sendJson(raw, 200, result.ok
				? QJsonObject{
					{ u"ok"_q, true },
					{ u"title"_q, result.title },
					{ u"creator"_q, result.creator },
					{ u"duration"_q, double(result.duration) },
					{ u"thumbnail"_q, result.thumbnail },
					{ u"qualities"_q, qualities },
					{ u"file"_q, result.plainFile },
				}
				: QJsonObject{
					{ u"ok"_q, false },
					{ u"error"_q, result.error.split('\n').front() },
				}, origin);
		},
		std::move(context));
	analyzer->setQuick(true);
	const auto pointer = analyzer.get();
	_analyzers.emplace(raw, std::move(analyzer));
	pointer->start();
}

bool BrowserServer::resolved(const QJsonObject &data) {
	const auto key = data.value(u"id"_q).toString();
	auto found = uint64(0);
	for (const auto &[id, job] : _jobs) {
		if (!key.isEmpty() && job.key == key) {
			found = id;
			break;
		}
	}
	if (!found) {
		return false;
	}
	auto report = BrowserReport();
	report.title = CleanTitle(data.value(u"title"_q).toString());
	report.pageUrl = WebUrl(data.value(u"pageUrl"_q));
	for (const auto &value : data.value(u"streams"_q).toArray()) {
		if (int(report.streams.size()) >= kMaxStreams) {
			break;
		}
		const auto stream = value.toObject();
		const auto url = WebUrl(stream.value(u"url"_q));
		if (url.isEmpty() || stream.value(u"drm"_q).toBool()) {
			continue;
		}
		report.streams.push_back({
			.url = url,
			.kind = stream.value(u"kind"_q).toString(),
			.master = stream.value(u"master"_q).toBool(),
			.context = ContextFrom(
				url,
				WebUrl(stream.value(u"referer"_q)),
				stream.value(u"headers"_q).toObject(),
				QJsonArray()),
		});
	}
	finishJob(found, std::move(report));
	return true;
}

uint64 BrowserServer::resolve(
		const QString &url,
		Fn<void(BrowserReport)> done) {
	if (!extensionConnected() || !IsValidWebUrl(url)) {
		return 0;
	}
	const auto id = ++_nextJob;
	const auto key = u"%1%2"_q
		.arg(base::RandomValue<uint64>(), 16, 16, QChar('0'))
		.arg(base::RandomValue<uint32>(), 8, 16, QChar('0'));
	_jobs.emplace(id, Job{ .key = key, .done = std::move(done) });
	// A fragment never reaches the site; the extension spots it, starts the
	// player, reports the streams and closes the tab.
	const auto marked = url
		+ (url.contains('#') ? '&' : '#')
		+ u"gb-resolve="_q
		+ key;
	QDesktopServices::openUrl(QUrl(marked, QUrl::TolerantMode));
	base::call_delayed(kResolveTimeout, this, [=] {
		finishJob(id, {});
	});
	return id;
}

void BrowserServer::cancelResolve(uint64 id) {
	_jobs.erase(id);
}

void BrowserServer::finishJob(uint64 id, BrowserReport report) {
	const auto i = _jobs.find(id);
	if (i == end(_jobs)) {
		for (const auto &stream : report.streams) {
			RemoveCookieFile(stream.context.cookiesFile);
		}
		return;
	}
	const auto done = std::move(i->second.done);
	_jobs.erase(i);
	if (done) {
		done(std::move(report));
	}
}

QString BrowserServer::cachedInfo(const QString &url) {
	const auto now = crl::now();
	for (auto i = begin(_analyzed); i != end(_analyzed);) {
		if (now - i->second.when > kCacheLifetime) {
			QFile::remove(i->second.infoJsonPath);
			i = _analyzed.erase(i);
		} else {
			++i;
		}
	}
	const auto i = _analyzed.find(CacheKey(url));
	if (i == end(_analyzed) || !QFileInfo(i->second.infoJsonPath).isFile()) {
		return QString();
	}
	// Each download owns (and later removes) its own copy.
	const auto copy = DataPath() + u"/info/%1.json"_q.arg(
		base::RandomValue<uint64>(),
		16,
		16,
		QChar('0'));
	return QFile::copy(i->second.infoJsonPath, copy)
		? QDir::toNativeSeparators(copy)
		: QString();
}

void BrowserServer::rememberInfo(const QString &url, const QString &path) {
	const auto key = CacheKey(url);
	if (const auto i = _analyzed.find(key); i != end(_analyzed)) {
		if (i->second.infoJsonPath != path) {
			QFile::remove(i->second.infoJsonPath);
		}
	}
	_analyzed[key] = Cached{ .infoJsonPath = path, .when = crl::now() };
}

QString BrowserServer::ExportExtension(QString *error) {
	const auto source = u":/gb/browser_extension"_q;
	const auto target = DataPath() + u"/browser-extension"_q;
	if (!QFileInfo(source + u"/manifest.json"_q).exists()) {
		if (error) {
			*error = u"The extension is missing from this build."_q;
		}
		return QString();
	}
	QDir().mkpath(target);
	auto iterator = QDirIterator(
		source,
		QDir::Files,
		QDirIterator::Subdirectories);
	while (iterator.hasNext()) {
		const auto path = iterator.next();
		const auto relative = path.mid(source.size() + 1);
		const auto destination = target + '/' + relative;
		QDir().mkpath(QFileInfo(destination).absolutePath());
		auto input = QFile(path);
		auto output = QFile(destination);
		if (!input.open(QIODevice::ReadOnly)
			|| !output.open(QIODevice::WriteOnly)) {
			if (error) {
				*error = u"Could not write %1"_q.arg(
					QDir::toNativeSeparators(destination));
			}
			return QString();
		}
		output.write(input.readAll());
	}
	return target;
}

} // namespace GB::Downloader
