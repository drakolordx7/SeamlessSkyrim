#pragma once

// The engine events and call sites the portals hook: the crosshair's pick (an open doorway's door
// can be shut by aiming through its doorway), using a door (opening or shutting a seamless door
// instead of the trip with a loading screen), the actors' 3D position update, and the player
// taking a door the engine's way or arriving in another cell.
namespace Portal::EngineHooks
{
	// Once, with the portal on, after the other modules' hooks.
	void Install();

	// The player uses a_door as the activate key does (DoorActivateHook): a seamless door is opened,
	// or shut if a use opened it, and never taken the engine's way. False for a door that is not one
	// (the caller activates it as usual). Also the bridge's activate.
	bool UseDoor(RE::TESObjectREFR* a_door);
}
