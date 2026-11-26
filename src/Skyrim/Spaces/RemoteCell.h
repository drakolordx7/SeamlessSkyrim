#pragma once

#include "Skyrim/Portal/LightScene.h"
#include "Skyrim/Spaces/Placement.h"

// The space behind a portal door, which the player is *not* in, kept alive by the engine under
// the portal's private root: attached, its lights in the portal light scene and, while it is live, its NPCs in
// high process and an interior's Havok world stepped by us. Behind an exterior door: the door's cell and
// its loaded neighbours. All engine calls are the ones reversed from TES::SetCurrentCell
// (AE 1.7.104), see docs/ENGINE.md, "Cells and spaces".
//
// A space nobody can see (paused: kept loaded behind a shut or far door, T3) is dormant, as the engine
// leaves a cell it has loaded without the player in it: its people stay in the process level the engine
// put them in (no high process, which is what runs their AI), its Havok world is not stepped. Only when
// the space is the live one (T4, not paused) are its people raised to the high process; paused again,
// they are put back to the level they had (SettleActors).
//
// One class, in files by reason to change: Attach.cpp (lifecycle, readiness, persistent
// references, release), Havok.cpp (stepping), Placement.cpp (merging into the world's scene,
// bounds), Collision.cpp (a gate's passage), EngineHooks.cpp (hooks on the engine's attach and
// unload). The bridge describes a space from the accessors (Bridge::PortalDiagnostics).
namespace Spaces
{
	class RemoteCell
	{
	public:
		// Main thread. False if the cell can't be used (current cell, engine-attached, ...).
		// a_toMerge: the cell is merged into the player's space as soon as it is ready, not drawn
		// through the portal pass. a_show: its light scene is the one drawn (Show) from the start;
		// otherwise it is kept attached out of sight until shown.
		bool Attach(RE::TESObjectCELL* a_cell, bool a_toMerge = false, bool a_show = true);
		// Main thread: attached out of sight for its doorway pictures alone (Portal/Ahead), and let go
		// when they are taken. Vanilla behaviour: nothing in it runs. Its people stay in the process the
		// engine put them in (not raised to the high process, which is what runs their AI), its Havok
		// world is never stepped (not even while it loads), its references' animation graphs are not
		// run, and no loose item is touched. Only what a picture needs happens: the engine's own load of
		// the cells' 3D, the lights taken in, the references faded in. Interiors only.
		bool AttachForPicture(RE::TESObjectCELL* a_cell);
		[[nodiscard]] bool ForPicture() const { return forPicture; }
		// Main thread: this space's light scene becomes the one drawn and lit (Portal::LightScene::Show).
		void Show();
		[[nodiscard]] bool Shown() const { return Portal::LightScene::Shown(scene.get()); }
		// The light scene this space's lights are in (registered or not).
		[[nodiscard]] RE::ShadowSceneNode* Scene() const { return scene.get(); }
		// Set by the manager every frame for a kept space nobody can see: Update leaves its Havok world
		// unstepped (also while it loads) and, once loaded, its lights unanimated and not taken in (see
		// Update). A space attached out of sight starts paused. The people follow in SettleActors.
		void SetPaused(bool a_paused) { paused = a_paused; }
		[[nodiscard]] bool Paused() const { return paused; }
		// Main thread, once a frame after the engine's jobs are done (Travel's end-of-frame hook; raising or
		// lowering a process while the job threads update the actors raced them before, see KNOWN-ISSUES):
		// the space is live (not paused, not for a picture) and its people are not raised yet: every living
		// one not in the high process goes there, as the engine does for a cell the player enters, and what
		// level each had is remembered. The space is paused again: those still in its cells (not walked
		// into the player's space) are put back to that level, MoveToMiddleHigh / MoveToMiddleLow /
		// MoveToLow, the engine's own moves. People who were in the high process already are left alone.
		void SettleActors();
		// The people of the space and the process level each is in, for the bridge: counts per level over
		// the cells' own actors, how many were raised by us, and whether they are raised now.
		[[nodiscard]] json DescribeActors() const;
		// a_handOver: the player is about to arrive in these (exterior) cells. They are given to
		// the engine attached, under its own root, instead of detached (see Spaces/WalkOut).
		void Release(bool a_handOver = false);

		void Update(float a_delta);

		[[nodiscard]] bool               Attached() const { return !cells.empty(); }
		[[nodiscard]] bool               Healthy() const;
		// All references' 3D finished loading once since the attach. Before that, loader threads are
		// still building geometry and shader properties, and drawing it from the portal pass crashed.
		[[nodiscard]] bool               FullyLoaded() const { return !cells.empty() && ready; }
		// This frame's Update showed every reference of every cell at full fade (still loading).
		[[nodiscard]] bool               FadeForced() const { return fadeForced; }
		// The cell the portal door leads into (the neighbours only fill the view).
		[[nodiscard]] RE::TESObjectCELL* Cell() const { return cells.empty() ? nullptr : cells.front(); }
		[[nodiscard]] RE::NiAVObject*    Cell3D() const;
		[[nodiscard]] static RE::NiAVObject* Cell3D(const RE::TESObjectCELL* a_cell);
		[[nodiscard]] static std::vector<RE::Actor*> ActorsIn(const RE::TESObjectCELL* a_cell);
		[[nodiscard]] const std::vector<RE::TESObjectCELL*>& Cells() const { return cells; }
		// New on every attach, unique among all spaces; the renderer starts fresh render state when
		// it changes.
		[[nodiscard]] std::uint32_t Generation() const { return generation; }
		[[nodiscard]] int AdoptedLights() const { return adoptedLights; }
		// Frames in which a remote world was stepped, since the last attach of any space.
		[[nodiscard]] static std::uint64_t SteppedFrames() { return steppedFrames; }
		// Raised to the high process by us and not lowered yet (see SettleActors).
		[[nodiscard]] bool ActorsRaised() const { return awake; }
		// Persistent references' 3D hung under our exterior cells (see AdoptPersistent).
		[[nodiscard]] const std::vector<RE::NiPointer<RE::NiAVObject>>& BorrowedPersistent() const { return borrowedPersistent; }

		// Exterior cells around the door's (1 = the 3x3 block).
		static constexpr int kNeighbourRadius = 1;

		// Where the merged cells stand: a_placement.toSource, which follows from the door pair the
		// space is seen through and that pair's hand corrections. The space remembers those with its
		// place, so whoever shows it next (through another of its doors, as the shown room or beside
		// it) can tell that it stands somewhere else.
		struct Placement
		{
			RE::NiTransform toSource;
			RE::FormID      sourceDoor = 0;
			float           turn = 0.0f;
			RE::NiPoint3    shift;

			[[nodiscard]] bool SamePlace(const Placement& a_other) const { return sourceDoor == a_other.sourceDoor && turn == a_other.turn && shift == a_other.shift; }
		};
		struct MergeResult
		{
			int         cells;   // moved into the world's scene
			std::string parent;  // the node they hang under
		};
		// Render-merge: hang the remote cells into the world scene, moved so that the far door
		// coincides with the near one, to be drawn by the game's main pass.
		MergeResult Merge(const Placement& a_placement);
		[[nodiscard]] bool Merged() const { return merged; }
		// Merged: the cells placed anew (a door correction changed while looking at them, or the
		// space is now seen through another of its doors).
		void Remap(const Placement& a_placement);
		[[nodiscard]] const Placement& Placed() const { return placed; }
		// While merged, now and then: see MovedBounds::Unfix.
		void UnfixNewBounds(const RE::TESObjectCELL* a_cell);
		// While merged, now and then: references whose 3D came after the merge stand where the loader
		// left them, the cell's own coordinates, and no scene update moves them again (static things
		// are never updated). The cell is updated once more, so that they stand under it as the rest
		// does. Returns how many were found out of place.
		int SettleLatePieces(RE::TESObjectCELL* a_cell);
		// Tests (bridge late_pieces): the settling switched off, to see a room with late pieces as it was.
		static void SetSettleLate(bool a_on) { settleLate = a_on; }

		// A city gate: the cells of both worldspaces share one Havok world, and the other one's
		// buildings stand in the player's way at the gate (Tamriel's gatehouse has a raised floor
		// where Whiterun's passage is). Bodies of the attached cells near the doorway, on the
		// player's side of it (a_inward points away from the player), out of the world until
		// RestorePassage (Release does it too).
		void ClearPassage(const RE::NiPoint3& a_center, const RE::NiPoint3& a_inward, float a_halfWidth, float a_halfHeight);
		void RestorePassage();

		// Once at startup: step the remote cell's Havok world inside the engine's step job.
		// a_enabled false: the hook is installed but never steps a second world.
		static void InstallStepHook(bool a_enabled);
		[[nodiscard]] static bool SteppingEnabled() { return stepEnabled; }
		static void StepAtEndOfFrame();
		// Once at startup: the engine's buffer of recent interiors doesn't unload a cell held
		// attached (Holds), and its attach leaves the world's room graph alone.
		static void InstallGridHook();
		// Main thread: a_cell is attached by one of the spaces behind a door.
		[[nodiscard]] static bool Holds(const RE::TESObjectCELL* a_cell);
		// After walking out into a street let go (Release), handed over or the engine's way: the
		// persistent references whose 3D we loaded are hung into their cells as the engine's attach
		// of a cell does, each once its cell is attached.
		static void HangHandedPersistent();
		[[nodiscard]] static bool PersistentToHang() { return !handedPersistent.empty(); }

	private:
		// Collision objects switched to "visual only" while merged (see Merge), with their flags.
		using Stilled = std::vector<std::pair<RE::NiPointer<RE::NiAVObject>, std::uint32_t>>;

		// FormIDs of the worldspace's persistent references with 3D standing in each of a_cells.
		using PersistentByCell = std::unordered_map<const RE::TESObjectCELL*, std::vector<RE::FormID>>;

		static inline bool settleLate = true;

		static constexpr float kQuietSecondsRequired = 1.5f;
		static constexpr float kQuietSecondsPreloaded = 0.0f;  // drawn the frame it is attached

		static bool                     Attachable(const RE::TESObjectCELL* a_cell);

		// Attach.cpp
		void AttachOne(RE::TESObjectCELL* a_cell);
		// Those without 3D are queued for loading, each once (a_queued, by FormID); returns how many
		// were queued now in a_newlyQueued.
		static PersistentByCell PersistentIn(const std::vector<RE::TESObjectCELL*>& a_cells, std::unordered_set<RE::FormID>& a_queued, int& a_newlyQueued);
		// Returns how many were hung under a_cell anew.
		int  AdoptPersistent(RE::TESObjectCELL* a_cell, const PersistentByCell& a_persistent);
		// An interior's "always render" objects (BSPortalGraph::alwaysRenderChildren: Mistveil Keep's
		// steps up to the jarl's dais) hang nowhere: the engine culls them one by one from its portal
		// graph, which a remote cell's scene doesn't have. They are hung under a node of their cell's
		// 3D here, as long as the cell is ours (see Release).
		void HangAlwaysRender(RE::TESObjectCELL* a_cell);
		void UnhangAlwaysRender();
		// An exterior space: what the engine's change into it (the grid around the arrival cell) would
		// read from the plugin files, read beforehand, one piece a frame once the space is ready, kept
		// out of sight or shown.
		void               Prewarm();
		[[nodiscard]] bool PrewarmPending() const { return prewarmModel || prewarmLand || !prewarmCells.empty(); }
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
		bool GroundUnderStreet();

		// Processes.cpp
		// a_actor to the high process unless it is there; remembered with its level for LowerActors.
		void Raise(RE::Actor* a_actor);
		void RaiseActors();
		void LowerActors();

		// Havok.cpp
		void StepWorld(RE::bhkWorld* a_world);
		static int  StepAll(RE::bhkWorld* a_stepped, bool a_inJob);
		static void StepInEngineJob(RE::bhkWorld* a_world, std::uint32_t a_arg);

		// Placement.cpp
		void MoveBounds(const RE::NiTransform& a_toSource);
		void RestoreBounds();
		// The object's collision object switched to "visual only" (no longer driving its 3D from its
		// body) and kept in stilledClutter, which Release puts back. False if it has none, or it
		// isn't active.
		bool StillCollision(RE::NiAVObject* a_object);

		std::vector<RE::TESObjectCELL*>            cells;  // [0] = the door's cell
		RE::NiPointer<RE::ShadowSceneNode>         scene;  // the root the cells hang under; see LightScene
		std::uint32_t                              generation = 0;
		float                                      quietRequired = kQuietSecondsRequired;
		bool                                       paused = false;
		bool                                       forPicture = false;  // AttachForPicture
		// The people raised to the high process (SettleActors), with the level each had before.
		struct Raised
		{
			RE::ActorHandle  actor;
			RE::PROCESS_TYPE before;
		};
		std::vector<Raised>                        raised;
		bool                                       awake = false;  // raised, and not lowered since

		// Readiness, per attach.
		int   adoptedLights = 0;
		float quietSeconds = 0.0f;  // how long the load queues have stayed empty
		int   lastQueued = 0;       // queued references last frame, all cells
		float stuckSeconds = 0.0f;  // how long that count hasn't moved
		bool  ready = false;        // see FullyLoaded; set once per attach
		float readySeconds = 0.0f;  // since ready
		bool  itemsChecked = false;  // CheckLooseItems ran for this attach
		bool  fadeForced = false;    // every cell's references shown this frame
		bool  groundFound = false;
		float groundCheck = 0.0f;  // seconds until the next look

		std::unordered_set<RE::FormID> persistentQueued;  // since the attach
		float                          persistentCheck = 0.0f;  // seconds until the next round (Update)
		std::size_t                    roundNext = 0;           // cells of the round still to be looked over
		PersistentByCell               roundPersistent;         // as found when the round began
		std::vector<RE::NiPointer<RE::NiAVObject>>     borrowedPersistent;
		std::vector<RE::NiPointer<RE::NiNode>>         alwaysRenderHolders;

		std::vector<std::pair<std::int16_t, std::int16_t>> prewarmCells;  // the engine's grid around the door's cell
		bool                                               prewarmModel = false;
		RE::TESObjectCELL*                                 prewarmLand = nullptr;  // its land next
		RE::NiPointer<RE::NiNode>                          worldMapClouds;

		// Merged.
		bool                            merged = false;
		Stilled                         stilledClutter;
		MovedBounds                     movedBounds;
		// The cells Merge moved itself; the others (the engine's, walked in from the street) move
		// with the LOD root (Manager's MergeLOD).
		std::vector<RE::TESObjectCELL*> movedCells;
		Placement                       placed;
		// Bodies ClearPassage took out of the world, and that world.
		std::vector<RE::NiPointer<RE::bhkWorldObject>> passageCleared;
		RE::bhkWorld*                                  passageWorld = nullptr;
		std::chrono::steady_clock::time_point          passageLooked{};  // ClearPassage's last look

		// Read by the step job, written on the main thread; the lock keeps a release from
		// detaching a cell while its world is being stepped. One entry per space kept attached, by
		// its door's cell (a RemoteCell is moved between the manager's slots, the cell stays).
		RE::bhkWorld* stepping = nullptr;  // this space's entry in stepWorlds
		static inline std::mutex                                                       stepLock;
		static inline std::vector<std::pair<const RE::TESObjectCELL*, RE::bhkWorld*>> stepWorlds;
		static inline std::atomic<std::uint64_t>                                       steppedFrames = 0;
		static inline bool                                                             stepEnabled = true;

		static inline std::uint32_t                   lastGeneration = 0;
		static inline RE::NiPointer<RE::NiNode>       handedWorldMapClouds;  // see Release
		// Persistent references whose own 3D was ours when the street was let go (Release).
		static inline std::vector<RE::FormID>         handedPersistent;
		// Every cell attached by any space behind a door (see Holds).
		static inline std::vector<RE::TESObjectCELL*> held;
	};
}
