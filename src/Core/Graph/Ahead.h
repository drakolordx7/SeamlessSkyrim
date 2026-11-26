#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "Core/Graph/SpaceGraph.h"

// Doorway pictures taken before the player has ever been near: of a space the scheduler holds loaded
// and paused (T3) from where it is, and of a space only a picture is asked for (T1, T2) by attaching
// it briefly for that. Both are the same capture the doorway's pictures are taken with, a frame at a
// time and only with frame time to spare; the likeliest door first, in the scheduler's own order.
// What the adapter does with the answer (the attach, the capture pass) is Portal/Ahead.
namespace Core::Graph
{
	struct AheadConfig
	{
		// Frame time one capture pass takes: the room drawn from the doorway's camera into the
		// picture's targets, and its compression.
		float shotMs = 3.0f;
		// The attach of a space held only for its pictures (the same call as a kept space's).
		float attachMs = 1.5f;
		// Left over after either.
		float reserveMs = 1.0f;
		// A space is only attached for its pictures with this much memory to spare beyond what is held.
		float attachMb = 120.0f;
	};

	// A space some doorway of the player's asks a picture of.
	struct AheadSpace
	{
		float rank = 0.0f;         // TierScheduler::RankOf: lower is likelier
		bool  ready = false;       // attached, loaded and paused: a picture can be taken now
		bool  attachable = false;  // not attached, and cheap enough to attach for its pictures
	};

	// The space to take a picture of this frame: the likeliest that is ready, when the frame has the
	// time for a capture (a_headroomMs as Inputs::headroomMs). An index into a_spaces.
	[[nodiscard]] std::optional<std::size_t> NextShot(std::span<const AheadSpace> a_spaces, float a_headroomMs, const AheadConfig& a_config);

	// The space to attach for its pictures: one at a time (not while a_attachInFlight), the likeliest
	// that is attachable, when the frame has the time for the attach and a capture after it and the
	// memory has room (a_freeMb: the budget less what is held).
	[[nodiscard]] std::optional<std::size_t> NextAttach(std::span<const AheadSpace> a_spaces, bool a_attachInFlight, float a_headroomMs, float a_freeMb,
		const AheadConfig& a_config);
}
