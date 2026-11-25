#pragma once

#include "Core/Doors/Panel.h"
#include "Core/Doors/Swing.h"

// Where a door's panel is and how its leaves swing, measured from its mesh. Shared by the door
// pair (Pair), the renderer (portal camera, quad), the held-open state (Hold) and the swing.
namespace Portal::Doors
{
	// The door panel's corners in its cell's space (closed, as measured from the mesh: the leaves
	// its opening animation moves, between their faces). Corner index bits: 1 = max x, 2 = max y,
	// 4 = max z, in the door's model space.
	std::array<RE::NiPoint3, 8> Corners(RE::TESObjectREFR* a_door);
	RE::NiPoint3                Center(RE::TESObjectREFR* a_door);
	// From the panel's min face to its max face (the corners' x or y bit, whichever is thin): not
	// a unit vector, its length is the panel's thickness.
	RE::NiPoint3                PanelNormal(RE::TESObjectREFR* a_door);

	using PanelSize = Core::Doors::PanelSize;
	[[nodiscard]] PanelSize Size(RE::TESObjectREFR* a_door);

	// The doorway as a flat quad just off the door panel (as measured closed): in front of it on
	// the viewer's side, or with behindPanel just behind its far face. wholeMesh: as wide and high
	// as all of the door's mesh (a frame included), not only its leaves.
	struct QuadOptions
	{
		bool behindPanel = false;
		bool wholeMesh = false;
	};
	std::array<RE::NiPoint3, 4> Quad(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_viewer, QuadOptions a_options = {});
	// The same where the door is drawn: a cell merged into the player's space has its 3D moved,
	// and a picture or a swing direction in its own coordinates landed at the cell's unmoved place.
	// a_viewer is in the player's space. Identical to Quad and Center for any other door.
	std::array<RE::NiPoint3, 4> ShownQuad(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_viewer, QuadOptions a_options = {});
	RE::NiPoint3                ShownCenter(RE::TESObjectREFR* a_door);
	// The door's cell's own coordinates to where it is drawn (identity-like for an unmoved cell).
	RE::NiTransform             ToShown(RE::TESObjectREFR* a_door);

	// The leaves (the nodes the door's own opening animation turns; the root itself when nothing is
	// animated), whose shapes the held-open state hides.
	std::vector<RE::NiPointer<RE::NiAVObject>> LeafNodes(RE::TESObjectREFR* a_door);
	// The shapes standing behind the panel, away from the door's own side (where its partner's
	// marker puts whoever comes through): the box that hides the void behind a closed door.
	std::vector<RE::NiPointer<RE::BSGeometry>> BehindPanel(RE::TESObjectREFR* a_door);

	// Main thread: the door's own opening and shutting curves (how far open, TurnLeaves's a_amount,
	// and how long each takes), for a swing to step along: a door the portal swings moves as the
	// game moves it. The game plays its "Open" and "Close", not one backwards.
	[[nodiscard]] Core::Doors::SwingCurves SwingCurvesOf(RE::TESObjectREFR* a_door);
	// Main thread: the door's leaves swung open around their hinges, a_amount from 0 (closed) to 1
	// (open), towards a_room (a horizontal world direction: the side it opens into); its frame
	// stays. Placement and everything derived from it keep using the closed door. a_settings: the
	// pair's correction door, whose angle it takes.
	void TurnLeaves(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_room, float a_amount, RE::FormID a_settings);
	// Main thread: the panel back where it was.
	void RestoreLeaves(RE::FormID a_door);
	// Main thread: the door's leaves are turned by TurnLeaves (not put back by RestoreLeaves).
	[[nodiscard]] bool IsSwung(RE::FormID a_door);

	// Main thread, now and then: forgets the measurements of doors whose 3D has gone. The cache holds
	// the 3D it measured, and kept every door ever approached in memory with its mesh.
	void ForgetUnloaded();
}
