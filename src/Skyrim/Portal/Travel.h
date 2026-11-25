#pragma once

#include "Skyrim/Portal/Active.h"
#include "Skyrim/Doors/Hold.h"
#include "Skyrim/Doors/Pair.h"

// Walking through a load door without a loading screen.
//
// The engine's door transition runs in one frame (Main::Update -> ID 40438 -> 40744 ->
// TES::SetCurrentCell); the loading screen around it is opened by ID 13363, which skips it when
// the global ID 400337 is set (the engine sets it itself for short teleports inside one
// worldspace), and the fade before it comes from the door's HandleOpen, which Travel bypasses.
// The stalls of that transition are taken out in TransitionStalls and Spaces/WalkOut.
namespace Portal::Travel
{
	void Install();

	struct CrossingRequest
	{
		Active portal;
		// The camera was pulled in for this crossing and is let out after arrival.
		bool limitCamera = false;
		// The closest the let-out may hold the camera.
		float cameraFloor = 0.0f;
	};

	// Main thread: the player stepped through the portal's doorway. Carried out at the end of this
	// frame, after it has been drawn.
	void Request(const CrossingRequest& a_request);

	// Main thread: the player is taken through a_door to its marker the engine's way, with its
	// loading screen (the space behind isn't there to walk into).
	void ThroughDoor(RE::TESObjectREFR* a_door);

	// Per frame (player update): the purge after a gate trip, and the crossing's trace.
	void Update();

	// Per frame, right before CameraLimit(): the arrival's hold on the door and on the camera
	// moves on (the door given back, the let-out of the camera paced).
	void Tick();

	// Just came through a doorway: how far the camera may trail behind the player this frame, as
	// of the last Tick.
	[[nodiscard]] std::optional<float> CameraLimit();

	// Before a save or load: the door the player came out of is put back to normal. Held open it
	// carries activation-blocking flags, which would be written into the save with the door.
	void CloseArrivalDoor();

	// Before a load: nothing of the last crossing carries over into the loaded game.
	void ForgetTrip();

	// True once after a seamless crossing: the frame about to be drawn is the first in the new space.
	[[nodiscard]] bool ConsumeArrivalFrame();
	// The next frame drawn is the first in a new space (not consumed).
	[[nodiscard]] bool ArrivalFramePending();

	// The door the player last came out of through a seamless crossing, in the space they are in;
	// 0 once they have walked away from it.
	[[nodiscard]] RE::FormID ArrivalDoor();

	[[nodiscard]] json Status();
}
