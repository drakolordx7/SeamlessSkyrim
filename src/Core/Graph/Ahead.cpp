#include "Core/Graph/Ahead.h"

namespace Core::Graph
{
	namespace
	{
		// The best (lowest) rank among those a_wanted accepts.
		template <class F>
		std::optional<std::size_t> Likeliest(std::span<const AheadSpace> a_spaces, F&& a_wanted)
		{
			std::optional<std::size_t> best;
			for (std::size_t i = 0; i < a_spaces.size(); ++i) {
				if (a_wanted(a_spaces[i]) && (!best || a_spaces[i].rank < a_spaces[*best].rank)) {
					best = i;
				}
			}
			return best;
		}
	}

	std::optional<std::size_t> NextShot(std::span<const AheadSpace> a_spaces, float a_headroomMs, const AheadConfig& a_config)
	{
		if (a_headroomMs < a_config.shotMs + a_config.reserveMs) {
			return std::nullopt;
		}
		return Likeliest(a_spaces, [](const AheadSpace& a_space) { return a_space.ready; });
	}

	std::optional<std::size_t> NextAttach(std::span<const AheadSpace> a_spaces, bool a_attachInFlight, float a_headroomMs, float a_freeMb, const AheadConfig& a_config)
	{
		if (a_attachInFlight || a_headroomMs < a_config.attachMs + a_config.shotMs + a_config.reserveMs || a_freeMb < a_config.attachMb) {
			return std::nullopt;
		}
		return Likeliest(a_spaces, [](const AheadSpace& a_space) { return !a_space.ready && a_space.attachable; });
	}
}
