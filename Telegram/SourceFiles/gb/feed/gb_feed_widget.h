/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "base/unique_qptr.h"
#include "ui/rp_widget.h"

namespace Ui {
class RoundButton;
class IconButton;
class FlatLabel;
class InputField;
class ScrollArea;
class SettingsSlider;
class PopupMenu;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace GB::Feed {

class PostList;

// The Feed page: sign-in, composer, search, tabs and the post list.
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
	void showMainMenu();
	void showTab(int index);
	void updateSignedIn();
	void signIn();

	const not_null<Window::SessionController*> _controller;
	object_ptr<Ui::RoundButton> _compose;
	object_ptr<Ui::InputField> _search;
	object_ptr<Ui::RoundButton> _notifications;
	object_ptr<Ui::IconButton> _menuToggle;
	object_ptr<Ui::SettingsSlider> _tabs;
	object_ptr<Ui::ScrollArea> _scroll;
	QPointer<PostList> _list;
	object_ptr<Ui::FlatLabel> _signInAbout;
	object_ptr<Ui::RoundButton> _signIn;
	base::unique_qptr<Ui::PopupMenu> _menu;
	int _tab = 0;
	bool _signingIn = false;

};

} // namespace GB::Feed
