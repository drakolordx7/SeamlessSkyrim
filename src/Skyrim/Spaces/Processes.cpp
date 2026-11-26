#include "Skyrim/Spaces/RemoteCell.h"

#include "Skyrim/Diag/Probe.h"

namespace Spaces
{
	namespace
	{
		using Level = RE::PROCESS_TYPE;

		bool InHigh(RE::Actor* a_actor)
		{
			return a_actor->GetProcessLevel() == Level::kHigh;
		}
	}

	// The engine's own moves between the process lists (Actor vfuncs 0xF1..0xF4: MoveToHigh, MoveToLow,
	// MoveToMiddleLow, MoveToMiddleHigh). The high process is what the AI, the movement and the
	// animation of an NPC run in; the others are what it gets in a cell that is loaded and not the
	// player's (docs/ENGINE.md, "Actor processes").
	void RemoteCell::Raise(RE::Actor* a_actor)
	{
		if (InHigh(a_actor)) {
			return;
		}
		const auto before = a_actor->GetProcessLevel();
		a_actor->MoveToHigh();
		if (InHigh(a_actor)) {
			raised.push_back({ a_actor->GetHandle(), before });
		}
	}

	void RemoteCell::RaiseActors()
	{
		for (auto* cell : cells) {
			for (auto* actor : ActorsIn(cell)) {
				if (!actor->IsDead()) {
					Raise(actor);
				}
			}
		}
	}

	void RemoteCell::LowerActors()
	{
		const auto* playersCell = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		for (const auto& [handle, before] : raised) {
			const auto actor = handle.get();
			// Not the ones that walked through the door into the player's space (carried by NpcTravel):
			// they are the player's space's now, and stay as the engine has them. Nor one the engine has
			// moved by itself (it died, was moved down by a script).
			if (!actor || actor->IsDead() || actor->GetParentCell() == playersCell || !InHigh(actor.get())) {
				continue;
			}
			switch (before) {
			case Level::kMiddleHigh:
				actor->MoveToMiddleHigh();
				break;
			case Level::kMiddleLow:
				actor->MovetoMiddleLow();
				break;
			default:  // kLow, and kNone: MoveToHigh made its process, it had none before
				actor->MovetoLow();
				break;
			}
		}
	}

	void RemoteCell::SettleActors()
	{
		if (cells.empty()) {
			return;
		}
		const bool live = !paused && !forPicture;
		if (live && !awake) {
			RaiseActors();
			awake = true;
			logger::info("portal: {} live, {} of its people raised to the high process", Probe::DescribeCell(Cell()), raised.size());
		} else if (!live && awake) {
			LowerActors();
			logger::info("portal: {} paused, {} people put back to the level they had", Probe::DescribeCell(Cell()), raised.size());
			raised.clear();
			awake = false;
		}
	}

	json RemoteCell::DescribeActors() const
	{
		int alive = 0, dead = 0;
		int levels[5] = {};  // none, high, middle high, middle low, low
		for (auto* cell : cells) {
			for (auto* actor : ActorsIn(cell)) {
				if (actor->IsDead()) {
					++dead;
					continue;
				}
				++alive;
				++levels[static_cast<int>(actor->GetProcessLevel()) + 1];
			}
		}
		return { { "alive", alive }, { "dead", dead }, { "none", levels[0] }, { "high", levels[1] }, { "middleHigh", levels[2] }, { "middleLow", levels[3] }, { "low", levels[4] },
			{ "raisedByUs", raised.size() }, { "awake", awake } };
	}
}
