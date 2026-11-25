#include "Skyrim/Portal/Selection.h"

#include "Skyrim/Diag/Probe.h"
#include "Skyrim/Diag/Telemetry.h"
#include "Skyrim/Doors/DoorUse.h"
#include "Skyrim/Doors/Settings.h"
#include "Skyrim/Portal/Gates.h"
#include "Skyrim/Portal/MergedView.h"
#include "Skyrim/Portal/PortalManager.h"

namespace Portal::Selection
{
	namespace
	{
		float           doorOpenRadius = static_cast<float>(Plugin::Settings::Of(Plugin::Settings::Id::kDoorOpenDistance).def);
		// Few doors around the player (the scan, NearestPortalDoor): nothing competes for what is loaded,
		// so it is loaded from much further out without spaces being swapped as the player moves.
		// Outdoors the kept rooms reach further (warmReach); indoors, where one space is loaded, and
		// outdoors near a door into another worldspace, the shown one does (farActivate, 0 when it
		// doesn't apply).
		bool            sparse = false;
		float           farActivate = 0.0f;
		float           warmReach = kWarmDistance;
		int             shownRoomsSetting = Plugin::Settings::Of(Plugin::Settings::Id::kShownRooms).def;
		// Half the switch margin: turning round between two doors equally far changes their order by
		// twice this, which by itself must not swap what is loaded.
		constexpr float kHeadingNudge = kSwitchMargin / 2.0f;
		RE::NiPoint3    heading;
		RE::NiPoint3    headingFrom;

		// A room kept loaded is let go only kWarmMargin further out, so walking along that boundary
		// doesn't attach and let go of the same room over and over.
		constexpr float       kWarmMargin = 400.0f;
		// With few doors around (sparse), rooms are kept from this many times as far: warmReach.
		constexpr float       kSparseReach = 2.0f;
		// Up to this many spaces behind doors within reach (or as many as are loaded at once, if more)
		// count as few.
		constexpr std::size_t kFewDoors = 3;
		// One attach at a time, a few times a second at most: each makes a long frame (3-10 ms in
		// Whiterun's market, more for a room whose 3D has to be loaded).
		constexpr float       kWarmAttachSeconds = 0.5f;
		float                 warmAttachTimer = 0.0f;
		constexpr float       kRetryCooldown = 5.0f;

		std::vector<WarmTarget> warmTargets;
		std::vector<RemoteCell> warm;

		std::optional<Candidate> NearestPortalDoor(RE::PlayerCharacter* a_player)
		{
			warmTargets.clear();
			struct Found
			{
				RE::TESObjectREFR* door;
				RE::NiPointer<RE::TESObjectREFR> linked;
				RE::TESObjectCELL* cell;  // null: an exterior cell not loaded yet
				float              distance;
				float              reach;  // see Reach
				float              rank;
			};
			std::vector<Found> found;
			auto*              here = a_player->GetParentCell();
			const auto         origin = a_player->GetPosition();
			const bool         outdoors = !here->IsInteriorCell();
			const bool         collectWarm = shownRoomsSetting > 1 && outdoors;
			const float        farReach = outdoors ? kWarmDistance * kSparseReach : kWarmDistance;
			RE::TES::GetSingleton()->ForEachReferenceInRange(a_player, farReach + kWarmMargin, [&](RE::TESObjectREFR* a_ref) {
				auto linked = PortalLink(a_ref, here);
				if (!linked) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				// Switched off in the menu: an ordinary door (stored under either of the pair).
				if (Portal::Doors::PortalOff(a_ref->GetFormID()) || Portal::Doors::PortalOff(linked->GetFormID())) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				found.push_back({ a_ref, linked, linked->GetParentCell(), origin.GetDistance(a_ref->GetPosition()), Reach(origin, a_ref, linked.get()), Rank(origin, a_ref, linked.get()) });
				return RE::BSContainer::ForEachResult::kContinue;
			});
			{
				// Spaces, not doors: an inn's two doors load one room.
				const auto spaces = [&](float a_within) {
					std::vector<RE::FormID> ids;
					for (const auto& door : found) {
						const auto id = door.cell ? door.cell->GetFormID() : door.linked->GetFormID();
						if (door.distance <= a_within && std::ranges::find(ids, id) == ids.end()) {
							ids.push_back(id);
						}
					}
					return ids.size();
				};
				// Between the reach and the scan's margin beyond it, as it was: a door at the edge
				// doesn't change the reach, and with it what is loaded, back and forth.
				const auto few = std::max(kFewDoors, static_cast<std::size_t>(outdoors ? shownRoomsSetting : 1));
				if (spaces(farReach) > few) {
					sparse = false;
				} else if (spaces(FLT_MAX) <= few) {
					sparse = true;
				}
				// Outdoors a kept room stands in for loading the shown one early, but only interiors are
				// kept: a city gate's worldspace was loaded at the near reach, its low-detail stand-in seen
				// through the open gate until then.
				const bool worldDoor = std::ranges::any_of(found, [&](const Found& a_door) {
					return a_door.distance <= farReach && (!a_door.cell || !a_door.cell->IsInteriorCell());
				});
				farActivate = sparse && (!outdoors || worldDoor) ? farReach : 0.0f;
				warmReach = sparse && outdoors ? farReach : kWarmDistance;
			}
			const Found* best = nullptr;
			for (const auto& door : found) {
				if (collectWarm && door.cell && door.cell->IsInteriorCell()) {
					// A room with two doors: which room is kept goes by the better rank of the two, but
					// the door it is shown through is the nearer one. Taken by rank as well, the door
					// changed as the player turned on the spot, to one out of reach, and the room's open
					// doorway in front of them went shut.
					const auto room = std::ranges::find(warmTargets, door.cell->GetFormID(), &WarmTarget::cell);
					if (room == warmTargets.end()) {
						warmTargets.push_back({ door.cell->GetFormID(), door.distance, door.rank, door.door->GetFormID(), door.linked->GetFormID() });
					} else {
						room->rank = std::min(room->rank, door.rank);
						if (door.distance < room->distance) {
							room->distance = door.distance;
							room->door = door.door->GetFormID();
							room->linked = door.linked->GetFormID();
						}
					}
				}
				if (door.reach <= ActivateRadius() && (!best || door.rank < best->rank)) {
					best = &door;
				}
			}
			std::ranges::sort(warmTargets, {}, &WarmTarget::rank);
			if (!best) {
				return std::nullopt;
			}
			auto* cell = best->cell;
			if (!cell) {
				// Loaded for the nearest door only: done for every door in range, every scan, it could
				// go to disk for cells nobody was walking towards.
				auto* world = best->linked->GetWorldspace();
				SEAMLESS_ASSERT(world);  // chosen above only with a worldspace when it has no cell
				const auto pos = best->linked->GetPosition();
				cell = world->LoadCell(static_cast<std::int16_t>(std::floor(pos.x / 4096.0f)), static_cast<std::int16_t>(std::floor(pos.y / 4096.0f)));
				if (!cell) {
					return std::nullopt;
				}
			}
			return Candidate{ best->door, best->linked->GetFormID(), cell, best->distance, best->rank };
		}

		// The scan above walks every reference within ActivateRadius; doors don't come and go that
		// fast, so it runs a few times a second and the result is looked up again in between.
		constexpr float kScanSeconds = 0.2f;
		float           scanTimer = 0.0f;
		struct ScannedDoor
		{
			RE::FormID door;
			RE::FormID linkedDoor;
			RE::FormID cell;
		};
		std::optional<ScannedDoor> scanned;

		// The interiors to keep warm: the nearest of warmTargets within a_reach, as many as there is
		// room for beside the shown space, not that one.
		std::vector<RE::FormID> WarmWanted(float a_reach)
		{
			std::vector<RE::FormID> wanted;
			const auto              shown = Manager::CurrentPortal();
			for (const auto& target : warmTargets) {
				if (std::cmp_greater_equal(wanted.size() + 1, shownRoomsSetting)) {
					break;
				}
				if (target.distance <= a_reach && (!shown || shown->cell->GetFormID() != target.cell)) {
					wanted.push_back(target.cell);
				}
			}
			return wanted;
		}

		// How far the nearest door into a_cell was at the last scan (beyond any reach if none was).
		float WarmDistanceOf(RE::FormID a_cell)
		{
			const auto target = std::ranges::find(warmTargets, a_cell, &WarmTarget::cell);
			return target != warmTargets.end() ? target->distance : FLT_MAX;
		}

		// The same by Rank: which of two rooms is kept.
		float WarmRankOf(RE::FormID a_cell)
		{
			const auto target = std::ranges::find(warmTargets, a_cell, &WarmTarget::cell);
			return target != warmTargets.end() ? target->rank : FLT_MAX;
		}

		std::optional<DoorPair> NearestDoorPair(float a_radius)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* here = player ? player->GetParentCell() : nullptr;
			if (!here) {
				return std::nullopt;
			}
			std::optional<DoorPair> best;
			float                   bestDistance = 0.0f;
			const auto              origin = player->GetPosition();
			RE::TES::GetSingleton()->ForEachReferenceInRange(player, a_radius, [&](RE::TESObjectREFR* a_ref) {
				if (const auto linked = PortalLink(a_ref, here)) {
					const float distance = origin.GetDistance(a_ref->GetPosition());
					if (!best || distance < bestDistance) {
						best = DoorPair{ a_ref->GetFormID(), linked->GetFormID() };
						bestDistance = distance;
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return best;
		}
	}

	void Install()
	{
		SetDoorOpenDistance(static_cast<float>(Plugin::Settings::Int(Plugin::Settings::Id::kDoorOpenDistance)));
		SetShownRooms(Plugin::Settings::Int(Plugin::Settings::Id::kShownRooms));
	}

	float DoorOpenDistance()
	{
		return doorOpenRadius;
	}

	void SetDoorOpenDistance(float a_distance)
	{
		const auto& reach = Plugin::Settings::Of(Plugin::Settings::Id::kDoorOpenDistance);
		doorOpenRadius = std::clamp(a_distance, static_cast<float>(reach.min), static_cast<float>(reach.max));
	}

	int ShownRooms()
	{
		return shownRoomsSetting;
	}

	void SetShownRooms(int a_count)
	{
		shownRoomsSetting = std::clamp(a_count, 1, kMaxShownRooms);
	}

	float NearRadius()
	{
		return doorOpenRadius + kLoadAhead;
	}

	float ActivateRadius()
	{
		return std::max(NearRadius(), farActivate);
	}

	float ReleaseRadius()
	{
		return ActivateRadius() + kReleaseBeyond;
	}

	void TrackHeading(const RE::NiPoint3& a_position)
	{
		auto step = a_position - headingFrom;
		step.z = 0.0f;
		const float length = step.Length();
		// Over a few frames' worth of walking: a frame's step is too short to give a direction.
		if (length < 32.0f) {
			return;
		}
		headingFrom = a_position;
		// A jump is a teleport or a crossing into another space's coordinates, no direction.
		heading = length > 500.0f ? RE::NiPoint3() : step / length;
	}

	float Reach(const RE::NiPoint3& a_origin, RE::TESObjectREFR* a_door, RE::TESObjectREFR* a_linked)
	{
		const float distance = a_origin.GetDistance(a_door->GetPosition());
		return Portal::Doors::Outdoors(a_door) && Portal::Doors::Outdoors(a_linked) ? distance * Gates::kReachWeight : distance;
	}

	float Rank(const RE::NiPoint3& a_origin, RE::TESObjectREFR* a_door, RE::TESObjectREFR* a_linked)
	{
		const auto to = a_door->GetPosition() - a_origin;
		return Reach(a_origin, a_door, a_linked) - kHeadingNudge * heading.Dot(to) / std::max(to.Length(), 1.0f);
	}

	// Portal doors are load doors into another space: outside -> interior, interior -> outside or
	// another interior, and from one worldspace into another (a walled city's gate). Exterior doors
	// within one worldspace aren't: both ends are in the scene already.
	RE::NiPointer<RE::TESObjectREFR> PortalLink(RE::TESObjectREFR* a_ref, RE::TESObjectCELL* a_here)
	{
		auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
		// A locked door stays a door: closed, solid, to be picked or opened with its key. Unless
		// every door is to stand open (DoorUse's force).
		const bool locked = a_ref && a_ref->IsLocked() && Portal::DoorUse::GetForce() != Portal::DoorUse::Force::kAllOpen;
		if (!base || !base->Is(RE::FormType::Door) || a_ref->IsDisabled() || !a_ref->Get3D() || locked) {
			return nullptr;
		}
		// A marker door is no doorway: unseen, used by scripts (Riften's prison marker, which takes
		// an arrested player to the jail). Its portal stood in the open street, next to the keep.
		if (static_cast<RE::TESObject*>(base)->IsMarker()) {
			return nullptr;
		}
		auto* teleport = a_ref->extraList.GetByType<RE::ExtraTeleport>();
		auto  linked = teleport && teleport->teleportData ? teleport->teleportData->linkedDoor.get() : nullptr;
		if (!linked) {
			return nullptr;
		}
		// A persistent exterior door whose cell isn't loaded (a game loaded indoors) has no
		// parent cell yet; it stands outdoors in its worldspace.
		auto*       cell = linked->GetParentCell();
		auto* const linkedWorld = cell ? (cell->IsInteriorCell() ? nullptr : cell->GetRuntimeData().worldSpace) : linked->GetWorldspace();
		const bool  sameWorld = linkedWorld && !a_here->IsInteriorCell() && linkedWorld == a_here->GetRuntimeData().worldSpace;
		if ((!cell && !linkedWorld) || cell == a_here || sameWorld) {
			return nullptr;
		}
		return linked;
	}

	std::optional<Candidate> CandidateDoor(RE::PlayerCharacter* a_player, float a_delta)
	{
		if ((scanTimer -= a_delta) <= 0.0f) {
			scanTimer = kScanSeconds;
			const auto found = NearestPortalDoor(a_player);
			scanned = found ? std::optional(ScannedDoor{ found->door->GetFormID(), found->linkedDoor, found->cell->GetFormID() }) : std::nullopt;
			return found;
		}
		if (!scanned) {
			return std::nullopt;
		}
		// By ID: a door is a temporary reference of its cell and goes with it.
		auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(scanned->door);
		auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(scanned->cell);
		if (!door || !door->Get3D() || !cell) {
			return std::nullopt;
		}
		auto*      linked = RE::TESForm::LookupByID<RE::TESObjectREFR>(scanned->linkedDoor);
		const auto origin = a_player->GetPosition();
		// A teleport between scans leaves the last scan's door far behind: attached, it was let go
		// at once as walked away from, and attached again, every frame until the next scan.
		if (Reach(origin, door, linked) > ActivateRadius()) {
			return std::nullopt;
		}
		return Candidate{ door, scanned->linkedDoor, cell, origin.GetDistance(door->GetPosition()), Rank(origin, door, linked) };
	}

	void ScanNow()
	{
		scanTimer = 0.0f;
	}

	std::optional<DoorPair> DoorPairAt(float a_radius)
	{
		const auto portal = Manager::CurrentPortal();
		const auto nearby = NearestDoorPair(a_radius);
		if (!portal) {
			return nearby;
		}
		auto*      door = RE::TESForm::LookupByID<RE::TESObjectREFR>(portal->sourceDoor);
		auto*      nearest = nearby ? RE::TESForm::LookupByID<RE::TESObjectREFR>(nearby->sourceDoor) : nullptr;
		const auto origin = RE::PlayerCharacter::GetSingleton()->GetPosition();
		if (nearest && (!door || origin.GetDistance(nearest->GetPosition()) < origin.GetDistance(door->GetPosition()))) {
			return nearby;
		}
		return DoorPair{ portal->sourceDoor, portal->linkedDoor };
	}

	const WarmTarget* TargetOf(RE::FormID a_cell)
	{
		const auto target = std::ranges::find(warmTargets, a_cell, &WarmTarget::cell);
		return target != warmTargets.end() ? &*target : nullptr;
	}

	std::size_t TargetCount()
	{
		return warmTargets.size();
	}

	bool Sparse()
	{
		return sparse;
	}

	float WarmReach()
	{
		return warmReach;
	}

	void AttachRetry::Failed(RE::FormID a_cell)
	{
		cell = a_cell;
		cooldown = kRetryCooldown;
	}

	void UpdateWarm(RE::TESObjectCELL* a_here, float a_delta, WarmContext a_context)
	{
		if (shownRoomsSetting <= 1 || a_here->IsInteriorCell()) {
			if (!warm.empty()) {
				ReleaseWarm(a_here->IsInteriorCell() ? "player is indoors" : "none kept");
			}
			return;
		}
		// Fewer kept than before (changed in the menu): the farthest go first.
		while (std::cmp_greater_equal(warm.size(), shownRoomsSetting)) {
			LetGoKept(std::ranges::max_element(warm, {}, [](const Portal::RemoteCell& a_space) { return WarmRankOf(a_space.Cell()->GetFormID()); }),
				"fewer kept");
		}
		for (auto it = warm.begin(); it != warm.end();) {
			if (!it->Healthy()) {
				it = LetGoKept(it, "no longer attached under its scene");
			} else if (WarmDistanceOf(it->Cell()->GetFormID()) > warmReach + kWarmMargin) {
				it = LetGoKept(it, "out of reach");
			} else {
				// Merged beside the shown one and seen, it is drawn; any other kept room is out of sight.
				it->SetPaused(MergedView::KeptRoomPaused(it->Cell()));
				it->Update(a_delta);
				++it;
			}
		}
		const auto kept = [](RE::FormID a_cell) {
			return std::ranges::any_of(warm, [&](const Portal::RemoteCell& a_space) { return a_space.Cell()->GetFormID() == a_cell; });
		};
		const auto wanted = WarmWanted(warmReach);
		const auto next = std::ranges::find_if(wanted, [&](RE::FormID a_cell) { return !kept(a_cell) && !a_context.retry.Blocks(a_cell); });
		if (next == wanted.end() || (warmAttachTimer -= a_delta) > 0.0f || a_context.attachHeld) {
			return;
		}
		// The space shown, or about to be, loads first.
		if (a_context.shownLoading) {
			return;
		}
		auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(*next);
		if (!cell) {
			return;
		}
		// No room: the farthest kept room makes way, only for a door clearly nearer than its own.
		// Running along a street the order changes all the time; let go for every change, rooms
		// were attached and let go again a fifth of a second later.
		if (std::cmp_greater_equal(warm.size() + 1, shownRoomsSetting)) {
			const auto farthest = std::ranges::max_element(warm, {}, [](const Portal::RemoteCell& a_space) { return WarmRankOf(a_space.Cell()->GetFormID()); });
			if (farthest == warm.end() || WarmRankOf(*next) + kSwitchMargin >= WarmRankOf(farthest->Cell()->GetFormID())) {
				return;
			}
			LetGoKept(farthest, "a nearer door");
		}
		warmAttachTimer = kWarmAttachSeconds;
		const auto started = std::chrono::steady_clock::now();
		RemoteCell space;
		bool       attachedKept = false;
		{
			const Diag::Scope scope("attach kept");
			attachedKept = space.Attach(cell, false, false);
		}
		if (attachedKept) {
			logger::info("portal: attached {} out of sight behind its door ({:.1f} ms)", Probe::DescribeCell(cell),
				std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count());
			warm.push_back(std::move(space));
		} else {
			a_context.retry.Failed(*next);
			logger::info("portal: could not attach {} out of sight", Probe::DescribeCell(cell));
		}
	}

	std::vector<RemoteCell>& Kept()
	{
		return warm;
	}

	RemoteCell* KeptSpace(const RE::TESObjectCELL* a_cell)
	{
		const auto it = FindKept(a_cell);
		return it != warm.end() ? &*it : nullptr;
	}

	std::vector<RemoteCell>::iterator FindKept(const RE::TESObjectCELL* a_cell)
	{
		return std::ranges::find(warm, a_cell, &RemoteCell::Cell);
	}

	std::vector<RemoteCell>::iterator LetGoKept(std::vector<RemoteCell>::iterator a_space, std::string_view a_why)
	{
		MergedView::HideBesideOf(a_space->Cell(), a_why);
		logger::info("portal: let go of {}, kept behind its door ({})", Probe::DescribeCell(a_space->Cell()), a_why);
		a_space->Release();
		return warm.erase(a_space);
	}

	void ReleaseWarm(std::string_view a_reason)
	{
		while (!warm.empty()) {
			LetGoKept(warm.begin(), a_reason);
		}
	}
}
