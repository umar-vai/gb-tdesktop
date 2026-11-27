/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace GB::Transfers {

struct InviteInfo;

// Pick a file, offer it with p2pd and send the gb:// link to a chat.
void SendFile(not_null<Window::SessionController*> controller);

// Send an existing offer's link to a chat.
void SendInviteToChat(
	not_null<Window::SessionController*> controller,
	const QString &invite,
	const QString &name,
	int64 size,
	Fn<void(bool sent)> finished = nullptr);

// Paste a gb:// link by hand.
void ShowReceiveLinkBox(not_null<Window::SessionController*> controller);

// "X wants to send you a file": Receive / Later / Decline.
void ShowIncomingBox(
	not_null<Window::SessionController*> controller,
	const InviteInfo &info,
	const QString &from,
	TimeId date);

// Start receiving into the default folder and tell the user.
void Receive(
	not_null<Window::SessionController*> controller,
	const QString &link);

// Starts the engine and pops ShowIncomingBox for every new incoming
// message that carries a gb:// link. Safe to call once per window.
void WatchIncoming(not_null<Window::SessionController*> controller);

} // namespace GB::Transfers
