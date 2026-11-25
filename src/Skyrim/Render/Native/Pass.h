#pragma once

// Which pass the engine is drawing, as the native backend needs to know it. Three questions that
// look alike and are not, and must not be merged: each is asked at a different place for a
// different reason, and answers differently for the same draw.
namespace Render::Native::Pass
{
	// The accumulator draws the portal's light scene (BSShaderManager::State slot kPortalSceneSlot).
	// Told apart by the scene rather than by Begin/EndPortalPass alone: a pass left unended (an
	// exception in between) must not cut the world. Decides which cut the mask plans, and that the
	// lighting swap lights the pass as the portal's.
	[[nodiscard]] bool InPortalScene();

	// The main view's own pass: the main depth target (not shadow maps or other targets), drawn with
	// the world's light scene. Only there is the doorway of a merged room cut.
	[[nodiscard]] bool InMainWorldPass();

	// A shape of a room merged into the world, drawn by the world's light scene (the player's own
	// shapes, which are not cut at the door, are not). The lighting swap gives it its room's light.
	[[nodiscard]] bool IsMergedRoomShape(const RE::BSGeometry* a_shape);
}
