#pragma once

#include "Core/Math/Vec3.h"
#include "Core/Selection/Ranking.h"
#include "Skyrim/Plugin/Settings.h"

// Which doors are portal doors, and how the doors around the player are measured: the reach of a
// door, the direction the player heads in, and the order doors are ranked by. What is loaded, and
// which door is live, is the scheduler's decision (Schedule).
namespace Portal::Selection
{
	using Core::Selection::kLoadAhead;
	using Core::Selection::kReleaseBeyond;
	using Core::Selection::kSwitchMargin;

	// The reach of a door ([Portals] iDoorOpenDistance), from the settings.
	void Install();

	// How close (game units) the player comes before a door can be live (its space loads kLoadAhead
	// further out) and it can be walked through.
	[[nodiscard]] float DoorOpenDistance();
	void                SetDoorOpenDistance(float a_distance);

	[[nodiscard]] float NearRadius();
	// A shown space is let go beyond this, once its scheduler tier has dropped.
	[[nodiscard]] float ReleaseRadius();

	// The direction the player last moved in (horizontal, unit; zero until they have, in this
	// space): the door walked towards is the likelier one to be walked through. Every frame.
	void TrackHeading(const RE::NiPoint3& a_position);
	[[nodiscard]] const Core::Vec3& Heading();

	// a_door's distance from a_origin as the reaches and the order of doors count it.
	[[nodiscard]] float Reach(const RE::NiPoint3& a_origin, RE::TESObjectREFR* a_door, RE::TESObjectREFR* a_linked);
	// What doors are ordered by: the reach, a little less for a door ahead of the player and a
	// little more for one behind.
	[[nodiscard]] float Rank(const RE::NiPoint3& a_origin, RE::TESObjectREFR* a_door, RE::TESObjectREFR* a_linked);

	// The door at the other end of a_ref when a_ref would be a portal door, the menu's switch aside.
	[[nodiscard]] RE::NiPointer<RE::TESObjectREFR> PortalLink(RE::TESObjectREFR* a_ref, RE::TESObjectCELL* a_here);

	struct DoorPair
	{
		RE::FormID sourceDoor;  // in the player's space
		RE::FormID linkedDoor;
	};
	// Main thread: the pair of the doorway the player is at, for the menu: the portal's while its
	// door is the nearest, else the nearest door within a_radius. The portal's alone, the menu showed
	// a shop's other door (holding the portal from up to ReleaseRadius away) at a door switched off,
	// which then couldn't be switched back on.
	[[nodiscard]] std::optional<DoorPair> DoorPairAt(float a_radius);
}
