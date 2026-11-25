#pragma once

#include <array>

#include "Core/Math/Vec3.h"

namespace Core
{
	// The points P with dot(P, normal) == offset; Distance is negative on the side opposite the normal.
	struct Plane
	{
		Vec3  normal;  // unit
		float offset = 0.0f;

		[[nodiscard]] float Distance(const Vec3& a_point) const { return a_point.Dot(normal) - offset; }
	};

	// A doorway's corners as { bottom A, bottom B, top A, top B }: A and B are its two sides.
	using Quad = std::array<Vec3, 4>;
}
