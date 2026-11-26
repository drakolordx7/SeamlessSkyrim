#pragma once

#include "Core/Lighting/Daylight.h"
#include "Core/Render/Types.h"

#include <d3d11.h>

// The street's daylight coming into an interior through an open doorway, drawn by a pass of our own
// after the world (and the doorway's picture) is in the scene colour: a patch of sunlight where the sun's
// rays through the opening land, and a soft spill of the sky's light near it. It works from the scene's
// depth alone, so it needs nothing of the game's shaders: stock Community Shaders (any version) and
// ENB draw the scene as they do, and this brightens what they drew. It is for a room merged into the
// street (seen from outside, or walked into) and for the street merged into an interior (the
// patch on the interior's floor); nothing for doorways shown by a picture or the portal pass.
namespace Render::DoorwayDaylight
{
	struct State
	{
		Core::Daylight::Opening opening;
		Core::Vec3              towardSun;    // in the same coordinates as the opening
		Core::Render::Color     sun;          // as the main view is lit with it (the outdoors' sunlight scale in)
		Core::Render::Color     sky;          // the street's ambient light
		float                   open = 0.0f;  // how far the door stands open, 0 shut
	};

	// Main thread, every frame a space is merged (nullopt when none is).
	void Set(const std::optional<State>& a_state);

	// The shader and states made ahead.
	void Prewarm(ID3D11Device* a_device);

	// Render thread, after the world is drawn into the scene and before first person.
	// What it did, for portal_status (empty when nothing was drawn).
	json Draw();
}
