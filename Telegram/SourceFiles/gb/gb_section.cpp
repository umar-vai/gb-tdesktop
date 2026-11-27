/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/gb_section.h"

#include "gb/downloader/gb_dl_widget.h"
#include "gb/feed/gb_feed_widget.h"
#include "gb/transfers/gb_tr_widget.h"
#include "gb/gb_strings.h"
#include "ui/widgets/labels.h"
#include "ui/painter.h"
#include "window/window_session_controller.h"
#include "styles/style_gb.h"
#include "styles/style_window.h"

namespace GB {
namespace {

[[nodiscard]] rpl::variable<Section> &ShownSection() {
	static auto result = rpl::variable<Section>(Section::None);
	return result;
}

[[nodiscard]] QString TitleFor(Section section) {
	switch (section) {
	case Section::Home: return Strings::Home();
	case Section::Feed: return Strings::Feed();
	case Section::Transfers: return Strings::Transfers();
	case Section::None: break;
	}
	return Strings::Chats();
}

[[nodiscard]] QString AboutFor(Section section) {
	switch (section) {
	case Section::Home: return Strings::HomeAbout();
	case Section::Feed: return Strings::FeedAbout();
	case Section::Transfers: return Strings::TransfersAbout();
	case Section::None: break;
	}
	return QString();
}

[[nodiscard]] rpl::variable<Panel> &PanelState() {
	static auto result = rpl::variable<Panel>(Panel::Home);
	return result;
}

} // namespace

rpl::producer<Section> ShownSectionValue() {
	return ShownSection().value();
}

Panel CurrentPanel() {
	return PanelState().current();
}

rpl::producer<Panel> PanelValue() {
	return PanelState().value();
}

void ShowPanel(Panel panel) {
	PanelState() = panel;
}

void ShowDefaultSection(not_null<Window::SessionController*> controller) {
	crl::on_main(controller, [=] {
		if (ShownSection().current() == Section::Feed
			|| controller->activeChatCurrent()) {
			return;
		}
		controller->showSection(
			std::make_shared<SectionMemento>(Section::Feed),
			Window::SectionShow(
				Window::SectionShow::Way::ClearStack,
				anim::type::instant,
				anim::activation::background));
	});
}

object_ptr<Window::SectionWidget> SectionMemento::createWidget(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		Window::Column column,
		const QRect &geometry) {
	auto result = object_ptr<SectionWidget>(parent, controller, _section);
	result->setGeometry(geometry);
	return result;
}

SectionWidget::SectionWidget(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	Section section)
: Window::SectionWidget(parent, controller)
, _section(section)
, _title(this, TitleFor(section), st::gbPageTitle)
, _about(this, AboutFor(section), st::gbPageAbout) {
	setAttribute(Qt::WA_OpaquePaintEvent);
	if (_section == Section::Home) {
		_title->hide();
		_about->hide();
		_downloader.create(this, controller);
	} else if (_section == Section::Transfers) {
		_title->hide();
		_about->hide();
		_transfers.create(this, controller);
	} else if (_section == Section::Feed) {
		_title->hide();
		_about->hide();
		_feed.create(this, controller);
	}
	ShownSection() = _section;
}

SectionWidget::~SectionWidget() {
	// A newer GB page may already be shown (it is created before this one
	// is destroyed); only clear the state if it still points at us.
	if (ShownSection().current() == _section) {
		ShownSection() = Section::None;
	}
}

bool SectionWidget::showInternal(
		not_null<Window::SectionMemento*> memento,
		const Window::SectionShow &params) {
	// Showing the page that is already open keeps it as is.
	return sameTypeAs(memento);
}

bool SectionWidget::sameTypeAs(not_null<Window::SectionMemento*> memento) {
	const auto other = dynamic_cast<SectionMemento*>(memento.get());
	return other && (other->section() == _section);
}

bool SectionWidget::floatPlayerHandleWheelEvent(QEvent *e) {
	return false;
}

QRect SectionWidget::floatPlayerAvailableRect() {
	return mapToGlobal(rect());
}

void SectionWidget::doSetInnerFocus() {
	if (_downloader) {
		_downloader->setInnerFocus();
	} else if (_transfers) {
		_transfers->setInnerFocus();
	} else if (_feed) {
		_feed->setInnerFocus();
	} else {
		setFocus();
	}
}

void SectionWidget::resizeEvent(QResizeEvent *e) {
	if (_downloader) {
		_downloader->setGeometry(rect());
		return;
	} else if (_transfers) {
		_transfers->setGeometry(rect());
		return;
	} else if (_feed) {
		_feed->setGeometry(rect());
		return;
	}
	const auto &padding = st::gbPagePadding;
	const auto available = width() - padding.left() - padding.right();
	const auto contentWidth = std::max(
		std::min(available, st::gbPageMaxWidth),
		0);
	const auto left = (width() - contentWidth) / 2;
	_title->resizeToWidth(contentWidth);
	_title->moveToLeft(left, padding.top());
	_about->resizeToWidth(contentWidth);
	_about->moveToLeft(
		left,
		padding.top() + _title->height() + st::gbPageAboutSkip);
}

void SectionWidget::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::windowBg);
}

} // namespace GB
