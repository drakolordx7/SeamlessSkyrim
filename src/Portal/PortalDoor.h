#pragma once

// The shown portal's door while its space is drawn by the portal pass (not merged): it stands open
// while the portal is shown, so the player can walk into the doorway. Merged, the room's own door
// is the merged room's (MergedView).
namespace Portal::PortalDoor
{
	// The door swings towards open (else towards shut): Doors::Wanted, set by the manager every
	// frame.
	void               SetWanted(bool a_wanted);
	[[nodiscard]] bool Wanted();

	// Every frame while the portal is ready: swings the door and holds it open. A door swings open
	// into the space it opens into, or the linked door seen through it swings in the portal's
	// picture; a_inward: from the source door into the space behind.
	void Update(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_inward, float a_delta);
	// The portal ended: the door is let go where it is, for the next owner to take over.
	void Close();

	// The portal's door is the one held (a_door is the shown portal's source door).
	[[nodiscard]] bool Holds(RE::FormID a_door);
	// The door of the portal swings (a stand-in must not take it) and how far it is open.
	[[nodiscard]] bool  Shut();
	[[nodiscard]] float Amount();
	// The door that is swung: the source door, or the linked door seen through it; 0 when none.
	[[nodiscard]] RE::FormID Swung();
}
