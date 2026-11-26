#pragma once

#include "Core/Math/Vec3.h"

// The street's daylight coming into an interior through an open doorway: where the sun's light
// that passes the opening lands, how much of the opening a surface sees, and how strong each is.
// Plain floats; the doorway pass's pixel shader (Skyrim/Render/DaylightHlsl.h) repeats SunLit and
// SkyView line for line, and builds its constants from the ones here.
namespace Core::Daylight
{
	// The doorway between the street and an interior, in the coordinates of the space the camera
	// is in. inward points from the street into the interior and, like right, is horizontal and
	// of length 1. The opening is a rectangle: centre +- halfWidth along right, +- halfHeight up.
	struct Opening
	{
		Vec3  center;
		Vec3  inward;
		Vec3  right;
		float halfWidth = 0.0f;
		float halfHeight = 0.0f;

		static Opening Upright(const Vec3& a_center, const Vec3& a_inward, float a_halfWidth, float a_halfHeight)
		{
			return { a_center, { a_inward.x, a_inward.y, 0.0f }, { a_inward.y, -a_inward.x, 0.0f }, a_halfWidth, a_halfHeight };
		}
	};

	// How far from the doorway, along the sun's ray, a patch of sunlight is drawn. Beyond it the
	// patch fades out over a quarter of the distance: the ray left the building's reach long ago
	// (the street beyond has its own occluders that nothing here knows).
	inline constexpr float kReach = 1400.0f;
	// A surface this far into the interior is fully inside it; nearer the doorway the light fades in,
	// so a surface level with the opening takes none.
	inline constexpr float kDepthFade = 8.0f;
	// The soft edge of a patch, in units at the doorway, plus how much it grows per unit of ray.
	inline constexpr float kEdgeBase = 6.0f;
	inline constexpr float kEdgeGrowth = 0.02f;
	// The sun lower than this (the sine of its elevation against the plane) is a grazing ray: it
	// fades out between kGrazingLow and kGrazingHigh, so a low sun doesn't throw shimmering slivers.
	inline constexpr float kGrazingLow = 0.02f;
	inline constexpr float kGrazingHigh = 0.12f;
	// The sun is above the horizon: it lights nothing below kSunHorizonLow, fully above kSunHorizonHigh.
	inline constexpr float kSunHorizonLow = 0.02f;
	inline constexpr float kSunHorizonHigh = 0.15f;

	[[nodiscard]] inline float Smooth(float a_low, float a_high, float a_x)
	{
		const float t = std::clamp((a_x - a_low) / (a_high - a_low), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	// How much of the sun's light reaches a_point (0 to 1): the share of it that comes through the
	// opening, with a soft edge, for a surface inside the interior. a_towardSun: unit vector from
	// the surface to the sun. Occlusion inside the room (furniture, a wall beside) is the
	// pass's to test against the depth it has.
	[[nodiscard]] inline float SunLit(const Opening& a_opening, const Vec3& a_towardSun, const Vec3& a_point)
	{
		const Vec3  rel = a_point - a_opening.center;
		const float depth = rel.Dot(a_opening.inward);
		const float along = a_towardSun.Dot(a_opening.inward);  // below 0: the sun is on the street's side
		if (depth <= 0.0f || along >= -kGrazingLow) {
			return 0.0f;
		}
		const float t = depth / -along;  // distance along the ray to the doorway's plane
		if (t >= kReach) {
			return 0.0f;
		}
		const Vec3  hit = rel + a_towardSun * t;
		const float edge = kEdgeBase + kEdgeGrowth * t;
		const float u = std::abs(hit.Dot(a_opening.right));
		const float v = std::abs(hit.z);
		const float inside = (1.0f - Smooth(a_opening.halfWidth - edge, a_opening.halfWidth, u)) * (1.0f - Smooth(a_opening.halfHeight - edge, a_opening.halfHeight, v));
		return inside * (1.0f - Smooth(0.75f * kReach, kReach, t)) * Smooth(0.0f, kDepthFade, depth) * Smooth(kGrazingLow, kGrazingHigh, -along);
	}

	// How much of the opening a_normal-facing surface at a_point sees, as the share of its
	// light the street's sky sends through it (0 to 1): the opening taken as one small source at its
	// centre, cosines at both ends, over pi r^2 + the opening's area (so it stays below 1 at the
	// doorway itself). A zero a_normal is a surface of unknown facing, taken at 0.5.
	[[nodiscard]] inline float SkyView(const Opening& a_opening, const Vec3& a_point, const Vec3& a_normal)
	{
		const Vec3  rel = a_point - a_opening.center;
		const float r2 = std::max(rel.SqrLength(), 100.0f);
		const float r = std::sqrt(r2);
		const float atOpening = rel.Dot(a_opening.inward) / r;
		if (atOpening <= 0.0f) {
			return 0.0f;
		}
		const float atSurface = a_normal.SqrLength() > 0.0f ? std::max(-rel.Dot(a_normal) / r, 0.0f) : 0.5f;
		const float area = 4.0f * a_opening.halfWidth * a_opening.halfHeight;
		return std::clamp(area * atOpening * atSurface / (kPi * r2 + area), 0.0f, 1.0f);
	}

	// How strongly the light is added, as a multiple of what the surface already shows (the
	// pass brightens the scene by 1 + gain: the surface's own colour, in the sun's tint).
	struct Gains
	{
		float sun = 0.0f;    // a surface the sun reaches, facing it
		float spill = 0.0f;  // a surface facing the opening at its centre, next to it
	};

	inline constexpr float kSunGainPerLuminance = 1.0f;
	inline constexpr float kSunGainMost = 3.0f;
	inline constexpr float kSpillGainPerLuminance = 2.0f;
	inline constexpr float kSpillGainMost = 0.9f;

	// a_sunLuminance: the street's sun as the main view is lit with it (its colour times the
	// outdoors' sunlight scale), a_skyLuminance: its ambient light, a_towardSunZ: the sun's
	// elevation (the z of the unit vector to it), a_open: how far the door stands open (0 shut, 1
	// wide): a shut door lets in nothing, and a door only just ajar little.
	[[nodiscard]] inline Gains GainsOf(float a_sunLuminance, float a_skyLuminance, float a_towardSunZ, float a_open)
	{
		const float open = Smooth(0.05f, 0.5f, a_open);
		Gains       gains;
		gains.sun = std::clamp(a_sunLuminance * kSunGainPerLuminance, 0.0f, kSunGainMost) * Smooth(kSunHorizonLow, kSunHorizonHigh, a_towardSunZ) * open;
		gains.spill = std::clamp(a_skyLuminance * kSpillGainPerLuminance, 0.0f, kSpillGainMost) * open;
		return gains;
	}

	[[nodiscard]] inline float Luminance(float a_red, float a_green, float a_blue)
	{
		return 0.2126f * a_red + 0.7152f * a_green + 0.0722f * a_blue;
	}
}
