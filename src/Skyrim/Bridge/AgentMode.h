#pragma once

// The MCP server marks the games it launches with environment variables. Everything agent-related
// (the bridge, the Present hook for screenshots, forced bAlwaysActive, focus guards) only runs
// then: during normal play those hooks sat next to Community Shaders' frame-generation proxy and
// the image came out blurred/doubled even with portals off.
//   SEAMLESS_AGENT=1             launched by the MCP server
//   SEAMLESS_AGENT_BACKGROUND=1  ...and kept on another virtual desktop
//   SEAMLESS_AGENT_LOAD=<save>   ...and loads that save as soon as the main menu is up
//   SEAMLESS_AGENT_MUTE=1        ...and starts silent
//   SEAMLESS_TRAVEL_CAPTURE=<dir> ...and saves the first frames of every crossing and merge there
namespace AgentMode
{
	[[nodiscard]] bool Enabled();
	[[nodiscard]] bool Background();

	// When the game runs on another virtual desktop, Windows follows each activation of its window
	// to that desktop and drags the user along; these guards stop the game from activating its own
	// window. Must run before the game creates its window: call from SKSEPluginLoad.
	void InstallFocusGuards();

	// Agent mode: keys from the bridge reach the game (QueueKey); in background mode it also
	// ignores keyboard and mouse unless its window is in front.
	void InstallInputGuards();
	// Any thread: a key press (DirectInput scan code) handed to the game with its next keyboard
	// reads, down then up, as if typed: no window focus, no OS input.
	void QueueKey(std::uint32_t a_scanCode, int a_repeat);

	// SEAMLESS_AGENT_LOAD: the save is loaded the moment the main menu opens, from inside the game,
	// instead of by the MCP server polling for the menu and then asking over the bridge. Call at
	// kDataLoaded.
	void InstallAutoLoad();

	// Silences the game inside the process (XAudio2's mastering voice), not in the Windows volume
	// mixer: Windows keeps an application's mixer mute across runs, so a game that crashed or was
	// closed while muted left the user's own games silent. Call from SKSEPluginLoad, before the
	// game creates its audio.
	void InstallMute();
	void SetMuted(bool a_muted);
	[[nodiscard]] bool Muted();
}
