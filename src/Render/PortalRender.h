#pragma once

// Draws the active portal (Portal::Manager) every frame, inside the engine's own frame: right
// after the world is drawn, the remote cell is rendered from the main camera mirrored through the
// door pair (same cull -> submit sequence as the engine's menu-3D renderer, ID 52727), and that
// image is put into the scene at the source door, depth-tested, so the rest of the frame (first
// person, post-processing, UI) treats it like any other part of the world.
namespace Portal::Render
{
	// Once, at kDataLoaded (after Community Shaders installed its hooks at the same call sites).
	void Install();
	// Once, at kDataLoaded: the doorway's shaders compiled and its render targets made, behind the
	// startup screen. Made on first use, they took the first doorway of a session (~15 ms
	// compiling, more creating two screen-sized targets), right as the player walked through.
	void Prewarm();

	// Bridge thread: saves the raw portal image of the next rendered frame to a file.
	json RenderToFile(const std::string& a_path);

	// Diagnostics of the last frame a portal was drawn in.
	json LastFrame();
	json ActorCullStats();

	// Diagnostics: the outdoors' light as the last snapshot holds it and as worked out from the weather.
	json OutdoorLightingProbe();

	// Diagnostics: scale the portal's ambient / directional light, bypass the Community Shaders hook.
	json SetLightDebug(const json& a_settings);

	// Main thread: the actor counts as on screen this frame, whatever the engine's visibility test
	// (ID 40335) decided. That test fails every actor of a space shown behind a door.
	void ShowActor(RE::Actor* a_actor);

	// Rain and snow drawn in the portal pass of an outdoor space (a city gate's), on by default.
	[[nodiscard]] bool Precipitation();
	void               SetPrecipitation(bool a_shown);

	// Diagnostics: the doorway's picture tinted red, to tell whether a flash is in it.
	[[nodiscard]] bool TintDoorway();
	void               SetTintDoorway(bool a_tint);

	// Diagnostics (bridge: portal_switches): parts of the portal's work left out, to find which one
	// causes a whole-screen flash.
	struct DebugSwitches
	{
		bool skipPortalPass = false;        // no portal pass at all
		bool skipComposite = false;         // the pass drawn, the picture not put into the doorway
		bool skipMotion = false;            // no motion vectors for the doorway
		bool skipCommunityShaders = false;  // no hand-over of lighting, clip and IBL to Community Shaders
	};
	[[nodiscard]] DebugSwitches GetDebugSwitches();
	void                        SetDebugSwitches(const DebugSwitches& a_switches);

	// Render-merge: an interior's directional light and directional ambient as the patched Community
	// Shaders takes them (SeamlessPortal_SetMergedLighting), rotated by a_rotation (the interior's
	// placement in the world). Main thread.
	[[nodiscard]] std::optional<std::array<float, 19>> MergedLighting(RE::TESObjectCELL* a_cell, const RE::NiMatrix3& a_rotation);

	// The outdoors' distance fog for the street merged into an interior (the interior's fog,
	// a few thousand units deep and dark, turned the distant mountains black): near, far, power,
	// most it covers, near colour, far colour. From the weather, as WeatherLighting.
	[[nodiscard]] std::optional<std::array<float, 10>> MergedFog();
	// The outdoors' directional ambient for a surface facing a_normal (world), from the weather.
	[[nodiscard]] std::optional<RE::NiColor> OutdoorAmbient(const RE::NiPoint3& a_normal);
	// The same for a room merged into the street: the interior's own fog (its lighting, or its
	// lighting template's where it inherits), laid out as MergedFog.
	[[nodiscard]] std::optional<std::array<float, 10>> CellFog(RE::TESObjectCELL* a_cell);
}
