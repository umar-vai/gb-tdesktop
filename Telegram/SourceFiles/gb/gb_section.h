/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/object_ptr.h"
#include "window/section_memento.h"
#include "window/section_widget.h"

namespace Ui {
class FlatLabel;
} // namespace Ui

namespace GB::Downloader {
class Widget;
} // namespace GB::Downloader

namespace GB::Transfers {
class Widget;
} // namespace GB::Transfers

namespace GB::Feed {
class Widget;
} // namespace GB::Feed

namespace GB {

// The GB-only main sections. Chats is the regular Telegram chat list, so
// it has no section of its own: "no GB section shown" means Chats.
enum class Section : uchar {
	None,
	Home,
	Feed,
	Transfers,
};

// Which GB section is currently shown in the main column (None = chats).
[[nodiscard]] rpl::producer<Section> ShownSectionValue();

// What the left column (Telegram's chat list place) shows. The sidebar
// buttons switch it; the main column keeps showing the Feed (or a chat).
enum class Panel : uchar {
	Chats,
	Home,
	Feed,
	Transfers,
};

[[nodiscard]] Panel CurrentPanel();
[[nodiscard]] rpl::producer<Panel> PanelValue();
void ShowPanel(Panel panel);

// Shows the Feed in the main column unless a chat or the Feed already is.
// Called when the main column would otherwise be empty.
void ShowDefaultSection(not_null<Window::SessionController*> controller);

class SectionMemento final : public Window::SectionMemento {
public:
	explicit SectionMemento(Section section) : _section(section) {
	}

	object_ptr<Window::SectionWidget> createWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		Window::Column column,
		const QRect &geometry) override;

	[[nodiscard]] Section section() const {
		return _section;
	}

private:
	Section _section = Section::Home;

};

// A GB section page. Home is the downloader; Feed and Transfers are still
// placeholders (docs/ARCHITECTURE.md).
class SectionWidget final : public Window::SectionWidget {
public:
	SectionWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		Section section);
	~SectionWidget();

	[[nodiscard]] Section section() const {
		return _section;
	}

	bool showInternal(
		not_null<Window::SectionMemento*> memento,
		const Window::SectionShow &params) override;
	bool sameTypeAs(not_null<Window::SectionMemento*> memento) override;

	// Float player interface (required by Window::AbstractSectionWidget).
	bool floatPlayerHandleWheelEvent(QEvent *e) override;
	QRect floatPlayerAvailableRect() override;

	void doSetInnerFocus() override;

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	const Section _section;
	object_ptr<Ui::FlatLabel> _title;
	object_ptr<Ui::FlatLabel> _about;
	object_ptr<Downloader::Widget> _downloader = { nullptr };
	object_ptr<Transfers::Widget> _transfers = { nullptr };
	object_ptr<Feed::Widget> _feed = { nullptr };

};

} // namespace GB
