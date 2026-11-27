/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/transfers/gb_tr_daemon.h"

#include "base/unixtime.h"
#include "gb/downloader/gb_dl_settings.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace GB::Transfers {
namespace {

constexpr auto kFastPoll = 1000;
constexpr auto kSlowPoll = 4000;
constexpr auto kConnectAttempts = 60;
constexpr auto kMaxHandled = 500;
constexpr auto kMaxPending = 100;

Daemon *GlobalDaemon = nullptr;

[[nodiscard]] QString Base() {
	return QStandardPaths::writableLocation(
		QStandardPaths::GenericDataLocation) + u"/GB"_q;
}

// Same folder the p2pd CLI uses by default (%LocalAppData%\GB\p2pd).
[[nodiscard]] QString EngineDataDir() {
	return Base() + u"/p2pd"_q;
}

[[nodiscard]] QString SettingsPath() {
	const auto dir = Base() + u"/transfers"_q;
	QDir().mkpath(dir);
	return dir + u"/settings.json"_q;
}

[[nodiscard]] QString FindEngine() {
	const auto exe = u"p2pd.exe"_q;
	const auto appDir = QCoreApplication::applicationDirPath();
	for (const auto &candidate : {
		appDir + u"/engine/"_q + exe,
		appDir + '/' + exe,
		Downloader::EngineFolder() + '/' + exe,
	}) {
		if (QFileInfo(candidate).isFile()) {
			return QDir::toNativeSeparators(candidate);
		}
	}
	return QString();
}

[[nodiscard]] QString HandledKey(const InviteInfo &info) {
	return info.id + ':' + info.token.left(16);
}

[[nodiscard]] QString LogTail() {
	auto file = QFile(EngineDataDir() + u"/p2pd.log"_q);
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	const auto size = file.size();
	file.seek(std::max<qint64>(0, size - 600));
	auto lines = QString::fromUtf8(file.readAll()).split(
		'\n',
		Qt::SkipEmptyParts);
	while (lines.size() > 3) {
		lines.pop_front();
	}
	return lines.join(' ').trimmed();
}

} // namespace

std::optional<InviteInfo> ParseInvite(const QString &text) {
	const auto prefix = QString::fromLatin1(kInvitePrefix);
	const auto start = text.indexOf(prefix);
	if (start < 0) {
		return std::nullopt;
	}
	auto end = start + prefix.size();
	while (end < text.size()) {
		const auto ch = text[end];
		const auto ok = (ch >= 'a' && ch <= 'z')
			|| (ch >= 'A' && ch <= 'Z')
			|| (ch >= '0' && ch <= '9')
			|| ch == '-'
			|| ch == '_';
		if (!ok) {
			break;
		}
		++end;
	}
	const auto encoded = text.mid(
		start + prefix.size(),
		end - start - prefix.size());
	const auto decoded = QByteArray::fromBase64(
		encoded.toLatin1(),
		QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
	const auto object = QJsonDocument::fromJson(decoded).object();
	const auto version = object.value(u"v"_q).toInt();
	auto result = InviteInfo{
		.link = prefix + encoded,
		.id = object.value(u"i"_q).toString(),
		.token = object.value(u"t"_q).toString(),
		.name = object.value(u"n"_q).toString(),
		.size = int64(object.value(u"s"_q).toDouble()),
	};
	const auto addr = object.value(u"a"_q).toString();
	const auto lan = object.value(u"l"_q).toArray();
	result.lanOnly = addr.isEmpty();
	if (version < 1
		|| version > 2
		|| result.id.isEmpty()
		|| result.token.isEmpty()
		|| result.size < 0
		|| (addr.isEmpty() && lan.isEmpty())) {
		return std::nullopt;
	}
	if (result.name.isEmpty()) {
		result.name = u"file"_q;
	}
	return result;
}

QString FormatRoute(const QString &route) {
	if (route == u"lan"_q) {
		return u"Same Wi-Fi"_q;
	} else if (route == u"internet"_q) {
		return u"Internet"_q;
	} else if (route == u"direct"_q) {
		return u"Direct"_q;
	}
	return QString();
}

Daemon &Daemon::Instance() {
	if (!GlobalDaemon) {
		GlobalDaemon = new Daemon();
		QObject::connect(
			QCoreApplication::instance(),
			&QCoreApplication::aboutToQuit,
			[] {
				delete base::take(GlobalDaemon);
			});
	}
	return *GlobalDaemon;
}

Daemon::Daemon()
: _network(std::make_unique<QNetworkAccessManager>())
, _timer(std::make_unique<QTimer>()) {
	_network->setProxy(QNetworkProxy::NoProxy);
	_timer->setSingleShot(true);
	QObject::connect(_timer.get(), &QTimer::timeout, [=] { poll(); });
	loadSettings();
}

Daemon::~Daemon() {
	stop();
}

void Daemon::ensureStarted() {
	if (_started) {
		return;
	}
	_started = true;
	_status = Status();
	_status.engine = EngineState::Starting;
	notify();

	// Reuse an engine that is already running (e.g. started from the CLI).
	auto file = QFile(EngineDataDir() + u"/api.json"_q);
	if (file.open(QIODevice::ReadOnly)) {
		const auto object = QJsonDocument::fromJson(file.readAll()).object();
		file.close();
		_addr = object.value(u"addr"_q).toString();
		_key = object.value(u"key"_q).toString();
		if (!_addr.isEmpty() && !_key.isEmpty()) {
			request("GET", u"/api/status"_q, {}, [=](QByteArray data) {
				_status.engine = EngineState::Running;
				applyStatus(data);
				poll();
			}, [=](QString) {
				QFile::remove(EngineDataDir() + u"/api.json"_q);
				startProcess();
			});
			return;
		}
	}
	startProcess();
}

void Daemon::startProcess() {
	const auto engine = FindEngine();
	if (engine.isEmpty()) {
		_status.engine = EngineState::Missing;
		_status.engineError = u"p2pd.exe was not found. Put it in the "
			"\"engine\" folder next to the app or in %1."_q.arg(
				QDir::toNativeSeparators(Downloader::EngineFolder()));
		notify();
		return;
	}
	const auto data = EngineDataDir();
	QDir().mkpath(data);
	QFile::remove(data + u"/api.json"_q);
	const auto downloads = Downloader::DefaultDownloadDir();
	auto args = QStringList{
		u"serve"_q,
		u"-data"_q, QDir::toNativeSeparators(data),
		u"-download-dir"_q, downloads,
		u"-api"_q, u"127.0.0.1:0"_q,
	};
	if (_internet) {
		// -v logs magicsock/disco details (endpoints, pings, direct or
		// relay) to p2pd.log, needed to debug paths during testing.
		args.push_back(u"-tailscale"_q);
		args.push_back(u"-v"_q);
	}
	checkFirewall();
	_process = new QProcess();
	if (_internet && !_joinKey.isEmpty()) {
		// Through the environment, so the key is not in the command line.
		auto env = QProcessEnvironment::systemEnvironment();
		env.insert(u"TS_AUTHKEY"_q, _joinKey);
		_process->setProcessEnvironment(env);
	}
	_process->setProcessChannelMode(QProcess::MergedChannels);
	_process->setStandardOutputFile(data + u"/p2pd.log"_q);
	const auto process = _process;
	QObject::connect(
		process,
		&QProcess::finished,
		[=](int code, QProcess::ExitStatus) {
			if (_process != process) {
				return;
			}
			_process = nullptr;
			process->deleteLater();
			_addr = _key = QString();
			_status.engine = EngineState::Failed;
			const auto tail = LogTail();
			_status.engineError = u"The transfer engine stopped (code %1)."_q
				.arg(code) + (tail.isEmpty() ? QString() : (' ' + tail));
			_started = false;
			notify();
		});
	process->start(engine, args);
	if (!process->waitForStarted(5000)) {
		_status.engine = EngineState::Failed;
		_status.engineError = u"Could not start p2pd.exe: %1"_q.arg(
			process->errorString());
		_process = nullptr;
		process->deleteLater();
		_started = false;
		notify();
		return;
	}
	tryConnect(0);
}

void Daemon::tryConnect(int attempt) {
	if (!_process) {
		return;
	}
	auto file = QFile(EngineDataDir() + u"/api.json"_q);
	if (file.open(QIODevice::ReadOnly)) {
		const auto object = QJsonDocument::fromJson(file.readAll()).object();
		const auto pid = qint64(object.value(u"pid"_q).toDouble());
		if (pid == _process->processId()) {
			_addr = object.value(u"addr"_q).toString();
			_key = object.value(u"key"_q).toString();
			_status.engine = EngineState::Running;
			_status.engineError = QString();
			poll();
			return;
		}
	}
	if (attempt >= kConnectAttempts) {
		_status.engine = EngineState::Failed;
		_status.engineError = u"The transfer engine did not start. "_q
			+ LogTail();
		notify();
		return;
	}
	QTimer::singleShot(250, _timer.get(), [=] { tryConnect(attempt + 1); });
}

void Daemon::stop() {
	_timer->stop();
	if (const auto process = base::take(_process)) {
		process->disconnect();
		process->kill();
		process->waitForFinished(3000);
		delete process;
	}
	_addr = _key = QString();
	_started = false;
}

void Daemon::restart() {
	stop();
	_receiving.clear();
	_sending.clear();
	ensureStarted();
}

void Daemon::setWatching(bool watching) {
	_watching = std::max(0, _watching + (watching ? 1 : -1));
	if (watching) {
		ensureStarted();
		if (_status.engine == EngineState::Running && !_polling) {
			poll();
		}
	}
}

void Daemon::setInternetEnabled(bool enabled) {
	if (_internet == enabled) {
		return;
	}
	_internet = enabled;
	saveSettings();
	if (_started) {
		restart();
	}
}

void Daemon::setJoinKey(const QString &key) {
	const auto trimmed = key.trimmed();
	if (_joinKey == trimmed) {
		return;
	}
	_joinKey = trimmed;
	saveSettings();
	if (_started && _internet) {
		restart();
	}
}

FirewallState Daemon::checkFirewall() {
	if (_status.firewall == FirewallState::Fixing) {
		return _status.firewall;
	}
	const auto engine = FindEngine();
	if (engine.isEmpty()) {
		return _status.firewall;
	}
	auto process = QProcess();
	process.setProcessChannelMode(QProcess::MergedChannels);
	process.start(engine, { u"firewall"_q, u"check"_q });
	if (!process.waitForFinished(5000)) {
		process.kill();
		process.waitForFinished(1000);
		_status.firewall = FirewallState::Unknown;
		return _status.firewall;
	}
	const auto object = QJsonDocument::fromJson(
		process.readAll()).object();
	auto problems = QStringList();
	for (const auto value : object.value(u"problems"_q).toArray()) {
		problems.push_back(value.toString());
	}
	if (process.exitStatus() != QProcess::NormalExit) {
		_status.firewall = FirewallState::Unknown;
	} else if (process.exitCode() == 0) {
		_status.firewall = FirewallState::Ok;
	} else if (process.exitCode() == 3) {
		_status.firewall = FirewallState::Missing;
	} else {
		// An older p2pd without the "firewall" command.
		_status.firewall = FirewallState::Unknown;
	}
	_status.firewallProblem = problems.join(u"; "_q);
	return _status.firewall;
}

bool Daemon::shouldAskFirewall() {
	if (_firewallAsked) {
		return false;
	}
	return (checkFirewall() == FirewallState::Missing);
}

void Daemon::setFirewallAsked() {
	if (!_firewallAsked) {
		_firewallAsked = true;
		saveSettings();
	}
}

void Daemon::fixFirewall(Fn<void(bool ok, QString error)> done) {
	const auto engine = FindEngine();
	if (engine.isEmpty() || _status.firewall == FirewallState::Fixing) {
		if (done) {
			done(false, engine.isEmpty()
				? u"p2pd.exe was not found."_q
				: u"Already asking for permission."_q);
		}
		return;
	}
	_status.firewall = FirewallState::Fixing;
	notify();
	// p2pd asks for admin rights itself (UAC) and waits for its elevated
	// copy, so this process can take as long as the user needs.
	const auto process = new QProcess();
	process->setProcessChannelMode(QProcess::MergedChannels);
	QObject::connect(
		process,
		&QProcess::finished,
		[=](int code, QProcess::ExitStatus status) {
			const auto output = QString::fromUtf8(
				process->readAll()).trimmed();
			process->deleteLater();
			_status.firewall = FirewallState::Unknown;
			const auto state = checkFirewall();
			const auto ok = (status == QProcess::NormalExit)
				&& (code == 0)
				&& (state == FirewallState::Ok);
			notify();
			if (done) {
				done(ok, ok ? QString() : output.section('\n', -1));
			}
		});
	process->start(engine, { u"firewall"_q, u"add"_q });
}

void Daemon::schedulePoll() {
	const auto active = ranges::any_of(_receiving, [](const ReceiveRow &row) {
		return row.state == u"running"_q || row.state == u"queued"_q;
	}) || ranges::any_of(_sending, [](const SendRow &row) {
		return row.speed > 0.;
	});
	const auto fast = _watching > 0
		|| active
		|| _status.networkState == u"connecting"_q
		|| _status.networkState == u"needs_login"_q;
	_timer->start(fast ? kFastPoll : kSlowPoll);
}

void Daemon::poll() {
	if (_addr.isEmpty()) {
		return;
	}
	_timer->stop();
	_polling = true;
	const auto remaining = std::make_shared<int>(3);
	const auto finished = [=] {
		if (--*remaining == 0) {
			_polling = false;
			notify();
			schedulePoll();
		}
	};
	const auto failed = [=](QString) { finished(); };
	request("GET", u"/api/status"_q, {}, [=](QByteArray data) {
		applyStatus(data);
		finished();
	}, failed);
	request("GET", u"/api/downloads"_q, {}, [=](QByteArray data) {
		applyReceiving(data);
		finished();
	}, failed);
	request("GET", u"/api/offers"_q, {}, [=](QByteArray data) {
		applySending(data);
		finished();
	}, failed);
}

void Daemon::request(
		const QByteArray &verb,
		const QString &path,
		const QByteArray &body,
		Fn<void(QByteArray)> done,
		Fn<void(QString)> fail) {
	if (_addr.isEmpty()) {
		if (fail) {
			fail(u"The transfer engine is not running."_q);
		}
		return;
	}
	auto request = QNetworkRequest(QUrl(u"http://"_q + _addr + path));
	request.setRawHeader("X-GB-Key", _key.toLatin1());
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);
	request.setTransferTimeout(15000);
	const auto reply = _network->sendCustomRequest(request, verb, body);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto data = reply->readAll();
		const auto code = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (code >= 200 && code < 300) {
			if (done) {
				done(data);
			}
			return;
		}
		auto error = QJsonDocument::fromJson(data).object().value(
			u"error"_q).toString();
		if (error.isEmpty()) {
			error = reply->errorString();
		}
		if (fail) {
			fail(error);
		}
	});
}

void Daemon::applyStatus(const QByteArray &data) {
	const auto object = QJsonDocument::fromJson(data).object();
	_status.engine = EngineState::Running;
	_status.lanAddrs.clear();
	for (const auto value : object.value(u"lan"_q).toObject().value(
			u"addrs"_q).toArray()) {
		_status.lanAddrs.push_back(value.toString());
	}
	const auto network = object.value(u"network"_q).toObject();
	_status.networkState = network.value(u"state"_q).toString();
	_status.loginUrl = network.value(u"login_url"_q).toString();
	_status.networkError = network.value(u"error"_q).toString();
	_status.downloadDir = object.value(u"download_dir"_q).toString();
}

void Daemon::applyReceiving(const QByteArray &data) {
	_receiving.clear();
	for (const auto value : QJsonDocument::fromJson(data).array()) {
		const auto o = value.toObject();
		_receiving.push_back({
			.id = o.value(u"id"_q).toString(),
			.name = o.value(u"name"_q).toString(),
			.state = o.value(u"state"_q).toString(),
			.error = o.value(u"error"_q).toString(),
			.route = o.value(u"route"_q).toString(),
			.dir = o.value(u"dir"_q).toString(),
			.finalPath = o.value(u"final_path"_q).toString(),
			.inviteId = o.value(u"invite_id"_q).toString(),
			.size = int64(o.value(u"size"_q).toDouble()),
			.received = int64(o.value(u"received"_q).toDouble()),
			.progress = o.value(u"progress"_q).toDouble(),
			.speed = o.value(u"speed"_q).toDouble(),
			.eta = o.value(u"eta_seconds"_q).toDouble(),
		});
	}
}

void Daemon::applySending(const QByteArray &data) {
	const auto now = crl::now();
	auto samples = base::flat_map<QString, std::pair<int64, crl::time>>();
	_sending.clear();
	for (const auto value : QJsonDocument::fromJson(data).array()) {
		const auto o = value.toObject();
		auto row = SendRow{
			.id = o.value(u"id"_q).toString(),
			.name = o.value(u"name"_q).toString(),
			.path = o.value(u"path"_q).toString(),
			.invite = o.value(u"invite"_q).toString(),
			.size = int64(o.value(u"size"_q).toDouble()),
			.served = int64(o.value(u"bytes_served"_q).toDouble()),
		};
		const auto i = _servedSamples.find(row.id);
		if (i != end(_servedSamples) && now > i->second.second) {
			const auto bytes = row.served - i->second.first;
			row.speed = (bytes > 0)
				? (bytes * 1000. / (now - i->second.second))
				: 0.;
		}
		samples.emplace(row.id, std::make_pair(row.served, now));
		_sending.push_back(std::move(row));
	}
	_servedSamples = std::move(samples);
}

void Daemon::createOffer(
		const QString &path,
		Fn<void(SendRow)> done,
		Fn<void(QString)> fail) {
	const auto body = QJsonDocument(QJsonObject{
		{ u"path"_q, QDir::toNativeSeparators(path) },
	}).toJson(QJsonDocument::Compact);
	request("POST", u"/api/offers"_q, body, [=](QByteArray data) {
		const auto o = QJsonDocument::fromJson(data).object();
		done(SendRow{
			.id = o.value(u"id"_q).toString(),
			.name = o.value(u"name"_q).toString(),
			.path = o.value(u"path"_q).toString(),
			.invite = o.value(u"invite"_q).toString(),
			.size = int64(o.value(u"size"_q).toDouble()),
		});
		poll();
	}, fail);
}

void Daemon::deleteOffer(const QString &id) {
	request("DELETE", u"/api/offers/"_q + id, {}, [=](QByteArray) {
		poll();
	}, nullptr);
}

void Daemon::receive(
		const QString &link,
		const QString &dir,
		Fn<void()> done,
		Fn<void(QString)> fail) {
	auto object = QJsonObject{ { u"invite"_q, link } };
	if (!dir.isEmpty()) {
		object.insert(u"dir"_q, QDir::toNativeSeparators(dir));
	}
	const auto body = QJsonDocument(object).toJson(QJsonDocument::Compact);
	request("POST", u"/api/downloads"_q, body, [=](QByteArray) {
		removePending(link);
		if (const auto info = ParseInvite(link)) {
			markHandled(*info);
		}
		if (done) {
			done();
		}
		poll();
	}, fail);
}

void Daemon::pause(const QString &id) {
	request("POST", u"/api/downloads/%1/pause"_q.arg(id), {}, [=](
			QByteArray) {
		poll();
	}, nullptr);
}

void Daemon::resume(const QString &id) {
	request("POST", u"/api/downloads/%1/resume"_q.arg(id), {}, [=](
			QByteArray) {
		poll();
	}, nullptr);
}

void Daemon::remove(const QString &id, bool deleteFile) {
	const auto path = u"/api/downloads/"_q
		+ id
		+ (deleteFile ? u"?delete_file=1"_q : QString());
	request("DELETE", path, {}, [=](QByteArray) {
		poll();
	}, nullptr);
}

bool Daemon::wasHandled(const InviteInfo &info) const {
	return _handled.contains(HandledKey(info));
}

void Daemon::markHandled(const InviteInfo &info) {
	const auto key = HandledKey(info);
	if (_handled.contains(key)) {
		return;
	}
	_handled.push_back(key);
	while (_handled.size() > kMaxHandled) {
		_handled.pop_front();
	}
	saveSettings();
}

void Daemon::addPending(const QString &link, const QString &from, TimeId date) {
	if (ranges::any_of(_pending, [&](const PendingRow &row) {
		return row.link == link;
	})) {
		return;
	}
	_pending.insert(begin(_pending), PendingRow{ link, from, date });
	if (int(_pending.size()) > kMaxPending) {
		_pending.pop_back();
	}
	saveSettings();
	notify();
}

void Daemon::removePending(const QString &link) {
	const auto was = _pending.size();
	_pending.erase(ranges::remove(_pending, link, &PendingRow::link), end(_pending));
	if (_pending.size() != was) {
		saveSettings();
		notify();
	}
}

bool Daemon::isReceiving(const InviteInfo &info) const {
	return ranges::any_of(_receiving, [&](const ReceiveRow &row) {
		return row.inviteId == info.id;
	});
}

void Daemon::loadSettings() {
	auto file = QFile(SettingsPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	_internet = object.value(u"internet"_q).toBool();
	_joinKey = object.value(u"join_key"_q).toString();
	_firewallAsked = object.value(u"firewall_asked"_q).toBool();
	for (const auto value : object.value(u"handled"_q).toArray()) {
		_handled.push_back(value.toString());
	}
	for (const auto value : object.value(u"pending"_q).toArray()) {
		const auto o = value.toObject();
		const auto link = o.value(u"link"_q).toString();
		if (ParseInvite(link)) {
			_pending.push_back({
				.link = link,
				.from = o.value(u"from"_q).toString(),
				.date = TimeId(o.value(u"date"_q).toDouble()),
			});
		}
	}
}

void Daemon::saveSettings() {
	auto handled = QJsonArray();
	for (const auto &key : _handled) {
		handled.push_back(key);
	}
	auto pending = QJsonArray();
	for (const auto &row : _pending) {
		pending.push_back(QJsonObject{
			{ u"link"_q, row.link },
			{ u"from"_q, row.from },
			{ u"date"_q, double(row.date) },
		});
	}
	auto object = QJsonObject{
		{ u"internet"_q, _internet },
		{ u"firewall_asked"_q, _firewallAsked },
		{ u"handled"_q, handled },
		{ u"pending"_q, pending },
	};
	if (!_joinKey.isEmpty()) {
		object.insert(u"join_key"_q, _joinKey);
	}
	auto file = QFile(SettingsPath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(object).toJson());
	}
}

void Daemon::notify() {
	_updates.fire({});
}

} // namespace GB::Transfers
