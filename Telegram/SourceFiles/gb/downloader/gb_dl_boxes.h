/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "gb/downloader/gb_dl_common.h"

namespace Window {
class SessionController;
} // namespace Window

namespace GB::Downloader {

void StartDownload(
	not_null<Window::SessionController*> controller,
	const QString &url);
void ShowBatchBox(not_null<Window::SessionController*> controller);
void ShowSettingsBox(not_null<Window::SessionController*> controller);
void ShowPropertiesBox(
	not_null<Window::SessionController*> controller,
	uint64 id);

} // namespace GB::Downloader
