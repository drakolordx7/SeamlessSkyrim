#pragma once

#include "Core/Math/Mat3.h"
#include "Core/Math/Shapes.h"

// What the portal renderer is told about a merged space, as plain floats.
namespace Core::Render
{
	struct Color
	{
		float red = 0.0f;
		float green = 0.0f;
		float blue = 0.0f;
	};

	// A doorway between the player's space and a merged one. Normal and right are horizontal.
	struct DoorRect
	{
		Vec3  center;
		Vec3  normal;  // into the merged space
		Vec3  right;
		float halfWidth = 0.0f;
		float halfHeight = 0.0f;
		bool  cameraMerged = false;  // the camera is in the merged space

		// A doorway standing upright, facing a_inward (its z ignored).
		static DoorRect Upright(const Vec3& a_center, const Vec3& a_inward, float a_halfWidth, float a_halfHeight, bool a_cameraMerged)
		{
			return { a_center, { a_inward.x, a_inward.y, 0.0f }, { a_inward.y, -a_inward.x, 0.0f }, a_halfWidth, a_halfHeight, a_cameraMerged };
		}
	};

	// A merged space's own light. scale: for its point lights, sun and ambient.
	struct SpaceLight
	{
		Color sun;
		Vec3  towardSun;  // the way to the light, not the way it shines
		Mat3  ambientRotate;  // directional ambient: rotate * normal + translate
		Vec3  ambientTranslate;
		float scale = 1.0f;
	};

	struct SpaceFog
	{
		float nearDistance = 0.0f;
		float farDistance = 0.0f;
		float power = 1.0f;
		float most = 0.0f;  // the most it covers; 0: no fog
		Color nearColor;
		Color farColor;
	};

	// The plane a portal pass is cut at: pixels with Distance < 0 are cut. The sign convention is
	// stated by Plane and nowhere else.
	using ClipPlane = Plane;
}
