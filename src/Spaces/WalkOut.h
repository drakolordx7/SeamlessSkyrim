#pragma once

// Walking out into a street without a stall. Leaving an interior for the exterior it looks out on,
// the engine's change of space (TES::SetCurrentCell, ID 13317) sets the grid of cells around the
// arrival up anew and waits for the loader. The street behind the door is attached already, so
// the cells of it are handed to the engine attached (RemoteCell::Release) and the two waits that
// would have stalled the frame are let through:
//   - the grid check accepts the cells handed over,
//   - the wait for the task pool inside the change is skipped while they are there and the
//     teleport is one of ours.
namespace Spaces::WalkOut
{
	// Once at startup, with the other hooks of the remote cells. Where the grid check's call site
	// isn't found the street is not handed over (Tuning::handOverStreet is switched off).
	void Install();

	// The cells given to the engine attached by the last Release, until the next attach.
	[[nodiscard]] const std::vector<RE::TESObjectCELL*>& HandedOver();
	void Hand(RE::TESObjectCELL* a_cell);
	void Forget();
}
