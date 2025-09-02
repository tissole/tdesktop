/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <memory>

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

inline bool operator==(const FileDownloadProgress &a, const FileDownloadProgress &b) {
	return (a.randomId == b.randomId)
		&& (a.path == b.path)
		&& (a.ready == b.ready)
		&& (a.total == b.total);
}

struct PasswordCheckState {
	QString hint;
	QString unconfirmedPattern;
	bool requesting = true;
	bool hasPassword = false;
	bool checked = false;
	MTPInputPeer singlePeer = MTP_inputPeerEmpty();
};

inline bool operator==(const PasswordCheckState &a, const PasswordCheckState &b) {
	return (a.hint == b.hint)
		&& (a.unconfirmedPattern == b.unconfirmedPattern)
		&& (a.requesting == b.requesting)
		&& (a.hasPassword == b.hasPassword)
		&& (a.checked == b.checked)
		&& (a.singlePeer == b.singlePeer);
}

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

inline bool operator==(const ProcessingState &a, const ProcessingState &b) {
	return (a.step == b.step)
		&& (a.substepsPassed == b.substepsPassed)
		&& (a.substepsNow == b.substepsNow)
		&& (a.substepsTotal == b.substepsTotal)
		&& (a.entityType == b.entityType)
		&& (a.entityName == b.entityName)
		&& (a.entityIndex == b.entityIndex)
		&& (a.entityCount == b.entityCount)
		&& (a.itemIndex == b.itemIndex)
		&& (a.itemCount == b.itemCount)
		&& (a.activeDownloads == b.activeDownloads);
}

struct ApiErrorState {
	MTP::Error data;
};

inline bool operator==(const ApiErrorState &a, const ApiErrorState &b) {
	return a.data == b.data;
}

struct OutputErrorState {
	QString path;
};

inline bool operator==(const OutputErrorState &a, const OutputErrorState &b) {
	return a.path == b.path;
}

struct CancelledState {
};

inline bool operator==(const CancelledState &a, const CancelledState &b) {
	return true;
}

struct FinishedState {
	QString path;
	int filesCount = 0;
	int64 bytesCount = 0;
};

inline bool operator==(const FinishedState &a, const FinishedState &b) {
	return (a.path == b.path)
		&& (a.filesCount == b.filesCount)
		&& (a.bytesCount == b.bytesCount);
}

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
	std::shared_ptr<ControllerObject> _private;
	rpl::lifetime _lifetime;

};

} // namespace Export
