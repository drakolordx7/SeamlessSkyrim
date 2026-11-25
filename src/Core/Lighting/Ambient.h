#pragma once

#include "Core/Math/Transform.h"

// How a space's light is worked out from plain numbers; reading them out of the game is the
// caller's job.
namespace Core::Lighting
{
	// A directional ambient: the colour of a surface facing each way along an axis, a specular tint
	// and a fresnel power.
	struct Ambient
	{
		float colors[3][2][3];  // [x/y/z][max/min][rgb]
		float tint[3];
		float fresnel;
	};

	// The transform the engine's shaders read to get the ambient for a surface facing n:
	// rotate * n + translate. Per colour channel c and axis a: rotate[c][a] = (min - max) / 2,
	// translate[c] = (K[c] + sum over the axes of (max + min)) / 6, with K the engine's three constants.
	[[nodiscard]] inline Transform AmbientTransform(const Ambient& a_ambient, const float* a_offsets)
	{
		Transform transform;
		float     translate[3];
		for (int channel = 0; channel < 3; ++channel) {
			float sum = a_offsets[channel];
			for (int axis = 0; axis < 3; ++axis) {
				const float max = a_ambient.colors[axis][0][channel];
				const float min = a_ambient.colors[axis][1][channel];
				transform.rotate.entry[channel][axis] = 0.5f * (min - max);
				sum += max + min;
			}
			translate[channel] = sum / 6.0f;
		}
		transform.translate = { translate[0], translate[1], translate[2] };
		return transform;
	}

	// The sun's place on its arc, from the hour against the sunrise and sunset (their middles, in
	// hours): from east (+1) to west (-1) by day, and back by night.
	[[nodiscard]] inline float SunArc(float a_hour, float a_hoursPerDay, float a_sunrise, float a_sunset)
	{
		if (a_hour > a_sunrise && a_hour < a_sunset) {
			return 1.0f - 2.0f * (a_hour - a_sunrise) / (a_sunset - a_sunrise);
		}
		const float intoNight = a_hour >= a_sunset ? a_hour - a_sunset : a_hoursPerDay - a_sunset + a_hour;
		return 2.0f * intoNight / (a_hoursPerDay - (a_sunset - a_sunrise)) - 1.0f;
	}
}
