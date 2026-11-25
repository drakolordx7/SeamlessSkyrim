#pragma once

#include "Core/Math/Vec3.h"

namespace Core
{
	// Row-major: entry[row][column]. Turns a column vector, `rotate * v`.
	struct Mat3
	{
		float entry[3][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };

		[[nodiscard]] static Mat3 FromRows(const Vec3& a_row0, const Vec3& a_row1, const Vec3& a_row2)
		{
			Mat3 m;
			for (int c = 0; c < 3; ++c) {
				m.entry[0][c] = a_row0[c];
				m.entry[1][c] = a_row1[c];
				m.entry[2][c] = a_row2[c];
			}
			return m;
		}
		[[nodiscard]] static Mat3 Zero() { return FromRows({}, {}, {}); }

		// A frame turned a_radians about z, as a turn of vectors: rotates (1,0,0) towards (cos, sin, 0).
		[[nodiscard]] static Mat3 TurnZ(float a_radians)
		{
			const float c = std::cos(a_radians), s = std::sin(a_radians);
			return FromRows({ c, -s, 0.0f }, { s, c, 0.0f }, { 0.0f, 0.0f, 1.0f });
		}
		// The frame whose local +y points along heading a_yaw (forward = (sin, cos)), +z up: its
		// columns are the local axes in the outer frame.
		[[nodiscard]] static Mat3 FacingYaw(float a_yaw)
		{
			const float s = std::sin(a_yaw), c = std::cos(a_yaw);
			return FromRows({ c, s, 0.0f }, { -s, c, 0.0f }, { 0.0f, 0.0f, 1.0f });
		}

		[[nodiscard]] Mat3 Transpose() const
		{
			Mat3 t;
			for (int r = 0; r < 3; ++r) {
				for (int c = 0; c < 3; ++c) {
					t.entry[r][c] = entry[c][r];
				}
			}
			return t;
		}

		[[nodiscard]] Vec3 operator*(const Vec3& a_v) const
		{
			return { entry[0][0] * a_v.x + entry[0][1] * a_v.y + entry[0][2] * a_v.z,
				entry[1][0] * a_v.x + entry[1][1] * a_v.y + entry[1][2] * a_v.z,
				entry[2][0] * a_v.x + entry[2][1] * a_v.y + entry[2][2] * a_v.z };
		}

		[[nodiscard]] Mat3 operator*(const Mat3& a_rhs) const
		{
			Mat3 m;
			for (int r = 0; r < 3; ++r) {
				for (int c = 0; c < 3; ++c) {
					m.entry[r][c] = entry[r][0] * a_rhs.entry[0][c] + entry[r][1] * a_rhs.entry[1][c] + entry[r][2] * a_rhs.entry[2][c];
				}
			}
			return m;
		}
	};
}
