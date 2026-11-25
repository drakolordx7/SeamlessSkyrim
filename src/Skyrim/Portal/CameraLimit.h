#pragma once

// Keeps the third-person camera from being on the other side of a portal doorway than the
// player. The camera trails ~170 units behind; walking through a door it would still be outside
// when the player is already inside, and after the teleport it would sit behind the inner door
// (in the wall, so camera collision snapped it to the player's head for a frame or two). Near a
// doorway the camera is pulled in towards the player instead, like camera collision with the
// door frame would, so both pass the doorway together.
namespace Portal::CameraLimit
{
	void Install();

	// Main thread, per frame before the camera update: the largest horizontal distance the
	// camera may have from the player this frame (nothing = no limit).
	void Set(std::optional<float> a_maxDistance);

	// After a seamless crossing: keep the camera at this offset from the player for a moment
	// (fading into the engine's own placement), instead of the engine's restart at the head.
	void Carry(const RE::NiPoint3& a_offsetFromPlayer, float a_seconds);

	// A portal's doorway (centre, horizontal unit normal away from the player's space): the camera
	// is kept in front of it. ClearPlane when there is none (or the space behind is merged).
	void SetPlane(const RE::NiPoint3& a_center, const RE::NiPoint3& a_inward, float a_halfWidth);
	// The first-person camera's stop at the same doorway: a_maxDepth along a_inward from a_center,
	// short of the doorway's picture by the camera's near distance. Cleared with ClearPlane.
	void SetFirstPersonStop(const RE::NiPoint3& a_center, const RE::NiPoint3& a_inward, float a_halfWidth, float a_maxDepth);
	void ClearPlane();

	// The camera's state is a RE::ThirdPersonState: on foot, on a horse or on a dragon.
	[[nodiscard]] bool InThirdPerson();

	// The camera is at the player's head: the player's body is not drawn this frame.
	[[nodiscard]] bool HidePlayer();
}
