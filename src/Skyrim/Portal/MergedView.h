#pragma once

#include "Skyrim/Doors/Pair.h"
#include "Skyrim/Portal/Active.h"
#include "Skyrim/Portal/MergedCull.h"
#include "Skyrim/Spaces/RemoteCell.h"

// The shown room merged into the world (RemoteCell::Merge), drawn by the engine's main pass. The
// native renderer (Render/Native) masks the merged space and the player's against each other. It
// needs the room's doorway every frame, and IsMergedShape tells it per draw whether a shape is in
// the room. What the room needs while it is drawn, besides its cells (RemoteCell): its doors, its
// listed shapes, its light, its people's 3D.
namespace Portal::MergedView
{
	// How long after a space behind a door is attached its references skip the engine's fade-in
	// (merged, SkipFadeIn takes over): long enough for those loaded last.
	inline constexpr float kShowAtOnceSeconds = 3.0f;

	// Before the renderer can ask (Render::SceneQuery), whether or not portals are on; and the
	// settings ([Portals] bMerge, bMergeOutdoors, bPauseUnseenRooms).
	void Install();

	// [Portals] bMerge (on by default): from outside, draw the interior merged into the world.
	void               SetMergeEnabled(bool a_enabled);
	[[nodiscard]] bool MergeEnabled();
	// The native renderer is installed and merging is on.
	[[nodiscard]] bool MergeSupported();
	// Merging puts an interior into the exterior's scene. The other way round (the street, its land
	// and the distant LOD put into an interior's scene) is bMergeOutdoors: indoors the LOD stays
	// loaded, only hidden, and the sky's dome stays in the scene (bridge outdoors_probe).
	[[nodiscard]] bool Mergeable(const RE::TESObjectCELL* a_here, const RE::TESObjectCELL* a_behindDoor);

	// [Portals] bPauseUnseenRooms (on by default): a kept room (loaded out of sight) is paused (not
	// simulated, its lights not updated, not drawn).
	[[nodiscard]] bool PauseUnseenRooms();
	void               SetPauseUnseenRooms(bool a_pause);
	// Kept rooms are paused (PauseUnseenRooms).
	[[nodiscard]] bool KeptRoomPaused();

	// Where a space merged through a_portal stands.
	[[nodiscard]] Spaces::RemoteCell::Placement PlacementOf(const Active& a_portal, const Doors::Mapping& a_mapping);

	// Main thread, every frame the shown space is merged: its room is drawn and updated.
	// a_wanted: its door swings towards open. Its cull is set in a_cull.
	void UpdateShown(const Active& a_portal, bool a_wanted, float a_delta, std::optional<MergedCull::Room>& a_cull);
	// The shown room no longer drawn: everything its drawing changed goes back. Its cells stay
	// merged until released (RemoteCell::Release).
	void EndShown();

	// Publishes what the renderer asks for (IsMergedShape, IsPlayerShape, IsShownCell), whenever the room is merged or let go.
	void Publish();
	// The cells on screen behind a door right now (merged into the world, or drawn by the portal
	// pass), for the actor cull; a_shown false: none.
	void PublishShownCells(bool a_shown);
	// The Havok world the lights' line of sight is measured in (LightOcclusion): the shown room's,
	// from its own door. a_ready: the portal whose other side is ready.
	void UpdateLightWorlds(const std::optional<Active>& a_ready);

	// Main thread: someone entered or left the merged space; its shapes are listed anew at once.
	void RefreshShapes();
	// Main thread: every cell merged into the world and drawn now.
	[[nodiscard]] std::vector<RE::TESObjectCELL*> MergedCells();

	// Any thread: the shape is in the merged room (or is the water of a street merged into an interior).
	[[nodiscard]] bool IsMergedShape(const RE::BSGeometry* a_shape);
	// Any thread: the cell is on screen behind a door right now.
	[[nodiscard]] bool IsShownCell(const RE::TESObjectCELL* a_cell);
	// Any thread: the shape is part of the player (drawn on both sides of a merged doorway).
	[[nodiscard]] bool IsPlayerShape(const RE::BSGeometry* a_shape);

	// How far the shown room's door is open, as it is drawn (0 shut).
	[[nodiscard]] float ShownAmount();
	[[nodiscard]] bool  ShownShut();

	// Render: how bright a room's own lights and ambient are drawn right now, merged behind a_door
	// and seen from the street: as an eye used to the street sees it (its brightness against the
	// street's, Pictures), as from inside within a few steps of the doorway.
	[[nodiscard]] float RoomLightScale(RE::FormID a_door);
	// How bright an eye used to the view sees a_door's room, against the room as drawn from inside:
	// 1 or less (the room is never drawn brighter than from inside).
	[[nodiscard]] float StreetEyeScale(RE::FormID a_door);

	// Diagnostics, read only.
	struct RoomInfo
	{
		RE::FormID  door;  // in the player's space; 0 while not drawn
		float       swing;
		std::size_t shapes;
		float       away;  // how far it is drawn as an eye used to the street sees it
	};
	// The shown room, if the shown space is merged.
	[[nodiscard]] std::optional<RoomInfo> Shown();
	// Every shape listed in the merged room.
	[[nodiscard]] std::vector<const RE::BSGeometry*> Shapes();
}
