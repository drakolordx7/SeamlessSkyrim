#pragma once

#include "Portal/LightScene.h"

// The space behind a portal door, which the player is *not* in, kept alive by the engine under
// the portal's private root: attached, its NPCs in high process, its lights in the portal light
// scene, an interior's Havok world stepped by us. Behind an exterior door: the door's cell and
// its loaded neighbours. All engine calls are the ones reversed from TES::SetCurrentCell
// (AE 1.7.104), see docs/ENGINE.md, "Cells and spaces".
namespace Portal
{
	// Bounds that don't follow a subtree moved for a merge. Multibound shapes (BSMultiBoundNode, an
	// interior's rooms, a city's house blocks) keep theirs in world coordinates, and nodes flagged
	// kFixedBound (a cell's node, its land and statics nodes) never recompute theirs: moved, they
	// were culled against where they used to be (a room's walls missing seen from inside it, a
	// street's buildings and ground from indoors).
	class MovedBounds
	{
	public:
		// Before a_root is moved (its local transform set, or its parent's): its multibounds as they
		// are, what they go back to.
		void Capture(RE::NiAVObject* a_root);
		// Before a_root is moved and updated: its fixed bounds unfixed, so that the update computes
		// them (one update of the whole cell, not a second one to unfix them after).
		void UnfixBeforeMove(RE::NiAVObject* a_root);
		// After a_root was moved by a_move and updated: bounds recomputed, and the boxes captured
		// that the scene update didn't move along are moved by hand.
		void Move(RE::NiAVObject* a_root, const RE::NiTransform& a_move);
		// While moved: nodes the engine fixed since (land and statics attached as their cell finished
		// loading) are unfixed too. Their bound, fixed where it was computed, stayed in the moved
		// space after the move back: the land in front of the Bannered Mare, seen from the street,
		// was culled against the inn's own coordinates.
		void Unfix(RE::NiAVObject* a_root);
		// After everything moved is back in place and updated with its bounds still unfixed.
		void Restore();
		[[nodiscard]] std::size_t Multibounds() const { return moved.size(); }
		[[nodiscard]] std::size_t Fixed() const { return fixed.size(); }

	private:
		struct Saved
		{
			RE::NiPointer<RE::BSMultiBoundShape> shape;
			RE::NiPoint3                         center;
			RE::NiPoint3                         size;
			RE::NiMatrix3                        rotation;
		};
		std::vector<Saved>                         moved;
		std::size_t                                unmoved = 0;  // moved[unmoved..]: captured, not moved yet
		std::vector<RE::NiPointer<RE::NiAVObject>> fixed;
	};

	class RemoteCell
	{
	public:
		// Main thread. False if the cell can't be used (current cell, engine-attached, ...).
		// a_toMerge: the cell is merged into the player's space as soon as it is ready, not drawn
		// through the portal pass. a_show: its light scene is the one drawn (Show) from the start;
		// otherwise it is kept attached out of sight until shown.
		bool Attach(RE::TESObjectCELL* a_cell, bool a_toMerge = false, bool a_show = true);
		// Main thread: this space's light scene becomes the one drawn and lit (LightScene::Show).
		void Show();
		[[nodiscard]] bool Shown() const { return LightScene::Shown(scene.get()); }
		// The light scene this space's lights are in (registered or not).
		[[nodiscard]] RE::ShadowSceneNode* Scene() const { return scene.get(); }
		// Merged and not drawn: its cells left out of every cull until shown again (a room merged
		// beside the shown one whose doorway is no longer among those shown). Release shows it.
		void SetParked(bool a_parked);
		[[nodiscard]] bool Parked() const { return !parked.empty(); }
		// Set by the manager every frame for a kept space nobody can see: once loaded, Update leaves
		// its Havok world unstepped and its lights unanimated and not taken in (see Update).
		void SetPaused(bool a_paused) { paused = a_paused; }
		[[nodiscard]] bool Paused() const { return paused; }
		// a_handOver: the player is about to arrive in these (exterior) cells. They are given to
		// the engine attached, under its own root, instead of detached: the arrival's grid update
		// attaches only cells that are merely loaded (ID 13317 attaches kUnattached cells only), and attaching the
		// same cells again was most of the frame of walking out.
		void Release(bool a_handOver = false);

		// Per frame while attached: adopt late-created lights, pick the world to step, track readiness.
		void Update(float a_delta);

		// Once at startup: step the remote cell's Havok world inside the engine's step job.
		// a_enabled false: the hook is installed but never steps a second world.
		static void InstallStepHook(bool a_enabled);
		// Once at startup: the arrival's grid check accepts the cells handed over (Release), and the
		// engine's buffer of recent interiors doesn't unload a cell held attached (Holds).
		static void InstallGridHook();
		// Main thread: a_cell is attached by one of the spaces behind a door.
		[[nodiscard]] static bool Holds(const RE::TESObjectCELL* a_cell);
		static inline bool stepEnabled = true;
		// false: a room's world is stepped on the main thread at the end of the frame instead
		// (StepAtEndOfFrame), for setups where a second world in the step job deadlocks. A street's
		// is stepped there either way.
		static inline bool stepInJob = true;
		// Wait before a just-left exterior is drawn behind the door (settable from the bridge).
		static inline float kQuietSecondsLeftCell = 0.3f;
		// The street seen from indoors: its people raised to the high process and its world stepped
		// (end of frame only). Unstepped, they hung in a falling pose and moved in a jump every few
		// seconds. ([Portals] bStepExteriorFromInside; the long frames after walking out, 100-200 ms
		// about every other time, happen with and without it: measured alternating, 8 round trips.)
		static inline bool stepExterior = true;
		// [Portals] bHandOverStreet (see Release).
		static inline bool handOverStreet = true;
		static void StepAtEndOfFrame();

		// Diagnostics: the cached render passes of the remote cells' shapes; a_drop forgets the
		// lists that hold passes already returned to the pool.
		nlohmann::json RenderPasses(bool a_drop);
		// Diagnostics (bridge command portal_debug): bit mask of attach steps to leave out.
		static inline std::uint32_t debugSkip = 0;
		// Testing ([Portals] iSlowLoadMs): a space counts as loaded only this long after it was
		// attached, as on a slow computer, to see what the doorway does meanwhile.
		static inline int slowLoadMs = 0;

		// Read by the step job, written on the main thread; the lock keeps a release from
		// detaching a cell while its world is being stepped. One entry per space kept attached, by
		// its door's cell (a RemoteCell is moved between the manager's slots, the cell stays).
		static inline std::mutex                                                    stepLock;
		static inline std::vector<std::pair<const RE::TESObjectCELL*, RE::bhkWorld*>> stepWorlds;
		static inline std::atomic<std::uint64_t>                                    steppedFrames = 0;

		[[nodiscard]] bool               Attached() const { return !cells.empty(); }
		[[nodiscard]] bool               Healthy() const;
		// All references' 3D finished loading once since the attach. Before that, loader threads are
		// still building geometry and shader properties, and drawing it from the portal pass crashed.
		[[nodiscard]] bool               FullyLoaded() const;
		// This frame's Update showed every reference of every cell at full fade (still loading).
		[[nodiscard]] bool               FadeForced() const { return fadeForced; }
		// The cell the portal door leads into (the neighbours only fill the view).
		[[nodiscard]] RE::TESObjectCELL* Cell() const { return cells.empty() ? nullptr : cells.front(); }
		[[nodiscard]] RE::NiAVObject*    Cell3D() const;
		[[nodiscard]] const std::vector<RE::TESObjectCELL*>& Cells() const { return cells; }

		json Describe() const;

		// Render-merge: hang the remote cells into the world scene, moved so that the far door
		// coincides with the near one, to be drawn by the game's main pass.
		// Where: a_placement.toSource, which follows from the door pair the space is seen through
		// and that pair's hand corrections. The space remembers those with its place, so whoever
		// shows it next (through another of its doors, as the shown room or beside it) can tell
		// that it stands somewhere else.
		struct Placement
		{
			RE::NiTransform toSource;
			RE::FormID      sourceDoor = 0;
			float           turn = 0.0f;
			RE::NiPoint3    shift;

			[[nodiscard]] bool SamePlace(const Placement& a_other) const { return sourceDoor == a_other.sourceDoor && turn == a_other.turn && shift == a_other.shift; }
		};
		json Merge(const Placement& a_placement);
		// Merged: the cells placed anew (a door correction changed while looking at them, or the
		// space is now seen through another of its doors).
		void Remap(const Placement& a_placement);
		[[nodiscard]] const Placement& Placed() const { return placed; }
		// Merged, for walking into the room without leaving the world: copies of its fixed collision
		// (walls, floors, furniture) put into a_world where its 3D is drawn (a_toSource). The originals
		// stay in the room's own world, under its NPCs. a_skip: a reference left out (its door, open).
		// Replaces any copies made before.
		// a_doorCenter, a_inward: the room's door and the direction into the room, in the room's
		// coordinates; what lies wholly behind that door (the box behind it) is left out.
		void MergeCollision(const RE::NiTransform& a_toSource, RE::bhkWorld* a_world, RE::FormID a_skip, const RE::NiPoint3& a_doorCenter, const RE::NiPoint3& a_inward);
		void UnmergeCollision();
		[[nodiscard]] bool CollisionMerged() const { return !mergedBodies.empty(); }
		// The copies that stand across the room's doorway (its wall with the doorway in it, which
		// also holds the box behind the door) taken out of the world while the player passes
		// through: walked out of the room, the box's inner wall stopped them just outside the door.
		void SetDoorwayPiecesPresent(bool a_present);
		// A city gate: the cells of both worldspaces share one Havok world, and the other one's
		// buildings stand in the player's way at the gate (Tamriel's gatehouse has a raised floor
		// where Whiterun's passage is). Bodies of the attached cells near the doorway, on the
		// player's side of it (a_inward points away from the player), out of the world until
		// RestorePassage (Release does it too).
		void ClearPassage(const RE::NiPoint3& a_center, const RE::NiPoint3& a_inward, float a_halfWidth, float a_halfHeight);
		void RestorePassage();
		// Per frame while the room's collision is copied: the player and the room's people are in
		// different Havok worlds and walked through each other. Each gets a stand-in in the other's
		// world (a copy of one of its body's bones, kept where that bone is, mapped), which the other
		// side's character controllers bump into. a_world: the player's.
		void UpdateStandIns(const RE::NiTransform& a_toSource, RE::bhkWorld* a_world);
		void ClearStandIns();
		struct StandIn
		{
			RE::NiPointer<RE::bhkRigidBody> copy;
			RE::NiPointer<RE::bhkRigidBody> original;  // the bone body it follows
		};
		bool merged = false;
		// Collision objects switched to "visual only" while merged (see Merge), with their flags.
		std::vector<std::pair<RE::NiPointer<RE::NiAVObject>, std::uint32_t>> stilledClutter;

		// New on every attach, unique among all spaces; the renderer starts fresh render state when
		// it changes.
		[[nodiscard]] std::uint32_t Generation() const { return generation; }

		// Exterior cells around the door's (1 = the 3x3 block).
		static constexpr int kNeighbourRadius = 1;

	private:
		std::uint32_t                      generation = 0;
		static inline std::uint32_t        lastGeneration = 0;
		RE::NiPointer<RE::ShadowSceneNode> scene;  // the root the cells hang under; see LightScene
		RE::bhkWorld*                      stepping = nullptr;  // this space's entry in stepWorlds
		void                               StepWorld(RE::bhkWorld* a_world);
		static constexpr float kQuietSecondsRequired = 1.5f;
		static constexpr float kQuietSecondsPreloaded = 0.0f;  // drawn the frame it is attached
		float                  quietRequired = kQuietSecondsRequired;

		static RE::NiAVObject* Cell3D(const RE::TESObjectCELL* a_cell);
		static bool            Attachable(const RE::TESObjectCELL* a_cell);
		void                   AttachOne(RE::TESObjectCELL* a_cell);
		// FormIDs of the worldspace's persistent references with 3D standing in each of a_cells.
		using PersistentByCell = std::unordered_map<const RE::TESObjectCELL*, std::vector<RE::FormID>>;
		// Those without 3D are queued for loading, each once (a_queued, by FormID); returns how many
		// were queued now in a_newlyQueued.
		static PersistentByCell PersistentIn(const std::vector<RE::TESObjectCELL*>& a_cells, std::unordered_set<RE::FormID>& a_queued, int& a_newlyQueued);
		std::unordered_set<RE::FormID> persistentQueued;  // since the attach
		// Returns how many were hung under a_cell anew.
		int                    AdoptPersistent(RE::TESObjectCELL* a_cell, const PersistentByCell& a_persistent);
		float                  persistentCheck = 0.0f;  // seconds until the next round (Update)
		std::size_t            roundNext = 0;           // cells of the round still to be looked over
		PersistentByCell       roundPersistent;         // as found when the round began
		bool                   fadeForced = false;      // every cell's references shown this frame

		// A street to be merged into an interior: what the engine's change into it on the walk out
		// would read from disk, read beforehand, one piece a frame once the space is ready (Prewarm).
		std::vector<std::pair<std::int16_t, std::int16_t>> prewarmCells;  // the engine's grid around the door's cell
		bool                                               prewarmModel = false;
		RE::TESObjectCELL*                                 prewarmLand = nullptr;  // its land next
		RE::NiPointer<RE::NiNode>                          worldMapClouds;
		static inline RE::NiPointer<RE::NiNode>            handedWorldMapClouds;  // see Release
		void                                               Prewarm();

		// An interior's "always render" objects (BSPortalGraph::alwaysRenderChildren: Mistveil Keep's
		// steps up to the jarl's dais) hang nowhere: the engine culls them one by one from its portal
		// graph, which a remote cell's scene doesn't have. They are hung under a node of their cell's
		// 3D here, as long as the cell is ours (see Release).
		void                                    HangAlwaysRender(RE::TESObjectCELL* a_cell);
		void                                    UnhangAlwaysRender();
		std::vector<RE::NiPointer<RE::NiNode>> alwaysRenderHolders;

		std::vector<RE::TESObjectCELL*> cells;  // [0] = the door's cell
		std::vector<RE::NiPointer<RE::NiAVObject>> parked;  // cells' 3D hidden by SetParked
		bool                                       paused = false;
		std::vector<RE::NiPointer<RE::bhkRigidBody>> mergedBodies;  // see MergeCollision
		MovedBounds             movedBounds;  // the merged cells'
		// The cells Merge moved itself; the others (the engine's, walked in from the street) move
		// with the LOD root (Manager's MergeLOD).
		std::vector<RE::TESObjectCELL*> movedCells;
		Placement                       placed;  // while merged
		void                    MoveBounds(const RE::NiTransform& a_toSource);

	public:
		// While merged, now and then: see MovedBounds::Unfix.
		void UnfixNewBounds();
		void UnfixNewBounds(const RE::TESObjectCELL* a_cell);

	private:
		void                    RestoreBounds();
		std::vector<RE::NiPointer<RE::bhkRigidBody>> doorwayPieces;  // of mergedBodies, see SetDoorwayPiecesPresent
		RE::bhkWorld*                                mergedWorld = nullptr;
		bool                                         doorwayPiecesPresent = true;
		std::unordered_map<RE::FormID, StandIn> actorStandIns;  // the room's people, in the player's world
		StandIn                                 playerStandIn;  // the player, in the room's world
		// The last cells handed over attached (Release), until the next attach.
		static inline std::vector<RE::TESObjectCELL*> handedOver;
		// Persistent references whose own 3D was ours when the street was let go (Release).
		static inline std::vector<RE::FormID> handedPersistent;
		// Every cell attached by any space behind a door (see Holds).
		static inline std::vector<RE::TESObjectCELL*> held;

	public:
		[[nodiscard]] static const std::vector<RE::TESObjectCELL*>& HandedOver() { return handedOver; }
		// After walking out into a street let go (Release), handed over or the engine's way: the
		// persistent references whose 3D we loaded are hung into their cells as the engine's attach
		// of a cell does, each once its cell is attached.
		static void HangHandedPersistent();
		[[nodiscard]] static bool PersistentToHang() { return !handedPersistent.empty(); }
		// Persistent references' 3D hung under our exterior cells (see AttachOne).
		std::vector<RE::NiPointer<RE::NiAVObject>> borrowedPersistent;
		// Bodies ClearPassage took out of the world, and that world.
		std::vector<RE::NiPointer<RE::bhkWorldObject>> passageCleared;
		RE::bhkWorld*                                  passageWorld = nullptr;
		std::chrono::steady_clock::time_point          passageLooked{};  // ClearPassage's last look
		int                adoptedLights = 0;
		float              quietSeconds = 0.0f;  // how long the load queues have stayed empty
		int                lastQueued = 0;       // queued references last frame, all cells
		float              stuckSeconds = 0.0f;  // how long that count hasn't moved
		bool               ready = false;        // see FullyLoaded; set once per attach
		std::chrono::steady_clock::time_point attachedAt{};  // see slowLoadMs
		float              readySeconds = 0.0f;  // since ready
		bool               itemsChecked = false;  // CheckLooseItems ran for this attach

		// Loose items lying in the remote cells (weapons, food: inventory objects placed in the
		// world) were missing from the street seen from indoors. Logs which have no 3D (or 3D far
		// from where the item is, i.e. fallen), and loads the 3D of those the engine isn't loading.
		void CheckLooseItems();

		// The street seen from indoors: its loose objects' collision made visual only, as for a
		// merged room (see Merge). Their bodies aren't placed in the Havok world for it; synced from
		// them, weapons and food lying around jumped to the world's origin (Whiterun's centre) a few
		// frames after the street was attached. Restored on release (stilledClutter).
		void StillLooseObjects(RE::TESObjectCELL* a_cell);

		// An exterior's Havok world is only stepped from indoors once this finds ground under the
		// street's NPCs and loose items (see Update).
		bool  GroundUnderStreet();
		bool  groundFound = false;
		float groundCheck = 0.0f;  // seconds until the next look
	};

	// After moving a_root as a whole: each object's previous transform set to the current one, so
	// the motion vectors don't reach back to where it came from (see RemoteCell.cpp).
	void SettleMotion(RE::NiAVObject* a_root);

	// Every bound under a_root computed anew from where its shapes are now, fixed ones included.
	// A scene update leaves them: a city's land, moved into an interior and back, kept the bounds
	// it had there.
	void RecomputeBounds(RE::NiAVObject* a_root);
	// a_into grown to hold a_other too.
	void MergeBound(RE::NiBound& a_into, const RE::NiBound& a_other);
}
