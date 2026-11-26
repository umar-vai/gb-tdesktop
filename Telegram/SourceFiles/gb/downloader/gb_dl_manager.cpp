/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/downloader/gb_dl_manager.h"

#include "base/call_delayed.h"
#include "base/random.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>

namespace GB::Downloader {
namespace {

constexpr auto kRateLimitCooldown = 120 * crl::time(1000);
constexpr auto kRateLimitConcurrency = 2;
constexpr auto kSaveDelay = crl::time(1500);

Manager *GlobalManager = nullptr;

[[nodiscard]] QString ListFile() {
	return DataPath() + u"/downloads.json"_q;
}

[[nodiscard]] QString StatusKey(Status status) {
	switch (status) {
	case Status::Completed: return u"completed"_q;
	case Status::Failed: return u"failed"_q;
	case Status::Cancelled: return u"cancelled"_q;
	case Status::Queued:
	case Status::Running:
	case Status::Pausing:
	case Status::Cancelling:
	case Status::Paused: break;
	}
	return u"paused"_q;
}

[[nodiscard]] Status StatusFromKey(const QString &key) {
	return (key == u"completed"_q)
		? Status::Completed
		: (key == u"failed"_q)
		? Status::Failed
		: (key == u"cancelled"_q)
		? Status::Cancelled
		: Status::Paused;
}

[[nodiscard]] QString DateKey(const QDateTime &date) {
	return date.isValid() ? date.toUTC().toString(Qt::ISODate) : QString();
}

[[nodiscard]] QDateTime DateFromKey(const QString &key) {
	return QDateTime::fromString(key, Qt::ISODate);
}

[[nodiscard]] QJsonObject Serialize(const Item &item) {
	auto object = QJsonObject();
	object.insert(u"url"_q, item.url);
	object.insert(u"page_url"_q, item.pageUrl);
	object.insert(u"title"_q, item.title);
	object.insert(u"folder"_q, item.folder);
	object.insert(u"file_path"_q, item.resultPath);
	object.insert(u"platform"_q, item.platform);
	object.insert(u"creator"_q, item.creator);
	object.insert(u"mode"_q, ModeKey(item.mode));
	object.insert(u"quality"_q, item.quality);
	object.insert(u"audio_format"_q, item.audioFormat);
	object.insert(u"audio_quality"_q, item.audioBitrate);
	object.insert(u"info_json"_q, item.infoJsonPath);
	object.insert(u"batch_id"_q, item.batchId);
	object.insert(u"batch_index"_q, item.batchIndex);
	object.insert(u"status"_q, StatusKey(item.status));
	object.insert(u"progress"_q, item.progress);
	object.insert(u"downloaded_bytes"_q, double(item.metrics.downloaded));
	object.insert(u"total_bytes"_q, double(item.metrics.total));
	object.insert(u"error"_q, item.error);
	object.insert(u"duration_seconds"_q, double(item.duration));
	object.insert(u"file_size"_q, double(item.fileSize));
	object.insert(u"added"_q, DateKey(item.added));
	object.insert(u"started_at"_q, DateKey(item.started));
	object.insert(u"finished_at"_q, DateKey(item.finished));
	return object;
}

[[nodiscard]] std::unique_ptr<Item> Deserialize(
		const QJsonObject &object,
		uint64 id) {
	auto result = std::make_unique<Item>();
	result->id = id;
	result->url = object.value(u"url"_q).toString();
	result->pageUrl = object.value(u"page_url"_q).toString();
	result->title = object.value(u"title"_q).toString();
	result->folder = object.value(u"folder"_q).toString();
	result->resultPath = object.value(u"file_path"_q).toString();
	result->platform = object.value(u"platform"_q).toString();
	result->creator = object.value(u"creator"_q).toString();
	result->mode = ModeFromKey(object.value(u"mode"_q).toString());
	result->quality = object.value(u"quality"_q).toString();
	result->audioFormat = object.value(u"audio_format"_q).toString();
	result->audioBitrate = object.value(u"audio_quality"_q).toString();
	result->infoJsonPath = object.value(u"info_json"_q).toString();
	result->batchId = object.value(u"batch_id"_q).toString();
	result->batchIndex = object.value(u"batch_index"_q).toInt();
	result->status = StatusFromKey(object.value(u"status"_q).toString());
	result->progress = std::clamp(
		object.value(u"progress"_q).toDouble(),
		0.,
		1.);
	result->metrics.downloaded = int64(
		object.value(u"downloaded_bytes"_q).toDouble());
	result->metrics.total = int64(object.value(u"total_bytes"_q).toDouble());
	result->error = object.value(u"error"_q).toString();
	result->duration = int64(object.value(u"duration_seconds"_q).toDouble());
	result->fileSize = int64(object.value(u"file_size"_q).toDouble());
	result->added = DateFromKey(object.value(u"added"_q).toString());
	result->started = DateFromKey(object.value(u"started_at"_q).toString());
	result->finished = DateFromKey(object.value(u"finished_at"_q).toString());
	if (!result->added.isValid()) {
		result->added = result->finished.isValid()
			? result->finished
			: QDateTime::currentDateTimeUtc();
	}
	if (result->status == Status::Paused) {
		result->detail = u"Paused - will resume where it stopped"_q;
	} else if (result->status == Status::Failed) {
		result->detail = u"Download failed"_q;
	}
	if (result->url.isEmpty() && result->resultPath.isEmpty()) {
		return nullptr;
	}
	return result;
}

[[nodiscard]] bool IsLive(Status status) {
	return (status == Status::Running)
		|| (status == Status::Pausing)
		|| (status == Status::Cancelling);
}

[[nodiscard]] QStringList WorkspacesFor(const Item &item) {
	auto result = QStringList();
	const auto stem = SafeFilename(item.title).toLower();
	const auto folder = QDir(item.folder);
	for (const auto &info : folder.entryInfoList(
			{ u".gb-download-*"_q },
			QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) {
		auto file = QFile(info.absoluteFilePath() + u"/.title"_q);
		if (file.open(QIODevice::ReadOnly)
			&& SafeFilename(QString::fromUtf8(file.readAll())).toLower() == stem) {
			result.push_back(info.absoluteFilePath());
		}
	}
	return result;
}

void DeletePartials(const Item &item) {
	for (const auto &path : WorkspacesFor(item)) {
		QDir(path).removeRecursively();
	}
	if (item.mode == Mode::File && !item.folder.isEmpty()) {
		const auto name = CleanFilename(item.title, QString());
		if (!name.isEmpty()) {
			QFile::remove(QDir(item.folder).filePath(name + u".mdpart"_q));
		}
	}
}

} // namespace

Display DisplayOf(const Item &item) {
	static const auto finishing = QRegularExpression(
		u"^(merging|converting|saving|finali[sz]ing|processing|"
		"download finished|embedding|writing|removed)"_q,
		QRegularExpression::CaseInsensitiveOption);
	switch (item.status) {
	case Status::Running:
		if (finishing.match(item.detail).hasMatch()) {
			return Display::Finishing;
		}
		return (item.progress > 0.0005)
			? Display::Downloading
			: Display::Connecting;
	case Status::Queued:
		return item.autoAttempts ? Display::Retrying : Display::Queued;
	case Status::Paused: return Display::Paused;
	case Status::Pausing:
	case Status::Cancelling: return Display::Stopping;
	case Status::Completed:
		return (item.resultPath.isEmpty()
			|| QFileInfo::exists(item.resultPath))
			? Display::Complete
			: Display::Missing;
	case Status::Failed: return Display::Failed;
	case Status::Cancelled: break;
	}
	return Display::Cancelled;
}

QString DisplayText(const Item &item) {
	const auto percent = [&](int precision) {
		return QString::number(item.progress * 100., 'f', precision) + '%';
	};
	switch (DisplayOf(item)) {
	case Display::Queued: return u"Queued"_q;
	case Display::Retrying:
		return item.detail.isEmpty()
			? u"Retrying (#%1)"_q.arg(item.autoAttempts)
			: item.detail;
	case Display::Connecting:
		return item.detail.startsWith(u"Retrying"_q)
			? item.detail
			: item.detail.isEmpty()
			? u"Connecting..."_q
			: (item.detail + u"..."_q);
	case Display::Downloading: return percent(1);
	case Display::Finishing: return item.detail + u"..."_q;
	case Display::Stopping: return u"Stopping..."_q;
	case Display::Paused:
		return (item.progress > 0.0005)
			? (u"Paused "_q + percent(0))
			: u"Paused"_q;
	case Display::Complete: return u"Complete"_q;
	case Display::Missing: return u"File missing"_q;
	case Display::Failed: return u"Failed"_q;
	case Display::Cancelled: break;
	}
	return u"Cancelled"_q;
}

QString NameOf(const Item &item) {
	if (!item.resultPath.isEmpty()) {
		return QFileInfo(item.resultPath).fileName();
	} else if (!item.title.isEmpty()) {
		return item.title;
	}
	const auto fromUrl = NameFromUrl(item.url);
	return fromUrl.isEmpty() ? u"Media download"_q : fromUrl;
}

Category CategoryOf(const Item &item) {
	return CategoryFor(NameOf(item), item.mode);
}

bool CanResume(const Item &item) {
	return (item.status == Status::Paused)
		|| (item.status == Status::Failed)
		|| (item.status == Status::Cancelled);
}

bool CanStop(const Item &item) {
	return (item.status == Status::Queued) || (item.status == Status::Running);
}

bool IsFinished(const Item &item) {
	return item.status == Status::Completed;
}

bool Matches(const Item &item, Filter filter, const QString &query) {
	const auto category = CategoryOf(item);
	switch (filter) {
	case Filter::All: break;
	case Filter::Unfinished:
		if (IsFinished(item)) {
			return false;
		}
		break;
	case Filter::Finished:
		if (!IsFinished(item)) {
			return false;
		}
		break;
	case Filter::Video:
		if (category != Category::Video) {
			return false;
		}
		break;
	case Filter::Audio:
		if (category != Category::Audio) {
			return false;
		}
		break;
	case Filter::Files:
		if (category == Category::Video || category == Category::Audio) {
			return false;
		}
		break;
	}
	const auto words = query.toLower().split(' ', Qt::SkipEmptyParts);
	if (words.isEmpty()) {
		return true;
	}
	const auto haystack = QStringList{
		NameOf(item),
		DisplayText(item),
		WebsiteOf(item.pageUrl.isEmpty() ? item.url : item.pageUrl),
		item.url,
		item.pageUrl,
		item.folder,
		item.quality,
	}.join(' ').toLower();
	for (const auto &word : words) {
		if (!haystack.contains(word)) {
			return false;
		}
	}
	return true;
}

Manager::Manager()
: _settings(LoadSettings())
, _saveTimer([=] { save(); }) {
	load();
}

Manager::~Manager() {
	for (const auto &item : _items) {
		if (item->task) {
			item->task->stop(true);
			item->task = nullptr;
			if (IsLive(item->status) || item->status == Status::Queued) {
				item->status = Status::Paused;
			}
		}
	}
	save();
}

Manager &Manager::Instance() {
	if (!GlobalManager) {
		GlobalManager = new Manager();
		QObject::connect(
			QCoreApplication::instance(),
			&QCoreApplication::aboutToQuit,
			[] {
				delete base::take(GlobalManager);
			});
	}
	return *GlobalManager;
}

void Manager::setSettings(Settings settings) {
	_settings = std::move(settings);
	_settings.maxConcurrent = std::clamp(_settings.maxConcurrent, 1, 10);
	SaveSettings(_settings);
	startAvailable();
	notifyList();
}

std::vector<not_null<Item*>> Manager::items() const {
	auto result = std::vector<not_null<Item*>>();
	result.reserve(_items.size());
	for (auto i = _items.rbegin(); i != _items.rend(); ++i) {
		result.push_back(i->get());
	}
	return result;
}

Item *Manager::find(uint64 id) const {
	for (const auto &item : _items) {
		if (item->id == id) {
			return item.get();
		}
	}
	return nullptr;
}

rpl::producer<> Manager::listChanges() const {
	return _listChanges.events();
}

rpl::producer<uint64> Manager::itemUpdates() const {
	return _itemUpdates.events();
}

Totals Manager::totals() const {
	auto result = Totals();
	for (const auto &item : _items) {
		switch (item->status) {
		case Status::Running:
		case Status::Pausing:
		case Status::Cancelling:
			++result.downloading;
			result.speed += item->metrics.speed;
			break;
		case Status::Queued:
			++result.queued;
			break;
		case Status::Paused:
			++result.paused;
			break;
		case Status::Failed:
			++result.failed;
			break;
		case Status::Completed:
		case Status::Cancelled:
			break;
		}
	}
	return result;
}

bool Manager::engineAvailable() const {
	return !FindYtDlp(_settings).isEmpty();
}

uint64 Manager::add(AddRequest request) {
	auto item = std::make_unique<Item>();
	item->id = _nextId++;
	item->url = request.url.trimmed();
	item->pageUrl = request.pageUrl;
	item->title = request.title;
	item->mode = request.mode;
	const auto target = ResolveTarget(
		_settings,
		request.pageUrl.isEmpty() ? item->url : request.pageUrl,
		request.quality.isEmpty() ? _settings.videoQuality : request.quality);
	item->folder = request.folder.isEmpty() ? target.folder : request.folder;
	item->quality = (request.mode == Mode::Video)
		? target.quality
		: QString();
	item->audioFormat = request.audioFormat.isEmpty()
		? _settings.audioFormat
		: request.audioFormat;
	item->audioBitrate = request.audioBitrate.isEmpty()
		? _settings.audioBitrate
		: request.audioBitrate;
	item->infoJsonPath = request.infoJsonPath;
	item->platform = request.platform.isEmpty()
		? PlatformName(item->url)
		: request.platform;
	item->creator = request.creator;
	item->duration = request.duration;
	item->fileSize = request.fileSize;
	item->forceRedownload = request.forceRedownload;
	item->added = QDateTime::currentDateTimeUtc();
	item->status = Status::Queued;
	const auto id = item->id;
	_items.push_back(std::move(item));
	_pending.push_back(id);
	notifyList();
	saveLater();
	startAvailable();
	return id;
}

QString Manager::addBatch(
		const QStringList &urls,
		Mode mode,
		const QString &quality) {
	if (urls.isEmpty() || urls.size() > kMaxBatchItems) {
		return u"Batch must contain between 1 and %1 items."_q.arg(
			kMaxBatchItems);
	} else if (outstanding() + urls.size() > kMaxOutstanding) {
		return u"Download queue is full (%1 pending/running items "
			"maximum)."_q.arg(kMaxOutstanding);
	}
	const auto batchId = QString::number(base::RandomValue<uint64>(), 16);
	auto index = 0;
	for (const auto &url : urls) {
		auto item = std::make_unique<Item>();
		item->id = _nextId++;
		item->url = url;
		item->mode = LooksLikeFile(url) ? Mode::File : mode;
		const auto target = ResolveTarget(
			_settings,
			url,
			quality.isEmpty() ? _settings.videoQuality : quality);
		item->folder = target.folder;
		item->quality = (item->mode == Mode::Video)
			? target.quality
			: QString();
		item->audioFormat = _settings.audioFormat;
		item->audioBitrate = _settings.audioBitrate;
		item->platform = PlatformName(url);
		item->batchId = batchId;
		item->batchIndex = ++index;
		item->added = QDateTime::currentDateTimeUtc();
		item->status = Status::Queued;
		_pending.push_back(item->id);
		_items.push_back(std::move(item));
	}
	notifyList();
	saveLater();
	startAvailable();
	return QString();
}

Item *Manager::findDownloaded(
		const QString &url,
		const QString &pageUrl,
		Mode mode) const {
	const auto direct = (mode == Mode::File);
	auto wanted = QSet<QString>{ LinkKey(url) };
	if (!direct) {
		wanted.insert(PageKey(pageUrl));
	}
	wanted.remove(QString());
	if (wanted.isEmpty()) {
		return nullptr;
	}
	for (auto i = _items.rbegin(); i != _items.rend(); ++i) {
		const auto item = i->get();
		if (item->status != Status::Completed || item->resultPath.isEmpty()) {
			continue;
		}
		auto keys = QSet<QString>{ LinkKey(item->url) };
		if (!direct && item->mode != Mode::File) {
			keys.insert(PageKey(item->pageUrl));
		}
		keys.remove(QString());
		auto same = wanted.intersects(keys);
		if (same && !direct && item->mode != mode) {
			same = false;
		}
		if (same && QFileInfo(item->resultPath).isFile()) {
			return item;
		}
	}
	return nullptr;
}

int Manager::outstanding() const {
	auto result = 0;
	for (const auto &item : _items) {
		if (item->status == Status::Queued || IsLive(item->status)) {
			++result;
		}
	}
	return result;
}

int Manager::effectiveConcurrency() const {
	return (crl::now() < _throttleUntil)
		? std::min(_settings.maxConcurrent, kRateLimitConcurrency)
		: _settings.maxConcurrent;
}

void Manager::startAvailable() {
	auto running = 0;
	for (const auto &item : _items) {
		if (IsLive(item->status)) {
			++running;
		}
	}
	while (!_pending.empty() && running < effectiveConcurrency()) {
		const auto id = _pending.front();
		_pending.pop_front();
		const auto item = find(id);
		if (!item || item->status != Status::Queued || item->waitingRetry) {
			continue;
		}
		++running;
		launch(item);
	}
}

void Manager::launch(not_null<Item*> item) {
	item->status = Status::Running;
	item->started = QDateTime::currentDateTimeUtc();
	item->detail = u"Starting"_q;
	item->error = QString();
	const auto id = item->id;
	auto request = DownloadRequest{
		.url = item->url,
		.pageUrl = item->pageUrl,
		.title = item->title,
		.folder = item->folder,
		.mode = item->mode,
		.quality = item->quality,
		.audioFormat = item->audioFormat,
		.audioBitrate = item->audioBitrate,
		.infoJsonPath = item->infoJsonPath,
		.skipExisting = _settings.skipExisting && !item->forceRedownload,
	};
	auto callbacks = TaskCallbacks{
		.status = crl::guard(this, [=](QString text) {
			if (const auto item = find(id); item && IsLive(item->status)) {
				item->detail = text;
				notifyItem(id);
			}
		}),
		.progress = crl::guard(this, [=](float64 value, Metrics metrics) {
			if (const auto item = find(id); item && IsLive(item->status)) {
				item->progress = std::max(item->progress, std::clamp(value, 0., 1.));
				item->metrics = metrics;
				if (item->detail == u"Starting"_q
					|| item->detail == u"Connecting"_q) {
					item->detail = u"Downloading"_q;
				}
				notifyItem(id);
			}
		}),
		.done = crl::guard(this, [=](DownloadResult result) {
			taskFinished(id, std::move(result));
		}),
	};
	if (item->mode == Mode::File || LooksLikeFile(item->url)) {
		item->mode = Mode::File;
		item->task = std::make_unique<HttpTask>(
			_settings,
			std::move(request),
			std::move(callbacks));
		item->task->start();
		notifyItem(id);
		return;
	}
	if (item->title.isEmpty()) {
		item->detail = u"Analyzing"_q;
		notifyItem(id);
		item->analyzer = std::make_unique<Analyzer>(
			_settings,
			item->url,
			crl::guard(this, [=](QString text) {
				if (const auto item = find(id); item && IsLive(item->status)) {
					item->detail = u"Analyzing: "_q + text;
					notifyItem(id);
				}
			}),
			crl::guard(this, [=](AnalyzeResult result) {
				const auto item = find(id);
				if (!item) {
					return;
				}
				if (item->analyzer) {
					base::call_delayed(0, [analyzer = std::shared_ptr<Analyzer>(
						item->analyzer.release())] {});
				}
				if (item->status != Status::Running) {
					return;
				}
				if (!result.ok) {
					taskFinished(id, { .error = result.error });
					return;
				}
				item->title = result.plainFile
					? result.filename
					: result.title;
				item->creator = result.creator;
				item->duration = result.duration;
				item->infoJsonPath = result.infoJsonPath;
				if (result.plainFile) {
					item->mode = Mode::File;
				}
				item->status = Status::Queued;
				launch(item);
			}));
		item->task = nullptr;
		item->analyzer->start();
		return;
	}
	item->task = std::make_unique<YtDlpTask>(
		_settings,
		std::move(request),
		std::move(callbacks));
	item->task->start();
	notifyItem(id);
}

void Manager::taskFinished(uint64 id, DownloadResult result) {
	const auto item = find(id);
	if (!item) {
		return;
	}
	if (item->task) {
		base::call_delayed(0, [task = std::shared_ptr<Task>(
			item->task.release())] {});
	}
	item->metrics.speed = 0.;
	item->metrics.eta = -1;
	if (result.ok) {
		item->status = Status::Completed;
		item->progress = 1.;
		item->resultPath = result.path;
		item->fileSize = QFileInfo(result.path).size();
		item->finished = QDateTime::currentDateTimeUtc();
		item->detail = result.skipped
			? u"Already downloaded - skipped"_q
			: u"Complete"_q;
		item->error = QString();
		item->forceRedownload = false;
		removeInfo(item);
	} else if (result.cancelled) {
		if (item->status == Status::Pausing) {
			item->status = Status::Paused;
			item->detail = u"Paused - will resume where it stopped"_q;
		} else {
			item->status = Status::Cancelled;
			item->detail = u"Cancelled"_q;
			item->progress = 0.;
			item->finished = QDateTime::currentDateTimeUtc();
		}
	} else if (!item->batchId.isEmpty()
		&& item->autoAttempts < kMaxBatchAutoRetries
		&& item->status == Status::Running) {
		scheduleAutoRetry(item, result.error);
	} else {
		item->status = Status::Failed;
		item->error = result.error;
		item->detail = u"Download failed"_q;
		item->finished = QDateTime::currentDateTimeUtc();
		if (IsRateLimitError(result.error)) {
			_throttleUntil = crl::now() + kRateLimitCooldown;
		}
	}
	notifyItem(id);
	notifyList();
	saveLater();
	startAvailable();
}

void Manager::scheduleAutoRetry(not_null<Item*> item, const QString &error) {
	const auto attempts = ++item->autoAttempts;
	auto delay = std::min(300, 10 * (1 << std::min(attempts - 1, 5)));
	if (IsRateLimitError(error)) {
		_throttleUntil = crl::now() + kRateLimitCooldown;
		delay = std::max(delay, 60);
	}
	item->status = Status::Queued;
	item->progress = 0.;
	item->error = error;
	item->waitingRetry = true;
	const auto reason = error.simplified().left(90);
	item->detail = u"Auto-retry #%1 in %2s"_q.arg(attempts).arg(delay)
		+ (reason.isEmpty() ? QString() : (u" - "_q + reason));
	const auto id = item->id;
	base::call_delayed(delay * crl::time(1000), this, [=] {
		const auto item = find(id);
		if (!item || item->status != Status::Queued || !item->waitingRetry) {
			return;
		}
		item->waitingRetry = false;
		item->detail = u"Retrying"_q;
		_pending.push_back(id);
		notifyItem(id);
		startAvailable();
	});
}

void Manager::pause(uint64 id) {
	const auto item = find(id);
	if (!item) {
		return;
	}
	if (item->status == Status::Queued) {
		_pending.erase(ranges::remove(_pending, id), end(_pending));
		item->waitingRetry = false;
		item->status = Status::Paused;
		item->detail = u"Paused"_q;
	} else if (item->status == Status::Running && item->analyzer) {
		item->analyzer = nullptr;
		item->status = Status::Paused;
		item->detail = u"Paused"_q;
		startAvailable();
	} else if (item->status == Status::Running) {
		item->status = Status::Pausing;
		item->detail = u"Pausing"_q;
		if (item->task) {
			item->task->stop(true);
		}
	}
	notifyItem(id);
	saveLater();
}

void Manager::resume(uint64 id) {
	const auto item = find(id);
	if (!item || !CanResume(*item)) {
		return;
	}
	if (item->status != Status::Paused) {
		item->progress = 0.;
		item->resultPath = QString();
		item->started = QDateTime();
		item->finished = QDateTime();
	}
	item->status = Status::Queued;
	item->error = QString();
	item->detail = u"Resuming"_q;
	item->autoAttempts = 0;
	item->waitingRetry = false;
	_pending.push_back(id);
	notifyItem(id);
	notifyList();
	saveLater();
	startAvailable();
}

void Manager::cancel(uint64 id) {
	const auto item = find(id);
	if (!item) {
		return;
	}
	if (item->analyzer
		|| item->status == Status::Queued
		|| item->status == Status::Paused) {
		item->analyzer = nullptr;
		_pending.erase(ranges::remove(_pending, id), end(_pending));
		item->waitingRetry = false;
		item->status = Status::Cancelled;
		item->detail = u"Cancelled"_q;
		item->progress = 0.;
		item->finished = QDateTime::currentDateTimeUtc();
		DeletePartials(*item);
	} else if (item->status == Status::Running
		|| item->status == Status::Pausing) {
		item->status = Status::Cancelling;
		item->detail = u"Cancelling"_q;
		if (item->task) {
			item->task->stop(false);
		}
	}
	notifyItem(id);
	notifyList();
	saveLater();
}

void Manager::retry(uint64 id) {
	resume(id);
}

void Manager::redownload(uint64 id) {
	const auto item = find(id);
	if (!item || item->url.isEmpty()) {
		return;
	}
	if (item->status == Status::Completed) {
		add({
			.url = item->url,
			.pageUrl = item->pageUrl,
			.title = (item->mode == Mode::File)
				? QFileInfo(item->resultPath).fileName()
				: item->title,
			.folder = item->folder,
			.mode = item->mode,
			.quality = item->quality,
			.audioFormat = item->audioFormat,
			.audioBitrate = item->audioBitrate,
			.platform = item->platform,
			.creator = item->creator,
			.duration = item->duration,
			.forceRedownload = true,
		});
		return;
	}
	item->forceRedownload = true;
	resume(id);
}

void Manager::remove(uint64 id, bool deleteFile) {
	const auto i = ranges::find(_items, id, &Item::id);
	if (i == end(_items)) {
		return;
	}
	const auto item = i->get();
	_pending.erase(ranges::remove(_pending, id), end(_pending));
	if (item->task) {
		item->task->stop(false);
		base::call_delayed(0, [task = std::shared_ptr<Task>(
			item->task.release())] {});
	}
	if (item->status != Status::Completed) {
		DeletePartials(*item);
	} else if (deleteFile && !item->resultPath.isEmpty()) {
		QFile::remove(item->resultPath);
	}
	removeInfo(item);
	_items.erase(i);
	notifyList();
	saveLater();
	startAvailable();
}

void Manager::pauseAll() {
	for (const auto &item : _items) {
		if (CanStop(*item)) {
			pause(item->id);
		}
	}
}

void Manager::resumeAll() {
	for (const auto &item : _items) {
		if (item->status == Status::Paused) {
			resume(item->id);
		}
	}
}

int Manager::retryFailed() {
	auto result = 0;
	for (const auto &item : _items) {
		if (item->status == Status::Failed) {
			resume(item->id);
			++result;
		}
	}
	return result;
}

int Manager::cleanUp() {
	auto ids = std::vector<uint64>();
	for (const auto &item : _items) {
		const auto display = DisplayOf(*item);
		if (display == Display::Missing
			|| display == Display::Cancelled
			|| display == Display::Failed) {
			ids.push_back(item->id);
		}
	}
	for (const auto id : ids) {
		remove(id, false);
	}
	return int(ids.size());
}

void Manager::removeInfo(not_null<Item*> item) {
	if (!item->infoJsonPath.isEmpty()
		&& QFileInfo(item->infoJsonPath).absolutePath()
			== QFileInfo(DataPath() + u"/info/x"_q).absolutePath()) {
		QFile::remove(item->infoJsonPath);
	}
	item->infoJsonPath = QString();
}

void Manager::notifyItem(uint64 id) {
	_itemUpdates.fire_copy(id);
}

void Manager::notifyList() {
	_listChanges.fire({});
}

void Manager::saveLater() {
	if (!_saveTimer.isActive()) {
		_saveTimer.callOnce(kSaveDelay);
	}
}

void Manager::load() {
	auto file = QFile(ListFile());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto object = QJsonDocument::fromJson(file.readAll()).object();
	const auto list = object.value(u"items"_q).toArray();
	for (auto i = list.size(); i != 0; --i) {
		if (auto item = Deserialize(list[i - 1].toObject(), _nextId)) {
			++_nextId;
			_items.push_back(std::move(item));
		}
	}
}

void Manager::save() {
	_saveTimer.cancel();
	auto list = QJsonArray();
	auto count = 0;
	for (auto i = _items.rbegin(); i != _items.rend(); ++i) {
		if (++count > kHistoryLimit) {
			break;
		}
		list.push_back(Serialize(**i));
	}
	auto object = QJsonObject();
	object.insert(u"version"_q, 1);
	object.insert(u"items"_q, list);
	auto file = QSaveFile(ListFile());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
		file.commit();
	}
}

} // namespace GB::Downloader
