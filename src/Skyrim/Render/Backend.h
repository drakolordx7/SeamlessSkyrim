#pragma once

#include "Skyrim/Render/Wire.h"

// Who draws the parts of seamless doors that need changes inside the shaders: the doorway mask
// (two spaces drawn in one pass, each only where the other isn't), the far door's clip in the
// portal pass, and the light of a room merged into the world.
//
// - Community Shaders, the SeamlessSkyrim fork (docs/COMMUNITY-SHADERS-PATCH.md): it replaces the
//   game's shaders and does all of it inside them, reached through its SeamlessPortal_* exports.
// - Native: the game's own shaders, unchanged (also what ENB runs on). The mask is cut by the
//   rasterizer from clip distances a geometry shader of ours adds behind the engine's vertex
//   shader (Render/Native/NativeMask.cpp); the light by changing what the engine hands its lighting
//   shader per draw (Render/Native/NativeLighting.cpp).
// - None: a stock Community Shaders without the fork's exports. It replaces the game's shaders
//   and ignores what the native backend sets up, so seamless rendering is off; crossing doors
//   without a loading screen still works.
//
// Chosen once at kDataLoaded, when every SKSE plugin is loaded. [Render] sBackend in
// SeamlessSkyrim.ini overrides the choice (auto, cs, native, none).
namespace Render::Backend
{
	// What a backend does. The defaults are the None backend's: nothing is done and nothing is
	// possible. What a backend can do is a property of it, asked here and not worked out from
	// which one it is.
	class Interface
	{
	public:
		virtual ~Interface() = default;

		[[nodiscard]] virtual const char* Name() const { return "none"; }
		// A room can be drawn into the world (render-merge): something masks the two spaces.
		[[nodiscard]] virtual bool CanMerge() const { return false; }
		// How many rooms can be merged at once, each masked at its own doorway and lit by its own
		// light: kMaxRooms with the fork's per-room exports, else 1. The native backend cuts the
		// world into convex pieces per doorway, and a shape near two doorways would multiply them.
		[[nodiscard]] virtual int MergedRooms() const { return 1; }
		// A city gate seen through as a merged door in room 0 (the portal pass draws the other
		// side) can be masked, see SetGatePassage.
		[[nodiscard]] virtual bool CanMaskGate() const { return false; }
		[[nodiscard]] virtual bool CanShadowPortal() const { return false; }
		// Whether the weather's rain and snow are masked at a merged doorway (the fork's particle
		// shader): without it, rain let into an interior with the street merged fell in the room too.
		[[nodiscard]] virtual bool MasksPrecipitation() const { return false; }
		// Whether shapes drawn with these shaders are cut at the far door in the portal pass.
		[[nodiscard]] virtual bool CutsPortalShader(RE::BSShader::Type) const { return false; }

		virtual void SetMergedDoor(int, const std::optional<DoorRect>&) {}
		virtual void SetMergedLighting(int, const std::optional<SpaceLight>&) {}
		virtual void SetMergedFog(const std::optional<SpaceFog>&) {}
		virtual void SetFrontFog(const std::optional<SpaceFog>&) {}
		virtual void SetMergedDaylight(int, const std::optional<Daylight>&) {}
		virtual void SetGatePassage(float) {}
		virtual void SetInteriorSun(bool) {}
		virtual void SettleSkylighting() {}
		virtual void SetPortalClip(const std::optional<ClipPlane>&) {}
		virtual void SetPortalWorldspace(RE::TESWorldSpace*) {}
		virtual void SetPortalShadows(RE::BSShadowDirectionalLight*, void*, void*) {}
		virtual void BeginPortalPass(RE::NiDirectionalLight*, bool, const RE::NiCamera*) {}
		virtual void EndPortalPass() {}
		virtual bool OutdoorSunDirection(float, const float*, float*) { return false; }

		// What this backend adds to Describe().
		[[nodiscard]] virtual json DescribeState() const { return json::object(); }
	};

	void Install();

	[[nodiscard]] const char* Name();
	[[nodiscard]] bool        CanMerge();
	[[nodiscard]] int         MergedRooms();
	[[nodiscard]] bool        CanMaskGate();
	[[nodiscard]] bool        CanShadowPortal();
	[[nodiscard]] bool        MasksPrecipitation();
	[[nodiscard]] bool        CutsPortalShader(RE::BSShader::Type a_type);

	// Render-merge, every frame while merged; nullopt when nothing is merged. a_room: 0 the room
	// the player can walk into (the only one with the camera inside), 1..3 rooms only seen from
	// the street (below MergedRooms()).
	void SetMergedDoor(int a_room, const std::optional<DoorRect>& a_door);
	// The merged space's own light. nullopt: lit like the world.
	void SetMergedLighting(int a_room, const std::optional<SpaceLight>& a_light);
	// The street merged into an interior: its fog.
	void SetMergedFog(const std::optional<SpaceFog>& a_fog);
	// Rooms merged into the street, seen from it: the street's fog for the air in front of their
	// doorways; nullopt: none. Community Shaders only.
	void SetFrontFog(const std::optional<SpaceFog>& a_fog);
	// A room merged into the street, seen from it: the street's light coming in through its
	// doorway; nullopt: none. Community Shaders only.
	void SetMergedDaylight(int a_room, const std::optional<Daylight>& a_daylight);
	// A city gate seen through as a merged door in room 0: the world's own shapes are masked through
	// it only up to a_depth beyond its plane, where its far gate stands in the world; 0 when room 0
	// isn't a gate. Only when CanMaskGate().
	void SetGatePassage(float a_depth);
	// The street merged into an interior gets the sun's shadows. Community Shaders only.
	void SetInteriorSun(bool a_on);
	// The player walked out of a doorway: lighting that builds up over time is settled at once.
	// The game's own lighting has nothing that builds up over time like CS's skylighting.
	void SettleSkylighting();

	// Around the portal pass: the clip plane is set before BeginPortalPass and cleared (nullopt)
	// after EndPortalPass. The passes don't nest.
	void SetPortalClip(const std::optional<ClipPlane>& a_plane);
	// The outdoor worldspace the portal pass draws, or nullptr: the fork keeps its terrain's shadows
	// (Terrain Shadows) for it while the player's worldspace (a city's) has none of its own.
	void SetPortalWorldspace(RE::TESWorldSpace* a_worldspace);
	// The sun's shadows of an outdoor space the portal pass draws, rendered with its own shadow light
	// into shadow map arrays made like the engine's (CanShadowPortal); set before Begin, cleared
	// (nullptr) after End.
	void SetPortalShadows(RE::BSShadowDirectionalLight* a_light, void* a_shadowMaps, void* a_volumetricShadowMaps);
	// a_sun: the light scene's sun, already carrying the other space's light. a_camera: the camera
	// the pass draws with; the fork turns the pass's depth back into distance with its near and far
	// (a city gate's portal camera has its near plane at the far gate).
	void BeginPortalPass(RE::NiDirectionalLight* a_sun, bool a_interior, const RE::NiCamera* a_camera);
	void EndPortalPass();

	// Community Shaders' Sky Sync: the sun's direction for an hour and its place on the sky.
	bool OutdoorSunDirection(float a_hour, const float* a_position, float* a_towardSun);

	[[nodiscard]] json Describe();
}
