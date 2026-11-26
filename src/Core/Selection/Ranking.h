#pragma once

#include <algorithm>

#include "Core/Math/Vec3.h"

// The distances, orders and hysteresis the tier scheduler (Graph/Tiers.h) ranks the doors around the
// player by, from plain measurements: how far a door counts as being, which of two is likelier to be
// walked through, and the margins that keep a choice from flapping.
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
	// Out to this reach a doorway holds a depth image (Graph::SchedulerConfig::depthReach). Its parallax
	// shows within a few doorway heights of the door; further out the image tier looks the same and
	// costs half as much.
	inline constexpr float kDepthReach = 1000.0f;
	// A doorway holds a picture (the image tier) out to this reach, kWarmDistance times kSparseReach.
	inline constexpr float kWarmDistance = 2000.0f;
	// A room kept loaded is let go only this much further out, so walking along that boundary
	// doesn't attach and let go of the same room over and over.
	inline constexpr float kWarmMargin = 400.0f;
	// Out to this many times kWarmDistance.
	inline constexpr float kSparseReach = 2.0f;
	// One attach at a time, a few times a second at most: each makes a long frame (3-10 ms, more
	// for a room whose 3D has to be loaded).
	inline constexpr float kWarmAttachSeconds = 0.5f;
	// A loaded space is held at least this long before it may be let go, and a space let go is not
	// attached again for kReattachSeconds: churn on a heavy space (its actors, physics, lights) is what
	// crashed the game; a ranking that flips is not a reason to do it.
	inline constexpr float kMinHoldSeconds = 8.0f;
	inline constexpr float kReattachSeconds = 4.0f;
	// The fastest the player can really move (a galloping horse, units per second). A step quicker than
	// this is a teleport (the harness's move_to, a crossing, fast travel), not a speed to lead the loads
	// by: counted as one, a short teleport towards a door made it look like running at it.
	inline constexpr float kMaxSpeed = 1200.0f;
	// A cell that failed to attach isn't tried again for this long.
	inline constexpr float kRetryCooldown = 5.0f;
	// A door someone used counts this much nearer for which space goes live: the door just opened wins over any
	// other open door in reach, a city gate standing open included (which counts a quarter of its distance).
	inline constexpr float kUsedBonus = 2000.0f;
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
}
