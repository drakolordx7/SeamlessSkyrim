#include "Skyrim/Portal/StandIns.h"

#include "Core/Crossing/Approach.h"

#include "Skyrim/Engine/CoreConvert.h"
#include "Skyrim/Engine/SameSpace.h"
#include "Skyrim/Doors/DoorUse.h"
#include "Skyrim/Doors/Geometry.h"
#include "Skyrim/Doors/Pair.h"
#include "Skyrim/Doors/Policy.h"
#include "Skyrim/Doors/Settings.h"
#include "Skyrim/Pictures/DoorwaySnapshot.h"
#include "Skyrim/Portal/Crossing.h"
#include "Skyrim/Portal/Gates.h"
#include "Skyrim/Portal/MergedView.h"
#include "Skyrim/Portal/PortalDoor.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/Schedule.h"
#include "Skyrim/Portal/Selection.h"
#include "Skyrim/Portal/Travel.h"
#include "Skyrim/Spaces/RemoteCell.h"

namespace Portal::StandIns
{
	namespace
	{
		struct PictureWait
		{
			RE::FormID                                           door = 0;
			std::optional<std::chrono::steady_clock::time_point> since;
			bool                                                 gaveUp = false;
		};
		PictureWait    pictureWait;
		RE::FormID     pictureWanted = 0;  // of the render, this frame
		constexpr auto kPictureLightsSettle = std::chrono::milliseconds(300);
		// A big room takes a few dozen capture frames to measure its lights for (LightOcclusion).
		constexpr auto kPictureGiveUp = std::chrono::milliseconds(3000);

		std::unordered_map<RE::FormID, DoorwayStandIn> standIns;  // by the door in the player's space
		constexpr float                                kStandInFadeSeconds = 0.4f;

		// The doors that may stand open, found a few times a second: the references around the player
		// within kStandInReach, the doors set to stand open always and those used, wherever they are.
		struct StandInDoor
		{
			RE::FormID door;
			RE::FormID linked;
			bool       outdoors;  // the space behind is outdoors
		};
		std::vector<StandInDoor> standInDoors;
		float                    standInScanTimer = 0.0f;
		constexpr float          kStandInScanSeconds = 0.5f;
		// As far as a city's gate can be seen from inside it (Whiterun's from its market, Solitude's
		// down its main street), or the doors of a street forced open.
		constexpr float          kStandInReach = 6000.0f;
		// The player's cell standInDoors was found from. Walked through a door, the list from the space
		// left still held the door just used, its far side now the player's own room: until the next
		// scan it got a stand-in, and walked through backwards, the doorway showed the space the player
		// stood in (the room's picture looking out of it, the street's looking into it).
		RE::TESObjectCELL*       standInScanCell = nullptr;
		// The space merged into the player's when standInDoors was found (InView). Walked into a shop,
		// the scan at the cell change ran before the street was merged into it: the street's doors
		// that stand open (the Bannered Mare's) were seen shut until the next scan, half a second on.
		RE::TESObjectCELL*       standInScanMerged = nullptr;

		// Which of the pair a stand-in swings: the one the portal would (Doors::SwingingDoor), its
		// picture showing the far one swung; but a gate without a picture swings its own, or the
		// gateway stood empty.
		RE::FormID StandInSwinging(RE::TESObjectREFR* a_door, DoorwayStandIn& a_standIn)
		{
			const auto id = a_door->GetFormID();
			a_standIn.picture = Gates::ShowsPicture(a_door, a_standIn.linked, a_standIn.outdoors, a_standIn.gate);
			return a_standIn.picture ? Portal::Doors::SwingingDoor(Portal::Doors::Side(a_door), { a_standIn.linked, a_standIn.outdoors }) : id;
		}

		// Shut again, the stand-in's door and, when the far door is the one swung, that one too: taken
		// over from a let-go (TakeOver), nothing else puts it back, and its cell's 3D, reused, showed
		// it standing open.
		void UnswingStandIn(RE::FormID a_door, const DoorwayStandIn& a_standIn)
		{
			Portal::Doors::RestoreLeaves(a_door);
			if (a_standIn.swinging && a_standIn.swinging != a_door && a_standIn.swinging != PortalDoor::Swung()) {
				Portal::Doors::RestoreLeaves(a_standIn.swinging);
			}
		}

		// In the player's space, or in the space merged into it (seen through a door): there a door set
		// to stand open always was drawn shut, the street seen from inside a shop showing the inn
		// across it with its door closed.
		bool InView(RE::TESObjectREFR* a_door, RE::TESObjectCELL* a_here)
		{
			return InPlayerSpace(a_door, a_here) || (Manager::Space().Merged() && a_door && std::ranges::find(Manager::Space().Cells(), a_door->GetParentCell()) != Manager::Space().Cells().end());
		}

		void ScanStandInDoors(RE::PlayerCharacter* a_player, RE::TESObjectCELL* a_here)
		{
			standInDoors.clear();
			const auto add = [&](RE::TESObjectREFR* a_door) {
				const auto linked = Selection::PortalLink(a_door, a_here);
				// A door out of a building shows the street only live (StreetShownLive).
				if (!linked || Portal::Doors::StreetShownLive(a_door, Portal::Doors::Outdoors(linked.get())) || Portal::Doors::PortalOff(a_door->GetFormID()) || Portal::Doors::PortalOff(linked->GetFormID()) ||
					std::ranges::any_of(standInDoors, [&](const StandInDoor& a_known) { return a_known.door == a_door->GetFormID(); })) {
					return;
				}
				standInDoors.push_back({ a_door->GetFormID(), linked->GetFormID(), Portal::Doors::Outdoors(linked.get()) });
			};
			RE::TES::GetSingleton()->ForEachReferenceInRange(a_player, kStandInReach, [&](RE::TESObjectREFR* a_ref) {
				add(a_ref);
				return RE::BSContainer::ForEachResult::kContinue;
			});
			// Stored under one door of the pair: either may be the one in the player's space.
			auto doors = Portal::Doors::AlwaysOpenDoors();
			std::ranges::copy(Portal::DoorUse::UsedDoors(), std::back_inserter(doors));
			for (const auto id : doors) {
				auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				for (auto* candidate : { door, Portal::Doors::LinkedOf(door) }) {
					if (InView(candidate, a_here) && candidate->Get3D()) {
						add(candidate);
					}
				}
			}
		}

		struct Pressing
		{
			RE::FormID door = 0;
			float      depth = 0.0f;
			float      held = 0.0f;  // seconds walked into it without getting nearer
		} pressing;
	}

	void Drop()
	{
		for (auto& [door, standIn] : standIns) {
			if (!standIn.handedOver) {
				UnswingStandIn(door, standIn);
			}
			Portal::Doors::Close(standIn.open);
		}
		standIns.clear();
	}

	void Forget()
	{
		Drop();
		standInDoors.clear();
		standInScanTimer = 0.0f;
		standInScanCell = nullptr;
		standInScanMerged = nullptr;
	}

	const std::unordered_map<RE::FormID, DoorwayStandIn>& Doorways()
	{
		return standIns;
	}

	// The side a stand-in's door swings away from (a_viewer's), in the player's space: Swing reads
	// it against the door as drawn, which for a door of the merged space isn't where its own cell
	// puts it.
	RE::NiPoint3 AwayFrom(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_viewer)
	{
		const auto c = Portal::Doors::ShownQuad(a_door, a_viewer);
		auto       normal = (c[1] - c[0]).Cross(c[2] - c[0]);
		normal.z = 0.0f;
		normal.Unitize();
		return normal.Dot(Portal::Doors::ShownCenter(a_door) - a_viewer) >= 0.0f ? normal : normal * -1.0f;
	}

	// In the space the player is in: their cell, or outdoors any cell of the same worldspace (the
	// cells around are walked between without a door, so their doors are as near as the cell's own).
	bool InPlayerSpace(RE::TESObjectREFR* a_door, RE::TESObjectCELL* a_here)
	{
		return Engine::SameSpace(a_door ? a_door->GetParentCell() : nullptr, a_here);
	}

	void Update(RE::PlayerCharacter* a_player, RE::TESObjectCELL* a_here, float a_delta)
	{
		auto* merged = Manager::Space().Merged() ? Manager::Space().Cell() : nullptr;
		if ((standInScanTimer -= a_delta) <= 0.0f || a_here != standInScanCell || merged != standInScanMerged) {
			standInScanTimer = kStandInScanSeconds;
			standInScanCell = a_here;
			standInScanMerged = merged;
			ScanStandInDoors(a_player, a_here);
		}
		const auto real = Manager::SpaceShown;
		const auto origin = a_player->GetPosition();
		for (const auto& candidate : standInDoors) {
			auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(candidate.door);
			// Not a door the portal holds open itself (swinging shut after its space was let go).
			// Only a doorway whose space the scheduler holds at a picture or better (Schedule::ShowsPicture).
			if (standIns.contains(candidate.door) || !door || !door->Get3D() || real(candidate.door) || (PortalDoor::Holds(candidate.door) && !PortalDoor::Shut()) ||
				!Schedule::ShowsPicture(candidate.door) || !Portal::Doors::Wanted(door, { candidate.linked, candidate.outdoors })) {
				continue;
			}
			auto& standIn = standIns[candidate.door];
			standIn.linked = candidate.linked;
			standIn.outdoors = candidate.outdoors;
			standIn.gate = candidate.outdoors && Portal::Doors::Outdoors(door);
			standIn.cameThrough = Portal::Travel::ArrivalDoor() == candidate.door;
			standIn.swinging = StandInSwinging(door, standIn);
			standIn.swing.TakeOver(standIn.swinging);
			if (Portal::Doors::StandsOpen(door, { candidate.linked, candidate.outdoors })) {
				standIn.swing.SnapOpen();
			}
			standIn.inwardInCell = Portal::Doors::ToShown(door).rotate.Transpose() * AwayFrom(door, origin);
		}
		for (auto it = standIns.begin(); it != standIns.end();) {
			const auto id = it->first;
			auto&      standIn = it->second;
			auto*      door = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
			if (standIn.handedOver) {
				const bool portalHolds = PortalDoor::Holds(id) && !PortalDoor::Shut();
				if (!real(id) && !portalHolds) {
					// The real space went while the picture was fading (the portal let go): the stand-in
					// holds the door again, from where the portal left it. Erased instead, the door had
					// no opener until the next frame made a new one: the box behind its panel and a
					// hidden near door back for a frame.
					standIn.handedOver = false;
					standIn.fade = 1.0f;
					standIn.swing.TakeOver(standIn.swinging);
				} else {
					standIn.fade -= a_delta / kStandInFadeSeconds;
					if (standIn.fade <= 0.0f || !real(id)) {
						Portal::Doors::Close(standIn.open);
						it = standIns.erase(it);
					} else {
						++it;
					}
					continue;
				}
			}
			if (!door || !door->Get3D() || !InView(door, a_here) || !Schedule::ShowsPicture(id)) {
				UnswingStandIn(id, standIn);
				Portal::Doors::Close(standIn.open);
				it = standIns.erase(it);
				continue;
			}
			if (real(id)) {
				// The real space is there: whoever opens the door for it now takes it over where the
				// stand-in left it (the merged room's own door, standing in the same place, among them).
				standIn.swing.LetGo(standIn.swinging);
				standIn.handedOver = true;
				standIn.fade = standIn.cameThrough ? 0.0f : 1.0f;
				++it;
				continue;
			}
			const Portal::Doors::Side farSide{ standIn.linked, standIn.outdoors };
			standIn.swing.Step(door, Portal::Doors::Wanted(door, farSide), a_delta);
			if (Portal::Doors::StandsOpen(door, farSide)) {
				standIn.swing.SnapOpen();
			}
			if (standIn.swing.Shut()) {
				UnswingStandIn(id, standIn);
				Portal::Doors::Close(standIn.open);
				it = standIns.erase(it);
				continue;
			}
			// The menu may have turned it round meanwhile.
			standIn.swinging = StandInSwinging(door, standIn);
			const bool farSwings = standIn.swinging != id;
			Portal::Doors::Open(door, standIn.open, { .hide = farSwings, .solid = true });
			if (farSwings) {
				// Its door is the one behind, not there yet: this one is hidden, and the picture shows
				// the doorway open (the far door swung in it, if it was taken while that stood open).
				Portal::Doors::RestoreLeaves(id);
			} else {
				auto*      linked = RE::TESForm::LookupByID<RE::TESObjectREFR>(standIn.linked);
				const auto settings = linked ? Portal::Doors::CorrectionDoor(door, linked)->GetFormID() : id;
				// A gate without a picture swings its own leaves where the portal swings the far
				// gate's: into the space behind. Swung towards the player instead, the leaves jumped
				// from one side of the gateway to the other when the portal took the gate over. Not
				// where the gates stand apart: there its leaves would swing into the masked passage.
				const bool intoFar = standIn.gate && Gates::SwingsIntoFar(door, standIn.linked, AwayFrom(door, origin));
				const auto away = Portal::Doors::ToShown(door).rotate * standIn.inwardInCell;
				standIn.swing.Turn(door, door, intoFar ? away : away * -1.0f, settings);
			}
			++it;
		}
	}

	// The amount to start swinging a door at for the space behind it: where a stand-in holds it, or
	// where it was let go of. The space becomes ready in the middle of a frame (RemoteCell::Update),
	// after the stand-ins were updated: the portal's door handling took the door that frame, before
	// the stand-in had let go of it, and swung it open again from shut.
	void TakeOverDoor(Portal::Doors::Swing& a_swing, RE::FormID a_door)
	{
		for (auto& [id, standIn] : standIns) {
			if (!standIn.handedOver && standIn.swinging == a_door) {
				standIn.handedOver = true;
				standIn.fade = standIn.cameThrough ? 0.0f : 1.0f;
				standIn.swing.LetGo(a_door);
				break;
			}
		}
		a_swing.TakeOver(a_door);
	}

	bool WaitForPicture(const Active& a_portal, const RE::TESObjectCELL* a_here, bool a_mergeable)
	{
		pictureWanted = 0;
		if (pictureWait.door != a_portal.sourceDoor) {
			pictureWait = { a_portal.sourceDoor };
		}
		if (!a_mergeable || a_here->IsInteriorCell() || !a_portal.cell->IsInteriorCell()) {
			pictureWait.since.reset();
			return false;
		}
		auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_portal.sourceDoor);
		if (pictureWait.gaveUp ||
			!Pictures::CameraPictureWanted(a_portal.sourceDoor, Portal::Doors::Quad(door, RE::PlayerCharacter::GetSingleton()->GetPosition(), { .behindPanel = true }))) {
			return false;
		}
		const auto now = std::chrono::steady_clock::now();
		if (!pictureWait.since) {
			pictureWait.since = now;
		}
		if (now - *pictureWait.since >= kPictureGiveUp) {
			pictureWait.gaveUp = true;
			logger::warn("portal: no doorway picture for door {:08X} from the capture camera; merged without", a_portal.sourceDoor);
			return false;
		}
		if (now - *pictureWait.since >= kPictureLightsSettle) {
			pictureWanted = a_portal.sourceDoor;
		}
		return true;
	}

	RE::FormID PictureWanted()
	{
		const auto portal = Manager::CurrentPortal();
		return portal && !Manager::Space().Merged() && portal->sourceDoor == pictureWanted ? pictureWanted : 0;
	}

	void WalkInto(RE::PlayerCharacter* a_player, RE::TESObjectCELL* a_here, float a_delta)
	{
		// From the doorway's centre out: the door's panel plus what may stop the player short of it
		// (a city gate's arch reaches well past its panel).
		constexpr float kReach = 120.0f;
		// Held back (by the door, or anything in front of it) rather than still walking up to it.
		constexpr float kHeldSpeed = 30.0f;
		constexpr float kHeldSeconds = 0.15f;
		RE::FormID  door = 0;
		float       depth = 0.0f;
		const float yaw = a_player->GetAngleZ();
		const auto  facing = RE::NiPoint3{ std::sin(yaw), std::cos(yaw), 0.0f };
		const auto  position = a_player->GetPosition();
		if (a_player->IsMoving() && Crossing::Ready()) {
			for (const auto& [id, standIn] : standIns) {
				auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				if (standIn.handedOver || standIn.swing.Amount() < 0.5f || !ref || !ref->Get3D() || !InPlayerSpace(ref, a_here) || ref->IsLocked()) {
					continue;
				}
				const auto size = Portal::Doors::Size(ref);
				const auto inward = AwayFrom(ref, position);
				const Core::Crossing::Doorway doorway{ ToCore(Portal::Doors::Center(ref)), ToCore(inward), size.halfWidth, size.halfDepth, size.halfHeight };
				const float d = Core::Crossing::DepthOf(doorway, ToCore(position));  // < 0: in front
				if (d >= -(size.halfDepth + kReach) && Core::Crossing::SideOf(doorway, ToCore(position)) <= size.halfWidth && Core::Crossing::FeetAtDoorway(position.z, doorway) &&
					facing.Dot(inward) > 0.5f) {
					door = id;
					depth = d;
					break;
				}
			}
		}
		if (!door || door != pressing.door) {
			pressing = { door, depth, 0.0f };
			return;
		}
		pressing.held = depth - pressing.depth < kHeldSpeed * a_delta ? pressing.held + a_delta : 0.0f;
		pressing.depth = depth;
		if (pressing.held < kHeldSeconds) {
			return;
		}
		pressing = {};
		auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(door);
		logger::info("portal: door {:08X} walked into before the space behind it was there: taken the engine's way", door);
		Crossing::ThroughDoor(ref);
	}

	std::vector<DoorwayFill> Fills()
	{
		std::vector<DoorwayFill> fills;
		for (const auto& [id, standIn] : standIns) {
			auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
			if (door && door->Get3D() && standIn.picture && (!standIn.handedOver || standIn.fade > 0.0f)) {
				fills.push_back({ id, standIn.outdoors, standIn.gate, standIn.handedOver ? standIn.fade : 1.0f });
			}
		}
		return fills;
	}

	std::optional<DoorwayFill> SnapshotChance()
	{
		// The doorway fully open on the real space for a moment: the room's late lights adopted,
		// the door done swinging.
		static std::optional<std::chrono::steady_clock::time_point> since;
		const auto portal = Manager::CurrentPortal();
		const bool open = portal && !standIns.contains(portal->sourceDoor) &&
		                  (Manager::Space().Merged() ? MergedView::ShownAmount() >= 1.0f : (Manager::ActivePortal() && PortalDoor::Holds(portal->sourceDoor) && PortalDoor::Amount() >= 1.0f));
		if (!open) {
			since.reset();
			return std::nullopt;
		}
		const auto now = std::chrono::steady_clock::now();
		if (!since) {
			since = now;
		}
		if (now - *since < std::chrono::milliseconds(1500)) {
			return std::nullopt;
		}
		auto* here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		const bool outdoors = portal->cell && !portal->cell->IsInteriorCell();
		const bool gate = Gates::IsGate(here, portal->cell);
		if (Gates::SkipsPicture(*portal, outdoors, gate)) {
			return std::nullopt;
		}
		return DoorwayFill{ portal->sourceDoor, outdoors, gate, 1.0f };
	}
}
