#include "Core/Pictures/Camera.h"

#include <algorithm>
#include <cmath>

namespace Core::Pictures::Camera
{
	namespace
	{
		// Its texels per doorway against a picture taken with the player's view: taken once and kept,
		// and seen at more of an angle (stretched further).
		constexpr float kCameraDensity = 2.0f;
	}

	DoorwayPlace PlaceOf(const CameraShape& a_shape, float a_eyeUp)
	{
		return { 0.5f, a_shape.atEye ? std::clamp(a_eyeUp, 0.0f, 1.0f) : 0.5f, -a_shape.back };
	}

	float AspectOf(const Quad& a_corners)
	{
		const float aspect = (a_corners[1] - a_corners[0]).Length() / (a_corners[2] - a_corners[0]).Length();
		return std::clamp(std::round(aspect * 16.0f) / 16.0f, kNarrowest, kWidest);
	}

	std::uint32_t WidthFor(std::uint32_t a_height)
	{
		return a_height * 3 / 4;
	}

	std::pair<std::uint32_t, std::uint32_t> SizeFor(const Extent& a_extent, int a_doorwayHeight, float a_aspect)
	{
		const float density = a_extent != kDoorwayOnly ? kCameraDensity : 1.0f;
		return { static_cast<std::uint32_t>(std::lround(a_doorwayHeight * a_aspect * density * (a_extent[1] - a_extent[0]))),
			static_cast<std::uint32_t>(std::lround(a_doorwayHeight * density * (a_extent[3] - a_extent[2]))) };
	}

	Frame::Frame(const Quad& a_corners, const Vec3& a_eye) :
		origin(a_corners[0]), across(a_corners[1] - a_corners[0]), up(a_corners[2] - a_corners[0])
	{
		inward = across.Cross(up);
		inward.Unitize();
		if (inward.Dot((a_corners[0] + a_corners[3]) * 0.5f - a_eye) < 0.0f) {
			inward = inward * -1.0f;
		}
	}

	DoorwayPlace Frame::PlaceOf(const Vec3& a_point) const
	{
		const auto offset = a_point - origin;
		return { offset.Dot(across) / across.SqrLength(), offset.Dot(up) / up.SqrLength(), offset.Dot(inward) };
	}

	Vec3 Frame::PointAt(const DoorwayPlace& a_place) const
	{
		return origin + across * a_place[0] + up * a_place[1] + inward * a_place[2];
	}

	CaptureCamera For(const Frame& a_frame, const CameraShape& a_shape, const DoorwayPlace& a_place)
	{
		CaptureCamera camera;
		const float   width = a_frame.across.Length();
		const float   height = a_frame.up.Length();
		const auto    forward = a_frame.inward;
		const auto    up = a_frame.up / height;
		const auto    right = forward.Cross(up);  // an NiCamera's columns: forward, up, right
		for (int row = 0; row < 3; ++row) {
			camera.transform.rotate.entry[row][0] = forward[row];
			camera.transform.rotate.entry[row][1] = up[row];
			camera.transform.rotate.entry[row][2] = right[row];
		}
		camera.transform.translate = a_frame.PointAt(a_place);
		camera.transform.scale = 1.0f;
		// The doorway's across may run either way on screen.
		const float acrossOnScreen = right.Dot(a_frame.across) / width;
		const float side0 = acrossOnScreen * (a_shape.extent[0] - a_place[0]) * width / a_shape.back;
		const float side1 = acrossOnScreen * (a_shape.extent[1] - a_place[0]) * width / a_shape.back;
		camera.left = std::min(side0, side1);
		camera.right = std::max(side0, side1);
		camera.bottom = (a_shape.extent[2] - a_place[1]) * height / a_shape.back;
		camera.top = (a_shape.extent[3] - a_place[1]) * height / a_shape.back;
		camera.toDoorway = a_shape.back;
		return camera;
	}

	std::array<float, 4> Clip(const CaptureCamera& a_camera, const Frame& a_frame, float a_across, float a_up)
	{
		const auto  point = a_frame.PointAt({ a_across, a_up, 0.0f });
		const auto  offset = point - a_camera.transform.translate;
		const auto& rotate = a_camera.transform.rotate;
		const float ahead = offset.x * rotate.entry[0][0] + offset.y * rotate.entry[1][0] + offset.z * rotate.entry[2][0];
		const float up = (offset.x * rotate.entry[0][1] + offset.y * rotate.entry[1][1] + offset.z * rotate.entry[2][1]) / ahead;
		const float right = (offset.x * rotate.entry[0][2] + offset.y * rotate.entry[1][2] + offset.z * rotate.entry[2][2]) / ahead;
		return { 2.0f * (right - a_camera.left) / (a_camera.right - a_camera.left) - 1.0f, 2.0f * (up - a_camera.bottom) / (a_camera.top - a_camera.bottom) - 1.0f, 0.0f, 1.0f };
	}
}
