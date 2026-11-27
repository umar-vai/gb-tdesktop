/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Window {
class SessionController;
} // namespace Window

namespace GB::Updates {

// GB's own updater (Telegram's update servers and keys are not used).
// The backend serves a manifest signed with our Ed25519 key
// (installer/publish-update.ps1). A newer build is downloaded in the
// background, checked (signature, size, SHA-256) and then offered with an
// "Update GB" button at the bottom of the chat list, like Telegram's.

// Starts checking: shortly after launch, then every few hours.
void Watch(not_null<Window::SessionController*> controller);

// A verified update is downloaded and ready to install.
[[nodiscard]] bool Ready();
[[nodiscard]] rpl::producer<bool> ReadyValue();
[[nodiscard]] QString ReadyVersion();

// Check right now (the result is reported with a toast when asked by hand).
void CheckNow(Fn<void(QString)> report = nullptr);

// Quits, replaces the app files with the new build and starts it again.
void Install();

} // namespace GB::Updates
