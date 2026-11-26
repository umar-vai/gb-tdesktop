/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/downloader/gb_dl_widget.h"

#include "core/file_utilities.h"
#include "gb/downloader/gb_dl_boxes.h"
#include "layout/layout_document_generic_preview.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"
#include "styles/style_chat_style.h"
#include "styles/style_dialogs.h"
#include "styles/style_gb.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_overview.h"
#include "styles/style_widgets.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtGui/QClipboard>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>

namespace GB::Downloader {
namespace {

[[nodiscard]] std::vector<QString> FilterLabels() {
	return {
		u"All"_q,
		u"Unfinished"_q,
		u"Finished"_q,
		u"Video"_q,
		u"Audio"_q,
		u"Files"_q,
	};
}

[[nodiscard]] QString StatusLine(const Item &item) {
	const auto display = DisplayOf(item);
	auto parts = QStringList();
	const auto &metrics = item.metrics;
	switch (display) {
	case Display::Downloading:
	case Display::Connecting:
	case Display::Finishing: {
		parts.push_back(DisplayText(item));
		if (metrics.downloaded > 0) {
			const auto total = FormatSize(metrics.total);
			parts.push_back(total.isEmpty()
				? FormatSize(metrics.downloaded)
				: (FormatSize(metrics.downloaded)
					+ u" of "_q
					+ (metrics.estimate ? u"~"_q : QString())
					+ total));
		}
		if (display != Display::Finishing && metrics.speed > 0.) {
			parts.push_back(FormatSpeed(metrics.speed));
		}
		if (display == Display::Downloading && metrics.eta >= 0) {
			parts.push_back(FormatEta(metrics.eta) + u" left"_q);
		}
	} break;
	case Display::Complete: {
		const auto size = item.fileSize
			? item.fileSize
			: QFileInfo(item.resultPath).size();
		parts.push_back(FormatSize(size));
		parts.push_back(item.detail.startsWith(u"Already"_q)
			? item.detail
			: u"Complete"_q);
	} break;
	case Display::Failed:
		parts.push_back(item.error.isEmpty()
			? u"Failed"_q
			: (u"Failed: "_q + item.error.simplified()));
		break;
	case Display::Paused:
		parts.push_back(DisplayText(item));
		if (metrics.downloaded > 0) {
			const auto total = FormatSize(metrics.total);
			parts.push_back(total.isEmpty()
				? FormatSize(metrics.downloaded)
				: (FormatSize(metrics.downloaded) + u" of "_q + total));
		}
		break;
	default:
		parts.push_back(DisplayText(item));
		break;
	}
	parts.removeAll(QString());
	return parts.join(u" • "_q);
}

[[nodiscard]] QString InfoLine(const Item &item) {
	auto parts = QStringList();
	parts.push_back(WebsiteOf(item.pageUrl.isEmpty() ? item.url : item.pageUrl));
	if (item.mode == Mode::Video && !item.quality.isEmpty()) {
		parts.push_back(item.quality);
	} else if (item.mode == Mode::Audio) {
		parts.push_back(item.audioFormat);
	}
	parts.push_back(FormatDate((item.finished.isValid()
		? item.finished
		: item.added)));
	parts.removeAll(QString());
	return parts.join(u" • "_q);
}

[[nodiscard]] QString ExtensionLabel(const Item &item) {
	auto ext = ExtensionOf(NameOf(item));
	if (ext.isEmpty()) {
		ext = (item.mode == Mode::Audio)
			? item.audioFormat.toLower()
			: (item.mode == Mode::Video)
			? u"mp4"_q
			: QString();
	}
	return ext.toUpper();
}

} // namespace

List::List(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: RpWidget(parent)
, _controller(controller) {
	setMouseTracking(true);
	auto &manager = Manager::Instance();
	manager.listChanges() | rpl::on_next([=] {
		refresh();
	}, lifetime());
	manager.itemUpdates() | rpl::on_next([=](uint64 id) {
		const auto item = Manager::Instance().find(id);
		if (item && !Matches(*item, _filter, _query)) {
			refresh();
		} else {
			updateRow(id);
		}
	}, lifetime());
	refresh();
}

void List::setFilter(Filter filter, const QString &query) {
	_filter = filter;
	_query = query;
	refresh();
}

void List::refresh() {
	_rows.clear();
	for (const auto item : Manager::Instance().items()) {
		if (Matches(*item, _filter, _query)) {
			_rows.push_back({ item->id });
		}
	}
	_over = -1;
	_overCircle = false;
	resizeToWidth(width());
	update();
}

int List::rowHeight() const {
	const auto &padding = st::gbDlRowPadding;
	return padding.top() + st::gbDlThumbSize + padding.bottom();
}

int List::resizeGetHeight(int newWidth) {
	return int(_rows.size()) * rowHeight();
}

int List::rowIndexAt(QPoint position) const {
	if (position.y() < 0 || position.x() < 0 || position.x() >= width()) {
		return -1;
	}
	const auto index = position.y() / rowHeight();
	return (index < int(_rows.size())) ? index : -1;
}

QRect List::circleRect(int top) const {
	const auto &padding = st::gbDlRowPadding;
	const auto thumb = QRect(
		padding.left(),
		top + padding.top(),
		st::gbDlThumbSize,
		st::gbDlThumbSize);
	const auto size = st::gbDlInnerSize;
	return QRect(
		thumb.x() + (thumb.width() - size) / 2,
		thumb.y() + (thumb.height() - size) / 2,
		size,
		size);
}

void List::updateRow(uint64 id) {
	for (auto i = 0; i != int(_rows.size()); ++i) {
		if (_rows[i].id == id) {
			update(0, i * rowHeight(), width(), rowHeight());
			return;
		}
	}
}

void List::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto clip = e->rect();
	p.fillRect(clip, st::windowBg);
	const auto height = rowHeight();
	const auto from = std::max(0, clip.top() / height);
	const auto till = std::min(
		int(_rows.size()),
		(clip.bottom() / height) + 1);
	auto &manager = Manager::Instance();
	for (auto i = from; i < till; ++i) {
		if (const auto item = manager.find(_rows[i].id)) {
			paintRow(p, *item, i * height, (i == _over));
		}
	}
}

void List::paintRow(Painter &p, const Item &item, int top, bool over) {
	const auto height = rowHeight();
	const auto selected = (_selected == item.id);
	if (selected || over) {
		p.fillRect(0, top, width(), height, selected
			? st::dialogsBgOver
			: st::windowBgOver);
	}
	const auto &padding = st::gbDlRowPadding;
	const auto name = NameOf(item);
	const auto preview = Layout::DocumentGenericPreview::Create(
		u"file."_q + ExtensionLabel(item).toLower());
	const auto thumb = QRect(
		padding.left(),
		top + padding.top(),
		st::gbDlThumbSize,
		st::gbDlThumbSize);
	const auto display = DisplayOf(item);
	const auto active = (display == Display::Connecting)
		|| (display == Display::Downloading)
		|| (display == Display::Finishing)
		|| (display == Display::Stopping)
		|| (display == Display::Queued)
		|| (display == Display::Retrying);
	const auto resumable = (display == Display::Paused)
		|| (display == Display::Failed)
		|| (display == Display::Cancelled);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(preview.color);
		p.drawRoundedRect(thumb, st::roundRadiusSmall, st::roundRadiusSmall);
	}
	if (active || resumable) {
		const auto inner = circleRect(top);
		const auto overCircle = over && _overCircle;
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(overCircle ? preview.over : preview.dark);
			p.drawEllipse(inner);
		}
		const auto &icon = active
			? st::historyFileThumbCancel
			: st::historyFileThumbDownload;
		icon.paintInCenter(p, inner);
		if (active) {
			auto hq = PainterHighQualityEnabler(p);
			const auto line = st::gbDlRadialLine;
			auto pen = st::historyFileThumbRadialFg->p;
			pen.setWidth(line);
			pen.setCapStyle(Qt::RoundCap);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			const auto arc = QRectF(inner).marginsRemoved(
				QMarginsF(line / 2., line / 2., line / 2., line / 2.));
			const auto value = std::clamp(item.progress, 0.03, 1.);
			p.drawArc(arc, 90 * 16, -int(value * 360 * 16));
		}
	} else {
		const auto ext = st::gbDlExtFont->elided(
			ExtensionLabel(item),
			st::gbDlThumbSize - 8);
		p.setFont(st::gbDlExtFont);
		p.setPen(st::overviewFileExtFg);
		p.drawText(thumb, Qt::AlignCenter, ext);
	}

	const auto left = thumb.x() + thumb.width() + st::gbDlNameLeft;
	const auto available = width() - left - padding.right();
	if (available <= 0) {
		return;
	}
	p.setFont(st::semiboldFont);
	p.setPen(st::windowBoldFg);
	p.drawTextLeft(
		left,
		top + st::gbDlNameTop,
		width(),
		st::semiboldFont->elided(name, available));

	p.setFont(st::normalFont);
	p.setPen((display == Display::Failed || display == Display::Missing)
		? st::boxTextFgError
		: (display == Display::Downloading)
		? st::windowActiveTextFg
		: st::windowSubTextFg);
	p.drawTextLeft(
		left,
		top + st::gbDlStatusTop,
		width(),
		st::normalFont->elided(StatusLine(item), available));

	p.setPen(st::windowSubTextFg);
	p.drawTextLeft(
		left,
		top + st::gbDlInfoTop,
		width(),
		st::normalFont->elided(InfoLine(item), available));

	if (!selected) {
		p.fillRect(
			left,
			top + height - st::lineWidth,
			width() - left,
			st::lineWidth,
			st::shadowFg);
	}
}

void List::updateOver(QPoint position) {
	const auto index = rowIndexAt(position);
	auto overCircle = false;
	if (index >= 0) {
		overCircle = circleRect(index * rowHeight()).contains(position);
	}
	if (index != _over || overCircle != _overCircle) {
		if (_over >= 0) {
			update(0, _over * rowHeight(), width(), rowHeight());
		}
		_over = index;
		_overCircle = overCircle;
		if (_over >= 0) {
			update(0, _over * rowHeight(), width(), rowHeight());
		}
		setCursor(_overCircle ? style::cur_pointer : style::cur_default);
	}
}

void List::mouseMoveEvent(QMouseEvent *e) {
	updateOver(e->pos());
}

void List::leaveEventHook(QEvent *e) {
	updateOver(QPoint(-1, -1));
}

void List::mousePressEvent(QMouseEvent *e) {
	updateOver(e->pos());
	_pressed = (e->button() == Qt::LeftButton) ? _over : -1;
	if (_over >= 0 && _over < int(_rows.size())) {
		_selected = _rows[_over].id;
		update();
	}
}

void List::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = base::take(_pressed);
	updateOver(e->pos());
	if (pressed >= 0 && pressed == _over && _overCircle) {
		clickCircle(_rows[pressed].id);
	}
}

void List::mouseDoubleClickEvent(QMouseEvent *e) {
	updateOver(e->pos());
	if (_over >= 0 && !_overCircle) {
		openItem(_rows[_over].id);
	}
}

void List::contextMenuEvent(QContextMenuEvent *e) {
	const auto position = (e->reason() == QContextMenuEvent::Mouse)
		? e->pos()
		: QPoint(0, 0);
	const auto index = rowIndexAt(position);
	if (index < 0) {
		return;
	}
	_selected = _rows[index].id;
	update();
	showMenu(_rows[index].id, e->globalPos());
	e->accept();
}

void List::clickCircle(uint64 id) {
	auto &manager = Manager::Instance();
	const auto item = manager.find(id);
	if (!item) {
		return;
	}
	if (CanStop(*item)) {
		manager.pause(id);
	} else if (CanResume(*item)) {
		manager.resume(id);
	} else if (DisplayOf(*item) == Display::Complete) {
		openItem(id);
	}
}

void List::openItem(uint64 id) {
	const auto item = Manager::Instance().find(id);
	if (!item) {
		return;
	}
	const auto display = DisplayOf(*item);
	if (display == Display::Complete && !item->resultPath.isEmpty()) {
		File::Launch(item->resultPath);
	} else if (display == Display::Missing) {
		_controller->showToast(u"The file was moved or deleted."_q);
	} else if (CanResume(*item)) {
		Manager::Instance().resume(id);
	} else if (display == Display::Failed) {
		ShowPropertiesBox(_controller, id);
	}
}

void List::showMenu(uint64 id, QPoint globalPosition) {
	const auto item = Manager::Instance().find(id);
	if (!item) {
		return;
	}
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	const auto menu = _menu.get();
	const auto controller = _controller;
	const auto display = DisplayOf(*item);
	const auto path = item->resultPath;
	const auto folder = item->folder;
	const auto url = item->url;
	const auto page = item->pageUrl;
	if (display == Display::Complete) {
		menu->addAction(u"Open"_q, [=] {
			File::Launch(path);
		}, &st::menuIconFile);
		menu->addAction(u"Show in folder"_q, [=] {
			File::ShowInFolder(path);
		}, &st::menuIconShowInFolder);
	} else if (!folder.isEmpty()) {
		menu->addAction(u"Open folder"_q, [=] {
			QDir().mkpath(folder);
			File::Launch(folder);
		}, &st::menuIconShowInFolder);
	}
	if (CanStop(*item)) {
		menu->addAction(u"Pause"_q, [=] {
			Manager::Instance().pause(id);
		}, &st::menuIconCancel);
	}
	if (display == Display::Paused) {
		menu->addAction(u"Resume"_q, [=] {
			Manager::Instance().resume(id);
		}, &st::menuIconDownload);
	} else if (display == Display::Failed || display == Display::Cancelled) {
		menu->addAction(u"Retry"_q, [=] {
			Manager::Instance().retry(id);
		}, &st::menuIconRestore);
	}
	if ((display == Display::Complete || display == Display::Missing)
		&& !url.isEmpty()) {
		menu->addAction(u"Download again"_q, [=] {
			Manager::Instance().redownload(id);
		}, &st::menuIconDownload);
	}
	if (!url.isEmpty()) {
		menu->addAction(u"Copy link"_q, [=] {
			QGuiApplication::clipboard()->setText(url);
			controller->showToast(u"Link copied."_q);
		}, &st::menuIconLink);
	}
	if (display == Display::Complete) {
		menu->addAction(u"Copy file path"_q, [=] {
			QGuiApplication::clipboard()->setText(path);
			controller->showToast(u"File path copied."_q);
		}, &st::menuIconCopy);
	}
	if (!page.isEmpty() || !url.isEmpty()) {
		menu->addAction(u"Open web page"_q, [=] {
			File::OpenUrl(page.isEmpty() ? url : page);
		}, &st::menuIconShare);
	}
	menu->addAction(u"Properties"_q, [=] {
		ShowPropertiesBox(controller, id);
	}, &st::menuIconInfo);
	menu->addSeparator();
	if (CanStop(*item) || display == Display::Paused) {
		menu->addAction(u"Cancel download"_q, [=] {
			Manager::Instance().cancel(id);
		}, &st::menuIconCancel);
	}
	menu->addAction(u"Remove from list"_q, [=] {
		Manager::Instance().remove(id, false);
	}, &st::menuIconRemove);
	if (display == Display::Complete) {
		menu->addAction(u"Delete file"_q, [=] {
			const auto name = QFileInfo(path).fileName();
			controller->show(Ui::MakeConfirmBox({
				.text = u"Delete \"%1\" from the disk? This cannot be "
					"undone."_q.arg(name),
				.confirmed = [=](Fn<void()> close) {
					Manager::Instance().remove(id, true);
					close();
				},
				.confirmText = rpl::single(u"Delete"_q),
				.confirmStyle = &st::attentionBoxButton,
			}));
		}, &st::menuIconDelete);
	}
	menu->popup(globalPosition);
}

Widget::Widget(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: RpWidget(parent)
, _controller(controller)
, _field(
	this,
	st::dialogsFilter,
	Ui::InputField::Mode::NoNewlines,
	rpl::single(u"Paste a link: video, music or any file"_q))
, _download(this, rpl::single(u"Download"_q), st::defaultActiveButton)
, _menuToggle(this, st::menuToggle)
, _tabs(this, st::defaultTabsSlider)
, _summary(this, QString(), st::gbDlSummary)
, _scroll(this)
, _empty(this, QString(), st::gbDlEmpty) {
	_download->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	_download->setClickedCallback([=] { submit(); });
	_field->submits() | rpl::on_next([=] {
		submit();
	}, _field->lifetime());
	_menuToggle->setClickedCallback([=] { showMainMenu(); });

	_tabs->setSections(FilterLabels());
	_tabs->setActiveSectionFast(0);
	_tabs->sectionActivated() | rpl::on_next([=](int index) {
		_filter = Filter(std::clamp(index, 0, 5));
		applyFilter();
	}, _tabs->lifetime());

	_list = _scroll->setOwnedWidget(object_ptr<List>(this, controller));
	_empty->setAttribute(Qt::WA_TransparentForMouseEvents);

	auto &manager = Manager::Instance();
	manager.listChanges() | rpl::on_next([=] {
		updateSummary();
		updateEmpty();
	}, lifetime());
	manager.itemUpdates() | rpl::on_next([=] {
		updateSummary();
	}, lifetime());
	updateSummary();
	updateEmpty();
}

void Widget::setInnerFocus() {
	_field->setFocusFast();
}

void Widget::submit() {
	const auto text = _field->getLastText().trimmed();
	if (text.isEmpty()) {
		_field->showError();
		return;
	}
	const auto parsed = ParseBatchUrls(text);
	if (parsed.urls.size() > 1) {
		ShowBatchBox(_controller);
		return;
	}
	StartDownload(_controller, parsed.urls.isEmpty()
		? text
		: parsed.urls.front());
	_field->setText(QString());
}

void Widget::showMainMenu() {
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	const auto menu = _menu.get();
	const auto controller = _controller;
	menu->addAction(u"Batch download"_q, [=] {
		ShowBatchBox(controller);
	}, &st::menuIconAdd);
	menu->addAction(u"Resume all"_q, [=] {
		Manager::Instance().resumeAll();
	}, &st::menuIconDownload);
	menu->addAction(u"Pause all"_q, [=] {
		Manager::Instance().pauseAll();
	}, &st::menuIconCancel);
	menu->addAction(u"Retry failed"_q, [=] {
		const auto count = Manager::Instance().retryFailed();
		controller->showToast(count
			? u"Retrying %1 failed downloads."_q.arg(count)
			: u"No failed downloads."_q);
	}, &st::menuIconRestore);
	menu->addAction(u"Clean up list"_q, [=] {
		const auto count = Manager::Instance().cleanUp();
		controller->showToast(count
			? u"Removed %1 failed, cancelled or missing items."_q.arg(count)
			: u"Nothing to clean up."_q);
	}, &st::menuIconClear);
	menu->addAction(u"Open download folder"_q, [=] {
		const auto folder = Manager::Instance().settings().downloadDir;
		QDir().mkpath(folder);
		File::Launch(folder);
	}, &st::menuIconShowInFolder);
	menu->addSeparator();
	menu->addAction(u"Downloader settings"_q, [=] {
		ShowSettingsBox(controller);
	}, &st::menuIconSettings);
	menu->popup(QCursor::pos());
}

void Widget::updateSummary() {
	const auto totals = Manager::Instance().totals();
	auto parts = QStringList();
	if (totals.downloading) {
		parts.push_back(u"%1 downloading"_q.arg(totals.downloading));
	}
	if (totals.speed > 0.) {
		parts.push_back(FormatSpeed(totals.speed));
	}
	if (totals.queued) {
		parts.push_back(u"%1 queued"_q.arg(totals.queued));
	}
	if (totals.paused) {
		parts.push_back(u"%1 paused"_q.arg(totals.paused));
	}
	if (totals.failed) {
		parts.push_back(u"%1 failed"_q.arg(totals.failed));
	}
	if (parts.isEmpty() && !Manager::Instance().engineAvailable()) {
		parts.push_back(u"Video engine (yt-dlp) not found: only direct "
			"file links will work. See Downloader settings."_q);
	}
	_summary->setText(parts.join(u" • "_q));
}

void Widget::updateEmpty() {
	if (!_list) {
		return;
	}
	const auto empty = _list->empty();
	_empty->setText((_filter == Filter::All && _list->empty())
		? u"No downloads yet.\nPaste a link above to download a video, "
			"music or any file."_q
		: u"Nothing here."_q);
	_empty->setVisible(empty);
	resizeEvent(nullptr);
}

void Widget::applyFilter() {
	if (_list) {
		_list->setFilter(_filter, QString());
	}
	updateEmpty();
}

void Widget::resizeEvent(QResizeEvent *e) {
	const auto &padding = st::gbDlPadding;
	const auto available = width() - padding.left() - padding.right();
	const auto contentWidth = std::max(
		std::min(available, st::gbDlMaxWidth),
		0);
	const auto left = (width() - contentWidth) / 2;
	auto top = padding.top();

	const auto buttonsWidth = _download->width()
		+ st::gbDlFieldSkip
		+ _menuToggle->width();
	const auto fieldWidth = std::max(contentWidth - buttonsWidth - st::gbDlFieldSkip, 0);
	_field->resizeToWidth(fieldWidth);
	_field->moveToLeft(left, top);
	const auto rowHeight = std::max(_field->height(), _download->height());
	_download->moveToLeft(
		left + fieldWidth + st::gbDlFieldSkip,
		top + (rowHeight - _download->height()) / 2);
	_menuToggle->moveToLeft(
		left + contentWidth - _menuToggle->width(),
		top + (rowHeight - _menuToggle->height()) / 2);
	top += rowHeight + st::gbDlTopSkip;

	_summary->resizeToWidth(contentWidth);
	_summary->moveToLeft(left, top);
	top += st::gbDlSummaryHeight;

	_tabs->resizeToWidth(contentWidth);
	_tabs->moveToLeft(left, top);
	top += _tabs->height() + st::gbDlTabsSkip;

	_scroll->setGeometry(left, top, contentWidth, std::max(height() - top, 0));
	if (_list) {
		_list->resizeToWidth(contentWidth);
	}
	_empty->resizeToWidth(contentWidth);
	_empty->moveToLeft(left, top + st::gbDlEmptyTop);
}

void Widget::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::windowBg);
}

} // namespace GB::Downloader
