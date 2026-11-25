#pragma once

#include "Core/Render/Types.h"
#include "Skyrim/Engine/CoreConvert.h"

// The typed structs the renderer takes (Core/Render/Types.h), with the engine-typed way to make
// a doorway. Nothing outside the native renderer knows how a number reaches the shaders.
namespace Render
{
	// The BSShaderManager::State slot the portal's light scene is registered in.
	inline constexpr std::uint32_t kPortalSceneSlot = 3;

	using Core::Render::ClipPlane;
	using Core::Render::DoorRect;
	using Core::Render::SpaceFog;
	using Core::Render::SpaceLight;

	inline DoorRect UprightDoor(const RE::NiPoint3& a_center, const RE::NiPoint3& a_inward, float a_halfWidth, float a_halfHeight, bool a_cameraMerged)
	{
		return DoorRect::Upright(ToCore(a_center), ToCore(a_inward), a_halfWidth, a_halfHeight, a_cameraMerged);
	}
}
