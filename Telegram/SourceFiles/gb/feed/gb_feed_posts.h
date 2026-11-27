/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/unique_qptr.h"
#include "ui/rp_widget.h"
#include "ui/wrap/vertical_layout.h"

#include <QtCore/QJsonObject>

namespace Ui {
class PopupMenu;
class FlatLabel;
class LinkButton;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace GB::Feed {

// A round user picture: our avatar from R2, or Telegram-style initials.
class Userpic final : public Ui::RpWidget {
public:
	Userpic(QWidget *parent, const QJsonObject &user, int size);

protected:
	void paintEvent(QPaintEvent *e) override;

private:
	const uint64 _id = 0;
	const QString _name;
	const int _size = 0;
	QImage _image;

};

// One post, painted like a Telegram channel post: header, text, file
// block for need/have posts, picture, poll, counters and actions.
class PostCard final : public Ui::VerticalLayout {
public:
	PostCard(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		const QJsonObject &post);

	void setPost(const QJsonObject &post);
	[[nodiscard]] int64 postId() const;

private:
	void fill();
	void addHeader();
	void addFile(const QJsonObject &file);
	void addImage(const QString &url);
	void addPoll(const QJsonObject &poll);
	void addShared(const QJsonObject &shared);
	void addActions();
	void showMenu();
	void showReactions(QPoint globalPosition);
	void react(const QString &emoji);
	void requestFile();
	void apply(const QJsonObject &response);

	const not_null<Window::SessionController*> _controller;
	QJsonObject _post;
	base::unique_qptr<Ui::PopupMenu> _menu;

};

// A paged list of posts from one endpoint ("/v1/feed?tab=all" etc).
class PostList final : public Ui::VerticalLayout {
public:
	PostList(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		const QString &path,
		const QString &emptyText);

	void reload();

private:
	void load();

	const not_null<Window::SessionController*> _controller;
	const QString _path;
	const QString _emptyText;
	Ui::VerticalLayout *_posts = nullptr;
	Ui::FlatLabel *_status = nullptr;
	Ui::LinkButton *_more = nullptr;
	QString _cursor;
	bool _loading = false;
	int _generation = 0;

};

[[nodiscard]] QString FormatBytes(int64 size);
[[nodiscard]] QString KindLabel(const QString &kind);

} // namespace GB::Feed
