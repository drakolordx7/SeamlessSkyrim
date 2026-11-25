#pragma once

// What the SeamlessSkyrim fork of Community Shaders calls in this plugin (docs/COMMUNITY-SHADERS-PATCH.md),
// found by name in the DLL's export table. The other direction, what the plugin calls in the fork,
// is Render/Wire.h's SEAMLESS_FORK_EXPORTS. All defined in ForkExports.cpp.
extern "C"
{
	// Asked for every shape the fork draws while a space is merged in: it belongs to a merged room.
	// (A marker bit in the shape's flags was tried first: bit 30 is in use on many exterior statics.)
	__declspec(dllexport) bool SeamlessPortal_IsMergedShape(RE::BSGeometry* a_shape);

	// The same, with which of the rooms merged at once (its slot, as the doors and lights were sent
	// through Render::Backend), -1 for none.
	__declspec(dllexport) int SeamlessPortal_MergedRoomOf(RE::BSGeometry* a_shape);

	// The light scene a merged room's shapes are lit from: rooms merged at once stand in the same
	// place in their own coordinates, each has its own, and only one is registered with the engine.
	__declspec(dllexport) RE::ShadowSceneNode* SeamlessPortal_MergedRoomScene(int a_slot);

	// Shapes left out of the doorway mask while a space is merged in: the player's, so they aren't
	// cut at the door plane.
	__declspec(dllexport) bool SeamlessPortal_IsUnmaskedShape(RE::BSGeometry* a_shape);

	// For every shadow-casting light the fork gives an object drawn in a portal pass: how much of it
	// reaches the object (Portal::LightOcclusion), 0 blocked .. 1 in plain view. The fork scales the
	// light by it.
	__declspec(dllexport) float SeamlessPortal_LightVisibility(RE::BSGeometry* a_geometry, RE::NiLight* a_light);
}
