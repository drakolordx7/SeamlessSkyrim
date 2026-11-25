#include "Skyrim/Portal/PortalManager.h"

#include "Skyrim/Diag/Probe.h"
#include "Skyrim/Diag/Telemetry.h"
#include "Skyrim/Doors/DoorUse.h"
#include "Skyrim/Doors/Geometry.h"
#include "Skyrim/Doors/Hold.h"
#include "Skyrim/Doors/Host.h"
#include "Skyrim/Doors/Pair.h"
#include "Skyrim/Doors/Policy.h"
#include "Skyrim/Doors/Settings.h"
#include "Skyrim/Doors/Swing.h"
#include "Skyrim/Engine/Paths.h"
#include "Skyrim/Pictures/DoorwaySnapshot.h"
#include "Skyrim/Plugin/Settings.h"
#include "Skyrim/Portal/CameraLimit.h"
#include "Skyrim/Portal/Crossing.h"
#include "Skyrim/Portal/EngineHooks.h"
#include "Skyrim/Portal/Gates.h"
#include "Skyrim/Portal/ImageSpaceBlend.h"
#include "Skyrim/Portal/MergedCull.h"
#include "Skyrim/Portal/MergedView.h"
#include "Skyrim/Portal/NpcTravel.h"
#include "Skyrim/Portal/PortalDoor.h"
#include "Skyrim/Portal/Selection.h"
#include "Skyrim/Portal/StandIns.h"
#include "Skyrim/Portal/StreetLOD.h"
#include "Skyrim/Portal/StreetMerge.h"
#include "Skyrim/Portal/Travel.h"
#include "Skyrim/Render/Backend.h"
#include "Skyrim/Render/Frame.h"
#include "Skyrim/Spaces/RemoteCell.h"
#include "Skyrim/Spaces/Tuning.h"

namespace Portal::Manager
{
	namespace
	{
		bool                  enabled = false;
		bool                  hooksInstalled = false;  // see Install
		RemoteCell            remote;                  // the space shown behind the portal's door
		std::mutex            activeLock;
		std::optional<Active> active;
		Selection::AttachRetry retry;
		std::string           lastEvent = "none";
		std::optional<float>  approachLimit;  // camera pull-in this frame, see Crossing::Approach

		// Far enough past ReleaseRadius (fast travel on horseback), a door still swinging is let go
		// anyway. Past it, by reach (Reach), as ReleaseRadius is: a fixed distance stood inside a gate's
		// release reach and a sparse street's, and those were let go at once, swinging or not.
		constexpr float kForceReleaseBeyond = 1350.0f;

		void SetActive(std::optional<Active> a_active)
		{
			std::scoped_lock lock(activeLock);
			active = a_active;
		}

		// The doors NPCs reached for (NpcTravel's door idle, on the AI's threads) open: a seamless
		// door, not a marker, its far end known.
		void NoteActorUses()
		{
			for (const auto id : Portal::DoorUse::TakeActorUses()) {
				auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				auto* linked = Portal::Doors::LinkedOf(door);
				auto* base = door ? door->GetBaseObject() : nullptr;
				const bool locked = door && Portal::Doors::LockedShut(door);
				if (!linked || !base || static_cast<RE::TESObject*>(base)->IsMarker() || locked || Doors::PortalOff(id) || Doors::PortalOff(linked->GetFormID())) {
					continue;
				}
				if (Portal::DoorUse::Use(id, linked->GetFormID())) {
					Portal::DoorUse::PlayDoorSound(id, linked->GetFormID(), true);
				}
			}
		}

		// Both doors of every pair standing (nearly) open, for the AI: an NPC walks through those
		// without reaching for the door.
		void PublishOpenDoors()
		{
			constexpr float         kOpen = 0.9f;
			std::vector<RE::FormID> doors;
			const auto              add = [&](RE::FormID a_door, RE::FormID a_linked) {
				doors.push_back(a_door);
				doors.push_back(a_linked);
			};
			if (const auto portal = CurrentPortal()) {
				if (remote.Merged() ? MergedView::ShownAmount() >= kOpen : PortalDoor::Holds(portal->sourceDoor) && PortalDoor::Amount() >= kOpen) {
					add(portal->sourceDoor, portal->linkedDoor);
				}
			}
			for (const auto& [door, linked] : MergedView::OpenBesideDoors(kOpen)) {
				add(door, linked);
			}
			for (const auto& [id, standIn] : StandIns::Doorways()) {
				if (!standIn.handedOver && standIn.swing.Amount() >= kOpen) {
					add(id, standIn.linked);
				}
			}
			Portal::DoorUse::PublishOpen(std::move(doors));
		}

		void ReleaseShown(std::string_view a_reason, float a_holdoff = 0.0f, bool a_handOver = false)
		{
			Crossing::HoldAttach(a_holdoff);
			if (!remote.Attached() && !CurrentPortal()) {
				return;
			}
			lastEvent = std::format("released: {}", a_reason);
			const auto started = std::chrono::steady_clock::now();
			struct Timing
			{
				std::chrono::steady_clock::time_point started;
				~Timing() { logger::info("portal: release took {:.1f} ms", std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count()); }
			} timing{ started };
			logger::info("portal: {}", lastEvent);
			PortalDoor::Close();
			Crossing::ClosePassage();
			MergedView::EndShown();
			StreetLOD::Hide();
			SetActive(std::nullopt);
			MergedView::PublishShownCells(false);
			remote.Release(a_handOver);
			MergedView::Publish();
		}

		// The shown room, merged, left for a nearer door: kept and drawn on beside the next one
		// instead of let go. False when it can't be.
		bool Demote(RE::TESObjectCELL* a_here, const Active& a_portal)
		{
			const int slot = MergedView::ReserveBesideSlot(a_here, a_portal);
			if (!slot) {
				return false;
			}
			PortalDoor::Close();
			Crossing::ClosePassage();
			SetActive(std::nullopt);
			MergedView::MoveShownBeside(slot, a_portal);
			lastEvent = std::format("{} goes on beside the nearer door", Probe::DescribeCell(a_portal.cell));
			logger::info("portal: {}", lastEvent);
			return true;
		}

		// The portal let go when the player walked away, a closer door took over, the door was locked
		// or the portal switched off; or only its door pair changed.
		void EndOrSwitchPortal(RE::PlayerCharacter* a_player, RE::TESObjectCELL* a_here, const std::optional<Selection::Candidate>& a_candidate)
		{
			const auto portal = CurrentPortal();
			if (!portal) {
				return;
			}
			auto*       door = RE::TESForm::LookupByID<RE::TESObjectREFR>(portal->sourceDoor);
			auto*       linkedDoor = RE::TESForm::LookupByID<RE::TESObjectREFR>(portal->linkedDoor);
			const float reach = door && linkedDoor ? Selection::Reach(a_player->GetPosition(), door, linkedDoor) : FLT_MAX;
			// Another door into the same space counts too (an inn's kitchen door next to its front
			// door): the space is placed by the door pair, so it has to be the nearest door's.
			const bool  better = a_candidate && (a_candidate->cell != portal->cell || a_candidate->door->GetFormID() != portal->sourceDoor) &&
			                     a_candidate->rank + Selection::kSwitchMargin < (door && linkedDoor ? Selection::Rank(a_player->GetPosition(), door, linkedDoor) : FLT_MAX);
			// A shop closing for the night locks its door.
			const bool  off = Portal::Doors::PortalOff(portal->sourceDoor) || Portal::Doors::PortalOff(portal->linkedDoor);
			const bool  locked = door && Portal::Doors::LockedShut(door);
			const char* end = reach > Selection::ReleaseRadius() ? "player walked away" : better ? "a closer door" : locked ? "door locked" :
			                  off                                ? "portal switched off for this door" :
			                                                       nullptr;
			// A door a stand-in will hold open is let go open, to be taken over where it is: swung shut
			// first, it swung open again right after.
			const bool keepOpen = door && linkedDoor && !off && !locked && Portal::Doors::Wanted(door, linkedDoor);
			PortalDoor::SetWanted(keepOpen);
			if (end && (DoorShut() || keepOpen || reach > Selection::ReleaseRadius() + kForceReleaseBeyond)) {
				if (better && a_candidate->cell == portal->cell && !remote.Merged()) {
					// The other door leads into the same space, which is attached already: only the
					// door pair changes. Let go and attached again, the doorway was empty for a third
					// of a second, every time the player walked between a shop's two doors.
					PortalDoor::Close();
					SetActive(Active{ a_candidate->door->GetFormID(), a_candidate->linkedDoor, portal->cell, portal->generation });
					PortalDoor::SetWanted(false);  // the other door's, until the next frame decides for this one
					// The street's distant land is placed by the door pair, as at the attach: kept, it
					// stood where the first door's pair put it, off the street seen through this one.
					StreetMerge::PlaceLOD(*CurrentPortal(), a_here);
					lastEvent = std::format("switched to door {:08X} into the same space", a_candidate->door->GetFormID());
					logger::info("portal: {}", lastEvent);
				} else if (!(better && Demote(a_here, *portal))) {
					ReleaseShown(end);
				}
			}
		}

		// The candidate door's space attached (or taken from the kept ones) and shown.
		void AttachCandidate(RE::TESObjectCELL* a_here, const std::optional<Selection::Candidate>& a_candidate)
		{
			const auto attachStarted = std::chrono::steady_clock::now();
			bool       attached = false;
			if (const auto kept = Selection::FindKept(a_candidate->cell); kept != Selection::Kept().end()) {
				// Attached out of sight already (Selection::UpdateWarm): only its light scene becomes the drawn one.
				// Merged beside the shown one already, it is drawn on as it was.
				MergedView::TakeBeside(a_candidate->cell);
				std::swap(remote, *kept);
				Selection::Kept().erase(kept);
				remote.SetParked(false);
				remote.SetPaused(false);
				remote.Show();
				attached = true;
				logger::info("portal: {} was kept behind its door{}", Probe::DescribeCell(a_candidate->cell), remote.FullyLoaded() ? ", loaded" : ", still loading");
			} else {
				const Diag::Scope scope("attach");
				attached = remote.Attach(a_candidate->cell, a_here->IsInteriorCell() && MergedView::Mergeable(a_here, a_candidate->cell));
			}
			logger::info("portal: attach took {:.1f} ms", std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - attachStarted).count());
			if (attached) {
				SetActive(Active{ a_candidate->door->GetFormID(), a_candidate->linkedDoor, a_candidate->cell, remote.Generation() });
				// Decided for this door from the next frame on; kept from the last portal's (an inn's
				// door standing open), a shut door swung open for a moment as its space came.
				PortalDoor::SetWanted(false);
				lastEvent = std::format("activated door {:08X} -> {}", a_candidate->door->GetFormID(), Probe::DescribeCell(a_candidate->cell));
				// Loaded behind a door, the engine faded the space's references in as they came:
				// the street opened with its market stalls missing for a fifth of a second.
				for (auto* cell : remote.Cells()) {
					NpcTravel::ShowAtOnce(cell, MergedView::kShowAtOnceSeconds);
				}
				// Indoors, the street's distant LOD is placed for it from now on, not from the merge:
				// the engine's own update at the crossing, centred on the interior, showed the low LOD
				// town and land in the street's houses for the half second the portal's picture was
				// drawn before the merge.
				StreetMerge::PlaceLOD(*CurrentPortal(), a_here);
			} else {
				retry.Failed(a_candidate->cell->GetFormID());
				lastEvent = std::format("could not attach {}", Probe::DescribeCell(a_candidate->cell));
			}
			logger::info("portal: {}", lastEvent);
		}

		// The space behind the door merged into the world: the room (or the street, into an interior)
		// is drawn by the main pass from now on.
		void MergeShown(const Active& a_portal, RE::PlayerCharacter* a_player, RE::TESObjectCELL* a_here)
		{
			PortalDoor::Close();
			try {
				Diag::CaptureEventFrames(L"merge", 40);
				const auto mergeStarted = std::chrono::steady_clock::now();
				const auto placement = MergedView::PlacementOf(a_portal, Doors::Compute(a_portal));
				const auto toSource = placement.toSource;
				const Diag::Scope scope("merge");
				const auto        result = remote.Merge(placement);
				if (StreetMerge::Merged()) {
					StreetMerge::Show(toSource, RE::TESForm::LookupByID<RE::TESObjectREFR>(a_portal.sourceDoor));
				}
				// The room's lights, still in the engine's scene from when it was the player's
				// space (walked out of it just now), into the portal's before the first frame:
				// adopted only in the next Update, the room was drawn once without them.
				remote.Update(0.0f);
				// The merged space's doors that stand open, open in the merge's own frame: the
				// stand-ins ran before it this frame, when those doors weren't in view yet.
				StandIns::Update(a_player, a_here, 0.0f);
				logger::info("portal: merge took {:.1f} ms", std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - mergeStarted).count());
				logger::info("portal: merged {} into the world ({})", Probe::DescribeCell(a_portal.cell), result.dump());
			} catch (const std::exception& e) {
				logger::warn("portal: merge failed: {}", e.what());
			}
		}

		// The shown portal's space, every frame: loaded, its door swung, merged when it is time, drawn
		// merged, and the crossing watched.
		void UpdateShown(RE::PlayerCharacter* a_player, RE::TESObjectCELL* a_here, float a_delta, std::vector<MergedCull::Room>& a_culls)
		{
			const auto portal = CurrentPortal();
			if (!portal) {
				return;
			}
			remote.Update(a_delta);
			// Also while the door is still swinging shut after the portal stopped being ready.
			if (const auto mapping = Doors::TryCompute(*portal); mapping && (ActivePortal() || (PortalDoor::Holds(portal->sourceDoor) && !PortalDoor::Shut()))) {
				PortalDoor::Update(RE::TESForm::LookupByID<RE::TESObjectREFR>(portal->sourceDoor), mapping->inward, a_delta);
			}
			// Loaded from further out than usual (farActivate), the space is only merged where it would
			// have been loaded: the street merged into an interior is drawn whole, seen or not (it
			// casts the sun's shadows, MergedCull), 2.6 ms a frame in Honningbrew Meadery's cellar.
			auto*      shownDoor = RE::TESForm::LookupByID<RE::TESObjectREFR>(portal->sourceDoor);
			const bool closeBy = shownDoor && a_player->GetPosition().GetDistance(shownDoor->GetPosition()) <= Selection::NearRadius();
			const bool mergeable = !remote.Merged() && closeBy && remote.Healthy() && remote.FullyLoaded() && MergedView::Mergeable(a_here, portal->cell);
			const bool waiting = StandIns::WaitForPicture(*portal, a_here, mergeable);
			if (mergeable && !waiting) {
				MergeShown(*portal, a_player, a_here);
			}
			if (remote.Merged()) {
				MergedView::Publish();
				MergedView::UpdateShown(*portal, PortalDoor::Wanted(), a_delta, a_culls);
			}
			approachLimit = Crossing::Approach(a_player, *portal, a_delta);
		}
	}

	void Install()
	{
		Portal::Doors::SetHost({ .prompt = Prompt, .hasPicture = Portal::Snapshot::HasPicture, .spaceShown = SpaceShown, .portalLink = Selection::PortalLink });
		MergedView::Install();
		using Plugin::Settings::Id;
		using Plugin::Settings::Bool;
		using Plugin::Settings::Int;
		enabled = Bool(Id::kEnabled);
		Selection::Install();
		Portal::DoorUse::SetForce(static_cast<Portal::DoorUse::Force>(Int(Id::kForceDoors)));
		Portal::Snapshot::SetPictureHeight(Int(Id::kDoorwayPictureHeight));
		MergedCull::SetMode(static_cast<MergedCull::Mode>(Int(Id::kMergedCull)));
		// Precision patches the engine's world step loop (ID 77851 + 0x407) with a pre-step hook
		// that takes its own locks for whichever world is stepped; stepping a second world in the
		// step job deadlocked the game in that hook (main thread waiting for the job, ID 35409).
		// Without stepping, NPCs behind the door don't move physically until the player is there.
		// See Settings kStepRemoteHavok for the modes; the default is 2 with Precision loaded
		// (see RemoteCell::StepAtEndOfFrame), else 1.
		const bool stepRemote = Int(Id::kStepRemoteHavok) != 0;
		Spaces::LoadTuning();
		// The hooks below patch engine code in the draw, cull, physics and travel paths. With
		// portals off they did nothing yet were all there, one more thing to clash with other
		// mods' hooks (as with Precision's in the physics step). They can't be put in later when
		// portals are switched on: the physics step's call site runs on job threads, and code
		// can't be patched safely under them. Agent sessions get them always (portals_enable).
		hooksInstalled = enabled || Diag::AgentMode();
		if (!hooksInstalled) {
			logger::info("portal: off in {}; no hooks installed (switching on needs a restart)", Paths::PluginDirectory().string());
			return;
		}
		// Before anything asks whether a room can be merged (MergedView::Mergeable).
		::Render::Backend::Install();
		RemoteCell::InstallStepHook(stepRemote);
		RemoteCell::InstallGridHook();
		Render::Frame::Install();
		Render::Frame::Prewarm();
		Portal::Snapshot::Prewarm();
		Portal::Travel::Install();
		Portal::CameraLimit::Install();
		Portal::StreetLOD::Install();
		MergedCull::Install();
		Portal::Doors::InstallPromptHook();
		EngineHooks::Install();
		logger::info("portal: manager installed, {} ({})", enabled ? "enabled" : "disabled", Paths::PluginDirectory().string());
	}

	bool HooksInstalled()
	{
		return hooksInstalled;
	}

	void SetEnabled(bool a_enabled)
	{
		if (a_enabled && !hooksInstalled) {
			logger::warn("portal: can't be switched on in this session (off in the ini at startup)");
			return;
		}
		enabled = a_enabled;
		logger::info("portal: {}", enabled ? "enabled" : "disabled");
	}

	bool Enabled()
	{
		return enabled;
	}

	void Update(float a_delta)
	{
		StreetMerge::EveryFrame();
		approachLimit.reset();
		Portal::Doors::HidePrompt();
		Portal::Doors::Swing::Expire(a_delta);
		static float forgetTimer = 0.0f;
		if ((forgetTimer -= a_delta) <= 0.0f) {
			forgetTimer = 5.0f;
			Doors::ForgetUnloaded();
		}
		retry.Tick(a_delta);
		Crossing::Tick(a_delta);
		if (!enabled) {
			StandIns::Drop();
			ReleaseAll("disabled");
			Portal::DoorUse::Clear();
			// Otherwise only cleared at the end of an enabled frame: switched off in a doorway, the
			// blended image space and the camera's stop at the door plane stayed.
			ImageSpaceBlend::Clear();
			Portal::CameraLimit::ClearPlane();
			return;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* here = player ? player->GetParentCell() : nullptr;
		if (!here) {
			return;
		}
		if (!here->IsInteriorCell() && RemoteCell::PersistentToHang()) {
			RemoteCell::HangHandedPersistent();
		}
		if (remote.Attached() && !remote.Healthy()) {
			ReleaseShown("remote cell no longer attached under the portal");
		}

		Selection::TrackHeading(player->GetPosition());
		NoteActorUses();
		for (const auto& [door, linked] : Portal::DoorUse::Update(a_delta)) {
			Portal::DoorUse::PlayDoorSound(door, linked, false);
		}
		const auto candidate = Selection::CandidateDoor(player, a_delta);
		EndOrSwitchPortal(player, here, candidate);
		// Before the portal's own door handling (UpdateShown): a door a stand-in held is handed over
		// this frame.
		StandIns::Update(player, here, a_delta);
		StandIns::WalkInto(player, here, a_delta);
		if (!CurrentPortal() && candidate && !Crossing::AttachHeld() && !retry.Blocks(candidate->cell->GetFormID())) {
			AttachCandidate(here, candidate);
		}
		Selection::UpdateWarm(here, a_delta, { retry, Crossing::AttachHeld(), CurrentPortal() && !(remote.Healthy() && remote.FullyLoaded()) });
		std::vector<MergedCull::Room> culls;
		UpdateShown(player, here, a_delta, culls);
		Gates::UpdateMask(here);
		MergedView::UpdateBeside(here, a_delta, culls);
		MergedCull::Set(std::move(culls));
		MergedView::Publish();
		Crossing::EndFrame(player);
		const auto ready = ReadyPortal();
		MergedView::UpdateLightWorlds(ready);
		MergedView::PublishShownCells(remote.Merged() || ActivePortal().has_value());
		if (ready) {
			NpcTravel::Update(*ready, remote.Cells());
		} else {
			NpcTravel::Reset();
		}
		ImageSpaceBlend::Update(ready);
		Crossing::UpdateCameraPlane(here, ready);
		PublishOpenDoors();
	}

	void OnArrival()
	{
		Update(0.0f);
	}

	void ReleaseAll(std::string_view a_reason, float a_holdoff, bool a_handOver)
	{
		{
			const Diag::Scope scope("release kept");
			Selection::ReleaseWarm(a_reason);
		}
		const Diag::Scope scope("release shown");
		ReleaseShown(a_reason, a_holdoff, a_handOver);
	}

	void ForgetDoors()
	{
		StandIns::Forget();
		Portal::Doors::Swing::ForgetAll();
		Portal::DoorUse::Clear();
	}

	void SetMergeEnabled(bool a_enabled)
	{
		MergedView::SetMergeEnabled(a_enabled);
		if (!a_enabled && remote.Merged()) {
			ReleaseAll("merging turned off");
		}
		logger::info("portal: merging {}", a_enabled ? "on" : "off");
	}

	std::optional<float> CameraLimit()
	{
		return approachLimit;
	}

	RemoteCell& Space()
	{
		return remote;
	}

	std::optional<Active> CurrentPortal()
	{
		std::scoped_lock lock(activeLock);
		return active;
	}

	std::optional<Active> ReadyPortal()
	{
		const auto portal = CurrentPortal();
		return portal && remote.Healthy() && remote.FullyLoaded() ? portal : std::nullopt;
	}

	std::optional<Active> ActivePortal()
	{
		return remote.Merged() ? std::nullopt : ReadyPortal();
	}

	bool SpaceShown(RE::FormID a_door)
	{
		const auto portal = CurrentPortal();
		return (portal && portal->sourceDoor == a_door && (remote.Merged() || ReadyPortal())) || MergedView::BesideDoor(a_door);
	}

	bool Merged()
	{
		return remote.Merged();
	}

	bool DoorShut()
	{
		return remote.Merged() ? MergedView::ShownShut() : PortalDoor::Shut();
	}

	const std::vector<RE::TESObjectCELL*>& RemoteCells()
	{
		return remote.Cells();
	}

	const std::string& LastEvent()
	{
		return lastEvent;
	}

	DoorPrompt Prompt(RE::FormID a_door)
	{
		auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_door);
		auto* linked = door && enabled ? Portal::Doors::UsedPairOf(door) : nullptr;
		return !linked                              ? DoorPrompt::kOpen :
		       Portal::Doors::StandsOpen(door, linked) ? DoorPrompt::kNone :
		       DoorUse::Used(a_door)                ? DoorPrompt::kClose :
		                                              DoorPrompt::kOpen;
	}

	void SetStepExterior(bool a_step)
	{
		Spaces::tuning.stepExterior = a_step;
	}

	bool StepExterior()
	{
		return Spaces::tuning.stepExterior;
	}
}
