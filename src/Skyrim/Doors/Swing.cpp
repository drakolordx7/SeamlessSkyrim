#include "Skyrim/Doors/Swing.h"

#include "Skyrim/Doors/Geometry.h"

namespace
{
	struct LetGoDoor
	{
		float amount;
		bool  opening;
		float seconds;  // since it was let go of
	};
	std::unordered_map<RE::FormID, LetGoDoor> letGo;

	constexpr float kLetGoSeconds = 3.0f;
}

namespace Portal::Doors
{
	void Swing::TakeOver(RE::FormID a_door)
	{
		const auto it = letGo.find(a_door);
		if (it == letGo.end()) {
			*this = {};
			return;
		}
		amount = it->second.amount;
		opening = it->second.opening;
		letGo.erase(it);
	}

	bool Swing::Step(RE::TESObjectREFR* a_timed, bool a_wanted, float a_delta)
	{
		const bool wasShut = Shut();
		if (a_wanted != opening && amount > 0.0f && amount < 1.0f) {
			const float eased = SwingAmount(a_timed, amount, opening);
			float       best = amount, bestOff = FLT_MAX;
			for (int i = 0; i <= 200; ++i) {
				const float t = static_cast<float>(i) / 200.0f;
				if (const float off = std::abs(SwingAmount(a_timed, t, a_wanted) - eased); off < bestOff) {
					bestOff = off;
					best = t;
				}
			}
			amount = best;
		}
		opening = a_wanted;
		const float step = a_delta / SwingSeconds(a_timed, a_wanted);
		amount = std::clamp(amount + (a_wanted ? step : -step), 0.0f, 1.0f);
		return wasShut;
	}

	void Swing::Turn(RE::TESObjectREFR* a_timed, RE::TESObjectREFR* a_door, const RE::NiPoint3& a_room, RE::FormID a_settings) const
	{
		TurnLeaves(a_door, a_room, SwingAmount(a_timed, amount, opening), a_settings);
	}

	void Swing::LetGo(RE::FormID a_door) const
	{
		letGo[a_door] = { amount, opening, 0.0f };
	}

	void Swing::Reclaim(RE::FormID a_door)
	{
		letGo.erase(a_door);
	}

	void Swing::Expire(float a_delta)
	{
		for (auto it = letGo.begin(); it != letGo.end();) {
			if ((it->second.seconds += a_delta) > kLetGoSeconds) {
				RestoreLeaves(it->first);
				it = letGo.erase(it);
			} else {
				++it;
			}
		}
	}

	void Swing::ForgetAll()
	{
		for (const auto& [door, left] : letGo) {
			RestoreLeaves(door);
		}
		letGo.clear();
	}
}
