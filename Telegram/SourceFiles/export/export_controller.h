/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <memory>
#include <map>

#include "base/variant.h"
#include "mtproto/mtproto_response.h"
#include "export/data/export_data_types.h"
#include "export/export_settings.h"
#include "export/output/export_output_stats.h"

#include <QtCore/QPointer>
#include <crl/crl_object_on_queue.h>

#include "base/flat_map.h"
#include "tl/tl_boxed.h"

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
	QString singlePeerName;
	int64 singlePeerId = 0;
};

struct FinishedState {
	QString path;
	int filesCount = 0;
	int messagesTextCount = 0;
	int messagesMediaCount = 0;
	int messagesTotalCount = 0;
	int64 bytesCount = 0;
	int totalUniqueCount = 0;
	int64 totalUniqueSize = 0;
	int totalTotalCount = 0;
	int64 totalTotalSize = 0;
	bool fullHistory = false;
	std::map<MediaSettings::Type, Output::StatItem> breakdown;
};

struct ProcessingState {
	enum class Step {
		Initializing,
		Scanning,
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
	bool isScanning = false;

	base::flat_map<uint64, FileDownloadProgress> activeDownloads;
	std::map<MediaSettings::Type, Output::StatItem> selectedStats;
};

struct ScanDoneState {
	std::map<MediaSettings::Type, Output::StatItem> stats;
};

struct ApiErrorState {
	MTP::Error data;
};

struct ValueErrorState {
	QString message;
};

struct OutputErrorState {
	QString path;
};

struct CancelledState {
};

using State = std::variant<
	v::null_t,
	PasswordCheckState,
	ProcessingState,
	ApiErrorState,
	ValueErrorState,
	OutputErrorState,
	CancelledState,
	ScanDoneState,
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
		QPointer<MTP::Instance> mtproto,
		const MTPInputPeer &peer,
		const QString &name = QString(),
		int64 id = 0);

	rpl::producer<State> state() const;

	// Password step.
	//void submitPassword(const QString &password);
	//void requestPasswordRecover();
	//rpl::producer<PasswordUpdate> passwordUpdate() const;
	//void reloadPasswordState();
	//void cancelUnconfirmedPassword();

	// Processing step.
	void runScan(
		const Settings &settings,
		const Environment &environment);
	void startExport(
		const Settings &settings,
		const Environment &environment);
	void skipFile(uint64 randomId);
	void cancelExportFast();

	rpl::lifetime &lifetime();

	~Controller();

private:
	using Implementation = ControllerObject;
	crl::object_on_queue<Implementation> _wrapped;
	rpl::lifetime _lifetime;

};

} // namespace Export