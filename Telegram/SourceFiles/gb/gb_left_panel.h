/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Ui {
class RpWidget;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace GB {

// Covers the chat list (the left column) with the GB panel chosen in the
// sidebar: Home = downloader, Transfers, Feed = the Feed menu. Hidden when
// the panel is Chats, so Telegram's own chat list shows through.
void SetupLeftPanel(
	not_null<Ui::RpWidget*> dialogs,
	not_null<Window::SessionController*> controller);

} // namespace GB
