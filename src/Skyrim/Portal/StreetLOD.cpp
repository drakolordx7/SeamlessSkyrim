#include "Skyrim/Portal/StreetLOD.h"

#include "Skyrim/Engine/Hooks.h"

namespace
{
	struct Street
	{
		RE::TESObjectCELL* cell = nullptr;
		RE::TESWorldSpace* world = nullptr;
		RE::NiTransform    toStreet;
		bool               moved = false;
	};
	// Set on the main thread, read by the engine's LOD job.
	std::mutex lock;
	Street     shown;

	Street Current()
	{
		std::scoped_lock guard(lock);
		return shown;
	}

	// The street's cell for the update in progress on this thread (the LOD job's).
	thread_local RE::TESObjectCELL* updating = nullptr;

	// A street newly shown: every node of the LOD is brought up to date in the next update.
	std::atomic<bool> refreshAll = false;

	// The engine's per-frame step for one node (BGSTerrainManager::Update's round robin): its grid
	// flags, then its land, objects and trees placed and shown or hidden for the view.
	void UpdateNode(RE::BGSTerrainManager* a_manager, RE::BGSTerrainNode* a_node, const RE::NiPoint3& a_viewPos, RE::TESObjectCELL* a_cell)
	{
		using Flags_t = void(RE::BGSTerrainNode*);
		using Place_t = void(RE::BGSTerrainManager*, RE::BGSTerrainNode*, const RE::NiPoint3*, RE::TESObjectCELL*, bool);
		static REL::Relocation<Flags_t> flags{ REL::RelocationID(31054, 31841) };
		static REL::Relocation<Place_t> place{ REL::RelocationID(31041, 31828) };
		flags(a_node);
		place(a_manager, a_node, &a_viewPos, a_cell, a_node->nodeState.any(RE::BGSTerrainNode::Flag::kInLargeRefGrid));
	}

	bool PlayerIndoors()
	{
		auto* cell = RE::PlayerCharacter::GetSingleton()->parentCell;
		return cell && cell->IsInteriorCell();
	}

	// The job's "which worldspace is the player in" (TESObjectREFR::GetWorldspace): none indoors,
	// and then the job stops. The street's while it is shown.
	struct WorldspaceHook
	{
		static RE::TESWorldSpace* Thunk(RE::TESObjectREFR* a_player)
		{
			auto* world = func(a_player);
			return world ? world : Current().world;
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// The loaded cells' area, where the LOD land's vertices are sunk under the real ground (the
	// LODLAND vertex shader's HighDetailRange): centre and half size in world coordinates, set by
	// the update (ID 105650) and read by the lighting shader's setup.
	struct HighDetailRange
	{
		float centerX, centerY, halfX, halfY;
	};

	// The update sets the street's area in the street's coordinates; merged, the LOD land is drawn
	// in the interior's, and the street's own ground was left with the low LOD land showing through
	// it (or not, wherever the area happened to land). Moved after every update: each one sets it
	// anew (ID 31802 calls 105650 unconditionally), so the move never adds up.
	void MoveHighDetailRange(const RE::NiTransform& a_toInterior)
	{
		static REL::Relocation<HighDetailRange*> range{ REL::RelocationID(513218, 390958) };
		auto&      r = *range;
		const auto center = a_toInterior * RE::NiPoint3{ r.centerX, r.centerY, 0.0f };
		const auto& m = a_toInterior.rotate;
		// An axis-aligned area turned: the one around it (a quarter turn swaps its sides).
		const float halfX = std::abs(m.entry[0][0]) * r.halfX + std::abs(m.entry[0][1]) * r.halfY;
		const float halfY = std::abs(m.entry[1][0]) * r.halfX + std::abs(m.entry[1][1]) * r.halfY;
		r = { center.x, center.y, halfX, halfY };
	}

	// The street's distant-object blocks (the lowest level, cut into one segment per cell): the
	// segments of cells whose own 3D is attached hidden. The engine does this (ID 523589) only during
	// a loading screen or for cells whose LOD visibility data is loaded, which the street's cells,
	// loaded behind a door, got about a second later: until then the low LOD town stood in the
	// street's houses, and cast the shadows the houses don't (the well's). Hidden only: showing them
	// again (cells let go of) is left to the engine.
	void HideStreetSegments(RE::BGSTerrainManager* a_manager, RE::TESWorldSpace* a_world, int a_x, int a_y)
	{
		using Hide_t = void(RE::BSSubIndexTriShape*, std::uint32_t);
		using Commit_t = void(RE::BSSubIndexTriShape*);
		static REL::Relocation<Hide_t>   hide{ REL::RelocationID(75028, 76790) };
		static REL::Relocation<Commit_t> commit{ REL::RelocationID(75029, 76791) };
		const auto level = a_manager->segmentedBlockLevel;
		std::function<void(RE::BGSTerrainNode*)> visit = [&](RE::BGSTerrainNode* a_node) {
			const int lod = static_cast<int>(a_node->GetLODLevel());
			if (a_node->baseCellX > a_x + 2 || a_node->baseCellX + lod <= a_x - 2 || a_node->baseCellY > a_y + 2 || a_node->baseCellY + lod <= a_y - 2) {
				return;
			}
			if (static_cast<std::uint32_t>(lod) != level) {
				if (a_node->children) {
					auto* kids = reinterpret_cast<RE::BGSTerrainNode*>(a_node->children);
					for (int k = 0; k < 4; ++k) {
						visit(&kids[k]);
					}
				}
				return;
			}
			auto* block = a_node->objects ? reinterpret_cast<RE::BGSDistantObjectBlock*>(a_node->objects->block) : nullptr;
			auto* node = block ? block->node.get() : nullptr;
			if (!node) {
				return;
			}
			RE::BSVisit::TraverseScenegraphGeometries(node, [&](RE::BSGeometry* a_shape) {
				auto*       shape = netimmerse_cast<RE::BSSubIndexTriShape*>(a_shape);
				const auto* data = shape ? &shape->GetSubIndexedTrishapeRuntimeData() : nullptr;
				bool        changed = false;
				for (std::uint32_t i = 0; data && data->segmentData && i < data->numSegments && i < level * level; ++i) {
					const auto& segment = data->segmentData[i];
					if (segment.unkTriCount == 0 || segment.unkFlags == 0) {
						continue;
					}
					const auto x = static_cast<std::int16_t>(a_node->baseCellX + i / level), y = static_cast<std::int16_t>(a_node->baseCellY + i % level);
					if (std::abs(x - a_x) > 2 || std::abs(y - a_y) > 2) {
						continue;  // the grid around the street's cell
					}
					const auto it = a_world->cellMap.find(RE::CellID(y, x));
					auto*      cell = it != a_world->cellMap.end() ? it->second : nullptr;
					if (cell && cell->cellState.get() == RE::TESObjectCELL::CellState::kAttached) {
						hide(shape, i);
						changed = true;
					}
				}
				if (changed) {
					commit(shape);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		};
		if (a_manager->rootNode) {
			visit(a_manager->rootNode);
		}
	}

	// TES's grid centre (currentGridX at +0xB0, currentGridY right after it) as one 64-bit value.
	struct GridSwap
	{
		static_assert(offsetof(RE::TES, currentGridY) == offsetof(RE::TES, currentGridX) + 4);

		explicit GridSwap(const RE::EXTERIOR_DATA* a_exterior)
		{
			if (!a_exterior) {
				return;
			}
			grid = reinterpret_cast<volatile LONG64*>(&RE::TES::GetSingleton()->currentGridX);
			own = *grid;
			street = Pack(a_exterior->cellX, a_exterior->cellY);
			swapped = own != street && ::InterlockedCompareExchange64(grid, street, own) == own;
		}
		~GridSwap()
		{
			// Walked out meanwhile, the crossing may have set the street's cell itself (the same
			// value): the player's own then, not to be undone.
			if (swapped && PlayerIndoors()) {
				::InterlockedCompareExchange64(grid, own, street);
			}
		}
		GridSwap(const GridSwap&) = delete;
		GridSwap& operator=(const GridSwap&) = delete;

	private:
		static LONG64 Pack(std::int32_t a_x, std::int32_t a_y)
		{
			return static_cast<LONG64>(static_cast<std::uint32_t>(a_x)) | (static_cast<LONG64>(static_cast<std::uint32_t>(a_y)) << 32);
		}

		volatile LONG64* grid = nullptr;
		LONG64           own = 0;
		LONG64           street = 0;
		bool             swapped = false;
	};

	struct UpdateHook
	{
		static void Thunk(RE::BGSTerrainManager* a_manager, const RE::NiPoint3& a_viewPos, const std::int32_t& a_updateType)
		{
			if (!PlayerIndoors()) {
				func(a_manager, a_viewPos, a_updateType);
				return;
			}
			// Indoors the job got this far only for a street shown in the interior. Hidden since
			// (the main thread, meanwhile), the update would place the LOD around the interior
			// cell, whose worldspace is none: that crashed.
			const auto street = Current();
			if (!street.cell) {
				return;
			}
			updating = street.cell;
			// The LOD of the cells around TES's grid centre gives way to their own houses, which
			// the street has loaded (BSSubIndexTriShape segments per cell). Indoors the engine keeps
			// that centre at the interior position's cell (TES::SetCurrentCell, and again every
			// frame), far from the street: from the Bannered Mare the low LOD town stood in the
			// street's houses. The street's cell is the centre for this update only.
			// The update runs on a worker thread (measured) and reads the centre in many places
			// (IDs 13312, 31841, ...), so it is swapped in TES itself while the main thread runs on:
			// both halves are one compare-exchange of the pair, so a value the main thread wrote
			// meanwhile (walking out, the crossing setting the new cell) is never overwritten.
			const auto* exterior = street.cell->GetRuntimeData().cellData.exterior;
			GridSwap    swap(exterior);
			const auto viewPos = street.toStreet * a_viewPos;
			// The engine steps one node a frame: shown in a room, the street's own cells were
			// drawn as low LOD land and houses for a fifth of a second until each node's turn came.
			if (refreshAll.exchange(false)) {
				for (auto* node : a_manager->updateNodes) {
					if (node) {
						UpdateNode(a_manager, node, viewPos, street.cell);
					}
				}
			}
			func(a_manager, viewPos, a_updateType);
			if (exterior) {
				HideStreetSegments(a_manager, street.world, exterior->cellX, exterior->cellY);
			}
			if (street.moved) {
				MoveHighDetailRange(street.toStreet.Invert());
			}
			updating = nullptr;
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// Inside the update, the LOD around the player's cell: the street's while it is shown.
	struct PlaceAroundHook
	{
		static void Thunk(RE::BGSTerrainManager* a_manager, void* a_node, const RE::NiPoint3* a_viewPos, RE::TESObjectCELL* a_cell, bool a_immediate)
		{
			func(a_manager, a_node, a_viewPos, updating ? updating : a_cell, a_immediate);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// TES's worldspace, which attaching the loaded blocks reads (none indoors: a crash).
	// Every caller comes here; only the LOD job's update for a street is given the street's.
	struct TESWorldspaceHook
	{
		static RE::TESWorldSpace* Thunk(RE::TES* a_tes)
		{
			return updating ? updating->GetRuntimeData().worldSpace : a_tes->GetRuntimeData2().worldSpace;
		}
	};

	struct PlaceAroundAllHook
	{
		static std::uintptr_t Thunk(void* a_node, const RE::NiPoint3* a_viewPos, RE::TESObjectCELL* a_cell, std::uint32_t a_flags)
		{
			return func(a_node, a_viewPos, updating ? updating : a_cell, a_flags);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};
}

namespace Portal::StreetLOD
{
	void Install()
	{
		// All or none: the job let through indoors without the update's cell taken over crashed.
		const REL::RelocationID kJob(38145, 39102), kUpdate(31017, 31802), kPlaceAround(31041, 31828), kPlaceAroundAll(31048, 31835);
		REL::Relocation<std::uintptr_t> worldSite{ kJob, Hooks::Offset(0x1E, 0x1E, 0x2F) };
		REL::Relocation<std::uintptr_t> updateSite{ kJob, Hooks::Offset(0x5D, 0x5D, 0x6E) };
		// SE inlines two of the update's three calls to kPlaceAround, leaving calls to kPlaceAroundAll
		// in their place: those are hooked as the update's cell (all) instead.
		std::vector<REL::Relocation<std::uintptr_t>> allSites, placeSites;
		for (const auto offset : REL::Relocate(std::vector{ 0x22C, 0x2C6, 0x381 }, std::vector{ 0x260 })) {
			allSites.emplace_back(kUpdate, offset);
		}
		for (const auto offset : REL::Relocate(std::vector{ 0x26D }, std::vector{ 0x2A1, 0x2F5, 0x37E })) {
			placeSites.emplace_back(kUpdate, offset);
		}
		bool ok = Hooks::CallsTo(worldSite.address(), REL::RelocationID(19389, 19816), "street LOD: the job's worldspace") &&
		          Hooks::CallsTo(updateSite.address(), kUpdate, "street LOD: the job's update");
		for (const auto& site : allSites) {
			ok = ok && Hooks::CallsTo(site.address(), kPlaceAroundAll, "street LOD: the update's cell (all)");
		}
		for (const auto& site : placeSites) {
			ok = ok && Hooks::CallsTo(site.address(), kPlaceAround, "street LOD: the update's cell");
		}
		// The whole function is "mov rax, [rcx+148h]; ret" (SE: +140h): replaced, not called.
		REL::Relocation<std::uintptr_t> tesWorld{ REL::RelocationID(13169, 13314) };
		const std::array<std::uint8_t, 8> kTesWorldCode{ 0x48, 0x8B, 0x81, REL::Relocate<std::uint8_t>(0x40, 0x48), 0x01, 0x00, 0x00, 0xC3 };
		if (std::memcmp(reinterpret_cast<const void*>(tesWorld.address()), kTesWorldCode.data(), kTesWorldCode.size()) != 0) {
			logger::error("hooks: street LOD: TES's worldspace getter isn't the one reversed (another mod's hook, or a different game build)");
			ok = false;
		}
		if (!ok) {
			logger::warn("street LOD: not installed; the view out of a room loaded into shows no distant land");
			return;
		}
		auto& trampoline = SKSE::GetTrampoline();
		WorldspaceHook::func = trampoline.write_call<5>(worldSite.address(), WorldspaceHook::Thunk);
		UpdateHook::func = trampoline.write_call<5>(updateSite.address(), UpdateHook::Thunk);
		{
			// Its callers may count on every register but rax staying as it was (the original
			// touches no other), so the C++ replacement is called through a stub keeping them.
			std::array<std::uint8_t, 41> stub{
				0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,  // push rcx, rdx, r8-r11
				0x48, 0x83, 0xEC, 0x28,                                      // sub rsp, 28h (aligned for the call)
				0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                          // mov rax, Thunk
				0xFF, 0xD0,                                                  // call rax
				0x48, 0x83, 0xC4, 0x28,                                      // add rsp, 28h
				0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,  // pop r11-r8, rdx, rcx
				0xC3                                                         // ret
			};
			const auto thunk = reinterpret_cast<std::uintptr_t>(&TESWorldspaceHook::Thunk);
			std::memcpy(stub.data() + 16, &thunk, sizeof(thunk));
			auto* code = trampoline.allocate(stub.size());
			std::memcpy(code, stub.data(), stub.size());
			trampoline.write_branch<5>(tesWorld.address(), reinterpret_cast<std::uintptr_t>(code));
		}
		for (const auto& site : allSites) {
			PlaceAroundAllHook::func = trampoline.write_call<5>(site.address(), PlaceAroundAllHook::Thunk);
		}
		for (const auto& site : placeSites) {
			// The engine's function at each site (checked above), whichever write returns it.
			PlaceAroundHook::func = trampoline.write_call<5>(site.address(), PlaceAroundHook::Thunk);
		}
		logger::info("street LOD: installed");
	}

	void Show(RE::TESObjectCELL* a_street, const RE::NiTransform& a_toStreet, bool a_moved)
	{
		auto* world = a_street ? a_street->GetRuntimeData().worldSpace : nullptr;
		std::scoped_lock guard(lock);
		shown = world ? Street{ a_street, world, a_toStreet, a_moved } : Street{};
		refreshAll = world != nullptr;
	}

	void Hide()
	{
		std::scoped_lock guard(lock);
		shown = {};
	}
}
