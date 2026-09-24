/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/view/export_view_settings.h"
#include <QtCore/QRegularExpression>
#include <QtCore/QPointer>
#include <crl/crl_on_main.h>

#include "export/output/export_output_abstract.h"
#include "export/view/export_view_panel_controller.h"
#include "lang/lang_keys.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/fade_wrap.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/boxes/calendar_box.h"
#include "ui/boxes/choose_time.h"
#include "platform/platform_specific.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "base/unixtime.h"
#include "main/main_session.h"
#include "styles/style_export.h"
#include "styles/style_layers.h"

namespace Export {
namespace View {
namespace {

constexpr auto kMegabyte = int64(1024) * 1024;

[[nodiscard]] PeerId ReadPeerId(
		not_null<Main::Session*> session,
		const MTPInputPeer &data) {
	return data.match([](const MTPDinputPeerUser &data) {
		return peerFromUser(data.vuser_id().v);
	}, [](const MTPDinputPeerUserFromMessage &data) {
		return peerFromUser(data.vuser_id().v);
	}, [](const MTPDinputPeerChat &data) {
		return peerFromChat(data.vchat_id().v);
	}, [](const MTPDinputPeerChannel &data) {
		return peerFromChannel(data.vchannel_id().v);
	}, [](const MTPDinputPeerChannelFromMessage &data) {
		return peerFromChannel(data.vchannel_id().v);
	}, [&](const MTPDinputPeerSelf &data) {
		return session->userPeerId();
	}, [](const MTPDinputPeerEmpty &data) {
		return PeerId(0);
	});
}

void ChooseFormatBox(
		not_null<Ui::GenericBox*> box,
		Output::Format format,
		Fn<void(Output::Format)> done) {
	using Format = Output::Format;
	const auto group = std::make_shared<Ui::RadioenumGroup<Format>>(format);
	const auto addFormatOption = [&](QString label, Format format) {
		box->addRow(
			object_ptr<Ui::Radioenum<Format>>(
				box,
				group,
				format,
				label,
				st::defaultBoxCheckbox),
			st::exportSettingPadding);
	};
	box->setTitle(tr::lng_export_option_choose_format());
	addFormatOption(tr::lng_export_option_html(tr::now), Format::Html);
	addFormatOption(tr::lng_export_option_json(tr::now), Format::Json);
	addFormatOption(
		tr::lng_export_option_html_and_json(tr::now),
		Format::HtmlAndJson);
	box->addButton(tr::lng_settings_save(), [=] { done(group->current()); });
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace

int64 SizeLimitByIndex(int index) {
	Expects(index >= 0 && index < kSizeValueCount);

	index += 1;
	const auto megabytes = [&] {
		if (index <= 10) {
			return index;
		} else if (index <= 30) {
			return 10 + (index - 10) * 2;
		} else if (index <= 40) {
			return 50 + (index - 30) * 5;
		} else if (index <= 60) {
			return 100 + (index - 40) * 10;
		} else if (index <= 70) {
			return 300 + (index - 60) * 20;
		} else if (index <= 80) {
			return 500 + (index - 70) * 50;
		} else if (index <= 90) {
			return 1000 + (index - 80) * 100;
		} else {
			return 2000 + (index - 90) * 200;
		}
	}();
	return megabytes * kMegabyte;
}

SettingsWidget::SettingsWidget(
	QWidget *parent,
	not_null<Main::Session*> session,
	Settings data)
: RpWidget(parent)
, _session(session)
, _singlePeerId(ReadPeerId(session, data.singlePeer))
, _internal_data(std::move(data)) {
	ResolveSettings(session, _internal_data);
	setupContent();
}

const Settings &SettingsWidget::readData() const {
	return _internal_data;
}

template <typename Callback>
void SettingsWidget::changeData(Callback &&callback) {
	callback(_internal_data);
	_changes.fire_copy(_internal_data);
}

void SettingsWidget::setupContent() {
	const auto scroll = Ui::CreateChild<Ui::ScrollArea>(
		this,
		st::boxScroll);
	const auto wrap = scroll->setOwnedWidget(
		object_ptr<Ui::OverrideMargins>(
			scroll,
			object_ptr<Ui::VerticalLayout>(scroll)));
	const auto content = static_cast<Ui::VerticalLayout*>(wrap->entity());

	const auto buttons = setupButtons(scroll, wrap);
	_content = content;
	_scroll = scroll;
	_buttonsHeight = buttons->height();
	setupOptions(content);
	setupPathAndFormat(content);

	sizeValue(
	) | rpl::on_next([=](QSize size) {
		scroll->resize(size.width(), size.height() - buttons->height());
		wrap->resizeToWidth(size.width());
		content->resizeToWidth(size.width());
	}, lifetime());
}

void SettingsWidget::setupOptions(not_null<Ui::VerticalLayout*> container) {
	if (!_singlePeerId) {
		setupFullExportOptions(container);
	}
	setupMediaOptions(container);
	if (!_singlePeerId) {
		setupOtherOptions(container);
	}
}

void SettingsWidget::setupFullExportOptions(
		not_null<Ui::VerticalLayout*> container) {
	addOptionWithAbout(
		container,
		tr::lng_export_option_info(tr::now),
		Type::PersonalInfo | Type::Userpics,
		tr::lng_export_option_info_about(tr::now));
	addOptionWithAbout(
		container,
		tr::lng_export_option_contacts(tr::now),
		Type::Contacts,
		tr::lng_export_option_contacts_about(tr::now));
	addOptionWithAbout(
		container,
		tr::lng_export_option_stories(tr::now),
		Type::Stories,
		tr::lng_export_option_stories_about(tr::now));
	addOptionWithAbout(
		container,
		tr::lng_export_option_profile_music(tr::now),
		Type::ProfileMusic,
		tr::lng_export_option_profile_music_about(tr::now));
	addHeader(container, tr::lng_export_header_chats(tr::now));
	addOption(
		container,
		tr::lng_export_option_personal_chats(tr::now),
		Type::PersonalChats);
	addOption(
		container,
		tr::lng_export_option_bot_chats(tr::now),
		Type::BotChats);
	addChatOption(
		container,
		tr::lng_export_option_private_groups(tr::now),
		Type::PrivateGroups);
	addChatOption(
		container,
		tr::lng_export_option_private_channels(tr::now),
		Type::PrivateChannels);
	addChatOption(
		container,
		tr::lng_export_option_public_groups(tr::now),
		Type::PublicGroups);
	addChatOption(
		container,
		tr::lng_export_option_public_channels(tr::now),
		Type::PublicChannels);
}

void SettingsWidget::setupMediaOptions(
		not_null<Ui::VerticalLayout*> container) {
	if (_singlePeerId != 0) {
		addMediaOptions(container);
		return;
	}
	const auto mediaWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));
	const auto media = mediaWrap->entity();
	addHeader(media, tr::lng_export_header_media(tr::now));
	addMediaOptions(media);

	value() | rpl::map([](const Settings &data) {
		return data.types;
	}) | rpl::distinct_until_changed(
	) | rpl::on_next([=](Settings::Types types) {
		mediaWrap->toggle((types & (Type::PersonalChats
			| Type::BotChats
			| Type::PrivateGroups
			| Type::PrivateChannels
			| Type::PublicGroups
			| Type::PublicChannels
			| Type::ProfileMusic)) != 0, anim::type::normal);
	}, mediaWrap->lifetime());

	widthValue(
	) | rpl::on_next([=](int width) {
		mediaWrap->resizeToWidth(width);
	}, mediaWrap->lifetime());
}

void SettingsWidget::setupOtherOptions(
		not_null<Ui::VerticalLayout*> container) {
	addHeader(container, tr::lng_export_header_other(tr::now));
	addOptionWithAbout(
		container,
		tr::lng_export_option_sessions(tr::now),
		Type::Sessions,
		tr::lng_export_option_sessions_about(tr::now));
	addOptionWithAbout(
		container,
		tr::lng_export_option_other(tr::now),
		Type::OtherData,
		tr::lng_export_option_other_about(tr::now));
}

void SettingsWidget::setupPathAndFormat(
		not_null<Ui::VerticalLayout*> container) {
	if (_singlePeerId != 0) {
		addFormatAndLocationLabel(container);
		addIdRangeOption(container, addLimitsLabel(container));
		return;
	}
	const auto formatGroup = std::make_shared<Ui::RadioenumGroup<Format>>(
		readData().format);
	formatGroup->setChangedCallback([=](Format format) {
		changeData([&](Settings &data) {
			data.format = format;
		});
	});
	const auto addFormatOption = [&](QString label, Format format) {
		container->add(
			object_ptr<Ui::Radioenum<Format>>(
				container,
				formatGroup,
				format,
				label,
				st::defaultBoxCheckbox),
			st::exportSettingPadding);
	};
	addHeader(container, tr::lng_export_header_format(tr::now));
	addLocationLabel(container);
	addFormatOption(tr::lng_export_option_html(tr::now), Format::Html);
	addFormatOption(tr::lng_export_option_json(tr::now), Format::Json);
	addFormatOption(tr::lng_export_option_html_and_json(tr::now), Format::HtmlAndJson);
}

void SettingsWidget::addLocationLabel(
		not_null<Ui::VerticalLayout*> container) {
#ifndef OS_MAC_STORE
	auto pathLink = value() | rpl::map([](const Settings &data) {
		return data.path;
	}) | rpl::distinct_until_changed(
	) | rpl::map([=](const QString &path) {
		const auto text = IsDefaultPath(_session, path)
			? Core::App().canReadDefaultDownloadPath()
			? u"Downloads/"_q + File::DefaultDownloadPathFolder(_session)
			: tr::lng_download_path_temp(tr::now)
			: path;
		return tr::link(
			QDir::toNativeSeparators(text),
			QString("internal:edit_export_path"));
	});
	const auto label = container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			tr::lng_export_option_location(
				lt_path,
				std::move(pathLink),
				tr::marked),
			st::exportLocationLabel),
		st::exportLocationPadding);
	label->overrideLinkClickHandler([=] {
		chooseFolder();
	});
#endif // OS_MAC_STORE
}

void SettingsWidget::chooseFormat() {
	const auto shared = std::make_shared<base::weak_qptr<Ui::GenericBox>>();
	const auto callback = [=](Format format) {
		changeData([&](Settings &data) {
			data.format = format;
		});
		if (const auto strong = shared->get()) {
			strong->closeBox();
		}
	};
	auto box = Box(
		ChooseFormatBox,
		readData().format,
		callback);
	*shared = base::make_weak(box.data());
	_showBoxCallback(std::move(box));
}

void SettingsWidget::addFormatAndLocationLabel(
		not_null<Ui::VerticalLayout*> container) {
#ifndef OS_MAC_STORE
	auto pathLink = value() | rpl::map([](const Settings &data) {
		return data.path;
	}) | rpl::distinct_until_changed(
	) | rpl::map([=](const QString &path) {
		const auto text = IsDefaultPath(_session, path)
			? Core::App().canReadDefaultDownloadPath()
			? u"Downloads/"_q + File::DefaultDownloadPathFolder(_session)
			: tr::lng_download_path_temp(tr::now)
			: path;
		return tr::link(
			QDir::toNativeSeparators(text),
			u"internal:edit_export_path"_q);
	});
	auto formatLink = value() | rpl::map([](const Settings &data) {
		return data.format;
	}) | rpl::distinct_until_changed(
	) | rpl::map([](Format format) {
		const auto text = (format == Format::Html)
			? "HTML"
			: (format == Format::Json)
			? "JSON"
			: tr::lng_export_option_html_and_json(tr::now);
		return tr::link(text, u"internal:edit_format"_q);
	});
	const auto label = container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			tr::lng_export_option_format_location(
				lt_format,
				std::move(formatLink),
				lt_path,
				std::move(pathLink),
				tr::marked),
			st::exportLocationLabel),
		st::exportLocationPadding);
	label->overrideLinkClickHandler([=](const QString &url) {
		if (url == u"internal:edit_export_path"_q) {
			chooseFolder();
		} else if (url == u"internal:edit_format"_q) {
			chooseFormat();
		} else {
			Unexpected("Click handler URL in export limits edit.");
		}
	});
#endif // OS_MAC_STORE
}

not_null<Ui::Checkbox*> SettingsWidget::addLimitsLabel(
		not_null<Ui::VerticalLayout*> container) {
	auto fromDateLink = value() | rpl::map([](const Settings &data) {
		return data.singlePeerFrom;
	}) | rpl::distinct_until_changed(
	) | rpl::map([](std::optional<TimeId> from) {
		return (from
			? rpl::single(langDayOfMonthFull(
				base::unixtime::parse(*from).date()))
			: tr::lng_export_beginning()
		) | rpl::map(tr::url(u"internal:edit_from"_q));
	}) | rpl::flatten_latest();

	const auto mapToTime = [](TimeId id, const QString &link) {
		return rpl::single(id
			? QLocale().toString(
				base::unixtime::parse(id).time(),
				QLocale::ShortFormat)
			: QString()
		) | rpl::map(tr::url(link));
	};

	const auto concat = [](TextWithEntities date, TextWithEntities link) {
		return link.text.isEmpty()
			? date
			: date.append(u", "_q).append(std::move(link));
	};

	auto fromTimeLink = value() | rpl::map([](const Settings &data) {
		return data.singlePeerFrom;
	}) | rpl::distinct_until_changed(
	) | rpl::map([=](std::optional<TimeId> from) {
		return mapToTime(from.value_or(0), u"internal:edit_from_time"_q);
	}) | rpl::flatten_latest();

	auto fromLink = rpl::combine(
		std::move(fromDateLink),
		std::move(fromTimeLink)
	) | rpl::map(concat);

	auto tillDateLink = value() | rpl::map([](const Settings &data) {
		return data.singlePeerTill;
	}) | rpl::distinct_until_changed(
	) | rpl::map([](std::optional<TimeId> till) {
		return (till
			? rpl::single(langDayOfMonthFull(
				base::unixtime::parse(*till).date()))
			: tr::lng_export_end()
		) | rpl::map(tr::url(u"internal:edit_till"_q));
	}) | rpl::flatten_latest();

	auto tillTimeLink = value() | rpl::map([](const Settings &data) {
		return data.singlePeerTill;
	}) | rpl::distinct_until_changed(
	) | rpl::map([=](std::optional<TimeId> till) {
		return mapToTime(till.value_or(0), u"internal:edit_till_time"_q);
	}) | rpl::flatten_latest();

	auto tillLink = rpl::combine(
		std::move(tillDateLink),
		std::move(tillTimeLink)
	) | rpl::map(concat);

	auto datesText = tr::lng_export_limits(
		lt_from,
		std::move(fromLink),
		lt_till,
		std::move(tillLink),
		tr::marked
	) | rpl::after_next([=] {
		container->resizeToWidth(container->width());
	});

	const auto dateRow = container->add(
		object_ptr<Ui::FixedHeightWidget>(container),
		st::exportRangePadding);
	const auto dateBox = Ui::CreateChild<Ui::Checkbox>(
		dateRow,
		QString(),
		!readData().useIdRange,
		st::defaultBoxCheckbox);
	// exportRangeLabel carries a top margin equal to the checkbox
	// text line, so both texts land at the same y with no runtime
	// offset math: the label just sits at the row top.
	const auto label = Ui::CreateChild<Ui::FlatLabel>(
		dateRow,
		std::move(datesText),
		st::exportRangeLabel);
	const auto layoutDateRow = [=] {
		const auto width = dateRow->width();
		const auto gap = st::defaultBox.buttonPadding.left();
		dateBox->resizeToWidth(dateBox->naturalWidth());
		const auto labelX = dateBox->checkRect().right() + gap;
		label->resizeToWidth(std::max(width - labelX, 0));
		dateBox->moveToLeft(0, 0);
		label->moveToLeft(labelX, 0);
		dateRow->resize(width, std::max(
			dateBox->height(),
			label->height()));
	};
	dateRow->widthValue(
	) | rpl::on_next([=](int) {
		layoutDateRow();
	}, dateRow->lifetime());
	label->heightValue(
	) | rpl::on_next([=](int) {
		layoutDateRow();
	}, dateRow->lifetime());
	layoutDateRow();
	const auto removeTime = [](TimeId dateTime) {
		return base::unixtime::serialize(
			QDateTime(
				base::unixtime::parse(dateTime).date(),
				QTime()));
	};

	const auto editTimeLimit = [=](Fn<TimeId()> now, Fn<void(TimeId)> done) {
		_showBoxCallback(Box([=](not_null<Ui::GenericBox*> box) {
			auto result = Ui::ChooseTimeWidget(
				box->verticalLayout(),
				[&] {
					const auto time = base::unixtime::parse(now()).time();
					return time.hour() * 3600
						+ time.minute() * 60
						+ time.second();
				}(),
				true);
			const auto widget = box->addRow(std::move(result.widget));
			const auto toSave = widget->lifetime().make_state<TimeId>(0);
			std::move(
				result.secondsValue
			) | rpl::on_next([=](TimeId t) {
				*toSave = t;
			}, box->lifetime());
			box->addButton(tr::lng_settings_save(), [=] {
				done(*toSave);
				box->closeBox();
			});
			box->addButton(tr::lng_cancel(), [=] {
				box->closeBox();
			});
			box->setTitle(tr::lng_settings_ttl_after_custom());
		}));
	};

	constexpr auto kOffset = 600;

	label->overrideLinkClickHandler([=](const QString &url) {
		if (url == u"internal:edit_from"_q) {
			const auto done = [=](TimeId limit) {
				changeData([&](Settings &settings) {
					settings.singlePeerFrom = limit;
				});
			};
			editDateLimit(
				readData().singlePeerFrom.value_or(0),
				0,
				readData().singlePeerTill.value_or(0),
				QTime(0, 0),
				tr::lng_export_from_beginning(),
				done);
		} else if (url == u"internal:edit_from_time"_q) {
			const auto now = [=] {
				auto result = TimeId(0);
				changeData([&](Settings &settings) {
					result = settings.singlePeerFrom.value_or(0);
				});
				return result;
			};
			const auto done = [=](TimeId time) {
				changeData([&](Settings &settings) {
					const auto result = time
						+ removeTime(settings.singlePeerFrom.value_or(0));
					if (settings.singlePeerTill
							&& result >= *settings.singlePeerTill) {
						settings.singlePeerFrom = *settings.singlePeerTill
							- kOffset;
					} else {
						settings.singlePeerFrom = result;
					}
				});
			};
			editTimeLimit(now, done);
		} else if (url == u"internal:edit_till"_q) {
			const auto done = [=](TimeId limit) {
				changeData([&](Settings &settings) {
					if (settings.singlePeerFrom
							&& limit <= *settings.singlePeerFrom) {
						settings.singlePeerTill = *settings.singlePeerFrom
							+ kOffset;
					} else {
						settings.singlePeerTill = limit;
					}
				});
			};
			editDateLimit(
				readData().singlePeerTill.value_or(0),
				readData().singlePeerFrom.value_or(0),
				0,
				QTime(23, 59, 59),
				tr::lng_export_till_end(),
				done);
		} else if (url == u"internal:edit_till_time"_q) {
			const auto now = [=] {
				auto result = TimeId(0);
				changeData([&](Settings &settings) {
					result = settings.singlePeerTill.value_or(0);
				});
				return result;
			};
			const auto done = [=](TimeId time) {
				changeData([&](Settings &settings) {
					const auto result = time
						+ removeTime(settings.singlePeerTill.value_or(0));
					if (settings.singlePeerFrom
							&& result <= *settings.singlePeerFrom) {
						settings.singlePeerTill = *settings.singlePeerFrom
							+ kOffset;
					} else {
						settings.singlePeerTill = result;
					}
				});
			};
			editTimeLimit(now, done);
		} else {
			Unexpected("Click handler URL in export limits edit.");
		}
	});
	return dateBox;
}

void SettingsWidget::addIdRangeOption(
		not_null<Ui::VerticalLayout*> container,
		not_null<Ui::Checkbox*> dateBox) {
	const auto row = container->add(
		object_ptr<Ui::FixedHeightWidget>(container),
		st::exportRangeLastPadding);
	const auto idBox = Ui::CreateChild<Ui::Checkbox>(
		row,
		QString(),
		readData().useIdRange,
		st::defaultBoxCheckbox);
	const auto fromLabel = Ui::CreateChild<Ui::FlatLabel>(
		row,
		tr::lng_export_from_id(tr::now),
		st::exportRangeLabel);
	const auto fromInput = Ui::CreateChild<Ui::InputField>(
		row,
		st::exportRangeInput);
	const auto toLabel = Ui::CreateChild<Ui::FlatLabel>(
		row,
		tr::lng_export_to_id(tr::now),
		st::exportRangeLabel);
	const auto toInput = Ui::CreateChild<Ui::InputField>(
		row,
		st::exportRangeInput);
	if (readData().singlePeerFromId) {
		fromInput->setText(QString::number(*readData().singlePeerFromId));
	}
	if (readData().singlePeerTillId) {
		toInput->setText(QString::number(*readData().singlePeerTillId));
	}

	const auto setDateMode = [=] {
		changeData([&](Settings &data) {
			data.useIdRange = false;
		});
		idBox->setChecked(false);
	};
	const auto setIdMode = [=] {
		changeData([&](Settings &data) {
			data.useIdRange = true;
			data.singlePeerFrom = std::nullopt;
			data.singlePeerTill = std::nullopt;
		});
		dateBox->setChecked(false);
	};
	const auto weakRow = QPointer<Ui::FixedHeightWidget>(row);
	dateBox->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		if (checked) {
			setDateMode();
		} else if (!idBox->checked()) {
			dateBox->setChecked(true);
		}
	}, dateBox->lifetime());
	idBox->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		if (checked) {
			setIdMode();
		} else if (!dateBox->checked()) {
			idBox->setChecked(true);
		}
	}, idBox->lifetime());

	const auto readId = [=](not_null<Ui::InputField*> input) {
		auto digits = QString();
		for (const auto ch : input->getLastText()) {
			if (ch.isDigit()) {
				digits.append(ch);
			}
		}
		digits = digits.left(10);
		if (digits != input->getLastText()) {
			input->setText(digits);
			return std::optional<uint64>();
		}
		return digits.isEmpty()
			? std::optional<uint64>()
			: std::optional<uint64>(digits.toULongLong());
	};
	fromInput->changes(
	) | rpl::on_next([=] {
		const auto id = readId(fromInput);
		if (id && !idBox->checked()) {
			setIdMode();
		}
		changeData([&](Settings &data) {
			data.singlePeerFromId = id;
		});
	}, fromInput->lifetime());
	toInput->changes(
	) | rpl::on_next([=] {
		const auto id = readId(toInput);
		if (id && !idBox->checked()) {
			setIdMode();
		}
		changeData([&](Settings &data) {
			data.singlePeerTillId = id;
		});
	}, toInput->lifetime());

	const auto layoutIdRow = [=] {
		const auto width = row->width();
		const auto gap = st::defaultBox.buttonPadding.left();
		idBox->resizeToWidth(idBox->naturalWidth());
		const auto startX = idBox->checkRect().right() + gap;
		const auto fromW = fromLabel->naturalWidth();
		const auto toW = toLabel->naturalWidth();
		fromLabel->resizeToWidth(fromW);
		toLabel->resizeToWidth(toW);
		auto inputW = (width - startX - fromW - toW - 3 * gap) / 2;
		inputW = std::max(inputW, 0);
		auto x = startX;
		// The field's text margin lands the value on the labels' line.
		const auto inputH = st::exportRangeInput.heightMin;
		const auto place = [&](auto *widget, int w, int h, int y) {
			widget->moveToLeft(x, y);
			widget->resize(w, h);
			x += w + gap;
		};
		idBox->moveToLeft(0, 0);
		place(fromLabel, fromW, fromLabel->height(), 0);
		place(fromInput, inputW, inputH, 0);
		place(toLabel, toW, toLabel->height(), 0);
		place(toInput, inputW, inputH, 0);
		row->resize(row->width(), idBox->height());
	};
	row->widthValue(
	) | rpl::on_next([=](int) {
		layoutIdRow();
	}, row->lifetime());
	fromLabel->heightValue(
	) | rpl::on_next([=](int) {
		layoutIdRow();
	}, row->lifetime());
	toLabel->heightValue(
	) | rpl::on_next([=](int) {
		layoutIdRow();
	}, row->lifetime());
	layoutIdRow();
}

void SettingsWidget::editDateLimit(
		TimeId current,
		TimeId min,
		TimeId max,
		QTime timeOfDay,
		rpl::producer<QString> resetLabel,
		Fn<void(TimeId)> done) {
	Expects(_showBoxCallback != nullptr);

	const auto highlighted = current
		? base::unixtime::parse(current).date()
		: max
		? base::unixtime::parse(max).date()
		: min
		? base::unixtime::parse(min).date()
		: QDate::currentDate();
	const auto month = highlighted;
	const auto shared = std::make_shared<base::weak_qptr<Ui::CalendarBox>>();
	const auto finalize = [=](not_null<Ui::CalendarBox*> box) {
		box->addLeftButton(std::move(resetLabel), crl::guard(this, [=] {
			done(0);
			if (const auto weak = shared->get()) {
				weak->closeBox();
			}
		}));
	};
	const auto callback = crl::guard(this, [=](
			const QDate &date,
			Fn<void()> close) {
		done(base::unixtime::serialize(QDateTime(date, timeOfDay)));
		close();
	});
	auto box = Box<Ui::CalendarBox>(Ui::CalendarBoxArgs{
		.month = month,
		.highlighted = highlighted,
		.callback = callback,
		.finalize = finalize,
		.st = st::exportCalendarSizes,
		.minDate = (min
			? base::unixtime::parse(min).date()
			: QDate(2013, 8, 1)), // Telegram was launched in August 2013 :)
		.maxDate = (max
			? base::unixtime::parse(max).date()
			: QDate::currentDate()),
	});
	*shared = base::make_weak(box.data());
	_showBoxCallback(std::move(box));
}

not_null<Ui::RpWidget*> SettingsWidget::setupButtons(
		not_null<Ui::ScrollArea*> scroll,
		not_null<Ui::RpWidget*> wrap) {
	using namespace rpl::mappers;

	const auto buttonsPadding = st::defaultBox.buttonPadding;
	const auto buttonsHeight = st::defaultBoxButton.height
		+ buttonsPadding.bottom();
	const auto buttons = Ui::CreateChild<Ui::FixedHeightWidget>(
		this,
		buttonsHeight);
	const auto topShadow = Ui::CreateChild<Ui::FadeShadow>(this);
	const auto bottomShadow = Ui::CreateChild<Ui::FadeShadow>(this);
	topShadow->toggleOn(scroll->scrollTopValue(
	) | rpl::map(_1 > 0));
	bottomShadow->toggleOn(rpl::combine(
		scroll->heightValue(),
		scroll->scrollTopValue(),
		wrap->heightValue(),
		_2
	) | rpl::map([=](int top) {
		return top < scroll->scrollTopMax();
	}));

	value() | rpl::map([](const Settings &data) {
		return (data.types != Types(0)) || data.onlySinglePeer();
	}) | rpl::distinct_until_changed(
	) | rpl::on_next([=](bool canStart) {
		refreshButtons(buttons, canStart);
		topShadow->raise();
		bottomShadow->raise();
	}, buttons->lifetime());

	sizeValue(
	) | rpl::on_next([=](QSize size) {
		buttons->resizeToWidth(size.width());
		buttons->moveToLeft(0, size.height() - buttons->height());
		topShadow->resizeToWidth(size.width());
		topShadow->moveToLeft(0, 0);
		bottomShadow->resizeToWidth(size.width());
		bottomShadow->moveToLeft(0, buttons->y() - st::lineWidth);
	}, buttons->lifetime());

	return buttons;
}

void SettingsWidget::addHeader(
		not_null<Ui::VerticalLayout*> container,
		const QString &text) {
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			text,
			st::exportHeaderLabel),
		st::exportHeaderPadding);
}

not_null<Ui::Checkbox*> SettingsWidget::addOption(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		Types types) {
	const auto checkbox = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			text,
			((readData().types & types) == types),
			st::defaultBoxCheckbox),
		st::exportSettingPadding);
	checkbox->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		changeData([&](Settings &data) {
			if (checked) {
				data.types |= types;
			} else {
				data.types &= ~types;
			}
		});
	}, checkbox->lifetime());
	return checkbox;
}

not_null<Ui::Checkbox*> SettingsWidget::addOptionWithAbout(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		Types types,
		const QString &about) {
	const auto result = addOption(container, text, types);
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			about,
			st::exportAboutOptionLabel),
		st::exportAboutOptionPadding);
	return result;
}

void SettingsWidget::addChatOption(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		Types types) {
	const auto checkbox = addOption(container, text, types);
	const auto onlyMy = container->add(
		object_ptr<Ui::SlideWrap<Ui::Checkbox>>(
			container,
			object_ptr<Ui::Checkbox>(
				container,
				tr::lng_export_option_only_my(tr::now),
				((readData().fullChats & types) != types),
				st::defaultBoxCheckbox),
			st::exportSubSettingPadding));

	onlyMy->entity()->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		changeData([&](Settings &data) {
			if (checked) {
				data.fullChats &= ~types;
			} else {
				data.fullChats |= types;
			}
		});
	}, onlyMy->lifetime());

	onlyMy->toggleOn(checkbox->checkedValue());

	if (types & (Type::PublicGroups | Type::PublicChannels)) {
		onlyMy->entity()->setChecked(true);
		onlyMy->entity()->setDisabled(true);
	}
}

void SettingsWidget::addMediaOptions(
	not_null<Ui::VerticalLayout*> container) {
	_mediaBoxes.clear();
	addMediaOption(
		container,
		tr::lng_export_option_photos(tr::now),
		MediaType::Photo);
	addMediaOption(
		container,
		tr::lng_export_option_video_files(tr::now),
		MediaType::Video);
	addMediaOption(
		container,
		tr::lng_export_option_audios(tr::now),
		MediaType::Audio);
	const auto files = addMediaOption(
		container,
		tr::lng_export_option_files(tr::now),
		MediaType::File);
	addMediaOption(
		container,
		tr::lng_export_option_video_messages(tr::now),
		MediaType::VideoMessage);
	addMediaOption(
		container,
		tr::lng_export_option_voice_messages(tr::now),
		MediaType::VoiceMessage);
	addMediaOption(
		container,
		tr::lng_export_option_gifs(tr::now),
		MediaType::GIF);
	addMediaOption(
		container,
		tr::lng_export_option_stickers(tr::now),
		MediaType::Sticker);
	addMediaOption(
		container,
		tr::lng_export_option_polls(tr::now),
		MediaType::Poll);
	addMediaOption(
		container,
		tr::lng_export_option_links(tr::now),
		MediaType::Link);
	addMediaOption(
		container,
		tr::lng_export_option_text_messages(tr::now),
		MediaType::Text);
	addMediaOption(
		container,
		tr::lng_export_option_full_history(tr::now),
		MediaType::FullHistory);
	addExtensionFilter(container);
	addSizeSlider(container);
}

not_null<Ui::Checkbox*> SettingsWidget::addMediaOption(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		MediaType type) {
	const auto checkbox = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			text,
			((readData().media.types & type) == type),
			st::defaultBoxCheckbox),
		(_singlePeerId != 0
			? st::exportSettingPaddingCompact
			: st::exportSettingPadding));
	_mediaBoxes.emplace_back(checkbox, type);
	checkbox->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		changeData([&](Settings &data) {
			if (checked) {
				// Full history and text-only are exclusive modes.
				if (type == MediaType::FullHistory
					|| type == MediaType::Text) {
					data.media.types = type;
				} else {
					data.media.types &= ~MediaType::FullHistory;
					data.media.types &= ~MediaType::Text;
					data.media.types |= type;
				}
			} else {
				data.media.types &= ~type;
			}
		});
		syncMediaBoxes();
	}, checkbox->lifetime());
	return checkbox;
}

void SettingsWidget::syncMediaBoxes() {
	const auto types = readData().media.types;
	for (const auto &[box, type] : _mediaBoxes) {
		const auto should = ((types & type) == type);
		if (box->checked() != should) {
			box->setChecked(
				should,
				Ui::Checkbox::NotifyAboutChange::DontNotify);
		}
	}
}

void SettingsWidget::addExtensionFilter(
		not_null<Ui::VerticalLayout*> container) {
	using ExtMode = MediaSettings::ExtFilterMode;
	using Type = MediaSettings::Type;

	const auto isEligible = [](const Settings &data) {
		return bool(data.media.types
			& (Type::Video | Type::Audio | Type::File
				| Type::Sticker | Type::FullHistory));
	};

	const auto whitelist = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			tr::lng_export_allowed_extensions(tr::now),
			(readData().media.extensionFilterMode == ExtMode::Whitelist),
			st::exportExtCheckboxGreen),
		(_singlePeerId != 0
			? st::exportSettingPaddingCompact
			: st::exportSettingPadding));
	const auto blacklist = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			tr::lng_export_blocked_extensions(tr::now),
			(readData().media.extensionFilterMode == ExtMode::Blacklist),
			st::exportExtCheckboxRed),
		(_singlePeerId != 0
			? st::exportSettingPaddingCompact
			: st::exportSettingPadding));

	auto inputWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::InputField>>(
			container,
			object_ptr<Ui::InputField>(
				container,
				st::exportExtInput,
				rpl::single(QString()))),
		(_singlePeerId != 0
			? st::exportSettingPaddingCompact
			: st::exportSettingPadding));
	const auto input = inputWrap->entity();
	input->setText(readData().media.extensionFilter.join(u" "_q));
	inputWrap->toggle(
		readData().media.extensionFilterMode != ExtMode::None,
		anim::type::instant);

	const auto setMode = [=](ExtMode mode) {
		changeData([&](Settings &data) {
			data.media.extensionFilterMode = mode;
		});
		whitelist->setChecked(mode == ExtMode::Whitelist);
		blacklist->setChecked(mode == ExtMode::Blacklist);
		inputWrap->toggle(mode != ExtMode::None, anim::type::normal);
		if (mode == ExtMode::None) {
			input->setText(QString());
			changeData([&](Settings &data) {
				data.media.extensionFilter.clear();
			});
		}
	};

	whitelist->checkedChanges() | rpl::filter([](bool v) {
		return v;
	}) | rpl::on_next([=] {
		setMode(ExtMode::Whitelist);
	}, whitelist->lifetime());
	blacklist->checkedChanges() | rpl::filter([](bool v) {
		return v;
	}) | rpl::on_next([=] {
		setMode(ExtMode::Blacklist);
	}, blacklist->lifetime());

	input->changes() | rpl::on_next([=] {
		const auto text = input->getLastText().toLower().trimmed();
		const auto parts = text.split(
			QRegularExpression(u"[\\s,;]+"_q),
			Qt::SkipEmptyParts);
		auto exts = QStringList();
		for (const auto &part : parts) {
			exts.append(part.startsWith(u'.') ? part.mid(1) : part);
		}
		changeData([&](Settings &data) {
			data.media.extensionFilter = exts;
		});
	}, input->lifetime());

	value() | rpl::map(isEligible) | rpl::distinct_until_changed() | rpl::on_next([=](bool eligible) {
		whitelist->setEnabled(eligible);
		blacklist->setEnabled(eligible);
		if (!eligible && readData().media.extensionFilterMode != ExtMode::None) {
			setMode(ExtMode::None);
		}
	}, whitelist->lifetime());
}

void SettingsWidget::addSizeSlider(
		not_null<Ui::VerticalLayout*> container) {
	const auto wrap = container->add(
		object_ptr<Ui::FixedHeightWidget>(container));
	const auto slider = container->add(
		object_ptr<Ui::MediaSlider>(container, st::exportFileSizeSlider),
		st::exportFileSizePadding);
	slider->resize(st::exportFileSizeSlider.seekSize);
	slider->setPseudoDiscrete(
		kSizeValueCount,
		SizeLimitByIndex,
		readData().media.sizeLimit,
		[=](int64 limit) {
			changeData([&](Settings &data) {
				data.media.sizeLimit = limit;
			});
		});

	// The label keeps its own row above the slider: the line before it
	// may hold the extension filter field. It stays right-aligned, so
	// it ends where the slider ends.
	const auto label = Ui::CreateChild<Ui::LabelSimple>(
		container.get(),
		st::exportFileSizeLabel);
	value() | rpl::map([](const Settings &data) {
		return data.media.sizeLimit;
	}) | rpl::on_next([=](int64 sizeLimit) {
		const auto limit = sizeLimit / kMegabyte;
		const auto size = QString::number(limit) + " MB";
		label->setText(tr::lng_export_option_size_limit(
			tr::now,
			lt_size,
			size));
		wrap->resize(wrap->width(), label->height());
	}, label->lifetime());
	wrap->resize(wrap->width(), label->height());

	rpl::combine(
		container->widthValue(),
		wrap->geometryValue()
	) | rpl::on_next([=](int width, QRect geometry) {
		label->moveToRight(
			st::exportFileSizePadding.right(),
			geometry.y(),
			width);
	}, label->lifetime());
}

void SettingsWidget::setResumeUpdateEnabled(bool resume, bool update) {
	if (_resumeEnabled == resume && _updateEnabled == update) {
		return;
	}
	_resumeEnabled = resume;
	_updateEnabled = update;
	if (const auto container = _buttonsContainer.data()) {
		refreshButtons(container, _canStart);
	}
}

void SettingsWidget::setStartEnabled(bool enabled) {
	if (_startEnabled == enabled) {
		return;
	}
	_startEnabled = enabled;
	if (const auto container = _buttonsContainer.data()) {
		refreshButtons(container, _canStart);
	}
}

void SettingsWidget::setOptionsEnabled(bool enabled) {
	if (_scroll) {
		_scroll->setEnabled(enabled);
	}
}

void SettingsWidget::refreshButtons(
		not_null<Ui::RpWidget*> container,
		bool canStart) {
	_canStart = canStart;
	_buttonsContainer = container.get();
	container->hideChildren();
	const auto children = container->children();
	for (const auto child : children) {
		if (child->isWidgetType()) {
			child->deleteLater();
		}
	}
	const auto setGrayed = [](Ui::RoundButton *button, bool enabled) {
		button->setEnabled(enabled);
		if (enabled) {
			button->setBrushOverride(std::nullopt);
			button->setTextFgOverride(std::nullopt);
		} else {
			auto bg = button->st().textBg->c;
			bg.setAlpha(70);
			auto fg = button->st().textFg->c;
			fg.setAlpha(110);
			button->setBrushOverride(QBrush(bg));
			button->setTextFgOverride(fg);
		}
		button->update();
	};
	const auto make = [&](rpl::producer<QString> text) {
		const auto button = Ui::CreateChild<Ui::RoundButton>(
			container.get(),
			std::move(text),
			st::defaultBoxButton);
		button->show();
		return button;
	};
	const auto start = make(tr::lng_export_start());
	setGrayed(start, canStart && _startEnabled);
	_startClicks = start->clicks() | rpl::to_empty;

	const auto scan = make(tr::lng_export_scan());
	setGrayed(scan, canStart && _startEnabled);
	_scanClicks = scan->clicks() | rpl::to_empty;

	const auto resume = make(tr::lng_export_resume());
	setGrayed(resume, _resumeEnabled);
	_resumeClicks = resume->clicks() | rpl::to_empty;

	const auto update = make(tr::lng_export_update());
	setGrayed(update, _updateEnabled);
	_updateClicks = update->clicks() | rpl::to_empty;

	const auto cancel = make(tr::lng_cancel());
	_cancelClicks = cancel->clicks() | rpl::to_empty;

	container->sizeValue(
	) | rpl::on_next([=](QSize size) {
		const auto margin = st::defaultBox.buttonPadding.right();
		constexpr auto top = 0;
		const auto gap = st::defaultBox.buttonPadding.left();
		const auto &bst = st::defaultBoxButton;
		const auto need = [&](const QString &text) {
			return bst.style.font->width(text)
				+ bst.height
				- bst.style.font->height;
		};
		const auto widths = {
			need(tr::lng_cancel(tr::now)),
			need(tr::lng_export_update(tr::now)),
			need(tr::lng_export_resume(tr::now)),
			need(tr::lng_export_scan(tr::now)),
			need(tr::lng_export_start(tr::now)),
		};
		const auto total = ranges::accumulate(widths, 4 * gap);
		if (total + 2 * margin <= int(size.width())) {
			auto offset = (int(size.width()) - total) / 2;
			const auto place = [&](Ui::RoundButton *button, int w) {
				button->setFullWidth(w);
				button->moveToLeft(offset, top);
				offset += w + gap;
			};
			auto i = widths.begin();
			place(cancel, *i++);
			place(update, *i++);
			place(resume, *i++);
			place(scan, *i++);
			place(start, *i++);
		} else {
			const auto fair = std::max(
				(int(size.width()) - 2 * margin - 4 * gap) / 5,
				64);
			auto offset = margin;
			const auto place = [&](Ui::RoundButton *button) {
				button->setFullWidth(fair);
				button->moveToLeft(offset, top);
				offset += fair + gap;
			};
			place(cancel);
			place(update);
			place(resume);
			place(scan);
			place(start);
		}
	}, cancel->lifetime());
}

void SettingsWidget::chooseFolder() {
	const auto callback = [=](QString &&result) {
		changeData([&](Settings &data) {
			data.path = std::move(result);
			data.forceSubPath = IsDefaultPath(_session, data.path);
		});
	};
	FileDialog::GetFolder(
		this,
		tr::lng_export_folder(tr::now),
		readData().path,
		callback);
}

rpl::producer<Settings> SettingsWidget::changes() const {
	return _changes.events();
}

rpl::producer<int> SettingsWidget::contentHeightValue() const {
	return _content->heightValue(
	) | rpl::map([=](int height) {
		return height + _buttonsHeight;
	});
}

rpl::producer<Settings> SettingsWidget::value() const {
	return rpl::single(readData()) | rpl::then(changes());
}

rpl::producer<> SettingsWidget::startClicks() const {
	return _startClicks.value(
	) | rpl::map([](Wrap &&wrap) {
		return std::move(wrap.value);
	}) | rpl::flatten_latest();
}

rpl::producer<> SettingsWidget::scanClicks() const {
	return _scanClicks.value(
	) | rpl::map([](Wrap &&wrap) {
		return std::move(wrap.value);
	}) | rpl::flatten_latest();
}

rpl::producer<> SettingsWidget::cancelClicks() const {
	return _cancelClicks.value(
	) | rpl::map([](Wrap &&wrap) {
		return std::move(wrap.value);
	}) | rpl::flatten_latest();
}

rpl::producer<> SettingsWidget::resumeClicks() const {
	return _resumeClicks.value(
	) | rpl::map([](Wrap &&wrap) {
		return std::move(wrap.value);
	}) | rpl::flatten_latest();
}

rpl::producer<> SettingsWidget::updateClicks() const {
	return _updateClicks.value(
	) | rpl::map([](Wrap &&wrap) {
		return std::move(wrap.value);
	}) | rpl::flatten_latest();
}

} // namespace View
} // namespace Export