#pragma once

#include "Portal/Active.h"

// What the doors need to ask of the portal manager, registered by it once so that Doors depends
// on nothing above it (the manager includes Doors, never the other way).
namespace Portal::Doors
{
	struct Host
	{
		// Main thread: what using the door does, for its prompt (Hold).
		DoorPrompt (*prompt)(RE::FormID a_door) = nullptr;
		// The doorway of a_door (a_linkedOutdoors: the space behind it) has a picture of what is
		// behind it (Policy: such a door may stand open from afar).
		bool (*hasPicture)(RE::FormID a_door, bool a_linkedOutdoors) = nullptr;
		// The space behind a_door is drawn in its doorway (merged, the portal ready, or a room beside),
		// not a stand-in's picture.
		bool (*spaceShown)(RE::FormID a_door) = nullptr;
		// The door at the other end of a_door when it would be a portal door, seen from a_here, the
		// menu's switch aside.
		RE::NiPointer<RE::TESObjectREFR> (*portalLink)(RE::TESObjectREFR* a_door, RE::TESObjectCELL* a_here) = nullptr;
	};

	void        SetHost(const Host& a_host);
	const Host& GetHost();
}
