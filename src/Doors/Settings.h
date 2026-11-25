#pragma once

#include "Doors/Door.h"

// What the player (or a patcher) set for a door pair: the in-game menu's switches, kept in
// SeamlessSkyrim_Doors.ini (the mod's own, never written) and SeamlessSkyrim_Doors_User.ini (the
// player's, which replaces all of a door's shipped settings). All of it is stored under one door
// of the pair, the correction door.
namespace Portal::Doors
{
	// Some interiors are a little turned against the outside they belong to (the doors' markers
	// don't quite agree); the player can correct that by hand: degrees the inside is turned around
	// its door. The correction belongs to the pair and is stored under one of its doors, the
	// outdoor one where there is one.
	[[nodiscard]] RE::TESObjectREFR* CorrectionDoor(RE::TESObjectREFR* a_source, RE::TESObjectREFR* a_linked);
	// The same by IDs and the sides' kinds: the linked door needn't be loaded.
	[[nodiscard]] RE::FormID         CorrectionDoor(const Side& a_source, const Side& a_linked);
	[[nodiscard]] float              Correction(RE::FormID a_door);
	void                             SetCorrection(RE::FormID a_door, float a_degrees);
	// Everything the menu sets for the pair (turn, shift, hinge and the switches) at once: the
	// menu applies each live and keeps it only on "Save for this door", so trying a setting out
	// doesn't change the file.
	void                             SaveCorrection(RE::FormID a_door);
	// The inside moved against the outside, in the outdoor door's frame: x to the right (seen
	// walking in), y further in, z up; in game units. Stored with the turn.
	[[nodiscard]] RE::NiPoint3       Shift(RE::FormID a_door);
	void                             SetShift(RE::FormID a_door, const RE::NiPoint3& a_shift);

	// How far the pair's door leaves turn, fully open (the menu, stored with the correction): less
	// where the wall beside the doorway is closer than a leaf is wide, or the leaf ends up in it.
	constexpr float     kSwingDegrees = 95.0f;
	[[nodiscard]] float SwingDegrees(RE::FormID a_door);
	void                SetSwingDegrees(RE::FormID a_door, float a_degrees);

	// The pair's portal switched off (the menu, stored with the correction): an ordinary door
	// again, with its loading screen. Either door of the pair may be passed.
	[[nodiscard]] bool PortalOff(RE::FormID a_door);
	void               SetPortalOff(RE::FormID a_door, bool a_off);

	// Which way the pair's door opens (the menu, stored with the correction): outwards, into the
	// street, unless set to open inwards (between two interiors "outwards" is the correction
	// door's side). The same seen from either side: of the two doors standing in the doorway, the
	// one in the space it opens into is swung (SwingingDoor), the other hidden while it is open.
	[[nodiscard]] bool       OpensInward(RE::FormID a_door);
	void                     SetOpensInward(RE::FormID a_door, bool a_inward);
	[[nodiscard]] RE::FormID SwingingDoor(const Side& a_source, const Side& a_linked);

	// The pair's doorway stands open whatever the player's distance (the menu, stored with the
	// correction), shown by its picture while the space behind isn't there (Snapshot).
	[[nodiscard]] bool                    AlwaysOpen(RE::FormID a_door);
	void                                  SetAlwaysOpen(RE::FormID a_door, bool a_open);
	[[nodiscard]] std::vector<RE::FormID> AlwaysOpenDoors();

	// A city gate shows its doorway picture while the city behind isn't there; before it has one,
	// with this set (the menu, stored with the correction), the plain colour. Without it the
	// gateway shows the player's own worldspace then, which in most cities holds a simple copy of
	// the other side (Whiterun's, Riften's houses); Solitude's gatehouse is walled up behind its
	// gate on both sides.
	[[nodiscard]] bool PictureFromAfar(RE::FormID a_door);
	void               SetPictureFromAfar(RE::FormID a_door, bool a_picture);
}
