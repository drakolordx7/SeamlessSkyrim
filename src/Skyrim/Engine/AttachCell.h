#pragma once

// The engine's cell attach, as TES::SetCurrentCell does it (AE 1.7.104), for cells we hang under
// a root of our own instead of the world's.
namespace Engine
{
	// Loads the cell's temporary references (ID 19111; a no-op when loaded), attaches it under
	// a_parent (ID 18976) and finishes the attach (ID 19063: state -> Attached).
	// The attach installs the cell's room graph in the world light scene (ID 106385, from
	// ID 18976): while this runs, AttachingRemote() is true so that the hook on that call can leave
	// the world's graph alone.
	void AttachCell(RE::TESObjectCELL* a_cell, RE::NiNode* a_parent);

	// The engine's detach of a_cell (ID 19130, with its 3D released).
	void DetachCell(RE::TESObjectCELL* a_cell);

	// Makes a_world's room graph and multi-bound map. The engine does it when a worldspace becomes the
	// current one (TES::SetWorldSpace, ID 13315, its first call, ID 20514) and an attach of one of its
	// cells reads them before that (ID 18978 -> 19437 reads ID 20514's map for each reference in a room):
	// a cell of Blackreach's door to Alftand attached while the player is in Blackreach crashed on a null
	// map. Does nothing when the graph is there. False where the function's SE ID isn't known (SE 1.5.97):
	// no cell of that worldspace may be attached.
	[[nodiscard]] bool PrepareWorldSpace(RE::TESWorldSpace* a_world);

	// Main thread: inside AttachCell.
	[[nodiscard]] bool AttachingRemote();
}
