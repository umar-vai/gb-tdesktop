/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/downloader/gb_dl_capture.h"

#include "base/call_delayed.h"
#include "base/unique_qptr.h"
#include "core/file_utilities.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/separate_panel.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_gb.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QCursor>
#include <QtGui/QDrag>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtWidgets/QApplication>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

namespace GB::Downloader {
namespace {

constexpr auto kTopmostFor = crl::time(600);
constexpr auto kCascadeStep = 28;

[[nodiscard]] bool SameFolder(const QString &a, const QString &b) {
	if (a.trimmed().isEmpty() || b.trimmed().isEmpty()) {
		return false;
	}
	const auto clean = [](const QString &path) {
		return QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()))
			.toLower();
	};
	return clean(a) == clean(b);
}

// Pops a window in front of the browser, then lets it behave like a normal
// window again (the panel starts as "stay on top").
void PopToFront(not_null<QWidget*> window) {
#ifdef Q_OS_WIN
	const auto handle = reinterpret_cast<HWND>(window->winId());
	const auto flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
	SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0, flags | SWP_SHOWWINDOW);
	base::call_delayed(kTopmostFor, window, [=] {
		SetWindowPos(handle, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
	});
#endif // Q_OS_WIN
	window->raise();
}

// "Drag this file into Telegram, a browser or a folder."
class DragOut final : public Ui::RpWidget {
public:
	DragOut(QWidget *parent, QString path)
	: RpWidget(parent)
	, _path(std::move(path)) {
		setCursor(style::cur_pointer);
		resize(width(), st::gbCaptureDropHeight);
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		auto pen = st::windowSubTextFg->p;
		pen.setStyle(Qt::DashLine);
		p.setPen(pen);
		p.setBrush(_over ? st::windowBgOver : st::windowBg);
		const auto half = st::lineWidth / 2.;
		p.drawRoundedRect(
			QRectF(rect()).marginsRemoved({ half, half, half, half }),
			st::roundRadiusLarge,
			st::roundRadiusLarge);
		p.setPen(st::windowSubTextFg);
		p.setFont(st::normalFont);
		p.drawText(
			rect(),
			u"Drag this file into Telegram, a browser or a folder"_q,
			style::al_center);
	}
	void enterEventHook(QEnterEvent *e) override {
		_over = true;
		update();
	}
	void leaveEventHook(QEvent *e) override {
		_over = false;
		update();
	}
	void mousePressEvent(QMouseEvent *e) override {
		_pressed = (e->button() == Qt::LeftButton);
		_pressPosition = e->pos();
	}
	void mouseReleaseEvent(QMouseEvent *e) override {
		_pressed = false;
	}
	void mouseMoveEvent(QMouseEvent *e) override {
		if (!_pressed
			|| (e->pos() - _pressPosition).manhattanLength()
				< QApplication::startDragDistance()) {
			return;
		}
		_pressed = false;
		if (!QFileInfo::exists(_path)) {
			return;
		}
		const auto drag = new QDrag(this);
		const auto data = new QMimeData();
		data->setUrls({ QUrl::fromLocalFile(_path) });
		drag->setMimeData(data);
		drag->exec(Qt::CopyAction);
	}

private:
	QString _path;
	QPoint _pressPosition;
	bool _pressed = false;
	bool _over = false;

};

// A thin progress bar in the palette's colors.
class Bar final : public Ui::RpWidget {
public:
	explicit Bar(QWidget *parent) : RpWidget(parent) {
		resize(width(), st::gbCaptureBarHeight);
	}

	void setValue(float64 value) {
		value = std::clamp(value, 0., 1.);
		if (_value != value) {
			_value = value;
			update();
		}
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto radius = height() / 2.;
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBgOver);
		p.drawRoundedRect(rect(), radius, radius);
		if (_value > 0.) {
			const auto filled = std::max(height(), int(width() * _value));
			p.setBrush(st::windowBgActive);
			p.drawRoundedRect(QRect(0, 0, filled, height()), radius, radius);
		}
	}

private:
	float64 _value = 0.;

};

class CaptureWindow final : public base::has_weak_ptr {
public:
	CaptureWindow(AddRequest request, int offset, Fn<void()> closed);
	~CaptureWindow();

private:
	enum class Buttons {
		None,
		Running,
		Paused,
		Stopping,
		Done,
		Failed,
		Ended,
	};
	struct Button {
		QString text;
		Fn<void()> callback;
	};

	void prepare();
	void preparePrompt();
	void showDuplicate(const QString &path);
	void showPrompt();
	void showProgress();
	void showDone();
	void begin(bool later);
	void browse();
	void refresh();
	void rememberChoice(const QString &folder);

	[[nodiscard]] not_null<Ui::VerticalLayout*> newContent();
	void setButtons(std::vector<Button> buttons);
	void buttonsFor(Buttons kind);
	void setStatus(const QString &text);
	void relayout();
	void place();
	void close();

	not_null<Ui::FlatLabel*> addLabel(
		const QString &text,
		const style::FlatLabel &st,
		int top = 0);

	AddRequest _request;
	const int _offset = 0;
	const Fn<void()> _closed;
	std::unique_ptr<Ui::SeparatePanel> _panel;
	rpl::variable<QString> _title;

	QString _name;
	QString _folder;
	QString _host;
	QString _ext;
	QString _fallbackNote;
	QString _proposedFolder;
	bool _extSaved = false;
	bool _siteSaved = false;

	Ui::RpWidget *_content = nullptr;
	Ui::VerticalLayout *_body = nullptr;
	Ui::RpWidget *_footer = nullptr;
	Ui::FlatLabel *_status = nullptr;
	QString _statusText;
	QString _shownName;
	std::vector<Ui::RoundButton*> _buttons;
	Buttons _buttonsKind = Buttons::None;

	Ui::InputField *_nameField = nullptr;
	Ui::InputField *_folderField = nullptr;
	Ui::Checkbox *_rememberExt = nullptr;
	Ui::Checkbox *_rememberSite = nullptr;

	Ui::FlatLabel *_nameLabel = nullptr;
	Ui::FlatLabel *_stateLabel = nullptr;
	Ui::FlatLabel *_statsLabel = nullptr;
	Bar *_bar = nullptr;

	uint64 _id = 0;
	bool _submitted = false;
	bool _ended = false;
	bool _placed = false;
	bool _closing = false;
	bool _relayouting = false;
	int _requestedHeight = 0;
	rpl::lifetime _itemLifetime;

};

std::vector<std::unique_ptr<CaptureWindow>> Windows;

CaptureWindow::CaptureWindow(
	AddRequest request,
	int offset,
	Fn<void()> closed)
: _request(std::move(request))
, _offset(offset)
, _closed(std::move(closed))
, _panel(std::make_unique<Ui::SeparatePanel>()) {
	prepare();
}

CaptureWindow::~CaptureWindow() {
	_itemLifetime.destroy();
	if (!_submitted) {
		// Nothing was queued: the browser's cookies and the cached video
		// info belong to nobody now.
		if (!_request.infoJsonPath.isEmpty()) {
			QFile::remove(_request.infoJsonPath);
		}
		RemoveCookieFile(_request.context.cookiesFile);
	}
}

void CaptureWindow::prepare() {
	_panel->setTitle(_title.value());
	// The panel needs a size before its first content (showInner).
	_panel->setInnerSize(QSize(st::gbCaptureWidth, st::gbCaptureDropHeight));
	_panel->closeRequests(
	) | rpl::on_next([=] {
		close();
	}, _panel->lifetime());
	_panel->closeEvents(
	) | rpl::on_next([=] {
		if (const auto callback = _closed) {
			crl::on_main(callback);
		}
	}, _panel->lifetime());

	preparePrompt();
	const auto &manager = Manager::Instance();
	const auto existing = _request.forceRedownload
		? nullptr
		: manager.findDownloaded(
			_request.url,
			_request.pageUrl.isEmpty() ? _request.url : _request.pageUrl,
			_request.mode);
	if (existing && !existing->resultPath.isEmpty()) {
		showDuplicate(existing->resultPath);
	} else {
		showPrompt();
	}
}

void CaptureWindow::preparePrompt() {
	const auto &settings = Manager::Instance().settings();
	const auto file = (_request.mode == Mode::File);
	const auto siteUrl = _request.pageUrl.isEmpty()
		? _request.url
		: _request.pageUrl;
	_host = HostOf(siteUrl);
	if (file) {
		_name = _request.title.isEmpty()
			? NameFromUrl(_request.url)
			: _request.title;
		if (_name.isEmpty()) {
			_name = u"download"_q;
		}
		const auto ext = ExtensionOf(_name);
		_ext = ext.isEmpty() ? QString() : ('.' + ext);
	} else {
		_name = _request.title;
		if (_name.isEmpty() || IsPlaceholderTitle(_name)) {
			const auto path = QUrl(siteUrl).path();
			_name = path.split('/', Qt::SkipEmptyParts).value(
				path.split('/', Qt::SkipEmptyParts).size() - 1);
			if (_name.isEmpty()) {
				_name = _host.isEmpty() ? u"video"_q : _host;
			}
		}
		_name = _name.simplified().left(150);
	}

	auto folder = ResolveTarget(settings, siteUrl, QString()).folder;
	auto remembered = false;
	for (const auto &key : {
			_ext.isEmpty() ? QString() : (u"ext:"_q + _ext),
			_host.isEmpty() ? QString() : (u"site:"_q + _host) }) {
		if (key.isEmpty()) {
			continue;
		}
		const auto i = settings.captureDirs.find(key);
		if (i != end(settings.captureDirs) && QFileInfo(i->second).isDir()) {
			folder = i->second;
			remembered = true;
			break;
		}
	}
	if (!remembered
		&& SameFolder(folder, settings.downloadDir)
		&& QFileInfo(settings.captureLastDir).isDir()) {
		folder = settings.captureLastDir;
	}
	const auto available = AvailableFolderFor(folder);
	_folder = QDir::toNativeSeparators(available.folder);
	_fallbackNote = available.note;
	_proposedFolder = _folder;
	const auto saved = [&](const QString &key) {
		const auto i = settings.captureDirs.find(key);
		return (i != end(settings.captureDirs))
			&& SameFolder(i->second, _folder);
	};
	_extSaved = !_ext.isEmpty() && saved(u"ext:"_q + _ext);
	_siteSaved = !_host.isEmpty() && saved(u"site:"_q + _host);
}

not_null<Ui::VerticalLayout*> CaptureWindow::newContent() {
	_buttons.clear();
	_buttonsKind = Buttons::None;
	_requestedHeight = 0;
	_nameField = _folderField = nullptr;
	_rememberExt = _rememberSite = nullptr;
	_nameLabel = _stateLabel = _statsLabel = nullptr;
	_bar = nullptr;

	auto owned = base::make_unique_q<Ui::RpWidget>(_panel.get());
	const auto content = owned.get();
	_content = content;
	_body = Ui::CreateChild<Ui::VerticalLayout>(content);
	_footer = Ui::CreateChild<Ui::RpWidget>(content);
	_statusText = QString();
	_status = Ui::CreateChild<Ui::FlatLabel>(
		_footer,
		QString(),
		st::gbCaptureStatus);

	_footer->show();
	content->widthValue(
	) | rpl::on_next([=] {
		relayout();
	}, content->lifetime());
	_body->heightValue(
	) | rpl::on_next([=] {
		relayout();
	}, _body->lifetime());

	_panel->showInner(std::move(owned));
	return _body;
}

not_null<Ui::FlatLabel*> CaptureWindow::addLabel(
		const QString &text,
		const style::FlatLabel &st,
		int top) {
	return _body->add(
		object_ptr<Ui::FlatLabel>(_body, text, st),
		style::margins(0, top, 0, 0));
}

// Everything is laid out for the fixed window width, then the window
// takes the height the content needs.
void CaptureWindow::relayout() {
	if (!_body || !_footer || _relayouting) {
		return;
	}
	_relayouting = true;
	const auto &padding = st::gbCapturePadding;
	const auto inner = st::gbCaptureWidth - padding.left() - padding.right();
	_body->resizeToWidth(inner);
	_body->moveToLeft(padding.left(), padding.top());

	auto right = inner;
	auto footerHeight = 0;
	for (auto i = _buttons.rbegin(); i != _buttons.rend(); ++i) {
		const auto button = *i;
		right -= button->width();
		button->moveToLeft(right, 0);
		right -= st::gbCaptureButtonSkip;
		footerHeight = std::max(footerHeight, button->height());
	}
	const auto statusWidth = std::max(right - st::gbCaptureSkip, 0);
	const auto statusShown = (statusWidth > 0) && !_statusText.isEmpty();
	if (statusShown) {
		_status->resizeToWidth(statusWidth);
		footerHeight = std::max(footerHeight, _status->height());
		_status->moveToLeft(
			0,
			std::max(0, (footerHeight - _status->height()) / 2));
	}
	_status->setVisible(statusShown);
	const auto footerTop = padding.top()
		+ _body->height()
		+ st::gbCaptureSkip * 2;
	_footer->setGeometry(padding.left(), footerTop, inner, footerHeight);

	const auto height = footerTop + footerHeight + padding.bottom();
	if (_requestedHeight != height) {
		_requestedHeight = height;
		// The panel's "inner size" includes its own title bar.
		_panel->setInnerSize(QSize(
			st::gbCaptureWidth,
			height + st::separatePanelTitleHeight));
	}
	_relayouting = false;
	place();
}

void CaptureWindow::place() {
	if (_placed) {
		return;
	}
	_placed = true;
	// Where the user is (the browser), centered and a third down, each new
	// window a step further so several do not cover each other.
	const auto screen = QGuiApplication::screenAt(QCursor::pos());
	const auto available = (screen ? screen : QGuiApplication::primaryScreen())
		->availableGeometry();
	const auto size = _panel->size();
	const auto step = kCascadeStep * (_offset % 8);
	_panel->move(
		available.x() + std::max(0, (available.width() - size.width()) / 2)
			+ step,
		available.y() + std::max(0, (available.height() - size.height()) / 3)
			+ step);
	_panel->showAndActivate();
	PopToFront(_panel.get());
}

void CaptureWindow::setStatus(const QString &text) {
	if (_status) {
		_statusText = text;
		_status->setText(text);
		relayout();
	}
}

void CaptureWindow::setButtons(std::vector<Button> buttons) {
	{
		for (const auto button : base::take(_buttons)) {
			button->hide();
			button->deleteLater();
		}
		for (auto &button : buttons) {
			const auto widget = Ui::CreateChild<Ui::RoundButton>(
				_footer,
				rpl::single(button.text),
				st::defaultBoxButton);
			widget->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
			// Deferred: a click often replaces the content holding the button.
			widget->setClickedCallback([=, callback = std::move(button.callback)] {
				crl::on_main(this, callback);
			});
			widget->show();
			_buttons.push_back(widget);
		}
	}
	relayout();
}

void CaptureWindow::showDuplicate(const QString &path) {
	_title = u"Already downloaded"_q;
	const auto body = newContent();
	addLabel(u"This file is already downloaded"_q, st::gbCaptureTitle);
	addLabel(QFileInfo(path).fileName(), st::gbCaptureText, st::gbCaptureSkip);
	addLabel(
		QDir::toNativeSeparators(QFileInfo(path).absolutePath()),
		st::gbCaptureAbout,
		st::gbCaptureSkip / 2);
	addLabel(
		u"Do you want to download it again?"_q,
		st::gbCaptureText,
		st::gbCaptureSkip * 2);
	body->resizeToWidth(body->width());
	setButtons({
		{ u"Download again"_q, [=] {
			_request.forceRedownload = true;
			showPrompt();
		} },
		{ u"Open"_q, [=] {
			File::Launch(path);
			close();
		} },
		{ u"Show in folder"_q, [=] {
			File::ShowInFolder(path);
			close();
		} },
		{ u"Cancel"_q, [=] { close(); } },
	});
	relayout();
}

void CaptureWindow::showPrompt() {
	_title = u"New download"_q;
	const auto body = newContent();
	addLabel(u"Download this file?"_q, st::gbCaptureTitle);

	auto info = QStringList();
	info.push_back((_request.fileSize > 0)
		? FormatSize(_request.fileSize)
		: u"Size unknown"_q);
	if (!_host.isEmpty()) {
		info.push_back(_host);
	}
	if (_request.mode == Mode::Audio) {
		info.push_back(u"Audio only"_q);
	} else if (_request.mode == Mode::Video && !_request.quality.isEmpty()) {
		info.push_back(_request.quality);
	}
	addLabel(info.join(u" • "_q), st::gbCaptureAbout, 2);

	_nameField = body->add(
		object_ptr<Ui::InputField>(
			body,
			st::defaultInputField,
			Ui::InputField::Mode::NoNewlines,
			rpl::single(u"Save as"_q),
			_name),
		style::margins(0, st::gbCaptureSkip, 0, 0));
	_nameField->submits(
	) | rpl::on_next([=] {
		crl::on_main(this, [=] { begin(false); });
	}, _nameField->lifetime());

	const auto row = body->add(
		object_ptr<Ui::RpWidget>(body),
		style::margins(0, st::gbCaptureSkip, 0, 0));
	_folderField = Ui::CreateChild<Ui::InputField>(
		row,
		st::defaultInputField,
		Ui::InputField::Mode::NoNewlines,
		rpl::single(u"Save to"_q),
		_folder);
	const auto browseButton = Ui::CreateChild<Ui::RoundButton>(
		row,
		rpl::single(u"Browse..."_q),
		st::defaultBoxButton);
	browseButton->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	browseButton->setClickedCallback([=] { browse(); });
	row->resize(row->width(), _folderField->height());
	row->widthValue(
	) | rpl::on_next([=](int width) {
		const auto buttonWidth = browseButton->width();
		_folderField->resize(
			std::max(width - buttonWidth - st::gbCaptureSkip, 0),
			_folderField->height());
		_folderField->moveToLeft(0, 0);
		browseButton->moveToLeft(
			width - buttonWidth,
			_folderField->height() - browseButton->height());
	}, row->lifetime());
	_folderField->submits(
	) | rpl::on_next([=] {
		crl::on_main(this, [=] { begin(false); });
	}, _folderField->lifetime());

	const auto checkboxMargins = style::margins(0, st::gbCaptureSkip, 0, 0);
	if (!_ext.isEmpty()) {
		_rememberExt = body->add(
			object_ptr<Ui::Checkbox>(
				body,
				u"Always save %1 files in this folder"_q.arg(_ext),
				_extSaved,
				st::defaultBoxCheckbox),
			checkboxMargins);
	}
	if (!_host.isEmpty()) {
		_rememberSite = body->add(
			object_ptr<Ui::Checkbox>(
				body,
				u"Always save downloads from %1 in this folder"_q.arg(_host),
				_siteSaved,
				st::defaultBoxCheckbox),
			checkboxMargins);
	}
	body->resizeToWidth(body->width());
	setButtons({
		{ u"Download"_q, [=] { begin(false); } },
		{ u"Later"_q, [=] { begin(true); } },
		{ u"Cancel"_q, [=] { close(); } },
	});
	setStatus(_fallbackNote);
	relayout();
	_nameField->setFocusFast();
	_nameField->selectAll();
}

void CaptureWindow::browse() {
	const auto start = _folderField
		? _folderField->getLastText().trimmed()
		: _folder;
	FileDialog::GetFolder(
		_panel.get(),
		u"Save to"_q,
		QFileInfo(start).isDir() ? start : QString(),
		crl::guard(this, [=](QString &&result) {
			if (!result.isEmpty() && _folderField) {
				_folderField->setText(QDir::toNativeSeparators(result));
			}
		}));
}

void CaptureWindow::begin(bool later) {
	if (!_nameField || !_folderField || _submitted) {
		return;
	}
	auto name = _nameField->getLastText().simplified();
	const auto folder = QDir::toNativeSeparators(
		_folderField->getLastText().trimmed());
	if (name.isEmpty()) {
		setStatus(u"Type a file name."_q);
		return;
	} else if (folder.isEmpty()) {
		setStatus(u"Choose where to save the file."_q);
		return;
	}
	const auto file = (_request.mode == Mode::File);
	if (file && !_ext.isEmpty() && ExtensionOf(name).isEmpty()) {
		name += _ext;
	}
	if (!QDir().mkpath(folder)) {
		const auto drive = MissingDrive(folder);
		setStatus(drive.isEmpty()
			? u"Cannot use that folder."_q
			: u"%1 not found. Choose another folder."_q.arg(drive));
		return;
	}
	auto request = _request;
	request.title = file ? CleanFilename(name, u"download"_q) : name;
	request.folder = folder;
	request.startPaused = later;
	request.fromBrowser = true;
	const auto id = Manager::Instance().add(std::move(request));
	if (!id) {
		setStatus(u"Could not add the download."_q);
		return;
	}
	_submitted = true;
	_id = id;
	rememberChoice(folder);
	_name = name;
	_folder = folder;
	if (later) {
		// Kept in the list, paused; it is started from there.
		close();
		return;
	}
	showProgress();
}

void CaptureWindow::rememberChoice(const QString &folder) {
	auto settings = Manager::Instance().settings();
	// A stand-in for a missing drive is not remembered as the user's choice.
	if (_fallbackNote.isEmpty() || !SameFolder(folder, _proposedFolder)) {
		settings.captureLastDir = folder;
	}
	const auto apply = [&](const QString &key, Ui::Checkbox *box) {
		if (key.isEmpty() || !box) {
			return;
		}
		if (box->checked()) {
			settings.captureDirs[key] = folder;
		} else if (const auto i = settings.captureDirs.find(key)
			; i != end(settings.captureDirs) && MissingDrive(i->second).isEmpty()) {
			// The box was cleared: stop using a saved folder.
			settings.captureDirs.erase(i);
		}
	};
	apply(_ext.isEmpty() ? QString() : (u"ext:"_q + _ext), _rememberExt);
	apply(_host.isEmpty() ? QString() : (u"site:"_q + _host), _rememberSite);
	Manager::Instance().setSettings(std::move(settings));
}

void CaptureWindow::showProgress() {
	_title = u"Downloading"_q;
	const auto body = newContent();
	_shownName = _name;
	_nameLabel = addLabel(_name, st::gbCaptureTitle);
	_stateLabel = addLabel(u"Starting..."_q, st::gbCaptureText, 2);
	_bar = body->add(
		object_ptr<Bar>(body),
		style::margins(0, st::gbCaptureSkip, 0, 0));
	_statsLabel = addLabel(QString(), st::gbCaptureAbout, st::gbCaptureSkip);
	addLabel(u"Save to: "_q + _folder, st::gbCaptureAbout, 2);
	body->resizeToWidth(body->width());
	buttonsFor(Buttons::Running);
	relayout();

	_itemLifetime.destroy();
	auto &manager = Manager::Instance();
	manager.itemUpdates(
	) | rpl::filter(
		rpl::mappers::_1 == _id
	) | rpl::on_next([=] {
		refresh();
	}, _itemLifetime);
	manager.listChanges(
	) | rpl::on_next([=] {
		refresh();
	}, _itemLifetime);
	refresh();
}

void CaptureWindow::buttonsFor(Buttons kind) {
	if (_buttonsKind == kind) {
		return;
	}
	_buttonsKind = kind;
	const auto hide = Button{ u"Hide"_q, [=] { close(); } };
	const auto closeButton = Button{ u"Close"_q, [=] { close(); } };
	const auto cancel = Button{ u"Cancel"_q, [=] {
		// Closes at once; the list finishes stopping in the background.
		Manager::Instance().cancel(_id);
		close();
	} };
	switch (kind) {
	case Buttons::Running:
		setButtons({
			{ u"Pause"_q, [=] { Manager::Instance().pause(_id); } },
			cancel,
			hide,
		});
		break;
	case Buttons::Paused:
		setButtons({
			{ u"Resume"_q, [=] { Manager::Instance().resume(_id); } },
			cancel,
			hide,
		});
		break;
	case Buttons::Stopping:
		setButtons({ hide });
		break;
	case Buttons::Failed:
		setButtons({
			{ u"Retry"_q, [=] { Manager::Instance().retry(_id); } },
			closeButton,
		});
		break;
	case Buttons::Done:
	case Buttons::Ended:
	case Buttons::None:
		setButtons({ closeButton });
		break;
	}
}

void CaptureWindow::refresh() {
	if (_ended || !_stateLabel) {
		return;
	}
	const auto item = Manager::Instance().find(_id);
	if (!item) {
		_ended = true;
		_itemLifetime.destroy();
		_stateLabel->setText(u"This download was removed from the list."_q);
		_statsLabel->setText(QString());
		buttonsFor(Buttons::Ended);
		return;
	}
	const auto name = NameOf(*item);
	if (!name.isEmpty() && _shownName != name) {
		_shownName = name;
		_nameLabel->setText(name);
	}
	const auto percent = std::clamp(item->progress, 0., 1.) * 100.;
	switch (item->status) {
	case Status::Completed:
		_ended = true;
		_itemLifetime.destroy();
		showDone();
		return;
	case Status::Failed: {
		_title = u"Download failed"_q;
		_stateLabel->setText(u"The download failed."_q);
		const auto error = item->error.isEmpty() ? item->detail : item->error;
		_statsLabel->setText(error.left(300));
		buttonsFor(Buttons::Failed);
	} return;
	case Status::Cancelled:
		_ended = true;
		_itemLifetime.destroy();
		_title = u"Download cancelled"_q;
		_stateLabel->setText(u"Download cancelled."_q);
		buttonsFor(Buttons::Ended);
		return;
	case Status::Paused:
		_title = u"Paused - "_q + name;
		_stateLabel->setText(u"Paused"_q);
		_bar->setValue(percent / 100.);
		buttonsFor(Buttons::Paused);
		return;
	case Status::Pausing:
	case Status::Cancelling:
		_stateLabel->setText(DisplayText(*item));
		buttonsFor(Buttons::Stopping);
		return;
	case Status::Queued:
	case Status::Running:
		break;
	}
	buttonsFor(Buttons::Running);
	const auto detail = item->detail;
	const auto rounded = QString::number(int(percent)) + '%';
	_stateLabel->setText((percent >= 1. && !detail.isEmpty())
		? (rounded + u"  •  "_q + detail)
		: (percent >= 1.)
		? rounded
		: detail.isEmpty()
		? u"Starting..."_q
		: detail);
	_bar->setValue(percent / 100.);
	auto parts = QStringList();
	const auto &metrics = item->metrics;
	if (metrics.downloaded > 0) {
		parts.push_back((metrics.total > 0)
			? u"%1 of %2"_q.arg(
				FormatSize(metrics.downloaded),
				FormatSize(metrics.total))
			: FormatSize(metrics.downloaded));
	}
	if (metrics.speed > 0.) {
		parts.push_back(FormatSpeed(metrics.speed));
	}
	if (metrics.eta > 0) {
		parts.push_back(FormatEta(metrics.eta) + u" left"_q);
	}
	_statsLabel->setText(parts.join(u"  •  "_q));
	_title = rounded + u" - "_q + name;
}

void CaptureWindow::showDone() {
	const auto item = Manager::Instance().find(_id);
	const auto path = item
		? QDir::toNativeSeparators(item->resultPath)
		: QString();
	_title = u"Download complete"_q;
	const auto body = newContent();
	addLabel(u"Download complete"_q, st::gbCaptureTitle);
	auto info = QStringList{ QFileInfo(path).fileName() };
	const auto size = QFileInfo(path).size();
	if (size > 0) {
		info.push_back(FormatSize(size));
	}
	addLabel(info.join(u" • "_q), st::gbCaptureText, 2);
	addLabel(u"Saved in:"_q, st::gbCaptureAbout, st::gbCaptureSkip);
	const auto pathLabel = addLabel(path, st::gbCaptureText, 2);
	pathLabel->setSelectable(true);
	pathLabel->setBreakEverywhere(true);
	if (QFileInfo::exists(path)) {
		body->add(
			object_ptr<DragOut>(body, path),
			style::margins(0, st::gbCaptureSkip * 2, 0, 0));
	}
	body->resizeToWidth(body->width());
	_buttonsKind = Buttons::Done;
	setButtons({
		{ u"Open"_q, [=] {
			if (QFileInfo::exists(path)) {
				File::Launch(path);
				close();
			} else {
				setStatus(u"The file is no longer on the disk."_q);
			}
		} },
		{ u"Show in folder"_q, [=] {
			File::ShowInFolder(path.isEmpty() ? _folder : path);
			close();
		} },
		{ u"Copy path"_q, [=] {
			QGuiApplication::clipboard()->setText(path);
			close();
		} },
		{ u"Delete"_q, [=] {
			if (!QFileInfo::exists(path)) {
				setStatus(u"The file is no longer on the disk."_q);
				return;
			}
			_panel->showBox(
				Ui::MakeConfirmBox({
					.text = u"Delete this file from the disk?\n\n%1\n\n"
						"This cannot be undone."_q.arg(path),
					.confirmed = [=, weak = base::make_weak(this)](
							Fn<void()> hide) {
						hide();
						if (const auto strong = weak.get()) {
							Manager::Instance().remove(strong->_id, true);
							strong->close();
						}
					},
					.confirmText = rpl::single(u"Delete"_q),
					.confirmStyle = &st::attentionBoxButton,
					.title = rpl::single(u"Delete file"_q),
				}),
				Ui::LayerOption::KeepOther,
				anim::type::normal);
		} },
		{ u"Close"_q, [=] { close(); } },
	});
	relayout();
	// A finished download comes to the front, even if it was buried.
	_panel->showAndActivate();
	PopToFront(_panel.get());
}

void CaptureWindow::close() {
	// Closing never stops a download: it carries on in the list.
	if (_closing) {
		return;
	}
	_closing = true;
	_itemLifetime.destroy();
	if (!_panel->isHidden()) {
		_panel->hideGetDuration();
	} else if (const auto callback = _closed) {
		crl::on_main(callback);
	}
}

} // namespace

void ShowCaptureWindow(AddRequest request) {
	const auto offset = int(Windows.size());
	auto window = std::unique_ptr<CaptureWindow>();
	const auto raw = std::make_shared<CaptureWindow*>(nullptr);
	window = std::make_unique<CaptureWindow>(
		std::move(request),
		offset,
		[=] {
			const auto i = ranges::find_if(Windows, [&](const auto &w) {
				return w.get() == *raw;
			});
			if (i != end(Windows)) {
				Windows.erase(i);
			}
		});
	*raw = window.get();
	Windows.push_back(std::move(window));
}

} // namespace GB::Downloader
