#include "Skyrim/Graph/DoorScan.h"

#include "Skyrim/Engine/CoreConvert.h"

namespace Graph
{
	using namespace Core::Graph;

	namespace
	{
		struct State
		{
			std::vector<SpaceRecord>   spaces;
			std::vector<DoorRecord>    doors;
			std::vector<RE::FormID>    spaceForms;  // by SpaceIx: the cell (interior) or worldspace
			std::vector<bool>          spaceIsWorld;
			std::vector<RE::FormID>    doorForms;  // by DoorIx
			std::unordered_map<RE::FormID, SpaceIx> spaceByForm;
			std::unordered_map<RE::FormID, DoorIx>  doorByForm;

			std::unique_ptr<SpaceGraph> graph = std::make_unique<SpaceGraph>(SpaceGraph::Build({}, {}));
			RebuiltCallback             onRebuilt = nullptr;
			bool                        dirty = false;
			std::uint32_t               revision = 0;

			// How many references each cell had when it was last looked over.
			std::unordered_map<RE::FormID, std::size_t> cellRefs;
			float                                       timer = 0.0f;

			// What the numbers say.
			std::size_t formsScanned = 0;
			std::size_t persistentCells = 0;
			std::size_t doorsAtLoad = 0;
			std::size_t doorsFound = 0;  // after the data load
			std::size_t cellsLooked = 0;
			std::size_t lockedWhenFound = 0;
			float       loadMs = 0.0f;
		} state;

		constexpr float kDiscoverSeconds = 0.5f;

		std::string FormKey(const RE::TESForm* a_form)
		{
			if (a_form->IsDynamicForm()) {
				return std::format("dynamic:{:08X}", a_form->GetFormID());
			}
			const auto* file = a_form->GetFile(0);
			return std::format("{}:{:06X}", file ? file->GetFilename() : "?", a_form->GetLocalFormID());
		}

		SpaceIx AddSpace(RE::FormID a_form, bool a_world, std::string a_id, std::string a_parent)
		{
			const auto ix = static_cast<SpaceIx>(state.spaces.size());
			state.spaces.push_back({ std::move(a_id), a_world ? SpaceKind::Worldspace : SpaceKind::Interior, std::move(a_parent) });
			state.spaceForms.push_back(a_form);
			state.spaceIsWorld.push_back(a_world);
			state.spaceByForm.emplace(a_form, ix);
			state.dirty = true;
			return ix;
		}

		// The space id of a worldspace, listing it (and its parent, first) when new.
		SpaceIx WorldSpace(RE::TESWorldSpace* a_world)
		{
			if (const auto it = state.spaceByForm.find(a_world->GetFormID()); it != state.spaceByForm.end()) {
				return it->second;
			}
			std::string parent;
			if (auto* up = a_world->parentWorld) {
				parent = state.spaces[WorldSpace(up)].id;
			}
			return AddSpace(a_world->GetFormID(), true, "worldspace:" + FormKey(a_world), std::move(parent));
		}

		SpaceIx InteriorSpace(RE::TESObjectCELL* a_cell)
		{
			if (const auto it = state.spaceByForm.find(a_cell->GetFormID()); it != state.spaceByForm.end()) {
				return it->second;
			}
			return AddSpace(a_cell->GetFormID(), false, "interior:" + FormKey(a_cell), {});
		}

		// kNoSpace for a reference that stands in neither (no cell and no worldspace: not loaded far enough to say).
		SpaceIx SpaceOfRef(RE::TESObjectREFR* a_ref)
		{
			auto* cell = a_ref->GetParentCell();
			if (cell && cell->IsInteriorCell()) {
				return InteriorSpace(cell);
			}
			auto* world = cell ? cell->GetRuntimeData().worldSpace : a_ref->GetWorldspace();
			return world ? WorldSpace(world) : kNoSpace;
		}

		bool Contains(const char* a_text, std::string_view a_word)
		{
			if (!a_text) {
				return false;
			}
			std::string lower(a_text);
			std::ranges::transform(lower, lower.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
			return lower.contains(a_word);
		}

		// A placed teleport door, if a_ref is one; true when it was new. a_depth: its other end is
		// noted too (once, that end's own other end is this one).
		bool NoteDoor(RE::TESObjectREFR* a_ref, int a_depth = 0)
		{
			auto* base = a_ref->GetBaseObject();
			if (!base || !base->Is(RE::FormType::Door) || a_ref->IsDynamicForm()) {
				return false;
			}
			auto* teleport = a_ref->extraList.GetByType<RE::ExtraTeleport>();
			if (!teleport || !teleport->teleportData) {
				return false;
			}
			// An unseen door scripts use (Riften's prison marker): no doorway.
			if (static_cast<RE::TESObject*>(base)->IsMarker()) {
				return false;
			}
			if (state.doorByForm.contains(a_ref->GetFormID())) {
				return false;
			}
			const auto space = SpaceOfRef(a_ref);
			if (space == kNoSpace) {
				return false;
			}
			auto* door = static_cast<RE::TESObjectDOOR*>(base);
			auto* data = teleport->teleportData;
			auto* linked = data->linkedDoor.get().get();

			DoorRecord record;
			record.id = FormKey(a_ref);
			record.space = state.spaces[space].id;
			record.pos = ToCore(a_ref->GetPosition());
			record.rot = ToCore(a_ref->GetAngle());
			const float scale = a_ref->GetScale();
			const auto& bounds = door->boundData;
			record.halfWidth = std::max(std::abs(bounds.boundMax.x - bounds.boundMin.x), std::abs(bounds.boundMax.y - bounds.boundMin.y)) * 0.5f * scale;
			record.halfHeight = std::abs(bounds.boundMax.z - bounds.boundMin.z) * 0.5f * scale;
			record.landPos = ToCore(data->position);
			record.landRot = ToCore(data->rotation);
			if (Contains(door->GetModel(), "trapdoor") || Contains(door->GetModel(), "hatch")) {
				record.kind = DoorKind::Hatch;
			}
			// Whether it is locked, scripted or open changes while playing: the scheduler is told of it
			// every tick (Inputs::blockedDoors), not by the graph. Scripts of a reference aren't there to
			// read before the virtual machine has them.
			record.minimalUse = door->flags.any(RE::TESObjectDOOR::Flag::kMinimalUse);
			record.automatic = door->flags.any(RE::TESObjectDOOR::Flag::kAutomatic);
			record.hidden = a_ref->IsDisabled();
			if (linked && linked->extraList.GetByType<RE::ExtraTeleport>()) {
				record.destDoor = FormKey(linked);
				if (!linked->IsDynamicForm()) {
					if (const auto linkedSpace = a_depth == 0 ? SpaceOfRef(linked) : kNoSpace; linkedSpace != kNoSpace) {
						record.destSpace = state.spaces[linkedSpace].id;
					}
				}
			}
			state.lockedWhenFound += a_ref->IsLocked();

			state.doorByForm.emplace(a_ref->GetFormID(), static_cast<DoorIx>(state.doors.size()));
			state.doors.push_back(std::move(record));
			state.doorForms.push_back(a_ref->GetFormID());
			state.dirty = true;
			if (linked && a_depth == 0) {
				NoteDoor(linked, 1);
			}
			return true;
		}

		void Rebuild()
		{
			auto next = std::make_unique<SpaceGraph>(SpaceGraph::Build(state.spaces, state.doors));
			// Every space is listed before a door names it: indices stay where they were.
			SEAMLESS_ASSERT(next->Stats().unlistedSpaces == 0 && next->SpaceCount() == state.spaces.size());
			if (state.onRebuilt) {
				state.onRebuilt(*state.graph, *next);
			}
			state.graph = std::move(next);
			state.dirty = false;
			++state.revision;
		}

		void LookOver(RE::TESObjectCELL* a_cell)
		{
			const auto count = a_cell->GetRuntimeData().references.size();
			auto&      known = state.cellRefs[a_cell->GetFormID()];
			if (known == count) {
				return;
			}
			known = count;
			++state.cellsLooked;
			std::vector<RE::TESObjectREFR*> doors;
			a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				auto* base = a_ref->GetBaseObject();
				if (base && base->Is(RE::FormType::Door)) {
					doors.push_back(a_ref);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			for (auto* door : doors) {
				state.doorsFound += NoteDoor(door);
			}
		}
	}

	void Install()
	{
		const auto started = std::chrono::steady_clock::now();
		std::vector<RE::TESObjectREFR*> refs;
		{
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			RE::BSReadLockGuard guard{ lock };
			if (map) {
				for (const auto& [id, form] : *map) {
					++state.formsScanned;
					if (form && form->Is(RE::FormType::Reference)) {
						auto* ref = static_cast<RE::TESObjectREFR*>(form);
						if (ref->extraList.GetByType<RE::ExtraTeleport>()) {
							refs.push_back(ref);
						}
					}
				}
			}
		}
		for (auto* ref : refs) {
			NoteDoor(ref);
		}
		for (auto* world : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESWorldSpace>()) {
			if (world && world->persistentCell) {
				++state.persistentCells;
				LookOver(world->persistentCell);
			}
		}
		state.doorsAtLoad = state.doors.size();
		state.doorsFound = 0;
		Rebuild();
		state.loadMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
		const auto& graph = *state.graph;
		logger::info("graph: {} forms looked over in {:.0f} ms: {} teleport doors, {} spaces, {} doorways ({} persistent cells); more are added as cells load", state.formsScanned,
			state.loadMs, graph.DoorCount(), graph.SpaceCount(), graph.DoorwayCount(), state.persistentCells);
	}

	void Discover(RE::TESObjectCELL* a_here, float a_delta)
	{
		if ((state.timer -= a_delta) > 0.0f) {
			return;
		}
		state.timer = kDiscoverSeconds;
		const auto doorsBefore = state.doors.size();
		if (a_here->IsInteriorCell()) {
			LookOver(a_here);
		} else {
			auto* tes = RE::TES::GetSingleton();
			for (std::uint32_t x = 0; x < tes->gridCells->length; ++x) {
				for (std::uint32_t y = 0; y < tes->gridCells->length; ++y) {
					if (auto* cell = tes->gridCells->GetCell(x, y); cell && cell->IsAttached()) {
						LookOver(cell);
					}
				}
			}
			if (auto* world = a_here->GetRuntimeData().worldSpace; world && world->persistentCell) {
				LookOver(world->persistentCell);
			}
		}
		if (state.doors.size() != doorsBefore) {
			logger::info("graph: {} more door(s) around {} ({} in all)", state.doors.size() - doorsBefore, a_here->GetFormEditorID(), state.doors.size());
		}
	}

	void SetOnRebuilt(RebuiltCallback a_callback)
	{
		state.onRebuilt = a_callback;
	}

	void Flush()
	{
		if (state.dirty) {
			Rebuild();
		}
	}

	const SpaceGraph& Get()
	{
		return *state.graph;
	}

	SpaceIx SpaceOf(RE::TESObjectCELL* a_cell)
	{
		if (a_cell->IsInteriorCell()) {
			return InteriorSpace(a_cell);
		}
		auto* world = a_cell->GetRuntimeData().worldSpace;
		SEAMLESS_ASSERT(world);
		return WorldSpace(world);
	}

	SpaceIx FindSpaceOf(RE::TESObjectCELL* a_cell)
	{
		auto* world = a_cell->IsInteriorCell() ? nullptr : a_cell->GetRuntimeData().worldSpace;
		const auto it = state.spaceByForm.find(world ? world->GetFormID() : a_cell->GetFormID());
		return it != state.spaceByForm.end() ? it->second : kNoSpace;
	}

	DoorIx DoorOf(RE::FormID a_door)
	{
		const auto it = state.doorByForm.find(a_door);
		return it != state.doorByForm.end() ? it->second : kNoDoor;
	}

	RE::FormID SpaceForm(SpaceIx a_space)
	{
		return state.spaceForms[a_space];
	}

	RE::FormID DoorForm(DoorIx a_door)
	{
		return state.doorForms[a_door];
	}

	bool IsWorld(SpaceIx a_space)
	{
		return state.spaceIsWorld[a_space];
	}

	json Summary()
	{
		const auto& graph = *state.graph;
		std::size_t interiors = 0;
		std::size_t worlds = 0;
		for (SpaceIx space = 0; space < graph.SpaceCount(); ++space) {
			(graph.GetSpace(space).kind == SpaceKind::Interior ? interiors : worlds)++;
		}
		std::size_t paired = 0;
		std::size_t oneWay = 0;
		std::size_t broken = 0;
		std::size_t gates = 0;
		std::size_t hatches = 0;
		std::size_t noDestination = 0;
		for (DoorwayIx way = 0; way < graph.DoorwayCount(); ++way) {
			const auto& doorway = graph.GetDoorway(way);
			paired += doorway.link == Link::Paired;
			oneWay += doorway.link == Link::OneWay;
			broken += doorway.link == Link::Broken;
			gates += doorway.kind == DoorKind::Gate;
			hatches += doorway.kind == DoorKind::Hatch;
			noDestination += doorway.spaceB == kNoSpace;
		}
		return {
			{ "spaces", graph.SpaceCount() },
			{ "interiors", interiors },
			{ "worldspaces", worlds },
			{ "doors", graph.DoorCount() },
			{ "doorways", graph.DoorwayCount() },
			{ "paired", paired },
			{ "oneWay", oneWay },
			{ "broken", broken },
			{ "noDestination", noDestination },
			{ "gates", gates },
			{ "hatches", hatches },
			{ "lockedWhenFound", state.lockedWhenFound },
			{ "revision", state.revision },
			{ "atDataLoad", { { "formsLookedOver", state.formsScanned }, { "persistentCells", state.persistentCells }, { "doors", state.doorsAtLoad }, { "ms", state.loadMs } } },
			{ "foundSince", { { "doors", state.doorsFound }, { "cellsLookedOver", state.cellsLooked } } },
		};
	}
}
