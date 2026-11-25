#pragma once

#include "Portal/Active.h"

// The street merged into the interior the player is in (docs/APPROACHES.md, 9): its cells, its
// distant land, objects and trees (the LOD) and the persistent references' 3D hung into the
// interior's scene, moved with the street, so the view out is drawn by the game's main pass
// instead of the portal pass. The other way round (a room merged into the street) is MergedView.
namespace Portal::StreetMerge
{
	// Main thread, every frame, also with portals off: the street's water reflections kept (engine
	// bug workaround, WaterReflections), and Community Shaders' skylighting settled on walking out.
	void EveryFrame();

	// The shown space is the street merged into an interior.
	[[nodiscard]] bool Merged();

	// a_portal, shown from an interior: the street's distant LOD is placed for it (StreetLOD) from
	// now on, not from the merge, as its picture is drawn before. a_here: the player's cell.
	void PlaceLOD(const Active& a_portal, RE::TESObjectCELL* a_here);
	// The street's distant LOD let go (StreetLOD), merged or not.
	void HideLOD();

	// a_door: the interior's door the street is seen through. The distant LOD shown in the
	// interior and moved with the street (a_toSource), the interior's sky dome given if it has none.
	void Show(const RE::NiTransform& a_toSource, RE::TESObjectREFR* a_door);
	// Everything Show changed put back (also the StreetLOD and the sky mode).
	void Hide();
	// Persistent references' nodes adopted since are taken on as well; every frame it is merged.
	void MergePersistent();

	// The nodes the street's own drawing is listed from: the LOD root and the persistent nodes
	// moved with it; none when the LOD isn't shown.
	[[nodiscard]] std::vector<RE::NiAVObject*> Roots();
	// The engine fixed the bounds of a root's nodes since the merge: unfixed again (MovedBounds::Unfix).
	void UnfixBounds(RE::NiAVObject* a_root);
	// Tests: where the LOD root is drawn, if shown.
	[[nodiscard]] std::optional<RE::NiPoint3> RootAt();
}
