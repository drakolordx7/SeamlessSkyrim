#pragma once

#include "Skyrim/Render/Wire.h"

// The space seen through a doorway, as planes. Shared by the native mask (Render/Native/NativeMask.cpp),
// which cuts draws at these planes, and the merged room's cull (Portal/MergedCull.cpp), which
// leaves out whole shapes beyond them: both must agree on what is seen, or a shape the mask would
// draw gets culled.
//
// With n the door's normal pointing away from the camera's space:
//   X = { P : P lies beyond the door plane (by a margin) and the line of sight from the camera
//         to P passes through the doorway's rectangle }
// X is convex: the door plane and the four planes through the camera and the doorway's edges.
namespace Render::Doorway
{
	using Door = DoorRect;

	// Within this of the door plane a pixel stays with the camera's space (the two spaces' door
	// frames stand in the same place and would z-fight). The Community Shaders fork uses the same.
	inline constexpr double kMargin = 5.0;
	// Bounds are trusted only this far: a shape counts as wholly on one side of a plane only when
	// its bounding sphere is clear of it by more than this.
	inline constexpr double kBoundSlack = 50.0;

	// Kept where dot(n, P) + d >= 0 (P in world coordinates).
	struct Plane
	{
		double n[3]{};
		double d = 0.0;

		[[nodiscard]] double At(const RE::NiPoint3& a_point) const { return n[0] * a_point.x + n[1] * a_point.y + n[2] * a_point.z + d; }
		[[nodiscard]] Plane  Flipped() const { return { { -n[0], -n[1], -n[2] }, -d }; }
	};

	inline Plane Through(const RE::NiPoint3& a_point, const RE::NiPoint3& a_normal)
	{
		Plane plane{ { a_normal.x, a_normal.y, a_normal.z }, 0.0 };
		plane.d = -(plane.n[0] * a_point.x + plane.n[1] * a_point.y + plane.n[2] * a_point.z);
		return plane;
	}

	// A plane through the camera and a doorway edge (a_edgePoint along a_along), facing the
	// doorway's centre.
	inline Plane EdgePlane(const RE::NiPoint3& a_eye, const RE::NiPoint3& a_edgePoint, const RE::NiPoint3& a_along, const RE::NiPoint3& a_center)
	{
		RE::NiPoint3 normal = (a_edgePoint - a_eye).Cross(a_along);
		const float  length = normal.Length();
		if (length > 1e-6f) {
			normal = normal / length;
		}
		Plane plane = Through(a_eye, normal);
		return plane.At(a_center) >= 0.0 ? plane : plane.Flipped();
	}

	// X's five planes, seen from a_eye. False when X is empty: the camera isn't on its own
	// space's side of the door plane, so no line of sight passes the doorway into the other space.
	inline bool Planes(const Door& a_door, const RE::NiPoint3& a_eye, Plane (&a_out)[5])
	{
		const auto away = a_door.cameraMerged ? a_door.normal * -1.0f : a_door.normal;
		if ((a_door.center - a_eye).Dot(away) <= 0.0f) {
			return false;
		}
		a_out[0] = Through(a_door.center, away);
		a_out[0].d -= kMargin;
		const RE::NiPoint3 up{ 0.0f, 0.0f, 1.0f };
		a_out[1] = EdgePlane(a_eye, a_door.center + a_door.right * a_door.halfWidth, up, a_door.center);
		a_out[2] = EdgePlane(a_eye, a_door.center - a_door.right * a_door.halfWidth, up, a_door.center);
		a_out[3] = EdgePlane(a_eye, a_door.center + up * a_door.halfHeight, a_door.right, a_door.center);
		a_out[4] = EdgePlane(a_eye, a_door.center - up * a_door.halfHeight, a_door.right, a_door.center);
		return true;
	}

	enum class Side
	{
		kInside,   // the whole bound is on the kept side
		kOutside,  // the whole bound is cut away
		kCrosses,
	};

	inline Side SideOf(const Plane& a_plane, const RE::NiPoint3& a_center, float a_radius)
	{
		const double at = a_plane.At(a_center);
		const double reach = a_radius + kBoundSlack;
		return at > reach ? Side::kInside : at < -reach ? Side::kOutside : Side::kCrosses;
	}

	inline Side SideOf(const Plane& a_plane, const RE::NiBound& a_bound)
	{
		return SideOf(a_plane, a_bound.center, a_bound.radius);
	}
}
