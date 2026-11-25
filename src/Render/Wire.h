#pragma once

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

	// A doorway between the player's space and a merged one. Normal and right are horizontal.
	struct DoorRect
	{
		RE::NiPoint3 center;
		RE::NiPoint3 normal;  // into the merged space
		RE::NiPoint3 right;
		float        halfWidth = 0.0f;
		float        halfHeight = 0.0f;
		bool         cameraMerged = false;  // the camera is in the merged space

		// A doorway standing upright, facing a_inward (its z ignored).
		static DoorRect Upright(const RE::NiPoint3& a_center, const RE::NiPoint3& a_inward, float a_halfWidth, float a_halfHeight, bool a_cameraMerged)
		{
			return { a_center, { a_inward.x, a_inward.y, 0.0f }, { a_inward.y, -a_inward.x, 0.0f }, a_halfWidth, a_halfHeight, a_cameraMerged };
		}

		void Pack(float (&a_out)[12]) const
		{
			const float values[12] = { center.x, center.y, center.z, normal.x, normal.y, normal.z, right.x, right.y, right.z, halfWidth, halfHeight, cameraMerged ? 1.0f : 0.0f };
			std::ranges::copy(values, a_out);
		}
	};

	// A merged space's own light. scale: for its point lights, sun and ambient.
	struct SpaceLight
	{
		RE::NiColor   sun;
		RE::NiPoint3  towardSun;      // the way to the light, not the way it shines
		RE::NiMatrix3 ambientRotate;  // directional ambient: rotate * normal + translate
		RE::NiPoint3  ambientTranslate;
		float         scale = 1.0f;

		void Pack(float (&a_out)[19]) const
		{
			a_out[0] = sun.red;
			a_out[1] = sun.green;
			a_out[2] = sun.blue;
			a_out[3] = towardSun.x;
			a_out[4] = towardSun.y;
			a_out[5] = towardSun.z;
			for (int row = 0; row < 3; ++row) {
				a_out[6 + row * 4] = ambientRotate.entry[row][0];
				a_out[7 + row * 4] = ambientRotate.entry[row][1];
				a_out[8 + row * 4] = ambientRotate.entry[row][2];
				a_out[9 + row * 4] = ambientTranslate[row];
			}
			a_out[18] = scale;
		}
	};

	struct SpaceFog
	{
		float       nearDistance = 0.0f;
		float       farDistance = 0.0f;
		float       power = 1.0f;
		float       most = 0.0f;  // the most it covers; 0: no fog
		RE::NiColor nearColor;
		RE::NiColor farColor;

		void Pack(float (&a_out)[10]) const
		{
			const float values[10] = { nearDistance, farDistance, power, most, nearColor.red, nearColor.green, nearColor.blue, farColor.red, farColor.green, farColor.blue };
			std::ranges::copy(values, a_out);
		}
	};

	// The street's light coming in through a room's doorway: the radiance of the opening.
	struct Daylight
	{
		RE::NiColor radiance;

		void Pack(float (&a_out)[3]) const
		{
			a_out[0] = radiance.red;
			a_out[1] = radiance.green;
			a_out[2] = radiance.blue;
		}
	};

	// The plane a portal pass is cut at: pixels with dot(P, normal) < offset are cut. The sign
	// convention is stated here and nowhere else.
	struct ClipPlane
	{
		RE::NiPoint3 normal;  // unit
		float        offset = 0.0f;

		[[nodiscard]] float Distance(const RE::NiPoint3& a_point) const { return a_point.Dot(normal) - offset; }  // negative: cut

		void Pack(float (&a_out)[4]) const
		{
			a_out[0] = normal.x;
			a_out[1] = normal.y;
			a_out[2] = normal.z;
			a_out[3] = offset;
		}
	};
}

// The functions the fork exports and the plugin calls, by name and signature. Every one is
// optional: a fork older than the plugin lacks some, and the one missing is named in the log once
// (Render/Backend.cpp).
// X(member, export name, return type, (parameter types))
#define SEAMLESS_FORK_EXPORTS(X)                                                                                  	X(setMergedDoor, "SeamlessPortal_SetMergedDoor", void, (const float*))                                        	X(setMergedLighting, "SeamlessPortal_SetMergedLighting", void, (const float*))                                	X(setMergedRoomDoor, "SeamlessPortal_SetMergedRoomDoor", void, (int, const float*))                           	X(setMergedRoomLighting, "SeamlessPortal_SetMergedRoomLighting", void, (int, const float*))                   	X(setMergedFog, "SeamlessPortal_SetMergedFog", void, (const float*))                                          	X(setFrontFog, "SeamlessPortal_SetFrontFog", void, (const float*))                                            	X(setMergedRoomDaylight, "SeamlessPortal_SetMergedRoomDaylight", void, (int, const float*))                   	X(setGatePassage, "SeamlessPortal_SetGatePassage", void, (float))                                             	X(setInteriorSun, "SeamlessPortal_SetInteriorSun", void, (bool))                                              	X(settleSkylighting, "SeamlessPortal_SettleSkylighting", void, ())                                            	X(setPortalClip, "SeamlessPortal_SetPortalClip", void, (const float*))                                        	X(setPortalShadows, "SeamlessPortal_SetPortalShadows", void, (RE::BSShadowDirectionalLight*, void*, void*))   	X(setPortalWorldspace, "SeamlessPortal_SetPortalWorldspace", void, (RE::TESWorldSpace*))                      	X(setPortalCamera, "SeamlessPortal_SetPortalCamera", void, (float, float))                                    	X(beginPass, "SeamlessPortal_BeginPass", void, (RE::NiDirectionalLight*, bool))                               	X(endPass, "SeamlessPortal_EndPass", void, ())                                                                	X(outdoorSunDirection, "SeamlessPortal_OutdoorSunDirection", bool, (float, const float*, float*))
