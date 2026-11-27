/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/feed/gb_feed_reactions.h"

#include "base/timer.h"
#include "data/data_message_reactions.h"
#include "data/data_session.h"
#include "history/view/reactions/history_view_reactions_strip.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "ui/effects/animations.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "window/main_window.h"
#include "window/window_session_controller.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_widgets.h"

namespace GB::Feed {
namespace {

using HistoryView::Reactions::Strip;

constexpr auto kShowDelay = crl::time(450);
constexpr auto kHideDelay = crl::time(300);
constexpr auto kCollapsedCount = 7;
constexpr auto kExpandedColumns = 8;
constexpr auto kTitleSpace = 30;
constexpr auto kShadow = 4;

class Pill final : public Ui::RpWidget {
public:
	Pill(QWidget *parent, not_null<Main::Session*> session);

	void showFor(
		QRect target,
		not_null<QWidget*> owner,
		Fn<void(QString)> chosen);
	void scheduleHide();
	void cancelHide(not_null<QWidget*> owner);
	void hidePill();

	[[nodiscard]] Main::Session *session() const {
		return _session;
	}

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void enterEventHook(QEnterEvent *e) override;
	void leaveEventHook(QEvent *e) override;

private:
	struct Row {
		std::unique_ptr<Strip> strip;
		std::vector<QString> titles;
	};
	struct Selection {
		int row = -1;
		int index = -1;
		friend inline bool operator==(Selection, Selection) = default;
	};

	void fill();
	void place();
	void select(Selection selection);
	void choose(Selection selection);
	[[nodiscard]] Selection lookup(QPoint position) const;
	[[nodiscard]] QRect pillRect() const;
	[[nodiscard]] int columns() const;

	const not_null<Main::Session*> _session;
	std::vector<Row> _rows;
	QRect _target;
	QPointer<QWidget> _owner;
	Fn<void(QString)> _chosen;
	base::Timer _hideTimer;
	Ui::Animations::Simple _shownAnimation;
	Selection _selected;
	Selection _pressed;
	bool _expanded = false;
	bool _shown = false;
	bool _hovered = false;

};

Pill::Pill(QWidget *parent, not_null<Main::Session*> session)
: RpWidget(parent)
, _session(session)
, _hideTimer([=] {
	if (!_hovered) {
		hidePill();
	}
}) {
	setMouseTracking(true);
	hide();
	_session->data().reactions().refreshDefault();
}

int Pill::columns() const {
	return _expanded ? kExpandedColumns : kCollapsedCount + 1;
}

void Pill::fill() {
	_rows.clear();
	const auto &all = _session->data().reactions().list(
		Data::Reactions::Type::Active);
	auto list = std::vector<not_null<const Data::Reaction*>>();
	for (const auto &reaction : all) {
		if (!reaction.premium && !reaction.id.emoji().isEmpty()) {
			list.push_back(&reaction);
		}
	}
	const auto perRow = _expanded ? kExpandedColumns : kCollapsedCount;
	const auto total = _expanded
		? int(list.size())
		: std::min(int(list.size()), kCollapsedCount);
	for (auto from = 0; from < total; from += perRow) {
		const auto till = std::min(from + perRow, total);
		auto row = Row{
			.strip = std::make_unique<Strip>(
				st::reactPanelEmojiPan,
				QRect(0, 0, st::reactStripSize, st::reactStripSize),
				st::reactStripImage,
				crl::guard(this, [=] { update(); })),
		};
		auto part = std::vector<not_null<const Data::Reaction*>>(
			list.begin() + from,
			list.begin() + till);
		for (const auto reaction : part) {
			row.titles.push_back(reaction->title);
		}
		const auto more = !_expanded && (int(list.size()) > total);
		row.strip->applyList(
			part,
			more ? Strip::AddedButton::Expand : Strip::AddedButton::None);
		row.strip->clearAppearAnimations(false);
		_rows.push_back(std::move(row));
	}
}

QRect Pill::pillRect() const {
	const auto rows = std::max(int(_rows.size()), 1);
	const auto skipy = (st::reactStripHeight - st::reactStripSize) / 2;
	const auto count = _rows.empty() ? 0 : _rows.front().strip->count();
	const auto perRow = _expanded ? kExpandedColumns : count;
	return QRect(
		kShadow,
		kTitleSpace,
		2 * st::reactStripSkip + perRow * st::reactStripSize,
		2 * skipy + rows * st::reactStripSize);
}

void Pill::place() {
	const auto inner = pillRect();
	const auto size = QSize(
		inner.width() + 2 * kShadow,
		inner.y() + inner.height() + kShadow);
	const auto parent = parentWidget();
	auto x = _target.x() - kShadow;
	auto y = _target.y() - size.height() + kShadow;
	if (y < 0) {
		y = _target.y() + _target.height() - kTitleSpace + kShadow;
	}
	x = std::clamp(x, 0, std::max(parent->width() - size.width(), 0));
	setGeometry(QRect(QPoint(x, std::max(y, 0)), size));
}

void Pill::showFor(
		QRect target,
		not_null<QWidget*> owner,
		Fn<void(QString)> chosen) {
	_hideTimer.cancel();
	if (_shown && _owner == owner) {
		return;
	}
	_owner = owner.get();
	_chosen = std::move(chosen);
	_target = target;
	_expanded = false;
	_selected = _pressed = {};
	fill();
	if (_rows.empty()) {
		return; // Reactions are not loaded yet.
	}
	place();
	_shown = true;
	show();
	raise();
	_shownAnimation.start([=] { update(); }, 0., 1., st::defaultPopupMenu.showDuration);
}

void Pill::scheduleHide() {
	_hideTimer.callOnce(kHideDelay);
}

void Pill::cancelHide(not_null<QWidget*> owner) {
	if (_owner == owner) {
		_hideTimer.cancel();
	}
}

void Pill::hidePill() {
	if (!_shown) {
		return;
	}
	_shown = false;
	select({});
	_shownAnimation.start([=] {
		update();
		if (!_shown && !_shownAnimation.animating()) {
			hide();
		}
	}, 1., 0., st::defaultPopupMenu.showDuration);
}

Pill::Selection Pill::lookup(QPoint position) const {
	const auto inner = pillRect();
	const auto skipy = (st::reactStripHeight - st::reactStripSize) / 2;
	const auto p = position
		- inner.topLeft()
		- QPoint(st::reactStripSkip, skipy);
	if (p.x() < 0 || p.y() < 0) {
		return {};
	}
	const auto row = p.y() / st::reactStripSize;
	const auto index = p.x() / st::reactStripSize;
	if (row >= int(_rows.size()) || index >= _rows[row].strip->count()) {
		return {};
	}
	return { row, index };
}

void Pill::select(Selection selection) {
	if (_selected == selection) {
		return;
	}
	for (auto i = 0; i != int(_rows.size()); ++i) {
		_rows[i].strip->setSelected(
			(i == selection.row) ? selection.index : -1);
	}
	_selected = selection;
	setCursor((selection.row >= 0) ? style::cur_pointer : style::cur_default);
	update();
}

void Pill::choose(Selection selection) {
	if (selection.row < 0 || selection.row >= int(_rows.size())) {
		return;
	}
	const auto value = _rows[selection.row].strip->selected();
	if (const auto id = std::get_if<Data::ReactionId>(&value)) {
		const auto emoji = id->emoji();
		const auto chosen = _chosen;
		hidePill();
		if (chosen && !emoji.isEmpty() && _owner) {
			chosen(emoji);
		}
	} else if (std::get<Strip::AddedButton>(value)
		== Strip::AddedButton::Expand) {
		_expanded = true;
		_selected = _pressed = {};
		fill();
		place();
		update();
	}
}

void Pill::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);
	const auto progress = _shownAnimation.value(_shown ? 1. : 0.);
	if (progress <= 0.) {
		return;
	}
	p.setOpacity(progress);
	const auto inner = pillRect();
	const auto shift = int((1. - progress) * st::reactStripSize / 2.);
	p.translate(0, shift);

	const auto radius = st::reactStripHeight / 2.;
	p.setPen(Qt::NoPen);
	for (auto i = kShadow; i > 0; --i) {
		auto color = st::shadowFg->c;
		color.setAlphaF(color.alphaF() * 0.12);
		p.setBrush(color);
		p.drawRoundedRect(
			QRectF(inner).marginsAdded({ i * 0.5, i * 0.5, i * 0.5, i * 1. }),
			radius + i * 0.5,
			radius + i * 0.5);
	}
	p.setBrush(st::emojiPanBg);
	p.drawRoundedRect(inner, radius, radius);

	const auto skipy = (st::reactStripHeight - st::reactStripSize) / 2;
	for (auto i = 0; i != int(_rows.size()); ++i) {
		_rows[i].strip->paint(
			p,
			inner.topLeft() + QPoint(
				st::reactStripSkip,
				skipy + i * st::reactStripSize),
			{ st::reactStripSize, 0 },
			rect(),
			1.,
			false);
	}

	// The hovered reaction's name in a small dark bubble above it.
	const auto row = (_selected.row >= 0) ? &_rows[_selected.row] : nullptr;
	if (row && _selected.index < int(row->titles.size())) {
		const auto title = row->titles[_selected.index];
		const auto &font = st::normalFont;
		const auto width = font->width(title) + 16;
		const auto height = font->height + 6;
		const auto center = inner.x()
			+ st::reactStripSkip
			+ _selected.index * st::reactStripSize
			+ st::reactStripSize / 2;
		const auto bubble = QRect(
			std::clamp(center - width / 2, 0, std::max(this->width() - width, 0)),
			inner.y() + _selected.row * st::reactStripSize - height - 4,
			width,
			height);
		p.setBrush(st::toastBg);
		p.drawRoundedRect(bubble, height / 2., height / 2.);
		p.setPen(st::toastFg);
		p.setFont(font);
		p.drawText(bubble, Qt::AlignCenter, title);
	}
}

void Pill::mouseMoveEvent(QMouseEvent *e) {
	select(lookup(e->pos()));
}

void Pill::mousePressEvent(QMouseEvent *e) {
	_pressed = lookup(e->pos());
}

void Pill::mouseReleaseEvent(QMouseEvent *e) {
	const auto selection = lookup(e->pos());
	if (selection.row >= 0 && selection == base::take(_pressed)) {
		choose(selection);
	}
}

void Pill::enterEventHook(QEnterEvent *e) {
	_hovered = true;
	_hideTimer.cancel();
}

void Pill::leaveEventHook(QEvent *e) {
	_hovered = false;
	select({});
	scheduleHide();
}

[[nodiscard]] not_null<Pill*> PillFor(
		not_null<Window::SessionController*> controller) {
	static auto pill = QPointer<Pill>();
	const auto parent = controller->widget()->bodyWidget();
	if (!pill
		|| pill->parentWidget() != parent
		|| pill->session() != &controller->session()) {
		delete pill.data();
		pill = Ui::CreateChild<Pill>(parent, &controller->session());
	}
	return pill.data();
}

} // namespace

void SetupReactionHover(
		not_null<Ui::RpWidget*> button,
		not_null<Window::SessionController*> controller,
		Fn<void(QString emoji)> chosen) {
	struct State {
		explicit State(Fn<void()> callback) : timer(std::move(callback)) {
		}
		base::Timer timer;
	};
	const auto state = button->lifetime().make_state<State>([=] {
		if (!button->underMouse()) {
			return;
		}
		const auto pill = PillFor(controller);
		const auto parent = pill->parentWidget();
		pill->showFor(
			QRect(button->mapTo(parent, QPoint()), button->size()),
			button,
			chosen);
	});
	button->events(
	) | rpl::on_next([=](not_null<QEvent*> e) {
		switch (e->type()) {
		case QEvent::Enter:
			PillFor(controller)->cancelHide(button);
			state->timer.callOnce(kShowDelay);
			break;
		case QEvent::Leave:
			state->timer.cancel();
			PillFor(controller)->scheduleHide();
			break;
		case QEvent::MouseButtonPress:
			state->timer.cancel();
			break;
		default:
			break;
		}
	}, button->lifetime());
}

} // namespace GB::Feed
