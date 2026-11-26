/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace style {
struct SideBarButton;
} // namespace style

namespace Ui {
class RpWidget;
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace GB {

// Adds the GB entries (Home, Feed, Chats, Transfers) to the left sidebar.
// The caller owns placement: it positions and sizes the returned layout.
[[nodiscard]] not_null<Ui::VerticalLayout*> SetupSidebarSections(
	not_null<Ui::RpWidget*> parent,
	not_null<Window::SessionController*> controller,
	const style::SideBarButton &st);

} // namespace GB
