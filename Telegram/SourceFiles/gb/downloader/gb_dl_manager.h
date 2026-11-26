/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "gb/downloader/gb_dl_tasks.h"

namespace GB::Downloader {

enum class Status : uchar {
	Queued,
	Running,
	Pausing,
	Cancelling,
	Paused,
	Completed,
	Failed,
	Cancelled,
};

enum class Display : uchar {
	Queued,
	Retrying,
	Connecting,
	Downloading,
	Finishing,
	Stopping,
	Paused,
	Complete,
	Missing,
	Failed,
	Cancelled,
};

enum class Filter : uchar {
	All,
	Unfinished,
	Finished,
	Video,
	Audio,
	Files,
};

struct Item {
	uint64 id = 0;
	QString url;
	QString pageUrl;
	QString title;
	QString folder;
	QString resultPath;
	QString platform;
	QString creator;
	Mode mode = Mode::Video;
	QString quality;
	QString audioFormat;
	QString audioBitrate;
	QString infoJsonPath;
	QString batchId;
	int batchIndex = 0;
	Status status = Status::Queued;
	float64 progress = 0.;
	Metrics metrics;
	QString detail;
	QString error;
	int autoAttempts = 0;
	int64 duration = 0;
	int64 fileSize = 0;
	bool forceRedownload = false;
	bool waitingRetry = false;
	QDateTime added;
	QDateTime started;
	QDateTime finished;
	std::unique_ptr<Task> task;
	std::unique_ptr<Analyzer> analyzer;
};

struct AddRequest {
	QString url;
	QString pageUrl;
	QString title;
	QString folder;
	Mode mode = Mode::Video;
	QString quality;
	QString audioFormat;
	QString audioBitrate;
	QString infoJsonPath;
	QString platform;
	QString creator;
	int64 duration = 0;
	int64 fileSize = 0;
	bool forceRedownload = false;
};

struct Totals {
	int downloading = 0;
	int queued = 0;
	int paused = 0;
	int failed = 0;
	float64 speed = 0.;
};

[[nodiscard]] Display DisplayOf(const Item &item);
[[nodiscard]] QString DisplayText(const Item &item);
[[nodiscard]] QString NameOf(const Item &item);
[[nodiscard]] Category CategoryOf(const Item &item);
[[nodiscard]] bool Matches(const Item &item, Filter filter, const QString &query);
[[nodiscard]] bool CanResume(const Item &item);
[[nodiscard]] bool CanStop(const Item &item);
[[nodiscard]] bool IsFinished(const Item &item);

class Manager final : public base::has_weak_ptr {
public:
	Manager();
	~Manager();

	[[nodiscard]] static Manager &Instance();

	[[nodiscard]] const Settings &settings() const {
		return _settings;
	}
	void setSettings(Settings settings);

	[[nodiscard]] std::vector<not_null<Item*>> items() const;
	[[nodiscard]] Item *find(uint64 id) const;
	[[nodiscard]] rpl::producer<> listChanges() const;
	[[nodiscard]] rpl::producer<uint64> itemUpdates() const;
	[[nodiscard]] Totals totals() const;

	uint64 add(AddRequest request);
	[[nodiscard]] QString addBatch(
		const QStringList &urls,
		Mode mode,
		const QString &quality);
	[[nodiscard]] Item *findDownloaded(
		const QString &url,
		const QString &pageUrl,
		Mode mode) const;

	void pause(uint64 id);
	void resume(uint64 id);
	void cancel(uint64 id);
	void retry(uint64 id);
	void redownload(uint64 id);
	void remove(uint64 id, bool deleteFile);
	void pauseAll();
	void resumeAll();
	int retryFailed();
	int cleanUp();

	[[nodiscard]] bool engineAvailable() const;

private:
	void load();
	void save();
	void saveLater();
	void startAvailable();
	void launch(not_null<Item*> item);
	void taskFinished(uint64 id, DownloadResult result);
	void scheduleAutoRetry(not_null<Item*> item, const QString &error);
	void notifyItem(uint64 id);
	void notifyList();
	[[nodiscard]] int effectiveConcurrency() const;
	[[nodiscard]] int outstanding() const;
	void removeInfo(not_null<Item*> item);

	Settings _settings;
	std::vector<std::unique_ptr<Item>> _items;
	std::deque<uint64> _pending;
	uint64 _nextId = 1;
	crl::time _throttleUntil = 0;
	base::Timer _saveTimer;
	rpl::event_stream<> _listChanges;
	rpl::event_stream<uint64> _itemUpdates;

};

} // namespace GB::Downloader
