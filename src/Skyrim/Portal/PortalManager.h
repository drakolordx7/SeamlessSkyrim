#pragma once

#include "Skyrim/Portal/Active.h"

namespace Spaces
{
	class RemoteCell;
}

// Runs the door portals: picks the load door the player is approaching (Selection), keeps the
// cell behind it alive (RemoteCell), shows it in the doorway (merged into the world, MergedView,
// or drawn by the portal pass) and carries the player through without a loading screen when they
// walk in (Crossing, Travel). The manager holds the portal shown and calls the modules in the one
// order of a frame.
namespace Portal::Manager
{
	// Takes its settings from Plugin::Settings (bEnabled, on by default). Called by Plugin::Install.
	void Install();

	// Switching on only works when the hooks were installed at startup (Plugin::HooksInstalled).
	void               SetEnabled(bool a_enabled);
	[[nodiscard]] bool Enabled();

	// Main thread, every frame (PlayerCharacter::Update hook).
	void Update(float a_delta);
	// A seamless crossing has arrived and the camera is about to be placed: the frame's update again,
	// for the space the player is in now.
	void OnArrival();

	// Release before anything that must not see a foreign attached cell (save, load).
	// a_holdoff: seconds before a portal may attach again (e.g. while a save is being written).
	// a_handOver: the player is crossing into the remote (exterior) cells right now; see
	// RemoteCell::Release.
	void ReleaseAll(std::string_view a_reason, float a_holdoff = 0.0f, bool a_handOver = false);
	// Before a load: the doors held open or left swinging by stand-ins are let go. Kept, their
	// state already matched after the load, so nothing was applied to the doors' new 3D (the box
	// behind a panel left standing in the doorway), and the old 3D was kept alive.
	void ForgetDoors();

	// [Portals] bMerge off while a room is merged: it is let go. (The setting is MergedView's.)
	void SetMergeEnabled(bool a_enabled);

	// Approaching the portal's doorway: how far the camera may trail behind the player this frame.
	[[nodiscard]] std::optional<float> CameraLimit();

	// The space behind the portal's door, the one shown: for the portal modules, which each work on
	// a part of it.
	[[nodiscard]] Spaces::RemoteCell& Space();
	// Main thread, once a frame with the engine's jobs done (Travel's end-of-frame call): the shown
	// space's people and the kept spaces' go to the process level their space calls for
	// (RemoteCell::SettleActors).
	void SettleActors();
	// The door pair in use, merged or not, loaded or not.
	[[nodiscard]] std::optional<Active> CurrentPortal();
	// The portal whose other side is ready, drawn by the portal pass or merged.
	[[nodiscard]] std::optional<Active> ReadyPortal();
	// For the renderer: the portal to draw this frame, if any (none while merged).
	[[nodiscard]] std::optional<Active> ActivePortal();
	// The space behind a_door is drawn in its doorway (merged or the portal ready),
	// not a stand-in's picture.
	[[nodiscard]] bool SpaceShown(RE::FormID a_door);
	// The remote cells are merged into the world scene right now.
	[[nodiscard]] bool Merged();
	// The door of the portal in use has swung shut (merged: the room's own door), so the portal can
	// be let go without a doorway left open onto nothing.
	[[nodiscard]] bool DoorShut();
	// Main thread: every cell kept alive behind the portal (the door's cell first).
	[[nodiscard]] const std::vector<RE::TESObjectCELL*>& RemoteCells();
	// The last thing the manager did to the portal, for the status.
	[[nodiscard]] const std::string& LastEvent();

	// Main thread: what using the door does, for its prompt (see DoorPrompt).
	[[nodiscard]] DoorPrompt Prompt(RE::FormID a_door);

	// For the in-game menu (main thread): the street's physics run while indoors.
	void               SetStepExterior(bool a_step);
	[[nodiscard]] bool StepExterior();
}
