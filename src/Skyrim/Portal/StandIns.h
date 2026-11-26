#pragma once

#include "Skyrim/Doors/Hold.h"
#include "Skyrim/Doors/Swing.h"
#include "Skyrim/Portal/Active.h"

// The doorway before the space behind it is there. A door used to stay shut until the space behind
// it had loaded: on a slow computer, walked up to and stood at, shut. A door that is to open
// (Doors::Wanted) now swings open whatever the load, and its doorway shows a picture of what was
// last seen through it (Pictures, Tiers.h: the depth image where the scheduler holds the space at T2
// and the picture is ready, else the image), or a plain colour, until the real space takes over and
// the picture fades out. The door's collision stays: there is nothing to walk into yet. A
// city gate shows its picture too, if set to (Gates).
namespace Portal::StandIns
{
	// A doorway to fill with a stand-in picture (Pictures, DoorwaySnapshot.h) or one to photograph.
	struct DoorwayFill
	{
		RE::FormID door;      // in the player's space
		bool       outdoors;  // what is behind the door is outdoors
		bool       gate;      // outdoors on both sides
		float      alpha;     // 1 while the space behind isn't there; fading out once it is
	};

	struct DoorwayStandIn
	{
		RE::FormID               linked = 0;
		RE::FormID               swinging = 0;        // of the pair, the one swung open (Doors::SwingingDoor)
		bool                     outdoors = false;    // the space behind is outdoors
		bool                     gate = false;        // outdoors on both sides
		bool                     picture = true;      // its doorway shows the picture (a gate only if set to)
		bool                     handedOver = false;  // the real space took the door over; the picture fades
		Portal::Doors::Swing     swing;               // of the door swinging
		float                    fade = 1.0f;         // the picture's, once handed over
		// The door the player just came out of: its picture is of the space they were standing in a
		// moment ago, and that space fading in over it read as a delay, not as a door opening.
		// Handed over, the picture goes at once.
		bool                     cameThrough = false;
		// Through the door, away from the player, in the door's own cell's coordinates: that cell's 3D
		// moves when it is merged into another space or handed back. Kept as drawn, the street's doors
		// seen from inside Belethor's swung into their buildings once the player walked out (the
		// Bannered Mare's out of sight until its own portal took it over).
		RE::NiPoint3             inwardInCell;
		Portal::Doors::OpenState open;  // solid: the collision stays
	};

	// Main thread, every frame, before the portal's own door handling (PortalDoor::Update): a door
	// a stand-in held is handed over to the portal that frame. Also run again in the merge's frame.
	void Update(RE::PlayerCharacter* a_player, RE::TESObjectCELL* a_here, float a_delta);
	// A doorway standing open while the space behind it isn't there to walk into is walked through
	// all the same: its door stays solid, and walking into it the player is taken through the
	// engine's way, with its loading screen. Without this they could only stand at the open doorway
	// and wait.
	void WalkInto(RE::PlayerCharacter* a_player, RE::TESObjectCELL* a_here, float a_delta);

	// Every stand-in's door let go as it is (the portal is off).
	void Drop();
	// Before a load: the doors held and the scans forgotten.
	void Forget();

	// The amount to start swinging a door at for the space behind it: where a stand-in holds it, or
	// where it was let go of. The space becomes ready in the middle of a frame (RemoteCell::Update),
	// after the stand-ins were updated: the portal's door handling took the door that frame, before
	// the stand-in had let go of it, and swung it open again from shut.
	void TakeOverDoor(Portal::Doors::Swing& a_swing, RE::FormID a_door);

	// The side a stand-in's door swings away from (a_viewer's), in the player's space: Swing reads
	// it against the door as drawn, which for a door of the merged space isn't where its own cell
	// puts it.
	[[nodiscard]] RE::NiPoint3 AwayFrom(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_viewer);
	// In the space the player is in: their cell, or outdoors any cell of the same worldspace (the
	// cells around are walked between without a door, so their doors are as near as the cell's own).
	[[nodiscard]] bool InPlayerSpace(RE::TESObjectREFR* a_door, RE::TESObjectCELL* a_here);

	// By the door in the player's space.
	[[nodiscard]] const std::unordered_map<RE::FormID, DoorwayStandIn>& Doorways();

	// Main thread: the doors swung open before the space behind them is there (or fading out after).
	[[nodiscard]] std::vector<DoorwayFill> Fills();
	// Main thread, render: the doorway shows the real space, fully open and settled.
	[[nodiscard]] std::optional<DoorwayFill> SnapshotChance();

	// A door into a room has its doorway picture taken with a camera of its own
	// (Pictures::CaptureCamera), which only the portal pass can draw with: merged, the room
	// is drawn with the world, and it is merged as soon as it has loaded. Once per door (the
	// picture is kept), the merge waits for it: a moment for the room's late lights, then the
	// frames it takes. Merged anyway if it doesn't come. a_mergeable: the room would be merged now;
	// returns true while the merge waits.
	[[nodiscard]] bool WaitForPicture(const Active& a_portal, const RE::TESObjectCELL* a_here, bool a_mergeable);
	// Render: the door whose room waits to be merged until the portal pass has drawn its doorway
	// picture with the capture camera (Pictures::CameraPictureWanted), or 0.
	[[nodiscard]] RE::FormID PictureWanted();
}
