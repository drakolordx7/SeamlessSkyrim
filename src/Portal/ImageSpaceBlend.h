#pragma once

#include "Portal/Active.h"

// An interior and the outdoors have different image spaces (tone mapping, brightness, tint); the
// engine switches when the player changes space. Through an open doorway that is a jump: the room
// seen from the porch is drawn with the outdoor one, and darkens at once on walking in. Instead
// the two are blended by where the camera is relative to the doorway, on both sides of the
// crossing, through the image space manager's override.
namespace Portal::ImageSpaceBlend
{
	// Main thread, every frame (and again after a crossing has placed the camera in the new space):
	// the blend for where the camera is against a_ready's doorway, none without one.
	void Update(const std::optional<Active>& a_ready);
	// The override taken off (the portal is off, or nothing is blended).
	void Clear();
	// The blend is the image space manager's override right now.
	[[nodiscard]] bool On();

	// The outdoors' image space as the engine last had it, or the current weather's for this part
	// of the day before the player has been outdoors in this session.
	[[nodiscard]] const RE::ImageSpaceBaseData* Outdoors();
}
