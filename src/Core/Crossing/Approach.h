#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "Core/Math/Vec3.h"

// Walking through a doorway: when the player is carried through, what the camera may do meanwhile,
// and the timers that keep a space from being attached again while a transition is under way.
namespace Core::Crossing
{
	// No second crossing for this long after one was asked for.
	inline constexpr float kCrossCooldown = 1.0f;
	// After a crossing was asked for, or the engine's own door trip begins, no attaching for this long.
	inline constexpr float kAttachHoldoff = 3.0f;

	class Timers
	{
	public:
		// Every frame.
		void Tick(float a_delta)
		{
			crossCooldown -= a_delta;
			attachHoldoff -= a_delta;
		}

		// A crossing is possible (not just after one).
		[[nodiscard]] bool Ready() const { return crossCooldown <= 0.0f; }
		// The spaces mustn't be attached now: a crossing or the engine's own door trip is under way.
		[[nodiscard]] bool AttachHeld() const { return attachHoldoff > 0.0f; }

		// No attaching for at least a_seconds.
		void HoldAttach(float a_seconds) { attachHoldoff = std::max(attachHoldoff, a_seconds); }
		// The space just left may be attached again after a_seconds.
		void EndHoldoff(float a_seconds) { attachHoldoff = a_seconds; }

		// A seamless crossing was asked for: no second one for a second, and no attaching until the
		// arrival has settled.
		void CrossingRequested()
		{
			crossCooldown = kCrossCooldown;
			attachHoldoff = kAttachHoldoff;
		}
		// The player takes a door the engine's way (its loading screen).
		void EngineTripBegins() { crossCooldown = kCrossCooldown; }
		// ... through the portal's own door: the engine gets the cells it leads into.
		void EngineTripThroughPortal() { attachHoldoff = kAttachHoldoff; }

	private:
		float crossCooldown = 0.0f;
		// After handing a cell to the engine's door transition, don't grab it back while the
		// transition is still pending.
		float attachHoldoff = 0.0f;
	};

	// The doorway as the approach sees it. `inward` is a horizontal unit vector pointing through the door.
	struct Doorway
	{
		Vec3  center;
		Vec3  inward;
		float halfWidth = 0.0f;
		float halfDepth = 0.0f;  // half the door's thickness: a city gate's is a step's worth
		float halfHeight = 0.0f;
	};

	// The feet at the doorway's own height: no lower than a step below the panel's bottom edge, no
	// higher than its middle. Everything that decides by where the player stands against a doorway
	// also checks this; horizontally, a flat roof over the door is inside its width and depth.
	[[nodiscard]] inline bool FeetAtDoorway(float a_feetZ, const Doorway& a_doorway)
	{
		const float bottom = a_doorway.center.z - a_doorway.halfHeight;
		return a_feetZ >= bottom - 64.0f && a_feetZ <= bottom + a_doorway.halfHeight;
	}

	// Where the doorway's picture lies, from the door's centre: behind the panel for a swinging door,
	// in front of it for a city gate, which keeps its picture in front of its panel.
	[[nodiscard]] inline float PictureDepth(float a_halfDepth, bool a_gate)
	{
		return a_gate ? -(a_halfDepth + 2.0f) : a_halfDepth + 2.0f;
	}
	// The first-person camera's stop: short of the picture by the camera's near distance (and a
	// unit beyond where walking in carries the player through, so that still happens).
	[[nodiscard]] inline float FirstPersonStop(float a_pictureDepth, float a_nearPlane) { return a_pictureDepth - a_nearPlane - 1.0f; }

	// How far in front of (negative) or behind the doorway's plane a point is, and how far to the
	// side of the doorway's middle.
	[[nodiscard]] inline float DepthOf(const Doorway& a_doorway, const Vec3& a_point)
	{
		const auto offset = a_point - a_doorway.center;
		return offset.x * a_doorway.inward.x + offset.y * a_doorway.inward.y;
	}
	[[nodiscard]] inline float SideOf(const Doorway& a_doorway, const Vec3& a_point)
	{
		const auto offset = a_point - a_doorway.center;
		return std::abs(offset.x * a_doorway.inward.y - offset.y * a_doorway.inward.x);
	}

	// A passage (what blocks the way in made non-collidable) is opened from kPassageOpenDepth in
	// front of the doorway's plane, and closed again once the player is further back than
	// kPassageCloseDepth.
	inline constexpr float kPassageOpenDepth = 25.0f;
	inline constexpr float kPassageCloseDepth = 45.0f;
	// When a wall closes the doorway from the player's side, the character controller keeps its
	// distance from it (about 28 units, a capsule's radius and skin): a wall 14 units in front of the
	// plane (Ysolda's House in Whiterun) stops the player 42 short of it, never within
	// kPassageOpenDepth, so the passage was never opened. With the player held by such a wall
	// (Approach::blockedAhead) it is opened from kBlockedReach, and closed again kBlockedCloseMargin
	// further back. A wall is looked for within kStandOff ahead of the player: the stand-off with room to spare.
	inline constexpr float kBlockedReach = 80.0f;
	inline constexpr float kBlockedCloseMargin = 15.0f;
	inline constexpr float kStandOff = 45.0f;

	// What is measured each frame the portal is ready.
	struct Approach
	{
		std::uint32_t door = 0;
		Doorway       doorway;
		Vec3          player;
		float         playerYaw = 0.0f;
		bool          playerMoving = false;
		Vec3          camera;
		float         delta = 0.0f;
		// Merged (outside, the room drawn in the world): the player walks into the doorway and the
		// crossing waits until the camera has followed them through it.
		bool merged = false;
		// A portal pass without a mergeable space on the other side (interior to interior): a camera
		// left on the far side of a portal can't be drawn well, so it is pulled in to reach the
		// doorway together with the player.
		bool  pullIn = false;
		bool  firstPerson = false;
		bool  gate = false;  // the doorway is a city gate
		float nearPlane = 0.0f;
		bool  passageOpen = false;  // the building behind the door has a passage made through it
		// Something solid stands within the character controller's stand-off ahead of the player,
		// in the way to the doorway: a wall closing it from this side, which keeps the player from
		// ever getting within the passage's usual reach (kBlockedReach).
		bool blockedAhead = false;
	};

	struct Decision
	{
		// False: the player isn't at the doorway (the other fields except closePassage mean nothing).
		bool active = false;
		// The building's collision back on: the player walked off, or isn't at the doorway's height.
		bool closePassage = false;
		// Look for what blocks the way in, and make it non-collidable.
		bool openPassage = false;
		// Carry the player through (if a crossing is possible).
		bool                 cross = false;
		bool                 limitCamera = false;
		float                cameraFloor = 0.0f;
		std::optional<float> cameraLimit;  // how far the camera may trail behind the player, if it is held
		float                depth = 0.0f;
		float                side = 0.0f;
		float                cameraDepth = 0.0f;
	};

	class ApproachState
	{
	public:
		[[nodiscard]] Decision Decide(const Approach& a_approach);

	private:
		float         passageCheck = 0.0f;
		float         passageCloseDepth = kPassageCloseDepth;  // how far in front of the doorway an open passage is closed again
		std::uint32_t lastDoor = 0;
		float         lastDepth = 0.0f;
	};
}
