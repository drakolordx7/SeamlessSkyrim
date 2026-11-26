#pragma once

#include "Core/Math/Vec3.h"

// How a room's sound reaches a listener standing outside its open doorway: the whole room is
// heard from one point, the doorway, as loud as the door lets it through. Pure maths on plain
// floats; the adapter (Skyrim/Audio) plays the sounds and sets what this returns.
//
// The rules (.weaver/DESIGN.md): a room is heard only while its door is open and it is the live
// room; a door that shuts, or a room let go, fades out, it never cuts.
namespace Core::Audio
{
	// The doorway in the listener's space: its middle and the horizontal unit vector pointing
	// through it, from the listener's side into the room.
	struct Doorway
	{
		Vec3  center;
		Vec3  inward;
		float halfWidth = 50.0f;
		float halfHeight = 100.0f;
	};

	struct Tuning
	{
		// Game units. The room is heard at half strength from refDistance (a doorway's worth of
		// air); it can't be heard from further than maxDistance, and fades out over the last
		// quarter of it.
		float refDistance = 350.0f;
		float maxDistance = 2600.0f;
		// A sound's own distance from the doorway inside the room: half strength at this.
		float roomRefDistance = 600.0f;
		// How much of the sound survives at the doorway's side (the listener 90 degrees off its
		// axis, still seeing a sliver of it), against standing in front.
		float sideFloor = 0.1f;
		// The most the muffling takes off (a doorway seen from afar and from the side).
		float muffleDepth = 0.5f;
		// A door open less than this lets nothing through.
		float minOpen = 0.02f;
		// The listener this far past the door plane is in the room: the engine's own sound there.
		float sideBlend = 60.0f;
		float fadeInSeconds = 0.6f;
		float fadeOutSeconds = 0.8f;
	};

	struct Heard
	{
		float gain = 0.0f;      // 0..1, everything below, muffling included
		float muffle = 0.0f;    // 0 clear .. 1 as muffled as it gets: distance, angle, a door barely open
		float distance = 0.0f;  // listener to doorway centre
		float cosine = 0.0f;    // of the angle off the doorway's axis, 1 head-on
	};

	// a_open: how far the door stands open, 0 shut .. 1. Zero gain for a listener in the room (past
	// the door plane), a shut door and a doorway out of reach.
	[[nodiscard]] Heard ThroughDoorway(const Vec3& a_listener, const Doorway& a_doorway, float a_open, const Tuning& a_tuning = {});

	// How much of a sound inside the room reaches its doorway, by its distance from it: 1 at the
	// doorway, half at roomRefDistance, falling off as 1 / (1 + (d / ref)^2).
	[[nodiscard]] float RoomFalloff(float a_distance, const Tuning& a_tuning = {});

	// A level that moves towards a target at a set speed: up over fadeInSeconds, down over
	// fadeOutSeconds, from full to nothing. Linear, so a door that shuts takes the room's sound
	// with it over the time it takes, not at once.
	struct Fade
	{
		float level = 0.0f;

		float Step(float a_target, float a_delta, const Tuning& a_tuning = {})
		{
			const float up = a_delta / a_tuning.fadeInSeconds;
			const float down = a_delta / a_tuning.fadeOutSeconds;
			level = a_target > level ? std::min(level + up, a_target) : std::max(level - down, a_target);
			return level;
		}
	};
}
