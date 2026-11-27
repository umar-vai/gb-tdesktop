/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/downloader/gb_dl_tasks.h"

#include "base/call_delayed.h"
#include "base/random.h"
#include "gb/downloader/gb_dl_browser.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QMimeDatabase>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

#include <set>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace GB::Downloader {
namespace {

constexpr auto kAnalyzeTimeout = 90 * 1000;
constexpr auto kProbeTimeout = 15 * 1000;
constexpr auto kReportInterval = crl::time(250);

const auto kUserAgent = u"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
	"AppleWebKit/537.36 (KHTML, like Gecko) "
	"Chrome/155.0.0.0 Safari/537.36"_q;

const auto kWorkspacePrefix = u".gb-download-"_q;

[[nodiscard]] QSet<QString> PornhubHosts() {
	return { u"pornhub.com"_q, u"www.pornhub.com"_q, u"m.pornhub.com"_q };
}

[[nodiscard]] QString WithHost(const QString &url, const QString &host) {
	auto parsed = QUrl(url.trimmed());
	if (parsed.scheme().isEmpty()) {
		parsed.setScheme(u"https"_q);
	}
	parsed.setHost(host);
	return parsed.toString();
}

[[nodiscard]] bool IsFacebook(const QString &url) {
	return PlatformName(url) == u"Facebook"_q;
}

[[nodiscard]] bool IsYouTube(const QString &url) {
	return PlatformName(url) == u"YouTube"_q;
}

[[nodiscard]] bool IsInstagram(const QString &url) {
	return PlatformName(url) == u"Instagram"_q;
}

[[nodiscard]] bool IsDirectStream(const QString &url) {
	static const auto re = QRegularExpression(
		u"\\.(m3u8|mpd|mp4|webm|mkv|mov|m4v|m4a|mp3|ogg|opus|flac|wav)$"_q,
		QRegularExpression::CaseInsensitiveOption);
	return !HostOf(url).isEmpty()
		&& re.match(QUrl(url.trimmed()).path()).hasMatch();
}

[[nodiscard]] QString FacebookMobileWatchUrl(const QString &url) {
	if (!IsFacebook(url)) {
		return url;
	}
	const auto parsed = QUrl(url.trimmed());
	const auto parts = parsed.path().split('/', Qt::SkipEmptyParts);
	const auto isDigits = [](const QString &value) {
		static const auto re = QRegularExpression(u"^\\d+$"_q);
		return re.match(value).hasMatch();
	};
	auto videoId = QString();
	if (parts.size() >= 2
		&& parts[0].toLower() == u"reel"_q
		&& isDigits(parts[1])) {
		videoId = parts[1];
	} else if (ranges::contains(parts, u"videos"_q)) {
		for (auto i = parts.size(); i != 0; --i) {
			if (isDigits(parts[i - 1])) {
				videoId = parts[i - 1];
				break;
			}
		}
	}
	if (videoId.isEmpty()) {
		const auto query = QUrlQuery(parsed);
		auto candidate = query.queryItemValue(u"v"_q);
		if (candidate.isEmpty()) {
			candidate = query.queryItemValue(u"video_id"_q);
		}
		if (isDigits(candidate)) {
			videoId = candidate;
		}
	}
	return videoId.isEmpty()
		? url
		: (u"https://m.facebook.com/watch/?v="_q + videoId + u"&_rdr"_q);
}

[[nodiscard]] QStringList FacebookShareVariants(const QString &url) {
	if (!IsFacebook(url)) {
		return {};
	}
	const auto parts = QUrl(url.trimmed()).path().split(
		'/',
		Qt::SkipEmptyParts);
	if (parts.size() < 2
		|| parts[0].toLower() != u"share"_q
		|| !QStringList{ u"v"_q, u"r"_q, u"p"_q }.contains(parts[1].toLower())) {
		return {};
	}
	return {
		WithHost(url, u"m.facebook.com"_q),
		WithHost(url, u"mbasic.facebook.com"_q),
	};
}

[[nodiscard]] QString EpornerEmbedUrl(const QString &url) {
	const auto host = HostOf(url);
	if (host != u"eporner.com"_q && host != u"www.eporner.com"_q) {
		return QString();
	}
	static const auto re = QRegularExpression(
		u"/(?:video-|hd-porn/|embed/)([A-Za-z0-9]+)"_q);
	const auto match = re.match(QUrl(url.trimmed()).path());
	return match.hasMatch()
		? (u"https://www.eporner.com/embed/"_q + match.captured(1) + '/')
		: QString();
}

[[nodiscard]] QStringList BaseArgs() {
	auto result = QStringList{
		u"--add-header"_q,
		u"User-Agent:"_q + kUserAgent,
		u"--add-header"_q,
		u"Accept-Language:en-US,en;q=0.9"_q,
	};
	const auto proxy = ActiveProxyUrl();
	if (!proxy.isEmpty()) {
		result << u"--proxy"_q << proxy;
	}
	return result;
}

[[nodiscard]] QStringList LegacyArgs() {
	return BaseArgs() + QStringList{
		u"--compat-options"_q,
		u"prefer-legacy-http-handler"_q,
		u"--source-address"_q,
		u"0.0.0.0"_q,
	};
}

[[nodiscard]] QStringList Generic() {
	return { u"--force-generic-extractor"_q };
}

[[nodiscard]] QStringList Impersonate() {
	return { u"--impersonate"_q, u"chrome"_q };
}

[[nodiscard]] QStringList SourceAddress() {
	return { u"--source-address"_q, u"0.0.0.0"_q };
}

using AttemptList = std::vector<std::pair<QString, QStringList>>;

[[nodiscard]] AttemptList GenericTransportAttempts(
		const QString &url,
		bool allowImpersonation) {
	auto result = AttemptList{
		{ url, LegacyArgs() },
		{ url, BaseArgs() },
		{ url, LegacyArgs() + Generic() },
		{ url, BaseArgs() + Generic() },
	};
	if (allowImpersonation) {
		result.push_back({ url, BaseArgs() + Impersonate() });
		result.push_back({ url, BaseArgs() + Impersonate() + Generic() });
	}
	return result;
}

[[nodiscard]] AttemptList ExtractionAttempts(const QString &url) {
	const auto host = HostOf(url);
	if (host.isEmpty()) {
		return {};
	}
	const auto web = !IsYouTube(url) && !IsFacebook(url) && !IsInstagram(url);
	if (web && IsDirectStream(url)) {
		return {
			{ url, LegacyArgs() + Generic() },
			{ url, BaseArgs() + Generic() },
		};
	}
	if (PornhubHosts().contains(host)) {
		const auto mobile = WithHost(url, u"m.pornhub.com"_q);
		auto result = AttemptList{ { url, LegacyArgs() } };
		if (mobile != url) {
			result.push_back({ mobile, LegacyArgs() });
		}
		result.push_back({ url, LegacyArgs() + Generic() });
		if (mobile != url) {
			result.push_back({ mobile, LegacyArgs() + Generic() });
		}
		return result;
	}
	if (host == u"eporner.com"_q || host == u"www.eporner.com"_q) {
		const auto embed = EpornerEmbedUrl(url);
		auto result = AttemptList{ { url, LegacyArgs() } };
		if (!embed.isEmpty() && embed != url) {
			result.push_back({ embed, LegacyArgs() });
		}
		result.push_back({ url, LegacyArgs() + Generic() });
		if (!embed.isEmpty() && embed != url) {
			result.push_back({ embed, LegacyArgs() + Generic() });
		}
		return result;
	}
	if (IsFacebook(url)) {
		auto ordered = QStringList();
		for (const auto &candidate : FacebookShareVariants(url)
				+ QStringList{ FacebookMobileWatchUrl(url), url }) {
			if (!ordered.contains(candidate)) {
				ordered.push_back(candidate);
			}
		}
		auto result = AttemptList();
		for (const auto &candidate : ordered) {
			result.push_back({ candidate, BaseArgs() + SourceAddress() });
			result.push_back({
				candidate,
				BaseArgs() + SourceAddress() + Impersonate(),
			});
		}
		result.push_back({ url, BaseArgs() + Generic() });
		return result;
	}
	if (IsInstagram(url)) {
		return GenericTransportAttempts(url, false);
	}
	if (IsYouTube(url)) {
		return {
			{ url, BaseArgs() },
			{ url, BaseArgs() + SourceAddress() },
		};
	}
	return GenericTransportAttempts(url, true);
}

[[nodiscard]] QStringList InstalledCookieBrowsers() {
	const auto local = qEnvironmentVariable("LOCALAPPDATA");
	const auto roaming = qEnvironmentVariable("APPDATA");
	auto candidates = std::vector<std::pair<QString, QString>>();
	if (!local.isEmpty()) {
		candidates.push_back({ u"chrome"_q, local + u"/Google/Chrome/User Data"_q });
		candidates.push_back({ u"edge"_q, local + u"/Microsoft/Edge/User Data"_q });
		candidates.push_back({ u"brave"_q, local + u"/BraveSoftware/Brave-Browser/User Data"_q });
	}
	if (!roaming.isEmpty()) {
		candidates.push_back({ u"firefox"_q, roaming + u"/Mozilla/Firefox/Profiles"_q });
	}
	auto result = QStringList();
	for (const auto &[name, path] : candidates) {
		if (!result.contains(name) && QFileInfo::exists(path)) {
			result.push_back(name);
		}
	}
	return result;
}

[[nodiscard]] AttemptList BrowserSessionAttempts(const QString &url) {
	if (IsYouTube(url) || IsFacebook(url) || IsInstagram(url)) {
		return {};
	}
	auto result = AttemptList();
	for (const auto &browser : InstalledCookieBrowsers()) {
		const auto cookies = QStringList{
			u"--cookies-from-browser"_q,
			browser,
		};
		result.push_back({ url, LegacyArgs() + cookies });
		result.push_back({ url, LegacyArgs() + cookies + Generic() });
	}
	return result;
}

[[nodiscard]] QStringList CookieArgs(
		const Settings &settings,
		const QStringList &attempt,
		const BrowserContext &context) {
	if (attempt.contains(u"--cookies-from-browser"_q)
		|| attempt.contains(u"--cookies"_q)) {
		return {};
	}
	// The browser's own cookies for this very link beat any global choice.
	if (!context.cookiesFile.isEmpty()
		&& QFileInfo(context.cookiesFile).isFile()) {
		return { u"--cookies"_q, context.cookiesFile };
	}
	if (!settings.cookiesFile.isEmpty()
		&& QFileInfo(settings.cookiesFile).isFile()) {
		return { u"--cookies"_q, settings.cookiesFile };
	}
	const auto browser = settings.cookiesBrowser.trimmed().toLower();
	if (QStringList{ u"firefox"_q, u"chrome"_q, u"edge"_q, u"brave"_q }.contains(browser)) {
		return { u"--cookies-from-browser"_q, browser };
	}
	return {};
}

[[nodiscard]] QStringList ContextArgs(const BrowserContext &context) {
	auto result = QStringList();
	for (const auto &[name, value] : context.headers) {
		result << u"--add-header"_q << (name + ':' + value);
	}
	return result;
}

// Puts the browser's headers right before the "--" that ends the options,
// after the attempt's own, so the player's User-Agent and Referer win.
[[nodiscard]] QStringList WithContext(
		QStringList args,
		const BrowserContext &context) {
	const auto extra = ContextArgs(context);
	if (extra.isEmpty()) {
		return args;
	}
	const auto dash = args.indexOf(u"--"_q);
	if (dash < 0) {
		return args + extra;
	}
	for (auto i = 0; i != extra.size(); ++i) {
		args.insert(dash + i, extra[i]);
	}
	return args;
}

[[nodiscard]] bool DomainMatches(const QString &host, QString domain) {
	if (domain.startsWith('.')) {
		domain = domain.mid(1);
	}
	domain = domain.toLower();
	return !domain.isEmpty()
		&& (host == domain || host.endsWith('.' + domain));
}

[[nodiscard]] QByteArray CookieHeaderFromFile(
		const QString &path,
		const QString &url) {
	auto file = QFile(path);
	if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) {
		return QByteArray();
	}
	const auto parsed = QUrl(url);
	const auto host = parsed.host().toLower();
	const auto secure = (parsed.scheme().toLower() == u"https"_q);
	auto result = QStringList();
	for (const auto &raw : QString::fromUtf8(file.readAll()).split('\n')) {
		auto line = raw.trimmed();
		if (line.startsWith(u"#HttpOnly_"_q)) {
			line = line.mid(10);
		} else if (line.isEmpty() || line.startsWith('#')) {
			continue;
		}
		const auto parts = line.split('\t');
		if (parts.size() < 7
			|| !DomainMatches(host, parts[0])
			|| (parts[3] == u"TRUE"_q && !secure)) {
			continue;
		}
		result.push_back(parts[5] + '=' + parts[6]);
	}
	return result.join(u"; "_q).toUtf8();
}

[[nodiscard]] QString CacheDir() {
	const auto result = DataPath() + u"/yt-dlp-cache"_q;
	QDir().mkpath(result);
	return QDir::toNativeSeparators(result);
}

[[nodiscard]] QProcessEnvironment ProcessEnvironment() {
	auto result = QProcessEnvironment::systemEnvironment();
	result.insert(u"PYTHONIOENCODING"_q, u"utf-8"_q);
	result.insert(u"PYTHONUTF8"_q, u"1"_q);
	return result;
}

void KillTree(QProcess *process) {
	if (!process || process->state() == QProcess::NotRunning) {
		return;
	}
#ifdef Q_OS_WIN
	QProcess::startDetached(u"taskkill"_q, {
		u"/PID"_q,
		QString::number(process->processId()),
		u"/T"_q,
		u"/F"_q,
	});
#else // Q_OS_WIN
	process->kill();
#endif // Q_OS_WIN
}

[[nodiscard]] QString LastErrorLines(const QString &text) {
	auto result = QStringList();
	for (const auto &line : text.split('\n')) {
		const auto trimmed = line.trimmed();
		if (trimmed.startsWith(u"ERROR:"_q)) {
			result.push_back(trimmed.mid(6).trimmed());
		}
	}
	if (result.isEmpty()) {
		const auto lines = text.trimmed().split('\n');
		if (!lines.isEmpty() && !lines.back().trimmed().isEmpty()) {
			result.push_back(lines.back().trimmed());
		}
	}
	return result.join(' ').left(1000);
}

[[nodiscard]] bool HasSeparateAudio(const QJsonObject &info) {
	for (const auto &value : info.value(u"formats"_q).toArray()) {
		const auto format = value.toObject();
		const auto vcodec = format.value(u"vcodec"_q).toString();
		const auto acodec = format.value(u"acodec"_q).toString();
		if (vcodec == u"none"_q && !acodec.isEmpty() && acodec != u"none"_q) {
			return true;
		}
		const auto id = format.value(u"format_id"_q).toString().toLower();
		if (vcodec.isEmpty()
			&& (id.startsWith(u"hls-audio"_q)
				|| id.startsWith(u"audio"_q)
				|| id.startsWith(u"aud"_q))) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] QString VideoFormatSelector(
		const QString &url,
		const QString &quality,
		bool separateAudio) {
	const auto flexible = !IsYouTube(url);
	const auto best = (quality.isEmpty() || quality == BestQuality());
	const auto height = best ? 0 : QString(quality).remove('p').toInt();
	if (flexible && separateAudio) {
		if (best || height <= 0) {
			return u"bv*+ba/b/bv*"_q;
		}
		return u"bv*[height<=?%1]+ba/b[height<=?%1]/bv*+ba/b/bv*"_q.arg(height);
	}
	if (best || height <= 0) {
		return flexible
			? u"b[ext=mp4]/b/bv*[ext=mp4]+ba[ext=m4a]/bv*+ba/bv+ba"_q
			: u"bv*[ext=mp4]+ba[ext=m4a]/b[ext=mp4]/bv*+ba/b"_q;
	}
	if (flexible) {
		return (u"b[height<=%1][ext=mp4]/b[height<=%1]/b[ext=mp4]/b/"
			"bv*[height<=%1][ext=mp4]+ba[ext=m4a]/bv*[height<=%1]+ba/"
			"bv*[ext=mp4]+ba[ext=m4a]/bv*+ba/bv+ba"_q).arg(height);
	}
	return (u"bv*[height<=%1][ext=mp4]+ba[ext=m4a]/b[height<=%1][ext=mp4]/"
		"bv*[height<=%1]+ba/b[height<=%1]"_q).arg(height);
}

[[nodiscard]] QString AudioCodec(const QString &format) {
	const auto lower = format.toLower();
	return QStringList{ u"mp3"_q, u"m4a"_q, u"flac"_q, u"opus"_q, u"wav"_q }.contains(lower)
		? lower
		: u"mp3"_q;
}

[[nodiscard]] bool IsFinalMedia(const QFileInfo &info) {
	static const auto skip = QSet<QString>{
		u"srt"_q, u"vtt"_q, u"ass"_q, u"lrc"_q, u"jpg"_q, u"jpeg"_q,
		u"png"_q, u"webp"_q, u"json"_q, u"description"_q, u"part"_q,
		u"ytdl"_q, u"tmp"_q, u"temp"_q, u"mdpart"_q, u"unpng"_q,
	};
	return info.isFile()
		&& info.size() > 0
		&& !skip.contains(info.suffix().toLower())
		&& !info.fileName().startsWith('.');
}

[[nodiscard]] QString StemOf(const QFileInfo &info) {
	const auto name = info.fileName();
	const auto dot = name.lastIndexOf('.');
	return (dot > 0) ? name.left(dot) : name;
}

[[nodiscard]] QString ChooseFilename(
		const QString &suggested,
		const QString &disposition,
		const QString &url,
		const QString &contentType) {
	const auto clean = CleanFilename(suggested, QString());
	auto name = !ExtensionOf(clean).isEmpty() ? clean : QString();
	if (name.isEmpty()) {
		name = FilenameFromDisposition(disposition);
	}
	if (name.isEmpty()) {
		name = clean;
	}
	if (name.isEmpty()) {
		name = NameFromUrl(url);
	}
	if (name.isEmpty()) {
		name = u"download"_q;
	}
	if (ExtensionOf(name).isEmpty()) {
		const auto mime = QMimeDatabase().mimeTypeForName(
			contentType.split(';').value(0).trimmed());
		const auto suffix = mime.preferredSuffix();
		if (!suffix.isEmpty() && suffix != u"html"_q && suffix != u"htm"_q) {
			name += '.' + suffix;
		}
	}
	return name;
}

[[nodiscard]] QNetworkRequest MakeRequest(
		const QString &url,
		const QString &referer,
		int64 offset) {
	auto result = QNetworkRequest(QUrl(url));
	result.setRawHeader("User-Agent", kUserAgent.toUtf8());
	result.setRawHeader("Accept", "*/*");
	if (!referer.isEmpty()) {
		result.setRawHeader("Referer", referer.toUtf8());
	}
	if (offset > 0) {
		result.setRawHeader("Range", "bytes=" + QByteArray::number(offset) + '-');
	}
	result.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	return result;
}

[[nodiscard]] QString FreeTarget(const QString &folder, const QString &name) {
	const auto dot = name.lastIndexOf('.');
	const auto stem = (dot > 0) ? name.left(dot) : name;
	const auto ext = (dot > 0) ? name.mid(dot) : QString();
	for (auto index = 1; index != 1000; ++index) {
		const auto candidate = QDir(folder).filePath((index == 1)
			? name
			: (u"%1 (%2)%3"_q.arg(stem).arg(index).arg(ext)));
		if (!QFileInfo::exists(candidate)) {
			return candidate;
		}
	}
	return QDir(folder).filePath(
		stem + ' ' + QString::number(QDateTime::currentSecsSinceEpoch()) + ext);
}

} // namespace

QString BrowserContext::header(const QString &name) const {
	for (const auto &[key, value] : headers) {
		if (key.compare(name, Qt::CaseInsensitive) == 0) {
			return value;
		}
	}
	return QString();
}

QString CookieFolder() {
	const auto result = DataPath() + u"/browser-cookies"_q;
	QDir().mkpath(result);
	return result;
}

QString WriteCookieFile(const std::vector<BrowserCookie> &cookies) {
	auto lines = QStringList{ u"# Netscape HTTP Cookie File"_q };
	const auto bad = [](const QString &text) {
		return text.contains('\r')
			|| text.contains('\n')
			|| text.contains('\t');
	};
	const auto fallbackExpiry = QDateTime::currentSecsSinceEpoch()
		+ 7 * 24 * 3600;
	for (const auto &cookie : cookies) {
		auto domain = cookie.domain.trimmed();
		if (domain.isEmpty()
			|| cookie.name.isEmpty()
			|| bad(domain)
			|| bad(cookie.name)
			|| bad(cookie.value)
			|| bad(cookie.path)) {
			continue;
		}
		if (!cookie.hostOnly && !domain.startsWith('.')) {
			domain = '.' + domain;
		}
		lines.push_back(QStringList{
			(cookie.httpOnly ? u"#HttpOnly_"_q : QString()) + domain,
			cookie.hostOnly ? u"FALSE"_q : u"TRUE"_q,
			cookie.path.isEmpty() ? u"/"_q : cookie.path,
			cookie.secure ? u"TRUE"_q : u"FALSE"_q,
			QString::number((cookie.expires > 0)
				? cookie.expires
				: fallbackExpiry),
			cookie.name,
			cookie.value,
		}.join('\t'));
	}
	if (lines.size() < 2) {
		return QString();
	}
	const auto path = CookieFolder() + u"/%1.txt"_q.arg(
		base::RandomValue<uint64>(),
		16,
		16,
		QChar('0'));
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		return QString();
	}
	file.write((lines.join('\n') + '\n').toUtf8());
	return QDir::toNativeSeparators(path);
}

std::vector<BrowserCookie> CookiesFromHeader(
		const QString &url,
		const QString &header) {
	auto result = std::vector<BrowserCookie>();
	const auto parsed = QUrl(url);
	const auto host = parsed.host().toLower();
	if (host.isEmpty()) {
		return result;
	}
	for (const auto &pair : header.split(';', Qt::SkipEmptyParts)) {
		const auto equal = pair.indexOf('=');
		if (equal <= 0) {
			continue;
		}
		result.push_back({
			.domain = host,
			.name = pair.left(equal).trimmed(),
			.value = pair.mid(equal + 1).trimmed(),
			.path = u"/"_q,
			.hostOnly = true,
			.secure = (parsed.scheme().toLower() == u"https"_q),
		});
	}
	return result;
}

void RemoveCookieFile(const QString &path) {
	if (path.isEmpty()) {
		return;
	}
	const auto folder = QFileInfo(CookieFolder()).absoluteFilePath();
	if (QFileInfo(path).absolutePath().compare(
			folder,
			Qt::CaseInsensitive) == 0) {
		QFile::remove(path);
	}
}

QString AvailableName(
		const QString &folder,
		const QString &requested,
		const QStringList &sidecarSuffixes) {
	const auto base = SafeFilename(requested);
	auto existing = QSet<QString>();
	for (const auto &info : QDir(folder).entryInfoList(QDir::Files)) {
		if (IsFinalMedia(info)) {
			existing.insert(StemOf(info).toLower());
		}
	}
	auto candidate = base;
	for (auto index = 2; ; ++index) {
		auto taken = existing.contains(candidate.toLower());
		for (const auto &suffix : sidecarSuffixes) {
			taken = taken
				|| QFileInfo::exists(QDir(folder).filePath(candidate + suffix));
		}
		if (!taken) {
			return candidate;
		}
		candidate = u"%1 (%2)"_q.arg(base).arg(index);
	}
}

void ProbeFile(
		QNetworkAccessManager *network,
		const QString &url,
		Fn<void(std::optional<FileProbe>)> done) {
	if (IsStreamPlaylist(url)) {
		done(std::nullopt);
		return;
	}
	const auto reply = network->get(MakeRequest(url, QString(), 0));
	const auto finished = std::make_shared<bool>(false);
	const auto complete = [=](std::optional<FileProbe> result) {
		if (*finished) {
			return;
		}
		*finished = true;
		reply->abort();
		reply->deleteLater();
		done(std::move(result));
	};
	QTimer::singleShot(kProbeTimeout, reply, [=] {
		complete(std::nullopt);
	});
	QObject::connect(reply, &QNetworkReply::metaDataChanged, [=] {
		const auto code = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (code >= 300 && code < 400) {
			return;
		} else if (code >= 400 || code == 0) {
			complete(std::nullopt);
			return;
		}
		const auto disposition = QString::fromUtf8(
			reply->rawHeader("Content-Disposition"));
		const auto contentType = QString::fromUtf8(
			reply->rawHeader("Content-Type")).toLower();
		if (contentType.contains(u"mpegurl"_q)
			|| contentType.contains(u"dash+xml"_q)) {
			complete(std::nullopt);
			return;
		}
		if (!disposition.toLower().contains(u"attachment"_q)
			&& (contentType.isEmpty()
				|| contentType.startsWith(u"text/html"_q)
				|| contentType.startsWith(u"application/xhtml"_q))) {
			complete(std::nullopt);
			return;
		}
		complete(FileProbe{
			.name = ChooseFilename(
				QString(),
				disposition,
				reply->url().toString(),
				contentType),
			.size = reply->header(
				QNetworkRequest::ContentLengthHeader).toLongLong(),
			.contentType = contentType,
		});
	});
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		complete(std::nullopt);
	});
}

Analyzer::Analyzer(
	Settings settings,
	QString url,
	Fn<void(QString)> status,
	Fn<void(AnalyzeResult)> done,
	BrowserContext context)
: _settings(std::move(settings))
, _url(url.trimmed())
, _status(std::move(status))
, _done(std::move(done))
, _context(std::move(context)) {
}

Analyzer::~Analyzer() {
	cancel();
}

void Analyzer::setQuick(bool quick) {
	_quick = quick;
}

void Analyzer::start() {
	if (!IsValidWebUrl(_url)) {
		finish({ .error = u"Please provide a valid http:// or https:// "
			"media URL."_q });
		return;
	} else if (LooksLikeFile(_url)) {
		probeAsFile();
		return;
	}
	const auto ytDlp = FindYtDlp(_settings);
	if (ytDlp.isEmpty()) {
		probeAsFile();
		return;
	}
	for (const auto &[url, args] : ExtractionAttempts(_url)) {
		if (_quick && _attempts.size() >= 3) {
			break;
		}
		_attempts.push_back({ args, url, _context });
	}
	if (!_quick) {
		for (const auto &[url, args] : BrowserSessionAttempts(_url)) {
			_attempts.push_back({ args, url, _context });
		}
	}
	runAttempt();
}

void Analyzer::cancel() {
	_cancelled = true;
	if (_resolveId) {
		if (const auto bridge = BrowserServer::Instance()) {
			bridge->cancelResolve(_resolveId);
		}
		_resolveId = 0;
	}
	if (_process) {
		KillTree(_process.get());
		_process->waitForFinished(500);
		_process = nullptr;
	}
	_network = nullptr;
}

void Analyzer::runAttempt() {
	if (_cancelled) {
		return;
	} else if (_index >= int(_attempts.size())) {
		if (!resolveInBrowser()) {
			probeAsFile();
		}
		return;
	}
	const auto &attempt = _attempts[_index];
	if (_status) {
		_status(u"Connection %1/%2"_q.arg(_index + 1).arg(_attempts.size()));
	}
	auto args = QStringList{
		u"--ignore-config"_q,
		u"--no-warnings"_q,
		u"--no-playlist"_q,
		u"--dump-single-json"_q,
		u"--socket-timeout"_q,
		u"12"_q,
		u"--retries"_q,
		u"0"_q,
		u"--fragment-retries"_q,
		u"0"_q,
		u"--cache-dir"_q,
		CacheDir(),
	};
	args += attempt.args;
	args += CookieArgs(_settings, attempt.args, attempt.context);
	args += ContextArgs(attempt.context);
	args += u"--"_q;
	args += attempt.url;

	_process = std::make_unique<QProcess>();
	const auto process = _process.get();
	process->setProcessEnvironment(ProcessEnvironment());
	const auto guard = base::make_weak(this);
	QObject::connect(
		process,
		&QProcess::finished,
		[=](int exitCode, QProcess::ExitStatus exitStatus) {
			if (!guard || _process.get() != process) {
				return;
			}
			const auto output = process->readAllStandardOutput();
			const auto errors = QString::fromUtf8(
				process->readAllStandardError());
			const auto ok = (exitStatus == QProcess::NormalExit)
				&& (exitCode == 0)
				&& !output.trimmed().isEmpty();
			if (!ok) {
				_errors.push_back(u"Attempt %1: %2"_q.arg(_index + 1).arg(
					LastErrorLines(errors)));
			}
			attemptFinished(ok, output);
		});
	QObject::connect(process, &QProcess::errorOccurred, [=](
			QProcess::ProcessError error) {
		if (guard
			&& _process.get() == process
			&& error == QProcess::FailedToStart) {
			_errors.push_back(u"Could not start yt-dlp: "_q
				+ process->errorString());
			_index = int(_attempts.size());
			attemptFinished(false, QByteArray());
		}
	});
	QTimer::singleShot(kAnalyzeTimeout, process, [=] {
		KillTree(process);
	});
	process->start(FindYtDlp(_settings), args);
}

void Analyzer::attemptFinished(bool ok, const QByteArray &output) {
	if (_cancelled || _finished) {
		return;
	}
	if (!ok) {
		base::call_delayed(0, this, [=] {
			_process = nullptr;
			++_index;
			runAttempt();
		});
		return;
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(output, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		_errors.push_back(u"Attempt %1: unreadable media information"_q.arg(
			_index + 1));
		base::call_delayed(0, this, [=] {
			_process = nullptr;
			++_index;
			runAttempt();
		});
		return;
	}
	const auto info = document.object();
	const auto &attempt = _attempts[_index];
	const auto fromBrowser = !_stream.url.isEmpty()
		&& (attempt.url == _stream.url);
	auto result = AnalyzeResult();
	result.ok = true;
	result.url = fromBrowser ? attempt.url : _url;
	result.pageUrl = fromBrowser ? _url : QString();
	result.context = attempt.context;
	result.title = info.value(u"title"_q).toString();
	if (fromBrowser
		&& !_streamTitle.isEmpty()
		&& (result.title.isEmpty() || IsPlaceholderTitle(result.title))) {
		result.title = _streamTitle;
	}
	if (result.title.isEmpty()) {
		result.title = info.value(u"description"_q).toString().left(150);
	}
	if (result.title.isEmpty()) {
		result.title = u"Media"_q;
	}
	for (const auto key : { "channel", "uploader", "uploader_id" }) {
		if (result.creator.isEmpty()) {
			result.creator = info.value(QString::fromLatin1(key)).toString();
		}
	}
	result.duration = int64(info.value(u"duration"_q).toDouble());
	result.thumbnail = info.value(u"thumbnail"_q).toString();
	result.platform = PlatformName(_url);
	result.filename = SafeFilename(result.title);
	auto heights = std::set<int>();
	for (const auto &value : info.value(u"formats"_q).toArray()) {
		const auto format = value.toObject();
		const auto height = format.value(u"height"_q).toInt();
		if (format.value(u"vcodec"_q).toString() != u"none"_q && height > 0) {
			heights.insert(height);
		}
	}
	result.qualities.push_back(BestQuality());
	for (auto i = heights.rbegin(); i != heights.rend(); ++i) {
		result.qualities.push_back(QString::number(*i) + 'p');
	}
	const auto ext = info.value(u"ext"_q).toString();
	if (info.value(u"direct"_q).toBool() && !IsMediaExtension(ext)) {
		result.plainFile = true;
		result.filename = NameFromUrl(_url);
	} else {
		const auto folder = DataPath() + u"/info"_q;
		QDir().mkpath(folder);
		const auto path = folder
			+ u"/%1.json"_q.arg(base::RandomValue<uint64>(), 16, 16, QChar('0'));
		auto file = QFile(path);
		if (file.open(QIODevice::WriteOnly)) {
			file.write(output);
			file.close();
			result.infoJsonPath = QDir::toNativeSeparators(path);
		}
	}
	finish(std::move(result));
}

bool Analyzer::resolveInBrowser() {
	const auto bridge = BrowserServer::Instance();
	if (_quick
		|| _browserTried
		|| !bridge
		|| !bridge->extensionConnected()
		|| IsDirectStream(_url)
		|| FindYtDlp(_settings).isEmpty()) {
		return false;
	}
	_browserTried = true;
	if (_status) {
		_status(u"Opening the page in your browser to find the stream"_q);
	}
	const auto guard = base::make_weak(this);
	_resolveId = bridge->resolve(_url, [=](BrowserReport report) {
		if (!guard || _cancelled) {
			return;
		}
		_resolveId = 0;
		useBrowserReport(std::move(report));
	});
	return (_resolveId != 0);
}

void Analyzer::useBrowserReport(BrowserReport report) {
	const auto rank = [](const BrowserStream &stream) {
		return (stream.kind == u"hls"_q && stream.master)
			? 0
			: (stream.kind == u"hls"_q || stream.kind == u"dash"_q)
			? 1
			: 2;
	};
	auto chosen = (const BrowserStream*)nullptr;
	for (const auto &stream : report.streams) {
		if (IsValidWebUrl(stream.url)
			&& (!chosen || rank(stream) < rank(*chosen))) {
			chosen = &stream;
		}
	}
	if (!chosen) {
		_errors.push_back(u"Browser: the page's player loaded no stream"_q);
		probeAsFile();
		return;
	}
	_stream = *chosen;
	_streamTitle = report.title.simplified().left(180);
	_attempts.push_back({
		BaseArgs() + Generic(),
		_stream.url,
		_stream.context,
	});
	_attempts.push_back({
		LegacyArgs() + Generic(),
		_stream.url,
		_stream.context,
	});
	_index = int(_attempts.size()) - 2;
	if (_status) {
		_status(u"Reading stream qualities"_q);
	}
	runAttempt();
}

void Analyzer::probeAsFile() {
	if (_cancelled) {
		return;
	}
	if (_status) {
		_status(u"Checking the link"_q);
	}
	_network = std::make_unique<QNetworkAccessManager>();
	const auto guard = base::make_weak(this);
	ProbeFile(_network.get(), _url, [=](std::optional<FileProbe> probe) {
		if (!guard || _cancelled) {
			return;
		}
		if (probe) {
			finish({
				.ok = true,
				.plainFile = true,
				.url = _url,
				.title = probe->name,
				.platform = PlatformName(_url),
				.filename = probe->name,
				.fileSize = probe->size,
			});
			return;
		}
		auto error = u"Could not find a playable stream from this page. "_q
			+ FailureHint(_errors.join('\n'));
		if (FindYtDlp(_settings).isEmpty()) {
			error = u"The video engine (yt-dlp.exe) was not found. "
				"Put yt-dlp.exe in %1 or next to the app."_q.arg(
					QDir::toNativeSeparators(EngineFolder()));
		} else if (!_errors.isEmpty()) {
			error += u"\n\n"_q + _errors.mid(
				std::max(0, int(_errors.size()) - 3)).join('\n');
		}
		finish({ .error = error });
	});
}

void Analyzer::finish(AnalyzeResult result) {
	if (_finished || _cancelled) {
		return;
	}
	_finished = true;
	const auto done = _done;
	base::call_delayed(0, this, [=] {
		if (done) {
			done(result);
		}
	});
}

Task::Task(TaskCallbacks callbacks) : _callbacks(std::move(callbacks)) {
}

void Task::status(const QString &text) {
	if (!_finished && _callbacks.status) {
		_callbacks.status(text);
	}
}

void Task::progress(float64 value, const Metrics &metrics) {
	if (!_finished && _callbacks.progress) {
		_callbacks.progress(value, metrics);
	}
}

void Task::finish(DownloadResult result) {
	if (_finished) {
		return;
	}
	_finished = true;
	const auto done = _callbacks.done;
	base::call_delayed(0, this, [=] {
		if (done) {
			done(result);
		}
	});
}

YtDlpTask::YtDlpTask(
	Settings settings,
	DownloadRequest request,
	TaskCallbacks callbacks)
: Task(std::move(callbacks))
, _settings(std::move(settings))
, _request(std::move(request)) {
}

YtDlpTask::~YtDlpTask() {
	if (_process) {
		_stopping = true;
		KillTree(_process.get());
		_process->waitForFinished(3000);
		_process = nullptr;
	}
}

void YtDlpTask::start() {
	_ytDlp = FindYtDlp(_settings);
	_ffmpeg = FindFfmpeg(_settings);
	if (_ytDlp.isEmpty()) {
		finish({ .error = u"The video engine (yt-dlp.exe) was not found. "
			"Put yt-dlp.exe in %1 or next to the app."_q.arg(
				QDir::toNativeSeparators(EngineFolder())) });
		return;
	}
	QDir().mkpath(_request.folder);
	if (const auto existing = existingDownload(); !existing.isEmpty()) {
		status(u"Already downloaded - skipped"_q);
		finish({ .ok = true, .skipped = true, .path = existing });
		return;
	}
	const auto identity = QStringList{
		_request.url,
		_request.title,
		ModeKey(_request.mode),
		_request.quality,
		_request.audioFormat,
		_request.audioBitrate,
	}.join(QChar(0));
	const auto digest = QCryptographicHash::hash(
		identity.toUtf8(),
		QCryptographicHash::Sha256).toHex().left(24);
	_workspace = QDir(_request.folder).filePath(
		kWorkspacePrefix + QString::fromLatin1(digest));
	QDir().mkpath(_workspace + u"/work"_q);
	{
		auto title = QFile(_workspace + u"/.title"_q);
		if (title.open(QIODevice::WriteOnly)) {
			title.write(_request.title.toUtf8());
		}
	}
	const auto attempts = ExtractionAttempts(_request.url);
	const auto infoExists = !_request.infoJsonPath.isEmpty()
		&& QFileInfo(_request.infoJsonPath).isFile();
	if (infoExists) {
		auto first = attempts.empty() ? BaseArgs() : attempts.front().second;
		first.removeAll(u"--force-generic-extractor"_q);
		_attempts.push_back(first
			+ QStringList{ u"--load-info-json"_q, _request.infoJsonPath });
	}
	for (const auto &[url, args] : attempts) {
		_attempts.push_back(args + QStringList{ u"--"_q, url });
	}
	if (_attempts.empty()) {
		finish({ .error = u"Please provide a valid http:// or https:// "
			"media URL."_q });
		return;
	}
	runAttempt();
}

QString YtDlpTask::formatSelector() const {
	auto separate = false;
	if (!_request.infoJsonPath.isEmpty()) {
		auto file = QFile(_request.infoJsonPath);
		if (file.open(QIODevice::ReadOnly)) {
			separate = HasSeparateAudio(
				QJsonDocument::fromJson(file.readAll()).object());
		}
	}
	return VideoFormatSelector(_request.url, _request.quality, separate);
}

QStringList YtDlpTask::commonArgs() const {
	auto result = QStringList{
		u"--ignore-config"_q,
		u"--no-warnings"_q,
		u"--no-playlist"_q,
		u"--newline"_q,
		u"--no-colors"_q,
		u"--progress"_q,
		u"--cache-dir"_q,
		CacheDir(),
		u"--socket-timeout"_q,
		u"30"_q,
		u"--retries"_q,
		u"10"_q,
		u"--fragment-retries"_q,
		u"15"_q,
		u"--extractor-retries"_q,
		u"3"_q,
		u"--file-access-retries"_q,
		u"6"_q,
		u"--retry-sleep"_q,
		u"http:exp=1:20"_q,
		u"--retry-sleep"_q,
		u"fragment:linear=1:10:1"_q,
		u"--retry-sleep"_q,
		u"extractor:exp=1:15"_q,
		u"--abort-on-unavailable-fragments"_q,
		u"--concurrent-fragments"_q,
		_settings.fastMode ? u"16"_q : u"8"_q,
		u"--buffer-size"_q,
		u"256K"_q,
		// yt-dlp otherwise grows each read up to 4 MB and reports progress
		// only after a whole read, so on a slow line the bar froze for many
		// seconds. Fixed 256 KB reads, and at most ~4 progress lines a second.
		u"--no-resize-buffer"_q,
		u"--progress-delta"_q,
		u"0.25"_q,
		u"--no-overwrites"_q,
		u"--continue"_q,
		u"--output"_q,
		u"media.%(ext)s"_q,
		u"--paths"_q,
		u"home:"_q + QDir::toNativeSeparators(_workspace),
		u"--paths"_q,
		u"temp:"_q + QDir::toNativeSeparators(_workspace + u"/work"_q),
		u"--postprocessor-args"_q,
		u"ffmpeg:-movflags -faststart"_q,
		u"--progress-template"_q,
		u"download:GBPROG %(progress.status)s|%(progress.downloaded_bytes)s|"
		"%(progress.total_bytes)s|%(progress.total_bytes_estimate)s|"
		"%(progress.speed)s|%(progress.eta)s|%(progress.fragment_index)s|"
		"%(progress.fragment_count)s|%(info.format_id)s"_q,
		u"--progress-template"_q,
		u"postprocess:GBPP %(progress.status)s|%(progress.postprocessor)s"_q,
		u"--print"_q,
		u"before_dl:GBPLAN %(requested_formats.:.format_id)j|"
		"%(requested_formats.:.filesize)j|"
		"%(requested_formats.:.filesize_approx)j|"
		"%(format_id)s|%(filesize,filesize_approx)s"_q,
		u"--print"_q,
		u"after_move:GBFILE %(filepath)s"_q,
	};
	if (!_ffmpeg.isEmpty()) {
		result << u"--ffmpeg-location"_q << _ffmpeg;
	}
	const auto audio = (_request.mode == Mode::Audio);
	// Without ffmpeg nothing can be merged, converted or embedded: yt-dlp
	// would leave separate video/audio files and the download would fail.
	const auto ffmpeg = !_ffmpeg.isEmpty();
	if (ffmpeg && _settings.embedMetadata) {
		result << u"--embed-metadata"_q;
	}
	if (ffmpeg && _settings.embedThumbnail) {
		result << u"--embed-thumbnail"_q
			<< u"--convert-thumbnails"_q
			<< u"jpg"_q;
	}
	if (!audio && _settings.subtitles != Subtitles::Off) {
		auto langs = QStringList();
		for (const auto &lang : QString(_settings.subtitleLangs).replace(
				';',
				',').split(',', Qt::SkipEmptyParts)) {
			langs.push_back(lang.trimmed());
		}
		langs.removeAll(QString());
		result << u"--write-subs"_q
			<< u"--write-auto-subs"_q
			<< u"--sub-langs"_q
			<< (langs.isEmpty() ? u"en"_q : langs.join(','))
			<< u"--sub-format"_q
			<< u"srt/vtt/best"_q;
		if (ffmpeg && _settings.subtitles == Subtitles::Embed) {
			result << u"--embed-subs"_q;
		}
	}
	if (audio && !ffmpeg) {
		result << u"--format"_q
			<< u"bestaudio[ext=m4a]/bestaudio[ext=mp3]/bestaudio/best"_q;
	} else if (!audio && !ffmpeg) {
		const auto height = QString(_request.quality).remove('p').toInt();
		result << u"--format"_q
			<< ((height > 0)
				? u"b[height<=%1][ext=mp4]/b[height<=%1]/b"_q.arg(height)
				: u"b[ext=mp4]/b"_q);
	} else if (audio) {
		result << u"--format"_q
			<< u"bestaudio/best"_q
			<< u"--extract-audio"_q
			<< u"--audio-format"_q
			<< AudioCodec(_request.audioFormat)
			<< u"--audio-quality"_q
			<< ((_request.audioBitrate.isEmpty()
				? u"192"_q
				: _request.audioBitrate) + 'K');
	} else {
		result << u"--format"_q
			<< formatSelector()
			<< u"--merge-output-format"_q
			<< u"mp4"_q;
	}
	return result;
}

void YtDlpTask::runAttempt() {
	if (_stopping) {
		finishStopped();
		return;
	}
	if (_index >= int(_attempts.size())) {
		auto error = u"No compatible download path succeeded."_q;
		const auto from = std::max(0, int(_errors.size()) - 3);
		if (!_errors.isEmpty()) {
			error += '\n' + _errors.mid(from).join('\n');
		}
		finish({ .error = error });
		return;
	}
	status((_index == 0)
		? u"Starting"_q
		: u"Retrying (%1/%2)"_q.arg(_index + 1).arg(_attempts.size()));
	_streams.clear();
	_streamOrder.clear();
	_expectedStreams = 1;
	_plan.clear();
	_attemptErrors.clear();
	_produced.clear();
	_buffer.clear();
	_errorBuffer.clear();

	auto args = commonArgs();
	args += CookieArgs(_settings, _attempts[_index], _request.context);
	args += WithContext(_attempts[_index], _request.context);

	_process = std::make_unique<QProcess>();
	const auto process = _process.get();
	process->setProcessEnvironment(ProcessEnvironment());
	process->setWorkingDirectory(_workspace);
	const auto guard = base::make_weak(this);
	QObject::connect(process, &QProcess::readyReadStandardOutput, [=] {
		if (!guard || _process.get() != process) {
			return;
		}
		_buffer += QString::fromUtf8(process->readAllStandardOutput());
		// A line may end with '\r' (console-style progress) or '\n'.
		static const auto separator = QRegularExpression(u"[\r\n]"_q);
		auto newline = _buffer.indexOf(separator);
		while (newline >= 0) {
			handleLine(_buffer.left(newline).trimmed());
			_buffer.remove(0, newline + 1);
			newline = _buffer.indexOf(separator);
		}
		if (_buffer.size() > 1024 * 1024) {
			_buffer.clear();
		}
	});
	QObject::connect(process, &QProcess::readyReadStandardError, [=] {
		if (!guard || _process.get() != process) {
			return;
		}
		const auto text = QString::fromUtf8(process->readAllStandardError());
		_errorBuffer += text;
		auto newline = _errorBuffer.indexOf('\n');
		while (newline >= 0) {
			const auto line = _errorBuffer.left(newline).trimmed();
			_errorBuffer.remove(0, newline + 1);
			if (line.startsWith(u"GBPP "_q)) {
				handleLine(line);
			} else {
				_attemptErrors += line + '\n';
			}
			newline = _errorBuffer.indexOf('\n');
		}
		if (_attemptErrors.size() > 64 * 1024) {
			_attemptErrors = _attemptErrors.right(32 * 1024);
		}
	});
	QObject::connect(
		process,
		&QProcess::finished,
		[=](int exitCode, QProcess::ExitStatus exitStatus) {
			if (!guard || _process.get() != process) {
				return;
			}
			processFinished((exitStatus == QProcess::NormalExit)
				? exitCode
				: -1);
		});
	QObject::connect(process, &QProcess::errorOccurred, [=](
			QProcess::ProcessError error) {
		if (guard
			&& _process.get() == process
			&& error == QProcess::FailedToStart) {
			_attemptErrors = u"Could not start yt-dlp: "_q
				+ process->errorString();
			_index = int(_attempts.size()) - 1;
			processFinished(-1);
		}
	});
	process->start(_ytDlp, args);
}

void YtDlpTask::handlePlan(const QString &text) {
	// "["401","140"]|[335600758,13192871]|[...approx...]|401+140|348793629"
	// For a single format the lists are "NA" and the last two fields count.
	const auto parts = text.split('|');
	const auto list = [&](int index) {
		return QJsonDocument::fromJson(parts.value(index).toUtf8()).array();
	};
	const auto ids = list(0);
	const auto exact = list(1);
	const auto approx = list(2);
	const auto size = [](const QJsonValue &value) {
		return value.isDouble() ? int64(value.toDouble()) : int64(0);
	};
	_plan.clear();
	if (!ids.isEmpty()) {
		for (auto i = 0; i != ids.size(); ++i) {
			const auto bytes = size(exact.at(i));
			_plan.push_back({
				ids.at(i).toString(),
				bytes ? bytes : size(approx.at(i)),
			});
		}
	} else {
		auto ok = false;
		const auto bytes = parts.value(4).trimmed().toLongLong(&ok);
		_plan.push_back({ parts.value(3).trimmed(), ok ? bytes : 0 });
	}
	_expectedStreams = std::max(1, int(_plan.size()));
}

void YtDlpTask::handleLine(const QString &original) {
	auto line = original;
	// With several parallel progress lines yt-dlp prefixes "1: ", "2: ".
	static const auto numbered = QRegularExpression(u"^\\d+: (GB[A-Z]+ )"_q);
	if (const auto match = numbered.match(line); match.hasMatch()) {
		line = line.mid(match.capturedStart(1));
	}
	if (line.startsWith(u"GBPLAN "_q)) {
		handlePlan(line.mid(7));
	} else if (line.startsWith(u"GBPROG "_q)) {
		handleProgress(line.mid(7).split('|'));
	} else if (line.startsWith(u"GBPP "_q)) {
		const auto parts = line.mid(5).split('|');
		if (parts.value(0) != u"started"_q) {
			return;
		}
		const auto name = parts.value(1);
		if (name.contains(u"Merger"_q)) {
			status(u"Merging video and audio"_q);
		} else if (name.contains(u"ExtractAudio"_q)) {
			status(u"Converting audio"_q);
		} else if (name.contains(u"MoveFiles"_q)) {
			status(u"Saving file"_q);
		} else if (name.contains(u"Embed"_q) || name.contains(u"Metadata"_q)) {
			status(u"Embedding metadata"_q);
		} else {
			status(u"Processing file"_q);
		}
	} else if (line.startsWith(u"GBFILE "_q)) {
		_produced = line.mid(7).trimmed();
	} else if (line.startsWith(u"[info]"_q) && _plan.empty()) {
		static const auto re = QRegularExpression(
			u"Downloading \\d+ format\\(s\\):\\s*(\\S+)"_q);
		const auto match = re.match(line);
		if (match.hasMatch()) {
			_expectedStreams = std::max(
				1,
				int(match.captured(1).split('+').size()));
		}
	} else if (line.startsWith(u"ERROR:"_q)) {
		_attemptErrors += line + '\n';
	} else if (line.contains(u"has already been downloaded"_q)) {
		status(u"Finalizing file"_q);
	}
}

void YtDlpTask::handleProgress(const QStringList &parts) {
	const auto number = [&](int index) {
		auto ok = false;
		const auto value = parts.value(index).toDouble(&ok);
		return ok ? value : 0.;
	};
	const auto state = parts.value(0);
	const auto key = parts.value(8).isEmpty() || parts.value(8) == u"NA"_q
		? u"stream"_q
		: parts.value(8);
	if (!_streams.contains(key)) {
		for (auto &[other, stream] : _streams) {
			if (!stream.finished) {
				stream.finished = true;
				stream.fraction = 1.;
				stream.total = std::max(stream.total, stream.downloaded);
			}
		}
		_streamOrder.push_back(key);
	}
	auto &stream = _streams[key];
	const auto downloaded = int64(number(1));
	const auto exactTotal = int64(number(2));
	const auto total = exactTotal ? exactTotal : int64(number(3));
	const auto fragmentIndex = number(6);
	const auto fragmentCount = number(7);
	if (state == u"finished"_q) {
		stream.finished = true;
		stream.fraction = 1.;
		const auto bytes = exactTotal ? exactTotal : downloaded;
		if (bytes) {
			stream.total = stream.downloaded = bytes;
		}
		stream.estimated = false;
		if (int(_streams.size()) >= _expectedStreams) {
			status(u"Finalizing file"_q);
		}
		reportProgress(0., -1);
		return;
	} else if (state != u"downloading"_q) {
		return;
	}
	auto fraction = stream.fraction;
	if (fragmentCount > 0) {
		fraction = fragmentIndex / fragmentCount;
	} else if (exactTotal > 0) {
		fraction = float64(downloaded) / exactTotal;
	} else if (total > 0) {
		fraction = std::min(float64(downloaded) / total, 0.9);
	}
	stream.fraction = std::max(stream.fraction, std::clamp(fraction, 0., 1.));
	stream.downloaded = downloaded;
	stream.total = total;
	stream.estimated = (exactTotal == 0);
	reportProgress(number(4), parts.value(5) == u"NA"_q
		? -1
		: int64(number(5)));
}

void YtDlpTask::reportProgress(float64 speed, int64 eta) {
	const auto count = std::max(_expectedStreams, int(_streams.size()));

	// The weights are fixed once, from the sizes yt-dlp planned before the
	// download (GBPLAN). Switching weights mid-way made a 4K video (335 MB
	// video + 13 MB audio) crawl at half speed and then jump to 98%.
	auto planTotal = int64(0);
	auto planKnown = !_plan.empty() && (int(_plan.size()) >= count);
	for (const auto &[id, bytes] : _plan) {
		planTotal += bytes;
		planKnown = planKnown && (bytes > 0);
	}
	const auto planned = [&](const QString &key) -> int64 {
		for (const auto &[id, bytes] : _plan) {
			if (id == key) {
				return bytes;
			}
		}
		return 0;
	};
	if (planKnown) {
		for (const auto &[key, stream] : _streams) {
			if (!planned(key)) {
				planKnown = false; // A stream we did not plan for.
				break;
			}
		}
	}

	auto value = 0.;
	auto doneAll = int64(0);
	auto knownAll = int64(0);
	auto estimate = false;
	for (const auto &[key, stream] : _streams) {
		const auto weight = planKnown
			? (float64(planned(key)) / planTotal)
			: (1. / count);
		value += (stream.finished ? 1. : stream.fraction) * weight;
		doneAll += (stream.finished && stream.total)
			? stream.total
			: stream.downloaded;
		knownAll += stream.total ? stream.total : planned(key);
		estimate = estimate || (stream.estimated && !stream.finished);
	}
	// Size of the streams that have not started yet.
	for (const auto &[id, bytes] : _plan) {
		if (!_streams.contains(id)) {
			knownAll += bytes;
		}
	}
	const auto weightTotal = std::max(knownAll, planTotal);
	value = std::min(value, 0.985);
	_best = std::max(_best, value);
	auto metrics = Metrics{
		.downloaded = doneAll,
		.total = weightTotal,
		.speed = speed,
		.eta = eta,
		.estimate = estimate
			|| (!planKnown && (int(_streams.size()) < count)),
	};
	if (speed > 0. && weightTotal > doneAll) {
		metrics.eta = int64((weightTotal - doneAll) / speed);
	}
	progress(_best, metrics);
}

bool YtDlpTask::stripDisguisedStreams() {
	auto fixed = false;
	auto it = QDirIterator(
		_workspace,
		QDir::Files,
		QDirIterator::Subdirectories);
	while (it.hasNext()) {
		const auto path = it.next();
		const auto suffix = QFileInfo(path).suffix().toLower();
		if (suffix == u"part"_q || suffix == u"ytdl"_q) {
			continue;
		}
		if (StripPngDisguise(path)) {
			fixed = true;
		}
	}
	return fixed;
}

void YtDlpTask::processFinished(int exitCode) {
	if (_process) {
		_buffer += QString::fromUtf8(_process->readAllStandardOutput());
		_errorBuffer += QString::fromUtf8(_process->readAllStandardError());
		_process->disconnect();
		_process.release()->deleteLater();
	}
	for (const auto &line : _buffer.split('\n')) {
		handleLine(line.trimmed());
	}
	_buffer.clear();
	_attemptErrors += _errorBuffer;
	_errorBuffer.clear();
	if (_stopping) {
		finishStopped();
		return;
	}
	if (exitCode == 0) {
		const auto produced = findProduced();
		if (!produced.isEmpty()) {
			publish(produced);
			return;
		}
		_attemptErrors += u"Download finished, but the final file could "
			"not be located."_q;
	}
	const auto errors = LastErrorLines(_attemptErrors);
	if (!_pngFixed
		&& (_attemptErrors.contains(u"Error opening output files"_q)
			|| _attemptErrors.contains(u"Invalid data found"_q)
			|| _attemptErrors.contains(u"Postprocessing"_q))) {
		_pngFixed = true;
		if (stripDisguisedStreams()) {
			status(u"Removed fake image headers from the stream"_q);
			runAttempt();
			return;
		}
	}
	_errors.push_back(u"Attempt %1: %2"_q.arg(_index + 1).arg(errors));
	if (IsRateLimitError(errors)) {
		_index = int(_attempts.size());
	} else {
		++_index;
	}
	runAttempt();
}

QString YtDlpTask::findProduced() const {
	if (!_produced.isEmpty()) {
		const auto info = QFileInfo(_produced);
		if (IsFinalMedia(info)) {
			return info.absoluteFilePath();
		}
	}
	for (const auto &info : QDir(_workspace).entryInfoList(QDir::Files)) {
		if (IsFinalMedia(info) && StemOf(info) == u"media"_q) {
			return info.absoluteFilePath();
		}
	}
	return QString();
}

QString YtDlpTask::existingDownload() const {
	if (!_request.skipExisting || IsPlaceholderTitle(_request.title)) {
		return QString();
	}
	static const auto audioSuffixes = QSet<QString>{
		u"mp3"_q, u"m4a"_q, u"aac"_q, u"flac"_q, u"opus"_q, u"ogg"_q,
		u"wav"_q,
	};
	const auto wantedAudio = (_request.mode == Mode::Audio);
	const auto stem = SafeFilename(_request.title).toLower();
	for (const auto &info : QDir(_request.folder).entryInfoList(QDir::Files)) {
		if (StemOf(info).toLower() != stem || info.size() <= 0) {
			continue;
		}
		const auto suffix = info.suffix().toLower();
		if (IsMediaExtension(suffix)
			&& (audioSuffixes.contains(suffix) == wantedAudio)) {
			return info.absoluteFilePath();
		}
	}
	return QString();
}

void YtDlpTask::publish(const QString &produced) {
	const auto info = QFileInfo(produced);
	const auto stem = StemOf(info);
	auto sidecars = std::vector<std::pair<QString, QString>>();
	for (const auto &other : QDir(info.absolutePath()).entryInfoList(QDir::Files)) {
		const auto name = other.fileName();
		const auto suffix = other.suffix().toLower();
		if (name.startsWith(stem + '.')
			&& name != info.fileName()
			&& QStringList{ u"srt"_q, u"vtt"_q, u"ass"_q, u"lrc"_q }.contains(suffix)) {
			sidecars.push_back({ other.absoluteFilePath(), name.mid(stem.size()) });
		}
	}
	auto suffixes = QStringList();
	for (const auto &[path, suffix] : sidecars) {
		suffixes.push_back(suffix);
	}
	const auto name = AvailableName(_request.folder, _request.title, suffixes);
	const auto target = QDir(_request.folder).filePath(
		name + '.' + info.suffix());
	for (const auto &[path, suffix] : sidecars) {
		QFile::rename(path, QDir(_request.folder).filePath(name + suffix));
	}
	if (!QFile::rename(produced, target)) {
		finish({ .error = u"Could not move the finished file into %1."_q.arg(
			QDir::toNativeSeparators(_request.folder)) });
		return;
	}
	removeWorkspace();
	progress(1., Metrics{
		.downloaded = QFileInfo(target).size(),
		.total = QFileInfo(target).size(),
	});
	finish({ .ok = true, .path = QDir::toNativeSeparators(target) });
}

void YtDlpTask::removeWorkspace() {
	if (!_workspace.isEmpty()
		&& QFileInfo(_workspace).fileName().startsWith(kWorkspacePrefix)) {
		QDir(_workspace).removeRecursively();
	}
}

void YtDlpTask::finishStopped() {
	if (!_keepPartial) {
		const auto workspace = _workspace;
		const auto remove = [=] {
			if (!workspace.isEmpty()
				&& QFileInfo(workspace).fileName().startsWith(kWorkspacePrefix)) {
				QDir(workspace).removeRecursively();
			}
		};
		remove();
		if (QFileInfo::exists(workspace)) {
			QTimer::singleShot(1500, remove);
		}
	}
	finish({ .cancelled = true });
}

void YtDlpTask::stop(bool keepPartial) {
	_keepPartial = keepPartial;
	_stopping = true;
	if (_process) {
		KillTree(_process.get());
	} else {
		finishStopped();
	}
}

HttpTask::HttpTask(
	Settings settings,
	DownloadRequest request,
	TaskCallbacks callbacks)
: Task(std::move(callbacks))
, _settings(std::move(settings))
, _request(std::move(request)) {
}

HttpTask::~HttpTask() {
	_stopping = true;
	if (_reply) {
		_reply->disconnect();
		_reply->abort();
		_reply->deleteLater();
		_reply = nullptr;
	}
	_file = nullptr;
}

void HttpTask::chooseTarget(const QString &name) {
	_final = FreeTarget(_request.folder, name);
	_part = _final + u".mdpart"_q;
}

void HttpTask::start() {
	QDir().mkpath(_request.folder);
	_network = std::make_unique<QNetworkAccessManager>();
	status(u"Connecting"_q);
	const auto known = ExtensionOf(_request.title).isEmpty()
		? QString()
		: CleanFilename(_request.title, QString());
	if (!known.isEmpty()) {
		const auto partOf = [&](const QString &path) {
			return path + u".mdpart"_q;
		};
		const auto direct = QDir(_request.folder).filePath(known);
		if (QFileInfo::exists(partOf(direct)) && !QFileInfo::exists(direct)) {
			_final = direct;
			_part = partOf(direct);
		} else {
			chooseTarget(known);
		}
		request(QFileInfo(_part).size());
	} else {
		request(0);
	}
}

void HttpTask::request(int64 offset) {
	_offset = offset;
	_done = offset;
	_headersChecked = false;
	_restarting = false;
	_window.clear();
	const auto referer = _request.context.header(u"Referer"_q);
	auto request = MakeRequest(
		_request.url,
		referer.isEmpty() ? _request.pageUrl : referer,
		offset);
	for (const auto &[name, value] : _request.context.headers) {
		if (name.compare(u"Referer"_q, Qt::CaseInsensitive) != 0) {
			request.setRawHeader(name.toUtf8(), value.toUtf8());
		}
	}
	const auto cookie = CookieHeaderFromFile(
		_request.context.cookiesFile,
		_request.url);
	if (!cookie.isEmpty()) {
		request.setRawHeader("Cookie", cookie);
	}
	_reply = _network->get(request);
	const auto reply = _reply;
	QObject::connect(reply, &QNetworkReply::metaDataChanged, [=] {
		if (_reply == reply) {
			headersReceived();
		}
	});
	QObject::connect(reply, &QNetworkReply::readyRead, [=] {
		if (_reply == reply) {
			dataReceived();
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		if (_reply == reply) {
			requestFinished();
		}
	});
}

void HttpTask::headersReceived() {
	if (_headersChecked) {
		return;
	}
	const auto code = _reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (code >= 300 && code < 400) {
		return;
	}
	_headersChecked = true;
	if (code == 416 && _offset > 0) {
		_restarting = true;
		_reply->abort();
		return;
	} else if (code >= 400) {
		return;
	}
	if (_part.isEmpty()) {
		chooseTarget(ChooseFilename(
			_request.title,
			QString::fromUtf8(_reply->rawHeader("Content-Disposition")),
			_reply->url().toString(),
			QString::fromUtf8(_reply->rawHeader("Content-Type"))));
		const auto existing = QFileInfo(_part).size();
		const auto ranges = QString::fromUtf8(
			_reply->rawHeader("Accept-Ranges")).toLower().contains(u"bytes"_q);
		const auto length = _reply->header(
			QNetworkRequest::ContentLengthHeader).toLongLong();
		if (existing > 0 && ranges && (!length || existing < length)) {
			_restarting = true;
			_offset = existing;
			_reply->abort();
			return;
		}
	}
	static const auto range = QRegularExpression(
		u"^bytes (\\d+)-(\\d+)/(\\d+)$"_q);
	const auto contentRange = QString::fromUtf8(
		_reply->rawHeader("Content-Range")).trimmed();
	const auto match = range.match(contentRange);
	if (code == 206) {
		if (!match.hasMatch() || match.captured(1).toLongLong() != _offset) {
			if (_offset > 0) {
				_restarting = true;
				_offset = 0;
				QFile::remove(_part);
				_reply->abort();
				return;
			}
			fail(u"Invalid Content-Range during download."_q);
			return;
		}
		_total = match.captured(3).toLongLong();
	} else {
		_offset = 0;
		_done = 0;
		_total = _reply->header(
			QNetworkRequest::ContentLengthHeader).toLongLong();
	}
	_file = std::make_unique<QFile>(_part);
	const auto mode = (_offset > 0)
		? (QIODevice::WriteOnly | QIODevice::Append)
		: (QIODevice::WriteOnly | QIODevice::Truncate);
	if (!_file->open(mode)) {
		fail(u"Could not write %1."_q.arg(QDir::toNativeSeparators(_part)));
		return;
	}
	status((_offset > 0) ? u"Resuming download"_q : u"Downloading"_q);
}

void HttpTask::dataReceived() {
	const auto reply = _reply;
	if (!_headersChecked) {
		headersReceived();
		if (_reply != reply) {
			return;
		}
	}
	if (!_file || _restarting) {
		reply->readAll();
		return;
	}
	const auto data = reply->readAll();
	if (_file->write(data) != data.size()) {
		fail(u"Could not write to the disk (is it full?)."_q);
		return;
	}
	_done += data.size();
	const auto now = crl::now();
	_window.push_back({ now, _done });
	while (_window.size() > 2 && now - _window.front().first > 3000) {
		_window.pop_front();
	}
	if (now - _lastReport >= kReportInterval) {
		_lastReport = now;
		const auto span = now - _window.front().first;
		const auto speed = (span > 200)
			? (float64(_done - _window.front().second) * 1000. / span)
			: 0.;
		progress(_total ? std::min(0.999, float64(_done) / _total) : 0., {
			.downloaded = _done,
			.total = _total,
			.speed = speed,
			.eta = (_total && speed > 0.)
				? int64((_total - _done) / speed)
				: int64(-1),
		});
	}
}

void HttpTask::requestFinished() {
	const auto reply = _reply;
	_reply = nullptr;
	reply->deleteLater();
	if (_file) {
		_file->write(reply->readAll());
		_file->close();
	}
	if (_stopping) {
		_file = nullptr;
		if (!_keepPartial && !_part.isEmpty()) {
			QFile::remove(_part);
		}
		finish({ .cancelled = true });
		return;
	}
	if (_restarting) {
		_file = nullptr;
		request(_offset);
		return;
	}
	const auto code = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (reply->error() != QNetworkReply::NoError || code >= 400) {
		_file = nullptr;
		fail(code >= 400
			? u"HTTP Error %1: %2"_q.arg(code).arg(reply->attribute(
				QNetworkRequest::HttpReasonPhraseAttribute).toString())
			: (u"Connection failed: "_q + reply->errorString()));
		return;
	}
	if (!_file) {
		fail(u"The server sent no file."_q);
		return;
	}
	_file = nullptr;
	if (_total && QFileInfo(_part).size() != _total) {
		fail(u"Incomplete download: received %1 of %2 bytes "
			"(connection reset)."_q.arg(QFileInfo(_part).size()).arg(_total));
		return;
	}
	if (QFileInfo::exists(_final)) {
		_final = FreeTarget(_request.folder, QFileInfo(_final).fileName());
	}
	if (!QFile::rename(_part, _final)) {
		fail(u"Could not rename the finished file."_q);
		return;
	}
	const auto size = QFileInfo(_final).size();
	progress(1., { .downloaded = size, .total = size });
	finish({ .ok = true, .path = QDir::toNativeSeparators(_final) });
}

void HttpTask::fail(const QString &error) {
	if (_reply) {
		const auto reply = _reply;
		_reply = nullptr;
		reply->disconnect();
		reply->abort();
		reply->deleteLater();
	}
	_file = nullptr;
	finish({ .error = error });
}

void HttpTask::stop(bool keepPartial) {
	_keepPartial = keepPartial;
	_stopping = true;
	if (_reply) {
		_reply->abort();
	} else {
		finish({ .cancelled = true });
	}
}

} // namespace GB::Downloader
