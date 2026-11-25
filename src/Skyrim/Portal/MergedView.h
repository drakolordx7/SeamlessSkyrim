#pragma once

#include "Skyrim/Doors/Pair.h"
#include "Skyrim/Portal/Active.h"
#include "Skyrim/Portal/MergedCull.h"
#include "Skyrim/Spaces/RemoteCell.h"

// Rooms merged into the world (RemoteCell::Merge), drawn by the engine's main pass: the shown
// room's and, outdoors, those of the kept rooms beside it whose door is open. The render backend
// (the patched Community Shaders, or the native one on the game's own shaders; Render/Backend.h)
// masks the merged spaces and the player's against each other. It needs each room's doorway
// every frame, and RoomOf tells it per draw which room a shape is in.
//
// A merged room is in a slot with the render backend: slot 0 is the shown room (the only one
// that can be walked into and be the street merged into an interior), 1..3 are the rooms merged
// beside it. What a room needs while it is drawn, besides its cells (RemoteCell): its doors, its
// listed shapes, its light, its people's 3D, is kept apart from the cells so that a room handed
// from being shown beside the nearest door to being the shown one (or back) is drawn on as it
// was: nothing swung shut, listed again or unlit for a frame.
namespace Portal::MergedView
{
	// How long after a space behind a door is attached its references skip the engine's fade-in
	// (merged, SkipFadeIn takes over): long enough for those loaded last.
	inline constexpr float kShowAtOnceSeconds = 3.0f;

	// Before the fork can ask (Render::SceneQuery), whether or not portals are on; and the
	// settings ([Portals] bMerge, bMergeOutdoors, bPauseUnseenRooms).
	void Install();

	// [Portals] bMerge (on by default): from outside, draw the interior merged into the world.
	void               SetMergeEnabled(bool a_enabled);
	[[nodiscard]] bool MergeEnabled();
	// The render backend can merge and merging is on.
	[[nodiscard]] bool MergeSupported();
	// Merging puts an interior into the exterior's scene. The other way round (the street, its land
	// and the distant LOD put into an interior's scene) is bMergeOutdoors: indoors the LOD stays
	// loaded, only hidden, and the sky's dome stays in the scene (bridge outdoors_probe).
	[[nodiscard]] bool Mergeable(const RE::TESObjectCELL* a_here, const RE::TESObjectCELL* a_behindDoor);

	// [Portals] bPauseUnseenRooms (on by default): a kept room nobody can see is paused (not
	// simulated, its lights not updated, not drawn) until its doorway comes into view.
	[[nodiscard]] bool PauseUnseenRooms();
	void               SetPauseUnseenRooms(bool a_pause);
	// A kept room is paused: it isn't merged beside the shown one, or its doorway is out of sight.
	[[nodiscard]] bool KeptRoomPaused(const RE::TESObjectCELL* a_cell);

	// Where a space merged through a_portal stands.
	[[nodiscard]] Spaces::RemoteCell::Placement PlacementOf(const Active& a_portal, const Doors::Mapping& a_mapping);

	// Main thread, every frame the shown space is merged: its room is drawn and updated (slot 0).
	// a_wanted: its door swings towards open. Its cull is added to a_culls.
	void UpdateShown(const Active& a_portal, bool a_wanted, float a_delta, std::vector<MergedCull::Room>& a_culls);
	// Main thread, every frame: the kept rooms whose door is to be open are merged beside the shown
	// one, drawn and updated (slots 1..3); those no longer are, ended.
	void UpdateBeside(RE::TESObjectCELL* a_here, float a_delta, std::vector<MergedCull::Room>& a_culls);
	// The shown room no longer drawn: everything its drawing changed goes back. Its cells stay
	// merged until released (RemoteCell::Release).
	void EndShown();

	// A kept room that was merged beside the shown one becomes the shown room (a_cell, about to be
	// swapped in as the shown space): its drawing state goes with it, so it is drawn on without a
	// break.
	void TakeBeside(const RE::TESObjectCELL* a_cell);
	// The shown room, merged, left for a nearer door: kept and drawn on beside the next one instead
	// of let go. Finds it a slot (the farthest room beside makes room if none is free); 0 when it
	// can't be (the street, nothing loaded, no room for it).
	[[nodiscard]] int ReserveBesideSlot(RE::TESObjectCELL* a_here, const Active& a_portal);
	// The shown space (which Manager::Space() holds) is moved among the kept ones and its room
	// goes beside the shown one, in a_slot (ReserveBesideSlot).
	void MoveShownBeside(int a_slot, const Active& a_portal);
	// The space a room merged beside lives in is let go.
	void HideBesideOf(const RE::TESObjectCELL* a_cell, std::string_view a_why);
	// A kept space is shown beside the shown room by the door it is merged through.
	[[nodiscard]] bool BesideDoor(RE::FormID a_door);

	// Publishes what the renderer asks for (RoomOf, SceneOf, IsPlayerShape, IsShownCell), whenever a room is merged, handed from one role to the other, or let go.
	void Publish();
	// The cells on screen behind a door right now (merged into the world, or drawn by the portal
	// pass), for the actor cull; a_shown false: none.
	void PublishShownCells(bool a_shown);
	// The Havok worlds the lights' line of sight is measured in (LightOcclusion): the shown room's
	// and each room beside it in its own, from its own door. a_ready: the portal whose other side
	// is ready.
	void UpdateLightWorlds(const std::optional<Active>& a_ready);

	// Main thread, right before the player's view: the rooms merged beside the shown one get the
	// light update the engine gives registered light scenes only (LightScene::PrepareUnshownForView).
	void BeforePlayerView();
	// Main thread: someone entered or left the merged space; its shapes are listed anew at once.
	void RefreshShapes();
	// Main thread: every cell merged into the world and drawn now, the shown room's and those beside
	// it (not paused ones).
	[[nodiscard]] std::vector<RE::TESObjectCELL*> MergedCells();

	// Any thread: which merged room a shape is in, by its slot with the render backend (0 the shown
	// room), or -1.
	[[nodiscard]] int RoomOf(const RE::BSGeometry* a_shape);
	// Any thread: the light scene of the merged room in a_slot.
	[[nodiscard]] RE::ShadowSceneNode* SceneOf(int a_slot);
	// Any thread: the cell is on screen behind a door right now.
	[[nodiscard]] bool IsShownCell(const RE::TESObjectCELL* a_cell);
	// Any thread: the shape is part of the player (drawn on both sides of a merged doorway).
	[[nodiscard]] bool IsPlayerShape(const RE::BSGeometry* a_shape);

	// How far the shown room's door is open, as it is drawn (0 shut).
	[[nodiscard]] float ShownAmount();
	[[nodiscard]] bool  ShownShut();
	// The doors of the rooms merged beside, whose door is open at least a_open.
	[[nodiscard]] std::vector<std::pair<RE::FormID, RE::FormID>> OpenBesideDoors(float a_open);

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
		int                slot;
		RE::TESObjectCELL* cell;  // beside rooms (slot > 0)
		RE::FormID         door;        // in the player's space; 0 while not drawn
		RE::FormID         sourceDoor;  // beside rooms: the door they are merged through
		float              swing;
		std::size_t        shapes;
		float              daylight;
		bool               seen;
		bool               paused;
	};
	// The shown room (if the shown space is merged) and those beside it.
	[[nodiscard]] std::vector<RoomInfo> Rooms();
	// Every shape listed in a merged room.
	[[nodiscard]] std::vector<const RE::BSGeometry*> Shapes();
}
