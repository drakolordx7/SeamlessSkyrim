#pragma once

// The street's distant land, objects and trees while the street is drawn in the interior the
// player is in. The engine streams them in a job (each frame, around the camera) only while the
// player is outdoors: with a save loaded indoors there were no mountains behind the street, and
// walked in from outside they stayed as they were left. While a street is shown here, that job
// goes on for it, seen from where the camera would be in the street.
namespace Portal::StreetLOD
{
	// Once, at kDataLoaded.
	void Install();

	// Main thread. a_toStreet: the interior's coordinates to the street's. a_moved: the LOD is drawn
	// in the interior's coordinates (merged), not in the street's (the portal's picture).
	void Show(RE::TESObjectCELL* a_street, const RE::NiTransform& a_toStreet, bool a_moved);
	void Hide();
}
