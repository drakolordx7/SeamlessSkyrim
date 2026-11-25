#pragma once

#include "Portal/Active.h"

struct IDXGISwapChain;

// Runs the door portals: picks the load door the player is approaching, keeps the cell behind it
// alive (RemoteCell), shows it in the doorway (merged into the world, or drawn by PortalRender)
// and carries the player through without a loading screen when they walk in (Travel).
namespace Portal::Manager
{
	using Active = Portal::Active;
	using DoorPrompt = Portal::DoorPrompt;

	// Reads Data/SKSE/Plugins/SeamlessSkyrim.ini ([Portals] bEnabled, on by default) and hooks
	// the events the manager reacts to. Call at kDataLoaded.
	void Install();

	// Switching on only works when the hooks were installed at startup (HooksInstalled).
	void SetEnabled(bool a_enabled);
	[[nodiscard]] bool Enabled();
	// Portals were on in the ini at startup, or this is an agent session.
	[[nodiscard]] bool HooksInstalled();

	// Main thread, every frame (PlayerCharacter::Update hook).
	void Update(float a_delta);
	// The image space blended across the doorway, for where the camera is now. Update does it too;
	// a crossing calls it again once it has placed the camera in the new space.
	void UpdateImageSpace();

	// Release before anything that must not see a foreign attached cell (save, load).
	// a_holdoff: seconds before a portal may attach again (e.g. while a save is being written).
	// a_handOver: the player is crossing into the remote (exterior) cells right now; see
	// RemoteCell::Release.
	void ReleaseAll(std::string_view a_reason, float a_holdoff = 0.0f, bool a_handOver = false);
	// Before a load: the doors held open or left swinging by stand-ins are let go. Kept, their
	// state already matched after the load, so nothing was applied to the doors' new 3D (the box
	// behind a panel left standing in the doorway), and the old 3D was kept alive.
	void ForgetDoors();

	// Saving while walking in a room walked into from outdoors: the player is outdoors, inside the
	// walls of the room's house, on the room's copied collision. Before the save (kSaveGame): where
	// they are in the room is noted, for the SKSE co-save. After loading that save (kPostLoadGame)
	// they are carried into the room itself, to that place. Returns false when the player isn't in
	// such a room (the portal is let go as usual then).
	bool NoteRoomForSave();
	void SaveRoom(SKSE::SerializationInterface* a_interface);
	void LoadRoom(SKSE::SerializationInterface* a_interface);
	void ForgetRoom();
	void EnterRoomAfterLoad();

	// After a seamless crossing has arrived: the space just left may be attached again at once
	// (it is still loaded), so the door behind the player shows it instead of a hole.
	void EndHoldoff(float a_seconds = 0.0f);

	// Approaching the portal's doorway: how far the camera may trail behind the player this frame.
	[[nodiscard]] std::optional<float> CameraLimit();

	// For the renderer: the portal to draw this frame, if any (none while merged).
	[[nodiscard]] std::optional<Active> ActivePortal();
	// The portal's door stands (at least partly) open; shut, the portal isn't drawn on it.
	[[nodiscard]] bool DoorwayOpen();
	// Main thread: what using the door does, for its prompt (see DoorPrompt).
	[[nodiscard]] DoorPrompt Prompt(RE::FormID a_door);
	// Diagnostics: the image-space blend across the doorway is on (UpdateTone).
	[[nodiscard]] bool ToneBlendOn();
	// The portal whose other side is ready, drawn by the portal pass or merged.
	[[nodiscard]] std::optional<Active> ReadyPortal();
	// The door pair in use, merged or not, loaded or not.
	[[nodiscard]] std::optional<Active> CurrentPortal();
	struct DoorPair
	{
		RE::FormID sourceDoor;  // in the player's space
		RE::FormID linkedDoor;
	};
	// Main thread: the nearest door within a_radius that would be a portal, switched off in the
	// menu or not. The menu finds a switched-off door with it, which never becomes CurrentPortal.
	[[nodiscard]] std::optional<DoorPair> NearestDoorPair(float a_radius);
	// Main thread: the pair of the doorway the player is at, for the menu. The portal's while in
	// the room walked into, or while its door is the nearest; else the nearest door within
	// a_radius. The portal's alone, the menu showed a shop's other door (holding the portal from up
	// to ReleaseRadius away) at a door switched off, which then couldn't be switched back on.
	[[nodiscard]] std::optional<DoorPair> DoorPairAt(float a_radius);
	// The remote cells are merged into the world scene right now.
	[[nodiscard]] bool Merged();
	// Main thread: the street is merged into the interior the player is in.
	[[nodiscard]] bool StreetMerged();
	// Render: how bright a room's own lights and ambient are drawn right now, merged behind a_door
	// and seen from the street: as an eye used to the street sees it (its brightness against the
	// street's, Portal::Snapshot), as from inside within a few steps of the doorway.
	[[nodiscard]] float RoomLightScale(RE::FormID a_door);
	// Diagnostics (portal_merge): the brightness of the view and of the rooms merged, and their scales.
	[[nodiscard]] json DescribeAdaptation();
	// Diagnostics: false draws rooms seen from the street as from inside, to compare.
	void SetStreetEye(bool a_on);
	// Render: the door whose room waits to be merged until the portal pass has drawn its doorway
	// picture with the capture camera (Portal::Snapshot::CameraPictureWanted), or 0.
	[[nodiscard]] RE::FormID PictureWanted();

	// A doorway to fill with a stand-in picture (Portal::Snapshot) or one to photograph.
	struct DoorwayFill
	{
		RE::FormID door;      // in the player's space
		bool       outdoors;  // what is behind the door is outdoors
		bool       gate;      // outdoors on both sides
		float      alpha;     // 1 while the space behind isn't there; fading out once it is
	};
	// Main thread: the doors swung open before the space behind them is there (or fading out after).
	[[nodiscard]] std::vector<DoorwayFill> StandIns();
	// Main thread, render: the doorway shows the real space, fully open and settled.
	[[nodiscard]] std::optional<DoorwayFill> SnapshotChance();

	// For the in-game menu (main thread).
	void                SetStepExterior(bool a_step);
	[[nodiscard]] bool  StepExterior();
	// How close (game units) the player comes before the space behind a door loads (300 units
	// further out) and the door can be walked through. [Portals] iDoorOpenDistance.
	[[nodiscard]] float DoorOpenDistance();
	void                SetDoorOpenDistance(float a_distance);
	// Outdoors, how many rooms behind doors are active at once, the nearest door's included
	// ([Portals] iShownRooms; 1 = that one only): the others are the rooms behind the nearest other
	// doors, kept loaded so they open at once, and those whose door is open are drawn in it, merged
	// beside the shown one, as many as the render backend can mask (Render::Backend::MergedRooms: 1
	// on the game's own shaders, where the others stay loaded out of sight).
	[[nodiscard]] int ShownRooms();
	void              SetShownRooms(int a_count);
	inline constexpr int kMaxShownRooms = 4;
	// [Portals] bPauseUnseenRooms (on by default): a kept room nobody can see is paused (not
	// simulated, its lights not updated, not drawn) until its doorway comes into view.
	[[nodiscard]] bool PauseUnseenRooms();
	void               SetPauseUnseenRooms(bool a_pause);
	// Main thread: every cell merged into the world and drawn now, the shown room's and those beside
	// it (not paused ones).
	[[nodiscard]] std::vector<RE::TESObjectCELL*> MergedCells();
	// Any thread: which merged room a shape is in, by its slot with the render backend (0 the shown
	// room), or -1.
	[[nodiscard]] int MergedRoomOf(const RE::BSGeometry* a_shape);
	// Any thread: the light scene of the merged room in a_slot.
	[[nodiscard]] RE::ShadowSceneNode* MergedRoomScene(int a_slot);
	// Main thread, right before the player's view: the rooms merged beside the shown one get the
	// light update the engine gives registered light scenes only (LightScene::PrepareUnshownForView).
	void BeforePlayerView();
	// Main thread: someone entered or left the merged space; its shapes are listed anew at once.
	void RefreshMergedShapes();
	[[nodiscard]] nlohmann::json RemoteRenderPasses(bool a_drop);
	// Any thread: the cell is on screen behind a door right now (merged into the world, or drawn
	// by the portal pass).
	[[nodiscard]] bool IsShownRemoteCell(const RE::TESObjectCELL* a_cell);
	// Any thread: the shape is part of the player (drawn on both sides of a merged doorway).
	[[nodiscard]] bool IsPlayerShape(const RE::BSGeometry* a_shape);

	// [Portals] bMerge (on by default): from outside, draw the interior merged into the world.
	void SetMergeEnabled(bool a_enabled);
	[[nodiscard]] bool MergeEnabled();
	// Diagnostics: false lights the merged space like the world (no own directional / ambient).
	void SetMergedOwnLighting(bool a_own);

	// Main thread: every cell kept alive behind the portal (the door's cell first).
	[[nodiscard]] const std::vector<RE::TESObjectCELL*>& RemoteCells();

	json Status();

	// Diagnostics: merge the current portal's cell now, see RemoteCell::Merge.
	json MergeTest();
	// Tests: still things (statics, trees, furniture...) within a_radius of the player in the player's
	// own space, and of the door in the merged space, that hang nowhere or aren't drawn where they
	// belong; the merged street's land and distant LOD the same. "ok": none found.
	json SceneCheck(float a_radius);
	// Diagnostics: merged shapes (or their skin bones) farther than a_far from a_near.
	json MergedShapesReport(const RE::NiPoint3& a_near, float a_far);
	[[nodiscard]] bool IsMergedShape(const RE::BSGeometry* a_shape);
}
