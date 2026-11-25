#pragma once

// The engine's door transition (ID 40438, TES::SetCurrentCell) does things that suit a loading
// screen and stall the frame when there is none: an autosave, a wait for every queued load, the
// loading screen's close, one more draw of the space being left, and the arrival's lights all
// taken in at once. For a teleport Travel queued (a seamless one) they are skipped or spread over
// frames; trips of the game's own go through untouched. The one other stall, the outdoor grid's
// load wait, is in Spaces/WalkOut.
namespace Portal::TransitionStalls
{
	// Once, at kDataLoaded.
	void Install();

	// True while ID 40438 carries out a teleport Travel queued (set by Travel around it).
	void SetSeamless(bool a_seamless);
	[[nodiscard]] bool Seamless();
}
