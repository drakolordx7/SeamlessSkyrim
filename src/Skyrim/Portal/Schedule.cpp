#include "Skyrim/Portal/Schedule.h"

#include "Skyrim/Diag/Probe.h"
#include "Skyrim/Diag/Telemetry.h"
#include "Skyrim/Doors/DoorUse.h"
#include "Skyrim/Doors/Policy.h"
#include "Skyrim/Doors/Settings.h"
#include "Skyrim/Engine/CoreConvert.h"
#include "Skyrim/Graph/DoorScan.h"
#include "Skyrim/Pictures/PictureSize.h"
#include "Skyrim/Pictures/Tiers.h"
#include "Skyrim/Plugin/Settings.h"
#include "Skyrim/Portal/Ahead.h"
#include "Skyrim/Portal/Crossing.h"
#include "Skyrim/Doors/Hold.h"
#include "Skyrim/Portal/MergedView.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/Selection.h"

namespace Portal::Schedule
{
	using namespace Core::Graph;
	using Plugin::Settings::Id;

	namespace
	{
		std::unique_ptr<TierScheduler> scheduler;

		int loadedRooms = Plugin::Settings::Of(Id::kLoadedRooms).def;
		int memoryBudgetMb = Plugin::Settings::Of(Id::kMemoryBudgetMb).def;
		int targetFrameRate = Plugin::Settings::Of(Id::kTargetFrameRate).def;

		std::vector<Spaces::RemoteCell> kept;
		// The doors (in the player's space) whose pictures the scheduler has asked Pictures for.
		std::vector<RE::FormID> pictureDoors;
		// The last thing decided about a space (by id) and why a door fell back: for DescribeDoor.
		std::unordered_map<std::string, std::string> lastDecision;
		std::unordered_map<RE::FormID, std::string>  fallbackWhy;

		std::optional<Candidate>  live;
		std::optional<Approached> approached;
		SpaceIx                   playerSpace = kNoSpace;

		// Where the player was, for their speed.
		std::optional<RE::NiPoint3> lastPosition;
		Core::Vec3                  velocity;

		// The state of the doors around the player, looked at a few times a second: a door's lock, its
		// opening and the menu's switches don't change faster, and looking costs a lookup each.
		constexpr float kStateSeconds = 0.2f;
		// Doors further than this hold nothing worth the lookup (the reach of a gate counts a quarter).
		constexpr float kStateReach = 4500.0f;
		float                stateTimer = 0.0f;
		SpaceIx              stateSpace = kNoSpace;
		std::vector<DoorIx>  openDoors;
		std::vector<DoorIx>  usedDoors;
		std::vector<DoorIx>  blockedDoors;

		// The main thread's work in a frame from the player update to Present, smoothed (FrameEnded).
		std::optional<std::chrono::steady_clock::time_point> frameStart;
		float                                                busyMs = -1.0f;
		float                                                headroomMs = 0.0f;
		bool                                                 frozen = false;

		SchedulerConfig MakeConfig()
		{
			SchedulerConfig config;
			config.maxLive = 1;  // the manager shows one portal
			config.maxLoaded = static_cast<std::uint32_t>(loadedRooms);
			config.liveReach = Selection::DoorOpenDistance();
			config.defaultCost = SpaceCost::ForPictures(Pictures::PictureHeight(), Pictures::DepthLayers());
			return config;
		}

		// In range of the graph the scheduler was made with; a space added since isn't there until Flush.
		SpaceIx KnownSpace(RE::TESObjectCELL* a_cell)
		{
			const auto space = Graph::FindSpaceOf(a_cell);
			return space < Graph::Get().SpaceCount() ? space : kNoSpace;
		}

		RE::TESObjectCELL* CellFor(SpaceIx a_space, RE::TESObjectREFR* a_linked)
		{
			if (!Graph::IsWorld(a_space)) {
				return RE::TESForm::LookupByID<RE::TESObjectCELL>(Graph::SpaceForm(a_space));
			}
			if (auto* cell = a_linked->GetParentCell()) {
				return cell;
			}
			// A persistent street door whose cell isn't loaded: read the cell it stands in. Only for the
			// door the scheduler holds, never for every door in range.
			auto* world = a_linked->GetWorldspace();
			SEAMLESS_ASSERT(world);
			const auto pos = a_linked->GetPosition();
			return world->LoadCell(static_cast<std::int16_t>(std::floor(pos.x / 4096.0f)), static_cast<std::int16_t>(std::floor(pos.y / 4096.0f)));
		}

		void Rebound(const SpaceGraph& a_old, const SpaceGraph& a_new)
		{
			scheduler->Rebind(a_old, a_new);
			stateSpace = kNoSpace;  // door indices are the same, but the doorways under them are not
		}

		void RefreshDoorStates(const Core::Vec3& a_position)
		{
			openDoors.clear();
			usedDoors.clear();
			blockedDoors.clear();
			const auto& graph = Graph::Get();
			const float openReach = Selection::DoorOpenDistance() * 4.0f;  // a gate counts a quarter
			const bool  forceNone = Portal::DoorUse::GetForce() == Portal::DoorUse::Force::kNone;
			for (const auto& hit : graph.DoorsWithin(playerSpace, a_position, kStateReach)) {
				const auto form = Graph::DoorForm(hit.door);
				auto*      ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(form);
				auto*      linked = ref ? Portal::Doors::LinkedOf(ref) : nullptr;
				// What PortalLink says of a door: it has to be there, enabled, with its other end loaded, unlocked and not switched off.
				const bool usable = ref && linked && ref->Get3D() && !ref->IsDisabled() && !Portal::Doors::LockedShut(ref) && !Portal::Doors::PortalOff(form) &&
				                    !Portal::Doors::PortalOff(linked->GetFormID());
				if (!usable) {
					blockedDoors.push_back(hit.door);
					continue;
				}
				if (hit.distance <= openReach &&
					(Portal::Doors::Wanted(ref, Portal::Doors::Side(linked)) ||
						// Set to stand open always: open for the scheduler before its space is shown, which Wanted waits for.
						(forceNone && Portal::Doors::AlwaysOpen(Portal::Doors::CorrectionDoor(ref, linked)->GetFormID())))) {
					openDoors.push_back(hit.door);
					if (Portal::DoorUse::Used(form)) {
						usedDoors.push_back(hit.door);
					}
				}
			}
		}

		void CancelPictures(RE::FormID a_door)
		{
			Pictures::Cancel(a_door);
			std::erase(pictureDoors, a_door);
		}

		// A space's tier changed: the pictures of the doorways leading to it follow. T1 asks for the
		// image, T2 and up for the depth image too; below T1 nothing is asked for. A door whose space is
		// not the player's (it was let go as the player moved on) only has its requests dropped.
		void DrivePictures(const TierChange& a_change)
		{
			const auto& graph = Graph::Get();
			for (const auto wayIx : graph.DoorwaysOf(a_change.space)) {
				const auto& way = graph.GetDoorway(wayIx);
				if (a_change.to < Tier::Image) {
					for (const auto end : { way.a, way.b }) {
						if (end != kNoDoor) {
							CancelPictures(Graph::DoorForm(end));
						}
					}
					continue;
				}
				const auto doorIx = way.Other(a_change.space) == playerSpace ? way.DoorIn(playerSpace) : kNoDoor;
				if (doorIx == kNoDoor) {
					continue;
				}
				const Pictures::Doorway doorway{ Graph::DoorForm(doorIx), Graph::IsWorld(a_change.space), way.kind == DoorKind::Gate };
				if (a_change.to < Tier::DepthImage && a_change.from >= Tier::DepthImage) {
					// Back to the image alone: the depth image's views are no longer wanted.
					CancelPictures(doorway.door);
				}
				if (!std::ranges::contains(pictureDoors, doorway.door)) {
					pictureDoors.push_back(doorway.door);
				}
				Pictures::Request(doorway, Pictures::Tier::kImage);
				if (a_change.to >= Tier::DepthImage) {
					Pictures::Request(doorway, Pictures::Tier::kDepthImage);
				}
			}
		}

		void LetGo(std::vector<Spaces::RemoteCell>::iterator a_space, std::string_view a_why)
		{
			logger::info("schedule: let go of {}, kept loaded ({})", Probe::DescribeCell(a_space->Cell()), a_why);
			a_space->Release();
			kept.erase(a_space);
		}

		void AttachKept(const TierChange& a_change, RE::TESObjectCELL* a_here)
		{
			const auto& graph = Graph::Get();
			const auto  otherDoor = graph.GetDoorway(a_change.via).DoorIn(a_change.space);
			auto*       linked = otherDoor != kNoDoor ? RE::TESForm::LookupByID<RE::TESObjectREFR>(Graph::DoorForm(otherDoor)) : nullptr;
			auto*       cell = linked ? CellFor(a_change.space, linked) : nullptr;
			if (!cell) {
				scheduler->Failed(a_change.space, a_change.from);
				lastDecision[graph.GetSpace(a_change.space).id] = "could not keep loaded: no cell for the door behind it";
				logger::info("schedule: no cell to keep loaded for {}", graph.GetSpace(a_change.space).id);
				return;
			}
			if (Manager::Space().Cell() == cell || FindKept(cell) != kept.end()) {
				return;
			}
			const auto started = std::chrono::steady_clock::now();
			Spaces::RemoteCell space;
			bool               attached = false;
			{
				const Diag::Scope scope("attach kept");
				// A space attached only for its pictures is let go first: the cell is the scheduler's now.
				Ahead::LetGo(cell, "the scheduler keeps it");
				attached = space.Attach(cell, MergedView::Mergeable(a_here, cell), false);
			}
			if (attached) {
				logger::info("schedule: attached {} out of sight behind its door ({:.1f} ms)", Probe::DescribeCell(cell),
					std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count());
				kept.push_back(std::move(space));
			} else {
				scheduler->Failed(a_change.space, a_change.from);
				lastDecision[graph.GetSpace(a_change.space).id] = "could not keep loaded: the cell would not attach";
				logger::info("schedule: could not attach {} out of sight", Probe::DescribeCell(cell));
			}
		}

		void Apply(const std::vector<TierChange>& a_changes, RE::TESObjectCELL* a_here)
		{
			const auto& graph = Graph::Get();
			static constexpr const char* reasons[] = { "entered", "approach", "retreat", "door closed", "left reach", "budget", "headroom" };
			for (const auto& change : a_changes) {
				lastDecision[graph.GetSpace(change.space).id] = std::format("{} -> {} ({})", Name(change.from), Name(change.to), reasons[static_cast<int>(change.reason)]);
				// Pictures come and go with the tier (DrivePictures); only the ones that hold a cell are logged.
				if (change.from >= Tier::LoadedPaused || change.to >= Tier::LoadedPaused) {
					logger::info("schedule: {} {} -> {} ({})", graph.GetSpace(change.space).id, Name(change.from), Name(change.to), reasons[static_cast<int>(change.reason)]);
				}
				if (change.space == playerSpace) {
					continue;
				}
				DrivePictures(change);
				if (change.to < Tier::LoadedPaused) {
					// The shown portal's space is let go by the manager, once its door has shut.
					for (auto it = kept.begin(); it != kept.end(); ++it) {
						if (Graph::FindSpaceOf(it->Cell()) == change.space) {
							LetGo(it, "scheduler");
							break;
						}
					}
				} else if (change.to == Tier::LoadedPaused && change.from < Tier::LoadedPaused) {
					AttachKept(change, a_here);
				}
			}
		}

		// Kept spaces: paused, and let go when they stopped being attached under their scene.
		void UpdateKept(float a_delta)
		{
			for (auto it = kept.begin(); it != kept.end();) {
				if (!it->Healthy()) {
					const auto space = KnownSpace(it->Cell());
					LetGo(it, "no longer attached under its scene");
					if (space != kNoSpace) {
						scheduler->Failed(space, Tier::Cold);
					}
					it = kept.begin();
					continue;
				}
				it->SetPaused(true);
				it->Update(a_delta);
				++it;
			}
		}

		void FindLive(RE::PlayerCharacter* a_player)
		{
			live.reset();
			const auto& graph = Graph::Get();
			const auto  origin = a_player->GetPosition();
			for (const auto wayIx : graph.DoorwaysOf(playerSpace)) {
				const auto& way = graph.GetDoorway(wayIx);
				const auto  other = way.Other(playerSpace);
				if (other == kNoSpace || other == playerSpace || scheduler->TierOf(other) != Tier::Live) {
					continue;
				}
				const auto doorIx = way.DoorIn(playerSpace);
				const auto otherIx = way.DoorIn(other);
				if (doorIx == kNoDoor || otherIx == kNoDoor) {
					continue;
				}
				auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(Graph::DoorForm(doorIx));
				auto* linked = RE::TESForm::LookupByID<RE::TESObjectREFR>(Graph::DoorForm(otherIx));
				if (!door || !linked || !door->Get3D()) {
					continue;
				}
				const float rank = Selection::Rank(origin, door, linked);
				// Of several doors into the space (an inn's front and kitchen door, Jorrvaskr's four), the one
				// that is open is the doorway: the crossing is prepared for the door the player uses.
				const bool open = std::ranges::contains(openDoors, doorIx) || std::ranges::contains(openDoors, otherIx);
				if (live && (live->open != open ? live->open : rank >= live->rank)) {
					continue;
				}
				auto* cell = CellFor(other, linked);
				if (!cell) {
					continue;
				}
				live = Candidate{ door, linked->GetFormID(), cell, origin.GetDistance(door->GetPosition()), rank, open };
			}
		}

		void FindApproached(const Core::Vec3& a_position)
		{
			approached.reset();
			const auto& graph = Graph::Get();
			const float reachLimit = Selection::DoorOpenDistance();
			float       best = reachLimit;
			for (const auto doorIx : openDoors) {
				const auto& door = graph.GetDoor(doorIx);
				const auto& way = graph.GetDoorway(door.doorway);
				const auto  other = way.Other(playerSpace);
				const auto  otherIx = way.DoorIn(other);
				if (other == kNoSpace || other == playerSpace || otherIx == kNoDoor) {
					continue;
				}
				const float reach = Core::Selection::Reach((door.pos - a_position).Length(), way.kind == DoorKind::Gate);
				if (reach <= best) {
					best = reach;
					approached = Approached{ { door.doorway, doorIx, playerSpace, other }, Graph::DoorForm(doorIx), Graph::DoorForm(otherIx) };
				}
			}
		}
	}

	void Install()
	{
		loadedRooms = Plugin::Settings::Int(Id::kLoadedRooms);
		memoryBudgetMb = Plugin::Settings::Int(Id::kMemoryBudgetMb);
		targetFrameRate = Plugin::Settings::Int(Id::kTargetFrameRate);
		scheduler = std::make_unique<TierScheduler>(Graph::Get(), MakeConfig());
		Graph::SetOnRebuilt(Rebound);
		Diag::frameEndHook = FrameEnded;
	}

	int LoadedRooms()
	{
		return loadedRooms;
	}

	void SetLoadedRooms(int a_count)
	{
		const auto& range = Plugin::Settings::Of(Id::kLoadedRooms);
		loadedRooms = std::clamp(a_count, range.min, range.max);
	}

	int MemoryBudgetMb()
	{
		return memoryBudgetMb;
	}

	void SetMemoryBudgetMb(int a_mb)
	{
		const auto& range = Plugin::Settings::Of(Id::kMemoryBudgetMb);
		memoryBudgetMb = std::clamp(a_mb, range.min, range.max);
	}

	int TargetFrameRate()
	{
		return targetFrameRate;
	}

	void SetTargetFrameRate(int a_fps)
	{
		const auto& range = Plugin::Settings::Of(Id::kTargetFrameRate);
		targetFrameRate = std::clamp(a_fps, range.min, range.max);
	}

	void Update(RE::PlayerCharacter* a_player, RE::TESObjectCELL* a_here, float a_delta)
	{
		frameStart = std::chrono::steady_clock::now();
		Graph::Discover(a_here, a_delta);
		playerSpace = Graph::SpaceOf(a_here);
		Graph::Flush();

		const auto position = a_player->GetPosition();
		if (lastPosition && a_delta > 0.0f) {
			const auto step = position - *lastPosition;
			// A step quicker than anyone can move is a teleport or a crossing, no speed (a short teleport, like
			// the crawler's move_to, looked like running at the door and loaded rooms for it).
			if (step.Length() > 500.0f || step.Length() > Core::Selection::kMaxSpeed * a_delta) {
				velocity = Core::Vec3();
			} else {
				velocity = velocity + (ToCore(step) / a_delta - velocity) * std::min(1.0f, 6.0f * a_delta);
			}
		}
		lastPosition = position;
		const auto here = ToCore(position);

		UpdateKept(a_delta);

		// Nothing is attached while a crossing or the engine's own door trip is under way, nor while the
		// shown space still loads: it loads first.
		const auto& shown = Manager::Space();
		frozen = Crossing::AttachHeld() || (Manager::CurrentPortal() && !(shown.Healthy() && shown.FullyLoaded()));
		if (!frozen) {
			stateTimer -= a_delta;
			if (stateTimer <= 0.0f || stateSpace != playerSpace) {
				stateTimer = kStateSeconds;
				stateSpace = playerSpace;
				RefreshDoorStates(here);
			}
			const float targetMs = 1000.0f / static_cast<float>(targetFrameRate);
			headroomMs = busyMs < 0.0f ? targetMs * 0.5f : targetMs - busyMs;
			scheduler->Configure(MakeConfig());
			const Inputs inputs{
				.dt = a_delta,
				.playerSpace = playerSpace,
				.position = here,
				.heading = Selection::Heading(),
				.velocity = velocity,
				.openDoors = openDoors,
				.usedDoors = usedDoors,
				.blockedDoors = blockedDoors,
				.headroomMs = headroomMs,
				.memoryBudgetMb = static_cast<float>(memoryBudgetMb),
			};
			Apply(scheduler->Tick(inputs), a_here);
		}
		if (!frozen) {
			Ahead::Update(a_here, a_delta);
		}
		FindLive(a_player);
		FindApproached(here);
	}

	const std::optional<Candidate>& LiveCandidate()
	{
		return live;
	}

	Tier TierOf(RE::TESObjectCELL* a_cell)
	{
		const auto space = KnownSpace(a_cell);
		return space != kNoSpace ? scheduler->TierOf(space) : Tier::Cold;
	}

	const std::optional<Approached>& ApproachedDoorway()
	{
		return approached;
	}

	Tier DoorTier(RE::FormID a_door)
	{
		const auto ix = Graph::DoorOf(a_door);
		if (ix == kNoDoor || ix >= Graph::Get().DoorCount()) {
			return Tier::Image;
		}
		const auto& graph = Graph::Get();
		const auto& door = graph.GetDoor(ix);
		// A door of a space merged into the player's, seen through another: no tier of its own here.
		if (door.space != playerSpace) {
			return Tier::Image;
		}
		const auto other = graph.GetDoorway(door.doorway).Other(door.space);
		return other != kNoSpace ? scheduler->TierOf(other) : Tier::Cold;
	}

	void Fallback(RE::FormID a_door, std::string_view a_reason)
	{
		const auto ix = Graph::DoorOf(a_door);
		if (ix == kNoDoor || ix >= Graph::Get().DoorCount()) {
			return;
		}
		scheduler->Fallback(Graph::Get().GetDoor(ix).doorway);
		fallbackWhy[a_door] = std::string(a_reason);
		logger::warn("schedule: door {:08X} keeps the engine's own behaviour from now on: {}", a_door, a_reason);
	}

	bool IsFallback(RE::FormID a_door)
	{
		const auto ix = Graph::DoorOf(a_door);
		return ix != kNoDoor && ix < Graph::Get().DoorCount() && scheduler->IsFallback(Graph::Get().GetDoor(ix).doorway);
	}

	void AttachFailed(RE::TESObjectCELL* a_cell)
	{
		if (const auto space = KnownSpace(a_cell); space != kNoSpace) {
			scheduler->Failed(space, Tier::DepthImage);
			lastDecision[Graph::Get().GetSpace(space).id] = "could not be shown: the attach failed";
		}
	}

	void ReleaseAll(std::string_view a_reason)
	{
		while (!kept.empty()) {
			LetGo(kept.begin(), a_reason);
		}
		while (!pictureDoors.empty()) {
			CancelPictures(pictureDoors.back());
		}
		Ahead::ReleaseAll(a_reason);
		scheduler->Reset();
		live.reset();
	}

	std::vector<Spaces::RemoteCell>& Kept()
	{
		return kept;
	}

	std::vector<Spaces::RemoteCell>::iterator FindKept(const RE::TESObjectCELL* a_cell)
	{
		return std::ranges::find(kept, a_cell, &Spaces::RemoteCell::Cell);
	}

	bool TakeKept(const RE::TESObjectCELL* a_cell, Spaces::RemoteCell& a_into)
	{
		const auto it = FindKept(a_cell);
		if (it == kept.end()) {
			return false;
		}
		std::swap(a_into, *it);
		kept.erase(it);
		return true;
	}

	void LetGoKept(const RE::TESObjectCELL* a_cell, std::string_view a_why)
	{
		if (const auto it = FindKept(a_cell); it != kept.end()) {
			LetGo(it, a_why);
		}
	}

	std::optional<AheadTarget> AheadTargetOf(RE::FormID a_door)
	{
		const auto& graph = Graph::Get();
		const auto  ix = Graph::DoorOf(a_door);
		if (ix == kNoDoor || ix >= graph.DoorCount()) {
			return std::nullopt;
		}
		const auto& door = graph.GetDoor(ix);
		if (door.space != playerSpace) {
			return std::nullopt;
		}
		const auto& way = graph.GetDoorway(door.doorway);
		const auto  other = way.Other(door.space);
		if (other == kNoSpace || other == door.space || other >= graph.SpaceCount() || scheduler->IsFallback(door.doorway)) {
			return std::nullopt;
		}
		const auto linkedIx = way.DoorIn(other);
		if (linkedIx == kNoDoor) {
			return std::nullopt;
		}
		AheadTarget target{ { a_door, Graph::IsWorld(other), way.kind == DoorKind::Gate }, Graph::DoorForm(linkedIx), other, scheduler->TierOf(other), scheduler->RankOf(other), nullptr, nullptr };
		target.interior = Graph::IsWorld(other) ? nullptr : RE::TESForm::LookupByID<RE::TESObjectCELL>(Graph::SpaceForm(other));
		for (auto& space : kept) {
			if (Graph::FindSpaceOf(space.Cell()) == other) {
				target.kept = &space;
				break;
			}
		}
		return target;
	}

	AheadConfig AheadBudget()
	{
		return MakeConfig().ahead;
	}

	float HeadroomMs()
	{
		return headroomMs;
	}

	bool Frozen()
	{
		return frozen;
	}

	float FreeMb()
	{
		return static_cast<float>(memoryBudgetMb) - scheduler->MemoryMb();
	}

	void FrameEnded()
	{
		if (!frameStart) {
			return;
		}
		const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - *frameStart).count();
		frameStart.reset();
		busyMs = busyMs < 0.0f ? ms : busyMs + (ms - busyMs) * 0.1f;
	}

	json DescribeDoor(RE::FormID a_door)
	{
		const auto& graph = Graph::Get();
		auto*       ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_door);
		auto*       linked = ref ? Portal::Doors::LinkedOf(ref) : nullptr;
		const bool  shown = Manager::SpaceShown(a_door);
		json        out{ { "door", std::format("{:08X}", a_door) }, { "shown", shown }, { "loaded", ref != nullptr }, { "inGraph", false } };
		const auto  say = [&](std::string a_reason) {
			out["reason"] = shown ? json(nullptr) : json(std::move(a_reason));
			return out;
		};
		if (const auto why = fallbackWhy.find(a_door); why != fallbackWhy.end()) {
			out["fallback"] = why->second;
		}
		const auto ix = Graph::DoorOf(a_door);
		if (ix == kNoDoor || ix >= graph.DoorCount()) {
			return say(ref ? "not in the door graph: a marker door, not a teleport door, or its cell has not been looked over yet" : "not a loaded reference");
		}
		const auto& door = graph.GetDoor(ix);
		const auto& way = graph.GetDoorway(door.doorway);
		const auto  other = way.Other(door.space);
		const bool  otherKnown = other != kNoSpace && other < graph.SpaceCount();
		const auto  otherName = otherKnown ? graph.GetSpace(other).id : std::string();
		out["inGraph"] = true;
		out["space"] = graph.GetSpace(door.space).id;
		out["leadsTo"] = otherName;
		out["link"] = way.link == Link::Paired ? "paired" : way.link == Link::OneWay ? "one-way" : "broken";
		out["gate"] = way.kind == DoorKind::Gate;
		out["doorwayFallback"] = scheduler->IsFallback(door.doorway);
		if (otherKnown) {
			out["tier"] = Name(scheduler->TierOf(other));
			if (const auto decision = lastDecision.find(otherName); decision != lastDecision.end()) {
				out["lastDecision"] = decision->second;
			}
		}
		if (scheduler->IsFallback(door.doorway)) {
			const auto why = fallbackWhy.find(a_door);
			return say(std::format("fell back to the engine's own door: {}", why != fallbackWhy.end() ? why->second : std::string("set through the other door of the doorway")));
		}
		if (door.space != playerSpace) {
			return say(std::format("the door is not in the player's space ({} is)", playerSpace < graph.SpaceCount() ? graph.GetSpace(playerSpace).id : std::string("?")));
		}
		if (!ref || !linked) {
			return say(!ref ? "the door reference is not loaded" : "the door at the other end is not loaded");
		}
		if (!ref->Get3D() || ref->IsDisabled()) {
			return say(!ref->Get3D() ? "the door has no 3D (not loaded)" : "the door is disabled");
		}
		if (Portal::Doors::LockedShut(ref)) {
			return say("the door is locked");
		}
		if (Portal::Doors::PortalOff(a_door) || Portal::Doors::PortalOff(linked->GetFormID())) {
			return say("the portal is switched off for this door (menu)");
		}
		if (!otherKnown) {
			return say("no space is known behind the door (broken link)");
		}
		if (way.link != Link::Paired) {
			return say("the doorway is not paired: the door behind it is not in the graph");
		}
		const bool  wanted = Portal::Doors::Wanted(ref, Portal::Doors::Side(linked));
		const auto  origin = ToCore(RE::PlayerCharacter::GetSingleton()->GetPosition());
		const float reach = Core::Selection::Reach((door.pos - origin).Length(), way.kind == DoorKind::Gate);
		out["open"] = wanted;
		out["reach"] = reach;
		out["liveReach"] = Selection::DoorOpenDistance();
		const auto tier = scheduler->TierOf(other);
		if (tier < Tier::Live) {
			std::string cause = std::format("the scheduler holds the space at {}", Name(tier));
			if (!wanted) {
				cause += ", and the door is not open (nothing is simulated behind a shut door until it is used)";
			} else if (reach > Selection::DoorOpenDistance()) {
				cause += std::format(", the door is open but {:.0f} away in reach units (live within {:.0f})", reach, Selection::DoorOpenDistance());
			} else if (const auto decision = lastDecision.find(otherName); decision != lastDecision.end()) {
				cause += ", last decision: " + decision->second;
			}
			if (headroomMs < 3.0f) {
				cause += std::format("; frame headroom {:.1f} ms (a live room needs 3)", headroomMs);
			}
			if (frozen) {
				cause += "; the scheduler is paused (crossing, or the shown space is loading)";
			}
			return say(std::move(cause));
		}
		return say(live && live->door->GetFormID() == a_door ? "the scheduler holds it live; the manager has not shown it yet (the space is attaching, or another portal is being let go)" :
		                                                      "the scheduler holds the space behind another door of the doorway live");
	}

	json Status()
	{
		const auto& graph = Graph::Get();
		json        tiers = json::array();
		std::size_t fallbacks = 0;
		for (SpaceIx space = 0; space < graph.SpaceCount(); ++space) {
			if (space != playerSpace && scheduler->TierOf(space) != Tier::Cold) {
				tiers.push_back({ { "space", graph.GetSpace(space).id }, { "tier", Name(scheduler->TierOf(space)) } });
			}
		}
		for (DoorwayIx way = 0; way < graph.DoorwayCount(); ++way) {
			fallbacks += scheduler->IsFallback(way);
		}
		json keptList = json::array();
		for (const auto& space : kept) {
			keptList.push_back({ { "cell", Probe::DescribeCell(space.Cell()) }, { "healthy", space.Healthy() }, { "fullyLoaded", space.FullyLoaded() },
				{ "lights", space.AdoptedLights() }, { "paused", space.Paused() }, { "actors", space.DescribeActors() } });
		}
		return {
			{ "playerSpace", playerSpace != kNoSpace && playerSpace < graph.SpaceCount() ? graph.GetSpace(playerSpace).id : "" },
			{ "config", { { "loadedRooms", loadedRooms }, { "memoryBudgetMb", memoryBudgetMb }, { "targetFrameRate", targetFrameRate }, { "liveReach", Selection::DoorOpenDistance() } } },
			{ "frozen", frozen },
			{ "headroomMs", headroomMs },
			{ "busyMs", busyMs },
			{ "memoryMb", scheduler->MemoryMb() },
			{ "tiers", tiers },
			{ "kept", keptList },
			{ "live", live ? json{ { "door", std::format("{:08X}", live->door->GetFormID()) }, { "linkedDoor", std::format("{:08X}", live->linkedDoor) }, { "cell", Probe::DescribeCell(live->cell) } } : json(nullptr) },
			{ "approached", approached ? json{ { "door", std::format("{:08X}", approached->door) }, { "linkedDoor", std::format("{:08X}", approached->linkedDoor) } } : json(nullptr) },
			{ "openDoors", openDoors.size() },
			{ "blockedDoors", blockedDoors.size() },
			{ "fallbackDoorways", fallbacks },
			{ "ahead", Ahead::Status() },
		};
	}
}
