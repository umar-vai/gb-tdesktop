/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "gb/downloader/gb_dl_manager.h"

namespace GB::Downloader {

// The small "New download" window shown when the browser hands a download
// to GB, like Internet Download Manager's: a window of its own on top of the
// browser, not a box inside the Telegram window. It asks for the name and
// folder (Download / Later / Cancel), then shows the progress (Pause, Cancel,
// Hide) and, when the file is finished, the result (Open, Show in folder,
// Delete, Close). Port of Media-Downloader 6.1.3's capture_dialog.py.
void ShowCaptureWindow(AddRequest request);

} // namespace GB::Downloader
