/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/feed/gb_feed_widget.h"

#include "gb/feed/gb_feed_api.h"
#include "gb/feed/gb_feed_boxes.h"
#include "gb/feed/gb_feed_drive.h"
#include "gb/feed/gb_feed_posts.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "main/main_session.h"
#include "ui/painter.h"
#include "ui/userpic_view.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "window/window_session_controller.h"
#include "styles/style_gb.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtGui/QCursor>

namespace GB::Feed {
namespace {

struct TabInfo {
	QString title;
	QString path;
	QString empty;
};

const std::vector<TabInfo> &Tabs() {
	static const auto result = std::vector<TabInfo>{
		{
			u"For you"_q,
			u"/v1/feed?tab=all"_q,
			u"No posts yet.\nPress \"New post\" to ask for a file you need "
				"or share one you have."_q,
		},
		{
			u"Following"_q,
			u"/v1/feed?tab=following"_q,
			u"Posts from friends, people you follow and your communities "
				"appear here."_q,
		},
		{
			u"Need"_q,
			u"/v1/feed?tab=need"_q,
			u"Nobody is looking for a file right now."_q,
		},
		{
			u"Have"_q,
			u"/v1/feed?tab=have"_q,
			u"Nobody has shared a file yet."_q,
		},
		{
			u"Trending"_q,
			u"/v1/feed?tab=trending"_q,
			u"Nothing is trending this week."_q,
		},
	};
	return result;
}

// The card at the top of the Feed: your picture, "What do you want to
// share?" and quick buttons for the post kinds, like Facebook's composer.
class ComposerCard final : public Ui::RpWidget {
public:
	ComposerCard(
		QWidget *parent,
		not_null<Window::SessionController*> controller)
	: RpWidget(parent)
	, _controller(controller)
	, _field(this) {
		_field->setCursor(style::cur_text);
		_field->paintRequest() | rpl::on_next([=] {
			auto p = QPainter(_field.data());
			auto hq = PainterHighQualityEnabler(p);
			const auto r = _field->rect();
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(r, r.height() / 2., r.height() / 2.);
			p.setFont(st::normalFont);
			p.setPen(st::windowSubTextFg);
			const auto user = _controller->session().user();
			p.drawText(
				r.marginsRemoved({ st::gbFeedWriteSkip, 0, 0, 0 }),
				Qt::AlignLeft | Qt::AlignVCenter,
				u"What do you want to share, %1?"_q.arg(user->firstName));
		}, _field->lifetime());
		_field->setClickedCallback([=] { ShowComposeBox(_controller); });

		const auto add = [&](
				const style::icon &icon,
				const QString &text,
				const QString &kind) {
			const auto button = CreateActionButton(this, &icon, text);
			button->setClickedCallback([=] {
				ShowComposeBox(_controller, { .kind = kind });
			});
			_buttons.push_back(button);
		};
		add(st::menuIconFile, u"Share a file"_q, u"file"_q);
		add(st::menuIconSearch, u"Need a file"_q, u"need"_q);
		add(st::menuIconPhoto, u"Photo"_q, QString());
		add(st::menuIconCreatePoll, u"Poll"_q, u"poll"_q);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto &pad = st::gbFeedCardPadding;
		const auto size = st::gbFeedComposerHeight;
		const auto fieldLeft = pad.left() + size + st::gbFeedComposerSkip;
		_field->setGeometry(
			fieldLeft,
			pad.top(),
			std::max(newWidth - fieldLeft - pad.right(), 1),
			size);
		_lineTop = pad.top() + size + st::gbFeedComposerSkip;
		const auto top = _lineTop + st::gbFeedSkip / 2;
		const auto inner = newWidth - pad.left() / 2 - pad.right() / 2;
		const auto count = int(_buttons.size());
		for (auto i = 0; i != count; ++i) {
			const auto left = pad.left() / 2 + inner * i / count;
			const auto right = pad.left() / 2 + inner * (i + 1) / count;
			_buttons[i]->setGeometry(
				left,
				top,
				right - left,
				st::gbFeedActionsHeight);
		}
		return top + st::gbFeedActionsHeight + st::gbFeedSkip / 2;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::windowBg);
		p.drawRoundedRect(rect(), st::gbFeedCardRadius, st::gbFeedCardRadius);
		const auto &pad = st::gbFeedCardPadding;
		_controller->session().user()->paintUserpicLeft(
			p,
			_userpic,
			pad.left(),
			pad.top(),
			width(),
			st::gbFeedComposerHeight);
		p.fillRect(
			pad.left(),
			_lineTop,
			width() - pad.left() - pad.right(),
			st::lineWidth,
			st::shadowFg);
	}

private:
	const not_null<Window::SessionController*> _controller;
	object_ptr<Ui::AbstractButton> _field;
	std::vector<not_null<Ui::AbstractButton*>> _buttons;
	Ui::PeerUserpicView _userpic;
	int _lineTop = 0;

};

} // namespace

Widget::Widget(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: RpWidget(parent)
, _controller(controller)
, _compose(this, rpl::single(u"New post"_q), st::defaultActiveButton)
, _search(
	this,
	st::defaultInputField,
	rpl::single(u"Search people, posts, files"_q),
	QString())
, _notifications(
	this,
	Api::Instance().unreadValue() | rpl::map([](const Unread &unread) {
		return unread.notifications
			? (u"Notifications ("_q
				+ QString::number(unread.notifications)
				+ ')')
			: u"Notifications"_q;
	}),
	st::defaultLightButton)
, _menuToggle(this, st::menuToggle)
, _tabs(this, st::defaultTabsSlider)
, _scroll(this)
, _signInAbout(
	this,
	u"The Feed is GB's social network: ask for files you need, share "
		"files you have, follow people and join communities.\n\nSign in "
		"with your Telegram account. GB sends a one-time code to "
		"@GnBfeedBot from your account; no password is needed."_q,
	st::gbDlEmpty)
, _signIn(
	this,
	rpl::single(u"Sign in with Telegram"_q),
	st::defaultActiveButton) {
	_compose->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	_notifications->setTextTransform(
		Ui::RoundButtonTextTransform::NoTransform);
	_signIn->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);

	_compose->setClickedCallback([=] { ShowComposeBox(_controller); });
	_notifications->setClickedCallback([=] {
		ShowNotificationsBox(_controller);
	});
	_menuToggle->setClickedCallback([=] { showMainMenu(); });
	_signIn->setClickedCallback([=] { signIn(); });
	_search->submits() | rpl::on_next([=] {
		const auto query = _search->getLastText().trimmed();
		if (!query.isEmpty()) {
			ShowSearchBox(_controller, query);
		}
	}, _search->lifetime());
	_notifications->widthValue() | rpl::on_next([=] {
		resizeEvent(nullptr);
	}, _notifications->lifetime());

	auto titles = std::vector<QString>();
	for (const auto &tab : Tabs()) {
		titles.push_back(tab.title);
	}
	_tabs->setSections(titles);
	_tabs->setActiveSectionFast(0);
	_tabs->sectionActivated() | rpl::on_next([=](int index) {
		showTab(index);
	}, _tabs->lifetime());

	auto &api = Api::Instance();
	api.setAccount(&controller->session());
	api.signedInValue() | rpl::on_next([=] {
		updateSignedIn();
	}, lifetime());
	api.changes() | rpl::on_next([=] {
		if (_list) {
			_list->reload();
		}
	}, lifetime());
	api.refreshUnread();

	// No extra clicks: signing in to the Feed only needs the Telegram
	// account, so it starts by itself the first time the Feed is shown.
	if (!api.signedIn()) {
		crl::on_main(this, [=] {
			signIn();
		});
	}
}

void Widget::setInnerFocus() {
	setFocus();
}

void Widget::signIn() {
	if (_signingIn) {
		return;
	}
	_signingIn = true;
	_controller->showToast(u"Signing in through @GnBfeedBot..."_q);
	const auto weak = QPointer<Widget>(this);
	Api::Instance().signIn(_controller, [=] {
		if (const auto strong = weak.data()) {
			strong->_signingIn = false;
			strong->_controller->showToast(u"Signed in to GB Feed."_q);
		}
	}, [=](const QString &error) {
		if (const auto strong = weak.data()) {
			strong->_signingIn = false;
			strong->_controller->showToast(ErrorText(error));
		}
	});
}

void Widget::updateSignedIn() {
	const auto signedIn = Api::Instance().signedIn();
	_compose->setVisible(false); // The composer card replaces it.
	_search->setVisible(signedIn);
	_notifications->setVisible(signedIn);
	_tabs->setVisible(signedIn);
	_scroll->setVisible(signedIn);
	_signInAbout->setVisible(!signedIn);
	_signIn->setVisible(!signedIn);
	if (signedIn) {
		SyncLibraries(_controller);
		showTab(_tab);
	} else {
		_list = nullptr;
		_scroll->setOwnedWidget(object_ptr<Ui::RpWidget>(this));
	}
	resizeEvent(nullptr);
}

void Widget::showTab(int index) {
	_tab = index;
	const auto &tab = Tabs()[index];
	_list = _scroll->setOwnedWidget(object_ptr<PostList>(
		this,
		_controller,
		tab.path,
		tab.empty));
	_list->setTop(object_ptr<ComposerCard>(_list.data(), _controller));
	_scroll->scrollToY(0);
	resizeEvent(nullptr);
}

void Widget::showMainMenu() {
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	const auto menu = _menu.get();
	const auto controller = _controller;
	auto &api = Api::Instance();
	if (!api.signedIn()) {
		menu->addAction(u"Sign in with Telegram"_q, [=] {
			signIn();
		}, &st::menuIconProfile);
		menu->popup(QCursor::pos());
		return;
	}
	const auto friendRequests = api.unread().friendRequests;
	const auto approved = api.unread().approvedRequests;
	menu->addAction(u"My profile"_q, [=] {
		ShowProfileBox(controller, Api::Instance().myId());
	}, &st::menuIconProfile);
	menu->addAction(friendRequests
		? u"Friend requests (%1)"_q.arg(friendRequests)
		: u"Friend requests"_q, [=] {
		ShowFriendRequestsBox(controller);
	}, &st::menuIconInvite);
	menu->addAction(approved
		? u"File requests (%1 ready)"_q.arg(approved)
		: u"File requests"_q, [=] {
		ShowRequestsBox(controller);
	}, &st::menuIconDownload);
	menu->addAction(u"Communities"_q, [=] {
		ShowCommunitiesBox(controller);
	}, &st::menuIconGroups);
	menu->addAction(u"People you may know"_q, [=] {
		ShowSuggestionsBox(controller);
	}, &st::menuIconAddToFolder);
	menu->addAction(u"Saved posts"_q, [=] {
		ShowPostsBox(
			controller,
			u"Saved"_q,
			u"/v1/me/saved"_q,
			u"Nothing saved yet."_q);
	}, &st::menuIconFave);
	menu->addAction(u"Refresh"_q, [=] {
		if (_list) {
			_list->reload();
		}
		Api::Instance().refreshUnread();
	}, &st::menuIconRestore);
	menu->addSeparator();
	menu->addAction(u"Sign out of GB Feed"_q, [=] {
		Api::Instance().signOut();
	}, &st::menuIconLeave);
	menu->popup(QCursor::pos());
}

void Widget::resizeEvent(QResizeEvent *e) {
	const auto &padding = st::gbFeedPadding;
	const auto available = width() - padding.left() - padding.right();
	const auto contentWidth = std::max(
		std::min(available, st::gbFeedMaxWidth),
		0);
	const auto left = (width() - contentWidth) / 2;

	if (!Api::Instance().signedIn()) {
		_signInAbout->resizeToWidth(std::min(contentWidth, st::gbPageMaxWidth));
		_signInAbout->moveToLeft(
			(width() - _signInAbout->width()) / 2,
			st::gbFeedSignInTop);
		_signIn->moveToLeft(
			(width() - _signIn->width()) / 2,
			st::gbFeedSignInTop + _signInAbout->height() + st::gbFeedSkip * 3);
		return;
	}
	auto top = padding.top();
	const auto rowHeight = std::max(
		_compose->height(),
		_menuToggle->height());
	_compose->moveToLeft(left, top + (rowHeight - _compose->height()) / 2);
	_menuToggle->moveToLeft(
		left + contentWidth - _menuToggle->width(),
		top + (rowHeight - _menuToggle->height()) / 2);
	_notifications->moveToLeft(
		left + contentWidth - _menuToggle->width() - st::gbDlFieldSkip
			- _notifications->width(),
		top + (rowHeight - _notifications->height()) / 2);
	const auto searchLeft = left;
	const auto searchWidth = std::max(
		_notifications->x() - st::gbDlFieldSkip - searchLeft,
		0);
	_search->resize(searchWidth, _search->height());
	_search->moveToLeft(
		searchLeft,
		top + (rowHeight - _search->height()) / 2);
	top += rowHeight + st::gbDlTopSkip;

	_tabs->resizeToWidth(contentWidth);
	_tabs->moveToLeft(left, top);
	top += _tabs->height() + st::gbDlTabsSkip;

	_scroll->setGeometry(left, top, contentWidth, std::max(height() - top, 0));
	if (_list) {
		_list->resizeToWidth(contentWidth);
	}
}

void Widget::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	// Gray page, white cards (Facebook's layout in Telegram's colors).
	p.fillRect(e->rect(), st::windowBgOver);
}

} // namespace GB::Feed
