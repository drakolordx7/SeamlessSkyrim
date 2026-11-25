#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

// Plain-float geometry for everything game-independent. The conventions are the Creation Engine's:
// right-handed, z up, a heading is a rotation about z with forward = (sin, cos, 0).
namespace Core
{
	inline constexpr float kPi = std::numbers::pi_v<float>;

	[[nodiscard]] inline float DegToRad(float a_degrees) { return a_degrees * (kPi / 180.0f); }
	[[nodiscard]] inline float RadToDeg(float a_radians) { return a_radians * (180.0f / kPi); }

	struct Vec3
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;

		[[nodiscard]] float operator[](std::size_t a_index) const { return a_index == 0 ? x : a_index == 1 ? y : z; }

		[[nodiscard]] Vec3 operator+(const Vec3& a_rhs) const { return { x + a_rhs.x, y + a_rhs.y, z + a_rhs.z }; }
		[[nodiscard]] Vec3 operator-(const Vec3& a_rhs) const { return { x - a_rhs.x, y - a_rhs.y, z - a_rhs.z }; }
		[[nodiscard]] Vec3 operator-() const { return { -x, -y, -z }; }
		[[nodiscard]] Vec3 operator*(float a_scalar) const { return { x * a_scalar, y * a_scalar, z * a_scalar }; }
		[[nodiscard]] Vec3 operator/(float a_scalar) const { return { x / a_scalar, y / a_scalar, z / a_scalar }; }
		Vec3&              operator+=(const Vec3& a_rhs) { return *this = *this + a_rhs; }

		[[nodiscard]] float Dot(const Vec3& a_rhs) const { return x * a_rhs.x + y * a_rhs.y + z * a_rhs.z; }
		[[nodiscard]] Vec3  Cross(const Vec3& a_rhs) const { return { y * a_rhs.z - z * a_rhs.y, z * a_rhs.x - x * a_rhs.z, x * a_rhs.y - y * a_rhs.x }; }
		[[nodiscard]] float SqrLength() const { return x * x + y * y + z * z; }
		[[nodiscard]] float Length() const { return std::sqrt(SqrLength()); }
		[[nodiscard]] float DistanceTo(const Vec3& a_rhs) const { return (*this - a_rhs).Length(); }

		// Scaled to length 1; a vector too short to have a direction becomes zero. Returns the old length.
		float Unitize()
		{
			const float length = Length();
			if (length == 1.0f) {
				return length;
			}
			if (length > 1.1920929e-7f) {
				*this = *this / length;
				return length;
			}
			*this = {};
			return 0.0f;
		}

		[[nodiscard]] bool operator==(const Vec3&) const = default;
	};

	[[nodiscard]] inline Vec3 operator*(float a_scalar, const Vec3& a_vector) { return a_vector * a_scalar; }
}
