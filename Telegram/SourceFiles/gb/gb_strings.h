/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// GB user-facing strings (English for now).
//
// Why not Resources/langs/lang.strings: that file generates a header that
// nearly every tdesktop source includes, so each new key triggers an almost
// full rebuild (~1.5 h here). GB strings are collected here during
// development and moved into lang.strings (as lng_gb_*) in one batch before
// translation work starts. Keep every GB string in this file.

namespace GB::Strings {

[[nodiscard]] inline QString Home() {
	return u"Home"_q;
}
[[nodiscard]] inline QString Feed() {
	return u"Feed"_q;
}
[[nodiscard]] inline QString Chats() {
	return u"Chats"_q;
}
[[nodiscard]] inline QString Transfers() {
	return u"Transfers"_q;
}

[[nodiscard]] inline QString HomeAbout() {
	return u"The downloader will live here: paste a link, choose the quality and download videos, audio and files."_q;
}
[[nodiscard]] inline QString FeedAbout() {
	return u"Posts from people and communities will appear here. Ask for a file you need, or share one you have."_q;
}
[[nodiscard]] inline QString TransfersAbout() {
	return u"Large files sent directly between devices will appear here, with progress and resume."_q;
}

} // namespace GB::Strings
