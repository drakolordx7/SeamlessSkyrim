#include "Skyrim/Spaces/WalkOut.h"

#include "Skyrim/Engine/Hooks.h"
#include "Skyrim/Portal/TransitionStalls.h"
#include "Skyrim/Portal/TravelTrace.h"
#include "Skyrim/Spaces/Tuning.h"

namespace
{
	using CellState = RE::TESObjectCELL::CellState;

	std::vector<RE::TESObjectCELL*> handedOver;

	// The change into an exterior (ID 13317) re-uses the grid buffered from before only if
	// every one of its cells is merely loaded (ID 15951: state kLoaded); otherwise it loads the
	// grid anew and then sleeps until the loader's queue is empty (ID 75708, 50 ms at a time).
	// Cells handed over still attached failed that check, and the walk out took longer than
	// with them detached and attached again. They count as ready here; the attach loop that
	// follows skips them (it attaches unattached cells only).
	struct GridReadyHook
	{
		static bool Thunk(void* a_grid)
		{
			if (func(a_grid)) {
				return true;
			}
			if (handedOver.empty() || !a_grid) {
				return false;
			}
			const auto* raw = static_cast<const std::uint8_t*>(a_grid);
			const auto  size = *reinterpret_cast<const std::uint32_t*>(raw + 0x10);
			auto* const* gridCells = *reinterpret_cast<RE::TESObjectCELL* const* const*>(raw + 0x18);
			int          accepted = 0;
			for (std::uint32_t i = 0; gridCells && i < size * size; ++i) {
				auto* cell = gridCells[i];
				const auto state = cell ? cell->cellState.get() : CellState::kUnloaded;
				if (state == CellState::kUnattached) {
					continue;
				}
				if (state != CellState::kAttached || std::ranges::find(handedOver, cell) == handedOver.end()) {
					auto* coordinates = cell ? cell->GetCoordinates() : nullptr;
					logger::info("portal: grid not ready: cell ({},{}) in state {}{}", coordinates ? coordinates->cellX : 0, coordinates ? coordinates->cellY : 0,
						static_cast<int>(state), cell ? "" : " (none)");
					return false;
				}
				++accepted;
			}
			logger::info("portal: grid ready with {} cell(s) handed over attached", accepted);
			return true;
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// The change into an exterior (ID 13317) sets the worldspace up anew when the game's grid isn't
	// set up yet (the first trip outdoors after loading a save made indoors), and then waits for
	// the task pool twice (+0x4A0, +0x4D5), until the whole grid around the player has loaded: the
	// first walk out took 1.2 to 1.4 s, a frozen frame. The cells around the door are attached
	// already (handed over from the doorway's view, RemoteCell::Release); the rest streams in over
	// the next frames, as in the trips after it.
	struct GridLoadWaitHook
	{
		static std::uint64_t Thunk(void* a_taskPool, std::uint32_t a_level)
		{
			if (Portal::TransitionStalls::Seamless() && !handedOver.empty()) {
				Portal::TravelTrace::Note("waiting for the outdoor grid to load skipped");
				return 0;
			}
			return func(a_taskPool, a_level);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};
}

namespace Spaces::WalkOut
{
	void Install()
	{
		REL::Relocation<std::uintptr_t> site{ REL::RelocationID(13172, 13317), REL::Relocate(0x104, 0xF4) };
		if (Hooks::CallsTo(site.address(), REL::RelocationID(15716, 15951), "portal: grid ready check")) {
			GridReadyHook::func = SKSE::GetTrampoline().write_call<5>(site.address(), GridReadyHook::Thunk);
		} else {
			logger::warn("portal: street cells are detached when walking out");
			tuning.handOverStreet = false;
		}
		const REL::RelocationID kWaitForLoads(73972, 75708);
		for (const auto offset : REL::Relocate(std::array{ 0x402, 0x437 }, std::array{ 0x4A0, 0x4D5 })) {
			REL::Relocation<std::uintptr_t> gridWait{ REL::RelocationID(13172, 13317), offset };
			if (Hooks::CallsTo(gridWait.address(), kWaitForLoads, "portal: grid load wait")) {
				GridLoadWaitHook::func = SKSE::GetTrampoline().write_call<5>(gridWait.address(), GridLoadWaitHook::Thunk);
			}
		}
	}

	const std::vector<RE::TESObjectCELL*>& HandedOver()
	{
		return handedOver;
	}

	void Hand(RE::TESObjectCELL* a_cell)
	{
		handedOver.push_back(a_cell);
	}

	void Forget()
	{
		handedOver.clear();
	}
}
