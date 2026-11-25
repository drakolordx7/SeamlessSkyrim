#pragma once

#include "Core/Render/Types.h"
#include "Skyrim/Engine/CoreConvert.h"

// What the plugin and the Community Shaders fork agree on (docs/COMMUNITY-SHADERS-PATCH.md): the
// data they pass each other and the functions each exports to the other. The native backend takes
// the same typed structs, so nothing outside this file knows where a number sits in an array.
namespace Render
{
	// Rooms merged into the world at once, each masked at its own doorway and lit by its own light:
	// the fork's kSeamlessRooms (array sizes, room bits in ExtraShaderDescriptors).
	inline constexpr int kMaxRooms = 4;

	// The BSShaderManager::State slot the fork's light limit reads the portal's light scene from.
	inline constexpr std::uint32_t kPortalSceneSlot = 3;

	using Core::Render::ClipPlane;
	using Core::Render::Daylight;
	using Core::Render::DoorRect;
	using Core::Render::SpaceFog;
	using Core::Render::SpaceLight;

	inline DoorRect UprightDoor(const RE::NiPoint3& a_center, const RE::NiPoint3& a_inward, float a_halfWidth, float a_halfHeight, bool a_cameraMerged)
	{
		return DoorRect::Upright(ToCore(a_center), ToCore(a_inward), a_halfWidth, a_halfHeight, a_cameraMerged);
	}

	// The fork's layouts of the typed structs: a_out is what its exports read.
	inline void Pack(const DoorRect& a_door, float (&a_out)[12])
	{
		const float values[12] = { a_door.center.x, a_door.center.y, a_door.center.z, a_door.normal.x, a_door.normal.y, a_door.normal.z, a_door.right.x, a_door.right.y, a_door.right.z, a_door.halfWidth, a_door.halfHeight, a_door.cameraMerged ? 1.0f : 0.0f };
		std::ranges::copy(values, a_out);
	}

	inline void Pack(const SpaceLight& a_light, float (&a_out)[19])
	{
		a_out[0] = a_light.sun.red;
		a_out[1] = a_light.sun.green;
		a_out[2] = a_light.sun.blue;
		a_out[3] = a_light.towardSun.x;
		a_out[4] = a_light.towardSun.y;
		a_out[5] = a_light.towardSun.z;
		for (int row = 0; row < 3; ++row) {
			a_out[6 + row * 4] = a_light.ambientRotate.entry[row][0];
			a_out[7 + row * 4] = a_light.ambientRotate.entry[row][1];
			a_out[8 + row * 4] = a_light.ambientRotate.entry[row][2];
			a_out[9 + row * 4] = a_light.ambientTranslate[row];
		}
		a_out[18] = a_light.scale;
	}

	inline void Pack(const SpaceFog& a_fog, float (&a_out)[10])
	{
		const float values[10] = { a_fog.nearDistance, a_fog.farDistance, a_fog.power, a_fog.most, a_fog.nearColor.red, a_fog.nearColor.green, a_fog.nearColor.blue, a_fog.farColor.red, a_fog.farColor.green, a_fog.farColor.blue };
		std::ranges::copy(values, a_out);
	}

	inline void Pack(const Daylight& a_daylight, float (&a_out)[3])
	{
		a_out[0] = a_daylight.radiance.red;
		a_out[1] = a_daylight.radiance.green;
		a_out[2] = a_daylight.radiance.blue;
	}

	inline void Pack(const ClipPlane& a_plane, float (&a_out)[4])
	{
		a_out[0] = a_plane.normal.x;
		a_out[1] = a_plane.normal.y;
		a_out[2] = a_plane.normal.z;
		a_out[3] = a_plane.offset;
	}
}

// The functions the fork exports and the plugin calls, by name and signature. Every one is
// optional: a fork older than the plugin lacks some, and the one missing is named in the log once
// (Render/Backend.cpp).
// X(member, export name, return type, (parameter types))
#define SEAMLESS_FORK_EXPORTS(X)                                                                                  	X(setMergedDoor, "SeamlessPortal_SetMergedDoor", void, (const float*))                                        	X(setMergedLighting, "SeamlessPortal_SetMergedLighting", void, (const float*))                                	X(setMergedRoomDoor, "SeamlessPortal_SetMergedRoomDoor", void, (int, const float*))                           	X(setMergedRoomLighting, "SeamlessPortal_SetMergedRoomLighting", void, (int, const float*))                   	X(setMergedFog, "SeamlessPortal_SetMergedFog", void, (const float*))                                          	X(setFrontFog, "SeamlessPortal_SetFrontFog", void, (const float*))                                            	X(setMergedRoomDaylight, "SeamlessPortal_SetMergedRoomDaylight", void, (int, const float*))                   	X(setGatePassage, "SeamlessPortal_SetGatePassage", void, (float))                                             	X(setInteriorSun, "SeamlessPortal_SetInteriorSun", void, (bool))                                              	X(settleSkylighting, "SeamlessPortal_SettleSkylighting", void, ())                                            	X(setPortalClip, "SeamlessPortal_SetPortalClip", void, (const float*))                                        	X(setPortalShadows, "SeamlessPortal_SetPortalShadows", void, (RE::BSShadowDirectionalLight*, void*, void*))   	X(setPortalWorldspace, "SeamlessPortal_SetPortalWorldspace", void, (RE::TESWorldSpace*))                      	X(setPortalCamera, "SeamlessPortal_SetPortalCamera", void, (float, float))                                    	X(beginPass, "SeamlessPortal_BeginPass", void, (RE::NiDirectionalLight*, bool))                               	X(endPass, "SeamlessPortal_EndPass", void, ())                                                                	X(outdoorSunDirection, "SeamlessPortal_OutdoorSunDirection", bool, (float, const float*, float*))
