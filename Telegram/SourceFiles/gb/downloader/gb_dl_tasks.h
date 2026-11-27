/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"
#include "gb/downloader/gb_dl_settings.h"

class QProcess;
class QNetworkAccessManager;
class QNetworkReply;
class QFile;

namespace GB::Downloader {

struct Metrics {
	int64 downloaded = 0;
	int64 total = 0;
	float64 speed = 0.;
	int64 eta = -1;
	bool estimate = false;
};

// What a browser (the GB extension) knows about a link: the headers its
// player sent and its cookies, which many CDNs require.
struct BrowserContext {
	std::vector<std::pair<QString, QString>> headers;
	QString cookiesFile;

	[[nodiscard]] bool empty() const {
		return headers.empty() && cookiesFile.isEmpty();
	}
	[[nodiscard]] QString header(const QString &name) const;
};

// A stream the extension saw a page's player load.
struct BrowserStream {
	QString url;
	QString kind;
	bool master = false;
	BrowserContext context;
};

struct BrowserReport {
	QString title;
	QString pageUrl;
	std::vector<BrowserStream> streams;
};

struct AnalyzeResult {
	bool ok = false;
	bool plainFile = false;
	QString error;
	QString url;
	QString pageUrl;
	BrowserContext context;
	QString title;
	QString creator;
	QString platform;
	QString thumbnail;
	QString filename;
	QStringList qualities;
	int64 duration = 0;
	int64 fileSize = 0;
	QString infoJsonPath;
};

struct DownloadRequest {
	QString url;
	QString pageUrl;
	QString title;
	QString folder;
	Mode mode = Mode::Video;
	QString quality;
	QString audioFormat;
	QString audioBitrate;
	QString infoJsonPath;
	BrowserContext context;
	bool skipExisting = true;
};

struct DownloadResult {
	bool ok = false;
	bool cancelled = false;
	bool skipped = false;
	QString path;
	QString error;
};

struct TaskCallbacks {
	Fn<void(QString)> status;
	Fn<void(float64, Metrics)> progress;
	Fn<void(DownloadResult)> done;
};

struct FileProbe {
	QString name;
	int64 size = 0;
	QString contentType;
};

class Analyzer final : public base::has_weak_ptr {
public:
	Analyzer(
		Settings settings,
		QString url,
		Fn<void(QString)> status,
		Fn<void(AnalyzeResult)> done,
		BrowserContext context = {});
	~Analyzer();

	// The extension waits on a menu: only the fastest attempts are tried
	// and the page is never opened in the browser.
	void setQuick(bool quick);

	void start();
	void cancel();

private:
	struct Attempt {
		QStringList args;
		QString url;
		BrowserContext context;
	};
	void runAttempt();
	void attemptFinished(bool ok, const QByteArray &output);
	bool resolveInBrowser();
	void useBrowserReport(BrowserReport report);
	void probeAsFile();
	void finish(AnalyzeResult result);

	const Settings _settings;
	const QString _url;
	const Fn<void(QString)> _status;
	const Fn<void(AnalyzeResult)> _done;
	const BrowserContext _context;
	std::vector<Attempt> _attempts;
	int _index = 0;
	QStringList _errors;
	std::unique_ptr<QProcess> _process;
	std::unique_ptr<QNetworkAccessManager> _network;
	BrowserStream _stream;
	QString _streamTitle;
	uint64 _resolveId = 0;
	bool _quick = false;
	bool _browserTried = false;
	bool _cancelled = false;
	bool _finished = false;

};

class Task : public base::has_weak_ptr {
public:
	explicit Task(TaskCallbacks callbacks);
	virtual ~Task() = default;

	virtual void start() = 0;
	virtual void stop(bool keepPartial) = 0;

protected:
	void status(const QString &text);
	void progress(float64 value, const Metrics &metrics);
	void finish(DownloadResult result);

private:
	TaskCallbacks _callbacks;
	bool _finished = false;

};

class YtDlpTask final : public Task {
public:
	YtDlpTask(Settings settings, DownloadRequest request, TaskCallbacks callbacks);
	~YtDlpTask();

	void start() override;
	void stop(bool keepPartial) override;

private:
	struct Stream {
		float64 fraction = 0.;
		int64 downloaded = 0;
		int64 total = 0;
		bool estimated = true;
		bool finished = false;
	};
	void runAttempt();
	void processFinished(int exitCode);
	void handleLine(const QString &line);
	void handleProgress(const QStringList &parts);
	void handlePlan(const QString &text);
	void reportProgress(float64 speed, int64 eta);
	[[nodiscard]] QStringList commonArgs() const;
	[[nodiscard]] QString formatSelector() const;
	[[nodiscard]] QString findProduced() const;
	[[nodiscard]] QString existingDownload() const;
	[[nodiscard]] bool stripDisguisedStreams();
	void publish(const QString &produced);
	void removeWorkspace();
	void finishStopped();

	const Settings _settings;
	const DownloadRequest _request;
	QString _workspace;
	QString _ytDlp;
	QString _ffmpeg;
	std::vector<QStringList> _attempts;
	int _index = 0;
	bool _pngFixed = false;
	QStringList _errors;
	QString _attemptErrors;
	QString _produced;
	QString _buffer;
	QString _errorBuffer;
	std::unique_ptr<QProcess> _process;
	std::map<QString, Stream> _streams;
	std::vector<QString> _streamOrder;
	int _expectedStreams = 1;
	std::vector<std::pair<QString, int64>> _plan;
	float64 _best = 0.;
	bool _stopping = false;
	bool _keepPartial = false;

};

class HttpTask final : public Task {
public:
	HttpTask(Settings settings, DownloadRequest request, TaskCallbacks callbacks);
	~HttpTask();

	void start() override;
	void stop(bool keepPartial) override;

private:
	void request(int64 offset);
	void headersReceived();
	void dataReceived();
	void requestFinished();
	void chooseTarget(const QString &name);
	void fail(const QString &error);

	const Settings _settings;
	const DownloadRequest _request;
	std::unique_ptr<QNetworkAccessManager> _network;
	QNetworkReply *_reply = nullptr;
	std::unique_ptr<QFile> _file;
	QString _final;
	QString _part;
	int64 _offset = 0;
	int64 _done = 0;
	int64 _total = 0;
	bool _headersChecked = false;
	bool _restarting = false;
	bool _stopping = false;
	bool _keepPartial = false;
	crl::time _lastReport = 0;
	std::deque<std::pair<crl::time, int64>> _window;

};

void ProbeFile(
	QNetworkAccessManager *network,
	const QString &url,
	Fn<void(std::optional<FileProbe>)> done);

// Netscape cookies.txt for yt-dlp, made from what the browser sent.
struct BrowserCookie {
	QString domain;
	QString name;
	QString value;
	QString path;
	bool hostOnly = false;
	bool secure = false;
	bool httpOnly = false;
	int64 expires = 0;
};
[[nodiscard]] QString WriteCookieFile(
	const std::vector<BrowserCookie> &cookies);
[[nodiscard]] std::vector<BrowserCookie> CookiesFromHeader(
	const QString &url,
	const QString &header);
[[nodiscard]] QString CookieFolder();
void RemoveCookieFile(const QString &path);

[[nodiscard]] QString AvailableName(
	const QString &folder,
	const QString &requested,
	const QStringList &sidecarSuffixes = {});

} // namespace GB::Downloader
