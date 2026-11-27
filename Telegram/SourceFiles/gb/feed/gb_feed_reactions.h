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

namespace GB::Feed {

// Facebook-style reactions on a Like button: resting the mouse on it for a
// moment pops a pill with Telegram's own animated reactions above it (the
// same icons and animations as in Telegram chats). The pill stays while the
// mouse travels from the button to it; "+" opens the full list.
void SetupReactionHover(
	not_null<Ui::RpWidget*> button,
	not_null<Window::SessionController*> controller,
	Fn<void(QString emoji)> chosen);

} // namespace GB::Feed
