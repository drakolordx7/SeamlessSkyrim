#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <utility>

#include "Core/Math/Vec3.h"
#include "Core/Pictures/Camera.h"

// The depth image tier: a doorway's room as a layered depth image. One to three pinhole views just
// inside the doorway, each looking straight in, keep colour and how far behind the door's plane the
// surface is, per texel. A doorway is drawn by marching the eye's ray against those depths, so
// walking past at an angle shows the room in parallax, and what one view hides behind a chair
// another may still show. The march is here as plain maths (the shaders in Skyrim/Pictures/Gpu.cpp
// do the same with the constants below); what the layers don't cover is left to the image tier.
//
// Coordinates are the doorway's own, in game units: x along its bottom edge, y up its side, z
// inward (away from whoever looks in). A view's image is in tangents: s = x / z, t = y / z of a
// point relative to the view, so a texel is a direction.
namespace Core::Pictures::Layers
{
	inline constexpr int kMaxLayers = 3;

	// Where the views stand: this far behind the doorway's plane (in front of it nothing of the
	// room is in view, and the door's frame and wall have no business in the pictures), at the
	// height of an eye in the doorway.
	inline constexpr float kInsideDepth = 36.0f;
	inline constexpr float kEyeUp = 0.55f;

	// What each view covers, in tangents: about 120 degrees across and 105 up and down. Close up the
	// room's walls beside and above the doorway are well off the axis of anyone who looks in.
	inline constexpr float kS0 = -1.7f, kS1 = 1.7f;
	inline constexpr float kT0 = -1.3f, kT1 = 1.3f;
	// A view sees nothing nearer than this in front of it.
	inline constexpr float kNearZ = 6.0f;

	// The march. The eye's ray runs from the doorway's plane inward in steps even in
	// p = z / (z + kReach): a few units apiece near the door, tens of units at the far wall of a hall.
	// It stops at kFarFraction (z about 4800 units: a hall, not a street).
	inline constexpr int   kSteps = 64;
	inline constexpr int   kRefine = 6;
	inline constexpr float kReach = 150.0f;
	inline constexpr float kFarFraction = 0.97f;
	// A texel's surface is "at" a point within this much along the view's axis (and this much of
	// the depth, further off).
	inline constexpr float kToleranceAbs = 3.0f;
	inline constexpr float kToleranceRel = 0.015f;
	// Alpha at or past this is no surface: the sky, or nothing in the picture.
	inline constexpr float kNoSurface = 15000.0f;

	// A view's place as the picture is kept: across and up as fractions of the doorway's size, and
	// how far behind it (units). The doorway's size changes with the door between sessions; the
	// views move with it.
	using Place = Camera::DoorwayPlace;
	[[nodiscard]] std::array<Place, kMaxLayers> PlacesFor(int a_count);

	struct View
	{
		Vec3  position;  // doorway coordinates, units
		float nearZ = kNearZ;
	};
	[[nodiscard]] View ViewAt(const Place& a_place, float a_width, float a_height);

	// A layer's size in texels at a_height rows: the extent's own shape.
	[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> SizeOf(int a_height);

	// The doorway's axes, for taking points in and out of its coordinates.
	struct Axes
	{
		Vec3  origin;
		Vec3  across;  // unit
		Vec3  up;      // unit
		Vec3  inward;  // unit
		float width = 0.0f;
		float height = 0.0f;

		explicit Axes(const Camera::Frame& a_frame);

		[[nodiscard]] Vec3 Local(const Vec3& a_world) const;
		[[nodiscard]] Vec3 World(const Vec3& a_local) const;
	};

	// The camera a layer is taken with: at its place, looking inward, its frustum the view's extent.
	// The doorway's across may run either way on screen; the image is resampled into the layer's own
	// (s, t) by the corners CaptureCorners gives.
	[[nodiscard]] Camera::CaptureCamera CameraFor(const Camera::Frame& a_frame, const View& a_view);
	// Clip space positions of the layer's (s, t) = (kS0, kT0), (kS1, kT0), (kS0, kT1): where the
	// picture's bottom left, bottom right and top left corners are in the camera's image.
	[[nodiscard]] std::array<std::array<float, 4>, 3> CaptureCorners(const Camera::CaptureCamera& a_camera, const Camera::Frame& a_frame, const View& a_view);

	struct Projection
	{
		bool  inView = false;  // in front of the view, within its extent
		float s = 0.0f, t = 0.0f;
		float z = 0.0f;            // along the view's axis
		float u = 0.0f, v = 0.0f;  // the picture's: across 0..1, down 0 (top) .. 1
	};
	[[nodiscard]] Projection Project(const View& a_view, const Vec3& a_local);

	// What a layer says of a point in the doorway's coordinates:
	enum class Vote : std::uint8_t
	{
		kNone,    // not in its picture
		kFree,    // the view sees past it, to a surface further off (or the sky)
		kOn,      // the surface it sees is there
		kBehind,  // it is hidden from the view by a nearer surface
	};
	// a_exact: no tolerance, a surface is passed or not (the march's last steps find where).
	[[nodiscard]] Vote VoteOf(const View& a_view, const Projection& a_projection, float a_stored, bool a_exact = false);

	// Solid when a layer sees its surface there and no more layers see through it than see it; or,
	// where it is hidden to all that have it in view, when none sees through. The second is the case
	// where one view alone has the ray behind a nearer thing; the first is what stops a ray that
	// grazes a chair's edge from stopping at the edge's stretch of colour, drawn out in a streak
	// (another view sees past the chair and says the space there is free).
	struct Votes
	{
		int on = 0, free = 0, behind = 0;
		void Add(Vote a_vote)
		{
			on += a_vote == Vote::kOn;
			free += a_vote == Vote::kFree;
			behind += a_vote == Vote::kBehind;
		}
		[[nodiscard]] bool Solid() const { return (on > 0 && on >= free) || (on == 0 && behind > 0 && free == 0); }
	};

	// The picture's depth as the layers hold it: the distance behind the doorway's plane stored at
	// layer a_layer's (u, v).
	using DepthAt = std::function<float(int a_layer, float a_u, float a_v)>;

	struct Setup
	{
		int                          count = 0;
		std::array<View, kMaxLayers> views{};
		float                        width = 0.0f;  // the doorway, units
		float                        height = 0.0f;
		float                        texelsAcross = 0.0f;  // one layer's size; the depth is read at texel centres (0: anywhere)
		float                        texelsDown = 0.0f;
	};
	[[nodiscard]] Setup SetupFor(int a_count, float a_width, float a_height, int a_rows);

	struct Hit
	{
		bool  hit = false;
		Vec3  point;       // doorway coordinates
		int   layer = -1;  // the layer whose picture colours it
		float u = 0.0f, v = 0.0f;
	};

	// The ray from a_origin (on the doorway's plane, z = 0) along a_direction (unit, z > 0). No hit when
	// the ray leaves what the layers cover, or the doorway's opening where none do yet, before it meets
	// a surface: the image tier draws it.
	[[nodiscard]] Hit March(const Setup& a_setup, const DepthAt& a_depth, const Vec3& a_origin, const Vec3& a_direction);

	// z at path position p in 0..1 (0 at the doorway's plane, 1 at infinity).
	[[nodiscard]] inline float DepthOf(float a_p)
	{
		return kReach * a_p / (1.0f - a_p);
	}
}
