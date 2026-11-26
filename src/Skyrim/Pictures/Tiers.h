#pragma once

#include "Skyrim/Pictures/DoorwaySnapshot.h"

// The picture tiers of a doorway, as a scheduler asks for them. The ladder for the space behind a
// door is far = the image tier, closer = the depth image tier, close and the door open = the
// real space; this is the first two. All of it is drawn in our own pass after the scene (depth
// tested against the world, the doorway's quad), so it needs no shader of the game's: stock
// Community Shaders (any version) and ENB are as they are.
//
//   image:       one picture from in front of the doorway at the size set (Settings
//                iDoorwayPictureHeight, 512 texels by default), sharp, ray-marched against its
//                stored depth. Per door; for a door to the outdoors per part of the day.
//   depth image: one to three views from just inside the doorway (Settings iDoorwayDepthLayers),
//                colour and linear depth, marched in parallax so walking past at an angle
//                shows what is behind furniture without smearing; where the views don't reach the
//                ray, the image tier draws that pixel. No city gates.
//
// Both are drawn as bright as the viewer's eye makes them (the scene meter against the picture's
// own brightness), and kept on disk by door (plugin and FormID), part of the day and load order
// hash, within the size budget (iDoorwayCacheMB).
//
// To have a tier captured: Request it, and have the doorway's portal pass drawn (the portal
// active for that door, Render::PortalPass); the capture takes one frame a picture (a layer),
// every other frame at most, and the doorway on screen keeps showing its last picture meanwhile.
// A door whose space is not the shown portal's is photographed from the space itself while the scheduler
// holds it (T3), or attached for it briefly (T1, T2): Portal/Ahead draws the pass, from PendingRequests.
// Poll Ready (or Wanted: false) for it; a request that is no longer wanted is dropped.
// Everything here is the render thread's (the main thread's, between frames), and additive to
// DoorwaySnapshot.h.
namespace Pictures
{
	enum class Tier : std::uint8_t
	{
		kImage,
		kDepthImage,
	};

	struct Doorway
	{
		RE::FormID door = 0;      // in the player's space (as Portal::StandIns::DoorwayFill::door)
		bool       outdoors = false;  // what is behind it is outdoors
		bool       gate = false;      // outdoors on both sides
	};

	// The tier exists for this doorway: a gate has no depth image, and the setting may turn it off.
	[[nodiscard]] bool Offered(const Doorway& a_doorway, Tier a_tier);

	// Drawable now (for the image tier of a door to the outdoors, as of another part of the day when
	// this one's was never taken). May read the file the first time; false for a tier not Offered.
	[[nodiscard]] bool Ready(const Doorway& a_doorway, Tier a_tier);

	// A capture would improve on what there is: Offered, and not Ready as taken for this part of the
	// day, at this size, with this many views, for this load order (or only taken from the player's
	// view, for the image tier).
	[[nodiscard]] bool Wanted(const Doorway& a_doorway, Tier a_tier);

	// Asks for the capture; a request for what is not Wanted is ignored. It stands until the tier
	// is complete (or Cancel).
	void Request(const Doorway& a_doorway, Tier a_tier);
	// The requests for a door dropped.
	void Cancel(RE::FormID a_door);
	// A capture of the tier is asked for and still to be taken.
	[[nodiscard]] bool Requested(const Doorway& a_doorway, Tier a_tier);

	// The captures asked for and still to be taken (not Wanted ones dropped), in the order they were
	// asked: the scheduler asks the likeliest space's doors first (Portal/Ahead takes them from here).
	struct Pending
	{
		Doorway doorway;
		Tier    tier;
	};
	[[nodiscard]] std::vector<Pending> PendingRequests();
	// Every picture of the door's, this part of the day, dropped from memory and disk, so that it is
	// Wanted again (the bridge's capture_ahead with retake).
	void Retake(RE::FormID a_door, bool a_outdoors);

	// Render, as the portal pass for a_portalDoor is about to be drawn: the camera of the next
	// capture requested for it, if any (Pictures::CaptureCameraFor uses this).
	[[nodiscard]] std::optional<CaptureCamera> NextRequestedCamera(RE::FormID a_portalDoor, const RE::NiPoint3& a_viewer);

	// Render, after the world is drawn into the main scene and before first person (as
	// UpdateDoorwayPictures does for the doors it shows): the doorway's tier into a_corners (the
	// doorway as seen from the viewer: Portal::Doors::ShownQuad), a_alpha 1 opaque. false when it is
	// not Ready, nothing drawn. View: SnapshotView.
	bool Draw(const Doorway& a_doorway, Tier a_tier, const View& a_view, const Corners& a_corners, float a_alpha = 1.0f);

	// The main view as the doorway is drawn with, this frame.
	[[nodiscard]] View SnapshotView();
}
