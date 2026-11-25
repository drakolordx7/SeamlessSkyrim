#pragma once

#include "Skyrim/Doors/Door.h"

// Whether a door pair is to stand open, and what using it does. A door doesn't open as the player
// comes near. It opens when someone uses it (DoorUse: the player, an NPC reaching for it) and shuts
// a few seconds after; it stands open for good when it is a city gate, set to stand open always
// (once its doorway has a picture to show from afar: never seen open, it showed the plain colour),
// or every door is to (the global force). Forced shut, only a use opens it.
//
// a_door is the door in the player's space; a_linked, its other end, needn't be loaded (a Side
// converts from a door reference, or is given by ID and kind).
namespace Portal::Doors
{
	// A door out of a building (a_door indoors, its pair outdoors) shows the street only live, never
	// a picture of it: the street's pictures, taken from inside and drawn with their depth, broke up
	// into streaks seen from beside the doorway, and drawn flat they looked nothing like the street.
	// So such a door opens from inside only once its street is shown (StandsOpen), and has no
	// stand-in.
	[[nodiscard]] bool StreetShownLive(RE::TESObjectREFR* a_door, bool a_linkedOutdoors);

	// The pair stands open whoever uses it (it can't be shut), and already before the space behind
	// it is there: it is never seen shut, so never seen opening either.
	[[nodiscard]] bool OpenFromAfar(RE::TESObjectREFR* a_door, const Side& a_linked);
	// The pair stands open whoever uses it (it can't be shut): from afar, or set to stand open always
	// and its space shown. The picture is only taken of a doorway seen open: from inside Understone
	// Keep, never seen open from there, the doors stood shut for good. Such a door was seen shut
	// until its space came, so it swings open then (OpenFromAfar's are shown open at once).
	[[nodiscard]] bool StandsOpen(RE::TESObjectREFR* a_door, const Side& a_linked);
	// The pair's doors are to be open now: standing open, or used.
	[[nodiscard]] bool Wanted(RE::TESObjectREFR* a_door, const Side& a_linked);

	// The far end of a door the player's use is the manager's (DoorActivateHook): a seamless door,
	// seen from the player's space. Null for any other.
	[[nodiscard]] RE::TESObjectREFR* UsedPairOf(RE::TESObjectREFR* a_door);
	// Open because the player or an NPC used it, so using it shuts it.
	[[nodiscard]] bool ClosableDoor(RE::TESObjectREFR* a_door);
}
