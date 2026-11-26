/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "base/unique_qptr.h"
#include "gb/downloader/gb_dl_manager.h"
#include "ui/rp_widget.h"

class Painter;

namespace Ui {
class InputField;
class RoundButton;
class IconButton;
class FlatLabel;
class ScrollArea;
class SettingsSlider;
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace GB::Downloader {

class List final : public Ui::RpWidget {
public:
	List(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	void setFilter(Filter filter, const QString &query);
	void refresh();
	[[nodiscard]] bool empty() const {
		return _rows.empty();
	}

	int resizeGetHeight(int newWidth) override;

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;

private:
	struct Row {
		uint64 id = 0;
	};
	void paintRow(Painter &p, const Item &item, int top, bool over);
	void updateRow(uint64 id);
	void updateOver(QPoint position);
	[[nodiscard]] int rowHeight() const;
	[[nodiscard]] int rowIndexAt(QPoint position) const;
	[[nodiscard]] QRect circleRect(int top) const;
	void clickCircle(uint64 id);
	void openItem(uint64 id);
	void showMenu(uint64 id, QPoint globalPosition);

	const not_null<Window::SessionController*> _controller;
	std::vector<Row> _rows;
	Filter _filter = Filter::All;
	QString _query;
	int _over = -1;
	bool _overCircle = false;
	int _pressed = -1;
	uint64 _selected = 0;
	base::unique_qptr<Ui::PopupMenu> _menu;

};

class Widget final : public Ui::RpWidget {
public:
	Widget(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	void setInnerFocus();

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void submit();
	void showMainMenu();
	void updateSummary();
	void updateEmpty();
	void applyFilter();

	const not_null<Window::SessionController*> _controller;
	object_ptr<Ui::InputField> _field;
	object_ptr<Ui::RoundButton> _download;
	object_ptr<Ui::IconButton> _menuToggle;
	object_ptr<Ui::SettingsSlider> _tabs;
	object_ptr<Ui::FlatLabel> _summary;
	object_ptr<Ui::ScrollArea> _scroll;
	QPointer<List> _list;
	object_ptr<Ui::FlatLabel> _empty;
	base::unique_qptr<Ui::PopupMenu> _menu;
	Filter _filter = Filter::All;

};

} // namespace GB::Downloader
