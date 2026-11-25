#pragma once

#include "Skyrim/Pictures/DoorwaySnapshot.h"
#include "Skyrim/Plugin/Settings.h"

// Where a doorway's picture is taken from and what part of the door's plane it covers, and how
// many texels it gets.
namespace Pictures::Camera
{
	using Corners = Portal::Snapshot::Corners;

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

	// The size pictures are taken at, as the doorway's height in texels; the width is three
	// quarters of it. Small on purpose: shown blurred, a hundred texels across are all it needs. A
	// room door's picture (the capture camera's) is 2.4 MB on disk at the default size, 3.2 MB of
	// video memory with its mipmaps; sixteen times that at the largest.
	inline constexpr auto kHeightSetting = Plugin::Settings::Of(Plugin::Settings::Id::kDoorwayPictureHeight);
	inline constexpr int  kDefaultHeight = kHeightSetting.def;
	inline constexpr int  kMinHeight = kHeightSetting.min;
	inline constexpr int  kMaxHeight = kHeightSetting.max;
	// Half-float RGBA, in video memory and on disk.
	inline constexpr int kBytesPerTexel = 8;
	// A doorway's shape: three quarters as wide as high, or wider (a double door, a city gate: at
	// three quarters its picture had a third or less of the texels across that it had up, the
	// street seen through Solitude's gate smeared sideways). Its own shape there, in sixteenths.
	inline constexpr float kNarrowest = 0.75f;
	inline constexpr float kWidest = 3.0f;

	[[nodiscard]] int  PictureHeight();
	void               SetPictureHeight(int a_height);
	[[nodiscard]] float AspectOf(const Corners& a_corners);
	[[nodiscard]] UINT  WidthFor(UINT a_height);
	// A picture's size in texels: the doorway at the size set and its shape (AspectOf), times the extent.
	[[nodiscard]] std::pair<UINT, UINT> SizeFor(const Extent& a_extent, int a_doorwayHeight, float a_aspect);

	// The doorway's frame: corner 0 and its edges, and its normal turned away from a_eye (so the
	// same whichever order the corners come in for the side it is seen from).
	struct Frame
	{
		RE::NiPoint3 origin;
		RE::NiPoint3 across;
		RE::NiPoint3 up;
		RE::NiPoint3 inward;

		Frame(const Corners& a_corners, const RE::NiPoint3& a_eye);

		[[nodiscard]] DoorwayPlace PlaceOf(const RE::NiPoint3& a_point) const;
		[[nodiscard]] RE::NiPoint3 PointAt(const DoorwayPlace& a_place) const;
	};

	// Looking along the doorway's normal, so its plane is parallel to the image: the picture's
	// texels are evenly spread over the door's plane, as the draw reads them.
	[[nodiscard]] Portal::Snapshot::CaptureCamera For(RE::FormID a_door, const Frame& a_frame, const CameraShape& a_shape, const DoorwayPlace& a_place);

	// Where a place on the door's plane is in the capture camera's image, in clip space.
	[[nodiscard]] std::array<float, 4> Clip(const Portal::Snapshot::CaptureCamera& a_camera, const Frame& a_frame, float a_across, float a_up);
}
