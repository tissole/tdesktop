/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flags.h"
#include "base/flat_map.h"

namespace Export {
namespace Output {
enum class Format;
} // namespace Output

struct MediaSettings {
	bool validate() const;

	enum class ExtFilterMode {
		None      = 0,
		Whitelist = 1,
		Blacklist = 2,
	};

	enum class Type {
		Photo        = 0x01,
		Video        = 0x02,
		VoiceMessage = 0x04,
		VideoMessage = 0x08,
		Sticker      = 0x10,
		GIF          = 0x20,
		File         = 0x40,
		Text         = 0x80,
		Audio        = 0x100,
		FullHistory  = 0x200,
		Link         = 0x400,

		MediaMask    = Photo | Video | VoiceMessage | VideoMessage | Audio,
		AllMask      = MediaMask | Sticker | GIF | File | Text | FullHistory | Link,
	};
	using Types = base::flags<Type>;
	friend inline constexpr auto is_flag_type(Type) { return true; };

	Types types = DefaultTypes();
	int64 sizeLimit = 8 * 1024 * 1024;

	// Extension filter (applies to Video, Audio, File types)
	ExtFilterMode extensionFilterMode = ExtFilterMode::None;
	QStringList extensionFilter; // lowercase, no dots, e.g. {"pdf","docx"}

	static inline Types DefaultTypes() {
		return Types(0);
	}

};

struct Settings {
	bool validate() const;

	enum class Type {
		PersonalInfo        = 0x001,
		Userpics            = 0x002,
		Contacts            = 0x004,
		Sessions            = 0x008,
		OtherData           = 0x010,
		PersonalChats       = 0x020,
		BotChats            = 0x040,
		PrivateGroups       = 0x080,
		PublicGroups        = 0x100,
		PrivateChannels     = 0x200,
		PublicChannels      = 0x400,
		Stories             = 0x800,

		GroupsMask          = PrivateGroups | PublicGroups,
		ChannelsMask        = PrivateChannels | PublicChannels,
		GroupsChannelsMask  = GroupsMask | ChannelsMask,
		NonChannelChatsMask = PersonalChats | BotChats | PrivateGroups,
		AnyChatsMask        = PersonalChats | BotChats | GroupsChannelsMask,
		NonChatsMask        = (PersonalInfo
			| Userpics
			| Contacts
			| Stories
			| Sessions),
		AllMask             = NonChatsMask | OtherData | AnyChatsMask,
	};
	using Types = base::flags<Type>;
	friend inline constexpr auto is_flag_type(Type) { return true; };

	QString path;
	bool forceSubPath = false;
	Output::Format format = Output::Format();

	Types types = DefaultTypes();
	Types fullChats = DefaultFullChats();
	MediaSettings media;

	MTPInputPeer singlePeer = MTP_inputPeerEmpty();
	QString singlePeerName;
	int64 singlePeerId = 0;
	TimeId singlePeerFrom = 0;
	TimeId singlePeerTill = 0;
	
	// ID range export fields
	int32 singlePeerFromId = 0;  // 0 means no limit
	int32 singlePeerTillId = 0;  // 0 means no limit
	bool useIdRange = false;     // Flag to indicate ID range export

	TimeId availableAt = 0;

	bool onlySinglePeer() const {
		return singlePeer.type() != mtpc_inputPeerEmpty;
	}

	static inline Types DefaultTypes() {
		return Type::PersonalInfo
			| Type::Userpics
			| Type::Contacts
			| Type::Stories
			| Type::PersonalChats
			| Type::PrivateGroups;
	}

	static inline Types DefaultFullChats() {
		return Type::PersonalChats
			| Type::BotChats;
	}

};

struct Environment {
	QString internalLinksDomain;
	QByteArray aboutTelegram;
	QByteArray aboutContacts;
	QByteArray aboutFrequent;
	QByteArray aboutSessions;
	QByteArray aboutWebSessions;
	QByteArray aboutChats;
	QByteArray aboutLeftChats;
};

} // namespace Export
