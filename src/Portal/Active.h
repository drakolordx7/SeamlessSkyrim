#pragma once

// The types Portal::Manager hands to its callers, in a header of their own so that the door and
// travel modules can use them without including the manager.
namespace Portal
{
	struct Active
	{
		RE::FormID         sourceDoor;  // in the player's space
		RE::FormID         linkedDoor;  // in the remote cell
		RE::TESObjectCELL* cell;
		std::uint32_t      generation;  // changes whenever the remote cell is (re)attached
	};

	// Main thread: what using the door does, for its prompt: nothing (it stands open), shut it
	// (someone opened it, DoorUse), or what it does for any door (open it).
	enum class DoorPrompt
	{
		kOpen,
		kClose,
		kNone,
	};
}
