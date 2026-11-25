#pragma once

#include "Portal/Active.h"
#include "Doors/Hold.h"
#include "Doors/Pair.h"

// Walking through a load door without a loading screen.
//
// The engine's door transition runs in one frame (Main::Update -> ID 40438 -> 40744 ->
// TES::SetCurrentCell); the loading screen around it is opened by ID 13363, which skips it when
// the global ID 400337 is set (the engine sets it itself for short teleports inside one
// worldspace), and the fade before it comes from the door's HandleOpen, which Travel bypasses.
namespace Portal::Travel
{
	// Once, at kDataLoaded.
	void Install();

	// Main thread: the player stepped through the portal's doorway. Carried out at the end of this
	// frame, after it has been drawn.
	// a_limitCamera: the camera was pulled in for this crossing and is let out after arrival.
	// a_cameraFloor: the closest the let-out may hold the camera.
	// a_exact: the player is already standing in the other space as drawn (a merged room walked into
	// on its copied collision): they come out exactly where that is, not a step past the far door.
	void Request(const Active& a_portal, bool a_limitCamera, float a_cameraFloor = 0.0f, bool a_exact = false);

	// Right after loading a save: the player is taken through a_door (a load door, without opening it)
	// to a_position (in the space it leads to) facing a_yaw, with the game's usual loading screen.
	void ThroughDoorTo(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_position, float a_yaw);

	// Main thread: the player is taken through a_door to its marker the engine's way, with its
	// loading screen (the space behind isn't there to walk into).
	void ThroughDoor(RE::TESObjectREFR* a_door);

	// Per frame (player update): records frame times while a transition is running.
	void Update();

	// Just came through a doorway: how far the camera may trail behind the player this frame.
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

	// Diagnostics: sample the main thread's stacks for 1.5 s from the next crossing on (Sampler).
	void ProfileNext(const std::filesystem::path& a_file);

	// Diagnostics: which parts of the engine's transition are skipped or changed (skipLoadWait,
	// skipLoadScreenClose, skipExtraFrame, spreadLights).
	json SetOptions(const json& a_options);
}
