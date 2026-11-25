#pragma once

// Which doors stand open because someone used them. A seamless door doesn't open as the player
// comes near: the player opens it by using it (no loading screen), an NPC by reaching for it on
// its way through, and it swings shut again a few seconds later, once nobody stands in it. The
// manager decides from this, the menu's switches and the global setting what each door does
// (Doors::Wanted).
namespace Portal::DoorUse
{
	// [Portals] iForceDoors: every door open (locked ones too), or every door shut until used
	// (the per-door "always open" and the gates included).
	enum class Force
	{
		kNone = 0,
		kAllOpen = 1,
		kAllShut = 2,
	};
	[[nodiscard]] Force GetForce();
	void                SetForce(Force a_force);

	// How long a used door stays open after nobody stands in its doorway any more.
	inline constexpr float kOpenSeconds = 5.0f;

	// Main thread: the pair is opened (again for the full time if it was already). True when it
	// was shut.
	bool Use(RE::FormID a_door, RE::FormID a_linked);
	// Main thread: the pair is shut again (the player used the open door). True when it was open.
	bool Shut(RE::FormID a_door);
	// Main thread: either door of the pair a_door belongs to was used and is still open.
	[[nodiscard]] bool Used(RE::FormID a_door);
	// Main thread: the doors used, both of each pair.
	[[nodiscard]] std::vector<RE::FormID> UsedDoors();

	// Main thread: the door's own open or close sound (its base form's), from the door of the pair
	// in the player's space: the other one stands in a space drawn moved, or not at all, and was
	// heard from wherever its cell has it.
	void PlayDoorSound(RE::FormID a_door, RE::FormID a_linked, bool a_open);

	// Any thread (the AI's): an NPC reached for a_door on its way through.
	void NoteActorUse(RE::FormID a_door);
	// Main thread: the doors NPCs reached for since the last call.
	[[nodiscard]] std::vector<RE::FormID> TakeActorUses();

	// Main thread, every frame: counts down each used door; one with someone standing in its
	// doorway (the player or an NPC) stays open. Returns the pairs that swing shut now.
	std::vector<std::pair<RE::FormID, RE::FormID>> Update(float a_delta);

	// Main thread: the doors standing open right now (both of each pair), for the AI.
	void PublishOpen(std::vector<RE::FormID> a_doors);
	// Any thread: a_door stands open (PublishOpen): an NPC walks through without reaching for it.
	[[nodiscard]] bool StandsOpen(RE::FormID a_door);

	// Main thread: forget every use (portals switched off, a game loaded).
	void Clear();

	json Status();
}
