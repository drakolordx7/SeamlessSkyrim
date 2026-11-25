#pragma once

#include <algorithm>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "Core/Math/Vec3.h"

// Which load door is the portal, and which spaces behind other doors are kept loaded out of sight
// ("warm") so that their doorway opens at once when it becomes the nearest: the distances, orders
// and hysteresis that decide it, from plain measurements.
namespace Core::Selection
{
	// The other side starts loading this much further out than the door opens (time to load it
	// while walking up) and is let go kReleaseBeyond further out still (hysteresis, so standing at
	// the boundary doesn't attach/detach every frame). Both follow the door distance: loading at a
	// fixed 1200 units, a shop merely walked past was loaded and held while the door walked to
	// waited.
	inline constexpr float kLoadAhead = 300.0f;
	inline constexpr float kReleaseBeyond = 400.0f;
	// A different door must be this much closer before we switch portals.
	inline constexpr float kSwitchMargin = 200.0f;
	// Rooms behind doors this near (at most) are kept loaded.
	inline constexpr float kWarmDistance = 2000.0f;
	// A room kept loaded is let go only this much further out, so walking along that boundary
	// doesn't attach and let go of the same room over and over.
	inline constexpr float kWarmMargin = 400.0f;
	// With few doors around, rooms are kept from this many times as far.
	inline constexpr float kSparseReach = 2.0f;
	// Up to this many spaces behind doors within reach (or as many as are loaded at once, if more)
	// count as few.
	inline constexpr std::size_t kFewDoors = 3;
	// One attach at a time, a few times a second at most: each makes a long frame (3-10 ms, more
	// for a room whose 3D has to be loaded).
	inline constexpr float kWarmAttachSeconds = 0.5f;
	// A cell that failed to attach isn't tried again for this long.
	inline constexpr float kRetryCooldown = 5.0f;
	// The scan walks every reference around the player; doors don't come and go that fast.
	inline constexpr float kScanSeconds = 0.2f;
	// Half the switch margin: turning round between two doors equally far changes their order by
	// twice this, which by itself must not swap what is loaded.
	inline constexpr float kHeadingNudge = kSwitchMargin / 2.0f;
	// A city gate seen from far down the road counts as nearer than it is, in which door is shown
	// and in how far out its space loads and is let go. Not by a fixed amount: a house door must
	// still win when walked up to near a gate (it has to be the shown one to be walked through), and
	// at a quarter of the distance it does unless it stands within four times the switch margin of
	// the gate.
	inline constexpr float kReachWeight = 0.25f;

	// The direction the player last moved in (horizontal, unit; zero until they have): the door
	// walked towards is the likelier one to be walked through.
	class HeadingTracker
	{
	public:
		// Every frame.
		void Track(const Vec3& a_position)
		{
			auto step = a_position - from;
			step.z = 0.0f;
			const float length = step.Length();
			// Over a few frames' worth of walking: a frame's step is too short to give a direction.
			if (length < 32.0f) {
				return;
			}
			from = a_position;
			// A jump is a teleport or a crossing into another space's coordinates, no direction.
			heading = length > 500.0f ? Vec3() : step / length;
		}
		[[nodiscard]] const Vec3& Heading() const { return heading; }

	private:
		Vec3 heading;
		Vec3 from;
	};

	// A door's distance as the reaches and the order of doors count it: a gate counts nearer.
	[[nodiscard]] inline float Reach(float a_distance, bool a_outdoorsBothSides)
	{
		return a_outdoorsBothSides ? a_distance * kReachWeight : a_distance;
	}

	// What doors are ordered by: the reach, a little less for a door ahead of the player and a
	// little more for one behind. a_toDoor: from the player to the door.
	[[nodiscard]] inline float Rank(float a_reach, const Vec3& a_heading, const Vec3& a_toDoor)
	{
		return a_reach - kHeadingNudge * a_heading.Dot(a_toDoor) / std::max(a_toDoor.Length(), 1.0f);
	}

	// How far out a space loads and is let go for a door that opens at a_doorOpenDistance.
	[[nodiscard]] inline float NearRadius(float a_doorOpenDistance) { return a_doorOpenDistance + kLoadAhead; }
	[[nodiscard]] inline float ActivateRadius(float a_doorOpenDistance, float a_farActivate) { return std::max(NearRadius(a_doorOpenDistance), a_farActivate); }
	[[nodiscard]] inline float ReleaseRadius(float a_doorOpenDistance, float a_farActivate) { return ActivateRadius(a_doorOpenDistance, a_farActivate) + kReleaseBeyond; }

	// Few doors around the player: nothing competes for what is loaded, so it is loaded from much
	// further out without spaces being swapped as the player moves.
	struct Crowd
	{
		bool  sparse = false;
		float farActivate = 0.0f;  // outdoors, the shown space's reach when the crowd is sparse and (outdoors) a door into another worldspace is near; else 0
		float warmReach = kWarmDistance;
	};

	// a_spacesInReach: how many spaces (not doors: an inn's two doors load one room) lie behind
	// doors within a_farReach; a_spacesInScan: behind any door the scan found. Between the two the
	// last answer stands: a door at the edge doesn't change what is loaded back and forth.
	// a_few: how many count as few. a_worldDoorNear: a door within reach leads to a worldspace (or
	// to a cell not loaded yet) rather than to an interior.
	[[nodiscard]] inline Crowd Assess(bool a_wasSparse, bool a_outdoors, std::size_t a_spacesInReach, std::size_t a_spacesInScan, std::size_t a_few, bool a_worldDoorNear, float a_farReach)
	{
		Crowd crowd;
		crowd.sparse = a_wasSparse;
		if (a_spacesInReach > a_few) {
			crowd.sparse = false;
		} else if (a_spacesInScan <= a_few) {
			crowd.sparse = true;
		}
		// Outdoors a kept room stands in for loading the shown one early, but only interiors are
		// kept: a city gate's worldspace was loaded at the near reach, its low-detail stand-in seen
		// through the open gate until then.
		crowd.farActivate = crowd.sparse && (!a_outdoors || a_worldDoorNear) ? a_farReach : 0.0f;
		crowd.warmReach = crowd.sparse && a_outdoors ? a_farReach : kWarmDistance;
		return crowd;
	}

	// The far reach of a scan: outdoors a sparse crowd reaches kSparseReach times as far.
	[[nodiscard]] inline float FarReach(bool a_outdoors) { return a_outdoors ? kWarmDistance * kSparseReach : kWarmDistance; }
	[[nodiscard]] inline std::size_t FewDoors(bool a_outdoors, int a_shownRooms)
	{
		return std::max(kFewDoors, static_cast<std::size_t>(a_outdoors ? a_shownRooms : 1));
	}

	// The interiors behind portal doors within reach of the last scan, each once.
	struct WarmTarget
	{
		std::uint32_t cell;
		float         distance;
		float         rank;
		std::uint32_t door;    // the nearest door into it, in the player's space
		std::uint32_t linked;  // its other end, in the cell
	};

	class WarmTargets
	{
	public:
		void Clear() { targets.clear(); }

		// A room with two doors: which room is kept goes by the better rank of the two, but the door
		// it is shown through is the nearer one. Taken by rank as well, the door changed as the player
		// turned on the spot, to one out of reach, and the room's open doorway in front of them went shut.
		void Note(const WarmTarget& a_door)
		{
			const auto room = std::ranges::find(targets, a_door.cell, &WarmTarget::cell);
			if (room == targets.end()) {
				targets.push_back(a_door);
				return;
			}
			room->rank = std::min(room->rank, a_door.rank);
			if (a_door.distance < room->distance) {
				room->distance = a_door.distance;
				room->door = a_door.door;
				room->linked = a_door.linked;
			}
		}
		// Best ranked first.
		void Sort() { std::ranges::sort(targets, {}, &WarmTarget::rank); }

		[[nodiscard]] const WarmTarget* Find(std::uint32_t a_cell) const
		{
			const auto it = std::ranges::find(targets, a_cell, &WarmTarget::cell);
			return it != targets.end() ? &*it : nullptr;
		}
		[[nodiscard]] std::size_t Count() const { return targets.size(); }

		// How far the nearest door into a_cell was at the last scan (beyond any reach if none was);
		// the same by Rank: which of two rooms is kept.
		[[nodiscard]] float DistanceOf(std::uint32_t a_cell) const
		{
			const auto* target = Find(a_cell);
			return target ? target->distance : FLT_MAX;
		}
		[[nodiscard]] float RankOf(std::uint32_t a_cell) const
		{
			const auto* target = Find(a_cell);
			return target ? target->rank : FLT_MAX;
		}

		// The interiors to keep warm: the nearest within a_reach, as many as there is room for
		// beside the shown space (a_shownCell, 0 if none), not that one.
		[[nodiscard]] std::vector<std::uint32_t> Wanted(float a_reach, std::uint32_t a_shownCell, int a_shownRooms) const
		{
			std::vector<std::uint32_t> wanted;
			for (const auto& target : targets) {
				if (std::cmp_greater_equal(wanted.size() + 1, a_shownRooms)) {
					break;
				}
				if (target.distance <= a_reach && a_shownCell != target.cell) {
					wanted.push_back(target.cell);
				}
			}
			return wanted;
		}

	private:
		std::vector<WarmTarget> targets;
	};

	// No room: the farthest kept room makes way, only for a door clearly nearer than its own.
	// Running along a street the order changes all the time; let go for every change, rooms
	// were attached and let go again a fifth of a second later.
	[[nodiscard]] inline bool MakesWayFor(float a_nextRank, float a_farthestRank)
	{
		return a_nextRank + kSwitchMargin < a_farthestRank;
	}

	// A cell that failed to attach isn't tried again for a few seconds.
	struct AttachRetry
	{
		void Failed(std::uint32_t a_cell)
		{
			cell = a_cell;
			cooldown = kRetryCooldown;
		}
		[[nodiscard]] bool Blocks(std::uint32_t a_cell) const { return a_cell == cell && cooldown > 0.0f; }
		void               Tick(float a_delta) { cooldown -= a_delta; }

		std::uint32_t cell = 0;
		float         cooldown = 0.0f;
	};
}
