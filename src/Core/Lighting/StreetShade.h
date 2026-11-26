#pragma once

#include <optional>

#include "Core/Lighting/Daylight.h"

// Shade the street's sun makes, found in the scene's depth: a march from a surface toward the sun
// that counts samples lying just behind something the camera sees (a screen-space shadow). Two
// uses, both in the doorway daylight pass (Skyrim/Render/DoorwayDaylight.h):
//  - the street merged into an interior has no sun shadow of its own (an interior has no sun shadow
//    map), so the street seen through the doorway is darkened where the march meets a house, an
//    eave or a tree on the way to the sun;
//  - the sunlight patch on an interior's floor is dimmed when the ray to the sun, once through the
//    doorway, runs into the street's own geometry (the eave over the door, the house opposite).
// Plain floats; the pass's pixel shader (Skyrim/Render/DaylightHlsl.h) repeats Hit, SampleAlong,
// SunShare and the product of Lit line for line, and takes the constants from here.
namespace Core::StreetShade
{
	// The street's march: samples along the ray and how far it reaches (a building's shadow is
	// longer than a tree's; the samples crowd near the surface, where contact shadows are).
	inline constexpr int   kSteps = 12;
	inline constexpr float kReach = 700.0f;
	// The patch's march, from the doorway plane out into the street.
	inline constexpr int   kPatchSteps = 8;
	inline constexpr float kPatchReach = 500.0f;
	// A sample counts as inside something when it lies this far behind the surface the camera sees
	// there (less: the surface itself), and not when it lies further than a thickness behind it
	// (then the ray passes behind a thin thing, not through a thick one). The thickness grows with
	// the distance along the ray: far shadows come from big things.
	inline constexpr float kOccluderMin = 6.0f;
	inline constexpr float kThicknessBase = 40.0f;
	inline constexpr float kThicknessPerUnit = 0.35f;
	// What one sample inside something takes of the light.
	inline constexpr float kHit = 0.6f;
	// The darkest a shadow makes a surface that is lit by the sun alone (shade isn't black: the
	// sky fills it, and the march sees only what the camera sees).
	inline constexpr float kMost = 0.8f;
	// Added to the ambient when the sun's share of a surface's light is worked out.
	inline constexpr float kAmbientFloor = 0.05f;

	// 0 to 1: how much a sample at a_gap behind the surface seen there, a_along units along the ray,
	// counts as lying inside something. a_gap: distance of the sample from the camera less the
	// distance of the scene there (above 0: behind what the camera sees).
	[[nodiscard]] inline float Hit(float a_gap, float a_along)
	{
		const float thickness = kThicknessBase + kThicknessPerUnit * a_along;
		return Daylight::Smooth(kOccluderMin, kOccluderMin * 1.7f, a_gap) * (1.0f - Daylight::Smooth(0.7f * thickness, thickness, a_gap));
	}

	// Distance of sample a_index of a_steps along a ray of a_length; a_noise (0 to 1) moves them all.
	[[nodiscard]] inline float SampleAlong(int a_index, float a_noise, int a_steps, float a_length)
	{
		const float x = (static_cast<float>(a_index) + a_noise) / static_cast<float>(a_steps);
		return a_length * x * x;
	}

	// The share of the light that reaches a_start along a_dir (a unit vector toward the sun), 0 to 1.
	// a_gapAt(point): the sample's gap (Hit), or nullopt where the camera sees nothing there (off
	// screen, behind it).
	template <class GapAt>
	[[nodiscard]] float Lit(const Vec3& a_start, const Vec3& a_dir, float a_length, float a_noise, int a_steps, GapAt&& a_gapAt)
	{
		float lit = 1.0f;
		for (int i = 0; i < a_steps; ++i) {
			const float t = SampleAlong(i, a_noise, a_steps, a_length);
			if (const auto gap = a_gapAt(a_start + a_dir * t)) {
				lit *= 1.0f - kHit * Hit(*gap, t);
			}
		}
		return lit;
	}

	// The part of a surface's light that is the sun's: sun luminance times the cosine, against the
	// same and the ambient. The shadow takes this much of it, not the ambient's.
	[[nodiscard]] inline float SunShare(float a_sunLuminance, float a_skyLuminance, float a_cosine)
	{
		const float sun = a_sunLuminance * std::clamp(a_cosine, 0.0f, 1.0f);
		return sun / (sun + a_skyLuminance + kAmbientFloor);
	}

	// The most a shadow darkens a surface lit by the sun alone, as the sun stands (the sine of its
	// elevation) and the door is open (0 shut, 1 wide: a shut door shows no street).
	[[nodiscard]] inline float Strength(float a_towardSunZ, float a_open)
	{
		return kMost * Daylight::Smooth(Daylight::kSunHorizonLow, Daylight::kSunHorizonHigh, a_towardSunZ) * Daylight::Smooth(0.05f, 0.5f, a_open);
	}

	// What a street surface is multiplied by: 1 in the sun, down to 1 - a_strength * share in shade.
	[[nodiscard]] inline float Multiplier(float a_strength, float a_share, float a_lit)
	{
		return 1.0f - a_strength * a_share * (1.0f - a_lit);
	}
}
