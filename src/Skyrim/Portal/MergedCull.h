#pragma once

#include "Skyrim/Render/Wire.h"

// A room merged into the world (RemoteCell::Merge) is drawn by the engine's main pass, which culls
// only against the camera's view; the doorway mask then throws away every pixel of it not seen
// through the doorway. Looking at the house, the whole room behind its wall was set up and drawn
// for nothing. This leaves out, for the length of the player's view, what the mask would throw
// away whole: everything when no line of sight passes the doorway, otherwise the parts lying
// wholly outside the space seen through it (Core/Render/DoorwayPlanes.h). An interior built of rooms is
// also culled through its portals, as the engine culls the current cell.
namespace Portal::MergedCull
{
	struct Room
	{
		Render::DoorRect                            door;        // as sent to Render::Backend::SetMergedDoor
		std::vector<RE::TESObjectCELL*>             cells;       // merged; their 3D is culled
		std::vector<RE::NiPointer<RE::NiAVObject>> keep;        // drawn as the engine has them (the room's door)
		bool                                        doorShut = false;
		// The street merged into an interior (its distant land and persistent references in roots),
		// culled at most as far as SetStreetMode allows.
		bool                                        street = false;
		std::vector<RE::NiPointer<RE::NiAVObject>> roots;
	};

	enum class Mode
	{
		kOff,
		kWhole,     // only all or nothing
		kShapes,    // and shape by shape
		kHideAll,   // diagnostics: the room hidden whatever is seen (what is to be won)
	};

	// kDataLoaded: the engine's cull of room bounds, opened for merged rooms (see RoomCullHook).
	void Install();

	// Main thread, every frame: the rooms merged now (the shown one and those merged beside it),
	// none when nothing is.
	void Set(std::vector<Room> a_rooms);

	// Around the engine's player view (Main::RenderPlayerView). What BeforeView hides, AfterView
	// shows again, so nothing hidden outlives the view: saves, crossings and the rest of the frame
	// see the room as it was.
	void BeforeView();
	void AfterView();

	void                 SetMode(Mode a_mode);
	// On by default: an interior's rooms culled through their portals (see Walk). Off for comparing.
	void                 SetRoomCull(bool a_on);
	// kOff (the default), kWhole or kShapes. Diagnostics: both change the sun's shadows, see Cull.
	void                 SetStreetMode(Mode a_mode);
	[[nodiscard]] json   Describe();
}
