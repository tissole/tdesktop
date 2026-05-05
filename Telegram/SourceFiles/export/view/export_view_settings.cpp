/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/view/export_view_settings.h"

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
#include <QtCore/QMetaObject>
#include <QtCore/QTimer>
#include "ui/wrap/wrap.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/fade_wrap.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/boxes/calendar_box.h"
#include "ui/boxes/choose_time.h"
#include "ui/text/format_values.h"
#include "platform/platform_specific.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "base/unixtime.h"
#include "main/main_session.h"
#include "styles/style_widgets.h"
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

	_changes.events() | rpl::on_next([=](const Settings &data) {
		const auto old = _internal_data;
		const bool filtersChanged = (data.media.types != old.media.types)
			|| (data.types != old.types)
			|| (data.media.sizeLimit != old.media.sizeLimit);
		const bool rangeChanged = (data.singlePeerFrom != old.singlePeerFrom)
			|| (data.singlePeerTill != old.singlePeerTill)
			|| (data.singlePeerFromId != old.singlePeerFromId)
			|| (data.singlePeerTillId != old.singlePeerTillId)
			|| (data.useIdRange != old.useIdRange);

		if (!_isScanning && (filtersChanged || rangeChanged)) {
			_scanResults.clear();
			if (_scanResultsLabel) {
				_scanResultsLabel->entity()->setText(QString());
				_scanResultsLabel->hide(anim::type::instant);
				_container->resizeToWidth(_container->width());
			}
			_scanInvalidated.fire({});
		}
	}, lifetime());
}

rpl::producer<> SettingsWidget::scanInvalidated() const {
	return _scanInvalidated.events();
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
	_container = content;

	const auto buttons = setupButtons(scroll, wrap);
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
	addChatOption(
		container,
		tr::lng_export_option_personal_chats(tr::now),
		Type::PersonalChats);
	addChatOption(
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
		addLimitsLabel(container);
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

	{
		auto wrap = object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			container,
			object_ptr<Ui::FlatLabel>(
				container,
				QString(),
				st::exportAboutOptionLabel),
			st::exportAboutOptionPadding);
		_scanResultsLabel = wrap.data();
		container->add(std::move(wrap));
	}
	_scanResultsLabel->hide(anim::type::instant);
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

void SettingsWidget::addLimitsLabel(
		not_null<Ui::VerticalLayout*> container) {
	// Add export mode selection (date range or ID range)
	const auto modeGroup = std::make_shared<Ui::RadioenumGroup<bool>>(
		readData().useIdRange ? true : false);

	[[maybe_unused]] const auto dateMode = container->add(
		object_ptr<Ui::Radioenum<bool>>(
			container,
			modeGroup,
			false, // date mode
			tr::lng_export_mode_date(tr::now),
			st::defaultBoxCheckbox),
		st::exportSettingPadding);

	[[maybe_unused]] const auto idMode = container->add(
		object_ptr<Ui::Radioenum<bool>>(
			container,
			modeGroup,
			true, // ID mode
			tr::lng_export_mode_id(tr::now),
			st::defaultBoxCheckbox),
		st::exportSettingPadding);

	modeGroup->setChangedCallback([=](bool useIdRange) {
		changeData([&](Settings &settings) {
			settings.useIdRange = useIdRange;
		});
	});

	value()
		| rpl::map([](const Settings &data) {
			return data.useIdRange;
		})
		| rpl::distinct_until_changed()
		| rpl::on_next([=](bool useIdRange) {
			modeGroup->setValue(useIdRange);
		}, container->lifetime());

	// Date range UI (visible when date mode is selected)
	auto fromDateLink = value() | rpl::map([](const Settings &data) {
		return data.singlePeerFrom;
	}) | rpl::distinct_until_changed(
	) | rpl::map([](TimeId from) {
		return (from
			? rpl::single(langDayOfMonthFull(
				base::unixtime::parse(from).date()))
			: tr::lng_export_beginning()
		) | rpl::map(tr::url(u"internal:edit_from"_q));
	}) | rpl::flatten_latest();

	const auto mapToTime = [](TimeId id, const QString &link) {
		return rpl::single(id
			? base::unixtime::parse(id).time().toString(u"HH:mm:ss"_q)
			: QString()
		) | rpl::map(tr::url(link));
	};

	auto fromTimeLink = value() | rpl::map([](const Settings &data) {
		return data.singlePeerFrom;
	}) | rpl::distinct_until_changed(
	) | rpl::map([=](TimeId from) {
		return mapToTime(from, u"internal:edit_from_time"_q);
	}) | rpl::flatten_latest();

	auto fromLink = rpl::combine(
		std::move(fromDateLink),
		std::move(fromTimeLink)
	) | rpl::map([](TextWithEntities date, TextWithEntities link) {
		return link.text.isEmpty()
			? date
			: date.append(u", "_q).append(std::move(link));
	});

	auto tillDateLink = value() | rpl::map([](const Settings &data) {
		return data.singlePeerTill;
	}) | rpl::distinct_until_changed(
	) | rpl::map([](TimeId till) {
		return (till
			? rpl::single(langDayOfMonthFull(
				base::unixtime::parse(till).date()))
			: tr::lng_export_end()
		) | rpl::map(tr::url(u"internal:edit_till"_q));
	}) | rpl::flatten_latest();

	auto tillTimeLink = value() | rpl::map([](const Settings &data) {
		return data.singlePeerTill;
	}) | rpl::distinct_until_changed(
	) | rpl::map([=](TimeId till) {
		return mapToTime(till, u"internal:edit_till_time"_q);
	}) | rpl::flatten_latest();

	const auto concat = [](TextWithEntities date, TextWithEntities link) {
		return link.text.isEmpty()
			? date
			: date.append(u", "_q).append(std::move(link));
	};

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

	const auto dateLabelWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			container,
			object_ptr<Ui::FlatLabel>(
				container,
				std::move(datesText),
				st::boxLabel),
			st::exportLimitsPadding));
	const auto dateLabel = dateLabelWrap->entity();

    //const int idLabelH = st::exportIdFieldLabel.maxHeight;
    const int idLabelH = st::exportIdFieldLabel.style.font->height - 2;
    //const int idInputH = st::defaultInputField.heightMin;
    const int idInputH = st::exportIdInput.heightMin;
    const int idTopPad = st::exportSettingPadding.top();
    const int idPadL = st::exportSettingPadding.left();
    const int idGap = 16;
    const int idRowH = idTopPad + idLabelH + 2 + idInputH;
    
    const auto idContainerWrap = container->add(
        object_ptr<Ui::SlideWrap<Ui::FixedHeightWidget>>(
            container,
            object_ptr<Ui::FixedHeightWidget>(container, idRowH),
            style::margins(0, 0, 0, 0)));
    const auto idContainer = idContainerWrap->entity();

//	const int inputH = st::defaultInputField.heightMin;
//	const int labelH = st::exportIdFieldLabel.style.font->height;
//	const int idPadL = 22;
//	const int idGap  = 16;
//	const int idRowH = labelH + inputH; 
//
//
//	const auto idContainerWrap = container->add(
//		object_ptr<Ui::SlideWrap<Ui::FixedHeightWidget>>(
//			container,
//			object_ptr<Ui::FixedHeightWidget>(container, idRowH),
//			style::margins(0, 0, 0, 0)));
//	const auto idContainer = idContainerWrap->entity();

	const auto fromIdLabel = Ui::CreateChild<Ui::FlatLabel>(
		idContainer,
		tr::lng_export_id_from_placeholder(tr::now),
		st::exportIdFieldLabel);

//	const auto fromIdInput = Ui::CreateChild<Ui::InputField>(
//		idContainer,
//		st::defaultInputField,
//		rpl::single(QString()),
//		u"Message ID"_q);

    const auto fromIdInput = Ui::CreateChild<Ui::InputField>(
        idContainer,
        st::exportIdInput,
        rpl::single(QString()),
        u"Message ID"_q);

	const auto tillIdLabel = Ui::CreateChild<Ui::FlatLabel>(
		idContainer,
		tr::lng_export_id_till_placeholder(tr::now),
		st::exportIdFieldLabel);

//	const auto tillIdInput = Ui::CreateChild<Ui::InputField>(
//		idContainer,
//		st::defaultInputField,
//		rpl::single(QString()),
//		u"Message ID"_q);

    const auto tillIdInput = Ui::CreateChild<Ui::InputField>(
        idContainer,
        st::exportIdInput,
        rpl::single(QString()),
        u"Message ID"_q);

    const auto layoutIdRow = [=](int w) {
        const int half = (w - idPadL * 2 - idGap) / 2;
        if (half < 20) return;
        const int labelY = idTopPad;
        const int inputY = idTopPad + idLabelH + 0;
        fromIdLabel->resizeToWidth(half);
        fromIdLabel->move(idPadL, labelY);
        fromIdInput->setGeometry(idPadL, inputY, half, idInputH);
        const int x2 = idPadL + half + idGap;
        tillIdLabel->resizeToWidth(half);
        tillIdLabel->move(x2, labelY);
        tillIdInput->setGeometry(x2, inputY, half, idInputH);
    };

//	const auto layoutIdRow = [=](int w) {
//		const int half = (w - idPadL * 2 - idGap) / 2;
//		if (half < 20) return;
//		fromIdLabel->resizeToWidth(half);
//		fromIdLabel->move(idPadL, 0);
//		fromIdInput->setGeometry(idPadL, labelH, half, inputH);
//		const int x2 = idPadL + half + idGap;
//		tillIdLabel->resizeToWidth(half);
//		tillIdLabel->move(x2, 0);
//		tillIdInput->setGeometry(x2, labelH, half, inputH);
//	};
    
	idContainer->widthValue()
		| rpl::on_next([=](int w) { layoutIdRow(w); }, idContainer->lifetime());



	// Bind inputs to data
	value()
		| rpl::map([](const Settings &data) {
			return data.singlePeerFromId;
		})
		| rpl::on_next([=](int32 fromId) {
			const auto s = (fromId > 0) ? QString::number(fromId) : QString();
			if (fromIdInput->getLastText() != s) {
				fromIdInput->setText(s);
			}
		}, fromIdInput->lifetime());

	value()
		| rpl::map([](const Settings &data) {
			return data.singlePeerTillId;
		})
		| rpl::on_next([=](int32 tillId) {
			const auto s = (tillId > 0) ? QString::number(tillId) : QString();
			if (tillIdInput->getLastText() != s) {
				tillIdInput->setText(s);
			}
		}, tillIdInput->lifetime());

	// errorLabel also in a SlideWrap so it collapses fully when hidden.
	const auto errorLabelWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			container,
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(QString()),
				st::exportErrorLabel),
			st::exportSettingPadding));
	const auto errorLabel = errorLabelWrap->entity();

	// Handle input changes with validation
	fromIdInput->changes() | rpl::on_next([=] {
		errorLabel->setText(QString());
		const auto text = fromIdInput->getLastText();
		bool ok = false;
		const auto value = text.toInt(&ok);

		if (!ok && !text.isEmpty()) {
			errorLabel->setText(tr::lng_export_error_invalid_id(tr::now));
			return;
		} else if (ok && value < 1) {
			errorLabel->setText(tr::lng_export_error_from_invalid(tr::now));
			return;
		} else if (!ok) {
			changeData([&](Settings &settings) {
				settings.singlePeerFromId = 0;
			});
			return;
		}

		const auto currentTillId = readData().singlePeerTillId;
		if (currentTillId > 0 && value > currentTillId) {
			errorLabel->setText(tr::lng_export_error_from_too_high(tr::now));
			return;
		}

		changeData([&](Settings &settings) {
			settings.singlePeerFromId = value;
		});
	}, fromIdInput->lifetime());

	tillIdInput->changes() | rpl::on_next([=] {
		errorLabel->setText(QString());
		const auto text = tillIdInput->getLastText();
		bool ok = false;
		const auto value = text.toInt(&ok);

		if (!ok || value < 1) {
			errorLabel->setText(tr::lng_export_error_invalid_id(tr::now));
			return;
		}

		const auto currentFromId = readData().singlePeerFromId;
		if (currentFromId > 0 && value < currentFromId) {
			errorLabel->setText(tr::lng_export_error_till_too_low(tr::now));
			return;
		}

		changeData([&](Settings &settings) {
			settings.singlePeerTillId = value;
		});
	}, tillIdInput->lifetime());

	// Toggle visibility based on mode — using SlideWrap::toggle() so the
	// hidden widget's height is removed from the layout entirely (setVisible
	// alone leaves the height as dead space).
	value()
		| rpl::map([](const Settings &data) {
			return data.useIdRange;
		})
		| rpl::on_next([=](bool useIdRange) {
			dateLabelWrap->toggle(!useIdRange, anim::type::instant);
			idContainerWrap->toggle(useIdRange, anim::type::instant);
			errorLabelWrap->toggle(useIdRange, anim::type::instant);
			if (useIdRange) {
				layoutIdRow(idContainer->width());
			}
			container->resizeToWidth(container->width());
		}, container->lifetime());

	// Initially set visibility.
	// Must call resizeToWidth() afterwards so the VerticalLayout re-queries
	// each SlideWrap's height — without it the collapsed wraps still occupy
	// their full height on the first paint, causing the visible gap.
	dateLabelWrap->toggle(!readData().useIdRange, anim::type::instant);
	idContainerWrap->toggle(readData().useIdRange, anim::type::instant);
	errorLabelWrap->toggle(readData().useIdRange, anim::type::instant);
	container->resizeToWidth(container->width());

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

	dateLabel->overrideLinkClickHandler([=](const QString &url) {
		if (url == u"internal:edit_from"_q) {
			const auto done = [=](TimeId limit) {
				changeData([&](Settings &settings) {
					settings.singlePeerFrom = limit;
				});
			};
			editDateLimit(
				readData().singlePeerFrom,
				0,
				readData().singlePeerTill,
				tr::lng_export_from_beginning(),
				done);
		} else if (url == u"internal:edit_from_time"_q) {
			const auto now = [=] {
				auto result = TimeId(0);
				changeData([&](Settings &settings) {
					result = settings.singlePeerFrom;
				});
				return result;
			};
			const auto done = [=](TimeId time) {
				changeData([&](Settings &settings) {
					const auto result = time
						+ removeTime(settings.singlePeerFrom);
					if (result > settings.singlePeerTill
							&& settings.singlePeerTill) {
						settings.singlePeerFrom = settings.singlePeerTill;
					} else {
						settings.singlePeerFrom = result;
					}
				});
			};
			editTimeLimit(now, done);
		} else if (url == u"internal:edit_till"_q) {
			const auto done = [=](TimeId limit) {
				changeData([&](Settings &settings) {
					const auto endOfDay = limit + 86399;
					if (endOfDay < settings.singlePeerFrom
							&& settings.singlePeerFrom) {
						settings.singlePeerTill = settings.singlePeerFrom;
					} else {
						settings.singlePeerTill = endOfDay;
					}
				});
			};
			editDateLimit(
				readData().singlePeerTill,
				readData().singlePeerFrom,
				0,
				tr::lng_export_till_end(),
				done);
		} else if (url == u"internal:edit_till_time"_q) {
			const auto now = [=] {
				auto result = TimeId(0);
				changeData([&](Settings &settings) {
					result = settings.singlePeerTill;
				});
				return result;
			};
		const auto done = [=](TimeId time) {
			changeData([&](Settings &settings) {
				const auto result = time + removeTime(settings.singlePeerTill);
				if (result < settings.singlePeerFrom
						&& settings.singlePeerFrom) {
					settings.singlePeerTill = settings.singlePeerFrom;
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
}

void SettingsWidget::editDateLimit(
		TimeId current,
		TimeId min,
		TimeId max,
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
		done(base::unixtime::serialize(date.startOfDay()));
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
	const auto buttonsPadding = st::defaultBox.buttonPadding;
	const auto buttonsHeight = buttonsPadding.top()
		+ st::defaultBoxButton.height
		+ buttonsPadding.bottom();
	const auto buttons = Ui::CreateChild<Ui::FixedHeightWidget>(
		this,
		buttonsHeight);
	const auto topShadow = Ui::CreateChild<Ui::FadeShadow>(this);
	const auto bottomShadow = Ui::CreateChild<Ui::FadeShadow>(this);
	topShadow->toggleOn(
		scroll->scrollTopValue() | rpl::map(rpl::mappers::_1 > 0)
	);
	bottomShadow->toggleOn(
		rpl::combine(
			scroll->heightValue(),
			scroll->scrollTopValue(),
			wrap->heightValue()
		) | rpl::map([=](int height, int top, int contentHeight) {
			return top < scroll->scrollTopMax();
		})
	);

	_buttonsContainer = buttons;

	value() | rpl::map([=](const Settings &data) {
		const auto extFilterActive = (data.media.extensionFilterMode != MediaSettings::ExtFilterMode::None);
		const auto extFilterEmpty = data.media.extensionFilter.isEmpty();
		if (extFilterActive && extFilterEmpty) {
			return false;
		}
		if (data.onlySinglePeer()) {
			return (data.media.types != MediaSettings::Types(0));
		}
		return (data.types != Types(0))
			|| (data.media.types != MediaSettings::Types(0));
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
				data.media.types &= ~MediaType::FullHistory;
				data.media.types &= ~MediaType::Link;
				data.types |= types;
			} else {
				data.types &= ~types;
			}
		});
	}, checkbox->lifetime());

	value() | rpl::map([=](const Settings &data) {
		const bool checked = (data.types & types) == types;
		const bool linkSelected = (data.media.types & MediaType::Link);
		const bool historySelected = (data.media.types & MediaType::FullHistory);
		const bool enabled = !linkSelected && !historySelected;
		return std::make_pair(checked, enabled);
	}) | rpl::distinct_until_changed(
	) | rpl::on_next([=](std::pair<bool, bool> state) {
		checkbox->setChecked(state.first);
		checkbox->setEnabled(state.second);
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

	value() | rpl::map([=](const Settings &data) {
		const bool checked = (data.fullChats & types) != types;
		const bool linkSelected = (data.media.types & MediaType::Link);
		const bool historySelected = (data.media.types & MediaType::FullHistory);
		const bool enabled = !linkSelected && !historySelected;
		return std::make_pair(checked, enabled);
	}) | rpl::distinct_until_changed(
	) | rpl::on_next([=](std::pair<bool, bool> state) {
		onlyMy->entity()->setChecked(state.first);
		onlyMy->entity()->setEnabled(state.second);
	}, onlyMy->lifetime());

	onlyMy->toggleOn(checkbox->checkedValue());
}

void SettingsWidget::addMediaOptions(
		not_null<Ui::VerticalLayout*> container) {
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
		tr::lng_export_option_video_messages(tr::now),
		MediaType::VideoMessage);
	addMediaOption(
		container,
		tr::lng_export_option_audios(tr::now),
		MediaType::Audio);
	addMediaOption(
		container,
		tr::lng_export_option_voice_messages(tr::now),
		MediaType::VoiceMessage);
	addMediaOption(
		container,
		tr::lng_export_option_files(tr::now),
		MediaType::File);
	addMediaOption(
		container,
		tr::lng_export_option_stickers(tr::now),
		MediaType::Sticker);
	addMediaOption(
		container,
		tr::lng_export_option_gifs(tr::now),
		MediaType::GIF);
	addMediaOption(
		container,
		tr::lng_export_option_text_messages(tr::now),
		MediaType::Text);
	addMediaOption(
		container,
		tr::lng_export_option_links(tr::now),
		MediaType::Link);
	addMediaOption(
		container,
		tr::lng_export_option_full_history(tr::now),
		MediaType::FullHistory);
	addExtensionFilter(container);
	addSizeSlider(container);

	{
		auto wrap = object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
			container,
			object_ptr<Ui::FlatLabel>(
				container,
				QString(),
				st::exportAboutOptionLabel),
			st::exportAboutOptionPadding);
		_scanResultsLabel = wrap.data();
		container->add(std::move(wrap));
	}
	_scanResultsLabel->hide(anim::type::instant);
}

void SettingsWidget::addMediaOption(
		not_null<Ui::VerticalLayout*> container,
		const QString &text,
		MediaType type) {
	const auto checkbox = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			text,
			((readData().media.types & type) == type),
			st::defaultBoxCheckbox),
		st::exportSettingPadding);
	checkbox->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		changeData([&](Settings &data) {
			if (checked) {
				if (type == MediaType::FullHistory) {
					data.media.types = MediaType::FullHistory;
					data.types = Settings::Types(0);
				} else if (type == MediaType::Link) {
					data.media.types = MediaType::Link;
					data.types = Settings::Types(0);
				} else {
					data.media.types &= ~MediaType::FullHistory;
					data.media.types &= ~MediaType::Link;
					data.media.types |= type;
				}
			} else {
				data.media.types &= ~type;
			}
		});
	}, checkbox->lifetime());

	value() | rpl::map([=](const Settings &data) {
		const bool checked = (data.media.types & type) == type;
		return std::make_pair(checked, true);
	}) | rpl::distinct_until_changed(
	) | rpl::on_next([=](std::pair<bool, bool> state) {
		checkbox->setChecked(state.first);
		checkbox->setEnabled(state.second);
	}, checkbox->lifetime());
}

void SettingsWidget::addExtensionFilter(
		not_null<Ui::VerticalLayout*> container) {
	using ExtMode = MediaSettings::ExtFilterMode;
	using Type    = MediaSettings::Type;

	const auto isEligible = [](const Settings &data) {
		return bool(data.media.types & (Type::Video | Type::Audio | Type::File | Type::Sticker | Type::FullHistory));
	};

	// ── Whitelist checkbox — label IS the ✓ sign, coloured green ──
	const auto wlCb = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			QString(u"\u2713  whitelist"_q),
			(readData().media.extensionFilterMode == ExtMode::Whitelist),
			st::exportExtCheckboxGreen),
		st::exportSettingPadding);

	// ── Blacklist checkbox — label IS the ✕ sign, coloured red ──
	const auto blCb = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			QString(u"\u2715  blacklist"_q),
			(readData().media.extensionFilterMode == ExtMode::Blacklist),
			st::exportExtCheckboxRed),
		st::exportSettingPadding);

	// ── Extension input — shown only when a filter mode is active ──
	//const auto inputWrap = container->add(
	//	object_ptr<Ui::SlideWrap<Ui::InputField>>(
	//		container,
	//		object_ptr<Ui::InputField>(
	//			container,
	//			st::exportExtInput,
	//			rpl::single(QString(u"pdf, docx, mp4..."_q))))),
	//	style::margins{ st::exportSubSettingPadding.left(), -10, st::exportSubSettingPadding.right(), 0 };
	//const auto input = inputWrap->entity();

    auto inputField = object_ptr<Ui::InputField>(
        container,
        st::exportExtInput,
        rpl::single(QString(u"pdf, docx, mp4..."_q)));
    auto slideWrap = object_ptr<Ui::SlideWrap<Ui::InputField>>(
        container,
        std::move(inputField));
    const auto inputWrap = container->add(
        std::move(slideWrap),
        //style::margins{ st::exportSubSettingPadding.left(), -10, st::exportSubSettingPadding.right(), 0 });
        style::margins{ st::exportSettingPadding.left(), -2, st::exportSettingPadding.right(), 0 });
    const auto input = inputWrap->entity();

	// Set initial text
	input->setText(readData().media.extensionFilter.join(u" "_q));
	inputWrap->toggle(
		readData().media.extensionFilterMode != ExtMode::None,
		anim::type::instant);

	// ── Mutual exclusion + mode update ──
	wlCb->checkedChanges()
		| rpl::filter([](bool v) { return v; })
		| rpl::on_next([=] {
			blCb->setChecked(false);
			changeData([&](Settings &data) {
				data.media.extensionFilterMode = ExtMode::Whitelist;
			});
		}, wlCb->lifetime());

	wlCb->checkedChanges()
		| rpl::filter([](bool v) { return !v; })
		| rpl::on_next([=] {
			if (!blCb->checked()) {
				input->setText(QString());
				changeData([&](Settings &data) {
					data.media.extensionFilterMode = ExtMode::None;
					data.media.extensionFilter.clear();
				});
			}
		}, wlCb->lifetime());

	blCb->checkedChanges()
		| rpl::filter([](bool v) { return v; })
		| rpl::on_next([=] {
			wlCb->setChecked(false);
			changeData([&](Settings &data) {
				data.media.extensionFilterMode = ExtMode::Blacklist;
			});
		}, blCb->lifetime());

	blCb->checkedChanges()
		| rpl::filter([](bool v) { return !v; })
		| rpl::on_next([=] {
			if (!wlCb->checked()) {
				input->setText(QString());
				changeData([&](Settings &data) {
					data.media.extensionFilterMode = ExtMode::None;
					data.media.extensionFilter.clear();
				});
			}
		}, blCb->lifetime());

	// ── Show/hide input when mode changes ──
	value()
		| rpl::map([](const Settings &data) {
			return data.media.extensionFilterMode != ExtMode::None;
		})
		| rpl::distinct_until_changed()
		| rpl::on_next([=](bool on) {
			inputWrap->toggle(on, anim::type::normal);
		}, inputWrap->lifetime());

	// ── Parse and save extensions on input change ──
	input->changes()
		| rpl::on_next([=] {
			const auto text = input->getLastText().toLower().trimmed();
			const auto parts = text.split(
				QRegularExpression(u"[\\s,;]+"_q),
				Qt::SkipEmptyParts);
			QStringList exts;
			for (const auto &p : parts) {
				exts.append(p.startsWith('.') ? p.mid(1) : p);
			}
			changeData([&](Settings &data) {
				data.media.extensionFilter = exts;
			});
		}, input->lifetime());

	// ── Sync checkboxes when data changes externally ──
	value()
		| rpl::map([](const Settings &data) {
			return data.media.extensionFilterMode;
		})
		| rpl::distinct_until_changed()
		| rpl::on_next([=](ExtMode mode) {
			wlCb->setChecked(mode == ExtMode::Whitelist);
			blCb->setChecked(mode == ExtMode::Blacklist);
			if (mode == ExtMode::None) {
				input->setText(QString());
			}
		}, wlCb->lifetime());

	// ── Enable/disable based on eligible types ──
	value()
		| rpl::map(isEligible)
		| rpl::distinct_until_changed()
		| rpl::on_next([=](bool eligible) {
			wlCb->setEnabled(eligible);
			blCb->setEnabled(eligible);
			if (!eligible) {
				// Clear filter when no eligible type is selected
				wlCb->setChecked(false);
				blCb->setChecked(false);
				input->setText(QString());
				changeData([&](Settings &data) {
					data.media.extensionFilterMode = ExtMode::None;
					data.media.extensionFilter.clear();
				});
			}
		}, wlCb->lifetime());

	// Add spacing before the size slider to prevent overlap with the input field label
	container->add(object_ptr<Ui::FixedHeightWidget>(container, 25));
}

void SettingsWidget::addSizeSlider(
		not_null<Ui::VerticalLayout*> container) {
	using namespace rpl::mappers;

	const auto slider = container->add(
		object_ptr<Ui::MediaSlider>(container, st::exportFileSizeSlider),
		st::exportFileSizePadding);
	slider->resize(st::exportFileSizeSlider.seekSize);

	const auto sectionsCount = (kSizeValueCount - 1);
	const auto getIndexByLimit = [](int64 sizeLimit) {
		for (auto index = 0; index != kSizeValueCount; ++index) {
			if (sizeLimit <= SizeLimitByIndex(index)) {
				return index;
			}
		}
		return kSizeValueCount - 1;
	};

	slider->setPseudoDiscrete(
		kSizeValueCount,
		SizeLimitByIndex,
		readData().media.sizeLimit,
		[=](int64 limit) {
			changeData([&](Settings &data) {
				data.media.sizeLimit = limit;
			});
		});

	const auto label = Ui::CreateChild<Ui::LabelSimple>(
		container.get(),
		st::exportFileSizeLabel);
	value() | rpl::map([](const Settings &data) {
		return std::make_pair(data.media.sizeLimit, data.media.types);
	}) | rpl::distinct_until_changed(
	) | rpl::on_next([=](std::pair<int64, MediaSettings::Types> state) {
		const auto sizeLimit = state.first;
		const auto types = state.second;
		const auto sizeRelevant = MediaSettings::Type::Photo
			| MediaSettings::Type::Video
			| MediaSettings::Type::File
			| MediaSettings::Type::Audio
			| MediaSettings::Type::VoiceMessage
			| MediaSettings::Type::VideoMessage
			| MediaSettings::Type::Sticker
			| MediaSettings::Type::GIF
			| MediaSettings::Type::FullHistory;
		const auto disabled = !(types & sizeRelevant);
		slider->setDisabled(disabled);

		const auto limit = sizeLimit / kMegabyte;
		const auto size = QString::number(limit) + " MB";
		const auto text = tr::lng_export_option_size_limit(
			tr::now,
			lt_size,
			size);
		label->setText(text);

		const auto index = getIndexByLimit(sizeLimit);
		const auto pos = index / float64(sectionsCount);
		if (std::abs(slider->value() - pos) > 0.001) {
			slider->setValue(pos);
		}
	}, slider->lifetime());

	rpl::combine(
		label->widthValue(),
		slider->geometryValue(),
		_2
	) | rpl::on_next([=](QRect geometry) {
		label->moveToRight(
			st::exportFileSizePadding.right(),
			//geometry.y() - label->height() - st::exportFileSizeLabelBottom);
            geometry.y() - label->height() - 4);
	}, label->lifetime());
}

void SettingsWidget::updateButtonsLayout() {
	if (!_buttonsContainer || !_exportButtonForLayout || !_scanButtonForLayout
			|| !_cancelButtonForLayout || !_resumeButton) {
		return;
	}
	// Ensure the buttons container has the correct width before computing layout.
	// This is critical because moveToRight() uses the container's actual width.
	if (_buttonsContainer->parentWidget()) {
		int parentWidth = _buttonsContainer->parentWidget()->width();
		if (parentWidth > 0 && _buttonsContainer->width() != parentWidth) {
			_buttonsContainer->resizeToWidth(parentWidth);
		}
	}
	// Use container's width for safety check (it should now be correct)
	int containerWidth = _buttonsContainer->width();
	if (containerWidth <= 0) {
		// Container not sized yet - retry after a delay
		QTimer::singleShot(100, this, [=]() {
			updateButtonsLayout();
		});
		return;
	}
	// Force Qt to update geometry of all buttons before we compute positions
	// This ensures widths are current (important after show/hide calls)
	_resumeButton->adjustSize();
	_updateButton->adjustSize();
	_exportButtonForLayout->adjustSize();
	_scanButtonForLayout->adjustSize();
	_cancelButtonForLayout->adjustSize();
	_buttonsContainer->updateGeometry();

	const auto padding = st::defaultBox.buttonPadding;
	const auto right = padding.right();
	const auto top = padding.top();

	// Export is always rightmost
	_exportButtonForLayout->moveToRight(right, top);
	int nextRight = right + _exportButtonForLayout->width() + padding.left();

	// Resume or Update is between Export and Scan (only if NOT hidden)
	if (!_resumeButton->isHidden()) {
		_resumeButton->moveToRight(nextRight, top);
		nextRight += _resumeButton->width() + padding.left();
	}
	if (!_updateButton->isHidden()) {
		_updateButton->moveToRight(nextRight, top);
		nextRight += _updateButton->width() + padding.left();
	}

	// Scan is next
	_scanButtonForLayout->moveToRight(nextRight, top);

	// Cancel is leftmost
	_cancelButtonForLayout->moveToRight(
		nextRight + _scanButtonForLayout->width() + padding.left(), top);
}

void SettingsWidget::refreshButtons(
		not_null<Ui::RpWidget*> container,
		bool canStart) {
	using namespace rpl::mappers;

	// Cancel the old sizeValue subscription BEFORE reparenting old buttons.
	// If we cancel after setParent(nullptr), a pending resize event can fire
	// the old lambda and call moveToRight on a widget with no parent → crash.
	_buttonsLayout.destroy();

	for (const auto child : container->children()) {
		if (child->isWidgetType()) {
			auto widget = static_cast<QWidget*>(child);
			widget->hide(); // Hide before reparenting to prevent floating widgets
			widget->setParent(nullptr);
			child->deleteLater();
		}
	}

	const auto mediaTypesSelected = (readData().media.types != MediaSettings::Types(0));

	const auto export_ = Ui::CreateChild<Ui::RoundButton>(
		container.get(),
		tr::lng_export_start(),
		st::defaultBoxButton);
	export_->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	export_->show();
	export_->clicks() | rpl::to_empty | rpl::start_to_stream(_exportClicks, export_->lifetime());
	_exportButtonForLayout = export_;

	// Resume button (always created, shown/hidden based on _existingExport)
	{
		const auto resume = Ui::CreateChild<Ui::RoundButton>(
			container.get(),
			rpl::single(QString("Resume")),
			st::defaultBoxButton);
		resume->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		resume->clicks() | rpl::to_empty | rpl::start_to_stream(_resumeClicks, resume->lifetime());
		if (_existingExport == ExistingExport::Incomplete) {
			resume->show();
		} else {
			resume->hide();
		}
		_resumeButton = resume;
	}

	// Update button (always created, shown/hidden based on _existingExport)
	{
		const auto update = Ui::CreateChild<Ui::RoundButton>(
			container.get(),
			rpl::single(QString("Update")),
			st::defaultBoxButton);
		update->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		update->clicks() | rpl::to_empty | rpl::start_to_stream(_updateClicks, update->lifetime());
		if (_existingExport == ExistingExport::Complete) {
			update->show();
		} else {
			update->hide();
		}
		_updateButton = update;
	}

	const auto scan = Ui::CreateChild<Ui::RoundButton>(
		container.get(),
		rpl::single(_isScanning ? tr::lng_export_scanning(tr::now) : tr::lng_export_scan(tr::now)),
		st::defaultBoxButton);
	scan->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	scan->show();
	scan->clicks() | rpl::to_empty | rpl::start_to_stream(_scanClicks, scan->lifetime());
	_scanButtonForLayout = scan;

	const auto cancel = Ui::CreateChild<Ui::RoundButton>(
		container.get(),
		tr::lng_cancel(),
		st::defaultBoxButton);
	cancel->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	cancel->show();
	cancel->clicks() | rpl::to_empty | rpl::on_next([=] {
		_cancelClicks.fire({});
	}, cancel->lifetime());
	_cancelButtonForLayout = cancel;

	// State management
	if (_isScanning) {
		export_->setDisabled(true);
		_resumeButton->setDisabled(true);
		_updateButton->setDisabled(true);
		scan->setDisabled(true);
	} else if (_hasScanResults) {
		export_->setDisabled(!canStart);
		_resumeButton->setDisabled(!canStart);
		_updateButton->setDisabled(!canStart);
		scan->setDisabled(true);
	} else {
		export_->setDisabled(!canStart);
		_resumeButton->setDisabled(!canStart);
		_updateButton->setDisabled(!canStart);
		scan->setDisabled(!canStart || !mediaTypesSelected);
	}

	// Layout is handled by:
	// 1. rpl::combine below (fires on container/button size changes)
	// 2. setExistingExport() QTimer::singleShot(0) callback

	rpl::combine(
		container->sizeValue(),
		export_->widthValue(),
		scan->widthValue(),
		_resumeButton->widthValue(),
		_resumeButton->shownValue(),
		_updateButton->widthValue(),
		_updateButton->shownValue()
	) | rpl::on_next([=](QSize, int, int, int, bool, int, bool) {
		updateButtonsLayout();
	}, _buttonsLayout);
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

rpl::producer<Settings> SettingsWidget::value() const {
	return rpl::single(readData()) | rpl::then(changes());
}

rpl::producer<> SettingsWidget::scanClicks() const {
	return _scanClicks.events();
}

rpl::producer<> SettingsWidget::exportClicks() const {
	return _exportClicks.events();
}

rpl::producer<> SettingsWidget::resumeClicks() const {
	return _resumeClicks.events();
}

rpl::producer<> SettingsWidget::updateClicks() const {
	return _updateClicks.events();
}

rpl::producer<> SettingsWidget::cancelClicks() const {
	return _cancelClicks.events();
}

void SettingsWidget::setScanProgress(int itemIndex, int itemCount) {
	if (!_scanResultsLabel) return;
	_scanResultsLabel->entity()->setText(tr::lng_export_scanning_progress(
		tr::now,
		lt_index,
		Lang::FormatCountDecimal(itemIndex),
		lt_amount,
		Lang::FormatCountDecimal(itemCount)));
	_scanResultsLabel->toggle(true, anim::type::instant);
	_container->resizeToWidth(_container->width());
}

void SettingsWidget::setScanning(bool scanning) {
	if (_isScanning != scanning) {
		_isScanning = scanning;
		if (scanning) {
			_hasScanResults = false;
			_scanResults.clear();
		}
	}
}

void SettingsWidget::setExistingExport(ExistingExport state) {
	if (_existingExport == state) {
		return;
	}
	_existingExport = state;
	if (_resumeButton && _updateButton) {
		if (state == ExistingExport::Incomplete) {
			_resumeButton->show();
			_updateButton->hide();
		} else if (state == ExistingExport::Complete) {
			_resumeButton->hide();
			_updateButton->show();
		} else {
			_resumeButton->hide();
			_updateButton->hide();
		}
		// Defer layout update to ensure button visibility change is processed
		QTimer::singleShot(0, this, [=]() {
			if (_buttonsContainer && _buttonsContainer->parentWidget()
					&& _buttonsContainer->parentWidget()->width() > 0) {
				_buttonsContainer->resizeToWidth(_buttonsContainer->parentWidget()->width());
			}
			updateButtonsLayout();
		});
	}
}

void SettingsWidget::setScanResults(std::map<MediaSettings::Type, Output::StatItem> stats) {
	setScanning(false);

	// We use our own calculated totals
	int totalUniqueMessagesCount = 0;
	int totalTotalMessagesCount = 0;
	for (const auto &[type, stat] : stats) {
		totalTotalMessagesCount += stat.localTotalCount;
	}

	if (totalTotalMessagesCount <= 0) {
		setScanning(false);
		using MediaType = MediaSettings::Type;
		const auto types = readData().media.types;
		const auto hasMedia = (types & MediaType::MediaMask) || (types & MediaType::Sticker) || (types & MediaType::GIF) || (types & MediaType::File);
		const auto textOnly = (types == MediaType::Text);
		const auto linksOnly = (types == MediaType::Link);
		const auto textAndLinks = (types == (MediaType::Text | MediaType::Link));

		QString text;
		if (hasMedia) {
			text = tr::lng_export_none_found(tr::now);
		} else if (textOnly || linksOnly || textAndLinks) {
			text = "No messages found in this range.";
		} else {
			text = "No items found matching selected filters.";
		}
		_scanResultsLabel->entity()->setText(text);
		_scanResultsLabel->toggle(true, anim::type::instant);
		_container->resizeToWidth(_container->width());
		return;
	}

	_scanResults = std::move(stats);
	_hasScanResults = true;
	_changes.fire_copy(readData());
	if (!_scanResultsLabel) return;

	QString text;
	totalUniqueMessagesCount = 0;
	totalTotalMessagesCount = 0;
	int64 totalUniqueMediaSize = 0;
	int64 totalMediaSize = 0;
	const auto fullHistory = (readData().media.types & MediaSettings::Type::FullHistory);
	const auto fullRange = (readData().singlePeerFrom == 0 && readData().singlePeerTill == 0)
		&& !readData().useIdRange;
	const auto showAllCategories = fullHistory && fullRange;

	using MediaType = MediaSettings::Type;
	const std::vector<MediaType> order = {
		MediaType::Photo,
		MediaType::Video,
		MediaType::VideoMessage,
		MediaType::Audio,
		MediaType::VoiceMessage,
		MediaType::File,
		MediaType::Sticker,
		MediaType::GIF,
		MediaType::Text,
		MediaType::Link
	};

	int categoriesCount = 0;

	for (const auto type : order) {
		const auto it = _scanResults.find(type);
		if (!showAllCategories && (it == _scanResults.end() || it->second.localTotalCount <= 0)) {
			continue;
		}
		const auto &item = (it != _scanResults.end()) ? it->second : Output::StatItem();
		QString label;
		switch (type) {
		case MediaType::Photo: label = tr::lng_export_option_photos(tr::now); break;
		case MediaType::Video: label = tr::lng_export_option_video_files(tr::now); break;
		case MediaType::VoiceMessage: label = tr::lng_export_option_voice_messages(tr::now); break;
		case MediaType::VideoMessage: label = tr::lng_export_option_video_messages(tr::now); break;
		case MediaType::Audio: label = tr::lng_export_option_audios(tr::now); break;
		case MediaType::Sticker: label = tr::lng_export_option_stickers(tr::now); break;
		case MediaType::GIF: label = tr::lng_export_option_gifs(tr::now); break;
		case MediaType::File: label = tr::lng_export_option_files(tr::now); break;
		case MediaType::Text: label = tr::lng_export_option_text_messages(tr::now); break;
		case MediaType::Link: label = tr::lng_export_option_links(tr::now); break;
		}
		if (!label.isEmpty()) {
			categoriesCount++;

			if (type == MediaType::Text) {
				text += label + ": " + Lang::FormatCountDecimal(item.localTotalCount) + "\n";
			} else if (type == MediaType::Link) {
				const auto messagesStr = item.messagesWithLinks > 0
					? " (" + Lang::FormatCountDecimal(item.messagesWithLinks) + " Messages)"
					: QString();
				if (item.uniqueCount == item.localTotalCount) {
					text += label + ": " + Lang::FormatCountDecimal(item.uniqueCount) + messagesStr + "\n";
				} else {
					text += label + ": " + Lang::FormatCountDecimal(item.uniqueCount)
						+ ", " + Lang::FormatCountDecimal(item.localTotalCount) + messagesStr + "\n";
				}
			} else {
				const bool noDuplicates = (item.uniqueCount == item.localTotalCount) && (item.uniqueSize == item.totalSize);
				if (noDuplicates) {
					text += label + ": " + Lang::FormatCountDecimal(item.localTotalCount) + " (" + Ui::FormatSizeText(item.totalSize) + ")\n";
				} else {
					const auto uniqueStr = Lang::FormatCountDecimal(item.uniqueCount)
						+ " (" + Ui::FormatSizeText(item.uniqueSize) + ")";
					const auto totalStr = Lang::FormatCountDecimal(item.localTotalCount)
						+ " (" + Ui::FormatSizeText(item.totalSize) + ")";

					text += label + ": " + uniqueStr + ", " + totalStr + "\n";
				}

				totalUniqueMediaSize += item.uniqueSize;
				totalMediaSize += item.totalSize;
			}

			if (type != MediaType::Link && type != MediaType::Text) {
				totalUniqueMessagesCount += item.uniqueCount;
				totalTotalMessagesCount += item.localTotalCount;
			}
		}
	}
	int mediaCategoriesCount = 0;
	for (const auto type : order) {
		const auto it = _scanResults.find(type);
		const bool hasData = (it != _scanResults.end() && it->second.localTotalCount > 0);
		const bool isTextOrLink = (type == MediaType::Text || type == MediaType::Link);
		const bool includeInTotal = !isTextOrLink;
		if ((showAllCategories || hasData) && includeInTotal) {
			mediaCategoriesCount++;
		}
	}
	if (mediaCategoriesCount > 1) {
		const auto label = "Total Media: ";
		const auto uniqueStr = Lang::FormatCountDecimal(totalUniqueMessagesCount)
			+ " (" + Ui::FormatSizeText(totalUniqueMediaSize) + ")";
		const auto totalStr = Lang::FormatCountDecimal(totalTotalMessagesCount)
			+ " (" + Ui::FormatSizeText(totalMediaSize) + ")";

		text += "\n" + QString(label) + uniqueStr + ", " + totalStr;
	}
	_scanResultsLabel->entity()->setText(text.trimmed());
	_scanResultsLabel->toggle(true, anim::type::instant);
	_container->resizeToWidth(_container->width());
	_changes.fire_copy(readData());
}

void SettingsWidget::clearScanResults() {
	_scanResults.clear();
	_hasScanResults = false;
	if (_scanResultsLabel) {
		_scanResultsLabel->entity()->setText(QString());
		_scanResultsLabel->hide(anim::type::instant);
		_container->resizeToWidth(_container->width());
	}
	_changes.fire_copy(readData());
}

void SettingsWidget::resetToDefault() {
	setScanning(false);
	changeData([&](Settings &data) {
		const auto oldSinglePeer = data.singlePeer;
		const auto oldName = data.singlePeerName;
		const auto oldId = data.singlePeerId;
		const auto oldPath = data.path;
		const auto oldFormat = data.format;
		const auto oldForce = data.forceSubPath;
		data = Settings();
		data.singlePeer = oldSinglePeer;
		data.singlePeerName = oldName;
		data.singlePeerId = oldId;
		data.path = oldPath;
		data.format = oldFormat;
		data.forceSubPath = oldForce;
		data.singlePeerFrom = 0;
		data.singlePeerTill = 0;
		data.singlePeerFromId = 0;
		data.singlePeerTillId = 0;
		data.useIdRange = false;
		data.media.types = MediaSettings::Types(0); // Reset all media type checkboxes
		data.media.sizeLimit = 4000LL * 1024 * 1024; // Explicitly reset size limit
		data.media.extensionFilter.clear();
		data.media.extensionFilterMode = MediaSettings::ExtFilterMode::None;
		data.types = Settings::Types(0);
	});
	clearScanResults();
}

void SettingsWidget::restoreSettings(const Settings &data) {
	changeData([&](Settings &current) {
		current.media.types = data.media.types;
		current.media.sizeLimit = data.media.sizeLimit;
		current.media.extensionFilterMode = data.media.extensionFilterMode;
		current.media.extensionFilter = data.media.extensionFilter;
		current.types = data.types;
		current.fullChats = data.fullChats;
		current.format = data.format;
		
		current.singlePeerFrom = data.singlePeerFrom;
		current.singlePeerTill = data.singlePeerTill;
		current.useIdRange = data.useIdRange;
		current.singlePeerFromId = data.singlePeerFromId;
		current.singlePeerTillId = data.singlePeerTillId;
	});
}

} // namespace View
} // namespace Export