#include "Core/Doors/Swing.h"

#include <cfloat>
#include <cmath>

#include "Core/Math/Vec3.h"

namespace Core::Doors
{
	SwingCurve StillCurve(bool a_opening)
	{
		return SampleCurve(1.0f, [&](float a_t) {
			const float eased = 0.5f - 0.5f * std::cos(kPi * a_t);
			return a_opening ? eased : 1.0f - eased;
		});
	}

	bool SwingState::Step(const SwingCurves& a_curves, bool a_wanted, float a_delta)
	{
		const bool wasShut = Shut();
		if (a_wanted != opening && amount > 0.0f && amount < 1.0f) {
			const float eased = a_curves.Amount(amount, opening);
			float       best = amount, bestOff = FLT_MAX;
			for (int i = 0; i <= 200; ++i) {
				const float t = static_cast<float>(i) / 200.0f;
				if (const float off = std::abs(a_curves.Amount(t, a_wanted) - eased); off < bestOff) {
					bestOff = off;
					best = t;
				}
			}
			amount = best;
		}
		opening = a_wanted;
		const float step = a_delta / a_curves.Seconds(a_wanted);
		amount = std::clamp(amount + (a_wanted ? step : -step), 0.0f, 1.0f);
		return wasShut;
	}

	void LetGoBook::LetGo(std::uint32_t a_door, const SwingState& a_state)
	{
		Reclaim(a_door);
		entries.push_back({ a_door, a_state.Amount(), a_state.Opening(), 0.0f });
	}

	bool LetGoBook::TakeOver(std::uint32_t a_door, SwingState& a_state)
	{
		const auto it = std::ranges::find(entries, a_door, &Entry::door);
		if (it == entries.end()) {
			a_state.Reset();
			return false;
		}
		a_state.SetProgress(it->amount, it->opening);
		entries.erase(it);
		return true;
	}

	void LetGoBook::Reclaim(std::uint32_t a_door)
	{
		std::erase_if(entries, [&](const Entry& a_entry) { return a_entry.door == a_door; });
	}

	std::vector<std::uint32_t> LetGoBook::Expire(float a_delta)
	{
		std::vector<std::uint32_t> expired;
		for (auto& entry : entries) {
			if ((entry.seconds += a_delta) > kSeconds) {
				expired.push_back(entry.door);
			}
		}
		std::erase_if(entries, [](const Entry& a_entry) { return a_entry.seconds > kSeconds; });
		return expired;
	}

	std::vector<std::uint32_t> LetGoBook::TakeAll()
	{
		std::vector<std::uint32_t> all;
		for (const auto& entry : entries) {
			all.push_back(entry.door);
		}
		entries.clear();
		return all;
	}
}
