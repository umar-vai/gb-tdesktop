/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/feed/gb_feed_posts.h"

#include "core/file_utilities.h"
#include "gb/feed/gb_feed_api.h"
#include "gb/feed/gb_feed_boxes.h"
#include "gb/transfers/gb_tr_boxes.h"
#include "layout/layout_document_generic_preview.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/empty_userpic.h"
#include "ui/painter.h"
#include "ui/text/text_entity.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_gb.h"
#include "styles/style_info.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtGui/QClipboard>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>

namespace GB::Feed {
namespace {

const auto kReactions = std::vector<QString>{
	u"👍"_q, u"❤️"_q, u"😂"_q, u"😮"_q, u"😢"_q, u"😡"_q, u"🔥"_q, u"🙏"_q,
};

[[nodiscard]] int64 IdOf(const QJsonValue &value) {
	return value.toVariant().toLongLong();
}

[[nodiscard]] QString PrivacyLabel(const QString &privacy) {
	if (privacy == u"friends"_q) {
		return u"Friends"_q;
	} else if (privacy == u"only_me"_q) {
		return u"Only me"_q;
	} else if (privacy == u"community"_q) {
		return QString();
	}
	return u"Public"_q;
}

// A clickable row: userpic, name and a subtitle line.
class Header final : public Ui::AbstractButton {
public:
	Header(QWidget *parent, const QJsonObject &user, const QString &sub)
	: AbstractButton(parent)
	, _userpic(this, user, st::gbFeedUserpic)
	, _name(this, user.value(u"name"_q).toString(), st::gbFeedName)
	, _sub(this, sub, st::gbFeedSub) {
		_userpic->setAttribute(Qt::WA_TransparentForMouseEvents);
		_name->setAttribute(Qt::WA_TransparentForMouseEvents);
		_sub->setAttribute(Qt::WA_TransparentForMouseEvents);
		resize(width(), st::gbFeedHeaderHeight);
	}

	void setRightSkip(int skip) {
		_rightSkip = skip;
		resizeToWidth(width());
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto available = std::max(
			newWidth - st::gbFeedNameLeft - _rightSkip,
			1);
		_userpic->moveToLeft(0, (st::gbFeedHeaderHeight - st::gbFeedUserpic) / 2);
		_name->resizeToNaturalWidth(available);
		_name->moveToLeft(st::gbFeedNameLeft, st::gbFeedNameTop);
		_sub->resizeToNaturalWidth(available);
		_sub->moveToLeft(st::gbFeedNameLeft, st::gbFeedSubTop);
		return st::gbFeedHeaderHeight;
	}

private:
	object_ptr<Userpic> _userpic;
	object_ptr<Ui::FlatLabel> _name;
	object_ptr<Ui::FlatLabel> _sub;
	int _rightSkip = 0;

};

// The "file" block of need/have posts, like a Telegram document row.
class FileBlock final : public Ui::RpWidget {
public:
	FileBlock(QWidget *parent, const QJsonObject &file, const QString &kind)
	: RpWidget(parent)
	, _name(file.value(u"name"_q).toString())
	, _kind(kind) {
		auto parts = QStringList();
		const auto size = IdOf(file.value(u"size"_q));
		if (size > 0) {
			parts.push_back(FormatBytes(size));
		}
		const auto category = file.value(u"category"_q).toString();
		if (!category.isEmpty()) {
			parts.push_back(category);
		}
		_info = parts.join(u" • "_q);
		resize(width(), st::gbFeedFileHeight);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return st::gbFeedFileHeight;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		const auto preview = Layout::DocumentGenericPreview::Create(_name);
		const auto size = st::gbFeedFileThumb;
		const auto top = (height() - size) / 2;
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(preview.color);
			p.drawRoundedRect(
				QRect(0, top, size, size),
				st::roundRadiusSmall,
				st::roundRadiusSmall);
		}
		auto ext = QFileInfo(_name).suffix().left(4).toUpper();
		if (ext.isEmpty()) {
			ext = u"FILE"_q;
		}
		p.setFont(st::gbDlExtFont);
		p.setPen(st::windowFgActive);
		p.drawText(QRect(0, top, size, size), Qt::AlignCenter, ext);

		const auto left = size + st::gbFeedSkip + 4;
		const auto available = width() - left;
		const auto badge = KindLabel(_kind);
		auto badgeWidth = 0;
		if (!badge.isEmpty()) {
			const auto &font = st::gbFeedBadgeFont;
			const auto &pad = st::gbFeedBadgePadding;
			badgeWidth = font->width(badge) + pad.left() + pad.right();
			const auto rect = QRect(
				left,
				top + 2,
				badgeWidth,
				font->height + pad.top() + pad.bottom());
			{
				auto hq = PainterHighQualityEnabler(p);
				p.setPen(Qt::NoPen);
				p.setBrush((_kind == u"need"_q)
					? st::attentionButtonFg
					: st::activeButtonBg);
				p.drawRoundedRect(rect, rect.height() / 2., rect.height() / 2.);
			}
			p.setFont(font);
			p.setPen(st::activeButtonFg);
			p.drawText(rect, Qt::AlignCenter, badge);
			badgeWidth += st::gbFeedSkip;
		}
		p.setFont(st::semiboldFont);
		p.setPen(st::windowFg);
		p.drawText(
			left + badgeWidth,
			top + 2 + st::semiboldFont->ascent + 2,
			st::semiboldFont->elided(_name, std::max(available - badgeWidth, 0)));
		p.setFont(st::normalFont);
		p.setPen(st::windowSubTextFg);
		p.drawText(
			left,
			top + size - st::normalFont->descent - 4,
			st::normalFont->elided(_info, std::max(available, 0)));
	}

private:
	const QString _name;
	const QString _kind;
	QString _info;

};

// A post picture, scaled to the card width.
class Picture final : public Ui::RpWidget {
public:
	Picture(QWidget *parent, const QString &url) : RpWidget(parent) {
		resize(width(), 0);
		const auto weak = QPointer<Picture>(this);
		Api::Instance().loadImage(url, [=](QImage image) {
			if (const auto strong = weak.data()) {
				strong->_image = std::move(image);
				strong->resizeToWidth(strong->width());
				strong->update();
			}
		});
		setCursor(style::cur_pointer);
		_url = url;
	}

protected:
	int resizeGetHeight(int newWidth) override {
		if (_image.isNull() || newWidth <= 0) {
			return 0;
		}
		const auto ratio = _image.height() / float64(_image.width());
		return std::min(int(newWidth * ratio), int(st::gbFeedImageMaxHeight));
	}

	void paintEvent(QPaintEvent *e) override {
		if (_image.isNull()) {
			return;
		}
		auto p = QPainter(this);
		auto hq = PainterHighQualityEnabler(p);
		const auto scaled = _image.size().scaled(size(), Qt::KeepAspectRatio);
		const auto rect = QRect(
			QPoint((width() - scaled.width()) / 2, 0),
			scaled);
		auto path = QPainterPath();
		path.addRoundedRect(rect, st::roundRadiusLarge, st::roundRadiusLarge);
		p.setClipPath(path);
		p.drawImage(rect, _image);
	}

	void mouseReleaseEvent(QMouseEvent *e) override {
		File::OpenUrl(_url);
	}

private:
	QImage _image;
	QString _url;

};

} // namespace

QString FormatBytes(int64 size) {
	if (size < 1024) {
		return QString::number(size) + u" B"_q;
	}
	const auto units = std::array{ u"KB"_q, u"MB"_q, u"GB"_q, u"TB"_q };
	auto value = size / 1024.;
	auto index = 0;
	while (value >= 1024. && index + 1 < int(units.size())) {
		value /= 1024.;
		++index;
	}
	return QString::number(value, 'f', (value < 10.) ? 1 : 0)
		+ ' '
		+ units[index];
}

QString KindLabel(const QString &kind) {
	if (kind == u"need"_q) {
		return u"NEED"_q;
	} else if (kind == u"have"_q) {
		return u"HAVE"_q;
	}
	return QString();
}

Userpic::Userpic(QWidget *parent, const QJsonObject &user, int size)
: RpWidget(parent)
, _id(uint64(IdOf(user.value(u"id"_q))))
, _name(user.value(u"name"_q).toString())
, _size(size) {
	resize(size, size);
	const auto url = user.value(u"avatar"_q).toString();
	if (!url.isEmpty()) {
		const auto weak = QPointer<Userpic>(this);
		Api::Instance().loadImage(url, [=](QImage image) {
			if (const auto strong = weak.data()) {
				strong->_image = std::move(image);
				strong->update();
			}
		});
	}
}

void Userpic::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	if (!_image.isNull()) {
		auto hq = PainterHighQualityEnabler(p);
		auto path = QPainterPath();
		path.addEllipse(0, 0, _size, _size);
		p.setClipPath(path);
		const auto side = std::min(_image.width(), _image.height());
		const auto source = QRect(
			(_image.width() - side) / 2,
			(_image.height() - side) / 2,
			side,
			side);
		p.drawImage(QRect(0, 0, _size, _size), _image, source);
		return;
	}
	const auto userpic = Ui::EmptyUserpic(
		Ui::EmptyUserpic::UserpicColor(Ui::EmptyUserpic::ColorIndex(_id)),
		_name);
	userpic.paintCircle(p, 0, 0, width(), _size);
}

PostCard::PostCard(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	const QJsonObject &post)
: VerticalLayout(parent)
, _controller(controller)
, _post(post) {
	fill();
}

int64 PostCard::postId() const {
	return IdOf(_post.value(u"id"_q));
}

void PostCard::setPost(const QJsonObject &post) {
	_post = post;
	fill();
}

void PostCard::fill() {
	clear();
	const auto &pad = st::gbFeedCardPadding;
	const auto margin = QMargins(pad.left(), 0, pad.right(), 0);
	Ui::AddSkip(this, pad.top());
	addHeader();

	const auto text = _post.value(u"text"_q).toString();
	if (!text.isEmpty()) {
		const auto label = add(
			object_ptr<Ui::FlatLabel>(this, QString(), st::gbFeedText),
			margin + QMargins(0, st::gbFeedSkip, 0, 0));
		label->setMarkedText(TextUtilities::ParseEntities(text, TextParseLinks));
		label->setSelectable(true);
	}
	const auto file = _post.value(u"file"_q).toObject();
	if (!file.isEmpty()) {
		addFile(file);
	}
	auto tags = QStringList();
	for (const auto tag : _post.value(u"tags"_q).toArray()) {
		tags.push_back('#' + tag.toString());
	}
	if (!tags.isEmpty()) {
		add(
			object_ptr<Ui::FlatLabel>(this, tags.join(' '), st::gbFeedSub),
			margin + QMargins(0, st::gbFeedSkip / 2, 0, 0));
	}
	const auto link = _post.value(u"link_url"_q).toString();
	if (!link.isEmpty()) {
		const auto button = add(
			object_ptr<Ui::LinkButton>(this, link),
			margin + QMargins(0, st::gbFeedSkip / 2, 0, 0));
		button->setClickedCallback([=] { File::OpenUrl(link); });
	}
	const auto media = _post.value(u"media"_q).toArray();
	if (!media.isEmpty()) {
		addImage(media.first().toString());
	}
	const auto poll = _post.value(u"poll"_q).toObject();
	if (!poll.isEmpty()) {
		addPoll(poll);
	}
	const auto shared = _post.value(u"shared"_q).toObject();
	if (!shared.isEmpty()) {
		addShared(shared);
	}
	addActions();
	Ui::AddSkip(this, pad.bottom());
	Ui::AddDivider(this);
	resizeToWidth(width());
}

void PostCard::addHeader() {
	const auto &pad = st::gbFeedCardPadding;
	const auto author = _post.value(u"author"_q).toObject();
	auto sub = QStringList();
	const auto community = _post.value(u"community"_q).toObject();
	if (!community.isEmpty()) {
		sub.push_back(u"in "_q + community.value(u"name"_q).toString());
	}
	sub.push_back(RelativeTime(IdOf(_post.value(u"created_at"_q))));
	const auto privacy = PrivacyLabel(_post.value(u"privacy"_q).toString());
	if (!privacy.isEmpty()) {
		sub.push_back(privacy);
	}
	if (!_post.value(u"edited_at"_q).isNull()) {
		sub.push_back(u"edited"_q);
	}
	const auto header = add(
		object_ptr<Header>(this, author, sub.join(u" • "_q)),
		QMargins(pad.left(), 0, pad.right(), 0));
	const auto userId = uint64(IdOf(author.value(u"id"_q)));
	const auto controller = _controller;
	header->setClickedCallback([=] {
		ShowProfileBox(controller, userId);
	});
	const auto toggle = Ui::CreateChild<Ui::IconButton>(
		header,
		st::infoTopBarMenu);
	header->setRightSkip(toggle->width());
	header->widthValue() | rpl::on_next([=](int width) {
		toggle->moveToRight(0, (header->height() - toggle->height()) / 2, width);
	}, toggle->lifetime());
	toggle->setClickedCallback([=] { showMenu(); });
	if (!community.isEmpty()) {
		const auto communityId = IdOf(community.value(u"id"_q));
		const auto name = community.value(u"name"_q).toString();
		const auto open = add(
			object_ptr<Ui::LinkButton>(this, u"Open "_q + name),
			QMargins(pad.left() + st::gbFeedNameLeft, 0, pad.right(), 0));
		open->setClickedCallback([=] {
			ShowCommunityBox(controller, communityId);
		});
	}
}

void PostCard::addFile(const QJsonObject &file) {
	const auto &pad = st::gbFeedCardPadding;
	add(
		object_ptr<FileBlock>(this, file, _post.value(u"kind"_q).toString()),
		QMargins(pad.left(), st::gbFeedSkip, pad.right(), 0));
}

void PostCard::addImage(const QString &url) {
	const auto &pad = st::gbFeedCardPadding;
	add(
		object_ptr<Picture>(this, url),
		QMargins(pad.left(), st::gbFeedSkip, pad.right(), 0));
}

void PostCard::addPoll(const QJsonObject &poll) {
	const auto options = poll.value(u"options"_q).toArray();
	const auto myVote = poll.value(u"my_vote"_q);
	auto total = 0;
	for (const auto option : options) {
		total += option.toObject().value(u"votes"_q).toInt();
	}
	const auto voted = !myVote.isNull();
	Ui::AddSkip(this, st::gbFeedSkip / 2);
	for (const auto value : options) {
		const auto option = value.toObject();
		const auto index = option.value(u"idx"_q).toInt();
		const auto votes = option.value(u"votes"_q).toInt();
		auto label = QString();
		if (voted) {
			const auto percent = total ? (votes * 100 / total) : 0;
			label = QString::number(percent) + u"% ("_q
				+ QString::number(votes) + ')';
		}
		const auto mine = voted && (myVote.toInt() == index);
		const auto button = ::Settings::AddButtonWithLabel(
			this,
			rpl::single((mine ? u"✓ "_q : QString())
				+ option.value(u"text"_q).toString()),
			rpl::single(label),
			st::settingsButtonNoIcon);
		const auto id = postId();
		button->setClickedCallback([=] {
			auto body = QJsonObject();
			body.insert(u"option"_q, index);
			const auto weak = QPointer<PostCard>(this);
			Api::Instance().send(
				u"POST"_q,
				u"/v1/posts/%1/vote"_q.arg(id),
				body,
				[=](const QJsonObject &r) {
					if (const auto strong = weak.data()) {
						strong->apply(r);
					}
				},
				[=](const QString &error) {
					_controller->showToast(ErrorText(error));
				});
		});
	}
	const auto &pad = st::gbFeedCardPadding;
	add(
		object_ptr<Ui::FlatLabel>(
			this,
			QString::number(total) + (total == 1 ? u" vote"_q : u" votes"_q),
			st::gbFeedSub),
		QMargins(pad.left(), st::gbFeedSkip / 2, pad.right(), 0));
}

void PostCard::addShared(const QJsonObject &shared) {
	const auto &pad = st::gbFeedCardPadding;
	const auto margin = QMargins(
		pad.left() + st::gbFeedSkip * 2,
		st::gbFeedSkip,
		pad.right(),
		0);
	if (shared.value(u"unavailable"_q).toBool()) {
		add(
			object_ptr<Ui::FlatLabel>(
				this,
				u"↪ This post is no longer available."_q,
				st::gbFeedSub),
			margin);
		return;
	}
	const auto author = shared.value(u"author"_q).toObject();
	auto text = u"↪ "_q + author.value(u"name"_q).toString();
	const auto file = shared.value(u"file"_q).toObject();
	if (!file.isEmpty()) {
		text += u" • "_q + KindLabel(shared.value(u"kind"_q).toString())
			+ ' ' + file.value(u"name"_q).toString();
	}
	const auto body = shared.value(u"text"_q).toString();
	if (!body.isEmpty()) {
		text += u"\n"_q + body.left(300);
	}
	const auto button = add(
		object_ptr<Ui::LinkButton>(this, u"Open the original post"_q),
		margin);
	insert(
		count() - 1,
		object_ptr<Ui::FlatLabel>(this, text, st::gbFeedSub),
		margin);
	const auto id = IdOf(shared.value(u"id"_q));
	const auto controller = _controller;
	button->setClickedCallback([=] { ShowPostBox(controller, id); });
}

void PostCard::addActions() {
	const auto &pad = st::gbFeedCardPadding;
	const auto counts = _post.value(u"counts"_q).toObject();
	const auto reactions = counts.value(u"reactions"_q).toInt();
	const auto comments = counts.value(u"comments"_q).toInt();
	const auto shares = counts.value(u"shares"_q).toInt();
	const auto requests = counts.value(u"requests"_q).toInt();
	const auto kind = _post.value(u"kind"_q).toString();
	const auto mine = _post.value(u"mine"_q).toBool();
	const auto id = postId();
	const auto controller = _controller;

	auto summary = QStringList();
	if (reactions) {
		summary.push_back(QString::number(reactions)
			+ (reactions == 1 ? u" reaction"_q : u" reactions"_q));
	}
	if (comments) {
		summary.push_back(QString::number(comments)
			+ (comments == 1 ? u" comment"_q : u" comments"_q));
	}
	if (shares) {
		summary.push_back(QString::number(shares)
			+ (shares == 1 ? u" share"_q : u" shares"_q));
	}
	if (requests && kind == u"have"_q) {
		summary.push_back(QString::number(requests)
			+ (requests == 1 ? u" request"_q : u" requests"_q));
	}
	if (!summary.isEmpty()) {
		const auto button = add(
			object_ptr<Ui::LinkButton>(
				this,
				summary.join(u" • "_q),
				st::defaultLinkButton),
			QMargins(pad.left(), st::gbFeedSkip, pad.right(), 0));
		button->setColorOverride(st::windowSubTextFg->c);
		button->setClickedCallback([=] {
			if (reactions) {
				ShowReactionsBox(controller, id);
			} else {
				ShowCommentsBox(controller, _post);
			}
		});
	}

	const auto row = add(
		object_ptr<Ui::RpWidget>(this),
		QMargins(pad.left(), st::gbFeedSkip / 2, pad.right(), 0));
	row->resize(row->width(), st::gbFeedActionsHeight);
	auto buttons = std::vector<Ui::LinkButton*>();
	const auto addButton = [&](const QString &text, Fn<void()> callback) {
		const auto button = Ui::CreateChild<Ui::LinkButton>(row, text);
		button->setClickedCallback(std::move(callback));
		buttons.push_back(button);
		return button;
	};
	const auto myReaction = _post.value(u"my_reaction"_q).toString();
	addButton(
		myReaction.isEmpty() ? u"👍 Like"_q : (myReaction + u" Reacted"_q),
		[=] { react(myReaction.isEmpty() ? u"👍"_q : QString()); });
	addButton(u"😀 React"_q, [=] { showReactions(QCursor::pos()); });
	addButton(u"💬 Comment"_q, [=] { ShowCommentsBox(controller, _post); });
	addButton(u"↪ Share"_q, [=] {
		ShowComposeBox(controller, { .shared = _post });
	});
	if (kind == u"have"_q) {
		if (mine) {
			addButton(u"📥 Requests"_q, [=] { ShowRequestsBox(controller); });
		} else {
			const auto request = _post.value(u"my_request"_q).toObject();
			const auto status = request.value(u"status"_q).toString();
			if (status == u"approved"_q) {
				const auto invite = request.value(u"invite"_q).toString();
				addButton(u"⬇ Receive file"_q, [=] {
					Transfers::Receive(controller, invite);
				});
			} else if (status == u"pending"_q) {
				addButton(u"⏳ Requested"_q, [=] {
					controller->showToast(
						u"Waiting for the owner to approve your request."_q);
				});
			} else {
				addButton(u"📥 Request file"_q, [=] { requestFile(); });
			}
		}
	}
	row->widthValue() | rpl::on_next([=](int width) {
		auto left = 0;
		for (const auto button : buttons) {
			button->moveToLeft(left, (row->height() - button->height()) / 2, width);
			left += button->width() + st::gbFeedActionSkip;
		}
	}, row->lifetime());
}

void PostCard::apply(const QJsonObject &response) {
	const auto post = response.value(u"post"_q).toObject();
	if (!post.isEmpty()) {
		setPost(post);
	}
}

void PostCard::react(const QString &emoji) {
	const auto id = postId();
	auto body = QJsonObject();
	if (!emoji.isEmpty()) {
		body.insert(u"emoji"_q, emoji);
	}
	const auto weak = QPointer<PostCard>(this);
	const auto controller = _controller;
	Api::Instance().send(
		emoji.isEmpty() ? u"DELETE"_q : u"PUT"_q,
		u"/v1/posts/%1/reaction"_q.arg(id),
		body,
		[=](const QJsonObject &r) {
			if (const auto strong = weak.data()) {
				strong->apply(r);
			}
		},
		[=](const QString &error) { controller->showToast(ErrorText(error)); });
}

void PostCard::showReactions(QPoint globalPosition) {
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	for (const auto &emoji : kReactions) {
		_menu->addAction(emoji, [=] { react(emoji); });
	}
	if (!_post.value(u"my_reaction"_q).toString().isEmpty()) {
		_menu->addSeparator();
		_menu->addAction(u"Remove reaction"_q, [=] { react(QString()); });
	}
	_menu->popup(globalPosition);
}

void PostCard::requestFile() {
	const auto id = postId();
	const auto weak = QPointer<PostCard>(this);
	const auto controller = _controller;
	const auto name = _post.value(u"file"_q).toObject().value(
		u"name"_q).toString();
	controller->show(Ui::MakeConfirmBox({
		.text = u"Ask for \"%1\"? The owner gets your request and can send "
			"the file straight to your PC."_q.arg(name),
		.confirmed = [=](Fn<void()> close) {
			Api::Instance().send(
				u"POST"_q,
				u"/v1/posts/%1/request"_q.arg(id),
				{},
				[=](const QJsonObject &) {
					controller->showToast(u"Request sent."_q);
					Api::Instance().get(
						u"/v1/posts/%1"_q.arg(id),
						[=](const QJsonObject &r) {
							if (const auto strong = weak.data()) {
								strong->apply(r);
							}
						});
				},
				[=](const QString &error) {
					controller->showToast(ErrorText(error));
				});
			close();
		},
		.confirmText = rpl::single(u"Request"_q),
	}));
}

void PostCard::showMenu() {
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	const auto menu = _menu.get();
	const auto controller = _controller;
	const auto id = postId();
	const auto post = _post;
	const auto saved = _post.value(u"saved"_q).toBool();
	const auto mine = _post.value(u"mine"_q).toBool();
	const auto weak = QPointer<PostCard>(this);
	menu->addAction(saved ? u"Remove from saved"_q : u"Save post"_q, [=] {
		Api::Instance().send(
			saved ? u"DELETE"_q : u"PUT"_q,
			u"/v1/posts/%1/save"_q.arg(id),
			{},
			[=](const QJsonObject &) {
				controller->showToast(saved ? u"Removed."_q : u"Saved."_q);
				if (const auto strong = weak.data()) {
					auto copy = strong->_post;
					copy.insert(u"saved"_q, !saved);
					strong->_post = copy;
				}
			},
			[=](const QString &error) {
				controller->showToast(ErrorText(error));
			});
	}, &st::menuIconFave);
	const auto text = _post.value(u"text"_q).toString();
	if (!text.isEmpty()) {
		menu->addAction(u"Copy text"_q, [=] {
			QGuiApplication::clipboard()->setText(text);
			controller->showToast(u"Copied."_q);
		}, &st::menuIconCopy);
	}
	const auto file = _post.value(u"file"_q).toObject();
	if (!file.isEmpty()) {
		const auto name = file.value(u"name"_q).toString();
		menu->addAction(u"Copy file name"_q, [=] {
			QGuiApplication::clipboard()->setText(name);
			controller->showToast(u"Copied."_q);
		}, &st::menuIconCopy);
	}
	menu->addAction(u"Comments"_q, [=] {
		ShowCommentsBox(controller, post);
	}, &st::menuIconReply);
	const auto author = _post.value(u"author"_q).toObject();
	if (!mine && !author.value(u"username"_q).toString().isEmpty()) {
		menu->addAction(u"Message in Telegram"_q, [=] {
			OpenTelegramChat(controller, author);
		}, &st::menuIconChats);
	}
	if (mine) {
		menu->addAction(u"Edit"_q, [=] {
			ShowComposeBox(controller, { .edit = post });
		}, &st::menuIconEdit);
		menu->addAction(u"Delete"_q, [=] {
			controller->show(Ui::MakeConfirmBox({
				.text = u"Delete this post?"_q,
				.confirmed = [=](Fn<void()> close) {
					Api::Instance().send(
						u"DELETE"_q,
						u"/v1/posts/%1"_q.arg(id),
						{},
						[=](const QJsonObject &) {
							if (const auto strong = weak.data()) {
								strong->hide();
								strong->resize(strong->width(), 0);
							}
						},
						[=](const QString &error) {
							controller->showToast(ErrorText(error));
						});
					close();
				},
				.confirmText = rpl::single(u"Delete"_q),
				.confirmStyle = &st::attentionBoxButton,
			}));
		}, &st::menuIconDelete);
	} else {
		menu->addAction(u"Report"_q, [=] {
			ShowReportBox(controller, u"post"_q, id);
		}, &st::menuIconReport);
	}
	menu->popup(QCursor::pos());
}

PostList::PostList(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	const QString &path,
	const QString &emptyText)
: VerticalLayout(parent)
, _controller(controller)
, _path(path)
, _emptyText(emptyText) {
	_posts = add(object_ptr<Ui::VerticalLayout>(this));
	_status = add(
		object_ptr<Ui::FlatLabel>(this, QString(), st::gbDlEmpty),
		QMargins(st::gbFeedSkip, st::gbDlEmptyTop / 2, st::gbFeedSkip, 0));
	_more = add(
		object_ptr<Ui::LinkButton>(this, u"Show more"_q),
		QMargins(0, st::gbFeedSkip, 0, st::gbFeedSkip * 2),
		style::al_top);
	_more->setClickedCallback([=] { load(); });
	_more->hide();
	reload();
}

void PostList::reload() {
	++_generation;
	_loading = false;
	_cursor = QString();
	_posts->clear();
	load();
}

void PostList::load() {
	if (_loading) {
		return;
	}
	_loading = true;
	_more->hide();
	if (_posts->count() == 0) {
		_status->setText(u"Loading..."_q);
		_status->show();
	}
	const auto generation = _generation;
	auto path = _path;
	if (!_cursor.isEmpty()) {
		path += (path.contains('?') ? u"&cursor="_q : u"?cursor="_q) + _cursor;
	}
	const auto weak = QPointer<PostList>(this);
	Api::Instance().get(path, [=](const QJsonObject &r) {
		const auto strong = weak.data();
		if (!strong || strong->_generation != generation) {
			return;
		}
		strong->_loading = false;
		for (const auto value : r.value(u"posts"_q).toArray()) {
			strong->_posts->add(object_ptr<PostCard>(
				strong->_posts,
				strong->_controller,
				value.toObject()));
		}
		const auto next = r.value(u"next_cursor"_q);
		strong->_cursor = next.isNull()
			? QString()
			: QString::number(next.toVariant().toLongLong());
		strong->_more->setVisible(!strong->_cursor.isEmpty());
		strong->_status->setText(strong->_emptyText);
		strong->_status->setVisible(strong->_posts->count() == 0);
		strong->resizeToWidth(strong->width());
	}, [=](const QString &error) {
		const auto strong = weak.data();
		if (!strong || strong->_generation != generation) {
			return;
		}
		strong->_loading = false;
		strong->_status->setText(ErrorText(error));
		strong->_status->show();
		strong->_more->setVisible(!strong->_cursor.isEmpty());
	});
}

} // namespace GB::Feed
