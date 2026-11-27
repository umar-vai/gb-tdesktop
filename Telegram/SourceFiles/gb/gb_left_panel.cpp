/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/gb_left_panel.h"

#include "base/object_ptr.h"
#include "gb/gb_section.h"
#include "gb/downloader/gb_dl_widget.h"
#include "gb/feed/gb_feed_nav.h"
#include "gb/transfers/gb_tr_widget.h"
#include "ui/rp_widget.h"
#include "window/window_session_controller.h"
#include "styles/style_window.h"

namespace GB {
namespace {

class LeftPanel final : public Ui::RpWidget {
public:
	LeftPanel(
		not_null<Ui::RpWidget*> parent,
		not_null<Window::SessionController*> controller);

protected:
	void resizeEvent(QResizeEvent *e) override;
	void paintEvent(QPaintEvent *e) override;

private:
	void showPanel(Panel panel);

	const not_null<Window::SessionController*> _controller;
	object_ptr<Downloader::Widget> _downloader = { nullptr };
	object_ptr<Transfers::Widget> _transfers = { nullptr };
	object_ptr<Feed::NavPanel> _feed = { nullptr };

};

LeftPanel::LeftPanel(
	not_null<Ui::RpWidget*> parent,
	not_null<Window::SessionController*> controller)
: RpWidget(parent)
, _controller(controller) {
	setAttribute(Qt::WA_OpaquePaintEvent);
	parent->sizeValue(
	) | rpl::on_next([=](QSize size) {
		setGeometry(QRect(QPoint(), size));
		if (!isHidden()) {
			raise();
		}
	}, lifetime());

	PanelValue(
	) | rpl::on_next([=](Panel panel) {
		showPanel(panel);
	}, lifetime());
}

void LeftPanel::showPanel(Panel panel) {
	// Pages are created on first use and kept, so a running download list
	// or a scrolled menu stays as it was when switching back and forth.
	if (panel == Panel::Home && !_downloader) {
		_downloader.create(this, _controller);
	} else if (panel == Panel::Transfers && !_transfers) {
		_transfers.create(this, _controller);
	} else if (panel == Panel::Feed && !_feed) {
		_feed.create(this, _controller);
	}
	const auto toggle = [&](auto &widget, bool shown) {
		if (widget) {
			widget->setGeometry(rect());
			widget->setVisible(shown);
		}
	};
	toggle(_downloader, panel == Panel::Home);
	toggle(_transfers, panel == Panel::Transfers);
	toggle(_feed, panel == Panel::Feed);

	if (panel == Panel::Chats) {
		hide();
		return;
	}
	RpWidget::show();
	raise();
	// Chat list children may be raised after us during this event loop.
	crl::on_main(this, [=] {
		if (!isHidden()) {
			raise();
		}
	});
	if (panel == Panel::Home) {
		_downloader->setInnerFocus();
	} else if (panel == Panel::Transfers) {
		_transfers->setInnerFocus();
	}
}

void LeftPanel::resizeEvent(QResizeEvent *e) {
	for (const auto widget : std::initializer_list<QWidget*>{
		_downloader.data(),
		_transfers.data(),
		_feed.data(),
	}) {
		if (widget) {
			widget->setGeometry(rect());
		}
	}
}

void LeftPanel::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::windowBg);
}

} // namespace

void SetupLeftPanel(
		not_null<Ui::RpWidget*> dialogs,
		not_null<Window::SessionController*> controller) {
	Ui::CreateChild<LeftPanel>(dialogs.get(), controller);
}

} // namespace GB
