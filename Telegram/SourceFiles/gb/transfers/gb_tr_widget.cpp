/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/transfers/gb_tr_widget.h"

#include "base/unixtime.h"
#include "core/file_utilities.h"
#include "gb/downloader/gb_dl_common.h"
#include "gb/downloader/gb_dl_settings.h"
#include "gb/transfers/gb_tr_boxes.h"
#include "gb/transfers/gb_tr_daemon.h"
#include "layout/layout_document_generic_preview.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/discrete_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/scroll_area.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"
#include "styles/style_chat_style.h"
#include "styles/style_dialogs.h"
#include "styles/style_gb.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_overview.h"
#include "styles/style_widgets.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtGui/QClipboard>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>

namespace GB::Transfers {
namespace {

using Downloader::FormatEta;
using Downloader::FormatSize;
using Downloader::FormatSpeed;

[[nodiscard]] QString Join(QStringList parts) {
	parts.removeAll(QString());
	return parts.join(u" • "_q);
}

[[nodiscard]] QString OfTotal(int64 received, int64 size) {
	return FormatSize(received) + u" of "_q + FormatSize(size);
}

[[nodiscard]] QString ExtensionOf(const QString &name) {
	return QFileInfo(name).suffix().left(4).toUpper();
}

} // namespace

List::List(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: RpWidget(parent)
, _controller(controller) {
	setMouseTracking(true);
	Daemon::Instance().updates() | rpl::on_next([=] {
		refresh();
	}, lifetime());
	refresh();
}

void List::setTab(Tab tab) {
	_tab = tab;
	refresh();
}

void List::refresh() {
	auto &daemon = Daemon::Instance();
	_rows.clear();
	if (_tab == Tab::Receiving) {
		for (const auto &pending : daemon.pending()) {
			const auto info = ParseInvite(pending.link);
			if (!info) {
				continue;
			}
			_rows.push_back({
				.kind = Kind::Pending,
				.key = pending.link,
				.name = info->name,
				.status = Join({
					FormatSize(info->size),
					u"from %1, waiting for you"_q.arg(pending.from),
				}),
				.info = Join({
					Downloader::FormatDate(
						base::unixtime::parse(pending.date)),
					info->lanOnly ? u"same Wi-Fi only"_q : QString(),
				}),
				.circle = Circle::Start,
			});
		}
		for (const auto &row : daemon.receiving()) {
			auto status = QString();
			auto circle = Circle::None;
			auto error = false;
			auto active = false;
			if (row.state == u"running"_q) {
				active = true;
				circle = Circle::Stop;
				status = Join({
					u"Receiving"_q,
					OfTotal(row.received, row.size),
					(row.speed > 0.) ? FormatSpeed(row.speed) : QString(),
					(row.eta > 0.)
						? (FormatEta(int64(row.eta)) + u" left"_q)
						: QString(),
					FormatRoute(row.route),
				});
			} else if (row.state == u"queued"_q) {
				active = true;
				circle = Circle::Stop;
				status = u"Connecting to the sender..."_q;
			} else if (row.state == u"waiting"_q) {
				circle = Circle::Stop;
				error = true;
				status = Join({
					u"Waiting for the sender"_q,
					OfTotal(row.received, row.size),
					row.error,
				});
			} else if (row.state == u"paused"_q) {
				circle = Circle::Start;
				status = Join({
					u"Paused"_q,
					OfTotal(row.received, row.size),
				});
			} else if (row.state == u"completed"_q) {
				status = Join({ FormatSize(row.size), u"Received"_q });
			} else {
				circle = Circle::Start;
				error = true;
				status = u"Failed: "_q + row.error;
			}
			_rows.push_back({
				.kind = Kind::Receive,
				.key = row.id,
				.name = row.name,
				.status = status,
				.info = QDir::toNativeSeparators(row.dir),
				.path = row.finalPath,
				.progress = row.progress,
				.circle = circle,
				.error = error,
				.active = active,
			});
		}
	} else {
		for (const auto &row : daemon.sending()) {
			_rows.push_back({
				.kind = Kind::Send,
				.key = row.id,
				.name = row.name,
				.status = Join({
					FormatSize(row.size),
					(row.speed > 0.)
						? (u"Sending "_q + FormatSpeed(row.speed))
						: u"Shared, waiting for the receiver"_q,
					(row.served > 0)
						? (FormatSize(row.served) + u" sent"_q)
						: QString(),
				}),
				.info = QDir::toNativeSeparators(row.path),
				.path = row.path,
				.active = (row.speed > 0.),
			});
		}
	}
	const auto still = ranges::any_of(_rows, [&](const Row &row) {
		return row.key == _selected;
	});
	if (!still) {
		_selected = QString();
	}
	_over = -1;
	_overCircle = false;
	resizeToWidth(width());
	update();
}

int List::rowHeight() const {
	const auto &padding = st::gbDlRowPadding;
	return padding.top() + st::gbDlThumbSize + padding.bottom();
}

int List::resizeGetHeight(int newWidth) {
	return int(_rows.size()) * rowHeight();
}

int List::rowIndexAt(QPoint position) const {
	if (position.y() < 0 || position.x() < 0 || position.x() >= width()) {
		return -1;
	}
	const auto index = position.y() / rowHeight();
	return (index < int(_rows.size())) ? index : -1;
}

QRect List::circleRect(int top) const {
	const auto &padding = st::gbDlRowPadding;
	const auto size = st::gbDlInnerSize;
	return QRect(
		padding.left() + (st::gbDlThumbSize - size) / 2,
		top + padding.top() + (st::gbDlThumbSize - size) / 2,
		size,
		size);
}

void List::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto clip = e->rect();
	p.fillRect(clip, st::windowBg);
	const auto height = rowHeight();
	const auto from = std::max(0, clip.top() / height);
	const auto till = std::min(int(_rows.size()), (clip.bottom() / height) + 1);
	for (auto i = from; i < till; ++i) {
		paintRow(p, _rows[i], i * height, (i == _over));
	}
}

void List::paintRow(Painter &p, const Row &row, int top, bool over) {
	const auto height = rowHeight();
	const auto selected = (_selected == row.key);
	if (selected || over) {
		p.fillRect(0, top, width(), height, selected
			? st::dialogsBgOver
			: st::windowBgOver);
	}
	const auto &padding = st::gbDlRowPadding;
	const auto preview = Layout::DocumentGenericPreview::Create(
		u"file."_q + ExtensionOf(row.name).toLower());
	const auto thumb = QRect(
		padding.left(),
		top + padding.top(),
		st::gbDlThumbSize,
		st::gbDlThumbSize);
	{
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(preview.color);
		p.drawRoundedRect(thumb, st::roundRadiusSmall, st::roundRadiusSmall);
	}
	if (row.circle != Circle::None) {
		const auto inner = circleRect(top);
		{
			auto hq = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush((over && _overCircle) ? preview.over : preview.dark);
			p.drawEllipse(inner);
		}
		const auto &icon = (row.circle == Circle::Stop)
			? st::historyFileThumbCancel
			: st::historyFileThumbDownload;
		icon.paintInCenter(p, inner);
		if (row.circle == Circle::Stop) {
			auto hq = PainterHighQualityEnabler(p);
			const auto line = st::gbDlRadialLine;
			auto pen = st::historyFileThumbRadialFg->p;
			pen.setWidth(line);
			pen.setCapStyle(Qt::RoundCap);
			p.setPen(pen);
			p.setBrush(Qt::NoBrush);
			const auto arc = QRectF(inner).marginsRemoved(
				QMarginsF(line / 2., line / 2., line / 2., line / 2.));
			const auto value = std::clamp(row.progress, 0.03, 1.);
			p.drawArc(arc, 90 * 16, -int(value * 360 * 16));
		}
	} else {
		p.setFont(st::gbDlExtFont);
		p.setPen(st::overviewFileExtFg);
		p.drawText(
			thumb,
			Qt::AlignCenter,
			st::gbDlExtFont->elided(
				ExtensionOf(row.name),
				st::gbDlThumbSize - 8));
	}

	const auto left = thumb.x() + thumb.width() + st::gbDlNameLeft;
	const auto available = width() - left - padding.right();
	if (available <= 0) {
		return;
	}
	p.setFont(st::semiboldFont);
	p.setPen(st::windowBoldFg);
	p.drawTextLeft(
		left,
		top + st::gbDlNameTop,
		width(),
		st::semiboldFont->elided(row.name, available));

	p.setFont(st::normalFont);
	p.setPen(row.error
		? st::boxTextFgError
		: row.active
		? st::windowActiveTextFg
		: st::windowSubTextFg);
	p.drawTextLeft(
		left,
		top + st::gbDlStatusTop,
		width(),
		st::normalFont->elided(row.status, available));

	p.setPen(st::windowSubTextFg);
	p.drawTextLeft(
		left,
		top + st::gbDlInfoTop,
		width(),
		st::normalFont->elided(row.info, available));

	if (!selected) {
		p.fillRect(
			left,
			top + height - st::lineWidth,
			width() - left,
			st::lineWidth,
			st::shadowFg);
	}
}

void List::updateOver(QPoint position) {
	const auto index = rowIndexAt(position);
	const auto overCircle = (index >= 0)
		&& (_rows[index].circle != Circle::None)
		&& circleRect(index * rowHeight()).contains(position);
	if (index != _over || overCircle != _overCircle) {
		if (_over >= 0) {
			update(0, _over * rowHeight(), width(), rowHeight());
		}
		_over = index;
		_overCircle = overCircle;
		if (_over >= 0) {
			update(0, _over * rowHeight(), width(), rowHeight());
		}
		setCursor(_overCircle ? style::cur_pointer : style::cur_default);
	}
}

void List::mouseMoveEvent(QMouseEvent *e) {
	updateOver(e->pos());
}

void List::leaveEventHook(QEvent *e) {
	updateOver(QPoint(-1, -1));
}

void List::mousePressEvent(QMouseEvent *e) {
	updateOver(e->pos());
	_pressed = (e->button() == Qt::LeftButton) ? _over : -1;
	if (_over >= 0) {
		_selected = _rows[_over].key;
		update();
	}
}

void List::mouseReleaseEvent(QMouseEvent *e) {
	const auto pressed = base::take(_pressed);
	updateOver(e->pos());
	if (pressed >= 0 && pressed == _over && _overCircle) {
		const auto row = _rows[pressed];
		clickCircle(row);
	}
}

void List::mouseDoubleClickEvent(QMouseEvent *e) {
	updateOver(e->pos());
	if (_over >= 0 && !_overCircle) {
		const auto row = _rows[_over];
		openRow(row);
	}
}

void List::contextMenuEvent(QContextMenuEvent *e) {
	const auto position = (e->reason() == QContextMenuEvent::Mouse)
		? e->pos()
		: QPoint(0, 0);
	const auto index = rowIndexAt(position);
	if (index < 0) {
		return;
	}
	_selected = _rows[index].key;
	update();
	const auto row = _rows[index];
	showMenu(row, e->globalPos());
	e->accept();
}

void List::clickCircle(const Row &row) {
	auto &daemon = Daemon::Instance();
	if (row.kind == Kind::Pending) {
		Receive(_controller, row.key);
	} else if (row.circle == Circle::Stop) {
		daemon.pause(row.key);
	} else if (row.circle == Circle::Start) {
		daemon.resume(row.key);
	}
}

void List::openRow(const Row &row) {
	if (row.kind == Kind::Pending) {
		Receive(_controller, row.key);
	} else if (!row.path.isEmpty() && QFileInfo::exists(row.path)) {
		if (row.kind == Kind::Send) {
			File::ShowInFolder(row.path);
		} else {
			File::Launch(row.path);
		}
	} else if (row.circle == Circle::Start) {
		Daemon::Instance().resume(row.key);
	}
}

void List::showMenu(const Row &row, QPoint globalPosition) {
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	const auto menu = _menu.get();
	const auto controller = _controller;
	const auto key = row.key;
	const auto path = row.path;
	const auto name = row.name;
	switch (row.kind) {
	case Kind::Pending:
		menu->addAction(u"Receive"_q, [=] {
			Receive(controller, key);
		}, &st::menuIconDownload);
		menu->addAction(u"Copy link"_q, [=] {
			QGuiApplication::clipboard()->setText(key);
			controller->showToast(u"Link copied."_q);
		}, &st::menuIconLink);
		menu->addSeparator();
		menu->addAction(u"Decline"_q, [=] {
			Daemon::Instance().removePending(key);
		}, &st::menuIconRemove);
		break;
	case Kind::Receive: {
		const auto exists = !path.isEmpty() && QFileInfo::exists(path);
		if (exists) {
			menu->addAction(u"Open"_q, [=] {
				File::Launch(path);
			}, &st::menuIconFile);
			menu->addAction(u"Show in folder"_q, [=] {
				File::ShowInFolder(path);
			}, &st::menuIconShowInFolder);
		}
		if (row.circle == Circle::Stop) {
			menu->addAction(u"Pause"_q, [=] {
				Daemon::Instance().pause(key);
			}, &st::menuIconCancel);
		} else if (row.circle == Circle::Start) {
			menu->addAction(u"Resume"_q, [=] {
				Daemon::Instance().resume(key);
			}, &st::menuIconDownload);
		}
		menu->addSeparator();
		menu->addAction(u"Remove from list"_q, [=] {
			Daemon::Instance().remove(key, false);
		}, &st::menuIconRemove);
		menu->addAction(exists ? u"Delete file"_q : u"Cancel and delete"_q, [=] {
			controller->show(Ui::MakeConfirmBox({
				.text = u"Delete \"%1\" from the disk? This cannot be "
					"undone."_q.arg(name),
				.confirmed = [=](Fn<void()> close) {
					Daemon::Instance().remove(key, true);
					close();
				},
				.confirmText = rpl::single(u"Delete"_q),
				.confirmStyle = &st::attentionBoxButton,
			}));
		}, &st::menuIconDelete);
	} break;
	case Kind::Send: {
		const auto rows = Daemon::Instance().sending();
		const auto i = ranges::find(rows, key, &SendRow::id);
		if (i == end(rows)) {
			return;
		}
		const auto invite = i->invite;
		const auto size = i->size;
		menu->addAction(u"Send link to a chat"_q, [=] {
			SendInviteToChat(controller, invite, name, size);
		}, &st::menuIconForward);
		menu->addAction(u"Copy link"_q, [=] {
			QGuiApplication::clipboard()->setText(invite);
			controller->showToast(u"Link copied."_q);
		}, &st::menuIconLink);
		menu->addAction(u"Show in folder"_q, [=] {
			File::ShowInFolder(path);
		}, &st::menuIconShowInFolder);
		menu->addSeparator();
		menu->addAction(u"Stop sharing"_q, [=] {
			Daemon::Instance().deleteOffer(key);
		}, &st::menuIconRemove);
	} break;
	}
	menu->popup(globalPosition);
}

Widget::Widget(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: RpWidget(parent)
, _controller(controller)
, _send(this, rpl::single(u"Send a file"_q), st::defaultActiveButton)
, _receive(this, rpl::single(u"Paste a link"_q), st::defaultLightButton)
, _menuToggle(this, st::menuToggle)
, _status(this, QString(), st::gbDlSummary)
, _signIn(this, u"Sign in to connect"_q)
, _firewall(this, u"Allow in firewall"_q)
, _tabs(this, st::defaultTabsSlider)
, _scroll(this)
, _empty(this, QString(), st::gbDlEmpty) {
	_send->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	_receive->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	_send->setClickedCallback([=] { SendFile(_controller); });
	_receive->setClickedCallback([=] { ShowReceiveLinkBox(_controller); });
	_menuToggle->setClickedCallback([=] { showMainMenu(); });
	_signIn->setClickedCallback([=] {
		const auto url = Daemon::Instance().status().loginUrl;
		if (!url.isEmpty()) {
			QGuiApplication::clipboard()->setText(url);
			File::OpenUrl(url);
			_controller->showToast(u"Sign-in link opened and copied. On a "
				"friend's PC, send this link to the account owner: they "
				"open it and click Connect."_q);
		}
	});
	_signIn->hide();
	_firewall->setClickedCallback([=] {
		ShowFirewallBox(_controller);
	});
	_firewall->hide();

	_tabs->setSections({ u"Receiving"_q, u"Sending"_q });
	_tabs->setActiveSectionFast(0);
	_tabs->sectionActivated() | rpl::on_next([=](int index) {
		_tab = index ? Tab::Sending : Tab::Receiving;
		if (_list) {
			_list->setTab(_tab);
		}
		updateEmpty();
	}, _tabs->lifetime());

	_list = _scroll->setOwnedWidget(object_ptr<List>(this, controller));
	_empty->setAttribute(Qt::WA_TransparentForMouseEvents);

	auto &daemon = Daemon::Instance();
	daemon.setWatching(true);
	daemon.updates() | rpl::on_next([=] {
		updateStatus();
		updateEmpty();
	}, lifetime());
	updateStatus();
	updateEmpty();
}

Widget::~Widget() {
	Daemon::Instance().setWatching(false);
}

void Widget::setInnerFocus() {
	setFocus();
}

void Widget::showMainMenu() {
	_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
	const auto menu = _menu.get();
	const auto controller = _controller;
	auto &daemon = Daemon::Instance();
	menu->addAction(u"Open received files folder"_q, [=] {
		auto folder = Daemon::Instance().status().downloadDir;
		if (folder.isEmpty()) {
			folder = Downloader::DefaultDownloadDir();
		}
		QDir().mkpath(folder);
		File::Launch(folder);
	}, &st::menuIconShowInFolder);
	const auto internet = daemon.internetEnabled();
	menu->addAction(internet
		? u"Turn off internet transfers"_q
		: u"Turn on internet transfers (test mode)"_q, [=] {
		if (internet) {
			Daemon::Instance().setInternetEnabled(false);
			return;
		}
		controller->show(Ui::MakeConfirmBox({
			.text = u"Until GB's own network server is ready, internet "
				"transfers use a Tailscale account in test mode. GB will "
				"show a sign-in link: both PCs must sign in with the SAME "
				"Tailscale account.\n\nSame Wi-Fi transfers work without "
				"this."_q,
			.confirmed = [=](Fn<void()> close) {
				Daemon::Instance().setInternetEnabled(true);
				close();
			},
			.confirmText = rpl::single(u"Turn on"_q),
		}));
	}, &st::menuIconSettings);
	menu->addAction(daemon.hasJoinKey()
		? u"Change join key..."_q
		: u"Use a join key..."_q, [=] {
		showJoinKeyBox();
	}, &st::menuIconLock);
	const auto url = daemon.status().loginUrl;
	if (!url.isEmpty()
		&& daemon.status().networkState == u"needs_login"_q) {
		menu->addAction(u"Copy sign-in link"_q, [=] {
			QGuiApplication::clipboard()->setText(url);
			controller->showToast(u"Sign-in link copied."_q);
		}, &st::menuIconCopy);
	}
	if (daemon.status().firewall != FirewallState::Ok) {
		menu->addAction(u"Allow in Windows Firewall"_q, [=] {
			ShowFirewallBox(controller);
		}, &st::menuIconPermissions);
	}
	menu->addAction(u"Restart transfer engine"_q, [=] {
		Daemon::Instance().restart();
	}, &st::menuIconRestore);
	menu->popup(QCursor::pos());
}

void Widget::showJoinKeyBox() {
	const auto controller = _controller;
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Join key"_q));
		box->setWidth(st::boxWideWidth);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"Test mode: paste a reusable Tailscale auth key "
				"(tskey-auth-...) from the account owner's admin page "
				"(Settings, Keys). This PC then joins internet transfers "
				"without a browser sign-in. Leave empty to use the "
				"sign-in link instead. Keep the key secret."_q,
			st::boxLabel));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"tskey-auth-..."_q),
			QString()));
		const auto submit = [=] {
			const auto key = field->getLastText().trimmed();
			if (!key.isEmpty() && !key.startsWith(u"tskey-"_q)) {
				field->showError();
				return;
			}
			auto &daemon = Daemon::Instance();
			daemon.setJoinKey(key);
			if (!key.isEmpty() && !daemon.internetEnabled()) {
				daemon.setInternetEnabled(true);
			}
			box->closeBox();
		};
		field->submits() | rpl::on_next(submit, field->lifetime());
		box->addButton(rpl::single(u"Save"_q), submit);
		box->addButton(rpl::single(u"Cancel"_q), [=] { box->closeBox(); });
		box->setFocusCallback([=] { field->setFocusFast(); });
	}));
}

void Widget::updateStatus() {
	const auto &status = Daemon::Instance().status();
	auto text = QString();
	auto signIn = false;
	auto showFirewall = false;
	switch (status.engine) {
	case EngineState::Missing:
	case EngineState::Failed:
		text = status.engineError;
		break;
	case EngineState::Starting:
		text = u"Starting the transfer engine..."_q;
		break;
	case EngineState::Running: {
		const auto lan = status.lanAddrs.isEmpty()
			? u"Same Wi-Fi: no local network"_q
			: (u"Same Wi-Fi: ready ("_q
				+ status.lanAddrs.front().section(':', 0, 0)
				+ ')');
		auto internet = QString();
		const auto &state = status.networkState;
		if (state == u"running"_q) {
			internet = u"Internet: connected"_q;
		} else if (state == u"connecting"_q) {
			internet = u"Internet: connecting..."_q;
		} else if (state == u"needs_login"_q) {
			internet = u"Internet: sign-in needed"_q;
			signIn = !status.loginUrl.isEmpty();
		} else if (state == u"failed"_q) {
			internet = u"Internet: failed ("_q + status.networkError + ')';
		} else {
			internet = u"Internet: off"_q;
		}
		auto firewall = QString();
		if (status.firewall == FirewallState::Missing) {
			firewall = u"Firewall: blocked"_q;
			showFirewall = true;
		} else if (status.firewall == FirewallState::Fixing) {
			firewall = u"Firewall: waiting for permission..."_q;
		}
		text = Join({ lan, internet, firewall });
	} break;
	}
	_status->setText(text);
	if (_signIn->isHidden() == signIn
		|| _firewall->isHidden() == showFirewall) {
		_signIn->setVisible(signIn);
		_firewall->setVisible(showFirewall);
		resizeEvent(nullptr);
	}
}

void Widget::updateEmpty() {
	if (!_list) {
		return;
	}
	_empty->setText((_tab == Tab::Receiving)
		? u"Nothing received yet.\nWhen someone sends you a file with GB, "
			"it appears here. Same Wi-Fi goes at full LAN speed."_q
		: u"Nothing shared yet.\nPress \"Send a file\" to send any size "
			"of file directly to a chat."_q);
	_empty->setVisible(_list->empty());
	resizeEvent(nullptr);
}

void Widget::resizeEvent(QResizeEvent *e) {
	const auto &padding = st::gbDlPadding;
	const auto available = width() - padding.left() - padding.right();
	const auto contentWidth = std::max(
		std::min(available, st::gbDlMaxWidth),
		0);
	const auto left = (width() - contentWidth) / 2;
	auto top = padding.top();

	const auto rowHeight = std::max(_send->height(), _menuToggle->height());
	_send->moveToLeft(left, top + (rowHeight - _send->height()) / 2);
	_receive->moveToLeft(
		left + _send->width() + st::gbDlFieldSkip,
		top + (rowHeight - _receive->height()) / 2);
	_menuToggle->moveToLeft(
		left + contentWidth - _menuToggle->width(),
		top + (rowHeight - _menuToggle->height()) / 2);
	top += rowHeight + st::gbDlTopSkip;

	auto statusWidth = contentWidth;
	auto linksRight = left + contentWidth;
	for (const auto link : { _signIn.data(), _firewall.data() }) {
		if (!link->isHidden()) {
			linksRight -= link->width();
			link->moveToLeft(linksRight, top);
			linksRight -= st::gbDlFieldSkip;
			statusWidth -= link->width() + st::gbDlFieldSkip;
		}
	}
	_status->resizeToWidth(std::max(statusWidth, 0));
	_status->moveToLeft(left, top);
	top += std::max(_status->height(), st::gbDlSummaryHeight);

	_tabs->resizeToWidth(contentWidth);
	_tabs->moveToLeft(left, top);
	top += _tabs->height() + st::gbDlTabsSkip;

	_scroll->setGeometry(left, top, contentWidth, std::max(height() - top, 0));
	if (_list) {
		_list->resizeToWidth(contentWidth);
	}
	_empty->resizeToWidth(contentWidth);
	_empty->moveToLeft(left, top + st::gbDlEmptyTop);
}

void Widget::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	p.fillRect(e->rect(), st::windowBg);
}

} // namespace GB::Transfers
