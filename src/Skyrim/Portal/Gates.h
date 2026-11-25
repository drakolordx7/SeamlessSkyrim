#pragma once

#include "Skyrim/Portal/Active.h"

// City gates: a door from the outdoors into another worldspace, many times a house door's size.
// Both worldspaces share their coordinates and one Havok world, the far gate stands a few units
// behind the near one (Whiterun's) or a gatehouse apart (Solitude's), and the player's own
// worldspace shows its low-detail copy of the city through the open gate until the real one is
// there. What the other modules do differently for a gate is here, with the numbers tuned together.
namespace Portal::Gates
{
	// A city gate seen from far down the road counts as nearer than it is, in which door is shown
	// and in how far out its space loads and is let go. Not by a fixed amount: a house door must
	// still win when walked up to near a gate (it has to be the shown one to be walked through), and
	// at a quarter of the distance it does unless it stands within four times the switch margin of
	// the gate (Selection::kSwitchMargin / kReachWeight).
	inline constexpr float kReachWeight = 0.25f;

	// The two cells are outdoors both: what is behind the door is another worldspace.
	[[nodiscard]] bool IsGate(const RE::TESObjectCELL* a_here, const RE::TESObjectCELL* a_behind);

	// A stand-in on a gate shows its picture only if the gate is set to (Doors::PictureFromAfar);
	// else the player's own worldspace shows through it, drawn in 3D.
	[[nodiscard]] bool ShowsPicture(RE::TESObjectREFR* a_door, RE::FormID a_linked, bool a_linkedOutdoors, bool a_gate);
	// A gate without a picture swings its own leaves where the portal swings the far gate's: into
	// the space behind. Not where the gates stand apart: there its leaves would swing into the
	// masked passage. a_away: from the player, in the player's space.
	[[nodiscard]] bool SwingsIntoFar(RE::TESObjectREFR* a_gate, RE::FormID a_farGate, const RE::NiPoint3& a_away);
	// No doorway picture to take of a doorway fully open on the real space: a gate that shows none
	// (ShowsPicture), nor the street from inside, which is shown only live (Doors::StreetShownLive).
	[[nodiscard]] bool SkipsPicture(const Active& a_portal, bool a_outdoors, bool a_gate);

	// The portal's door is open on a gate: the far gate swung open or hidden (a_farSwings), its
	// passage cleared of what the other worldspace's buildings put in the player's way. Every frame
	// while the door stands open; ReleaseFar when it is shut or let go.
	void HoldFar(const Active& a_portal, RE::TESObjectREFR* a_linked, bool a_farSwings);
	void ReleaseFar();

	// Main thread, every frame: the gate's doorway is masked like a merged room's (room 0), so the
	// world's own side of the gatehouse doesn't show where the other worldspace draws nothing.
	void UpdateMask(RE::TESObjectCELL* a_here);

	// How far a door's collision reaches past its centre (a_center) towards a_inward, at a_height: a
	// ray from that side back through the door, to the first body of a thing (not a person, not the
	// land). Not only the door's own: a gate's thick arch may be a static of its own, and placed
	// short of it the player was pushed out of it just the same.
	[[nodiscard]] std::optional<float> CollisionReach(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_center, const RE::NiPoint3& a_inward, float a_height);
}
