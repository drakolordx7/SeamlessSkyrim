#pragma once

// Doorways before the space behind them is there (Portal::Snapshot): driven once a frame after the
// world and the portal's picture are drawn.
namespace Pictures
{
	// The doorway shown open on the real space is photographed, a door swung open before its space
	// is there gets its picture, and the scene's brightness is measured.
	// a_realShown: the real space is in the doorway this frame. A frame without it (the picture
	// not composited) was photographed with the void behind the door, and that picture was kept on
	// disk and shown on every slow load of the door after.
	void UpdateDoorwayPictures(bool a_realShown);
}
