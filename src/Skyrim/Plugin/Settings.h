#pragma once

#include <array>

// Every key the plugin reads from its ini files, with its type, default and range. Read once at
// startup (Load, before any hook is installed) from SeamlessSkyrim.ini, then from
// SeamlessSkyrim_User.ini next to it: the user file wins per key. The in-game menu writes only the
// user file, so an update that replaces the shipped file keeps what the player chose.
namespace Plugin::Settings
{
	inline constexpr const char* kShippedFile = "SeamlessSkyrim.ini";
	inline constexpr const char* kUserFile = "SeamlessSkyrim_User.ini";

	enum class Id
	{
		kEnabled,
		kForceDoors,
		kDoorOpenDistance,
		kDoorwayPictureHeight,
		kDoorwayCacheMegabytes,
		kDoorwayDepthLayers,
		kShownRooms,
		kPauseUnseenRooms,
		kMerge,
		kMergeOutdoors,
		kMergedCull,
		kStepRemoteHavok,
		kStepExteriorFromInside,
		kHandOverStreet,
		kProfileCrossingsTo,
		kCount,
	};

	enum class Type
	{
		kBool,
		kInt,
		kString,
	};

	// The default of kStepRemoteHavok: 2 with Precision loaded, else 1 (see Havok).
	[[nodiscard]] int RemoteHavokStepDefault();

	struct Entry
	{
		Id             id;
		const wchar_t* section;
		const wchar_t* key;
		Type           type;
		int            def;                         // bool and int
		int            min = 0;                     // int
		int            max = 0;
		const wchar_t* text = L"";                  // the default of a string
		int            (*computeDefault)() = nullptr;  // replaces def where it depends on the install
	};

	inline constexpr std::array<Entry, static_cast<std::size_t>(Id::kCount)> kEntries{ {
		{ Id::kEnabled, L"Portals", L"bEnabled", Type::kBool, 1 },
		{ Id::kForceDoors, L"Portals", L"iForceDoors", Type::kInt, 0, 0, 2 },
		// Game units: the reach of a door; its space loads Selection::kLoadAhead further out.
		{ Id::kDoorOpenDistance, L"Portals", L"iDoorOpenDistance", Type::kInt, 450, 150, 1000 },
		// A doorway picture's height in texels (a multiple of 4 is used).
		{ Id::kDoorwayPictureHeight, L"Portals", L"iDoorwayPictureHeight", Type::kInt, 512, 32, 1024 },
		// The doorway pictures kept on disk are held to this many megabytes; the least recently used go first.
		{ Id::kDoorwayCacheMegabytes, L"Portals", L"iDoorwayCacheMB", Type::kInt, 2048, 64, 32768 },
		// The depth image tier: views inside each doorway (0 = no depth images, the image tier alone).
		{ Id::kDoorwayDepthLayers, L"Portals", L"iDoorwayDepthLayers", Type::kInt, 3, 0, 3 },
		{ Id::kShownRooms, L"Portals", L"iShownRooms", Type::kInt, 2, 1, 4 },
		{ Id::kPauseUnseenRooms, L"Portals", L"bPauseUnseenRooms", Type::kBool, 1 },
		{ Id::kMerge, L"Portals", L"bMerge", Type::kBool, 1 },
		{ Id::kMergeOutdoors, L"Portals", L"bMergeOutdoors", Type::kBool, 1 },
		// 0 off, 1 a merged room left out whole when it can't be seen, 2 also shape by shape.
		{ Id::kMergedCull, L"Portals", L"iMergedCull", Type::kInt, 2, 0, 2 },
		// 0 never, 1 inside the engine's step job, 2 on the main thread at the end of the frame.
		{ Id::kStepRemoteHavok, L"Portals", L"iStepRemoteHavok", Type::kInt, 0, 0, 2, L"", RemoteHavokStepDefault },
		{ Id::kStepExteriorFromInside, L"Portals", L"bStepExteriorFromInside", Type::kBool, 1 },
		{ Id::kHandOverStreet, L"Portals", L"bHandOverStreet", Type::kBool, 1 },
		// Agent sessions: a folder every crossing's main-thread samples are written to.
		{ Id::kProfileCrossingsTo, L"Portals", L"sProfileCrossingsTo", Type::kString, 0, 0, 0, L"" },
	} };

	[[nodiscard]] constexpr const Entry& Of(Id a_id)
	{
		return kEntries[static_cast<std::size_t>(a_id)];
	}

	// kDataLoaded, before any hook is installed.
	void Load();

	[[nodiscard]] int                 Int(Id a_id);
	[[nodiscard]] bool                Bool(Id a_id);
	[[nodiscard]] const std::wstring& String(Id a_id);

	// What the menu keeps: the value (clamped to its range) is kept for this session and written to
	// the user file; the shipped file is never written.
	void SaveUser(Id a_id, int a_value);
}
