#pragma once

#include <array>
#include <cstdint>
#include <utility>

#include "Core/Math/Shapes.h"
#include "Core/Math/Transform.h"

// Where a doorway's picture is taken from and what part of the door's plane it covers, and how
// many texels it gets.
namespace Core::Pictures::Camera
{
	// A place in the doorway's own terms: across and up as fractions of its width and height from
	// the bottom corner, and the distance behind it (negative in front). The door's corners move
	// with the door between sessions; where the picture was taken from has to move with them.
	using DoorwayPlace = std::array<float, 3>;

	// The part of the door's plane a picture covers, in the doorway's widths and heights from its
	// bottom corner: left, right, bottom, top.
	using Extent = std::array<float, 4>;
	inline constexpr Extent kDoorwayOnly{ 0.0f, 1.0f, 0.0f, 1.0f };

	// The plain colour is seen from here, in front of the doorway's middle.
	inline constexpr DoorwayPlace kPlainEye{ 0.5f, 0.55f, -150.0f };

	// The capture camera's: one and a quarter doorway widths to either side, three quarters of its
	// height above, seen from in front of its middle. Stepping aside shows the room's side beyond
	// the doorway's edge, and coming close its ceiling; rays at up to about 55 degrees through the
	// doorway land in it. Not below: under the threshold is under the floor. From 80 units out
	// (a 130 degree view) the room beyond a few steps came out in a patch of thirty texels in the
	// middle, a dark smear in the doorway.
	struct CameraShape
	{
		Extent extent;
		float  back;   // how far in front of the doorway it stands
		bool   atEye;  // at the height of the eye it is taken for, not the doorway's middle
	};
	inline constexpr CameraShape kRoomCamera{ { -1.25f, 2.25f, 0.0f, 1.75f }, 150.0f, false };
	// A city gate's is seen from down the road, and what is behind it is a street thousands of
	// units deep: from the room's place the city beyond the gatehouse came out in a patch of ninety
	// texels, drawn nine times larger on screen from where a gate is walked up to. From about that
	// far back the far side keeps its size; the narrower view still covers a player at the gate.
	// At the road's eye height: Whiterun's road is below the gate's threshold, and from the
	// doorway's middle the gatehouse's inner arch hid the street that the eye sees under it (the
	// eye's rays ran off the picture's top, its edge drawn out in streaks).
	inline constexpr CameraShape kGateCamera{ { -0.75f, 1.75f, 0.0f, 1.75f }, 1000.0f, true };
	[[nodiscard]] inline const CameraShape& ShapeOf(bool a_gate)
	{
		return a_gate ? kGateCamera : kRoomCamera;
	}

	// a_eyeUp: the eye's height in the doorway's heights (Frame::PlaceOf).
	[[nodiscard]] DoorwayPlace PlaceOf(const CameraShape& a_shape, float a_eyeUp);

	// Half-float RGBA, in video memory and on disk: colour, and in alpha how far behind the door's
	// plane the surface seen there is.
	inline constexpr int kBytesPerTexel = 8;
	// A doorway's shape: three quarters as wide as high, or wider (a double door, a city gate: at
	// three quarters its picture had a third or less of the texels across that it had up, the
	// street seen through Solitude's gate smeared sideways). Its own shape there, in sixteenths.
	inline constexpr float kNarrowest = 0.75f;
	inline constexpr float kWidest = 3.0f;

	[[nodiscard]] float AspectOf(const Quad& a_corners);
	// The width of a picture of a_height texels at the narrowest shape.
	[[nodiscard]] std::uint32_t WidthFor(std::uint32_t a_height);
	// A picture's size in texels: the doorway at the size set and its shape (AspectOf), times the extent.
	[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> SizeFor(const Extent& a_extent, int a_doorwayHeight, float a_aspect);

	// The doorway's frame: corner 0 and its edges, and its normal turned away from a_eye (so the
	// same whichever order the corners come in for the side it is seen from).
	struct Frame
	{
		Vec3 origin;
		Vec3 across;
		Vec3 up;
		Vec3 inward;

		Frame(const Quad& a_corners, const Vec3& a_eye);

		[[nodiscard]] DoorwayPlace PlaceOf(const Vec3& a_point) const;
		[[nodiscard]] Vec3         PointAt(const DoorwayPlace& a_place) const;
	};

	// A camera of its own for the doorway's picture, in front of the doorway's middle and looking
	// straight through it, wide enough to see the room beside and above it too. A picture taken
	// with the player's view had only what the doorway let through from where they stood, so a
	// step aside showed the doorway's edge smeared over what the wall had hidden; and from a
	// fixed place the picture is the same whoever takes it.
	struct CaptureCamera
	{
		Transform transform;  // in the player's space, as an NiCamera's (looking along its rotation's first column)
		float     left = 0.0f, right = 0.0f, bottom = 0.0f, top = 0.0f;  // the frustum at distance 1
		float     toDoorway = 0.0f;  // the door plane's distance ahead, in the player's space (negative: the camera is behind it)
		// Its near plane's distance, in the player's space; 0 = just past the door plane (the picture
		// taken from in front of the doorway cuts the wall and frame there). A camera inside the
		// doorway (a depth image's layer, Layers.h) has no door plane to cut at.
		float nearClip = 0.0f;
	};

	// Looking along the doorway's normal, so its plane is parallel to the image: the picture's
	// texels are evenly spread over the door's plane, as the draw reads them.
	[[nodiscard]] CaptureCamera For(const Frame& a_frame, const CameraShape& a_shape, const DoorwayPlace& a_place);

	// Where a place on the door's plane is in the capture camera's image, in clip space.
	[[nodiscard]] std::array<float, 4> Clip(const CaptureCamera& a_camera, const Frame& a_frame, float a_across, float a_up);

	// Where any point in front of the camera is in its image, in clip space.
	[[nodiscard]] std::array<float, 4> ClipOfPoint(const CaptureCamera& a_camera, const Vec3& a_point);
}
