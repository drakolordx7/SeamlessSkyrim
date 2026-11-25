#include "Skyrim/Diag/Probe.h"

namespace
{
	using CellState = RE::TESObjectCELL::CellState;

	std::string_view StateName(CellState a_state)
	{
		switch (a_state) {
		case CellState::kUnloaded:   return "Unloaded"sv;
		case CellState::kUnloading:  return "Unloading"sv;
		case CellState::kDataLoaded: return "DataLoaded"sv;
		case CellState::kBuilding3D: return "Building3D"sv;
		case CellState::kUnattached: return "Unattached"sv;
		case CellState::kDetaching:  return "Detaching"sv;
		case CellState::kAttaching:  return "Attaching"sv;
		case CellState::kAttached:   return "Attached"sv;
		default:                     return "?"sv;
		}
	}

	struct ActorCounts
	{
		int total = 0;
		int high = 0;
		int with3D = 0;
	};

	ActorCounts CountActors(const RE::TESObjectCELL* a_cell)
	{
		ActorCounts counts;
		a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
			if (auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr) {
				++counts.total;
				auto* process = actor->GetActorRuntimeData().currentProcess;
				if (process && process->InHighProcess()) {
					++counts.high;
				}
				if (actor->Get3D()) {
					++counts.with3D;
				}
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		return counts;
	}

	std::string Describe(RE::TESObjectCELL* a_cell)
	{
		if (!a_cell) {
			return "<none>";
		}
		const char* edid = a_cell->GetFormEditorID();
		const auto* loaded = a_cell->GetRuntimeData().loadedData;
		const auto  actors = CountActors(a_cell);
		return std::format(
			"{:08X} '{}' state={} 3D={} actors={} (high={}, with3D={})",
			a_cell->GetFormID(),
			edid && *edid ? edid : a_cell->GetFullName(),
			StateName(a_cell->cellState.get()),
			loaded && loaded->cell3D ? "yes" : "no",
			actors.total, actors.high, actors.with3D);
	}
}

namespace Probe
{
	std::string DescribeCell(RE::TESObjectCELL* a_cell)
	{
		return Describe(a_cell);
	}

	void DumpWorldState(std::string_view a_reason)
	{
		logger::info("=== world state ({})", a_reason);

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* tes = RE::TES::GetSingleton();
		logger::info("player cell:   {}", Describe(player ? player->GetParentCell() : nullptr));
		logger::info("TES interior:  {}", Describe(tes ? tes->interiorCell : nullptr));

		// Every interior that is not fully unloaded: the engine's interior buffer, and ours held
		// behind doors.
		if (auto* data = RE::TESDataHandler::GetSingleton()) {
			auto* playerCell = player ? player->GetParentCell() : nullptr;
			int   resident = 0;
			for (auto* cell : data->interiorCells) {
				if (cell && cell->cellState.get() != CellState::kUnloaded && cell != playerCell) {
					logger::info("resident:      {}", Describe(cell));
					++resident;
				}
			}
			logger::info("{} other interior(s) resident", resident);
		}

		// Which cells the engine runs full AI and animation for: a space held behind a door should
		// be among them.
		if (auto* processes = RE::ProcessLists::GetSingleton()) {
			std::unordered_map<RE::TESObjectCELL*, int> highPerCell;
			for (auto& handle : processes->highActorHandles) {
				if (auto actor = handle.get()) {
					++highPerCell[actor->GetParentCell()];
				}
			}
			for (auto& [cell, count] : highPerCell) {
				logger::info("high-process:  {} actor(s) in {:08X}", count, cell ? cell->GetFormID() : 0u);
			}
		}
	}
}
