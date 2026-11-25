#pragma once

#include "Skyrim/Doors/Pair.h"
#include "Skyrim/Portal/Active.h"
#include "Skyrim/Portal/Travel.h"

// Walking through the portal's doorway: when the player is carried through, what the camera may do
// meanwhile, the passage made through the building behind an exterior door, and the timers that
// keep a space from being attached again while a transition is under way.
namespace Portal::Crossing
{
	// Main thread, every frame (the manager's timers).
	void Tick(float a_delta);

	// A crossing is possible (not just after one).
	[[nodiscard]] bool Ready();
	// The spaces mustn't be attached now: a crossing or the engine's own door trip is under way.
	[[nodiscard]] bool AttachHeld();
	// No attaching for at least a_seconds (a save is being written, the engine takes the cells).
	void HoldAttach(float a_seconds);
	// After a seamless crossing has arrived: the space just left may be attached again after
	// a_seconds (it is still loaded, so the door behind the player shows it instead of a hole), and
	// the door behind the player is looked for at once, not with a scan's delay.
	void EndHoldoff(float a_seconds = 0.0f);

	// The player walks through the portal's doorway (a seamless crossing, carried out at the end of
	// the frame, Travel::Request): no second crossing for a second, and the space isn't attached
	// again until the arrival has settled.
	void Request(const Travel::CrossingRequest& a_request);
	// The player is taken through a_door the engine's way, with its loading screen (Travel::ThroughDoor).
	void ThroughDoor(RE::TESObjectREFR* a_door);
	// The player is about to take a_door the engine's way: the engine gets the cells it leads into,
	// not ones attached under our scene.
	void BeforeEngineTrip(RE::TESObjectREFR* a_door);

	// Main thread, every frame the portal is ready: when to carry the player through, and the
	// passage through the building. Returns how far the camera may trail behind the player this
	// frame (CameraLimit), if it is held.
	[[nodiscard]] std::optional<float> Approach(RE::PlayerCharacter* a_player, const Active& a_portal, float a_delta);

	// Where the camera is, from the local transform its Update just set.
	[[nodiscard]] RE::NiPoint3 CameraPosition();

	// The building's collision back on, the player free of the passage's height hold.
	void ClosePassage();
	// Every frame, after Approach: the hold lasts only as long as Approach asks for it, frame by
	// frame.
	void EndFrame(RE::PlayerCharacter* a_player);

	// The camera's stop at the portal's doorway plane (third and first person), set every frame
	// from the ready portal; cleared when there is none.
	void UpdateCameraPlane(RE::TESObjectCELL* a_here, const std::optional<Active>& a_ready);
}
