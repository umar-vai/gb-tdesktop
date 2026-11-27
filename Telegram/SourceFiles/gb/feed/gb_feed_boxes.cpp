/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/feed/gb_feed_boxes.h"

#include "base/unixtime.h"
#include "core/file_utilities.h"
#include "gb/feed/gb_feed_api.h"
#include "gb/feed/gb_feed_drive.h"
#include "gb/feed/gb_feed_posts.h"
#include "gb/transfers/gb_tr_boxes.h"
#include "gb/transfers/gb_tr_daemon.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "settings/settings_common.h"
#include "ui/basic_click_handlers.h"
#include "ui/boxes/confirm_box.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_entity.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
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
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtGui/QCursor>

namespace GB::Feed {
namespace {

using Controller = not_null<Window::SessionController*>;

[[nodiscard]] int64 IdOf(const QJsonValue &value) {
	return value.toVariant().toLongLong();
}

[[nodiscard]] Fail Toaster(Controller controller) {
	const auto weak = base::make_weak(controller);
	return [=](const QString &error) {
		if (const auto strong = weak.get()) {
			strong->showToast(ErrorText(error));
		}
	};
}

[[nodiscard]] QString NameOf(const QJsonObject &user) {
	const auto name = user.value(u"name"_q).toString();
	return name.isEmpty() ? u"GB user"_q : name;
}

// Accepts "4.2 GB", "700 MB", "12345" (bytes). Returns -1 if invalid.
[[nodiscard]] int64 ParseSize(const QString &text) {
	const auto match = QRegularExpression(
		u"^\\s*([0-9]+(?:[.,][0-9]+)?)\\s*([kmgt]?i?b?)?\\s*$"_q,
		QRegularExpression::CaseInsensitiveOption).match(text);
	if (!match.hasMatch()) {
		return -1;
	}
	auto value = match.captured(1).replace(',', '.').toDouble();
	const auto unit = match.captured(2).toLower();
	const auto power = unit.startsWith('k') ? 1
		: unit.startsWith('m') ? 2
		: unit.startsWith('g') ? 3
		: unit.startsWith('t') ? 4
		: 0;
	for (auto i = 0; i != power; ++i) {
		value *= 1024.;
	}
	return int64(value);
}

not_null<Ui::SettingsButton*> AddChoice(
		not_null<Ui::VerticalLayout*> container,
		Controller controller,
		const QString &title,
		std::vector<QString> labels,
		not_null<rpl::variable<int>*> value) {
	auto label = value->value() | rpl::map([=](int index) {
		return (index >= 0 && index < int(labels.size()))
			? labels[index]
			: QString();
	});
	const auto button = ::Settings::AddButtonWithLabel(
		container,
		rpl::single(title),
		std::move(label),
		st::settingsButtonNoIcon);
	button->setClickedCallback([=] {
		controller->show(Box(SingleChoiceBox, SingleChoiceBoxArgs{
			.title = rpl::single(title),
			.options = labels,
			.initialSelection = value->current(),
			.callback = [=](int index) { *value = index; },
		}));
	});
	return button;
}

not_null<Ui::InputField*> AddField(
		not_null<Ui::VerticalLayout*> container,
		const QString &placeholder,
		const QString &value = QString(),
		bool multiline = false,
		int maxLength = 0) {
	const auto field = container->add(
		object_ptr<Ui::InputField>(
			container,
			multiline ? st::newGroupDescription : st::defaultInputField,
			multiline
				? Ui::InputField::Mode::MultiLine
				: Ui::InputField::Mode::SingleLine,
			rpl::single(placeholder),
			value),
		st::boxRowPadding + QMargins(0, 0, 0, st::gbFeedSkip));
	if (maxLength) {
		field->setMaxLength(maxLength);
	}
	return field;
}

// A person: userpic, name and a sub line; clickable.
class PersonRow final : public Ui::AbstractButton {
public:
	PersonRow(QWidget *parent, const QJsonObject &user, const QString &sub)
	: AbstractButton(parent)
	, _userpic(this, user, st::gbFeedUserpic)
	, _name(this, NameOf(user), st::gbFeedName)
	, _sub(this, sub, st::gbFeedSub) {
		for (const auto w : std::initializer_list<QWidget*>{
				_userpic.data(), _name.data(), _sub.data() }) {
			w->setAttribute(Qt::WA_TransparentForMouseEvents);
		}
		resize(width(), st::gbFeedHeaderHeight + st::gbFeedSkip);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto &pad = st::boxRowPadding;
		const auto top = st::gbFeedSkip / 2;
		const auto available = std::max(
			newWidth - pad.left() - pad.right() - st::gbFeedNameLeft,
			1);
		_userpic->moveToLeft(pad.left(), top + (st::gbFeedHeaderHeight - st::gbFeedUserpic) / 2);
		_name->resizeToNaturalWidth(available);
		_name->moveToLeft(pad.left() + st::gbFeedNameLeft, top + st::gbFeedNameTop);
		_sub->resizeToNaturalWidth(available);
		_sub->moveToLeft(pad.left() + st::gbFeedNameLeft, top + st::gbFeedSubTop);
		return st::gbFeedHeaderHeight + st::gbFeedSkip;
	}

	void paintEvent(QPaintEvent *e) override {
		if (isOver()) {
			auto p = QPainter(this);
			p.fillRect(e->rect(), st::windowBgOver);
		}
	}

	void onStateChanged(State was, StateChangeSource source) override {
		update();
	}

private:
	object_ptr<Userpic> _userpic;
	object_ptr<Ui::FlatLabel> _name;
	object_ptr<Ui::FlatLabel> _sub;

};

not_null<PersonRow*> AddPerson(
		not_null<Ui::VerticalLayout*> container,
		Controller controller,
		const QJsonObject &user,
		const QString &sub) {
	const auto row = container->add(object_ptr<PersonRow>(container, user, sub));
	const auto id = uint64(IdOf(user.value(u"id"_q)));
	row->setClickedCallback([=] { ShowProfileBox(controller, id); });
	return row;
}

// A line of link buttons laid out left to right.
not_null<Ui::RpWidget*> AddLinks(
		not_null<Ui::VerticalLayout*> container,
		std::vector<std::pair<QString, Fn<void()>>> links,
		QMargins margin = st::boxRowPadding) {
	const auto row = container->add(object_ptr<Ui::RpWidget>(container), margin);
	row->resize(row->width(), st::gbFeedActionsHeight);
	auto buttons = std::vector<Ui::LinkButton*>();
	for (auto &[text, callback] : links) {
		const auto button = Ui::CreateChild<Ui::LinkButton>(row, text);
		button->setClickedCallback(std::move(callback));
		buttons.push_back(button);
	}
	row->widthValue() | rpl::on_next([=](int width) {
		auto left = 0;
		for (const auto button : buttons) {
			button->moveToLeft(left, (row->height() - button->height()) / 2, width);
			left += button->width() + st::gbFeedActionSkip;
		}
	}, row->lifetime());
	return row;
}

not_null<Ui::FlatLabel*> AddText(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		const style::FlatLabel &st = st::gbFeedText,
		QMargins margin = st::boxRowPadding) {
	return container->add(object_ptr<Ui::FlatLabel>(container, text, st), margin);
}

[[nodiscard]] QString NotificationText(const QJsonObject &n) {
	const auto actor = NameOf(n.value(u"actor"_q).toObject());
	const auto type = n.value(u"type"_q).toString();
	const auto text = n.value(u"text"_q).toString();
	const auto file = n.value(u"file_name"_q).toString();
	const auto community = n.value(u"community"_q).toObject().value(
		u"name"_q).toString();
	if (type == u"reaction"_q) {
		return actor + u" reacted "_q + text + u" to your post."_q;
	} else if (type == u"comment"_q) {
		return actor + u" commented: "_q + text;
	} else if (type == u"reply"_q) {
		return actor + u" replied: "_q + text;
	} else if (type == u"share"_q) {
		return actor + u" shared your post."_q;
	} else if (type == u"follow"_q) {
		return actor + u" started following you."_q;
	} else if (type == u"friend_request"_q) {
		return actor + u" sent you a friend request."_q;
	} else if (type == u"friend_accepted"_q) {
		return actor + u" accepted your friend request."_q;
	} else if (type == u"file_request"_q) {
		return actor + u" wants your file \""_q + file + u"\"."_q;
	} else if (type == u"file_approved"_q) {
		return actor + u" approved your request for \""_q + file
			+ u"\". Receive it now."_q;
	} else if (type == u"file_declined"_q) {
		return actor + u" declined your request for \""_q + file + u"\"."_q;
	} else if (type == u"join_request"_q) {
		return actor + u" asked to join "_q + community + '.';
	} else if (type == u"join_approved"_q) {
		return u"You were accepted into "_q + community + '.';
	}
	return actor + ' ' + type;
}

void ChooseAndUploadImage(
		Controller controller,
		QPointer<QWidget> parent,
		Fn<void(QString key)> done) {
	FileDialog::GetOpenPath(
		parent,
		u"Choose a picture"_q,
		u"Images (*.jpg *.jpeg *.png *.webp *.gif)"_q,
		[=](FileDialog::OpenResult &&result) {
			if (result.paths.isEmpty()) {
				return;
			}
			controller->showToast(u"Uploading..."_q);
			Api::Instance().uploadImage(
				result.paths.front(),
				[=](const QJsonObject &r) {
					done(r.value(u"key"_q).toString());
				},
				Toaster(controller));
		});
}

} // namespace

void OpenTelegramChat(Controller controller, const QJsonObject &user) {
	const auto username = user.value(u"username"_q).toString();
	if (username.isEmpty()) {
		controller->showToast(u"This user has no public @username, so GB "
			"can't open a chat. Ask them to message you first."_q);
		return;
	}
	// t.me links are handled inside the app (opens the chat).
	UrlClickHandler::Open(u"https://t.me/"_q + username);
}

void ShowComposeBox(Controller controller, ComposeArgs args) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto editing = !args.edit.isEmpty();
		const auto sharing = !args.shared.isEmpty();
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(editing
			? u"Edit post"_q
			: sharing
			? u"Share post"_q
			: args.communityId
			? u"New community post"_q
			: u"New post"_q));
		const auto container = box->verticalLayout();

		const auto kinds = std::vector<QString>{
			u"text"_q, u"need"_q, u"have"_q, u"poll"_q, u"file"_q };
		const auto kindLabels = std::vector<QString>{
			u"Post"_q,
			u"I need a file"_q,
			u"I have a big file (direct transfer)"_q,
			u"Poll"_q,
			u"Share a file (Telegram cloud)"_q };
		const auto privacies = std::vector<QString>{
			u"public"_q, u"friends"_q, u"only_me"_q };
		const auto privacyLabels = std::vector<QString>{
			u"Public"_q, u"Friends"_q, u"Only me"_q };
		const auto categories = std::vector<QString>{
			u"video"_q, u"movie"_q, u"series"_q, u"music"_q, u"software"_q,
			u"game"_q, u"book"_q, u"course"_q, u"document"_q, u"image"_q,
			u"archive"_q, u"other"_q };

		struct State {
			rpl::variable<int> kind = 0;
			rpl::variable<int> privacy = 0;
			rpl::variable<int> category = 11;
			rpl::variable<QString> image;
			rpl::variable<QString> localPath;
			bool sending = false;
		};
		const auto state = box->lifetime().make_state<State>();
		const auto initialKind = ranges::find(kinds, args.kind) - begin(kinds);
		if (initialKind < int(kinds.size())) {
			state->kind = int(initialKind);
		}

		if (sharing) {
			const auto author = NameOf(args.shared.value(u"author"_q).toObject());
			AddText(container, u"Sharing a post by "_q + author, st::gbFeedSub);
			Ui::AddSkip(container);
		} else if (!editing) {
			AddChoice(container, controller, u"Type"_q, kindLabels, &state->kind);
			Ui::AddSkip(container);
		}
		const auto text = AddField(
			container,
			u"What's on your mind?"_q,
			editing ? args.edit.value(u"text"_q).toString() : QString(),
			true,
			5000);

		// need / have: the file.
		const auto fileWrap = container->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				container,
				object_ptr<Ui::VerticalLayout>(container)));
		const auto file = fileWrap->entity();
		const auto fileName = AddField(file, u"File name"_q, QString(), false, 300);
		const auto fileSize = AddField(file, u"Size, e.g. 4.2 GB (optional)"_q);
		AddChoice(file, controller, u"Category"_q, categories, &state->category);
		AddLinks(file, { { u"Choose the file from this PC..."_q, [=] {
			FileDialog::GetOpenPath(
				QPointer<QWidget>(box.get()),
				u"Choose the file"_q,
				QString(),
				[=](FileDialog::OpenResult &&result) {
					if (result.paths.isEmpty()) {
						return;
					}
					const auto info = QFileInfo(result.paths.front());
					fileName->setText(info.fileName());
					fileSize->setText(FormatBytes(info.size()));
				});
		} } });
		fileWrap->toggleOn(state->kind.value() | rpl::map([=](int kind) {
			return !editing && !sharing && (kind == 1 || kind == 2);
		}));

		// file: a file from this PC, uploaded to the user's own G&B Drive
		// (their Telegram), then shared in the post.
		const auto driveWrap = container->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				container,
				object_ptr<Ui::VerticalLayout>(container)));
		const auto drive = driveWrap->entity();
		const auto driveLabel = AddText(drive, QString(), st::gbFeedSub);
		state->localPath.value() | rpl::on_next([=](const QString &path) {
			const auto info = QFileInfo(path);
			driveLabel->setText(path.isEmpty()
				? u"Up to %1. The file stays in your own Telegram (an "
					"archived \"G&B Drive\" channel); anyone in the Feed "
					"can download it."_q.arg(
						FormatBytes(DriveFileLimit(controller)))
				: (u"📁 "_q + info.fileName() + u" · "_q
					+ FormatBytes(info.size())));
		}, driveLabel->lifetime());
		AddLinks(drive, { { u"Choose the file..."_q, [=] {
			FileDialog::GetOpenPath(
				QPointer<QWidget>(box.get()),
				u"Choose the file to share"_q,
				QString(),
				[=](FileDialog::OpenResult &&result) {
					if (result.paths.isEmpty()) {
						return;
					}
					const auto path = result.paths.front();
					if (QFileInfo(path).size() > DriveFileLimit(controller)) {
						controller->showToast(u"This file is bigger than "
							"Telegram allows. Choose \"I have a big file\" "
							"to send it directly instead."_q);
						return;
					}
					state->localPath = path;
				});
		} } });
		AddChoice(drive, controller, u"Category"_q, categories, &state->category);
		driveWrap->toggleOn(state->kind.value() | rpl::map([=](int kind) {
			return !editing && !sharing && kind == 4;
		}));

		// poll: options.
		const auto pollWrap = container->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				container,
				object_ptr<Ui::VerticalLayout>(container)));
		const auto options = AddField(
			pollWrap->entity(),
			u"Options, one per line (2 to 10)"_q,
			QString(),
			true);
		pollWrap->toggleOn(state->kind.value() | rpl::map([=](int kind) {
			return !editing && !sharing && kind == 3;
		}));

		Ui::InputField *tags = nullptr;
		Ui::InputField *link = nullptr;
		if (!sharing) {
			auto initialTags = QStringList();
			for (const auto tag : args.edit.value(u"tags"_q).toArray()) {
				initialTags.push_back('#' + tag.toString());
			}
			tags = AddField(
				container,
				u"Tags, e.g. #linux #iso (optional)"_q,
				initialTags.join(' '));
			if (!editing) {
				link = AddField(container, u"Link (optional)"_q);
				const auto imageLabel = AddText(
					container,
					QString(),
					st::gbFeedSub);
				state->image.value() | rpl::on_next([=](const QString &key) {
					imageLabel->setText(key.isEmpty()
						? QString()
						: u"Picture attached."_q);
				}, imageLabel->lifetime());
				AddLinks(container, { { u"Attach a picture..."_q, [=] {
					ChooseAndUploadImage(
						controller,
						QPointer<QWidget>(box.get()),
						crl::guard(box, [=](QString key) {
							state->image = key;
						}));
				} } });
			}
		}
		if (!args.communityId
			&& (editing
				? args.edit.value(u"community"_q).toObject().isEmpty()
				: true)) {
			if (editing) {
				const auto current = ranges::find(
					privacies,
					args.edit.value(u"privacy"_q).toString()) - begin(privacies);
				state->privacy = std::min(int(current), 0 + 2);
			}
			AddChoice(
				container,
				controller,
				u"Who can see it"_q,
				privacyLabels,
				&state->privacy);
		}

		const auto submit = [=] {
			if (state->sending) {
				return;
			}
			const auto kind = kinds[state->kind.current()];
			auto body = QJsonObject();
			body.insert(u"text"_q, text->getLastText().trimmed());
			if (tags) {
				body.insert(u"tags"_q, tags->getLastText());
			}
			if (!args.communityId) {
				body.insert(u"privacy"_q, privacies[state->privacy.current()]);
			}
			auto method = u"POST"_q;
			auto path = u"/v1/posts"_q;
			if (editing) {
				method = u"PATCH"_q;
				path += '/' + QString::number(IdOf(args.edit.value(u"id"_q)));
			} else if (sharing) {
				body.insert(
					u"shared_post_id"_q,
					IdOf(args.shared.value(u"id"_q)));
			} else {
				body.insert(u"kind"_q, kind);
				if (args.communityId) {
					body.insert(u"community_id"_q, args.communityId);
				}
				if (link && !link->getLastText().trimmed().isEmpty()) {
					body.insert(u"link_url"_q, link->getLastText().trimmed());
				}
				if (!state->image.current().isEmpty()) {
					body.insert(
						u"media"_q,
						QJsonArray{ state->image.current() });
				}
				if (kind == u"need"_q || kind == u"have"_q) {
					auto f = QJsonObject();
					const auto name = fileName->getLastText().trimmed();
					if (name.isEmpty()) {
						fileName->showError();
						return;
					}
					f.insert(u"name"_q, name);
					const auto sizeText = fileSize->getLastText().trimmed();
					if (!sizeText.isEmpty()) {
						const auto size = ParseSize(sizeText);
						if (size < 0) {
							fileSize->showError();
							return;
						}
						f.insert(u"size"_q, double(size));
					}
					f.insert(
						u"category"_q,
						categories[state->category.current()]);
					body.insert(u"file"_q, f);
				} else if (kind == u"poll"_q) {
					auto list = QJsonArray();
					for (const auto &line : options->getLastText().split('\n')) {
						if (!line.trimmed().isEmpty()) {
							list.push_back(line.trimmed());
						}
					}
					if (list.size() < 2 || list.size() > 10) {
						options->showError();
						return;
					}
					body.insert(u"options"_q, list);
				}
			}
			state->sending = true;
			const auto weak = QPointer<Ui::GenericBox>(box.get());
			const auto post = [=](QJsonObject body) {
				Api::Instance().send(method, path, body, [=](const QJsonObject &) {
					controller->showToast(editing ? u"Saved."_q : u"Posted."_q);
					Api::Instance().notifyChanged();
					if (const auto strong = weak.data()) {
						strong->closeBox();
					}
				}, [=](const QString &error) {
					state->sending = false;
					controller->showToast(ErrorText(error));
				});
			};
			if (!editing && !sharing && kind == u"file"_q) {
				const auto local = state->localPath.current();
				if (local.isEmpty()) {
					state->sending = false;
					controller->showToast(u"Choose the file first."_q);
					return;
				}
				auto f = QJsonObject();
				f.insert(u"category"_q, categories[state->category.current()]);
				body.insert(u"file"_q, f);
				box->setTitle(rpl::single(u"Uploading to your G&B Drive..."_q));
				UploadToDrive(controller, local, crl::guard(box, [=](UploadProgress p) {
					if (p.total > 0) {
						box->setTitle(rpl::single(u"Uploading... %1%"_q.arg(
							int(p.sent * 100 / p.total))));
					}
				}), [=](QJsonObject file) {
					auto copy = body;
					copy.insert(u"cloud_file_id"_q, file.value(u"id"_q));
					post(copy);
				}, [=](QString error) {
					state->sending = false;
					if (const auto strong = weak.data()) {
						strong->setTitle(rpl::single(u"New post"_q));
					}
					controller->showToast((error == u"file_too_big"_q)
						? u"This file is bigger than Telegram allows."_q
						: ErrorText(error));
				});
				return;
			}
			post(body);
		};
		box->addButton(
			rpl::single(editing ? u"Save"_q : sharing ? u"Share"_q : u"Post"_q),
			submit);
		box->addButton(rpl::single(u"Cancel"_q), [=] { box->closeBox(); });
		box->setFocusCallback([=] { text->setFocusFast(); });
	}));
}

void ShowCommentsBox(Controller controller, const QJsonObject &post) {
	const auto postId = IdOf(post.value(u"id"_q));
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(u"Comments"_q));
		const auto container = box->verticalLayout();
		const auto author = NameOf(post.value(u"author"_q).toObject());
		auto summary = author;
		const auto file = post.value(u"file"_q).toObject();
		if (!file.isEmpty()) {
			summary += u" • "_q + KindLabel(post.value(u"kind"_q).toString())
				+ ' ' + file.value(u"name"_q).toString();
		}
		const auto text = post.value(u"text"_q).toString();
		if (!text.isEmpty()) {
			summary += u"\n"_q + text.left(400);
		}
		AddText(container, summary, st::gbFeedSub);
		Ui::AddSkip(container);
		Ui::AddDivider(container);
		Ui::AddSkip(container);

		struct State {
			int64 replyTo = 0;
			bool sending = false;
		};
		const auto state = box->lifetime().make_state<State>();
		const auto list = container->add(object_ptr<Ui::VerticalLayout>(container));
		const auto replying = container->add(
			object_ptr<Ui::SlideWrap<Ui::RpWidget>>(
				container,
				object_ptr<Ui::RpWidget>(container)));
		replying->hide(anim::type::instant);
		const auto replyRow = replying->entity();
		const auto replyLabel = Ui::CreateChild<Ui::LinkButton>(
			replyRow,
			QString());
		replyRow->resize(replyRow->width(), st::gbFeedActionsHeight);
		replyLabel->moveToLeft(st::boxRowPadding.left(), 6);
		const auto field = AddField(container, u"Write a comment..."_q, QString(), true, 2000);
		replyLabel->setClickedCallback([=] {
			state->replyTo = 0;
			replying->hide(anim::type::normal);
		});

		const auto load = std::make_shared<Fn<void()>>();
		*load = [=] {
			const auto weak = QPointer<Ui::VerticalLayout>(list);
			Api::Instance().get(u"/v1/posts/%1/comments"_q.arg(postId), [=](
					const QJsonObject &r) {
				const auto strong = weak.data();
				if (!strong) {
					return;
				}
				strong->clear();
				const auto comments = r.value(u"comments"_q).toArray();
				if (comments.isEmpty()) {
					AddText(strong, u"No comments yet. Be the first."_q, st::gbFeedSub);
				}
				auto authors = base::flat_map<int64, QString>();
				for (const auto value : comments) {
					const auto c = value.toObject();
					authors[IdOf(c.value(u"id"_q))] = NameOf(
						c.value(u"author"_q).toObject());
				}
				for (const auto value : comments) {
					const auto c = value.toObject();
					const auto id = IdOf(c.value(u"id"_q));
					const auto parent = IdOf(c.value(u"parent_id"_q));
					const auto user = c.value(u"author"_q).toObject();
					auto sub = RelativeTime(IdOf(c.value(u"created_at"_q)));
					if (parent && authors.contains(parent)) {
						sub = u"reply to "_q + authors[parent] + u" • "_q + sub;
					}
					const auto indent = parent ? st::gbFeedNameLeft : 0;
					const auto row = strong->add(object_ptr<PersonRow>(strong, user, sub));
					const auto userId = uint64(IdOf(user.value(u"id"_q)));
					row->setClickedCallback([=] {
						ShowProfileBox(controller, userId);
					});
					const auto label = AddText(
						strong,
						QString(),
						st::gbFeedText,
						st::boxRowPadding + QMargins(st::gbFeedNameLeft + indent, 0, 0, 0));
					label->setMarkedText(TextUtilities::ParseEntities(
						c.value(u"text"_q).toString(),
						TextParseLinks));
					label->setSelectable(true);
					auto links = std::vector<std::pair<QString, Fn<void()>>>();
					const auto name = NameOf(user);
					links.push_back({ u"Reply"_q, [=] {
						state->replyTo = id;
						replyLabel->setText(u"Replying to "_q + name + u"  ✕"_q);
						replying->show(anim::type::normal);
						field->setFocusFast();
					} });
					if (c.value(u"mine"_q).toBool()
						|| post.value(u"mine"_q).toBool()) {
						links.push_back({ u"Delete"_q, [=] {
							Api::Instance().send(
								u"DELETE"_q,
								u"/v1/comments/%1"_q.arg(id),
								{},
								[=](const QJsonObject &) { (*load)(); },
								Toaster(controller));
						} });
					} else {
						links.push_back({ u"Report"_q, [=] {
							ShowReportBox(controller, u"comment"_q, id);
						} });
					}
					AddLinks(
						strong,
						std::move(links),
						st::boxRowPadding + QMargins(st::gbFeedNameLeft + indent, 0, 0, 0));
				}
				strong->resizeToWidth(strong->width());
			}, Toaster(controller));
		};
		(*load)();

		const auto submit = [=] {
			const auto value = field->getLastText().trimmed();
			if (value.isEmpty() || state->sending) {
				field->showError();
				return;
			}
			auto body = QJsonObject();
			body.insert(u"text"_q, value);
			if (state->replyTo) {
				body.insert(u"parent_id"_q, state->replyTo);
			}
			state->sending = true;
			const auto weak = QPointer<Ui::InputField>(field);
			Api::Instance().send(
				u"POST"_q,
				u"/v1/posts/%1/comments"_q.arg(postId),
				body,
				[=](const QJsonObject &) {
					state->sending = false;
					state->replyTo = 0;
					if (const auto strong = weak.data()) {
						strong->clear();
						replying->hide(anim::type::instant);
						(*load)();
					}
					Api::Instance().notifyChanged();
				},
				[=](const QString &error) {
					state->sending = false;
					controller->showToast(ErrorText(error));
				});
		};
		box->addButton(rpl::single(u"Send"_q), submit);
		box->addButton(rpl::single(u"Close"_q), [=] { box->closeBox(); });
		box->setFocusCallback([=] { field->setFocusFast(); });
	}));
}

void ShowPostBox(Controller controller, int64 postId) {
	Api::Instance().get(u"/v1/posts/%1"_q.arg(postId), [=](const QJsonObject &r) {
		const auto post = r.value(u"post"_q).toObject();
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			box->setWidth(st::boxWideWidth);
			box->setTitle(rpl::single(u"Post"_q));
			box->verticalLayout()->add(
				object_ptr<PostCard>(box->verticalLayout(), controller, post));
			box->addButton(rpl::single(u"Comments"_q), [=] {
				ShowCommentsBox(controller, post);
			});
			box->addButton(rpl::single(u"Close"_q), [=] { box->closeBox(); });
		}));
	}, Toaster(controller));
}

void ShowProfileBox(Controller controller, uint64 userId) {
	Api::Instance().get(u"/v1/users/%1"_q.arg(userId), [=](const QJsonObject &r) {
		const auto user = r.value(u"user"_q).toObject();
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			box->setWidth(st::boxWideWidth);
			box->setTitle(rpl::single(NameOf(user)));
			const auto container = box->verticalLayout();
			const auto isMe = user.value(u"is_me"_q).toBool();
			const auto counts = user.value(u"counts"_q).toObject();
			const auto rel = user.value(u"relationship"_q).toObject();
			const auto username = user.value(u"username"_q).toString();

			auto sub = QStringList();
			if (!username.isEmpty()) {
				sub.push_back('@' + username);
			}
			sub.push_back(QString::number(counts.value(u"posts"_q).toInt()) + u" posts"_q);
			if (rel.value(u"followed_by"_q).toBool()) {
				sub.push_back(u"follows you"_q);
			}
			container->add(object_ptr<PersonRow>(container, user, sub.join(u" • "_q)));
			const auto bio = user.value(u"bio"_q).toString();
			if (!bio.isEmpty()) {
				AddText(container, bio);
				Ui::AddSkip(container);
			}
			const auto path = [=](const QString &what) {
				return u"/v1/users/%1/"_q.arg(userId) + what;
			};
			AddLinks(container, {
				{ QString::number(counts.value(u"followers"_q).toInt()) + u" followers"_q, [=] {
					ShowPeopleBox(controller, u"Followers"_q, path(u"followers"_q));
				} },
				{ QString::number(counts.value(u"following"_q).toInt()) + u" following"_q, [=] {
					ShowPeopleBox(controller, u"Following"_q, path(u"following"_q));
				} },
				{ QString::number(counts.value(u"friends"_q).toInt()) + u" friends"_q, [=] {
					ShowPeopleBox(controller, u"Friends"_q, path(u"friends"_q));
				} },
				{ QString::number(counts.value(u"have"_q).toInt()) + u" files I have"_q, [=] {
					ShowPostsBox(
						controller,
						NameOf(user) + u" has"_q,
						path(u"have"_q),
						u"No \"have\" posts yet."_q);
				} },
			});

			const auto act = [=](const QString &method, const QString &what) {
				const auto weak = QPointer<Ui::GenericBox>(box.get());
				Api::Instance().send(method, path(what), {}, [=](const QJsonObject &) {
					if (const auto strong = weak.data()) {
						strong->closeBox();
					}
					ShowProfileBox(controller, userId);
				}, Toaster(controller));
			};
			if (isMe) {
				AddLinks(container, {
					{ u"Edit profile"_q, [=] {
						box->closeBox();
						ShowEditProfileBox(controller);
					} },
					{ u"Saved posts"_q, [=] {
						ShowPostsBox(controller, u"Saved"_q, u"/v1/me/saved"_q, u"Nothing saved yet."_q);
					} },
				});
			} else {
				const auto following = rel.value(u"following"_q).toBool();
				const auto friendState = rel.value(u"friend"_q).toString();
				auto links = std::vector<std::pair<QString, Fn<void()>>>();
				links.push_back({ following ? u"Unfollow"_q : u"Follow"_q, [=] {
					act(following ? u"DELETE"_q : u"POST"_q, u"follow"_q);
				} });
				if (friendState == u"none"_q) {
					links.push_back({ u"Add friend"_q, [=] { act(u"POST"_q, u"friend"_q); } });
				} else if (friendState == u"incoming"_q) {
					links.push_back({ u"Accept friend request"_q, [=] { act(u"POST"_q, u"friend"_q); } });
				} else if (friendState == u"outgoing"_q) {
					links.push_back({ u"Cancel request"_q, [=] { act(u"DELETE"_q, u"friend"_q); } });
				} else {
					links.push_back({ u"Unfriend"_q, [=] { act(u"DELETE"_q, u"friend"_q); } });
				}
				if (!username.isEmpty()) {
					links.push_back({ u"Message"_q, [=] {
						box->closeBox();
						OpenTelegramChat(controller, user);
					} });
				}
				AddLinks(container, std::move(links));
				const auto muted = rel.value(u"muted"_q).toBool();
				const auto blocked = rel.value(u"blocked"_q).toBool();
				AddLinks(container, {
					{ muted ? u"Unmute"_q : u"Mute"_q, [=] {
						act(muted ? u"DELETE"_q : u"POST"_q, u"mute"_q);
					} },
					{ blocked ? u"Unblock"_q : u"Block"_q, [=] {
						act(blocked ? u"DELETE"_q : u"POST"_q, u"block"_q);
					} },
					{ u"Report"_q, [=] {
						ShowReportBox(controller, u"user"_q, int64(userId));
					} },
				});
			}
			Ui::AddSkip(container);
			Ui::AddDivider(container);
			container->add(object_ptr<PostList>(
				container,
				controller,
				path(u"posts"_q),
				u"No posts yet."_q));
			box->addButton(rpl::single(u"Close"_q), [=] { box->closeBox(); });
		}));
	}, Toaster(controller));
}

void ShowEditProfileBox(Controller controller) {
	Api::Instance().get(u"/v1/me"_q, [=](const QJsonObject &r) {
		const auto user = r.value(u"user"_q).toObject();
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			box->setTitle(rpl::single(u"Edit profile"_q));
			const auto container = box->verticalLayout();
			const auto name = AddField(container, u"Name"_q, NameOf(user), false, 64);
			const auto bio = AddField(container, u"About you"_q, user.value(u"bio"_q).toString(), true, 500);
			const auto avatar = box->lifetime().make_state<QString>();
			const auto label = AddText(container, QString(), st::gbFeedSub);
			AddLinks(container, { { u"Change profile picture..."_q, [=] {
				ChooseAndUploadImage(controller, QPointer<QWidget>(box.get()), crl::guard(box, [=](QString key) {
					*avatar = key;
					label->setText(u"New picture ready."_q);
				}));
			} } });
			box->addButton(rpl::single(u"Save"_q), [=] {
				auto body = QJsonObject();
				body.insert(u"name"_q, name->getLastText().trimmed());
				body.insert(u"bio"_q, bio->getLastText().trimmed());
				if (!avatar->isEmpty()) {
					body.insert(u"avatar"_q, *avatar);
				}
				const auto weak = QPointer<Ui::GenericBox>(box.get());
				Api::Instance().send(u"PATCH"_q, u"/v1/me"_q, body, [=](const QJsonObject &) {
					controller->showToast(u"Profile saved."_q);
					Api::Instance().notifyChanged();
					if (const auto strong = weak.data()) {
						strong->closeBox();
					}
				}, Toaster(controller));
			});
			box->addButton(rpl::single(u"Cancel"_q), [=] { box->closeBox(); });
		}));
	}, Toaster(controller));
}

void ShowNotificationsBox(Controller controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(u"Notifications"_q));
		const auto container = box->verticalLayout();
		const auto list = container->add(object_ptr<Ui::VerticalLayout>(container));
		AddText(list, u"Loading..."_q, st::gbFeedSub);
		const auto weak = QPointer<Ui::VerticalLayout>(list);
		Api::Instance().get(u"/v1/notifications"_q, [=](const QJsonObject &r) {
			const auto strong = weak.data();
			if (!strong) {
				return;
			}
			strong->clear();
			const auto items = r.value(u"notifications"_q).toArray();
			if (items.isEmpty()) {
				AddText(strong, u"No notifications yet."_q, st::gbFeedSub);
			}
			auto maxId = int64(0);
			for (const auto value : items) {
				const auto n = value.toObject();
				maxId = std::max(maxId, IdOf(n.value(u"id"_q)));
				const auto type = n.value(u"type"_q).toString();
				const auto actor = n.value(u"actor"_q).toObject();
				auto sub = RelativeTime(IdOf(n.value(u"created_at"_q)));
				if (!n.value(u"read"_q).toBool()) {
					sub = u"● new • "_q + sub;
				}
				const auto row = strong->add(object_ptr<PersonRow>(
					strong,
					actor.isEmpty() ? QJsonObject{ { u"name"_q, u"GB"_q } } : actor,
					sub));
				AddText(
					strong,
					NotificationText(n),
					st::gbFeedText,
					st::boxRowPadding + QMargins(st::gbFeedNameLeft, 0, 0, st::gbFeedSkip));
				const auto postId = IdOf(n.value(u"post_id"_q));
				const auto communityId = IdOf(
					n.value(u"community"_q).toObject().value(u"id"_q));
				const auto actorId = uint64(IdOf(actor.value(u"id"_q)));
				row->setClickedCallback([=] {
					if (type.startsWith(u"file_"_q)) {
						ShowRequestsBox(controller);
					} else if (type == u"friend_request"_q) {
						ShowFriendRequestsBox(controller);
					} else if (communityId) {
						ShowCommunityBox(controller, communityId);
					} else if (postId) {
						ShowPostBox(controller, postId);
					} else if (actorId) {
						ShowProfileBox(controller, actorId);
					}
				});
			}
			strong->resizeToWidth(strong->width());
			if (maxId) {
				auto body = QJsonObject();
				body.insert(u"up_to"_q, double(maxId));
				Api::Instance().send(u"POST"_q, u"/v1/notifications/read"_q, body, [](const QJsonObject &) {
					Api::Instance().refreshUnread();
				});
			}
		}, Toaster(controller));
		box->addButton(rpl::single(u"Close"_q), [=] { box->closeBox(); });
	}));
}

void ApproveFileRequest(
		Controller controller,
		int64 requestId,
		const QString &fileName,
		Fn<void()> done) {
	FileDialog::GetOpenPath(
		QPointer<QWidget>(controller->widget().get()),
		u"Choose \"%1\" on this PC"_q.arg(fileName),
		QString(),
		[=](FileDialog::OpenResult &&result) {
			if (result.paths.isEmpty()) {
				return;
			}
			auto &daemon = Transfers::Daemon::Instance();
			daemon.ensureStarted();
			daemon.createOffer(result.paths.front(), [=](Transfers::SendRow offer) {
				auto body = QJsonObject();
				body.insert(u"invite"_q, offer.invite);
				Api::Instance().send(
					u"POST"_q,
					u"/v1/requests/%1/approve"_q.arg(requestId),
					body,
					[=](const QJsonObject &) {
						controller->showToast(u"Approved. The file is shared "
							"and the requester's GB will start receiving it. "
							"Keep GB open until it finishes."_q);
						if (done) {
							done();
						}
					},
					Toaster(controller));
			}, [=](QString error) {
				controller->showToast(u"Could not share the file: "_q + error);
			});
		});
}

void ShowRequestsBox(Controller controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(u"File requests"_q));
		const auto container = box->verticalLayout();
		const auto fill = std::make_shared<Fn<void()>>();
		const auto incoming = container->add(object_ptr<Ui::VerticalLayout>(container));
		const auto outgoing = container->add(object_ptr<Ui::VerticalLayout>(container));
		*fill = [=] {
			const auto load = [=](
					not_null<Ui::VerticalLayout*> list,
					const QString &box_,
					const QString &title) {
				const auto weak = QPointer<Ui::VerticalLayout>(list.get());
				Api::Instance().get(u"/v1/requests?box="_q + box_, [=](const QJsonObject &r) {
					const auto strong = weak.data();
					if (!strong) {
						return;
					}
					strong->clear();
					Ui::AddSubsectionTitle(strong, rpl::single(title));
					const auto items = r.value(u"requests"_q).toArray();
					if (items.isEmpty()) {
						AddText(strong, u"None."_q, st::gbFeedSub);
					}
					const auto mineToGive = (box_ == u"incoming"_q);
					for (const auto value : items) {
						const auto q = value.toObject();
						const auto id = IdOf(q.value(u"id"_q));
						const auto status = q.value(u"status"_q).toString();
						const auto file = q.value(u"file"_q).toObject();
						const auto name = file.value(u"name"_q).toString();
						const auto size = IdOf(file.value(u"size"_q));
						auto sub = name;
						if (size > 0) {
							sub += u" • "_q + FormatBytes(size);
						}
						sub += u" • "_q + status;
						AddPerson(strong, controller, q.value(u"user"_q).toObject(), sub);
						const auto note = q.value(u"note"_q).toString();
						if (!note.isEmpty()) {
							AddText(strong, note, st::gbFeedSub,
								st::boxRowPadding + QMargins(st::gbFeedNameLeft, 0, 0, 0));
						}
						auto links = std::vector<std::pair<QString, Fn<void()>>>();
						if (mineToGive && status == u"pending"_q) {
							links.push_back({ u"Approve and send"_q, [=] {
								ApproveFileRequest(controller, id, name, [=] { (*fill)(); });
							} });
							links.push_back({ u"Decline"_q, [=] {
								Api::Instance().send(u"POST"_q, u"/v1/requests/%1/decline"_q.arg(id), {},
									[=](const QJsonObject &) { (*fill)(); }, Toaster(controller));
							} });
						} else if (!mineToGive && status == u"approved"_q) {
							const auto invite = q.value(u"invite"_q).toString();
							links.push_back({ u"Receive now"_q, [=] {
								Transfers::Receive(controller, invite);
							} });
						} else if (!mineToGive && status == u"pending"_q) {
							links.push_back({ u"Cancel request"_q, [=] {
								Api::Instance().send(u"POST"_q, u"/v1/requests/%1/cancel"_q.arg(id), {},
									[=](const QJsonObject &) { (*fill)(); }, Toaster(controller));
							} });
						}
						const auto postId = IdOf(q.value(u"post_id"_q));
						links.push_back({ u"Open post"_q, [=] { ShowPostBox(controller, postId); } });
						AddLinks(strong, std::move(links),
							st::boxRowPadding + QMargins(st::gbFeedNameLeft, 0, 0, st::gbFeedSkip));
					}
					strong->resizeToWidth(strong->width());
				}, Toaster(controller));
			};
			load(incoming, u"incoming"_q, u"People asking for your files"_q);
			load(outgoing, u"outgoing"_q, u"Files you asked for"_q);
		};
		(*fill)();
		box->addButton(rpl::single(u"Close"_q), [=] { box->closeBox(); });
	}));
}

void ShowPeopleBox(Controller controller, const QString &title, const QString &path) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(title));
		const auto list = box->verticalLayout()->add(
			object_ptr<Ui::VerticalLayout>(box->verticalLayout()));
		AddText(list, u"Loading..."_q, st::gbFeedSub);
		const auto weak = QPointer<Ui::VerticalLayout>(list);
		Api::Instance().get(path, [=](const QJsonObject &r) {
			const auto strong = weak.data();
			if (!strong) {
				return;
			}
			strong->clear();
			const auto users = r.value(u"users"_q).toArray();
			if (users.isEmpty()) {
				AddText(strong, u"Nobody here yet."_q, st::gbFeedSub);
			}
			for (const auto value : users) {
				const auto user = value.toObject();
				const auto username = user.value(u"username"_q).toString();
				AddPerson(strong, controller, user, username.isEmpty()
					? user.value(u"bio"_q).toString().left(60)
					: ('@' + username));
			}
			strong->resizeToWidth(strong->width());
		}, Toaster(controller));
		box->addButton(rpl::single(u"Close"_q), [=] { box->closeBox(); });
	}));
}

void ShowSuggestionsBox(Controller controller) {
	ShowPeopleBox(controller, u"People you may know"_q, u"/v1/suggestions"_q);
}

void ShowFriendRequestsBox(Controller controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Friend requests"_q));
		const auto list = box->verticalLayout()->add(
			object_ptr<Ui::VerticalLayout>(box->verticalLayout()));
		const auto fill = std::make_shared<Fn<void()>>();
		*fill = [=] {
			const auto weak = QPointer<Ui::VerticalLayout>(list);
			Api::Instance().get(u"/v1/me/friend-requests"_q, [=](const QJsonObject &r) {
				const auto strong = weak.data();
				if (!strong) {
					return;
				}
				strong->clear();
				const auto section = [&](const QString &title, const QString &key, bool incoming) {
					Ui::AddSubsectionTitle(strong, rpl::single(title));
					const auto users = r.value(key).toArray();
					if (users.isEmpty()) {
						AddText(strong, u"None."_q, st::gbFeedSub);
					}
					for (const auto value : users) {
						const auto user = value.toObject();
						const auto id = IdOf(user.value(u"id"_q));
						AddPerson(strong, controller, user, QString());
						const auto act = [=](const QString &method) {
							Api::Instance().send(method, u"/v1/users/%1/friend"_q.arg(id), {},
								[=](const QJsonObject &) {
									(*fill)();
									Api::Instance().refreshUnread();
								}, Toaster(controller));
						};
						if (incoming) {
							AddLinks(strong, {
								{ u"Accept"_q, [=] { act(u"POST"_q); } },
								{ u"Decline"_q, [=] { act(u"DELETE"_q); } },
							}, st::boxRowPadding + QMargins(st::gbFeedNameLeft, 0, 0, 0));
						} else {
							AddLinks(strong, {
								{ u"Cancel"_q, [=] { act(u"DELETE"_q); } },
							}, st::boxRowPadding + QMargins(st::gbFeedNameLeft, 0, 0, 0));
						}
					}
				};
				section(u"Received"_q, u"incoming"_q, true);
				section(u"Sent"_q, u"outgoing"_q, false);
				strong->resizeToWidth(strong->width());
			}, Toaster(controller));
		};
		(*fill)();
		box->addButton(rpl::single(u"Close"_q), [=] { box->closeBox(); });
	}));
}

void ShowPostsBox(
		Controller controller,
		const QString &title,
		const QString &path,
		const QString &emptyText) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(title));
		box->verticalLayout()->add(object_ptr<PostList>(
			box->verticalLayout(),
			controller,
			path,
			emptyText));
		box->addButton(rpl::single(u"Close"_q), [=] { box->closeBox(); });
	}));
}

void ShowReactionsBox(Controller controller, int64 postId) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Reactions"_q));
		const auto list = box->verticalLayout()->add(
			object_ptr<Ui::VerticalLayout>(box->verticalLayout()));
		const auto weak = QPointer<Ui::VerticalLayout>(list);
		Api::Instance().get(u"/v1/posts/%1/reactions"_q.arg(postId), [=](const QJsonObject &r) {
			const auto strong = weak.data();
			if (!strong) {
				return;
			}
			for (const auto value : r.value(u"reactions"_q).toArray()) {
				const auto item = value.toObject();
				AddPerson(
					strong,
					controller,
					item.value(u"user"_q).toObject(),
					item.value(u"emoji"_q).toString());
			}
			strong->resizeToWidth(strong->width());
		}, Toaster(controller));
		box->addButton(rpl::single(u"Close"_q), [=] { box->closeBox(); });
	}));
}

void ShowCommunitiesBox(Controller controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Communities"_q));
		const auto container = box->verticalLayout();
		AddLinks(container, { { u"+ Create a group or page"_q, [=] {
			box->closeBox();
			ShowCreateCommunityBox(controller);
		} } });
		const auto addList = [=](const QString &title, const QString &path) {
			const auto list = container->add(object_ptr<Ui::VerticalLayout>(container));
			const auto weak = QPointer<Ui::VerticalLayout>(list);
			Api::Instance().get(path, [=](const QJsonObject &r) {
				const auto strong = weak.data();
				if (!strong) {
					return;
				}
				Ui::AddSubsectionTitle(strong, rpl::single(title));
				const auto items = r.value(u"communities"_q).toArray();
				if (items.isEmpty()) {
					AddText(strong, u"None yet."_q, st::gbFeedSub);
				}
				for (const auto value : items) {
					const auto c = value.toObject();
					const auto id = IdOf(c.value(u"id"_q));
					auto sub = QStringList{
						c.value(u"kind"_q).toString() == u"page"_q ? u"Page"_q : u"Group"_q,
						c.value(u"privacy"_q).toString(),
						QString::number(c.value(u"member_count"_q).toInt()) + u" members"_q,
					};
					const auto role = c.value(u"my_role"_q).toString();
					if (!role.isEmpty()) {
						sub.push_back(role);
					}
					auto pseudoUser = QJsonObject{
						{ u"id"_q, double(id) },
						{ u"name"_q, c.value(u"name"_q).toString() },
						{ u"avatar"_q, c.value(u"avatar"_q) },
					};
					const auto row = strong->add(object_ptr<PersonRow>(
						strong,
						pseudoUser,
						sub.join(u" • "_q)));
					row->setClickedCallback([=] {
						ShowCommunityBox(controller, id);
					});
				}
				strong->resizeToWidth(strong->width());
			}, Toaster(controller));
		};
		addList(u"Yours"_q, u"/v1/communities?mine=1"_q);
		addList(u"Discover"_q, u"/v1/communities"_q);
		box->addButton(rpl::single(u"Close"_q), [=] { box->closeBox(); });
	}));
}

void ShowCommunityBox(Controller controller, int64 communityId) {
	const auto base = u"/v1/communities/%1"_q.arg(communityId);
	Api::Instance().get(base, [=](const QJsonObject &r) {
		const auto c = r.value(u"community"_q).toObject();
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			box->setWidth(st::boxWideWidth);
			box->setTitle(rpl::single(c.value(u"name"_q).toString()));
			const auto container = box->verticalLayout();
			const auto role = c.value(u"my_role"_q).toString();
			const auto isPage = (c.value(u"kind"_q).toString() == u"page"_q);
			const auto isStaff = (role == u"owner"_q || role == u"admin"_q || role == u"moderator"_q);
			const auto isMember = !role.isEmpty() && role != u"pending"_q;
			auto info = QStringList{
				isPage ? u"Page"_q : u"Group"_q,
				c.value(u"privacy"_q).toString(),
				QString::number(c.value(u"member_count"_q).toInt())
					+ (isPage ? u" followers"_q : u" members"_q),
			};
			if (!role.isEmpty()) {
				info.push_back(u"you: "_q + role);
			}
			AddText(container, info.join(u" • "_q), st::gbFeedSub);
			const auto description = c.value(u"description"_q).toString();
			if (!description.isEmpty()) {
				AddText(container, description);
			}
			const auto rules = c.value(u"rules"_q).toString();
			if (!rules.isEmpty()) {
				Ui::AddSubsectionTitle(container, rpl::single(u"Rules"_q));
				AddText(container, rules);
			}
			const auto reopen = [=] {
				box->closeBox();
				ShowCommunityBox(controller, communityId);
			};
			auto links = std::vector<std::pair<QString, Fn<void()>>>();
			if (role.isEmpty()) {
				links.push_back({ isPage ? u"Follow"_q : u"Join"_q, [=] {
					Api::Instance().send(u"POST"_q, base + u"/join"_q, {},
						[=](const QJsonObject &) { reopen(); }, Toaster(controller));
				} });
			} else if (role == u"pending"_q) {
				links.push_back({ u"Cancel join request"_q, [=] {
					Api::Instance().send(u"DELETE"_q, base + u"/join"_q, {},
						[=](const QJsonObject &) { reopen(); }, Toaster(controller));
				} });
			} else if (role != u"owner"_q) {
				links.push_back({ isPage ? u"Unfollow"_q : u"Leave"_q, [=] {
					Api::Instance().send(u"DELETE"_q, base + u"/join"_q, {},
						[=](const QJsonObject &) { reopen(); }, Toaster(controller));
				} });
			}
			const auto telegram = c.value(u"telegram_link"_q).toString();
			if (!telegram.isEmpty()) {
				links.push_back({ u"Open group chat"_q, [=] {
					box->closeBox();
					UrlClickHandler::Open(telegram);
				} });
			}
			if (isMember && (!isPage || isStaff)) {
				links.push_back({ u"New post"_q, [=] {
					ShowComposeBox(controller, { .communityId = communityId });
				} });
			}
			AddLinks(container, std::move(links));
			auto more = std::vector<std::pair<QString, Fn<void()>>>();
			more.push_back({ isPage ? u"Followers"_q : u"Members"_q, [=] {
				ShowPeopleBox(controller, u"Members"_q, base + u"/members"_q);
			} });
			if (isStaff && !isPage) {
				more.push_back({ u"Join requests"_q, [=] {
					controller->show(Box([=](not_null<Ui::GenericBox*> inner) {
						inner->setTitle(rpl::single(u"Join requests"_q));
						const auto list = inner->verticalLayout()->add(
							object_ptr<Ui::VerticalLayout>(inner->verticalLayout()));
						const auto weak = QPointer<Ui::VerticalLayout>(list);
						Api::Instance().get(base + u"/members?role=pending"_q, [=](const QJsonObject &r) {
							const auto strong = weak.data();
							if (!strong) {
								return;
							}
							const auto members = r.value(u"members"_q).toArray();
							if (members.isEmpty()) {
								AddText(strong, u"No requests."_q, st::gbFeedSub);
							}
							for (const auto value : members) {
								const auto user = value.toObject();
								const auto id = IdOf(user.value(u"id"_q));
								AddPerson(strong, controller, user, QString());
								const auto path = base + u"/members/"_q + QString::number(id);
								AddLinks(strong, {
									{ u"Approve"_q, [=] {
										auto body = QJsonObject();
										body.insert(u"role"_q, u"member"_q);
										Api::Instance().send(u"POST"_q, path, body, [=](const QJsonObject &) {
											controller->showToast(u"Approved."_q);
										}, Toaster(controller));
									} },
									{ u"Decline"_q, [=] {
										Api::Instance().send(u"DELETE"_q, path, {}, [=](const QJsonObject &) {
											controller->showToast(u"Declined."_q);
										}, Toaster(controller));
									} },
								}, st::boxRowPadding + QMargins(st::gbFeedNameLeft, 0, 0, 0));
							}
							strong->resizeToWidth(strong->width());
						}, Toaster(controller));
						inner->addButton(rpl::single(u"Close"_q), [=] { inner->closeBox(); });
					}));
				} });
			}
			if (!isStaff) {
				more.push_back({ u"Report"_q, [=] {
					ShowReportBox(controller, u"community"_q, communityId);
				} });
			}
			AddLinks(container, std::move(more));
			Ui::AddSkip(container);
			Ui::AddDivider(container);
			container->add(object_ptr<PostList>(
				container,
				controller,
				base + u"/posts"_q,
				(c.value(u"privacy"_q).toString() == u"private"_q && !isMember)
					? u"Join to see the posts."_q
					: u"No posts yet."_q));
			box->addButton(rpl::single(u"Close"_q), [=] { box->closeBox(); });
		}));
	}, Toaster(controller));
}

void ShowCreateCommunityBox(Controller controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"New community"_q));
		const auto container = box->verticalLayout();
		const auto kind = box->lifetime().make_state<rpl::variable<int>>(0);
		const auto privacy = box->lifetime().make_state<rpl::variable<int>>(0);
		AddChoice(container, controller, u"Type"_q,
			{ u"Group (members post)"_q, u"Page (only admins post)"_q }, kind);
		AddChoice(container, controller, u"Who can join"_q,
			{ u"Anyone (public)"_q, u"Approved members (private)"_q }, privacy);
		Ui::AddSkip(container);
		const auto name = AddField(container, u"Name"_q, QString(), false, 80);
		const auto handle = AddField(container, u"Handle, e.g. linux_bd (optional)"_q, QString(), false, 32);
		const auto description = AddField(container, u"Description"_q, QString(), true, 1000);
		const auto rules = AddField(container, u"Rules (optional)"_q, QString(), true, 2000);
		const auto telegram = AddField(container, u"Telegram group link, https://t.me/... (optional)"_q);
		box->addButton(rpl::single(u"Create"_q), [=] {
			if (name->getLastText().trimmed().isEmpty()) {
				name->showError();
				return;
			}
			auto body = QJsonObject();
			body.insert(u"kind"_q, kind->current() ? u"page"_q : u"group"_q);
			body.insert(u"privacy"_q, privacy->current() ? u"private"_q : u"public"_q);
			body.insert(u"name"_q, name->getLastText().trimmed());
			body.insert(u"handle"_q, handle->getLastText().trimmed());
			body.insert(u"description"_q, description->getLastText().trimmed());
			body.insert(u"rules"_q, rules->getLastText().trimmed());
			body.insert(u"telegram_link"_q, telegram->getLastText().trimmed());
			const auto weak = QPointer<Ui::GenericBox>(box.get());
			Api::Instance().send(u"POST"_q, u"/v1/communities"_q, body, [=](const QJsonObject &r) {
				if (const auto strong = weak.data()) {
					strong->closeBox();
				}
				ShowCommunityBox(
					controller,
					IdOf(r.value(u"community"_q).toObject().value(u"id"_q)));
			}, Toaster(controller));
		});
		box->addButton(rpl::single(u"Cancel"_q), [=] { box->closeBox(); });
		box->setFocusCallback([=] { name->setFocusFast(); });
	}));
}

void ShowSearchBox(Controller controller, const QString &query) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(u"Search GB"_q));
		const auto container = box->verticalLayout();
		const auto field = AddField(container, u"People, communities, posts, file names or SHA-256"_q, query);
		const auto results = container->add(object_ptr<Ui::VerticalLayout>(container));
		const auto run = [=] {
			const auto q = field->getLastText().trimmed();
			results->clear();
			if (q.isEmpty()) {
				return;
			}
			AddText(results, u"Searching..."_q, st::gbFeedSub);
			const auto weak = QPointer<Ui::VerticalLayout>(results);
			Api::Instance().get(
				u"/v1/search?q="_q + QString::fromUtf8(QUrl::toPercentEncoding(q)),
				[=](const QJsonObject &r) {
					const auto strong = weak.data();
					if (!strong) {
						return;
					}
					strong->clear();
					const auto people = r.value(u"people"_q).toArray();
					const auto communities = r.value(u"communities"_q).toArray();
					const auto posts = r.value(u"posts"_q).toArray();
					if (people.isEmpty() && communities.isEmpty() && posts.isEmpty()) {
						AddText(strong, u"Nothing found."_q, st::gbFeedSub);
					}
					if (!people.isEmpty()) {
						Ui::AddSubsectionTitle(strong, rpl::single(u"People"_q));
						for (const auto value : people) {
							const auto user = value.toObject();
							const auto username = user.value(u"username"_q).toString();
							AddPerson(strong, controller, user, username.isEmpty() ? QString() : ('@' + username));
						}
					}
					if (!communities.isEmpty()) {
						Ui::AddSubsectionTitle(strong, rpl::single(u"Communities"_q));
						for (const auto value : communities) {
							const auto c = value.toObject();
							const auto id = IdOf(c.value(u"id"_q));
							const auto row = strong->add(object_ptr<PersonRow>(
								strong,
								QJsonObject{
									{ u"id"_q, double(id) },
									{ u"name"_q, c.value(u"name"_q).toString() },
									{ u"avatar"_q, c.value(u"avatar"_q) },
								},
								QString::number(c.value(u"member_count"_q).toInt()) + u" members"_q));
							row->setClickedCallback([=] { ShowCommunityBox(controller, id); });
						}
					}
					if (!posts.isEmpty()) {
						Ui::AddSubsectionTitle(strong, rpl::single(u"Posts"_q));
						for (const auto value : posts) {
							strong->add(object_ptr<PostCard>(strong, controller, value.toObject()));
						}
					}
					strong->resizeToWidth(strong->width());
				},
				Toaster(controller));
		};
		field->submits() | rpl::on_next([=] { run(); }, field->lifetime());
		run();
		box->addButton(rpl::single(u"Search"_q), run);
		box->addButton(rpl::single(u"Close"_q), [=] { box->closeBox(); });
		box->setFocusCallback([=] { field->setFocusFast(); });
	}));
}

void ShowReportBox(Controller controller, const QString &targetType, int64 targetId) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Report"_q));
		const auto container = box->verticalLayout();
		const auto reasons = std::vector<QString>{
			u"Spam"_q, u"Copyright violation"_q, u"Abuse or harassment"_q,
			u"Illegal content"_q, u"Other"_q };
		const auto reason = box->lifetime().make_state<rpl::variable<int>>(0);
		AddChoice(container, controller, u"Reason"_q, reasons, reason);
		Ui::AddSkip(container);
		const auto details = AddField(container, u"Details (optional)"_q, QString(), true, 900);
		box->addButton(rpl::single(u"Send report"_q), [=] {
			auto body = QJsonObject();
			body.insert(u"target_type"_q, targetType);
			body.insert(u"target_id"_q, double(targetId));
			auto text = reasons[reason->current()];
			const auto extra = details->getLastText().trimmed();
			if (!extra.isEmpty()) {
				text += u": "_q + extra;
			}
			body.insert(u"reason"_q, text);
			const auto weak = QPointer<Ui::GenericBox>(box.get());
			Api::Instance().send(u"POST"_q, u"/v1/reports"_q, body, [=](const QJsonObject &) {
				controller->showToast(u"Thanks. Our moderators will review it."_q);
				if (const auto strong = weak.data()) {
					strong->closeBox();
				}
			}, Toaster(controller));
		});
		box->addButton(rpl::single(u"Cancel"_q), [=] { box->closeBox(); });
	}));
}

void WatchFeed(Controller controller) {
	auto &api = Api::Instance();
	api.setAccount(&controller->session());
	struct State {
		int notifications = -1;
		base::flat_set<QString> offered;
	};
	const auto state = std::make_shared<State>();
	const auto weak = base::make_weak(controller);
	api.unreadValue() | rpl::on_next([=](const Unread &unread) {
		const auto strong = weak.get();
		if (!strong) {
			return;
		}
		if (state->notifications >= 0
			&& unread.notifications > state->notifications) {
			const auto added = unread.notifications - state->notifications;
			strong->showToast(u"GB Feed: %1 new notification%2."_q
				.arg(added)
				.arg(added == 1 ? QString() : u"s"_q));
		}
		state->notifications = unread.notifications;
		if (unread.approvedRequests <= 0) {
			return;
		}
		Api::Instance().get(u"/v1/requests?box=outgoing"_q, [=](const QJsonObject &r) {
			const auto strong = weak.get();
			if (!strong) {
				return;
			}
			auto &daemon = Transfers::Daemon::Instance();
			for (const auto value : r.value(u"requests"_q).toArray()) {
				const auto q = value.toObject();
				if (q.value(u"status"_q).toString() != u"approved"_q) {
					continue;
				}
				const auto invite = q.value(u"invite"_q).toString();
				const auto info = Transfers::ParseInvite(invite);
				if (!info
					|| state->offered.contains(invite)
					|| daemon.wasHandled(*info)
					|| daemon.isReceiving(*info)) {
					continue;
				}
				state->offered.emplace(invite);
				Transfers::ShowIncomingBox(
					strong,
					*info,
					NameOf(q.value(u"user"_q).toObject()) + u" (GB Feed)"_q,
					TimeId(IdOf(q.value(u"updated_at"_q))));
			}
		});
	}, controller->lifetime());
}

} // namespace GB::Feed
