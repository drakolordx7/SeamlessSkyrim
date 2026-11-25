#pragma once

#include "Skyrim/Portal/Active.h"

// NPCs using the portal's door. The engine moves an NPC that takes a load door from where it
// stood in front of the door to the door's marker on the other side. With both sides alive and
// visible that is a jump you can watch. Here the NPC is put where it would be had it simply kept
// walking: its last position mapped through the door pair, like the player's crossing.
namespace Portal::NpcTravel
{
	// At kDataLoaded, with the portal hooks: NPCs walk through a door standing open without reaching
	// for it, and reaching for a shut one opens it (DoorUse).
	void Install();

	// Main thread, every frame a portal is active. a_remoteCells: the cells behind the door.
	void Update(const Active& a_portal, const std::vector<RE::TESObjectCELL*>& a_remoteCells);

	// No portal: forget what was tracked.
	void Reset();

	// Main thread, right before the world is drawn: actors passing the doorway stay opaque (the
	// engine fades them out and in around its transition, later in the frame than Update runs).
	void BeforeRender();

	// Main thread: the references of a_cell (just walked into, or loaded behind a door) are shown
	// at once for a_seconds, whatever fade-in the engine starts for them.
	void ShowAtOnce(RE::TESObjectCELL* a_cell, float a_seconds);

	// While an NPC is walked through a merged doorway it is drawn with the space it is seen in,
	// not the one its cell belongs to: on the porch with the world (the merged room is only shown
	// behind the door plane), behind the plane with the room. The 3D of the actors that are, right
	// now, the other way round than their cell says.
	void MergedOverrides(std::vector<RE::NiAVObject*>& a_withWorld, std::vector<RE::NiAVObject*>& a_withRoom);

	json Status();
}
