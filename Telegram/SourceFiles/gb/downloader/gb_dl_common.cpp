/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/downloader/gb_dl_common.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

namespace GB::Downloader {
namespace {

const auto kPngSignature = QByteArray("\x89PNG\r\n\x1a\n", 8);
constexpr auto kPngMaxImage = 1 << 20;
constexpr auto kPngChunk = 8 << 20;

[[nodiscard]] const QSet<QString> &FileExtensions() {
	static const auto result = QSet<QString>{
		u"zip"_q, u"rar"_q, u"7z"_q, u"tar"_q, u"gz"_q, u"tgz"_q, u"bz2"_q,
		u"xz"_q, u"zst"_q, u"iso"_q, u"img"_q, u"cab"_q, u"dmg"_q, u"exe"_q,
		u"msi"_q, u"msix"_q, u"appx"_q, u"apk"_q, u"xapk"_q, u"deb"_q,
		u"rpm"_q, u"appimage"_q, u"pkg"_q, u"bin"_q, u"pdf"_q, u"doc"_q,
		u"docx"_q, u"xls"_q, u"xlsx"_q, u"ppt"_q, u"pptx"_q, u"odt"_q,
		u"ods"_q, u"odp"_q, u"rtf"_q, u"txt"_q, u"csv"_q, u"epub"_q,
		u"torrent"_q, u"jar"_q, u"psd"_q, u"ai"_q, u"svg"_q, u"ttf"_q,
		u"otf"_q, u"woff2"_q,
	};
	return result;
}

[[nodiscard]] const QSet<QString> &StreamExtensions() {
	static const auto result = QSet<QString>{
		u"m3u8"_q, u"mpd"_q, u"ism"_q, u"f4m"_q,
	};
	return result;
}

[[nodiscard]] const QSet<QString> &MediaExtensions() {
	static const auto result = QSet<QString>{
		u"mp4"_q, u"mkv"_q, u"webm"_q, u"mov"_q, u"avi"_q, u"m4v"_q,
		u"flv"_q, u"wmv"_q, u"ts"_q, u"3gp"_q, u"mpg"_q, u"mpeg"_q,
		u"ogv"_q, u"mp3"_q, u"m4a"_q, u"aac"_q, u"flac"_q, u"opus"_q,
		u"ogg"_q, u"oga"_q, u"wav"_q, u"wma"_q, u"mka"_q,
	};
	return result;
}

[[nodiscard]] const QHash<QString, Category> &CategoryExtensions() {
	static const auto result = [] {
		auto map = QHash<QString, Category>();
		const auto add = [&](Category category, QStringList list) {
			for (const auto &ext : list) {
				map.insert(ext, category);
			}
		};
		add(Category::Video, {
			u"mp4"_q, u"mkv"_q, u"webm"_q, u"mov"_q, u"avi"_q, u"m4v"_q,
			u"flv"_q, u"wmv"_q, u"ts"_q, u"m2ts"_q, u"3gp"_q, u"mpg"_q,
			u"mpeg"_q, u"ogv"_q });
		add(Category::Audio, {
			u"mp3"_q, u"m4a"_q, u"aac"_q, u"flac"_q, u"opus"_q, u"ogg"_q,
			u"wav"_q, u"wma"_q, u"alac"_q, u"aiff"_q, u"aif"_q, u"mka"_q });
		add(Category::Compressed, {
			u"zip"_q, u"rar"_q, u"7z"_q, u"tar"_q, u"gz"_q, u"tgz"_q,
			u"bz2"_q, u"xz"_q, u"zst"_q, u"iso"_q, u"cab"_q });
		add(Category::Documents, {
			u"pdf"_q, u"doc"_q, u"docx"_q, u"xls"_q, u"xlsx"_q, u"ppt"_q,
			u"pptx"_q, u"txt"_q, u"rtf"_q, u"odt"_q, u"ods"_q, u"odp"_q,
			u"csv"_q, u"epub"_q, u"md"_q });
		add(Category::Programs, {
			u"exe"_q, u"msi"_q, u"msix"_q, u"apk"_q, u"dmg"_q, u"pkg"_q,
			u"deb"_q, u"rpm"_q, u"appimage"_q, u"bat"_q, u"cmd"_q });
		return map;
	}();
	return result;
}

[[nodiscard]] QString UrlPath(const QString &url) {
	return QUrl(url.trimmed()).path();
}

[[nodiscard]] int PngImageLength(const QByteArray &buffer, int start) {
	auto pos = start + int(kPngSignature.size());
	auto first = true;
	while (pos + 8 <= buffer.size()) {
		const auto length = (uint32(uchar(buffer[pos])) << 24)
			| (uint32(uchar(buffer[pos + 1])) << 16)
			| (uint32(uchar(buffer[pos + 2])) << 8)
			| uint32(uchar(buffer[pos + 3]));
		const auto kind = buffer.mid(pos + 4, 4);
		if (first && kind != "IHDR") {
			return -1;
		}
		first = false;
		for (const auto ch : kind) {
			if (!QChar::fromLatin1(ch).isLetter()) {
				return -1;
			}
		}
		if (length > uint32(kPngMaxImage)) {
			return -1;
		}
		pos += 12 + int(length);
		if (pos - start > kPngMaxImage || pos > buffer.size()) {
			return -1;
		}
		if (kind == "IEND") {
			return pos - start;
		}
	}
	return -1;
}

} // namespace

QStringList VideoQualities() {
	return {
		BestQuality(),
		u"2160p"_q,
		u"1440p"_q,
		u"1080p"_q,
		u"720p"_q,
		u"480p"_q,
		u"360p"_q,
	};
}

QStringList AudioFormats() {
	return { u"MP3"_q, u"M4A"_q, u"FLAC"_q, u"OPUS"_q, u"WAV"_q };
}

QStringList AudioBitrates() {
	return { u"320"_q, u"256"_q, u"192"_q, u"128"_q };
}

QString BestQuality() {
	return u"Best available"_q;
}

QString ModeKey(Mode mode) {
	switch (mode) {
	case Mode::Audio: return u"audio"_q;
	case Mode::File: return u"file"_q;
	case Mode::Video: break;
	}
	return u"video"_q;
}

Mode ModeFromKey(const QString &key) {
	const auto lower = key.trimmed().toLower();
	return (lower == u"audio"_q)
		? Mode::Audio
		: (lower == u"file"_q)
		? Mode::File
		: Mode::Video;
}

QString SafeFilename(const QString &value, const QString &fallback) {
	static const auto media = QRegularExpression(
		u"\\.(mp3|m4a|mp4|webm|mkv|mov|wav|flac|opus|ogg)$"_q,
		QRegularExpression::CaseInsensitiveOption);
	static const auto bad = QRegularExpression(
		u"[<>:\"/\\\\|?*\\x00-\\x1f]+"_q);
	static const auto spaces = QRegularExpression(u"\\s+"_q);
	auto text = value.trimmed();
	text.remove(media);
	text.replace(bad, u"_"_q);
	text.replace(spaces, u" "_q);
	while (!text.isEmpty()
		&& (text.front() == ' ' || text.front() == '.' || text.front() == '_')) {
		text.remove(0, 1);
	}
	while (!text.isEmpty()
		&& (text.back() == ' ' || text.back() == '.' || text.back() == '_')) {
		text.chop(1);
	}
	text = text.left(150).trimmed();
	return text.isEmpty() ? fallback : text;
}

QString CleanFilename(const QString &value, const QString &fallback) {
	static const auto bad = QRegularExpression(
		u"[<>:\"/\\\\|?*\\x00-\\x1f]+"_q);
	static const auto spaces = QRegularExpression(u"\\s+"_q);
	auto name = QUrl::fromPercentEncoding(value.toUtf8());
	name.replace(bad, u"_"_q);
	name = name.trimmed();
	while (!name.isEmpty() && (name.front() == '.' || name.front() == ' ')) {
		name.remove(0, 1);
	}
	while (!name.isEmpty() && (name.back() == '.' || name.back() == ' ')) {
		name.chop(1);
	}
	name.replace(spaces, u" "_q);
	if (name.size() > 180) {
		const auto dot = name.lastIndexOf('.');
		const auto ext = (dot > 0) ? name.mid(dot + 1) : QString();
		name = (!ext.isEmpty() && ext.size() <= 10)
			? (name.left(180 - ext.size() - 1) + '.' + ext)
			: name.left(180);
	}
	return name.isEmpty() ? fallback : name;
}

QString ExtensionOf(const QString &name) {
	const auto suffix = QFileInfo(name).suffix().toLower();
	if (suffix.isEmpty() || suffix.size() > 9) {
		return QString();
	}
	for (const auto ch : suffix) {
		if (!ch.isLetterOrNumber()) {
			return QString();
		}
	}
	return suffix;
}

QString NameFromUrl(const QString &url) {
	const auto path = UrlPath(url);
	const auto slash = path.lastIndexOf('/');
	return CleanFilename(
		(slash >= 0) ? path.mid(slash + 1) : path,
		QString());
}

bool LooksLikeFile(const QString &url) {
	const auto ext = ExtensionOf(NameFromUrl(url));
	return FileExtensions().contains(ext)
		&& !StreamExtensions().contains(ext);
}

bool IsStreamPlaylist(const QString &url) {
	return StreamExtensions().contains(ExtensionOf(NameFromUrl(url)));
}

bool IsMediaExtension(const QString &ext) {
	return MediaExtensions().contains(ext.toLower());
}

bool IsPlaceholderTitle(const QString &title) {
	static const auto placeholders = QSet<QString>{
		u"untitled"_q, u"untitled video"_q, u"no title"_q, u"video"_q,
		u"media"_q, u"browser media"_q, u"media download"_q,
		u"download"_q, u"watch"_q, u"player"_q, u"embed"_q, u"index"_q,
		u"master"_q, u"playlist"_q, u"stream"_q, u"videoplayback"_q,
	};
	static const auto separators = QRegularExpression(u"[\\s._-]+"_q);
	static const auto counter = QRegularExpression(u"\\s*\\(\\d+\\)$"_q);
	auto text = title;
	text.replace(separators, u" "_q);
	text = text.trimmed().toLower();
	text.remove(counter);
	return text.isEmpty() || placeholders.contains(text);
}

QString FilenameFromDisposition(const QString &value) {
	if (value.isEmpty()) {
		return QString();
	}
	static const auto extended = QRegularExpression(
		u"filename\\*\\s*=\\s*(?:UTF-8|utf-8)''([^;]+)"_q);
	static const auto plain = QRegularExpression(
		u"filename\\s*=\\s*\"?([^\";]+)\"?"_q,
		QRegularExpression::CaseInsensitiveOption);
	if (const auto match = extended.match(value); match.hasMatch()) {
		return CleanFilename(
			QUrl::fromPercentEncoding(match.captured(1).trimmed().toUtf8()),
			QString());
	}
	if (const auto match = plain.match(value); match.hasMatch()) {
		return CleanFilename(match.captured(1).trimmed(), QString());
	}
	return QString();
}

Category CategoryFor(const QString &name, Mode mode) {
	const auto i = CategoryExtensions().constFind(ExtensionOf(name));
	if (i != CategoryExtensions().constEnd()) {
		return *i;
	}
	switch (mode) {
	case Mode::Audio: return Category::Audio;
	case Mode::Video: return Category::Video;
	case Mode::File: break;
	}
	return Category::Misc;
}

QString HostOf(const QString &url) {
	const auto parsed = QUrl(url.trimmed());
	const auto scheme = parsed.scheme().toLower();
	if (scheme != u"http"_q && scheme != u"https"_q) {
		return QString();
	}
	auto host = parsed.host().toLower();
	while (host.endsWith('.')) {
		host.chop(1);
	}
	return host;
}

QString WebsiteOf(const QString &url) {
	auto host = HostOf(url);
	if (host.startsWith(u"www."_q)) {
		host = host.mid(4);
	}
	return host;
}

QString PlatformName(const QString &url) {
	const auto host = HostOf(url);
	static const auto youtube = QSet<QString>{
		u"youtu.be"_q, u"youtube.com"_q, u"www.youtube.com"_q,
		u"m.youtube.com"_q, u"music.youtube.com"_q,
		u"youtube-nocookie.com"_q, u"www.youtube-nocookie.com"_q,
	};
	static const auto facebook = QSet<QString>{
		u"facebook.com"_q, u"www.facebook.com"_q, u"m.facebook.com"_q,
		u"mbasic.facebook.com"_q, u"web.facebook.com"_q, u"fb.watch"_q,
		u"www.fb.watch"_q,
	};
	static const auto instagram = QSet<QString>{
		u"instagram.com"_q, u"www.instagram.com"_q, u"m.instagram.com"_q,
	};
	if (youtube.contains(host)) {
		return u"YouTube"_q;
	} else if (facebook.contains(host)) {
		return u"Facebook"_q;
	} else if (instagram.contains(host)) {
		return u"Instagram"_q;
	}
	const auto site = WebsiteOf(url);
	return site.isEmpty() ? u"Website"_q : site;
}

bool IsValidWebUrl(const QString &url) {
	const auto parsed = QUrl(url.trimmed(), QUrl::StrictMode);
	const auto scheme = parsed.scheme().toLower();
	return parsed.isValid()
		&& (scheme == u"http"_q || scheme == u"https"_q)
		&& !parsed.host().isEmpty()
		&& parsed.userName().isEmpty()
		&& parsed.password().isEmpty()
		&& url.size() <= 5000;
}

QString LinkKey(const QString &url) {
	static const auto share = QSet<QString>{
		u"fbclid"_q, u"gclid"_q, u"igshid"_q, u"igsh"_q, u"si"_q,
		u"feature"_q, u"mibextid"_q,
	};
	const auto parsed = QUrl(url.trimmed());
	const auto scheme = parsed.scheme().toLower();
	auto host = parsed.host().toLower();
	if ((scheme != u"http"_q && scheme != u"https"_q) || host.isEmpty()) {
		return QString();
	}
	if (host.startsWith(u"www."_q)) {
		host = host.mid(4);
	}
	if (host.startsWith(u"m."_q)) {
		host = host.mid(2);
	}
	auto path = parsed.path();
	while (path.endsWith('/')) {
		path.chop(1);
	}
	const auto query = QUrlQuery(parsed).queryItems(QUrl::FullyDecoded);
	if (host == u"youtube.com"_q
		|| host == u"music.youtube.com"_q
		|| host == u"youtu.be"_q) {
		const auto pieces = path.mid(1).split('/');
		auto video = QString();
		if (host == u"youtu.be"_q) {
			video = pieces.value(0);
		} else if (path == u"/watch"_q) {
			for (const auto &[key, value] : query) {
				if (key == u"v"_q) {
					video = value;
				}
			}
		} else if (pieces.size() == 2
			&& (pieces[0] == u"shorts"_q
				|| pieces[0] == u"embed"_q
				|| pieces[0] == u"live"_q)) {
			video = pieces[1];
		}
		if (!video.isEmpty()) {
			return u"youtube:"_q + video;
		}
	}
	auto kept = std::vector<std::pair<QString, QString>>();
	for (const auto &[key, value] : query) {
		const auto lower = key.toLower();
		if (!share.contains(lower) && !lower.startsWith(u"utm_"_q)) {
			kept.emplace_back(key, value);
		}
	}
	ranges::sort(kept);
	auto text = QStringList();
	for (const auto &[key, value] : kept) {
		text.push_back(key + '=' + value);
	}
	const auto port = parsed.port();
	return host
		+ ((port > 0) ? (':' + QString::number(port)) : QString())
		+ path
		+ (text.isEmpty() ? QString() : ('?' + text.join('&')));
}

QString PageKey(const QString &url) {
	const auto key = LinkKey(url);
	return (key.contains('/')
		|| key.contains('?')
		|| key.startsWith(u"youtube:"_q))
		? key
		: QString();
}

std::vector<Rule> ParseRules(const QString &text) {
	static const auto scheme = QRegularExpression(u"^https?://"_q);
	auto result = std::vector<Rule>();
	for (const auto &raw : text.split('\n')) {
		auto line = raw.trimmed();
		if (line.isEmpty() || line.startsWith('#')) {
			continue;
		}
		auto rule = Rule();
		const auto bar = line.lastIndexOf('|');
		if (bar >= 0) {
			auto quality = line.mid(bar + 1).trimmed();
			line = line.left(bar);
			if (quality.compare(u"best"_q, Qt::CaseInsensitive) == 0) {
				quality = BestQuality();
			}
			for (const auto &known : VideoQualities()) {
				if (known.compare(quality, Qt::CaseInsensitive) == 0) {
					rule.quality = known;
				}
			}
		}
		const auto arrow = line.indexOf(u"=>"_q);
		auto host = (arrow >= 0) ? line.left(arrow) : line;
		if (arrow >= 0) {
			rule.folder = line.mid(arrow + 2).trimmed();
			while (rule.folder.startsWith('"')) {
				rule.folder.remove(0, 1);
			}
			while (rule.folder.endsWith('"')) {
				rule.folder.chop(1);
			}
		}
		host = host.trimmed().toLower();
		host.remove(scheme);
		host = host.split('/').value(0);
		if (host.startsWith(u"www."_q)) {
			host = host.mid(4);
		}
		if (host.isEmpty()) {
			continue;
		}
		rule.host = host;
		result.push_back(std::move(rule));
	}
	return result;
}

std::optional<Rule> MatchRule(
		const QString &url,
		const std::vector<Rule> &rules) {
	const auto host = WebsiteOf(url);
	if (host.isEmpty()) {
		return std::nullopt;
	}
	auto best = std::optional<Rule>();
	for (const auto &rule : rules) {
		if (host == rule.host || host.endsWith('.' + rule.host)) {
			if (!best || rule.host.size() > best->host.size()) {
				best = rule;
			}
		}
	}
	return best;
}

BatchParse ParseBatchUrls(const QString &text) {
	static const auto url = QRegularExpression(
		u"https?://[^\\s<>\"'`]+"_q,
		QRegularExpression::CaseInsensitiveOption);
	const auto trailing = u".,;:!?)]}>\u2019\u201d"_q;
	auto result = BatchParse();
	auto seen = QSet<QString>();
	auto leftovers = text;
	auto i = url.globalMatch(text);
	while (i.hasNext()) {
		const auto match = i.next();
		auto token = match.captured(0);
		while (!token.isEmpty() && trailing.contains(token.back())) {
			token.chop(1);
		}
		if (!IsValidWebUrl(token)) {
			++result.invalid;
			continue;
		} else if (seen.contains(token)) {
			++result.duplicates;
			continue;
		}
		seen.insert(token);
		result.urls.push_back(token);
	}
	leftovers.replace(url, u" "_q);
	for (const auto &part : leftovers.split(
			QRegularExpression(u"\\s+"_q),
			Qt::SkipEmptyParts)) {
		if (part.contains(u"://"_q)) {
			++result.invalid;
		}
	}
	if (result.urls.isEmpty()) {
		result.error = u"Paste between 1 and %1 valid http(s) links, "
			"one per line."_q.arg(kMaxBatchItems);
	} else if (result.urls.size() > kMaxBatchItems) {
		result.error = u"A batch can hold up to %1 links; you pasted %2. "
			"Split it into two batches."_q.arg(kMaxBatchItems).arg(
				result.urls.size());
	}
	return result;
}

QString FormatSize(int64 bytes) {
	if (bytes <= 0) {
		return QString();
	}
	auto size = float64(bytes);
	const auto units = { u"B"_q, u"KB"_q, u"MB"_q, u"GB"_q, u"TB"_q };
	for (const auto &unit : units) {
		if (size < 1024. || unit == u"TB"_q) {
			return (unit == u"B"_q)
				? (QString::number(int64(size)) + u" B"_q)
				: (QString::number(size, 'f', 2) + ' ' + unit);
		}
		size /= 1024.;
	}
	return QString();
}

QString FormatSpeed(float64 bytesPerSecond) {
	const auto text = FormatSize(int64(bytesPerSecond));
	return text.isEmpty() ? QString() : (text + u"/s"_q);
}

QString FormatEta(int64 seconds) {
	if (seconds < 0) {
		return QString();
	} else if (seconds < 60) {
		return u"%1 sec"_q.arg(seconds);
	}
	const auto minutes = seconds / 60;
	if (minutes < 60) {
		return u"%1 min %2 sec"_q.arg(minutes).arg(
			seconds % 60,
			2,
			10,
			QChar('0'));
	}
	const auto hours = minutes / 60;
	if (hours < 48) {
		return u"%1 hr %2 min"_q.arg(hours).arg(
			minutes % 60,
			2,
			10,
			QChar('0'));
	}
	return u"%1 days"_q.arg(hours / 24);
}

QString FormatDuration(int64 seconds) {
	if (seconds <= 0) {
		return QString();
	}
	const auto hours = seconds / 3600;
	const auto minutes = (seconds / 60) % 60;
	const auto secs = seconds % 60;
	return hours
		? u"%1:%2:%3"_q.arg(hours).arg(minutes, 2, 10, QChar('0')).arg(
			secs,
			2,
			10,
			QChar('0'))
		: u"%1:%2"_q.arg(minutes).arg(secs, 2, 10, QChar('0'));
}

QString FormatDate(const QDateTime &date) {
	return date.isValid()
		? QLocale::system().toString(
			date.toLocalTime(),
			QLocale::ShortFormat)
		: QString();
}

QString FailureHint(const QString &errors) {
	const auto joined = errors.toLower();
	const auto any = [&](std::initializer_list<const char*> words) {
		for (const auto word : words) {
			if (joined.contains(QString::fromLatin1(word))) {
				return true;
			}
		}
		return false;
	};
	if (any({ "widevine", "drm protected", "drm-protected", "encrypted media" })) {
		return u"This media appears DRM-protected; protected streams "
			"cannot be downloaded."_q;
	} else if (any({ "429", "too many requests", "rate limit", "rate-limit" })) {
		return u"The website is rate-limiting requests. Reduce "
			"simultaneous downloads and retry later."_q;
	} else if (any({ "403", "401", "sign in", "login required", "cookies", "unauthorized", "forbidden" })) {
		return u"The website may require an authorized browser session "
			"or is denying access."_q;
	} else if (any({ "404", "410 gone", "video unavailable", "video has been removed" })) {
		return u"The original video URL may be expired, removed, or "
			"unavailable."_q;
	} else if (any({ "timed out", "timeout", "ssl", "tls", "connection reset", "connection aborted" })) {
		return u"A connection or TLS failure prevented extraction. "
			"Check connectivity and retry."_q;
	} else if (any({ "unsupported url", "no video formats", "no playable", "did not observe" })) {
		return u"The player did not expose a supported media stream. "
			"Try a direct, public media URL."_q;
	}
	return u"No supported media stream was detected. The page may "
		"require an interactive player."_q;
}

bool IsRateLimitError(const QString &error) {
	static const auto re = QRegularExpression(
		u"429|too many requests|rate.?limit"_q,
		QRegularExpression::CaseInsensitiveOption);
	return re.match(error).hasMatch();
}

bool StripPngDisguise(const QString &path) {
	auto source = QFile(path);
	if (!source.open(QIODevice::ReadOnly)) {
		return false;
	}
	const auto head = source.read(16);
	if (!head.startsWith(kPngSignature) || head.mid(12, 4) != "IHDR") {
		return false;
	}
	source.seek(0);
	const auto tempPath = path + u".unpng"_q;
	auto target = QFile(tempPath);
	if (!target.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	auto buffer = QByteArray();
	auto eof = false;
	auto ok = true;
	while (ok) {
		if (!eof && buffer.size() < kPngChunk + kPngMaxImage) {
			const auto data = source.read(kPngChunk);
			eof = data.isEmpty();
			buffer += data;
		}
		const auto index = buffer.indexOf(kPngSignature);
		if (index < 0) {
			const auto keep = eof
				? buffer.size()
				: std::max(0, int(buffer.size() - kPngSignature.size() + 1));
			ok = (target.write(buffer.constData(), keep) == keep);
			buffer.remove(0, keep);
			if (eof) {
				break;
			}
			continue;
		}
		ok = (target.write(buffer.constData(), index) == index);
		buffer.remove(0, index);
		if (!eof && buffer.size() < kPngMaxImage) {
			continue;
		}
		const auto size = PngImageLength(buffer, 0);
		if (size < 0) {
			ok = ok && (target.write(buffer.constData(), 1) == 1);
			buffer.remove(0, 1);
		} else {
			buffer.remove(0, size);
		}
		if (eof && buffer.isEmpty()) {
			break;
		}
	}
	source.close();
	target.close();
	if (!ok) {
		QFile::remove(tempPath);
		return false;
	}
	const auto backup = path + u".png-original"_q;
	QFile::remove(backup);
	if (!QFile::rename(path, backup)) {
		QFile::remove(tempPath);
		return false;
	} else if (!QFile::rename(tempPath, path)) {
		QFile::rename(backup, path);
		QFile::remove(tempPath);
		return false;
	}
	QFile::remove(backup);
	return true;
}

} // namespace GB::Downloader
