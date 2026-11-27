/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "base/unique_qptr.h"
#include "ui/rp_widget.h"

class Painter;

namespace Ui {
class RoundButton;
class IconButton;
class LinkButton;
class FlatLabel;
class ScrollArea;
class SettingsSlider;
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace GB::Transfers {

enum class Tab : uchar {
	Receiving,
	Sending,
};

class List final : public Ui::RpWidget {
public:
	List(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	void setTab(Tab tab);
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
	enum class Kind : uchar {
		Receive,
		Pending,
		Send,
	};
	enum class Circle : uchar {
		None,
		Stop,
		Start,
	};
	struct Row {
		Kind kind = Kind::Receive;
		QString key;
		QString name;
		QString status;
		QString info;
		QString path;
		float64 progress = 0.;
		Circle circle = Circle::None;
		bool error = false;
		bool active = false;
	};
	void paintRow(Painter &p, const Row &row, int top, bool over);
	void updateOver(QPoint position);
	[[nodiscard]] int rowHeight() const;
	[[nodiscard]] int rowIndexAt(QPoint position) const;
	[[nodiscard]] QRect circleRect(int top) const;
	void clickCircle(const Row &row);
	void openRow(const Row &row);
	void showMenu(const Row &row, QPoint globalPosition);

	const not_null<Window::SessionController*> _controller;
	std::vector<Row> _rows;
	Tab _tab = Tab::Receiving;
	int _over = -1;
	bool _overCircle = false;
	int _pressed = -1;
	QString _selected;
	base::unique_qptr<Ui::PopupMenu> _menu;

};

class Widget final : public Ui::RpWidget {
public:
	Widget(
		QWidget *parent,
		not_null<Window::SessionController*> controller);
	~Widget();

	void setInnerFocus();

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void showMainMenu();
	void updateStatus();
	void updateEmpty();

	const not_null<Window::SessionController*> _controller;
	object_ptr<Ui::RoundButton> _send;
	object_ptr<Ui::RoundButton> _receive;
	object_ptr<Ui::IconButton> _menuToggle;
	object_ptr<Ui::FlatLabel> _status;
	object_ptr<Ui::LinkButton> _signIn;
	object_ptr<Ui::SettingsSlider> _tabs;
	object_ptr<Ui::ScrollArea> _scroll;
	QPointer<List> _list;
	object_ptr<Ui::FlatLabel> _empty;
	base::unique_qptr<Ui::PopupMenu> _menu;
	Tab _tab = Tab::Receiving;

};

} // namespace GB::Transfers
