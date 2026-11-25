#pragma once

// A door standing open for a portal: its collision off, its panel hidden, its bodies out of the
// Havok world, and the "Open ..." prompt taken off it. Counted per door, so that two openers (the
// portal door of the space the player is in and the door they just came out of) can share one.
namespace Portal::Doors
{
	// What an opener asks of the door.
	//   hide: its panel not drawn (the leaves' shapes culled).
	//   passable: its bodies out of the Havok world as well. The layer change leaves a city gate
	//     solid, which is right only while there is nothing to walk through to.
	//   solid: its collision kept (a doorway shown open before the space behind it is there).
	struct OpenOptions
	{
		bool hide = false;
		bool passable = false;
		bool solid = false;
	};

	// One opener's share of a door held open; empty until Open.
	struct OpenState
	{
		RE::FormID door = 0;
		bool       hidden = false;
		bool       passable = false;
		bool       solid = false;
	};
	// The collision objects sit on child nodes of the door's 3D, so each one is switched.
	void Open(RE::TESObjectREFR* a_door, OpenState& a_state, OpenOptions a_options = {});
	void Close(OpenState& a_state);

	// Main thread: the door is held open by a portal right now.
	[[nodiscard]] bool IsOpen(RE::FormID a_door);
	// Main thread: the door is held open passable (Open), its collision out of the Havok world.
	[[nodiscard]] bool BodiesHidden(RE::FormID a_door);
	// Main thread, tests (Manager::SceneCheck): the door's shapes hidden that no opener hid.
	[[nodiscard]] std::uint32_t HiddenByNoOpener(RE::TESObjectREFR* a_door);

	// Once at startup: an open doorway shows no "Open ..." prompt, or "Close ..." when it can be
	// shut (Host::prompt).
	void InstallPromptHook();

	// Main thread, every frame: takes the "Open ..." prompt off the HUD while the crosshair is
	// on a door a portal holds open that can't be shut. Returns how often the engine asked for a
	// door's text.
	std::uint32_t HidePrompt();
}
