/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/transfers/gb_tr_boxes.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/unixtime.h"
#include "core/file_utilities.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_thread.h"
#include "gb/downloader/gb_dl_common.h"
#include "gb/transfers/gb_tr_daemon.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "window/window_peer_menu.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_gb.h"
#include "styles/style_layers.h"

namespace GB::Transfers {
namespace {

// Offers older than this are not popped up again (the sender has most
// likely moved on); they can still be received from the Transfers page.
constexpr auto kIncomingMaxAge = TimeId(3 * 24 * 60 * 60);

[[nodiscard]] QString MessageText(
		const QString &invite,
		const QString &name,
		int64 size) {
	return u"\U0001F4E6 %1 (%2)\n"
		"Sent directly with GB: open this chat in GB to receive it.\n"
		"%3"_q.arg(name, Downloader::FormatSize(size), invite);
}

[[nodiscard]] QString RouteHint(const InviteInfo &info) {
	return info.lanOnly
		? u"This link works when both PCs are on the same Wi-Fi or LAN."_q
		: u"Same Wi-Fi goes at full LAN speed; otherwise it goes over the "
			"internet at the sender's upload speed."_q;
}

base::flat_set<not_null<Main::Session*>> &Watched() {
	static auto result = base::flat_set<not_null<Main::Session*>>();
	return result;
}

} // namespace

void SendInviteToChat(
		not_null<Window::SessionController*> controller,
		const QString &invite,
		const QString &name,
		int64 size,
		Fn<void(bool sent)> finished) {
	const auto sent = std::make_shared<bool>(false);
	const auto weak = base::make_weak(controller);
	const auto chosen = [=](not_null<Data::Thread*> thread) {
		auto message = Api::MessageToSend(Api::SendAction(thread));
		message.textWithTags = { MessageText(invite, name, size), {} };
		message.action.clearDraft = false;
		thread->session().api().sendMessage(std::move(message));
		*sent = true;
		if (const auto strong = weak.get()) {
			strong->showToast(u"Link sent. Keep GB running on this PC until "
				"the transfer finishes."_q);
		}
		return true;
	};
	const auto box = Window::ShowChooseRecipientBox(
		controller,
		chosen,
		rpl::single(u"Send \"%1\" to..."_q.arg(name)));
	if (const auto strong = box.get()) {
		strong->boxClosing() | rpl::on_next([=] {
			if (finished) {
				finished(*sent);
			}
		}, strong->lifetime());
	} else if (finished) {
		finished(false);
	}
}

void SendFile(not_null<Window::SessionController*> controller) {
	auto &daemon = Daemon::Instance();
	daemon.ensureStarted();
	if (daemon.status().engine != EngineState::Running) {
		controller->showToast(daemon.status().engineError.isEmpty()
			? u"The transfer engine is still starting, try again in a "
				"moment."_q
			: daemon.status().engineError);
		return;
	}
	const auto weak = base::make_weak(controller);
	FileDialog::GetOpenPath(
		QPointer<QWidget>(controller->widget().get()),
		u"Choose a file to send"_q,
		QString(),
		[=](FileDialog::OpenResult &&result) {
			if (result.paths.isEmpty()) {
				return;
			}
			const auto path = result.paths.front();
			Daemon::Instance().createOffer(path, [=](SendRow offer) {
				const auto strong = weak.get();
				if (!strong) {
					return;
				}
				if (Daemon::Instance().status().networkState
						!= u"running"_q) {
					strong->showToast(u"Internet transfers are off: the "
						"receiver must be on the same Wi-Fi or LAN."_q);
				}
				const auto id = offer.id;
				SendInviteToChat(
					strong,
					offer.invite,
					offer.name,
					offer.size,
					[=](bool sent) {
						if (!sent) {
							Daemon::Instance().deleteOffer(id);
						}
					});
			}, [=](QString error) {
				if (const auto strong = weak.get()) {
					strong->showToast(u"Could not offer the file: "_q + error);
				}
			});
		});
}

void Receive(
		not_null<Window::SessionController*> controller,
		const QString &link) {
	const auto weak = base::make_weak(controller);
	auto &daemon = Daemon::Instance();
	daemon.ensureStarted();
	daemon.receive(link, QString(), [=] {
		if (const auto strong = weak.get()) {
			strong->showToast(u"Receiving. Progress is on the Transfers "
				"page."_q);
		}
	}, [=](QString error) {
		if (const auto strong = weak.get()) {
			strong->showToast(u"Could not receive: "_q + error);
		}
	});
}

void ShowReceiveLinkBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Receive a file"_q));
		box->setWidth(st::boxWideWidth);
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Paste the gb://transfer/ link"_q)));
		field->setMaxHeight(st::boxWideWidth / 2);
		const auto about = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"The sender gets this link when they send a file with GB."_q,
			st::gbDlBoxAbout));
		const auto submit = [=] {
			const auto info = ParseInvite(field->getLastText());
			if (!info) {
				field->showError();
				about->setText(u"That is not a GB transfer link."_q);
				return;
			}
			Receive(controller, info->link);
			box->closeBox();
		};
		field->submits() | rpl::on_next(submit, field->lifetime());
		box->addButton(rpl::single(u"Receive"_q), submit);
		box->addButton(rpl::single(u"Cancel"_q), [=] { box->closeBox(); });
		box->setFocusCallback([=] { field->setFocusFast(); });
	}));
}

void ShowIncomingBox(
		not_null<Window::SessionController*> controller,
		const InviteInfo &info,
		const QString &from,
		TimeId date) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Incoming file"_q));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"%1 wants to send you:"_q.arg(from),
			st::boxLabel));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			u"%1 (%2)"_q.arg(info.name, Downloader::FormatSize(info.size)),
			st::gbDlBoxTitle));
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			RouteHint(info),
			st::gbDlBoxAbout));
		const auto answered = std::make_shared<bool>(false);
		box->addButton(rpl::single(u"Receive"_q), [=] {
			*answered = true;
			Receive(controller, info.link);
			box->closeBox();
		});
		box->addButton(rpl::single(u"Later"_q), [=] {
			*answered = true;
			Daemon::Instance().addPending(info.link, from, date);
			Daemon::Instance().markHandled(info);
			box->closeBox();
		});
		box->addLeftButton(rpl::single(u"Decline"_q), [=] {
			*answered = true;
			Daemon::Instance().markHandled(info);
			box->closeBox();
		});
		box->boxClosing() | rpl::on_next([=] {
			if (!*answered) {
				// Closed with Escape or a click outside: keep it for later.
				Daemon::Instance().addPending(info.link, from, date);
				Daemon::Instance().markHandled(info);
			}
		}, box->lifetime());
	}));
}

void WatchIncoming(not_null<Window::SessionController*> controller) {
	Daemon::Instance().ensureStarted();
	const auto session = &controller->session();
	if (Watched().contains(session)) {
		return;
	}
	Watched().emplace(session);
	controller->lifetime().add([=] {
		Watched().remove(session);
	});
	session->data().newItemAdded(
	) | rpl::filter([](not_null<HistoryItem*> item) {
		return !item->out()
			&& item->originalText().text.contains(
				QString::fromLatin1(kInvitePrefix));
	}) | rpl::on_next([=](not_null<HistoryItem*> item) {
		const auto info = ParseInvite(item->originalText().text);
		if (!info) {
			return;
		}
		auto &daemon = Daemon::Instance();
		if (daemon.wasHandled(*info) || daemon.isReceiving(*info)) {
			return;
		}
		const auto from = item->from()->name();
		const auto date = item->date();
		if (base::unixtime::now() - date > kIncomingMaxAge) {
			daemon.addPending(info->link, from, date);
			daemon.markHandled(*info);
			return;
		}
		ShowIncomingBox(controller, *info, from, date);
	}, controller->lifetime());
}

} // namespace GB::Transfers
