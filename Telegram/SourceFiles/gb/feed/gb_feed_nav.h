/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "ui/rp_widget.h"

namespace Ui {
class ScrollArea;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace GB::Feed {

// The Feed menu in the left column, like Facebook's left navigation:
// the community header, then Feed, Notifications, Friends, Communities,
// File requests, Saved and the profile.
class NavPanel final : public Ui::RpWidget {
public:
	NavPanel(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	object_ptr<Ui::ScrollArea> _scroll;

};

} // namespace GB::Feed
