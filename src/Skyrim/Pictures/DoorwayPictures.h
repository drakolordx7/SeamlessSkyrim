#pragma once

#include "Skyrim/Pictures/DoorwaySnapshot.h"
#include "Skyrim/Portal/Active.h"

// Doorways before the space behind them is there (Pictures, DoorwaySnapshot.h): driven once a frame after the
// world and the portal's picture are drawn.
namespace Pictures
{
	// An interior never seen through its door: a dim grey, a little lighter than a dark room.
	inline constexpr RE::NiColor kPlainInside{ 0.08f, 0.08f, 0.08f };

	// How bright the day is outside, 0 at night .. 1 in full day, and the colour a street seen
	// through a doorway is drawn with before its picture exists.
	struct OutdoorLook
	{
		RE::NiColor plain;
		float       daylight;
	};
	[[nodiscard]] OutdoorLook OutdoorLookNow();

	// The doorway as its picture is asked for with (the door's quad, a gate's whole opening), seen
	// from a_viewer.
	[[nodiscard]] Pictures::Corners CaptureCorners(RE::FormID a_door, bool a_gate, const RE::NiPoint3& a_viewer);

	// Render, as the portal pass is about to be drawn for a_portal: the camera of its doorway's picture
	// to draw it with this frame, if a picture is to be taken now (Pictures::Capture asked for
	// one, or the door has none). a_forFile: a diagnostic file wants the player's view, so none.
	[[nodiscard]] std::optional<Pictures::CaptureCamera> CaptureCameraFor(const Portal::Active& a_portal, bool a_forFile);

	// The doorway shown open on the real space is photographed, a door swung open before its space
	// is there gets its picture, and the scene's brightness is measured.
	// a_realShown: the real space is in the doorway this frame. A frame without it (the picture
	// not composited) was photographed with the void behind the door, and that picture was kept on
	// disk and shown on every slow load of the door after.
	void UpdateDoorwayPictures(bool a_realShown);
}
