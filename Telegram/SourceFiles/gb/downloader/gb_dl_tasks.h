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

struct AnalyzeResult {
	bool ok = false;
	bool plainFile = false;
	QString error;
	QString url;
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
		Fn<void(AnalyzeResult)> done);
	~Analyzer();

	void start();
	void cancel();

private:
	struct Attempt {
		QStringList args;
		QString url;
	};
	void runAttempt();
	void attemptFinished(bool ok, const QByteArray &output);
	void probeAsFile();
	void finish(AnalyzeResult result);

	const Settings _settings;
	const QString _url;
	const Fn<void(QString)> _status;
	const Fn<void(AnalyzeResult)> _done;
	std::vector<Attempt> _attempts;
	int _index = 0;
	QStringList _errors;
	std::unique_ptr<QProcess> _process;
	std::unique_ptr<QNetworkAccessManager> _network;
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

[[nodiscard]] QString AvailableName(
	const QString &folder,
	const QString &requested,
	const QStringList &sidecarSuffixes = {});

} // namespace GB::Downloader
