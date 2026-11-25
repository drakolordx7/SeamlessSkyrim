#pragma once

#include "Core/Crossing/Sequence.h"
#include "Core/Graph/Tiers.h"
#include "Skyrim/Spaces/RemoteCell.h"

// The tier scheduler (Core/Graph/Tiers.h) run on the game: every frame it is given where the player
// is, which doors are open and how much frame time there is, and what it changes is carried out
// with the machinery the portals already have.
//
//   T0 cold          nothing held
//   T1 / T2 picture  the doorway may show its picture (StandIns asks ShowsPicture); Pictures keeps
//                    one kind of picture per door today, so a depth picture is the same one
//   T3 loaded        the space is attached and paused (RemoteCell, a "kept" space), ready to be shown
//   T4 live          the manager's portal (PortalManager): shown, simulated, merged when close
//
// The kept spaces are carried out here, as events (a promotion to T3 attaches, a demotion lets go).
// The live space is level-triggered: LiveCandidate is the doorway of the space the scheduler holds
// live, and the manager brings it up (from the kept space if there is one) and takes it down.
namespace Portal::Schedule
{
	// The door to show: in the player's space, its other end, and the space behind.
	struct Candidate
	{
		RE::TESObjectREFR* door;
		RE::FormID         linkedDoor;
		RE::TESObjectCELL* cell;
		float              distance;
		float              rank;
	};

	// At kDataLoaded, after Graph::Install and the settings.
	void Install();

	// [Portals] iLoadedRooms, iHeldMemoryMB, iTargetFrameRate (the menu and the bridge change them for the session).
	[[nodiscard]] int  LoadedRooms();
	void               SetLoadedRooms(int a_count);
	[[nodiscard]] int  MemoryBudgetMb();
	void               SetMemoryBudgetMb(int a_mb);
	[[nodiscard]] int  TargetFrameRate();
	void               SetTargetFrameRate(int a_fps);

	// Main thread, every frame, after the heading is tracked: looks for new doors, ticks the scheduler
	// and carries out what it changed for the spaces kept loaded.
	void Update(RE::PlayerCharacter* a_player, RE::TESObjectCELL* a_here, float a_delta);

	// The doorway of the space the scheduler holds live, for the manager to show; nullopt when none.
	[[nodiscard]] const std::optional<Candidate>& LiveCandidate();
	// The scheduler's tier of the space behind a cell the manager shows or keeps.
	[[nodiscard]] Core::Graph::Tier TierOf(RE::TESObjectCELL* a_cell);
	// The open door the player is nearest to within the door's reach: the doorway a crossing is being
	// prepared for. Both ends' forms for the crossing sequence's log; none if no open door is near.
	struct Approached
	{
		Core::Crossing::Target target;
		RE::FormID             door;
		RE::FormID             linkedDoor;
	};
	[[nodiscard]] const std::optional<Approached>& ApproachedDoorway();

	// The doorway of a_door (in the player's space) may show its picture: its space is held at T1 or
	// more. A door the graph doesn't know yet may.
	[[nodiscard]] bool ShowsPicture(RE::FormID a_door);
	// Why a door shows no portal, or that it does (bridge door_status): the scheduler's tier for the
	// space behind it, its last decision, the door's state and the first thing that stops it.
	[[nodiscard]] json DescribeDoor(RE::FormID a_door);

	// The doorway keeps the engine's own behaviour for good (a prewarm that came too late, a crossing
	// that timed out): an ordinary door, with its loading screen.
	void               Fallback(RE::FormID a_door, std::string_view a_reason);
	[[nodiscard]] bool IsFallback(RE::FormID a_door);

	// The manager could not attach the space it was asked to show: nothing is held for it.
	void AttachFailed(RE::TESObjectCELL* a_cell);
	// Everything attached was let go (a save, a load, the player arrived somewhere, portals off): the
	// scheduler holds nothing again. Kept spaces are let go here too.
	void ReleaseAll(std::string_view a_reason);

	// The spaces kept loaded out of sight (T3).
	[[nodiscard]] std::vector<Spaces::RemoteCell>&          Kept();
	[[nodiscard]] std::vector<Spaces::RemoteCell>::iterator FindKept(const RE::TESObjectCELL* a_cell);
	// The kept space of a_cell moved into a_into (which is then shown by the caller); false if none.
	bool TakeKept(const RE::TESObjectCELL* a_cell, Spaces::RemoteCell& a_into);
	// A kept space let go (the player took a door into it the engine's way).
	void LetGoKept(const RE::TESObjectCELL* a_cell, std::string_view a_why);

	// The end of a frame's main-thread work, at Present: the frame time the headroom is measured from.
	void FrameEnded();

	[[nodiscard]] json Status();
}
