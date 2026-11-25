#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

#include "Core/Crossing/Approach.h"

// An interior and the outdoors have different image spaces (tone mapping, brightness, tint); the
// engine switches when the player changes space. Through an open doorway that is a jump: the room
// seen from the porch is drawn with the outdoor one, and darkens at once on walking in. Instead
// the two are blended by where the camera is relative to the doorway, on both sides of the
// crossing. The weights and curves are here.
namespace Core::ImageSpace
{
	// How far the camera is inside the interior behind the doorway (negative: outside) is blended
	// over this range of depth.
	inline constexpr float kFrom = -150.0f;
	inline constexpr float kTo = 120.0f;
	// The eye adaptation walked out with was the room's: the street came in far too bright and
	// took seconds to settle. A load door starts it afresh behind its loading screen; here it is
	// let catch up with the street at once in the first frames outdoors.
	inline constexpr int kAdaptSnapFrames = 15;
	// Times the frame time: past 1, the shader's step is the whole way.
	inline constexpr float kSnapSpeed = 1000.0f;
	// The outdoors' image space taken indoors holds for this long, in game hours.
	inline constexpr float kOutdoorsHoldsForHours = 0.5f;
	// The outdoors' own settings only once the player has been out this many frames: taken in the
	// first frames of the first walk outdoors in a session, they were the engine's in-between
	// values, and the blend made of them darkened the street to near black for a second after it.
	inline constexpr int kOutdoorsSettledFrames = 30;

	// 1 with the camera in front of the doorway, falling to 0 beside, above or below it, where
	// the player's own space's image space applies alone. By depth alone, anyone level with the
	// door's plane (walking along the street past an open door, or teleported along it) had the
	// whole outdoors drawn with the interior's image space: a white flash.
	[[nodiscard]] inline float InDoorway(const Crossing::Doorway& a_doorway, const Vec3& a_camera)
	{
		const float above = std::abs(a_camera.z - a_doorway.center.z) - a_doorway.halfHeight;
		const auto  fade = [](float a_beyond) { return 1.0f - std::clamp(a_beyond / 100.0f, 0.0f, 1.0f); };
		return fade(Crossing::SideOf(a_doorway, a_camera) - a_doorway.halfWidth) * fade(above);
	}

	// How far the camera is into the interior behind the doorway. The source door's "inward" points
	// away from the player's space: into the interior from outside, out of it from inside.
	[[nodiscard]] inline float InteriorDepth(const Crossing::Doorway& a_doorway, const Vec3& a_camera, bool a_playerInside)
	{
		const float depth = Crossing::DepthOf(a_doorway, a_camera);
		return a_playerInside ? -depth : depth;
	}

	// The share of the interior's image space: 0 is the outdoors' alone, 1 the interior's.
	[[nodiscard]] inline float Weight(float a_interiorDepth, float a_inDoorway, bool a_playerInside)
	{
		const float t = SmoothStep(std::clamp((a_interiorDepth - kFrom) / (kTo - kFrom), 0.0f, 1.0f));
		return a_playerInside ? 1.0f - (1.0f - t) * a_inDoorway : t * a_inDoorway;
	}

	// The player's own space's image space alone: nothing to blend.
	[[nodiscard]] inline bool Settled(float a_weight, bool a_playerInside) { return a_playerInside ? a_weight >= 1.0f : a_weight <= 0.0f; }

	// a_blend = mix(a_outdoors, a_interior, a_weight), except at the indices in a_keep. Eye
	// adaptation's speed and strength stay the current space's own: on the first walk outdoors of
	// a session the engine starts its adaptation afresh, and with these blended it settled near
	// black for a second.
	inline void Mix(std::span<float> a_blend, std::span<const float> a_outdoors, std::span<const float> a_interior, float a_weight, std::span<const std::size_t> a_keep)
	{
		for (std::size_t i = 0; i < a_blend.size(); ++i) {
			if (std::ranges::find(a_keep, i) != a_keep.end()) {
				continue;
			}
			a_blend[i] = a_outdoors[i] + (a_interior[i] - a_outdoors[i]) * a_weight;
		}
	}

	// The outdoors' image space as the engine last had it holds for the weather and hour it was
	// taken in. The hour only counts indoors: outdoors it is taken every frame, except while the
	// blend is on, and standing at a doorway for half an hour mustn't swap it for the weather's.
	[[nodiscard]] inline bool OutdoorsHolds(bool a_sameWeather, bool a_playerInside, float a_hoursSinceTaken)
	{
		return a_sameWeather && (!a_playerInside || std::abs(a_hoursSinceTaken) < kOutdoorsHoldsForHours);
	}

	// Which of the weather's four image spaces is the day's by the hour.
	enum class WeatherTime
	{
		kNight,
		kSunrise,
		kDay,
		kSunset,
	};
	[[nodiscard]] inline WeatherTime WeatherTimeOf(float a_hour)
	{
		return a_hour < 5.0f || a_hour >= 21.0f ? WeatherTime::kNight : a_hour < 8.0f ? WeatherTime::kSunrise : a_hour < 17.0f ? WeatherTime::kDay : WeatherTime::kSunset;
	}
}
