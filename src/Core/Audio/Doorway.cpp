#include "Core/Audio/Doorway.h"

namespace Core::Audio
{
	Heard ThroughDoorway(const Vec3& a_listener, const Doorway& a_doorway, float a_open, const Tuning& a_tuning)
	{
		Heard heard;
		const Vec3  offset = a_listener - a_doorway.center;
		heard.distance = offset.Length();
		// Past the door plane the listener is in the room.
		const float depth = offset.Dot(a_doorway.inward);
		const float side = std::clamp(-depth / a_tuning.sideBlend, 0.0f, 1.0f);
		if (a_open < a_tuning.minOpen || side <= 0.0f || heard.distance >= a_tuning.maxDistance) {
			heard.muffle = 1.0f;
			return heard;
		}

		heard.cosine = heard.distance > 1.0f ? std::clamp(-depth / heard.distance, 0.0f, 1.0f) : 1.0f;
		const float open = SmoothStep(std::clamp(a_open, 0.0f, 1.0f));

		const float ratio = heard.distance / a_tuning.refDistance;
		float       distance = 1.0f / (1.0f + ratio * ratio);
		// Out to nothing over the last quarter of the reach, not at its edge.
		const float fadeStart = a_tuning.maxDistance * 0.75f;
		if (heard.distance > fadeStart) {
			distance *= 1.0f - SmoothStep((heard.distance - fadeStart) / (a_tuning.maxDistance - fadeStart));
		}
		const float angle = a_tuning.sideFloor + (1.0f - a_tuning.sideFloor) * std::pow(heard.cosine, 0.75f);

		heard.muffle = std::clamp(0.35f * heard.distance / a_tuning.maxDistance + 0.5f * (1.0f - heard.cosine) + 0.3f * (1.0f - open), 0.0f, 1.0f);
		heard.gain = distance * angle * open * SmoothStep(side) * (1.0f - a_tuning.muffleDepth * heard.muffle);
		return heard;
	}

	float LowPassCutoff(float a_muffle, const Tuning& a_tuning)
	{
		const float muffle = std::clamp(a_muffle, 0.0f, 1.0f);
		return a_tuning.clearCutoff * std::pow(a_tuning.mutedCutoff / a_tuning.clearCutoff, muffle);
	}

	float FilterFrequency(float a_cutoffHz, float a_sampleRate)
	{
		constexpr float pi = 3.14159265358979f;
		return std::clamp(2.0f * std::sin(pi * a_cutoffHz / a_sampleRate), 0.0f, 1.0f);
	}

	float RoomFalloff(float a_distance, const Tuning& a_tuning)
	{
		const float ratio = a_distance / a_tuning.roomRefDistance;
		return 1.0f / (1.0f + ratio * ratio);
	}
}
