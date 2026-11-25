#pragma once

#include "Core/Selection/Ranking.h"
#include "Skyrim/Plugin/Settings.h"
#include "Skyrim/Portal/Active.h"
#include "Skyrim/Spaces/RemoteCell.h"

// Which load door is the portal, and which interiors behind other doors are kept loaded out of
// sight ("warm") so that their doorway opens at once when it becomes the nearest.
namespace Portal::Selection
{
	using Core::Selection::kLoadAhead;
	using Core::Selection::kReleaseBeyond;
	using Core::Selection::kSwitchMargin;
	using Core::Selection::kWarmDistance;
	inline constexpr int   kMaxShownRooms = Plugin::Settings::Of(Plugin::Settings::Id::kShownRooms).max;

	// The reach of a door ([Portals] iDoorOpenDistance) and the shown-rooms count ([Portals]
	// iShownRooms), from the settings.
	void Install();

	// How close (game units) the player comes before the space behind a door loads (kLoadAhead
	// further out) and the door can be walked through.
	[[nodiscard]] float DoorOpenDistance();
	void                SetDoorOpenDistance(float a_distance);
	// Outdoors, how many rooms behind doors are active at once, the nearest door's included
	// (1 = that one only): the others are the rooms behind the nearest other doors, kept loaded so
	// they open at once, and those whose door is open are drawn in it, merged beside the shown one,
	// as many as the render backend can mask (Render::Backend::Interface::MergedRooms: 1 on the game's own
	// shaders, where the others stay loaded out of sight).
	[[nodiscard]] int ShownRooms();
	void              SetShownRooms(int a_count);

	[[nodiscard]] float NearRadius();
	[[nodiscard]] float ActivateRadius();
	[[nodiscard]] float ReleaseRadius();

	struct Candidate
	{
		RE::TESObjectREFR* door;
		RE::FormID         linkedDoor;
		RE::TESObjectCELL* cell;
		float              distance;
		float              rank;  // see Rank
	};

	// The direction the player last moved in (horizontal, unit; zero until they have, in this
	// space): the door walked towards is the likelier one to be walked through. Every frame.
	void TrackHeading(const RE::NiPoint3& a_position);

	// a_door's distance from a_origin as the reaches and the order of doors count it.
	[[nodiscard]] float Reach(const RE::NiPoint3& a_origin, RE::TESObjectREFR* a_door, RE::TESObjectREFR* a_linked);
	// What doors are ordered by: the reach, a little less for a door ahead of the player and a
	// little more for one behind.
	[[nodiscard]] float Rank(const RE::NiPoint3& a_origin, RE::TESObjectREFR* a_door, RE::TESObjectREFR* a_linked);

	// The door at the other end of a_ref when a_ref would be a portal door, the menu's switch aside.
	[[nodiscard]] RE::NiPointer<RE::TESObjectREFR> PortalLink(RE::TESObjectREFR* a_ref, RE::TESObjectCELL* a_here);

	// The portal door to show, within ActivateRadius. Scans the references around the player a few
	// times a second and looks the last result up again in between; also sets what UpdateWarm keeps.
	[[nodiscard]] std::optional<Candidate> CandidateDoor(RE::PlayerCharacter* a_player, float a_delta);
	// The next CandidateDoor scans.
	void ScanNow();

	struct DoorPair
	{
		RE::FormID sourceDoor;  // in the player's space
		RE::FormID linkedDoor;
	};
	// Main thread: the pair of the doorway the player is at, for the menu: the portal's while its
	// door is the nearest, else the nearest door within a_radius. The portal's alone, the menu showed
	// a shop's other door (holding the portal from up to ReleaseRadius away) at a door switched off,
	// which then couldn't be switched back on.
	[[nodiscard]] std::optional<DoorPair> DoorPairAt(float a_radius);

	// The interiors behind portal doors within reach of the last scan, best ranked first, each once
	// (several doors can lead into one inn). Only collected outdoors, while spaces are kept warm.
	using WarmTarget = Core::Selection::WarmTarget;
	[[nodiscard]] const WarmTarget* TargetOf(RE::FormID a_cell);
	[[nodiscard]] std::size_t       TargetCount();
	// Few doors around the player (the last scan): nothing competes for what is loaded, so rooms
	// are kept from further out (WarmReach).
	[[nodiscard]] bool  Sparse();
	[[nodiscard]] float WarmReach();

	using AttachRetry = Core::Selection::AttachRetry;

	struct WarmContext
	{
		AttachRetry& retry;
		bool         attachHeld;     // Crossing: not now, a transition is pending
		bool         shownLoading;   // the shown portal's space isn't loaded yet: it loads first
	};
	// Main thread, every frame: keeps the rooms behind the nearest other doors attached out of
	// sight, lets go of those out of reach.
	void UpdateWarm(RE::TESObjectCELL* a_here, float a_delta, WarmContext a_context);

	// The spaces kept attached out of sight; those whose door is open are also merged beside the
	// shown room (MergedView).
	[[nodiscard]] std::vector<RemoteCell>&          Kept();
	[[nodiscard]] RemoteCell*                       KeptSpace(const RE::TESObjectCELL* a_cell);
	[[nodiscard]] std::vector<RemoteCell>::iterator FindKept(const RE::TESObjectCELL* a_cell);
	// A kept space let go (its room merged beside the shown one is ended first).
	std::vector<RemoteCell>::iterator LetGoKept(std::vector<RemoteCell>::iterator a_space, std::string_view a_why);
	void                              ReleaseWarm(std::string_view a_reason);
}
