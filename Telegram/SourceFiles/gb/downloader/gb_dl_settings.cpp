/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/downloader/gb_dl_settings.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrl>

namespace GB::Downloader {
namespace {

[[nodiscard]] QString SettingsFile() {
	return DataPath() + u"/settings.json"_q;
}

[[nodiscard]] QString SubtitlesKey(Subtitles value) {
	switch (value) {
	case Subtitles::Sidecar: return u"sidecar"_q;
	case Subtitles::Embed: return u"embed"_q;
	case Subtitles::Off: break;
	}
	return u"off"_q;
}

[[nodiscard]] Subtitles SubtitlesFromKey(const QString &key) {
	return (key == u"sidecar"_q)
		? Subtitles::Sidecar
		: (key == u"embed"_q)
		? Subtitles::Embed
		: Subtitles::Off;
}

[[nodiscard]] QString FindExecutable(
		const QString &configured,
		const QString &name) {
	if (!configured.isEmpty() && QFileInfo(configured).isFile()) {
		return QDir::toNativeSeparators(configured);
	}
	const auto exe = name + u".exe"_q;
	const auto appDir = QCoreApplication::applicationDirPath();
	for (const auto &candidate : {
		appDir + u"/engine/"_q + exe,
		appDir + '/' + exe,
		EngineFolder() + '/' + exe,
	}) {
		if (QFileInfo(candidate).isFile()) {
			return QDir::toNativeSeparators(candidate);
		}
	}
	const auto found = QStandardPaths::findExecutable(name);
	return found.isEmpty() ? QString() : QDir::toNativeSeparators(found);
}

[[nodiscard]] QString NormalizeProxy(QString raw, bool socks) {
	raw = raw.trimmed();
	if (raw.isEmpty()) {
		return QString();
	}
	if (!raw.contains(u"://"_q)) {
		raw = (socks ? u"socks5://"_q : u"http://"_q) + raw;
	}
	const auto url = QUrl(raw);
	if (url.scheme().isEmpty() || url.host().isEmpty()) {
		return QString();
	}
	return url.scheme()
		+ u"://"_q
		+ url.authority();
}

[[nodiscard]] QString WindowsProxy() {
#ifdef Q_OS_WIN
	const auto settings = QSettings(
		u"HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\"
		"CurrentVersion\\Internet Settings"_q,
		QSettings::NativeFormat);
	if (!settings.value(u"ProxyEnable"_q).toInt()) {
		return QString();
	}
	const auto server = settings.value(u"ProxyServer"_q).toString().trimmed();
	if (!server.contains('=')) {
		return NormalizeProxy(server, false);
	}
	auto entries = QMap<QString, QString>();
	for (const auto &piece : server.split(';')) {
		const auto eq = piece.indexOf('=');
		if (eq > 0) {
			entries.insert(
				piece.left(eq).trimmed().toLower(),
				piece.mid(eq + 1).trimmed());
		}
	}
	for (const auto &key : { u"https"_q, u"http"_q, u"socks"_q, u"socks5"_q }) {
		if (entries.contains(key)) {
			return NormalizeProxy(entries.value(key), key.startsWith('s'));
		}
	}
#endif // Q_OS_WIN
	return QString();
}

} // namespace

QString DataPath() {
	const auto base = QStandardPaths::writableLocation(
		QStandardPaths::GenericDataLocation);
	const auto result = base + u"/GB/downloader"_q;
	QDir().mkpath(result);
	return result;
}

QString EngineFolder() {
	const auto base = QStandardPaths::writableLocation(
		QStandardPaths::GenericDataLocation);
	return base + u"/GB/engine"_q;
}

QString DefaultDownloadDir() {
	const auto downloads = QStandardPaths::writableLocation(
		QStandardPaths::DownloadLocation);
	return QDir::toNativeSeparators(
		(downloads.isEmpty() ? QDir::homePath() : downloads) + u"/GB"_q);
}

Settings LoadSettings() {
	auto result = Settings();
	result.downloadDir = DefaultDownloadDir();
	auto file = QFile(SettingsFile());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	const auto text = [&](const char *key, QString &value) {
		const auto i = object.find(QString::fromLatin1(key));
		if (i != object.end() && i->isString()) {
			value = i->toString();
		}
	};
	const auto flag = [&](const char *key, bool &value) {
		const auto i = object.find(QString::fromLatin1(key));
		if (i != object.end() && i->isBool()) {
			value = i->toBool();
		}
	};
	text("download_dir", result.downloadDir);
	if (result.downloadDir.trimmed().isEmpty()) {
		result.downloadDir = DefaultDownloadDir();
	}
	result.defaultMode = ModeFromKey(
		object.value(u"default_mode"_q).toString());
	if (result.defaultMode == Mode::File) {
		result.defaultMode = Mode::Video;
	}
	text("video_quality", result.videoQuality);
	if (!VideoQualities().contains(result.videoQuality)) {
		result.videoQuality = u"720p"_q;
	}
	text("audio_format", result.audioFormat);
	if (!AudioFormats().contains(result.audioFormat)) {
		result.audioFormat = u"MP3"_q;
	}
	text("audio_quality", result.audioBitrate);
	if (!AudioBitrates().contains(result.audioBitrate)) {
		result.audioBitrate = u"192"_q;
	}
	result.maxConcurrent = std::clamp(
		object.value(u"max_concurrent_downloads"_q).toInt(6),
		1,
		10);
	flag("skip_existing", result.skipExisting);
	flag("organize_by_site", result.organizeBySite);
	flag("fast_mode", result.fastMode);
	flag("embed_metadata", result.embedMetadata);
	flag("embed_thumbnail", result.embedThumbnail);
	flag("catch_browser_downloads", result.catchBrowserDownloads);
	flag("confirm_browser_downloads", result.confirmBrowserDownloads);
	result.subtitles = SubtitlesFromKey(
		object.value(u"subtitles"_q).toString());
	text("subtitle_langs", result.subtitleLangs);
	text("cookies_file", result.cookiesFile);
	text("cookies_browser", result.cookiesBrowser);
	text("folder_rules", result.folderRules);
	text("yt_dlp_path", result.ytDlpPath);
	text("ffmpeg_path", result.ffmpegPath);
	const auto dirs = object.value(u"capture_dirs"_q).toObject();
	for (auto i = dirs.begin(); i != dirs.end(); ++i) {
		const auto folder = i.value().toString().trimmed();
		if (!i.key().isEmpty() && !folder.isEmpty()) {
			result.captureDirs.emplace(i.key(), folder);
		}
	}
	text("capture_last_dir", result.captureLastDir);
	return result;
}

void SaveSettings(const Settings &settings) {
	auto object = QJsonObject();
	object.insert(u"download_dir"_q, settings.downloadDir);
	object.insert(u"default_mode"_q, ModeKey(settings.defaultMode));
	object.insert(u"video_quality"_q, settings.videoQuality);
	object.insert(u"audio_format"_q, settings.audioFormat);
	object.insert(u"audio_quality"_q, settings.audioBitrate);
	object.insert(u"max_concurrent_downloads"_q, settings.maxConcurrent);
	object.insert(u"skip_existing"_q, settings.skipExisting);
	object.insert(u"organize_by_site"_q, settings.organizeBySite);
	object.insert(u"fast_mode"_q, settings.fastMode);
	object.insert(u"embed_metadata"_q, settings.embedMetadata);
	object.insert(u"embed_thumbnail"_q, settings.embedThumbnail);
	object.insert(
		u"catch_browser_downloads"_q,
		settings.catchBrowserDownloads);
	object.insert(
		u"confirm_browser_downloads"_q,
		settings.confirmBrowserDownloads);
	object.insert(u"subtitles"_q, SubtitlesKey(settings.subtitles));
	object.insert(u"subtitle_langs"_q, settings.subtitleLangs);
	object.insert(u"cookies_file"_q, settings.cookiesFile);
	object.insert(u"cookies_browser"_q, settings.cookiesBrowser);
	object.insert(u"folder_rules"_q, settings.folderRules);
	object.insert(u"yt_dlp_path"_q, settings.ytDlpPath);
	object.insert(u"ffmpeg_path"_q, settings.ffmpegPath);
	auto dirs = QJsonObject();
	for (const auto &[key, folder] : settings.captureDirs) {
		dirs.insert(key, folder);
	}
	object.insert(u"capture_dirs"_q, dirs);
	object.insert(u"capture_last_dir"_q, settings.captureLastDir);
	auto file = QSaveFile(SettingsFile());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
		file.commit();
	}
}

QStringList SubtitleOptions() {
	return { u"Off"_q, u"Sidecar file"_q, u"Embed in video"_q };
}

QStringList CookieBrowserOptions() {
	return { u"None"_q, u"Firefox"_q, u"Chrome"_q, u"Edge"_q, u"Brave"_q };
}

QString FindYtDlp(const Settings &settings) {
	return FindExecutable(settings.ytDlpPath, u"yt-dlp"_q);
}

QString FindFfmpeg(const Settings &settings) {
	return FindExecutable(settings.ffmpegPath, u"ffmpeg"_q);
}

QString ActiveProxyUrl() {
	for (const auto key : {
			"HTTPS_PROXY",
			"https_proxy",
			"HTTP_PROXY",
			"http_proxy",
			"ALL_PROXY",
			"all_proxy" }) {
		const auto value = qEnvironmentVariable(key);
		const auto socks = QByteArray(key).toUpper().startsWith("ALL");
		const auto normalized = NormalizeProxy(value, socks);
		if (!normalized.isEmpty()) {
			return normalized;
		}
	}
	return WindowsProxy();
}

QString MissingDrive(const QString &folder) {
	const auto path = QDir::toNativeSeparators(folder.trimmed());
	auto root = QString();
	if (path.size() >= 2 && path[1] == ':' && path[0].isLetter()) {
		root = path.left(2);
	} else if (path.startsWith(u"\\\\"_q)) {
		const auto parts = path.mid(2).split('\\', Qt::SkipEmptyParts);
		if (parts.size() >= 2) {
			root = u"\\\\"_q + parts[0] + '\\' + parts[1];
		}
	}
	if (root.isEmpty() || QFileInfo(root + '\\').isDir()) {
		return QString();
	}
	return root.toUpper();
}

AvailableFolder AvailableFolderFor(const QString &folder) {
	const auto drive = MissingDrive(folder);
	if (drive.isEmpty()) {
		return { .folder = folder };
	}
	return {
		.folder = DefaultDownloadDir(),
		.note = u"%1 not found - using Downloads"_q.arg(drive),
	};
}

Target ResolveTarget(
		const Settings &settings,
		const QString &url,
		const QString &quality) {
	auto result = Target{
		.folder = settings.downloadDir,
		.quality = quality,
	};
	const auto rule = MatchRule(url, ParseRules(settings.folderRules));
	if (rule) {
		if (!rule->folder.isEmpty()) {
			result.folder = rule->folder;
		}
		if (!rule->quality.isEmpty()) {
			result.quality = rule->quality;
		}
		return result;
	}
	if (settings.organizeBySite) {
		auto label = PlatformName(url);
		label = SafeFilename(label, QString());
		if (!label.isEmpty()) {
			result.folder = QDir(settings.downloadDir).filePath(label);
		}
	}
	result.folder = QDir::toNativeSeparators(result.folder);
	return result;
}

} // namespace GB::Downloader
