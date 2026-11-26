/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "gb/downloader/gb_dl_common.h"

namespace GB::Downloader {

enum class Subtitles : uchar {
	Off,
	Sidecar,
	Embed,
};

struct Settings {
	QString downloadDir;
	Mode defaultMode = Mode::Video;
	QString videoQuality = u"720p"_q;
	QString audioFormat = u"MP3"_q;
	QString audioBitrate = u"192"_q;
	int maxConcurrent = 6;
	bool skipExisting = true;
	bool organizeBySite = false;
	bool fastMode = true;
	bool embedMetadata = true;
	bool embedThumbnail = false;
	Subtitles subtitles = Subtitles::Off;
	QString subtitleLangs = u"en"_q;
	QString cookiesFile;
	QString cookiesBrowser;
	QString folderRules;
	QString ytDlpPath;
	QString ffmpegPath;
};

[[nodiscard]] QString DataPath();
[[nodiscard]] QString DefaultDownloadDir();
[[nodiscard]] Settings LoadSettings();
void SaveSettings(const Settings &settings);

[[nodiscard]] QStringList SubtitleOptions();
[[nodiscard]] QStringList CookieBrowserOptions();

[[nodiscard]] QString FindYtDlp(const Settings &settings);
[[nodiscard]] QString FindFfmpeg(const Settings &settings);
[[nodiscard]] QString EngineFolder();
[[nodiscard]] QString ActiveProxyUrl();

struct Target {
	QString folder;
	QString quality;
};
[[nodiscard]] Target ResolveTarget(
	const Settings &settings,
	const QString &url,
	const QString &quality);

} // namespace GB::Downloader
