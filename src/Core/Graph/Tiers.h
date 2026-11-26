#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "Core/Graph/SpaceGraph.h"
#include "Core/Math/Vec3.h"
#include "Core/Selection/Ranking.h"

// How much of each space is held, decided every tick from plain measurements. Nothing here calls
// the engine: Tick() returns the changes, and the adapter carries them out (and tells the
// scheduler if it could not).
//
// The rules (.weaver/DESIGN.md): vanilla behaviour, only the view changes. Only the player's space
// simulates, plus at most maxLive spaces behind a door that is open and within reach; nothing
// behind a closed or far door does. Further out a space is shown as a picture, closer as a depth
// picture, and kept loaded but paused when it is likely to be entered next.
namespace Core::Graph
{
	enum class Tier : std::uint8_t
	{
		Cold,          // T0: nothing held
		Image,         // T1: a picture drawn in the doorway
		DepthImage,    // T2: a picture with depth
		LoadedPaused,  // T3: loaded and attached, no AI, physics or lights; ready to cross
		Live,          // T4: simulated and drawn
	};

	[[nodiscard]] constexpr const char* Name(Tier a_tier)
	{
		constexpr const char* names[] = { "cold", "image", "depth-image", "loaded-paused", "live" };
		return names[static_cast<int>(a_tier)];
	}

	// What holding a space at a tier costs. Estimates the adapter measures or guesses; the defaults are guesses.
	struct SpaceCost
	{
		// What holding the doorway's pictures takes, in video memory, at the default size (512 texels,
		// 3 views; ForPictures works it out for another): a room's image is 1344 x 896 half-float texels
		// with its mips, ~12 MB. The depth tier keeps the image too (the depth image falls back to it
		// where its views don't reach), so it is the image and the views' ~10 MB.
		float imageMb = 12.3f;
		float depthMb = 22.7f;
		float loadedMb = 120.0f;  // loaded and paused
		float liveMb = 20.0f;     // more, once live
		float attachMs = 1.5f;    // frame time a slice of attaching takes
		float liveMs = 2.0f;      // frame time per frame while live, the player's own space excepted

		// The picture costs of a room door's pictures at a_pictureHeight texels across the doorway's
		// height and a_depthLayers views (Settings iDoorwayPictureHeight, iDoorwayDepthLayers).
		[[nodiscard]] static SpaceCost ForPictures(int a_pictureHeight, int a_depthLayers);

		[[nodiscard]] constexpr float Mb(Tier a_tier) const
		{
			switch (a_tier) {
			case Tier::Image:
				return imageMb;
			case Tier::DepthImage:
				return depthMb;
			case Tier::LoadedPaused:
				return loadedMb;
			case Tier::Live:
				return loadedMb + liveMb;
			default:
				return 0.0f;
			}
		}
	};

	struct SchedulerConfig
	{
		// Spaces besides the player's that may be live at once.
		std::uint32_t maxLive = 1;
		// Spaces besides the live ones that may be kept loaded and paused.
		std::uint32_t maxLoaded = 3;
		// A door within this reach (gates count nearer, Selection::Reach) can make its space live when it is open.
		float liveReach = 450.0f;
		// Out to here the doorway holds a depth picture, and a picture beyond that out to imageReach.
		float depthReach = Selection::kDepthReach;
		float imageReach = Selection::kWarmDistance * Selection::kSparseReach;
		// Spaces are loaded for where the player will be this many seconds ahead, at the speed they
		// close in on the door: a run towards a door starts the load earlier than a stroll.
		float leadSeconds = 3.0f;
		// A door must have this much frame-time headroom, besides the space's own cost, to make a space live.
		float liveReserveMs = 1.0f;
		// At most one attach this often (Selection::kWarmAttachSeconds), and after the adapter
		// reported a failure none of that space for this long.
		float attachSeconds = Selection::kWarmAttachSeconds;
		float retrySeconds = Selection::kRetryCooldown;
		// A heavy space (loaded, T3 and up) is never let go this soon after it was attached, nor attached
		// again this soon after it was let go, whatever the ranking says in between: an attach costs
		// milliseconds and a release tears a cell's actors and physics down while the engine's job threads
		// may be updating them (the game died in an actor's character proxy when a room was released a
		// third of a second after its attach). Only the doorway no longer being a way through (locked,
		// switched off, no longer reachable from the player's space) lets one go earlier.
		float minHoldSeconds = Selection::kMinHoldSeconds;
		float reattachSeconds = Selection::kReattachSeconds;
		// The highest tier a doorway's space may reach by what the door is: a scripted door keeps the
		// engine's own behaviour (nothing here), a locked one can't be opened, so never live or loaded.
		Tier      scriptedCap = Tier::Cold;
		Tier      lockedCap = Tier::DepthImage;
		SpaceCost defaultCost;
	};

	struct Inputs
	{
		float   dt = 0.0f;  // seconds since the last tick
		SpaceIx playerSpace = kNoSpace;
		Vec3    position;  // in the player's space
		Vec3    heading;   // the direction they face, horizontal unit; zero if unknown (the velocity's direction is used)
		Vec3    velocity;  // units per second
		// Doors that are open now, in either space (the two ends of a doorway open together).
		std::span<const DoorIx> openDoors;
		// Of the open doors, the ones someone used (as against standing open: a city gate, a door set to open
		// always): the door the player has just opened is shown ahead of a gate standing open nearer by reach.
		std::span<const DoorIx> usedDoors;
		// Doors that are only doors right now (locked shut, disabled, switched off by the player): a
		// doorway with such an end is no way through, so nothing is held for it. Not a fact of the
		// graph, which is built once: whether a door is locked changes.
		std::span<const DoorIx> blockedDoors;
		// Target frame time minus the last frame's; negative when over budget.
		float headroomMs = 0.0f;
		float memoryBudgetMb = 0.0f;  // for what is held beyond the player's own space; 0 holds nothing
		// Indexed by space; spaces beyond the end use the config's default.
		std::span<const SpaceCost> costs;
	};

	enum class Reason : std::uint8_t
	{
		PlayerEntered,  // the space the player is in
		Approach,       // a door is nearer, or the player is heading for it
		Retreat,        // the door is further off (past the hysteresis)
		DoorClosed,     // live until the door shut
		LeftReach,      // not behind a door of the player's space any more
		Budget,         // memory: the least likely space gives way
		Headroom,       // no frame time to spare for a live space
	};

	struct TierChange
	{
		SpaceIx   space = kNoSpace;
		Tier      from = Tier::Cold;
		Tier      to = Tier::Cold;
		Reason    reason = Reason::Approach;
		DoorwayIx via = kNoDoorway;  // the doorway of the player's space that made it likely (kNoDoorway for the player's own space)
		float     rank = 0.0f;       // how likely: lower is likelier (Selection::Rank of the predicted reach); 0 for the player's space
	};

	class TierScheduler
	{
	public:
		explicit TierScheduler(const SpaceGraph& a_graph, const SchedulerConfig& a_config = {});

		// Every frame: what changes now. Promotions go one tier at a time (picture, depth picture,
		// loaded, live) and loading is rate limited; demotions go straight to the tier wanted.
		[[nodiscard]] std::vector<TierChange> Tick(const Inputs& a_inputs);

		// The tier the scheduler has put a space at (what the adapter should hold).
		[[nodiscard]] Tier  TierOf(SpaceIx a_space) const { return tiers[a_space]; }
		[[nodiscard]] float MemoryMb() const { return memoryMb; }

		// The adapter could not get a space to a tier: it holds a_reached instead (lower than put), and
		// the space is not promoted again for a few seconds.
		void Failed(SpaceIx a_space, Tier a_reached);
		// The doorway keeps the engine's own behaviour from now on (unmeasurable, a failed prewarm, a
		// crossing that timed out): its space is no longer held on its account.
		void Fallback(DoorwayIx a_doorway);
		[[nodiscard]] bool IsFallback(DoorwayIx a_doorway) const { return forced[a_doorway]; }

		// The budgets and reaches change while playing (the menu); takes effect at the next tick.
		void Configure(const SchedulerConfig& a_config) { config = a_config; }

		// The adapter let go of everything it held (a save, a load, the player entered another
		// space): every space is cold again and none waits to be retried. Fallbacks stay.
		void Reset();
		// The graph was built again with more in it (the adapter finds doors as cells load): the
		// tiers, retry waits and fallbacks follow their spaces and doors by id into a_new, which
		// replaces the graph this scheduler was made with. a_old is that graph, still alive.
		void Rebind(const SpaceGraph& a_old, const SpaceGraph& a_new);

	private:
		[[nodiscard]] Tier             CapOf(DoorwayIx a_doorway) const;
		[[nodiscard]] const SpaceCost& CostOf(const Inputs& a_inputs, SpaceIx a_space) const;

		const SpaceGraph*      graph;
		SchedulerConfig        config;
		std::vector<Tier>      tiers;           // by space
		std::vector<float>     retryCooldown;   // by space
		std::vector<float>     heldFor;         // by space: seconds at T3 or above, 0 below
		std::vector<float>     releasedFor;     // by space: seconds since it was let go from T3 or above
		std::vector<bool>      forced;          // by doorway
		std::vector<bool>      open;            // by door, set during a tick
		std::vector<bool>      doorUsed;           // by door, set during a tick
		std::vector<bool>      blocked;         // by door, set during a tick
		std::vector<int>       candidateOf;     // by space, -1 outside a tick
		float                  attachTimer = 0.0f;
		float                  memoryMb = 0.0f;
	};
}
