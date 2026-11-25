#pragma once

// Shadow-casting lights seen through a portal. The portal pass has no shadow maps, so such a light
// lit everything in its radius, through walls and ceilings: Lux interiors put strong lights above
// roofs and behind windows that rely on their shadows, and the room came out nearly white. This
// estimates per object how much of each shadow-casting light reaches it, with line-of-sight rays
// through the remote cell's Havok world, cached in that world's coordinates (static geometry and
// lights don't move, and the space keeps its pairs when it is drawn elsewhere or walked through).
//
// Used by the native lighting swap (Render/Native/NativeLighting.cpp).
namespace Portal::LightOcclusion
{
	// The room's door, in its Havok world.
	struct RoomDoor
	{
		RE::NiPoint3 center;    // middle of the panel
		RE::NiPoint3 intoRoom;  // horizontal unit vector
		float        halfWidth = 0;
		float        halfHeight = 0;
	};
	// Main thread, per frame: the world to cast rays in (nullptr = none). a_toPhysics maps where the
	// remote cell is drawn to where its Havok world has it: identity for the portal pass; for a room
	// merged into the world (its 3D moved to the door, its physics left in place) the door mapping.
	// a_interior: the remote space is an interior, with nothing around its shell. Set every frame.
	void SetWorld(RE::bhkWorld* a_world, const RE::NiTransform& a_toPhysics = {},
		const std::optional<RoomDoor>& a_door = std::nullopt, bool a_interior = false);

	// 0 = blocked, 1 = in plain view of the light (fraction of sample points that see it).
	[[nodiscard]] float Visibility(RE::BSGeometry* a_geometry, RE::NiLight* a_light);

	// Pairs drawn half lit so far this frame (since SetWorld), for want of rays: such a frame is
	// no picture to keep (the doorway picture's).
	[[nodiscard]] std::uint32_t UnmeasuredThisFrame();

	json Describe();
}
