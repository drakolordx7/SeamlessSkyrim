#pragma once

// The engine events and call sites the portals hook: the crosshair's pick (an open doorway's door
// can be shut by aiming through its doorway), using a door (opening or shutting a seamless door
// instead of the trip with a loading screen), the actors' 3D position update, and the player
// taking a door the engine's way or arriving in another cell.
namespace Portal::EngineHooks
{
	// Once, with the portal on, after the other modules' hooks.
	void Install();
}
