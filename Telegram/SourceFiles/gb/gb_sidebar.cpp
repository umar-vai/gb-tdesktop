/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/gb_sidebar.h"

#include "gb/gb_section.h"
#include "gb/gb_strings.h"
#include "gb/transfers/gb_tr_boxes.h"
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
		Section section = Section::None;
		QString label;
		Ui::FilterIcon icon = Ui::FilterIcon::All;
	};
	const auto entries = std::vector<Entry>{
		{ Section::Home, Strings::Home(), Ui::FilterIcon::Home },
		{ Section::Feed, Strings::Feed(), Ui::FilterIcon::Like },
		{ Section::None, Strings::Chats(), Ui::FilterIcon::All },
		{ Section::Transfers, Strings::Transfers(), Ui::FilterIcon::Airplane },
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

		const auto section = entry.section;
		button->setClickedCallback([=] {
			if (section == Section::None) {
				controller->clearSectionStack();
			} else {
				controller->showSection(
					std::make_shared<SectionMemento>(section));
			}
		});
		ShownSectionValue(
		) | rpl::on_next([=](Section shown) {
			button->setActive(shown == section);
		}, button->lifetime());
	}
	Transfers::WatchIncoming(controller);

	layout->show();
	return layout;
}

} // namespace GB
