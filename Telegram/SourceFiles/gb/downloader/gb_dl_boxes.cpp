/*
This file is part of GB, a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "gb/downloader/gb_dl_boxes.h"

#include "core/file_utilities.h"
#include "gb/downloader/gb_dl_manager.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_boxes.h"
#include "styles/style_gb.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace GB::Downloader {
namespace {

struct DownloadState {
	std::unique_ptr<Analyzer> analyzer;
	AnalyzeResult result;
	rpl::variable<int> type = 0;
	rpl::variable<int> quality = 0;
	rpl::variable<int> audioFormat = 0;
	rpl::variable<int> audioBitrate = 0;
	rpl::variable<QString> folder;
	bool folderChosen = false;
	Ui::InputField *name = nullptr;
	bool filled = false;
};

[[nodiscard]] std::vector<QString> ToVector(const QStringList &list) {
	return { list.begin(), list.end() };
}

not_null<Ui::SettingsButton*> AddChoice(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller,
		const QString &title,
		const QStringList &options,
		not_null<rpl::variable<int>*> value) {
	const auto list = ToVector(options);
	auto label = value->value() | rpl::map([=](int index) {
		return (index >= 0 && index < int(list.size()))
			? list[index]
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
			.options = list,
			.initialSelection = value->current(),
			.callback = [=](int index) {
				*value = index;
			},
		}));
	});
	return button;
}

not_null<Ui::SettingsButton*> AddFolderChoice(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		not_null<rpl::variable<QString>*> value,
		Fn<void()> chosen = nullptr) {
	auto label = value->value() | rpl::map([](const QString &path) {
		const auto native = QDir::toNativeSeparators(path);
		return (native.size() > 36)
			? (u"..."_q + native.right(34))
			: native;
	});
	const auto button = ::Settings::AddButtonWithLabel(
		container,
		rpl::single(title),
		std::move(label),
		st::settingsButtonNoIcon);
	button->setClickedCallback([=] {
		FileDialog::GetFolder(
			button.get(),
			title,
			value->current(),
			crl::guard(button.get(), [=](QString &&result) {
				if (!result.isEmpty()) {
					*value = QDir::toNativeSeparators(result);
					if (chosen) {
						chosen();
					}
				}
			}));
	});
	return button;
}

not_null<Ui::SettingsButton*> AddToggle(
		not_null<Ui::VerticalLayout*> container,
		const QString &title,
		bool initial,
		Fn<void(bool)> changed) {
	const auto button = container->add(object_ptr<Ui::SettingsButton>(
		container,
		rpl::single(title),
		st::settingsButtonNoIcon));
	button->toggleOn(rpl::single(initial));
	button->toggledChanges(
	) | rpl::on_next(std::move(changed), button->lifetime());
	return button;
}

[[nodiscard]] QString MetaLine(const AnalyzeResult &result) {
	auto parts = QStringList();
	if (!result.creator.isEmpty()) {
		parts.push_back(result.creator);
	}
	if (!result.platform.isEmpty()) {
		parts.push_back(result.platform);
	}
	if (result.duration > 0) {
		parts.push_back(FormatDuration(result.duration));
	}
	if (result.fileSize > 0) {
		parts.push_back(FormatSize(result.fileSize));
	}
	return parts.join(u" \u2022 "_q);
}

void AddConfirmed(
		not_null<Window::SessionController*> controller,
		AddRequest request) {
	auto &manager = Manager::Instance();
	const auto existing = request.forceRedownload
		? nullptr
		: manager.findDownloaded(
			request.url,
			request.pageUrl.isEmpty() ? request.url : request.pageUrl,
			request.mode);
	if (existing) {
		const auto path = existing->resultPath;
		controller->show(Ui::MakeConfirmBox({
			.text = u"This was already downloaded:\n%1\n\n"
				"Download it again?"_q.arg(QFileInfo(path).fileName()),
			.confirmed = [=](Fn<void()> close) {
				auto copy = request;
				copy.forceRedownload = true;
				Manager::Instance().add(std::move(copy));
				close();
				controller->showToast(u"Added to downloads."_q);
			},
			.cancelled = [=](Fn<void()> close) {
				close();
				File::ShowInFolder(path);
			},
			.confirmText = rpl::single(u"Download again"_q),
			.cancelText = rpl::single(u"Show file"_q),
			.strictCancel = true,
		}));
		return;
	}
	manager.add(std::move(request));
	controller->showToast(u"Added to downloads."_q);
}

void FillDownloadBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<DownloadState*> state,
		not_null<Ui::FlatLabel*> title,
		not_null<Ui::FlatLabel*> about) {
	const auto &result = state->result;
	const auto &settings = Manager::Instance().settings();
	title->setText(result.title);
	about->setText(MetaLine(result));
	if (state->filled) {
		return;
	}
	state->filled = true;
	const auto container = box->verticalLayout();
	Ui::AddSkip(container);

	const auto qualities = result.qualities.isEmpty()
		? VideoQualities()
		: result.qualities;
	if (!result.plainFile) {
		state->type = (settings.defaultMode == Mode::Audio) ? 1 : 0;
		AddChoice(
			container,
			controller,
			u"Download"_q,
			{ u"Video"_q, u"Audio only"_q },
			&state->type);

		const auto preferred = ResolveTarget(
			settings,
			result.url,
			settings.videoQuality).quality;
		auto qualityIndex = qualities.indexOf(preferred);
		if (qualityIndex < 0) {
			const auto wanted = QString(preferred).remove('p').toInt();
			qualityIndex = 0;
			for (auto i = 1; i < qualities.size(); ++i) {
				if (QString(qualities[i]).remove('p').toInt() <= wanted) {
					qualityIndex = i;
					break;
				}
			}
		}
		state->quality = qualityIndex;
		state->audioFormat = std::max(
			0,
			int(AudioFormats().indexOf(settings.audioFormat)));
		state->audioBitrate = std::max(
			0,
			int(AudioBitrates().indexOf(settings.audioBitrate)));

		const auto video = container->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				container,
				object_ptr<Ui::VerticalLayout>(container)));
		AddChoice(
			video->entity(),
			controller,
			u"Quality"_q,
			qualities,
			&state->quality);
		video->toggleOn(
			state->type.value() | rpl::map(rpl::mappers::_1 == 0),
			anim::type::instant);

		const auto audio = container->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				container,
				object_ptr<Ui::VerticalLayout>(container)));
		AddChoice(
			audio->entity(),
			controller,
			u"Audio format"_q,
			AudioFormats(),
			&state->audioFormat);
		auto bitrates = QStringList();
		for (const auto &value : AudioBitrates()) {
			bitrates.push_back(value + u" kbps"_q);
		}
		AddChoice(
			audio->entity(),
			controller,
			u"Audio bitrate"_q,
			bitrates,
			&state->audioBitrate);
		audio->toggleOn(
			state->type.value() | rpl::map(rpl::mappers::_1 == 1),
			anim::type::instant);
	}

	state->folder = ResolveTarget(settings, result.url, QString()).folder;
	AddFolderChoice(container, u"Save to"_q, &state->folder, [=] {
		state->folderChosen = true;
	});

	Ui::AddSkip(container);
	state->name = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::NoNewlines,
		rpl::single(u"File name"_q),
		result.plainFile ? result.filename : SafeFilename(result.title)));

	const auto submit = [=] {
		const auto &result = state->result;
		const auto mode = result.plainFile
			? Mode::File
			: (state->type.current() == 1)
			? Mode::Audio
			: Mode::Video;
		auto name = state->name->getLastText().trimmed();
		if (name.isEmpty()) {
			name = result.plainFile ? result.filename : result.title;
		}
		auto request = AddRequest{
			.url = result.url,
			.pageUrl = result.url,
			.title = name,
			.folder = state->folderChosen ? state->folder.current() : QString(),
			.mode = mode,
			.quality = (mode == Mode::Video)
				? qualities.value(state->quality.current())
				: QString(),
			.audioFormat = AudioFormats().value(state->audioFormat.current()),
			.audioBitrate = AudioBitrates().value(state->audioBitrate.current()),
			.infoJsonPath = result.infoJsonPath,
			.platform = result.platform,
			.creator = result.creator,
			.duration = result.duration,
			.fileSize = result.fileSize,
		};
		state->result.infoJsonPath = QString();
		box->closeBox();
		AddConfirmed(controller, std::move(request));
	};
	state->name->submits() | rpl::on_next(submit, state->name->lifetime());

	box->clearButtons();
	box->addButton(rpl::single(u"Download"_q), submit);
	box->addButton(rpl::single(u"Cancel"_q), [=] { box->closeBox(); });
}

void DownloadBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		QString url) {
	box->setTitle(rpl::single(u"Download"_q));
	box->setWidth(st::boxWideWidth);
	const auto state = box->lifetime().make_state<DownloadState>();
	const auto title = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		url,
		st::gbDlBoxTitle));
	const auto about = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		u"Reading the link..."_q,
		st::gbDlBoxAbout));
	box->addButton(rpl::single(u"Cancel"_q), [=] { box->closeBox(); });

	const auto run = std::make_shared<Fn<void()>>();
	*run = [=] {
		about->setText(u"Reading the link..."_q);
		box->clearButtons();
		box->addButton(rpl::single(u"Cancel"_q), [=] { box->closeBox(); });
		state->analyzer = std::make_unique<Analyzer>(
			Manager::Instance().settings(),
			url,
			crl::guard(box, [=](QString text) {
				about->setText(u"Reading the link: "_q + text);
			}),
			crl::guard(box, [=](AnalyzeResult result) {
				if (!result.ok) {
					about->setText(result.error);
					box->clearButtons();
					box->addButton(rpl::single(u"Try again"_q), [=] {
						(*run)();
					});
					box->addButton(rpl::single(u"Close"_q), [=] {
						box->closeBox();
					});
					return;
				}
				state->result = std::move(result);
				FillDownloadBox(box, controller, state, title, about);
			}));
		state->analyzer->start();
	};
	(*run)();
	box->boxClosing() | rpl::on_next([=] {
		state->analyzer = nullptr;
		if (!state->result.infoJsonPath.isEmpty()) {
			QFile::remove(state->result.infoJsonPath);
		}
		*run = nullptr;
	}, box->lifetime());
}

void BatchBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	const auto &settings = Manager::Instance().settings();
	box->setTitle(rpl::single(u"Batch download"_q));
	box->setWidth(st::boxWideWidth);
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::MultiLine,
		rpl::single(u"Paste up to %1 links, one per line"_q.arg(
			kMaxBatchItems))));
	field->setMaxHeight(st::boxWideWidth);
	const auto about = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		QString(),
		st::gbDlBoxAbout));
	const auto type = box->lifetime().make_state<rpl::variable<int>>(
		(settings.defaultMode == Mode::Audio) ? 1 : 0);
	const auto quality = box->lifetime().make_state<rpl::variable<int>>(
		std::max(0, int(VideoQualities().indexOf(settings.videoQuality))));
	const auto container = box->verticalLayout();
	Ui::AddSkip(container);
	AddChoice(
		container,
		controller,
		u"Download"_q,
		{ u"Video"_q, u"Audio only"_q },
		type);
	const auto video = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	AddChoice(
		video->entity(),
		controller,
		u"Quality"_q,
		VideoQualities(),
		quality);
	video->toggleOn(
		type->value() | rpl::map(rpl::mappers::_1 == 0),
		anim::type::instant);

	const auto updateAbout = [=] {
		const auto parsed = ParseBatchUrls(field->getLastText());
		if (parsed.urls.isEmpty()) {
			about->setText(u"Links are read from any text: numbered lists, "
				"quotes and several links on one line work too."_q);
			return;
		}
		auto text = u"%1 links found"_q.arg(parsed.urls.size());
		if (parsed.duplicates) {
			text += u", %1 duplicates skipped"_q.arg(parsed.duplicates);
		}
		if (parsed.invalid) {
			text += u", %1 invalid skipped"_q.arg(parsed.invalid);
		}
		if (!parsed.error.isEmpty() && parsed.urls.size() > kMaxBatchItems) {
			text = parsed.error;
		}
		about->setText(text);
	};
	field->changes() | rpl::on_next(updateAbout, field->lifetime());
	updateAbout();

	box->addButton(rpl::single(u"Add to queue"_q), [=] {
		const auto parsed = ParseBatchUrls(field->getLastText());
		if (!parsed.error.isEmpty()) {
			field->showError();
			box->showToast(parsed.error);
			return;
		}
		const auto error = Manager::Instance().addBatch(
			parsed.urls,
			(type->current() == 1) ? Mode::Audio : Mode::Video,
			VideoQualities().value(quality->current()));
		if (!error.isEmpty()) {
			box->showToast(error);
			return;
		}
		box->closeBox();
		controller->showToast(u"%1 links added to downloads."_q.arg(
			parsed.urls.size()));
	});
	box->addButton(rpl::single(u"Cancel"_q), [=] { box->closeBox(); });
	box->setFocusCallback([=] { field->setFocusFast(); });
}

void SettingsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller) {
	struct State {
		Settings settings;
		rpl::variable<QString> folder;
		rpl::variable<int> mode = 0;
		rpl::variable<int> quality = 0;
		rpl::variable<int> audioFormat = 0;
		rpl::variable<int> audioBitrate = 0;
		rpl::variable<int> concurrent = 0;
		rpl::variable<int> subtitles = 0;
		rpl::variable<int> cookiesBrowser = 0;
		rpl::variable<QString> cookiesFile;
	};
	const auto state = box->lifetime().make_state<State>();
	state->settings = Manager::Instance().settings();
	auto &settings = state->settings;
	state->folder = settings.downloadDir;
	state->mode = (settings.defaultMode == Mode::Audio) ? 1 : 0;
	state->quality = std::max(
		0,
		int(VideoQualities().indexOf(settings.videoQuality)));
	state->audioFormat = std::max(
		0,
		int(AudioFormats().indexOf(settings.audioFormat)));
	state->audioBitrate = std::max(
		0,
		int(AudioBitrates().indexOf(settings.audioBitrate)));
	state->concurrent = std::clamp(settings.maxConcurrent, 1, 10) - 1;
	state->subtitles = int(settings.subtitles);
	auto browserIndex = 0;
	for (auto i = 0; i != CookieBrowserOptions().size(); ++i) {
		if (CookieBrowserOptions()[i].compare(
				settings.cookiesBrowser,
				Qt::CaseInsensitive) == 0) {
			browserIndex = i;
		}
	}
	state->cookiesBrowser = browserIndex;
	state->cookiesFile = settings.cookiesFile;

	box->setTitle(rpl::single(u"Downloader settings"_q));
	box->setWidth(st::boxWideWidth);
	const auto container = box->verticalLayout();

	Ui::AddSubsectionTitle(container, rpl::single(u"Saving"_q));
	AddFolderChoice(container, u"Save to"_q, &state->folder);
	AddChoice(
		container,
		controller,
		u"Download type"_q,
		{ u"Video"_q, u"Audio only"_q },
		&state->mode);
	AddChoice(
		container,
		controller,
		u"Video quality"_q,
		VideoQualities(),
		&state->quality);
	AddChoice(
		container,
		controller,
		u"Audio format"_q,
		AudioFormats(),
		&state->audioFormat);
	auto bitrates = QStringList();
	for (const auto &value : AudioBitrates()) {
		bitrates.push_back(value + u" kbps"_q);
	}
	AddChoice(
		container,
		controller,
		u"Audio bitrate"_q,
		bitrates,
		&state->audioBitrate);
	auto counts = QStringList();
	for (auto i = 1; i <= 10; ++i) {
		counts.push_back(QString::number(i));
	}
	AddChoice(
		container,
		controller,
		u"Simultaneous downloads"_q,
		counts,
		&state->concurrent);
	Ui::AddDividerText(
		container,
		rpl::single(u"Default 6. More can trigger rate limits on some "
			"sites."_q));

	Ui::AddSubsectionTitle(container, rpl::single(u"Files"_q));
	AddToggle(container, u"Skip files already in the folder"_q, settings.skipExisting, [=](bool value) {
		state->settings.skipExisting = value;
	});
	AddToggle(container, u"Sub-folder for each website"_q, settings.organizeBySite, [=](bool value) {
		state->settings.organizeBySite = value;
	});
	AddToggle(container, u"Fast mode (parallel stream pieces)"_q, settings.fastMode, [=](bool value) {
		state->settings.fastMode = value;
	});
	AddToggle(container, u"Write title, artist and chapters"_q, settings.embedMetadata, [=](bool value) {
		state->settings.embedMetadata = value;
	});
	AddToggle(container, u"Video thumbnail as cover art"_q, settings.embedThumbnail, [=](bool value) {
		state->settings.embedThumbnail = value;
	});
	AddChoice(
		container,
		controller,
		u"Subtitles"_q,
		SubtitleOptions(),
		&state->subtitles);
	const auto langs = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::NoNewlines,
		rpl::single(u"Subtitle languages, e.g. en,bn"_q),
		settings.subtitleLangs));
	Ui::AddSkip(container);
	Ui::AddDivider(container);

	Ui::AddSubsectionTitle(container, rpl::single(u"Logged-in sites"_q));
	AddChoice(
		container,
		controller,
		u"Read cookies from"_q,
		CookieBrowserOptions(),
		&state->cookiesBrowser);
	auto cookiesLabel = state->cookiesFile.value(
	) | rpl::map([](const QString &path) {
		return path.isEmpty() ? u"Not set"_q : QFileInfo(path).fileName();
	});
	const auto cookies = ::Settings::AddButtonWithLabel(
		container,
		rpl::single(u"cookies.txt file"_q),
		std::move(cookiesLabel),
		st::settingsButtonNoIcon);
	cookies->setClickedCallback([=] {
		FileDialog::GetOpenPath(
			cookies.get(),
			u"Choose cookies.txt"_q,
			u"Cookie files (*.txt);;All files (*)"_q,
			crl::guard(cookies.get(), [=](FileDialog::OpenResult &&result) {
				if (!result.paths.isEmpty()) {
					state->cookiesFile = QDir::toNativeSeparators(
						result.paths.front());
				}
			}));
	});
	Ui::AddDividerText(
		container,
		rpl::single(u"Your browser's cookies are read on this computer by "
			"the engine and never stored by the app."_q));

	Ui::AddSubsectionTitle(container, rpl::single(u"Website folders"_q));
	const auto rules = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::MultiLine,
		rpl::single(u"example.com => D:\\Videos\\Example | 1080p"_q),
		settings.folderRules));
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::single(u"One rule per line: website => folder | quality. "
			"Either part may be left out."_q));

	Ui::AddSubsectionTitle(container, rpl::single(u"Engine"_q));
	const auto ytDlp = FindYtDlp(settings);
	const auto ffmpeg = FindFfmpeg(settings);
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		u"yt-dlp: %1\nffmpeg: %2"_q.arg(
			ytDlp.isEmpty() ? u"not found"_q : ytDlp,
			ffmpeg.isEmpty() ? u"not found"_q : ffmpeg),
		st::gbDlBoxAbout));
	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::single(u"Without yt-dlp only direct file links work. Put "
			"yt-dlp.exe and ffmpeg.exe in %1 or next to the app.\n\n"
			"Use only for content you own or have permission to "
			"download."_q.arg(QDir::toNativeSeparators(EngineFolder()))));

	box->addButton(rpl::single(u"Save"_q), [=] {
		auto result = state->settings;
		result.downloadDir = state->folder.current();
		result.defaultMode = (state->mode.current() == 1)
			? Mode::Audio
			: Mode::Video;
		result.videoQuality = VideoQualities().value(state->quality.current());
		result.audioFormat = AudioFormats().value(state->audioFormat.current());
		result.audioBitrate = AudioBitrates().value(
			state->audioBitrate.current());
		result.maxConcurrent = state->concurrent.current() + 1;
		result.subtitles = Subtitles(std::clamp(
			state->subtitles.current(),
			0,
			2));
		result.subtitleLangs = langs->getLastText().trimmed();
		const auto browser = CookieBrowserOptions().value(
			state->cookiesBrowser.current());
		result.cookiesBrowser = (browser == u"None"_q)
			? QString()
			: browser.toLower();
		result.cookiesFile = state->cookiesFile.current();
		result.folderRules = rules->getLastText().trimmed();
		Manager::Instance().setSettings(std::move(result));
		box->closeBox();
		controller->showToast(u"Downloader settings saved."_q);
	});
	box->addButton(rpl::single(u"Cancel"_q), [=] { box->closeBox(); });
}

void PropertiesBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		uint64 id) {
	const auto item = Manager::Instance().find(id);
	if (!item) {
		box->closeBox();
		return;
	}
	box->setTitle(rpl::single(u"Properties"_q));
	box->setWidth(st::boxWideWidth);
	auto lines = QStringList();
	const auto add = [&](const QString &key, const QString &value) {
		if (!value.trimmed().isEmpty()) {
			lines.push_back(key + u": "_q + value);
		}
	};
	add(u"Name"_q, NameOf(*item));
	add(u"Status"_q, DisplayText(*item));
	add(u"Size"_q, FormatSize(item->fileSize
		? item->fileSize
		: item->metrics.total));
	add(u"Saved to"_q, item->resultPath.isEmpty()
		? item->folder
		: item->resultPath);
	add(u"Website"_q, WebsiteOf(item->url));
	add(u"Creator"_q, item->creator);
	add(u"Type"_q, (item->mode == Mode::Audio)
		? (u"Audio "_q + item->audioFormat + ' ' + item->audioBitrate + u" kbps"_q)
		: (item->mode == Mode::File)
		? u"File"_q
		: (u"Video "_q + item->quality));
	add(u"Duration"_q, FormatDuration(item->duration));
	add(u"Link"_q, item->url);
	add(u"Added"_q, FormatDate(item->added));
	add(u"Finished"_q, FormatDate(item->finished));
	if (item->started.isValid() && item->finished.isValid()) {
		const auto seconds = item->started.secsTo(item->finished);
		if (seconds > 0) {
			add(u"Time taken"_q, FormatEta(seconds));
			if (item->fileSize > 0) {
				add(u"Average speed"_q, FormatSpeed(
					float64(item->fileSize) / seconds));
			}
		}
	}
	add(u"Error"_q, item->error);
	const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		lines.join('\n'),
		st::boxLabel));
	label->setSelectable(true);
	const auto url = item->url;
	box->addButton(rpl::single(u"Copy link"_q), [=] {
		QGuiApplication::clipboard()->setText(url);
		box->showToast(u"Link copied."_q);
	});
	box->addButton(rpl::single(u"Close"_q), [=] { box->closeBox(); });
}

} // namespace

void StartDownload(
		not_null<Window::SessionController*> controller,
		const QString &url) {
	const auto trimmed = url.trimmed();
	if (trimmed.isEmpty()) {
		controller->showToast(u"Paste a link first."_q);
		return;
	} else if (!IsValidWebUrl(trimmed)) {
		controller->showToast(u"This is not a valid http(s) link."_q);
		return;
	}
	controller->show(Box(DownloadBox, controller, trimmed));
}

void ShowBatchBox(not_null<Window::SessionController*> controller) {
	controller->show(Box(BatchBox, controller));
}

void ShowSettingsBox(not_null<Window::SessionController*> controller) {
	controller->show(Box(SettingsBox, controller));
}

void ShowPropertiesBox(
		not_null<Window::SessionController*> controller,
		uint64 id) {
	controller->show(Box(PropertiesBox, controller, id));
}

} // namespace GB::Downloader
