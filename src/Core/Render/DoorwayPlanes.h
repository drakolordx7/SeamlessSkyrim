#pragma once

#include "Core/Render/Types.h"

#include <algorithm>
#include <iterator>
#include <span>

// The space seen through a doorway, as planes. Shared by whatever cuts draws at these planes and
// whatever leaves out whole shapes beyond them: both must agree on what is seen, or a shape that
// would be drawn gets culled.
//
// With n the door's normal pointing away from the camera's space:
//   X = { P : P lies beyond the door plane (by a margin) and the line of sight from the camera
//         to P passes through the doorway's rectangle }
// X is convex: the door plane and the four planes through the camera and the doorway's edges.
namespace Core::Doorway
{
	using Door = Render::DoorRect;

	// Within this of the door plane a pixel stays with the camera's space (the two spaces' door
	// frames stand in the same place and would z-fight).
	inline constexpr double kMargin = 5.0;
	// Bounds are trusted only this far: a shape counts as wholly on one side of a plane only when
	// its bounding sphere is clear of it by more than this.
	inline constexpr double kBoundSlack = 50.0;

	// Kept where dot(n, P) + d >= 0 (P in world coordinates).
	struct Plane
	{
		double n[3]{};
		double d = 0.0;

		[[nodiscard]] double At(const Vec3& a_point) const { return n[0] * a_point.x + n[1] * a_point.y + n[2] * a_point.z + d; }
		[[nodiscard]] Plane  Flipped() const { return { { -n[0], -n[1], -n[2] }, -d }; }
	};

	[[nodiscard]] inline Plane Through(const Vec3& a_point, const Vec3& a_normal)
	{
		Plane plane{ { a_normal.x, a_normal.y, a_normal.z }, 0.0 };
		plane.d = -(plane.n[0] * a_point.x + plane.n[1] * a_point.y + plane.n[2] * a_point.z);
		return plane;
	}

	// A plane through the camera and a doorway edge (a_edgePoint along a_along), facing the
	// doorway's centre.
	[[nodiscard]] inline Plane EdgePlane(const Vec3& a_eye, const Vec3& a_edgePoint, const Vec3& a_along, const Vec3& a_center)
	{
		Vec3        normal = (a_edgePoint - a_eye).Cross(a_along);
		const float length = normal.Length();
		if (length > 1e-6f) {
			normal = normal / length;
		}
		Plane plane = Through(a_eye, normal);
		return plane.At(a_center) >= 0.0 ? plane : plane.Flipped();
	}

	// X's five planes, seen from a_eye. False when X is empty: the camera isn't on its own
	// space's side of the door plane, so no line of sight passes the doorway into the other space.
	[[nodiscard]] inline bool Planes(const Door& a_door, const Vec3& a_eye, Plane (&a_out)[5])
	{
		const auto away = a_door.cameraMerged ? a_door.normal * -1.0f : a_door.normal;
		if ((a_door.center - a_eye).Dot(away) <= 0.0f) {
			return false;
		}
		a_out[0] = Through(a_door.center, away);
		a_out[0].d -= kMargin;
		const Vec3 up{ 0.0f, 0.0f, 1.0f };
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

	[[nodiscard]] inline Side SideOf(const Plane& a_plane, const Vec3& a_center, float a_radius)
	{
		const double at = a_plane.At(a_center);
		const double reach = a_radius + kBoundSlack;
		return at > reach ? Side::kInside : at < -reach ? Side::kOutside : Side::kCrosses;
	}

	// Wholly outside X (any plane cuts the whole bound away), wholly inside (every plane keeps it), or neither.
	[[nodiscard]] inline Side Classify(std::span<const Plane> a_planes, const Vec3& a_center, float a_radius)
	{
		bool within = true;
		for (const auto& plane : a_planes) {
			const auto side = SideOf(plane, a_center, a_radius);
			if (side == Side::kOutside) {
				return Side::kOutside;
			}
			within &= side == Side::kInside;
		}
		return within ? Side::kInside : Side::kCrosses;
	}

	// A kept plane for a render clip plane (negative Distance = cut away).
	[[nodiscard]] inline Plane KeptSide(const Core::Plane& a_clip)
	{
		return { { a_clip.normal.x, a_clip.normal.y, a_clip.normal.z }, -static_cast<double>(a_clip.offset) };
	}

	// X's five planes and a city gate's sixth, where the far gate stands.
	inline constexpr int kMaxPlanes = 6;

	// A convex part of a shape: what every one of its planes keeps.
	struct Piece
	{
		Plane planes[kMaxPlanes];
		int   count = 0;
	};

	// How a shape is drawn: as it is, not at all, or in convex pieces one after the other.
	struct Cut
	{
		enum class Kind
		{
			kWhole,      // as it is
			kWholeSeen,  // as it is, and wholly in view through the doorway
			kLeftOut,    // not at all
			kPieces,     // once per piece (no pieces: not at all)
		};
		Kind  kind = Kind::kWhole;
		Piece pieces[kMaxPlanes];
		int   count = 0;
	};

	// A shape of the space behind a plane that is drawn only where it keeps (the portal pass's far
	// door), by its bounding sphere.
	[[nodiscard]] inline Cut CutAt(const Plane& a_plane, const Vec3& a_center, float a_radius)
	{
		Cut cut;
		switch (SideOf(a_plane, a_center, a_radius)) {
		case Side::kInside:
			break;
		case Side::kOutside:
			cut.kind = Cut::Kind::kLeftOut;
			break;
		case Side::kCrosses:
			cut.kind = Cut::Kind::kPieces;
			cut.count = 1;
			cut.pieces[0].planes[0] = a_plane;
			cut.pieces[0].count = 1;
			break;
		}
		return cut;
	}

	// A shape seen through the doorway from a_eye. A shape of the merged space is kept inside X (one
	// piece); one of the camera's own space outside X, which isn't convex: in up to six disjoint
	// convex pieces (not P0; P0 and not P1; P0, P1 and not P2; ...). a_passage > 0: a city gate, X
	// ends that far beyond the door plane.
	[[nodiscard]] inline Cut CutThroughDoorway(const Door& a_door, const Vec3& a_eye, float a_passage, bool a_shapeMerged, const Vec3& a_center, float a_radius)
	{
		Cut        cut;
		const bool cameraSpace = a_shapeMerged == a_door.cameraMerged;
		Plane      doorway[5];
		if (!Planes(a_door, a_eye, doorway)) {
			if (!cameraSpace) {
				cut.kind = Cut::Kind::kLeftOut;
			}
			return cut;
		}

		Plane planes[kMaxPlanes];
		std::copy(std::begin(doorway), std::end(doorway), planes);
		int count = 5;
		if (a_passage > 0.0f) {
			planes[count++] = Through(a_door.center + a_door.normal * a_passage, a_door.normal * -1.0f);
		}
		Side sides[kMaxPlanes];
		bool clear = false;  // wholly outside X
		bool within = true;  // wholly inside X
		for (int i = 0; i < count; ++i) {
			sides[i] = SideOf(planes[i], a_center, a_radius);
			clear |= sides[i] == Side::kOutside;
			within &= sides[i] == Side::kInside;
		}
		if (!cameraSpace) {
			if (clear) {
				cut.kind = Cut::Kind::kLeftOut;
			} else if (within) {
				cut.kind = Cut::Kind::kWholeSeen;
			} else {
				cut.kind = Cut::Kind::kPieces;
				cut.count = 1;
				std::copy(planes, planes + count, cut.pieces[0].planes);
				cut.pieces[0].count = count;
			}
			return cut;
		}
		if (clear) {
			return cut;
		}
		if (within) {
			cut.kind = Cut::Kind::kLeftOut;
			return cut;
		}
		cut.kind = Cut::Kind::kPieces;
		for (int k = 0; k < count; ++k) {
			// Piece k: planes 0..k-1 kept, plane k cut away.
			bool empty = sides[k] == Side::kInside;
			for (int i = 0; i < k && !empty; ++i) {
				empty = sides[i] == Side::kOutside;
			}
			if (empty) {
				continue;
			}
			Piece& piece = cut.pieces[cut.count++];
			std::copy(planes, planes + k, piece.planes);
			piece.planes[k] = planes[k].Flipped();
			piece.count = k + 1;
		}
		return cut;
	}
}
