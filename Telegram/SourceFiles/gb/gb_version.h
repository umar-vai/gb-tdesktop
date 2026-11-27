/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace GB {

// GB's own build number, separate from Telegram's AppVersion. Raise it by
// one for every build that is published with installer/publish-update.ps1;
// the in-app updater offers any published build with a higher number.
inline constexpr auto kVersion = 2;
inline constexpr auto kVersionString = "0.2.0";
inline constexpr auto kUpdateChannel = "beta";

} // namespace GB
