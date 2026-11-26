#pragma once

// Doorway pictures taken before the player has been near, of spaces held out of sight (the ladder's
// far = picture, closer = depth picture, DESIGN.md). The scheduler asks Pictures for a doorway's
// pictures as it promotes the space behind it to T1 and T2 (Portal/Schedule DrivePictures); until now
// only the shown portal's doorway was ever photographed, so those requests stood unanswered for a far
// door and it drew what an earlier visit had left on disk. Here they are taken from the space itself:
//
//   T3   the space is attached and paused already (a kept space): its pictures are taken from there, a
//        capture pass a frame at a time (every other frame at most), then it is just kept again.
//   T1/2 a space with no picture and cheap to attach (an interior of few references) is attached for
//        the purpose (RemoteCell::AttachForPicture: nothing of it runs), photographed and let go. One
//        such attach at a time, and only with the frame time and memory the scheduler has to spare
//        (Core::Graph::NextAttach, NextShot); a frame without the headroom takes no capture.
//
// In the scheduler's own order, the likeliest door first (TierScheduler::RankOf). The pass is the portal
// pass of a capture for the shown portal, with the space's own light scene and cells
// (Render::PortalPass::DrawAhead); nothing of it is on screen, and the picture is kept like any other
// (Pictures::Capture, with its source "ahead" in the file).
//
// Vanilla behaviour, only the view changes: a space held for a picture runs no AI (its people are not
// raised to the high process), no Havok step, no animation graphs, and nothing of it is changed; what
// the engine does by itself when a cell's 3D loads (the references' own load events) it does as for
// any kept space. Main thread throughout (the update and the render hook).
namespace Portal::Ahead
{
	// Every frame after the scheduler's tick: lets go of an attach held for pictures that are taken (or
	// that would not come), looks at what is asked for and starts the next attach if there is room.
	void Update(RE::TESObjectCELL* a_here, float a_delta);

	// Render, after the shown portal's pass and before UpdateDoorwayPictures takes what was drawn: the
	// capture pass of the likeliest ready space, if the frame has the time. True if one was drawn.
	bool DrawPass();

	// A space the scheduler attaches now takes the place of one held for pictures.
	void LetGo(const RE::TESObjectCELL* a_cell, std::string_view a_why);
	// Everything held for pictures is let go (the scheduler let go of everything).
	void ReleaseAll(std::string_view a_reason);

	// The bridge's capture_ahead: the door's pictures asked for (both tiers; a_retake drops what it has
	// first) and taken as soon as its space can be, whatever the frame time; the door must be one of the
	// player's space's. Says what it did.
	[[nodiscard]] json Force(RE::FormID a_door, bool a_retake);

	[[nodiscard]] json Status();
}
