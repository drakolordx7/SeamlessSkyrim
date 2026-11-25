#pragma once

#include "Core/Math/Transform.h"

// How the space in front of one door of a pair maps onto the space in front of the other: where
// the doorway is and where whoever comes through comes out.
namespace Core::Doors
{
	struct Mapping
	{
		Transform toLinked;       // source side -> linked side
		Vec3      sourceCenter;   // middle of the source door panel
		Vec3      inward;         // horizontal unit vector pointing through the source door
		float     yawDelta = 0;   // how much a heading turns when mapped
		float     halfWidth = 0;  // half the doorway's width
		float     halfDepth = 0;  // half the door's thickness
		float     halfHeight = 0;
	};

	// One door as measured, in its own cell's coordinates.
	struct PairDoor
	{
		Vec3  center;       // middle of the panel
		Vec3  panelNormal;  // across the panel's thickness; its length is the thickness
		float markerYaw;    // where the door's own teleport marker faces (heading, radians)
		float halfWidth;
		float halfDepth;
		float halfHeight;
		bool  outdoors;
	};

	// A hand correction stored for the pair, kept on its "key" door.
	struct Correction
	{
		float degrees = 0.0f;  // turns the inside around its own door
		Vec3  shift;           // right, in, up along the key door, moves the inside's picture
		bool  keyIsSource = true;
	};

	// Each door's facing is its panel's normal, the marker only picks which of its two sides. Two
	// outdoor doors of one mesh (a city gate placed at two sizes) map scaled by their height ratio
	// when it differs by more than 1%; a_sameModel tells the caller's comparison of their models.
	[[nodiscard]] Mapping MapPair(const PairDoor& a_source, const PairDoor& a_linked, bool a_sameModel, const Correction& a_correction);
}
