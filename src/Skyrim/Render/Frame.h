#pragma once

// Draws the active portal (Portal::Manager) every frame, inside the engine's own frame: right
// after the world is drawn, the remote cell is rendered from the main camera mirrored through the
// door pair (Render/PortalPass.h), and that image is put into the scene at the source door,
// depth-tested (Pictures/Composite.h), so the rest of the frame (first person, post-processing,
// UI) treats it like any other part of the world. The hooks on the engine's frame that do it.
namespace Render::Frame
{
	// Once, at kDataLoaded (after Community Shaders installed its hooks at the same call sites).
	void Install();
	// Once, at kDataLoaded: the doorway's shaders compiled and its render targets made, behind the
	// startup screen. Made on first use, they took the first doorway of a session (~15 ms
	// compiling, more creating two screen-sized targets), right as the player walked through.
	void Prewarm();

	// Main thread: the actor counts as on screen this frame, whatever the engine's visibility test
	// (ID 40335) decided. That test fails every actor of a space shown behind a door.
	void ShowActor(RE::Actor* a_actor);

	// Bridge thread: saves the raw portal image of the next rendered frame to a file.
	json RenderToFile(const std::string& a_path);

	// Diagnostics of the last frame a portal was drawn in.
	json LastFrame();
	json ActorCullStats();
}
