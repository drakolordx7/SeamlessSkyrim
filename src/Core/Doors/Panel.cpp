#include "Core/Doors/Panel.h"

#include <cmath>
#include <map>

namespace Core::Doors
{
	std::array<Vec3, 8> BoxCorners(const Box& a_box, const Transform& a_frame)
	{
		std::array<Vec3, 8> corners;
		for (int i = 0; i < 8; ++i) {
			const Vec3 local{ i & 1 ? a_box.max.x : a_box.min.x, i & 2 ? a_box.max.y : a_box.min.y, i & 4 ? a_box.max.z : a_box.min.z };
			corners[i] = a_frame * local;
		}
		return corners;
	}

	Mat3 UprightRotation(const Vec3& a_up)
	{
		const float along[3] = { a_up.x, a_up.y, a_up.z };
		int         axis = 2;
		for (int i = 0; i < 2; ++i) {
			if (std::abs(along[i]) > std::abs(along[axis])) {
				axis = i;
			}
		}
		const float sign = along[axis] >= 0.0f ? 1.0f : -1.0f;
		Mat3        upright = Mat3::Zero();
		upright.entry[0][(axis + 1) % 3] = sign;  // the sign twice: still a turn, not a mirror
		upright.entry[1][(axis + 2) % 3] = 1.0f;
		upright.entry[2][axis] = sign;
		return upright;
	}

	PanelSize SizeOf(const Box& a_panel, bool a_thinX, float a_scale)
	{
		const float x = a_panel.max.x - a_panel.min.x, y = a_panel.max.y - a_panel.min.y;
		return { 0.5f * a_scale * (a_thinX ? y : x), 0.5f * a_scale * (a_thinX ? x : y), 0.5f * a_scale * (a_panel.max.z - a_panel.min.z) };
	}

	Quad QuadOnFace(const std::array<Vec3, 8>& a_corners, bool a_thinX, const Vec3& a_viewer, bool a_behindPanel)
	{
		const auto& c = a_corners;
		const int   thinBit = a_thinX ? 1 : 2;
		const int   wideBit = a_thinX ? 2 : 1;
		Vec3        outward = c[thinBit] - c[0];
		outward.Unitize();
		const bool maxSide = (a_viewer - (c[0] + c[7]) * 0.5f).Dot(outward) > 0.0f;
		// The panel's max face is the viewer's when maxSide.
		const bool maxFace = maxSide != a_behindPanel;
		const int  face = maxFace ? thinBit : 0;
		const auto shift = outward * (maxFace ? 2.0f : -2.0f);
		return { c[face] + shift, c[face | wideBit] + shift, c[face | 4] + shift, c[face | wideBit | 4] + shift };
	}

	Box WholeMesh(const Box& a_panel, bool a_thinX, std::span<const Box> a_fixed)
	{
		Box box = a_panel;
		for (const auto& extent : a_fixed) {
			(a_thinX ? box.min.y : box.min.x) = std::min(a_thinX ? box.min.y : box.min.x, a_thinX ? extent.min.y : extent.min.x);
			(a_thinX ? box.max.y : box.max.x) = std::max(a_thinX ? box.max.y : box.max.x, a_thinX ? extent.max.y : extent.max.x);
			box.min.z = std::min(box.min.z, extent.min.z);
			box.max.z = std::max(box.max.z, extent.max.z);
		}
		return box;
	}

	PanelFaces FindFaces(std::span<const Triangle> a_triangles, bool a_thinX, std::size_t a_leafCount)
	{
		std::map<int, float> facing;
		for (const auto& t : a_triangles) {
			const auto  normal = (t.b - t.a).Cross(t.c - t.a);  // length: twice the area
			const float along = 0.5f * std::abs(a_thinX ? normal.x : normal.y);
			const float at = (a_thinX ? t.a.x + t.b.x + t.c.x : t.a.y + t.b.y + t.c.y) / 3.0f;
			facing[static_cast<int>(std::floor(at))] += along;
		}
		float most = 0.0f;
		for (const auto& [at, area] : facing) {
			most = std::max(most, area);
		}
		PanelFaces faces;
		for (const auto& [at, area] : facing) {
			if (area >= 0.25f * most) {
				faces.front = std::min(faces.front, at);
				faces.back = std::max(faces.back, at);
			}
		}
		std::vector<std::array<float, 2>> sides(a_leafCount);
		for (const auto& t : a_triangles) {
			const auto  normal = (t.b - t.a).Cross(t.c - t.a);
			const float along = a_thinX ? normal.x : normal.y;
			sides[t.leaf][along >= 0.0f ? 0 : 1] += std::abs(along);
		}
		faces.oneSided.resize(a_leafCount);
		for (std::size_t i = 0; i < a_leafCount; ++i) {
			faces.oneSided[i] = std::min(sides[i][0], sides[i][1]) < 0.1f * std::max(sides[i][0], sides[i][1]);
		}
		return faces;
	}

	void ClampToFaces(Box& a_panel, bool a_thinX, const PanelFaces& a_faces)
	{
		if (!a_faces.Found()) {
			return;
		}
		auto& low = a_thinX ? a_panel.min.x : a_panel.min.y;
		auto& high = a_thinX ? a_panel.max.x : a_panel.max.y;
		low = std::max(low, static_cast<float>(a_faces.front));
		high = std::min(high, static_cast<float>(a_faces.back + 1));
	}

	Transform LeafTurn(const LeafSwing& a_leaf, bool a_thinX, const Vec3& a_room, float a_amount, float a_degrees)
	{
		const bool  wideX = !a_thinX;
		const auto& box = a_leaf.box;
		const float low = wideX ? box.min.x : box.min.y;
		const float high = wideX ? box.max.x : box.max.y;
		const bool  hingeHigh = !a_leaf.isRoot && std::abs(a_leaf.pivotAlongWidth - high) < std::abs(a_leaf.pivotAlongWidth - low);
		const float hingeEnd = hingeHigh ? high : low;
		const float span = (high - low) * (hingeHigh ? -1.0f : 1.0f);
		const float thinMiddle = wideX ? 0.5f * (box.min.y + box.max.y) : 0.5f * (box.min.x + box.max.x);
		const Vec3  hinge = wideX ? Vec3{ hingeEnd, thinMiddle, 0.0f } : Vec3{ thinMiddle, hingeEnd, 0.0f };
		const Vec3  free = wideX ? Vec3{ span, 0.0f, 0.0f } : Vec3{ 0.0f, span, 0.0f };
		const float sign = (-free.y * a_room.x + free.x * a_room.y) >= 0.0f ? 1.0f : -1.0f;  // the free end turns towards the room
		Transform   turn;
		turn.rotate = Mat3::TurnZ(sign * a_amount * DegToRad(a_degrees));
		turn.translate = hinge - turn.rotate * hinge;
		return turn;
	}

	bool IsBehindPanel(const Box& a_shape, const Box& a_panel, bool a_thinX, const Vec3& a_front)
	{
		const auto  along = [&](const Vec3& a_point) { return a_thinX ? a_point.x : a_point.y; };
		const float low = along(a_panel.min), high = along(a_panel.max);
		const bool  frontHigh = along(a_front) > 0.5f * (low + high);
		return frontHigh ? along(a_shape.max) <= high + 1.0f && along(a_shape.min) < low : along(a_shape.min) >= low - 1.0f && along(a_shape.max) > high;
	}
}
