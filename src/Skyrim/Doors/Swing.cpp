#include "Skyrim/Doors/Swing.h"

#include "Skyrim/Doors/Geometry.h"

namespace
{
	Core::Doors::LetGoBook letGo;
}

namespace Portal::Doors
{
	void Swing::TakeOver(RE::FormID a_door)
	{
		letGo.TakeOver(a_door, state);
	}

	bool Swing::Step(RE::TESObjectREFR* a_timed, bool a_wanted, float a_delta)
	{
		return state.Step(SwingCurvesOf(a_timed), a_wanted, a_delta);
	}

	void Swing::Turn(RE::TESObjectREFR* a_timed, RE::TESObjectREFR* a_door, const RE::NiPoint3& a_room, RE::FormID a_settings) const
	{
		TurnLeaves(a_door, a_room, SwingCurvesOf(a_timed).Amount(state.Amount(), state.Opening()), a_settings);
	}

	void Swing::LetGo(RE::FormID a_door) const
	{
		letGo.LetGo(a_door, state);
	}

	void Swing::Reclaim(RE::FormID a_door)
	{
		letGo.Reclaim(a_door);
	}

	void Swing::Expire(float a_delta)
	{
		for (const auto door : letGo.Expire(a_delta)) {
			RestoreLeaves(door);
		}
	}

	void Swing::ForgetAll()
	{
		for (const auto door : letGo.TakeAll()) {
			RestoreLeaves(door);
		}
	}
}
