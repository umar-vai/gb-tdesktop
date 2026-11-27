/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/updates/gb_updates.h"

#include "core/application.h"
#include "gb/feed/gb_feed_api.h"
#include "gb/gb_version.h"
#include "window/window_session_controller.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <openssl/evp.h>

namespace GB::Updates {
namespace {

constexpr auto kFirstCheckMs = 15 * 1000;
constexpr auto kCheckIntervalMs = 4 * 60 * 60 * 1000;
constexpr auto kMaxManifest = 64 * 1024;

// Raw Ed25519 public key; the private key never leaves the publisher's PC
// (D:\GB\.keys\gb-update-ed25519.pem, see installer/publish-update.ps1).
constexpr auto kPublicKeyHex =
	"2df5264a34585f80302bc3c844ebbaeb193989af6d18165054b17c61d04e41dc";

struct Manifest {
	int version = 0;
	QString versionString;
	QString url;
	int64 size = 0;
	QByteArray sha256;
	QString notes;
};

struct State {
	std::unique_ptr<QNetworkAccessManager> network;
	std::unique_ptr<QTimer> timer;
	rpl::variable<bool> ready = false;
	QString readyPath;
	QString readyVersion;
	bool checking = false;
	bool downloading = false;
};

[[nodiscard]] State &Instance() {
	static auto result = State();
	return result;
}

[[nodiscard]] QString Folder() {
	const auto result = QStandardPaths::writableLocation(
		QStandardPaths::GenericDataLocation) + u"/GB/update"_q;
	QDir().mkpath(result);
	return result;
}

[[nodiscard]] bool VerifySignature(
		const QByteArray &payload,
		const QByteArray &signature) {
	const auto key = QByteArray::fromHex(kPublicKeyHex);
	if (key.size() != 32 || signature.size() != 64) {
		return false;
	}
	const auto pkey = EVP_PKEY_new_raw_public_key(
		EVP_PKEY_ED25519,
		nullptr,
		reinterpret_cast<const unsigned char*>(key.constData()),
		size_t(key.size()));
	if (!pkey) {
		return false;
	}
	auto result = false;
	if (const auto context = EVP_MD_CTX_new()) {
		if (EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, pkey) == 1) {
			result = (EVP_DigestVerify(
				context,
				reinterpret_cast<const unsigned char*>(signature.constData()),
				size_t(signature.size()),
				reinterpret_cast<const unsigned char*>(payload.constData()),
				size_t(payload.size())) == 1);
		}
		EVP_MD_CTX_free(context);
	}
	EVP_PKEY_free(pkey);
	return result;
}

// {"payload":"<base64 json>","signature":"<base64>"}; the signature covers
// the decoded payload bytes.
[[nodiscard]] std::optional<Manifest> ParseManifest(const QByteArray &data) {
	const auto outer = QJsonDocument::fromJson(data).object();
	const auto payload = QByteArray::fromBase64(
		outer.value(u"payload"_q).toString().toLatin1());
	const auto signature = QByteArray::fromBase64(
		outer.value(u"signature"_q).toString().toLatin1());
	if (payload.isEmpty() || !VerifySignature(payload, signature)) {
		return std::nullopt;
	}
	const auto object = QJsonDocument::fromJson(payload).object();
	if (object.value(u"platform"_q).toString() != u"win64"_q
		|| object.value(u"channel"_q).toString()
			!= QString::fromLatin1(kUpdateChannel)) {
		return std::nullopt;
	}
	auto result = Manifest{
		.version = object.value(u"version"_q).toInt(),
		.versionString = object.value(u"version_string"_q).toString(),
		.url = object.value(u"url"_q).toString(),
		.size = object.value(u"size"_q).toVariant().toLongLong(),
		.sha256 = QByteArray::fromHex(
			object.value(u"sha256"_q).toString().toLatin1()),
		.notes = object.value(u"notes"_q).toString(),
	};
	if (result.url.startsWith('/')) {
		result.url = Feed::ApiBase() + result.url;
	}
	if (result.version <= 0
		|| result.size <= 0
		|| result.sha256.size() != 32
		|| !result.url.startsWith(u"https://"_q)) {
		return std::nullopt;
	}
	return result;
}

[[nodiscard]] QByteArray FileSha256(const QString &path) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	auto hash = QCryptographicHash(QCryptographicHash::Sha256);
	hash.addData(&file);
	return hash.result();
}

void SetReady(const QString &path, const Manifest &manifest) {
	auto &state = Instance();
	state.readyPath = path;
	state.readyVersion = manifest.versionString;
	state.ready = true;
}

void Download(const Manifest &manifest, Fn<void(QString)> report) {
	auto &state = Instance();
	const auto path = Folder()
		+ u"/gb-%1.zip"_q.arg(manifest.version);
	if (QFileInfo(path).size() == manifest.size
		&& FileSha256(path) == manifest.sha256) {
		SetReady(path, manifest);
		if (report) {
			report(u"GB %1 is ready to install."_q.arg(manifest.versionString));
		}
		return;
	}
	if (state.downloading) {
		return;
	}
	state.downloading = true;
	const auto partial = path + u".part"_q;
	QFile::remove(partial);
	const auto file = std::make_shared<QFile>(partial);
	if (!file->open(QIODevice::WriteOnly)) {
		state.downloading = false;
		return;
	}
	auto request = QNetworkRequest(QUrl(manifest.url));
	request.setTransferTimeout(120000);
	const auto reply = state.network->get(request);
	QObject::connect(reply, &QNetworkReply::readyRead, [=] {
		file->write(reply->readAll());
		if (file->size() > manifest.size) {
			reply->abort();
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		file->write(reply->readAll());
		file->close();
		auto &state = Instance();
		state.downloading = false;
		const auto ok = (reply->error() == QNetworkReply::NoError)
			&& (QFileInfo(partial).size() == manifest.size)
			&& (FileSha256(partial) == manifest.sha256);
		if (!ok) {
			QFile::remove(partial);
			if (report) {
				report(u"The update download failed. GB will try again "
					"later."_q);
			}
			return;
		}
		QFile::remove(path);
		if (!QFile::rename(partial, path)) {
			return;
		}
		SetReady(path, manifest);
		if (report) {
			report(u"GB %1 is ready. Press \"Update GB\" under the chat "
				"list."_q.arg(manifest.versionString));
		}
	});
}

void Check(Fn<void(QString)> report) {
	auto &state = Instance();
	if (!state.network) {
		state.network = std::make_unique<QNetworkAccessManager>();
	}
	if (state.checking || state.ready.current()) {
		if (report && state.ready.current()) {
			report(u"GB %1 is ready to install."_q.arg(state.readyVersion));
		}
		return;
	}
	state.checking = true;
	auto request = QNetworkRequest(QUrl(Feed::ApiBase()
		+ u"/v1/app/latest?platform=win64&channel="_q
		+ QString::fromLatin1(kUpdateChannel)));
	request.setTransferTimeout(30000);
	const auto reply = state.network->get(request);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		auto &state = Instance();
		state.checking = false;
		const auto data = reply->read(kMaxManifest);
		if (reply->error() != QNetworkReply::NoError) {
			if (report) {
				report(u"Could not check for updates."_q);
			}
			return;
		}
		const auto manifest = ParseManifest(data);
		if (!manifest || manifest->version <= kVersion) {
			if (report) {
				report(u"GB is up to date (%1)."_q.arg(
					QString::fromLatin1(kVersionString)));
			}
			return;
		}
		Download(*manifest, report);
	});
}

[[nodiscard]] QString ApplyScript() {
	return uR"(param([int]$ProcessId, [string]$Zip, [string]$Dest, [string]$Exe)
$ErrorActionPreference = 'Stop'
$log = Join-Path (Split-Path $Zip) 'apply.log'
function Log($m) { Add-Content -LiteralPath $log -Value ((Get-Date -Format s) + ' ' + $m) }
try {
  Log "waiting for $ProcessId"
  Wait-Process -Id $ProcessId -Timeout 90 -ErrorAction SilentlyContinue
  Get-Process p2pd -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($Dest, [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force -ErrorAction SilentlyContinue
  $tmp = Join-Path (Split-Path $Zip) 'unpacked'
  if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
  Expand-Archive -LiteralPath $Zip -DestinationPath $tmp -Force
  for ($i = 0; $i -lt 20; $i++) {
    try { Copy-Item -Path (Join-Path $tmp '*') -Destination $Dest -Recurse -Force; break }
    catch { Log "copy retry: $_"; Start-Sleep -Seconds 1 }
  }
  Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $Zip -Force -ErrorAction SilentlyContinue
  Log 'done'
} catch { Log "failed: $_" }
Start-Process -FilePath $Exe
)"_q;
}

} // namespace

void Watch(not_null<Window::SessionController*> controller) {
	auto &state = Instance();
	if (state.timer) {
		return;
	}
	state.network = std::make_unique<QNetworkAccessManager>();
	state.timer = std::make_unique<QTimer>();
	QObject::connect(state.timer.get(), &QTimer::timeout, [] {
		Instance().timer->setInterval(kCheckIntervalMs);
		Check(nullptr);
	});
	state.timer->start(kFirstCheckMs);
	QObject::connect(qApp, &QCoreApplication::aboutToQuit, [] {
		auto &state = Instance();
		state.timer = nullptr;
		state.network = nullptr;
	});
	const auto weak = base::make_weak(controller);
	ReadyValue() | rpl::filter([](bool ready) {
		return ready;
	}) | rpl::take(1) | rpl::on_next([=] {
		if (const auto strong = weak.get()) {
			strong->showToast(u"A new version of GB (%1) is ready. Press "
				"\"Update GB\" under the chat list."_q.arg(ReadyVersion()));
		}
	}, controller->lifetime());
}

bool Ready() {
	return Instance().ready.current();
}

rpl::producer<bool> ReadyValue() {
	return Instance().ready.value();
}

QString ReadyVersion() {
	return Instance().readyVersion;
}

void CheckNow(Fn<void(QString)> report) {
	Check(std::move(report));
}

void Install() {
	auto &state = Instance();
	if (!state.ready.current() || !QFile::exists(state.readyPath)) {
		return;
	}
	const auto script = Folder() + u"/apply.ps1"_q;
	{
		auto file = QFile(script);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			return;
		}
		file.write(ApplyScript().toUtf8());
	}
	const auto exe = QDir::toNativeSeparators(
		QCoreApplication::applicationFilePath());
	const auto dest = QDir::toNativeSeparators(
		QCoreApplication::applicationDirPath());
	const auto started = QProcess::startDetached(u"powershell.exe"_q, {
		u"-NoProfile"_q,
		u"-ExecutionPolicy"_q,
		u"Bypass"_q,
		u"-WindowStyle"_q,
		u"Hidden"_q,
		u"-File"_q,
		QDir::toNativeSeparators(script),
		u"-ProcessId"_q,
		QString::number(QCoreApplication::applicationPid()),
		u"-Zip"_q,
		QDir::toNativeSeparators(state.readyPath),
		u"-Dest"_q,
		dest,
		u"-Exe"_q,
		exe,
	});
	if (started) {
		Core::Quit();
	}
}

} // namespace GB::Updates
