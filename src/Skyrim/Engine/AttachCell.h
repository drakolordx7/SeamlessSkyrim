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

	// Main thread: inside AttachCell.
	[[nodiscard]] bool AttachingRemote();
}
