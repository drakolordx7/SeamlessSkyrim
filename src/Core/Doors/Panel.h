#pragma once

#include <algorithm>
#include <array>
#include <cfloat>
#include <climits>
#include <cstddef>
#include <span>
#include <vector>

#include "Core/Math/Shapes.h"
#include "Core/Math/Transform.h"

// A door's panel as measured from its mesh, and what is read off the measurement. A panel is a box
// in the door's own space (z up) that is thin along x or y; corner bits: 1 = max x, 2 = max y, 4 = max z.
namespace Core::Doors
{
	struct Box
	{
		Vec3 min{ FLT_MAX, FLT_MAX, FLT_MAX };
		Vec3 max{ -FLT_MAX, -FLT_MAX, -FLT_MAX };

		void Add(const Vec3& a_point)
		{
			min = { std::min(min.x, a_point.x), std::min(min.y, a_point.y), std::min(min.z, a_point.z) };
			max = { std::max(max.x, a_point.x), std::max(max.y, a_point.y), std::max(max.z, a_point.z) };
		}
		[[nodiscard]] bool Empty() const { return min.x > max.x; }
		[[nodiscard]] bool ThinAlongX() const { return (max.x - min.x) < (max.y - min.y); }
	};

	// The box's eight corners put through a_frame (door space -> cell space).
	[[nodiscard]] std::array<Vec3, 8> BoxCorners(const Box& a_box, const Transform& a_frame);

	// A model isn't always made standing: the rotation that makes its upward axis z, by quarter
	// turns. a_up is the world's z written in the model's axes.
	[[nodiscard]] Mat3 UprightRotation(const Vec3& a_up);

	struct PanelSize
	{
		float halfWidth = 0;
		float halfDepth = 0;  // half the panel's thickness
		float halfHeight = 0;
	};
	// The panel's extent, scaled the way the door is placed.
	[[nodiscard]] PanelSize SizeOf(const Box& a_panel, bool a_thinX, float a_scale);

	// The doorway as a flat quad 2 units off one face of the panel: the viewer's face, or with
	// a_behindPanel the far one. a_corners are the panel's corners placed in the cell.
	[[nodiscard]] Quad QuadOnFace(const std::array<Vec3, 8>& a_corners, bool a_thinX, const Vec3& a_viewer, bool a_behindPanel);
	// The panel's box widened across and up (not along its faces) to hold the fixed shapes' boxes.
	[[nodiscard]] Box WholeMesh(const Box& a_panel, bool a_thinX, std::span<const Box> a_fixed);

	struct Triangle
	{
		Vec3 a, b, c;
		int  leaf;
	};
	struct PanelFaces
	{
		int               front = INT_MAX;  // the panel's faces along its thin axis, in whole units
		int               back = INT_MIN;
		std::vector<bool> oneSided;  // per leaf: modelled from one side only

		[[nodiscard]] bool Found() const { return front <= back; }
	};
	// Where the panel's faces are: surface facing along the thin axis, summed by where it is on that
	// axis (1 unit steps); the outermost steps holding a quarter of the most are the faces. A leaf
	// whose surface faces one way only (a tenth the other way at most) has no back.
	[[nodiscard]] PanelFaces FindFaces(std::span<const Triangle> a_triangles, bool a_thinX, std::size_t a_leafCount);
	// The panel's thin extent cut to its faces.
	void ClampToFaces(Box& a_panel, bool a_thinX, const PanelFaces& a_faces);

	// One leaf of a door that swings.
	struct LeafSwing
	{
		Box   box;                  // the leaf's shapes in door space
		float pivotAlongWidth = 0;  // its origin along the width axis: where its animation hinges it
		bool  isRoot = false;       // the whole door is the leaf (not animated): hinged at its low end
	};
	// The leaf's turn about its hinge in door space, a_amount of a_degrees towards a_room (a
	// horizontal direction in door space).
	[[nodiscard]] Transform LeafTurn(const LeafSwing& a_leaf, bool a_thinX, const Vec3& a_room, float a_amount, float a_degrees);

	// A shape the leaves don't move lies behind the panel, seen from a_front (a point on the side
	// the door's partner marker puts whoever comes through): it reaches out behind the panel and
	// nowhere in front of its face (the dragon door's box starts inside the panel's thickness); a
	// frame reaches out on both sides.
	[[nodiscard]] bool IsBehindPanel(const Box& a_shape, const Box& a_panel, bool a_thinX, const Vec3& a_front);
}
