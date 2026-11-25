#pragma once

#include "Skyrim/Pictures/DoorwaySnapshot.h"
#include "Skyrim/Portal/Active.h"

// Doorways before the space behind them is there (Portal::Snapshot): driven once a frame after the
// world and the portal's picture are drawn.
namespace Pictures
{
	// Render, as the portal pass is about to be drawn for a_portal: the camera of its doorway's picture
	// to draw it with this frame, if a picture is to be taken now (Portal::Snapshot::Capture asked for
	// one, or the door has none). a_forFile: a diagnostic file wants the player's view, so none.
	[[nodiscard]] std::optional<Portal::Snapshot::CaptureCamera> CaptureCameraFor(const Portal::Active& a_portal, bool a_forFile);

	// The doorway shown open on the real space is photographed, a door swung open before its space
	// is there gets its picture, and the scene's brightness is measured.
	// a_realShown: the real space is in the doorway this frame. A frame without it (the picture
	// not composited) was photographed with the void behind the door, and that picture was kept on
	// disk and shown on every slow load of the door after.
	void UpdateDoorwayPictures(bool a_realShown);
}
