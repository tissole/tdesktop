/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/variant.h"
#include "mtproto/mtproto_response.h"
#include "export/data/export_data_types.h"

#include <QtCore/QPointer>

#include "base/flat_map.h"

namespace Ui {
class Show;
}

namespace MTP {
class Instance;
} // namespace MTP

namespace Export {

class ControllerObject;
struct Settings;
struct Environment;

struct FileDownloadProgress {
	uint64 randomId = 0;
	QString path;
	int64 ready = 0;
	int64 total = 0;
};

struct PasswordCheckState {
	QString hint;
	QString unconfirmedPattern;
	bool requesting = true;
	bool hasPassword = false;
	bool checked = false;
	MTPInputPeer singlePeer = MTP_inputPeerEmpty();
};

struct ProcessingState {
	enum class Step {
		Initializing,
		DialogsList,
		PersonalInfo,
		Userpics,
		Stories,
		Contacts,
		Sessions,
		OtherData,
		Dialogs,
	};
	enum class EntityType {
		Chat,
		SavedMessages,
		RepliesMessages,
		VerifyCodes,
		Other,
	};

	Step step = Step::Initializing;

	int substepsPassed = 0;
	int substepsNow = 0;
	int substepsTotal = 0;

	EntityType entityType = EntityType::Other;
	QString entityName;
	int entityIndex = 0;
	int entityCount = 0;

	int itemIndex = 0;
	int itemCount = 0;

	//uint64 bytesRandomId = 0;
	//QString bytesName;
	//int64 bytesLoaded = 0;
	//int64 bytesCount = 0;
	base::flat_map<uint64, FileDownloadProgress> activeDownloads;
};

struct ApiErrorState {
	MTP::Error data;
};

struct OutputErrorState {
	QString path;
};

struct CancelledState {
};

struct FinishedState {
	QString path;
	int filesCount = 0;
	int64 bytesCount = 0;
};

using State = std::variant<
	v::null_t,
	PasswordCheckState,
	ProcessingState,
	ApiErrorState,
	OutputErrorState,
	CancelledState,
	FinishedState>;

//struct PasswordUpdate {
//	enum class Type {
//		CheckSucceed,
//		WrongPassword,
//		FloodLimit,
//		RecoverUnavailable,
//	};
//	Type type = Type::WrongPassword;
//
//};

class Controller {
public:
	Controller(
		not_null<Ui::Show*> show,
		QPointer<MTP::Instance> mtproto,
		const MTPInputPeer &peer);

	rpl::producer<State> state() const;

	// Password step.
	//void submitPassword(const QString &password);
	//void requestPasswordRecover();
	//rpl::producer<PasswordUpdate> passwordUpdate() const;
	//void reloadPasswordState();
	//void cancelUnconfirmedPassword();

	// Processing step.
	void startExport(
		const Settings &settings,
		const Environment &environment);
	void skipFile(uint64 randomId);
	void cancelExportFast();

	rpl::lifetime &lifetime();

	~Controller();

private:
	using Implementation = ControllerObject;
	std::unique_ptr<ControllerObject> _private;
	rpl::lifetime _lifetime;

};

} // namespace Export
