#pragma once

#include "Skyrim/Doors/Door.h"
#include "Skyrim/Portal/Active.h"

// How space in front of the source door of a pair maps to space in front of its linked door:
// where the doorway is and where whoever comes through comes out. Shared by the renderer (portal
// camera) and the travel code. Computed from the two doors' meshes and markers on every call.
namespace Portal::Doors
{
	struct Mapping
	{
		RE::NiTransform toLinked;      // source side -> linked side
		RE::NiPoint3    sourceCenter;  // middle of the source door panel
		RE::NiPoint3    inward;        // horizontal unit vector pointing through the source door
		float           yawDelta = 0;  // how much a heading turns when mapped
		float           halfWidth = 0; // half the doorway's width
		float           halfDepth = 0; // half the door's thickness: a city gate's is a step's worth
		float           halfHeight = 0;
	};

	// Throws if a door has no 3D or no teleport marker.
	Mapping Compute(RE::TESObjectREFR* a_source, RE::TESObjectREFR* a_linked);
	inline Mapping Compute(const Active& a_portal)
	{
		return Compute(Door(a_portal.sourceDoor), Door(a_portal.linkedDoor));
	}
	// Nothing while a door of the pair has no 3D: loading, or unloaded under a portal that is about
	// to be let go. Every frame's work on a door pair waits for both doors this way.
	std::optional<Mapping> TryCompute(const Active& a_portal);
}
