/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace GB::Downloader {

enum class Mode : uchar {
	Video,
	Audio,
	File,
};

enum class Category : uchar {
	Video,
	Audio,
	Compressed,
	Documents,
	Programs,
	Misc,
};

struct Rule {
	QString host;
	QString folder;
	QString quality;
};

struct BatchParse {
	QStringList urls;
	int duplicates = 0;
	int invalid = 0;
	QString error;
};

inline constexpr auto kMaxBatchItems = 300;
inline constexpr auto kMaxOutstanding = 600;
inline constexpr auto kHistoryLimit = 2000;
inline constexpr auto kMaxBatchAutoRetries = 6;

[[nodiscard]] QStringList VideoQualities();
[[nodiscard]] QStringList AudioFormats();
[[nodiscard]] QStringList AudioBitrates();
[[nodiscard]] QString BestQuality();

[[nodiscard]] QString ModeKey(Mode mode);
[[nodiscard]] Mode ModeFromKey(const QString &key);

[[nodiscard]] QString SafeFilename(
	const QString &value,
	const QString &fallback = u"media_download"_q);
[[nodiscard]] QString CleanFilename(
	const QString &value,
	const QString &fallback = u"download"_q);
[[nodiscard]] QString ExtensionOf(const QString &name);
[[nodiscard]] QString NameFromUrl(const QString &url);
[[nodiscard]] bool LooksLikeFile(const QString &url);
[[nodiscard]] bool IsStreamPlaylist(const QString &url);
[[nodiscard]] bool IsMediaExtension(const QString &ext);
[[nodiscard]] bool IsPlaceholderTitle(const QString &title);
[[nodiscard]] QString FilenameFromDisposition(const QString &value);

[[nodiscard]] Category CategoryFor(const QString &name, Mode mode);

[[nodiscard]] QString HostOf(const QString &url);
[[nodiscard]] QString WebsiteOf(const QString &url);
[[nodiscard]] QString PlatformName(const QString &url);
[[nodiscard]] QString LinkKey(const QString &url);
[[nodiscard]] QString PageKey(const QString &url);
[[nodiscard]] bool IsValidWebUrl(const QString &url);

[[nodiscard]] std::vector<Rule> ParseRules(const QString &text);
[[nodiscard]] std::optional<Rule> MatchRule(
	const QString &url,
	const std::vector<Rule> &rules);

[[nodiscard]] BatchParse ParseBatchUrls(const QString &text);

[[nodiscard]] QString FormatSize(int64 bytes);
[[nodiscard]] QString FormatSpeed(float64 bytesPerSecond);
[[nodiscard]] QString FormatEta(int64 seconds);
[[nodiscard]] QString FormatDuration(int64 seconds);
[[nodiscard]] QString FormatDate(const QDateTime &date);

[[nodiscard]] QString FailureHint(const QString &errors);
[[nodiscard]] bool IsRateLimitError(const QString &error);

[[nodiscard]] bool StripPngDisguise(const QString &path);

} // namespace GB::Downloader
