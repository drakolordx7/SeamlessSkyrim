#pragma once

namespace Engine
{
	// The same space: one interior, or anywhere in one worldspace (its cells are walked between
	// without a door).
	[[nodiscard]] inline bool SameSpace(const RE::TESObjectCELL* a_cell, const RE::TESObjectCELL* a_other)
	{
		if (!a_cell || !a_other) {
			return false;
		}
		if (a_cell->IsInteriorCell() || a_other->IsInteriorCell()) {
			return a_cell == a_other;
		}
		return a_cell->GetRuntimeData().worldSpace == a_other->GetRuntimeData().worldSpace;
	}
}
