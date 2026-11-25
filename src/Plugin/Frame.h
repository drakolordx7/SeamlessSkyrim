#pragma once

// The per-frame driver: the PlayerCharacter::Update hook runs the portal manager and travel.
namespace Plugin::Frame
{
	// Installed when the portals are hooked at startup or this is an agent session (the bridge
	// steers the player from it); also installs the Present hook (Render::Present). Call at
	// kDataLoaded, after Portal::Manager::Install.
	void Install();

	// Runs right after the engine's own player update, before the portals' (the bridge's walk_to).
	using PreUpdate = void (*)(RE::PlayerCharacter* a_player, float a_delta);
	void SetPreUpdate(PreUpdate a_callback);

	// Diagnostics: frame times of the last 120 frames (average, median, 95th percentile, worst).
	json FrameStats();
}
