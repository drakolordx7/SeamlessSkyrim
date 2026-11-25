#pragma once

#include "Portal/Active.h"

#include <d3d11.h>

// Putting the portal into the world. After the world is drawn and before first person,
// post-processing and the UI: the doorway quad goes into the scene's own colour target,
// depth-tested against the scene, showing the portal image pixel for pixel. So the player, the
// porch and the UI cover it like any other part of the world, and it gets the same tone mapping
// as everything around it.
namespace Pictures::Composite
{
	// The quad's shaders and states made ahead.
	void Prewarm(ID3D11Device* a_device);

	// The portal's last picture (Render::PortalPass::ShownTargets) in a_portal's doorway, with
	// motion vectors for the upscaler. What went into the frame, for portal_status.
	json Draw(const Portal::Active& a_portal);
}
