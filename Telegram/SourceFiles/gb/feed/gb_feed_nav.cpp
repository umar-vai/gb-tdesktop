/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/feed/gb_feed_nav.h"

#include "gb/gb_section.h"
#include "gb/feed/gb_feed_api.h"
#include "gb/feed/gb_feed_boxes.h"
#include "gb/feed/gb_feed_drive.h"
#include "ui/wrap/slide_wrap.h"
#include "settings/settings_common.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_gb.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"

namespace GB::Feed {
namespace {

[[nodiscard]] rpl::producer<QString> CountLabel(
		Fn<int(const Unread &)> field) {
	return Api::Instance().unreadValue(
	) | rpl::map([=](const Unread &unread) {
		const auto count = field(unread);
		return count ? QString::number(count) : QString();
	});
}

} // namespace

NavPanel::NavPanel(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: RpWidget(parent)
, _scroll(this) {
	const auto content = _scroll->setOwnedWidget(
		object_ptr<Ui::VerticalLayout>(_scroll.data()));

	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			u"G&B Community"_q,
			st::gbNavTitle),
		st::gbNavTitlePadding);
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			u"Make Internet Free Again"_q,
			st::gbNavSlogan),
		st::gbNavSloganPadding);
	Ui::AddDivider(content);
	Ui::AddSkip(content);

	const auto add = [&](
			const QString &text,
			const style::icon &icon,
			Fn<void()> callback,
			rpl::producer<QString> label = rpl::single(QString())) {
		const auto button = ::Settings::AddButtonWithLabel(
			content,
			rpl::single(text),
			std::move(label),
			st::settingsButton,
			{ &icon });
		button->setClickedCallback(std::move(callback));
	};
	const auto showFeed = [=] {
		controller->showSection(
			std::make_shared<SectionMemento>(Section::Feed),
			Window::SectionShow(Window::SectionShow::Way::ClearStack));
	};
	const auto signedIn = [=] {
		if (Api::Instance().signedIn()) {
			return true;
		}
		showFeed(); // The Feed page has the sign-in panel.
		return false;
	};
	const auto guarded = [=](Fn<void()> callback) {
		return [=] {
			if (signedIn()) {
				callback();
			}
		};
	};

	add(u"News feed"_q, st::menuIconChats, showFeed);
	add(u"Notifications"_q, st::menuIconNotifications, guarded([=] {
		ShowNotificationsBox(controller);
	}), CountLabel([](const Unread &u) { return u.notifications; }));
	add(u"Friend requests"_q, st::menuIconInvite, guarded([=] {
		ShowFriendRequestsBox(controller);
	}), CountLabel([](const Unread &u) { return u.friendRequests; }));
	add(u"People you may know"_q, st::menuIconAddToFolder, guarded([=] {
		ShowSuggestionsBox(controller);
	}));
	add(u"Communities"_q, st::menuIconGroups, guarded([=] {
		ShowCommunitiesBox(controller);
	}));
	add(u"File requests"_q, st::menuIconDownload, guarded([=] {
		ShowRequestsBox(controller);
	}), CountLabel([](const Unread &u) { return u.approvedRequests; }));
	add(u"Saved posts"_q, st::menuIconFave, guarded([=] {
		ShowPostsBox(
			controller,
			u"Saved"_q,
			u"/v1/me/saved"_q,
			u"Nothing saved yet."_q);
	}));

	Ui::AddSkip(content);
	Ui::AddDivider(content);
	Ui::AddSkip(content);
	add(u"My profile"_q, st::menuIconProfile, guarded([=] {
		ShowProfileBox(controller, Api::Instance().myId());
	}));
	add(u"Search"_q, st::menuIconSearch, guarded([=] {
		ShowSearchBox(controller, QString());
	}));

	// Project admins: one click sets up a G&B Library channel.
	const auto adminWrap = content->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			content,
			object_ptr<Ui::VerticalLayout>(content)));
	adminWrap->toggle(false, anim::type::instant);
	const auto adminButton = ::Settings::AddButtonWithIcon(
		adminWrap->entity(),
		rpl::single(u"Create G&B Library (admin)"_q),
		st::settingsButton,
		{ &st::menuIconChannel });
	adminButton->setClickedCallback([=] { CreateLibrary(controller); });
	Api::Instance().signedInValue(
	) | rpl::filter(rpl::mappers::_1) | rpl::on_next([=] {
		const auto weak = QPointer<Ui::SlideWrap<Ui::VerticalLayout>>(adminWrap);
		Api::Instance().get(u"/v1/me"_q, [=](const QJsonObject &r) {
			const auto admin = r.value(u"user"_q).toObject().value(
				u"is_admin"_q).toBool();
			if (const auto strong = weak.data()) {
				strong->toggle(admin, anim::type::instant);
			}
		});
	}, adminWrap->lifetime());
	Ui::AddSkip(content);
}

void NavPanel::resizeEvent(QResizeEvent *e) {
	_scroll->setGeometry(rect());
	if (const auto inner = static_cast<Ui::RpWidget*>(_scroll->widget())) {
		inner->resizeToWidth(width());
	}
}

} // namespace GB::Feed
