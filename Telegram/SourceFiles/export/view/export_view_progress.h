/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/rp_widget.h"
#include "export/view/export_view_content.h"
#include "base/object_ptr.h"
#include "base/timer.h"

namespace Ui {
class VerticalLayout;
class ScrollArea;
class RoundButton;
class FlatLabel;
class LinkButton;
class FixedHeightWidget;
template <typename Widget>
class FadeWrap;
} // namespace Ui

namespace Export {
namespace View {

class ProgressWidget : public Ui::RpWidget {
public:
	ProgressWidget(
		QWidget *parent,
		rpl::producer<Content> content);

	rpl::producer<uint64> skipFileClicks() const;
	rpl::producer<> cancelClicks() const;
	rpl::producer<> doneClicks() const;

	~ProgressWidget();

private:
	void setupBottomButton(not_null<Ui::RoundButton*> button);
	void updateState(Content &&content);
	void showDone(bool isScanning);

	Content _content;

	class Row;
	Ui::ScrollArea *_scroll = nullptr;
	not_null<Ui::VerticalLayout*> _body;
	std::vector<not_null<Row*>> _rows;

	base::unique_qptr<Ui::FadeWrap<Ui::LinkButton>> _skipFile;
	Ui::FixedHeightWidget *_skipFileWrap = nullptr;
	QPointer<Ui::FlatLabel> _about;
	base::unique_qptr<Ui::RoundButton> _cancel;
	base::unique_qptr<Ui::RoundButton> _done;
	rpl::event_stream<> _doneClicks;

	uint64 _fileRandomId = 0;
	base::Timer _fileShowSkipTimer;

};

} // namespace View
} // namespace Export
