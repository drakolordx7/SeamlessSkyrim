#include "Core/Crossing/Approach.h"

#include <cmath>

namespace Core::Crossing
{
	namespace
	{
		// In portal mode the camera must still be in front of the portal quad (2 units in front of
		// the panel) when the crossing is carried out.
		constexpr float kCrossLead = 4.0f;
		constexpr float kPullInDistance = 400.0f;
		// Merged, the player behind the doorway is on the exterior's physics (no interior floor or
		// furniture there yet), so not for long, even with the camera turned to look back out.
		constexpr float kMergedMaxDepth = 250.0f;
		// Merged: behind the door plane the player walks on the exterior's ground inside the
		// building's shell (it sinks or rises there), so only a short way; the camera is eased in
		// on the last steps so that it is through the doorway by then, never closer than
		// kMergedCameraMin (the player stays in the picture).
		constexpr float kMergedCrossDepth = 60.0f;
		constexpr float kMergedCameraMin = 55.0f;
	}

	Decision ApproachState::Decide(const Approach& a)
	{
		Decision out;
		const auto& doorway = a.doorway;
		const float depth = DepthOf(doorway, a.player);  // < 0: in front
		const float side = SideOf(doorway, a.player);
		out.depth = depth;
		out.side = side;
		// Behind the door plane counts too: someone who got past it while a crossing wasn't
		// possible yet (the cooldown) is standing in the void behind the doorway and must still be
		// carried through. Walked off the passage sideways: its building's collision back on under them.
		if (depth < -kPullInDistance || depth > kMergedMaxDepth + 50.0f || side > doorway.halfWidth + 150.0f) {
			out.closePassage = a.passageOpen;
			return out;
		}
		// Only at the doorway's own height: a roof over the door is inside its width and depth too.
		if (!FeetAtDoorway(a.player.z, doorway)) {
			out.closePassage = a.passageOpen;
			return out;
		}
		out.active = true;
		out.cameraDepth = DepthOf(doorway, a.camera);

		// A passage is opened only right at the threshold: the building's collision is also the porch
		// and the steps in front of the door, and with it gone earlier the player sinks into the
		// ground on the way up. Whatever the mode: a door can stand against a wall that closes the
		// void behind it, and the player would stop short of the crossing. Only on the way in: a
		// player set down at the door's marker just in front of it would be held above the floor.
		// Facing in and moving, not getting nearer: someone already stopped by the wall gets no nearer.
		const bool walkingIn = a.playerMoving && std::sin(a.playerYaw) * doorway.inward.x + std::cos(a.playerYaw) * doorway.inward.y > 0.5f;
		// (Blocked and not yet opened, or opened because of it and not closed yet: the reach holds
		// while the passage is open, or it would be shut again as soon as it had taken the wall away.)
		const bool  blocked = a.blockedAhead || (a.passageOpen && passageCloseDepth > kPassageCloseDepth);
		const float reach = blocked ? kBlockedReach : kPassageOpenDepth;
		if (depth > -reach && side <= doorway.halfWidth + 20.0f && (walkingIn || a.passageOpen)) {
			// Seconds, not frames: counted per frame it looked again twice as often at 120 fps.
			if (!a.passageOpen || (passageCheck -= a.delta) <= 0.0f) {
				passageCheck = 1.0f / 6.0f;  // what the per-frame count came to at 60 fps
				out.openPassage = true;
				if (!a.passageOpen) {
					passageCloseDepth = blocked ? kBlockedReach + kBlockedCloseMargin : kPassageCloseDepth;
				}
			}
		} else if (a.passageOpen && depth < -passageCloseDepth) {
			out.closePassage = true;
		}

		const bool inDoorway = side <= doorway.halfWidth + 20.0f;
		// The portal quad lies on the door's face, half its thickness in front of the centre the
		// depth is measured from: a thick door (a city gate) is crossed that much earlier, or the
		// camera would be through the quad, looking at the gate's planks, before the player is.
		const float lead = kCrossLead + doorway.halfDepth;
		// A gate's arrival marker can be within that distance of it: the player, just arrived and
		// standing there, was carried straight back, every two seconds. Only someone walking
		// towards the doorway is carried through.
		const bool approaching = lastDoor == a.door && depth > lastDepth + 0.05f;
		lastDoor = a.door;
		lastDepth = depth;
		// In first person nothing holds the camera back from the doorway (the stop at its plane is
		// the third-person camera's), and it is at the head, a little ahead of the body and bobbing
		// with each step: it got to the doorway's picture before the player did, and from beyond it
		// the room's own view showed the door from its far side, flashing with every step close to
		// the door. The player is carried through as soon as the camera's near plane would reach the picture.
		bool cameraAtPicture = false;
		if (!a.merged && a.firstPerson) {
			const float pictureDepth = PictureDepth(doorway.halfDepth, a.gate);
			cameraAtPicture = out.cameraDepth + a.nearPlane + 2.0f >= pictureDepth && SideOf(doorway, a.camera) <= doorway.halfWidth + 20.0f && inDoorway && approaching;
		}
		out.cross = a.merged ? (depth >= 15.0f && out.cameraDepth >= 2.0f) || depth >= kMergedCrossDepth :
		                       (depth >= -lead && inDoorway && (approaching || depth >= 0.0f)) || cameraAtPicture;
		out.limitCamera = a.pullIn || a.merged;
		out.cameraFloor = a.merged ? kMergedCameraMin : 0.0f;
		if (a.merged) {
			out.cameraLimit = inDoorway || depth > 0.0f ? std::optional(kMergedCameraMin + std::max(0.0f, kMergedCrossDepth - depth)) : std::nullopt;
		} else if (a.pullIn) {
			out.cameraLimit = std::max(0.0f, -depth - lead);
		}
		return out;
	}
}
