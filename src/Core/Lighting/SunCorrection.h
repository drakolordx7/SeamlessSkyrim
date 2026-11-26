#pragma once

#include "Core/Math/Vec3.h"

#include <array>

// The sun's direction outdoors as the game really has it, learned against the formula we compute.
// Indoors the sky updates no sun, so the street seen from an interior is lit from a direction we
// work out (Render/SpaceLighting.cpp, SunDirections::Outdoors). Mods that move the sun's path (Community
// Shaders' Sky Sync, which draws it on another arc and clamps its height) make the formula wrong by
// some degrees. Whenever the player is outdoors the engine's own light is compared with the formula
// and the difference in azimuth and elevation kept by the hour; indoors the formula's direction gets
// the difference of its hour added. Without such a mod the difference is zero and nothing changes.
namespace Core::Lighting
{
	class SunCorrection
	{
	public:
		static constexpr int   kBuckets = 48;  // half hours
		static constexpr float kHoursPerBucket = 0.5f;

		// a_formula and a_actual: the way the light travels, a_hour 0..24.
		void Observe(float a_hour, const Vec3& a_formula, const Vec3& a_actual)
		{
			Bucket& bucket = buckets[BucketOf(a_hour)];
			const float dAz = Wrap(Azimuth(a_actual) - Azimuth(a_formula));
			const float dEl = Elevation(a_actual) - Elevation(a_formula);
			if (!bucket.valid) {
				bucket = { dAz, dEl, true };
			} else {
				bucket.azimuth += 0.3f * Wrap(dAz - bucket.azimuth);
				bucket.elevation += 0.3f * (dEl - bucket.elevation);
			}
		}

		// a_formula with the difference learned for a_hour added.
		[[nodiscard]] Vec3 Apply(float a_hour, const Vec3& a_formula) const
		{
			const auto [dAz, dEl] = DifferenceAt(a_hour);
			if (dAz == 0.0f && dEl == 0.0f) {
				return a_formula;
			}
			const float az = Azimuth(a_formula) + dAz;
			const float el = std::clamp(Elevation(a_formula) + dEl, -0.5f * kPi, 0.5f * kPi);
			return { std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el) };
		}

		[[nodiscard]] int Samples() const
		{
			return static_cast<int>(std::ranges::count_if(buckets, [](const Bucket& a_bucket) { return a_bucket.valid; }));
		}

		void Reset() { buckets = {}; }

	private:
		struct Bucket
		{
			float azimuth = 0.0f;
			float elevation = 0.0f;
			bool  valid = false;
		};
		std::array<Bucket, kBuckets> buckets{};

		static float Azimuth(const Vec3& a_direction) { return std::atan2(a_direction.y, a_direction.x); }
		static float Elevation(const Vec3& a_direction) { return std::asin(std::clamp(a_direction.z / std::max(a_direction.Length(), 1e-6f), -1.0f, 1.0f)); }
		static float Wrap(float a_angle)
		{
			while (a_angle > kPi) {
				a_angle -= 2.0f * kPi;
			}
			while (a_angle < -kPi) {
				a_angle += 2.0f * kPi;
			}
			return a_angle;
		}
		static int BucketOf(float a_hour)
		{
			const int bucket = static_cast<int>(std::floor(a_hour / kHoursPerBucket));
			return ((bucket % kBuckets) + kBuckets) % kBuckets;
		}

		// The difference between the nearest learned hours before and after a_hour, by distance in
		// hours; held out to 2 hours from the nearest, gone by 5: the sun's arc is smooth, but the
		// hours between two visits are not known.
		[[nodiscard]] std::pair<float, float> DifferenceAt(float a_hour) const
		{
			const float position = a_hour / kHoursPerBucket - 0.5f;  // a bucket's centre is its sample's hour
			const int   home = static_cast<int>(std::floor(position));
			int         before = -1, after = -1;
			float       distanceBefore = 0.0f, distanceAfter = 0.0f;
			for (int step = 0; step < kBuckets && (before < 0 || after < 0); ++step) {
				if (before < 0 && buckets[(((home - step) % kBuckets) + kBuckets) % kBuckets].valid) {
					before = (((home - step) % kBuckets) + kBuckets) % kBuckets;
					distanceBefore = (position - static_cast<float>(home - step)) * kHoursPerBucket;
				}
				if (after < 0 && buckets[(((home + 1 + step) % kBuckets) + kBuckets) % kBuckets].valid) {
					after = (((home + 1 + step) % kBuckets) + kBuckets) % kBuckets;
					distanceAfter = (static_cast<float>(home + 1 + step) - position) * kHoursPerBucket;
				}
			}
			if (before < 0) {
				return { 0.0f, 0.0f };
			}
			const Bucket& b = buckets[before];
			const Bucket& a = buckets[after];
			const float   span = distanceBefore + distanceAfter;
			const float   mix = span > 1e-6f ? distanceBefore / span : 0.0f;
			const float   az = b.azimuth + mix * Wrap(a.azimuth - b.azimuth);
			const float   el = b.elevation + mix * (a.elevation - b.elevation);
			const float   confidence = 1.0f - std::clamp((std::min(distanceBefore, distanceAfter) - 2.0f) / 3.0f, 0.0f, 1.0f);
			return { az * confidence, el * confidence };
		}
	};
}
