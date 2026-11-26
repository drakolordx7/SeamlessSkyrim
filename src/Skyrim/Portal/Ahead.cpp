#include "Skyrim/Portal/Ahead.h"

#include "Skyrim/Diag/Probe.h"
#include "Skyrim/Doors/Pair.h"
#include "Skyrim/Graph/DoorScan.h"
#include "Skyrim/Pictures/Cache.h"
#include "Skyrim/Pictures/Tiers.h"
#include "Skyrim/Portal/LightOcclusion.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/Schedule.h"
#include "Skyrim/Render/PortalPass.h"
#include "Skyrim/Spaces/RemoteCell.h"

namespace Portal::Ahead
{
	namespace
	{
		using namespace Core::Graph;
		using CellState = RE::TESObjectCELL::CellState;

		// An interior of more references than this isn't cheap to attach for a picture (a hall, a
		// castle): its pictures wait for the scheduler to hold it, or for a visit. A cell never read has
		// none counted: its first attach may be dearer than it looks.
		constexpr std::size_t kBriefMaxReferences = 600;
		// Longest a space is held for its pictures; one that doesn't finish is let go and left alone for a while.
		constexpr float kBriefSeconds = 25.0f;
		constexpr float kSulkSeconds = 120.0f;

		// A space attached for its pictures, one at a time.
		struct Brief
		{
			Spaces::RemoteCell space;
			float              seconds = 0.0f;
		};
		std::optional<Brief> brief;

		// Cells that would not attach or finish: not tried again for a while.
		struct Sulk
		{
			RE::FormID cell;
			float      seconds;
		};
		std::vector<Sulk> sulking;

		// What is asked for this frame, as the scheduler asked it (Pictures::PendingRequests), one per
		// doorway tier; in step with spaces.
		struct Entry
		{
			Pictures::Doorway doorway;
			SpaceIx           space;
		};
		std::vector<Entry>      entries;
		std::vector<AheadSpace> spaces;

		RE::FormID forced = 0;
		// A capture a frame, every other frame at most: the room is drawn from the doorway's camera
		// into a target the size of the picture, and the compression follows it.
		int frameGap = 0;

		std::uint32_t shots = 0, attaches = 0, released = 0, retried = 0;
		std::string   lastShot;

		bool Sulking(RE::FormID a_cell)
		{
			return std::ranges::find(sulking, a_cell, &Sulk::cell) != sulking.end();
		}

		void SulkCell(RE::TESObjectCELL* a_cell, float a_seconds = kSulkSeconds)
		{
			sulking.push_back({ a_cell->GetFormID(), a_seconds });
		}

		// Cheap enough to attach for a picture, and free to be attached (the engine's own rules for a cell
		// the player isn't in, as RemoteCell::Attachable).
		bool Attachable(RE::TESObjectCELL* a_cell)
		{
			if (!a_cell || Sulking(a_cell->GetFormID()) || Spaces::RemoteCell::Holds(a_cell)) {
				return false;
			}
			const auto state = a_cell->cellState.get();
			return a_cell != RE::PlayerCharacter::GetSingleton()->GetParentCell() && state != CellState::kAttached && state != CellState::kAttaching && state != CellState::kDetaching &&
			       a_cell->GetRuntimeData().references.size() <= kBriefMaxReferences;
		}

		bool IsShown(SpaceIx a_space)
		{
			auto* cell = Manager::Space().Cell();
			return Manager::CurrentPortal() && cell && Graph::FindSpaceOf(cell) == a_space;
		}

		Spaces::RemoteCell* SpaceOf(const Schedule::AheadTarget& a_target)
		{
			if (a_target.kept) {
				return a_target.kept;
			}
			return brief && Graph::FindSpaceOf(brief->space.Cell()) == a_target.space ? &brief->space : nullptr;
		}

		void Gather()
		{
			entries.clear();
			spaces.clear();
			for (const auto& pending : Pictures::PendingRequests()) {
				const auto target = Schedule::AheadTargetOf(pending.doorway.door);
				// The shown portal's own doorway is photographed by the pass that draws it.
				if (!target || IsShown(target->space)) {
					continue;
				}
				const auto* space = SpaceOf(*target);
				AheadSpace  entry;
				entry.rank = target->rank;
				entry.ready = space && space->Healthy() && space->FullyLoaded();
				entry.attachable = !space && target->tier < Tier::LoadedPaused && Attachable(target->interior);
				entries.push_back({ pending.doorway, target->space });
				spaces.push_back(entry);
			}
		}

		void Release(std::string_view a_why)
		{
			logger::info("ahead: let go of {}, held for its pictures ({}, {:.1f} s)", Probe::DescribeCell(brief->space.Cell()), a_why, brief->seconds);
			brief->space.Release();
			brief.reset();
			++released;
		}

		void StartAttach()
		{
			std::optional<std::size_t> pick;
			// Forced: this door's space, whatever the frame time and memory.
			for (std::size_t i = 0; forced && i < entries.size(); ++i) {
				if (entries[i].doorway.door == forced && spaces[i].attachable) {
					pick = i;
				}
			}
			if (!pick) {
				pick = NextAttach(spaces, false, Schedule::HeadroomMs(), Schedule::FreeMb(), Schedule::AheadBudget());
			}
			if (!pick) {
				return;
			}
			const auto target = Schedule::AheadTargetOf(entries[*pick].doorway.door);
			if (!target || !target->interior) {
				return;
			}
			Brief attempt;
			const auto started = std::chrono::steady_clock::now();
			if (!attempt.space.AttachForPicture(target->interior)) {
				SulkCell(target->interior);
				logger::info("ahead: could not attach {} for its pictures", Probe::DescribeCell(target->interior));
				return;
			}
			++attaches;
			logger::info("ahead: attached {} for the pictures of door {:08X} ({:.1f} ms)", Probe::DescribeCell(target->interior), entries[*pick].doorway.door,
				std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count());
			brief.emplace(std::move(attempt));
		}

		// Held for its pictures: kept loading, nothing else (RemoteCell::AttachForPicture); let go when
		// no door of the player's asks for it any more, or when it won't be done.
		void Tend(float a_delta)
		{
			auto&      space = brief->space;
			auto*      cell = space.Cell();
			brief->seconds += a_delta;
			if (!space.Healthy()) {
				const auto form = cell ? cell->GetFormID() : 0;
				Release("no longer attached under its scene");
				sulking.push_back({ form, kSulkSeconds });
				return;
			}
			space.SetPaused(true);
			space.Update(a_delta);
			const auto ix = Graph::FindSpaceOf(cell);
			const bool wanted = std::ranges::any_of(entries, [&](const Entry& a_entry) { return a_entry.space == ix; });
			if (!wanted && space.FullyLoaded()) {
				Release("its pictures are taken");
			} else if (brief->seconds > kBriefSeconds) {
				SulkCell(cell);
				Release("it did not finish in time");
			}
		}

		// The cells' lights' world for the pass, and none after: the shown space's is set again in the
		// next frame's update, before anything is drawn.
		class LightWorld
		{
		public:
			LightWorld(const Active& a_portal, RE::TESObjectCELL* a_cell)
			{
				std::optional<LightOcclusion::RoomDoor> roomDoor;
				if (const auto mapping = Doors::TryCompute(a_portal)) {
					roomDoor = LightOcclusion::RoomDoor{ mapping->toLinked * mapping->sourceCenter, mapping->toLinked.rotate * mapping->inward, mapping->halfWidth, mapping->halfHeight };
				}
				LightOcclusion::SetWorld(a_cell->GetbhkWorld(), {}, roomDoor, a_cell->IsInteriorCell());
			}
			~LightWorld() { LightOcclusion::SetWorld(nullptr); }
			LightWorld(const LightWorld&) = delete;
			LightWorld& operator=(const LightWorld&) = delete;
		};
	}

	void Update(RE::TESObjectCELL*, float a_delta)
	{
		for (auto& sulk : sulking) {
			sulk.seconds -= a_delta;
		}
		std::erase_if(sulking, [](const Sulk& a_sulk) { return a_sulk.seconds <= 0.0f; });
		Gather();
		if (brief) {
			Tend(a_delta);
		} else {
			StartAttach();
		}
	}

	bool DrawPass()
	{
		if (frameGap > 0) {
			--frameGap;
			return false;
		}
		if (spaces.empty() || Schedule::Frozen() || Render::PortalPass::CaptureDrawnThisFrame()) {
			return false;
		}
		auto* camera = RE::Main::WorldRootCamera();
		auto  candidates = spaces;
		// A few tries: a door whose space can't be drawn this frame leaves the next likeliest its turn.
		for (int attempt = 0; attempt < 4; ++attempt) {
			std::optional<std::size_t> pick;
			for (std::size_t i = 0; forced && i < entries.size(); ++i) {
				if (entries[i].doorway.door == forced && candidates[i].ready) {
					pick = i;
				}
			}
			if (!pick) {
				pick = NextShot(candidates, Schedule::HeadroomMs(), Schedule::AheadBudget());
			}
			if (!pick) {
				return false;
			}
			candidates[*pick].ready = false;
			const auto door = entries[*pick].doorway.door;
			try {
				// Resolved again: the manager may have taken a kept space since the update.
				const auto target = Schedule::AheadTargetOf(door);
				auto*      space = target ? SpaceOf(*target) : nullptr;
				if (!space || !space->Healthy() || !space->FullyLoaded() || IsShown(target->space)) {
					continue;
				}
				const auto request = Pictures::NextRequestedCamera(door, camera->world.translate);
				if (!request) {
					continue;
				}
				const Active active{ door, target->linkedDoor, space->Cell(), space->Generation() };
				const LightWorld world(active, space->Cell());
				if (!Render::PortalPass::DrawAhead(active, *request, space->Scene(), space->Cells())) {
					continue;
				}
				++shots;
				frameGap = 1;
				lastShot = std::format("{:08X} {} from {}{}", door, request->layer < 0 ? "image" : std::format("depth layer {}", request->layer), Probe::DescribeCell(space->Cell()),
					target->kept ? " (kept)" : " (attached for it)");
				return true;
			} catch (const std::exception& e) {
				static int reported = 0;
				if (reported++ < 10) {
					logger::warn("ahead: door {:08X}: {}", door, e.what());
				}
			}
		}
		return false;
	}

	void LetGo(const RE::TESObjectCELL* a_cell, std::string_view a_why)
	{
		if (brief && brief->space.Cell() == a_cell) {
			Release(a_why);
		}
	}

	void ReleaseAll(std::string_view a_reason)
	{
		if (brief) {
			Release(a_reason);
		}
		entries.clear();
		spaces.clear();
		forced = 0;
	}

	json Force(RE::FormID a_door, bool a_retake)
	{
		const auto target = Schedule::AheadTargetOf(a_door);
		if (!target) {
			throw std::runtime_error("not a door of the player's space with a paired door behind it (door_status says why)");
		}
		if (a_retake) {
			Pictures::Retake(a_door, target->doorway.outdoors);
		}
		const bool image = Pictures::Wanted(target->doorway, Pictures::Tier::kImage), depth = Pictures::Wanted(target->doorway, Pictures::Tier::kDepthImage);
		Pictures::Request(target->doorway, Pictures::Tier::kImage);
		Pictures::Request(target->doorway, Pictures::Tier::kDepthImage);
		forced = a_door;
		++retried;
		const auto cell = target->kept ? target->kept->Cell() : target->interior;
		return { { "door", std::format("{:08X}", a_door) }, { "space", cell ? json(Probe::DescribeCell(cell)) : json(nullptr) }, { "imageWanted", image }, { "depthImageWanted", depth },
			{ "from", target->kept ? "kept (T3)" : target->interior && !Sulking(target->interior->GetFormID()) ? "an attach for its pictures" : "nothing: the space is not attached and not cheap to attach (an exterior or a large interior): hold it by approaching" },
			{ "note", image || depth ? "taken as soon as the space is loaded, a picture every other frame; watch the log for 'ahead:' lines" : "the door has all its pictures: pass retake to take them again" } };
	}

	json Status()
	{
		json pending = json::array();
		for (std::size_t i = 0; i < entries.size(); ++i) {
			pending.push_back({ { "door", std::format("{:08X}", entries[i].doorway.door) }, { "rank", spaces[i].rank }, { "ready", spaces[i].ready }, { "attachable", spaces[i].attachable } });
		}
		return { { "brief", brief ? json{ { "cell", Probe::DescribeCell(brief->space.Cell()) }, { "seconds", brief->seconds }, { "loaded", brief->space.FullyLoaded() } } : json(nullptr) }, { "pending", pending },
			{ "shots", shots }, { "attaches", attaches }, { "released", released }, { "forced", forced ? json(std::format("{:08X}", forced)) : json(nullptr) }, { "lastShot", lastShot },
			{ "sulking", sulking.size() } };
	}
}
