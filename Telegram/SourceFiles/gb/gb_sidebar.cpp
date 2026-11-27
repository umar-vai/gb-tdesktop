/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/gb_sidebar.h"

#include "gb/gb_section.h"
#include "gb/gb_strings.h"
#include "gb/feed/gb_feed_boxes.h"
#include "gb/transfers/gb_tr_boxes.h"
#include "gb/updates/gb_updates.h"
#include "ui/filter_icons.h"
#include "ui/widgets/side_bar_button.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

namespace GB {

not_null<Ui::VerticalLayout*> SetupSidebarSections(
		not_null<Ui::RpWidget*> parent,
		not_null<Window::SessionController*> controller,
		const style::SideBarButton &st) {
	struct Entry {
		Panel panel = Panel::Chats;
		QString label;
		Ui::FilterIcon icon = Ui::FilterIcon::All;
	};
	const auto entries = std::vector<Entry>{
		{ Panel::Home, Strings::Home(), Ui::FilterIcon::Home },
		{ Panel::Feed, Strings::Feed(), Ui::FilterIcon::Like },
		{ Panel::Chats, Strings::Chats(), Ui::FilterIcon::All },
		{ Panel::Transfers, Strings::Transfers(), Ui::FilterIcon::Airplane },
	};

	const auto layout = Ui::CreateChild<Ui::VerticalLayout>(parent.get());
	for (const auto &entry : entries) {
		const auto button = layout->add(object_ptr<Ui::SideBarButton>(
			layout,
			TextWithEntities{ entry.label },
			st));
		const auto &icons = Ui::LookupFilterIcon(entry.icon);
		button->setIconOverride(icons.normal, icons.active);
		button->setAccessibleName(entry.label);

		// Each button switches the left column. The main column keeps the
		// Feed; the Feed button also brings it back when a chat is open.
		const auto panel = entry.panel;
		button->setClickedCallback([=] {
			ShowPanel(panel);
			if (panel == Panel::Feed) {
				controller->showSection(
					std::make_shared<SectionMemento>(Section::Feed),
					Window::SectionShow(Window::SectionShow::Way::ClearStack));
			}
		});
		PanelValue(
		) | rpl::on_next([=](Panel shown) {
			button->setActive(shown == panel);
		}, button->lifetime());
	}
	Transfers::WatchIncoming(controller);
	Feed::WatchFeed(controller);
	Updates::Watch(controller);

	layout->show();
	return layout;
}

} // namespace GB
