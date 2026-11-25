#pragma once

#include "Core/Math/Shapes.h"
#include "Core/Math/Transform.h"
#include "Core/Render/Types.h"

// Engine types <-> the game-independent ones in Core. Done at the boundary, once per value.
[[nodiscard]] inline Core::Vec3 ToCore(const RE::NiPoint3& a_point)
{
	return { a_point.x, a_point.y, a_point.z };
}

[[nodiscard]] inline RE::NiPoint3 ToNi(const Core::Vec3& a_vector)
{
	return { a_vector.x, a_vector.y, a_vector.z };
}

[[nodiscard]] inline Core::Mat3 ToCore(const RE::NiMatrix3& a_matrix)
{
	Core::Mat3 m;
	std::memcpy(m.entry, a_matrix.entry, sizeof(m.entry));
	return m;
}

[[nodiscard]] inline RE::NiMatrix3 ToNi(const Core::Mat3& a_matrix)
{
	RE::NiMatrix3 m;
	std::memcpy(m.entry, a_matrix.entry, sizeof(m.entry));
	return m;
}

[[nodiscard]] inline Core::Transform ToCore(const RE::NiTransform& a_transform)
{
	return { ToCore(a_transform.rotate), ToCore(a_transform.translate), a_transform.scale };
}

[[nodiscard]] inline RE::NiTransform ToNi(const Core::Transform& a_transform)
{
	RE::NiTransform t;
	t.rotate = ToNi(a_transform.rotate);
	t.translate = ToNi(a_transform.translate);
	t.scale = a_transform.scale;
	return t;
}

[[nodiscard]] inline Core::Render::Color ToCore(const RE::NiColor& a_color)
{
	return { a_color.red, a_color.green, a_color.blue };
}

[[nodiscard]] inline RE::NiColor ToNi(const Core::Render::Color& a_color)
{
	return { a_color.red, a_color.green, a_color.blue };
}

[[nodiscard]] inline std::array<RE::NiPoint3, 4> ToNi(const Core::Quad& a_quad)
{
	return { ToNi(a_quad[0]), ToNi(a_quad[1]), ToNi(a_quad[2]), ToNi(a_quad[3]) };
}
