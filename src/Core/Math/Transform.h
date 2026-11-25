#pragma once

#include "Core/Math/Mat3.h"

namespace Core
{
	// p -> rotate * p * scale + translate.
	struct Transform
	{
		Mat3  rotate;
		Vec3  translate;
		float scale = 1.0f;

		[[nodiscard]] Transform Invert() const
		{
			Transform inverse;
			inverse.rotate = rotate.Transpose();
			inverse.scale = 1.0f / scale;
			inverse.translate = (inverse.rotate * -translate) * inverse.scale;
			return inverse;
		}

		// This one applied after a_rhs.
		[[nodiscard]] Transform operator*(const Transform& a_rhs) const
		{
			Transform t;
			t.scale = scale * a_rhs.scale;
			t.rotate = rotate * a_rhs.rotate;
			t.translate = translate + (rotate * a_rhs.translate) * scale;
			return t;
		}

		[[nodiscard]] Vec3 operator*(const Vec3& a_point) const { return (rotate * a_point) * scale + translate; }
	};
}
