#pragma once

#include "Skyrim/Render/Wire.h"

// The native backend (see Render/Backend.h): seamless rendering with the game's own shaders.
namespace Render::Native
{
	// Hooks the shaders' per-draw setup and the device context's draw calls. False if something
	// it needs isn't there (then the backend is off).
	bool Install();
	[[nodiscard]] bool Ready();

	// NativeMask.cpp: the doorway mask and the portal pass's far-door clip.
	void SetDoor(const std::optional<DoorRect>& a_door);
	void SetGatePassage(float a_depth);
	void SetPortalClip(const std::optional<ClipPlane>& a_plane);
	void SetPortalPass(bool a_active);
	[[nodiscard]] bool MergedDoorSet();  // a space is merged in this frame
	[[nodiscard]] json DescribeMask();

	// NativeLighting.cpp: the light of the merged space and of the portal pass.
	void SetMergedLighting(const std::optional<SpaceLight>& a_light);
	void SetMergedFog(const std::optional<SpaceFog>& a_fog);
	void InstallLightingHooks();
	[[nodiscard]] json DescribeLighting();

	// Called by NativeMask.cpp's hook on BSLightingShader::SetupGeometry around the engine's own:
	// changes what the engine reads for this pass, and the destructor puts everything back.
	struct LightingSwap
	{
		LightingSwap(RE::BSShader* a_shader, RE::BSRenderPass* a_pass);
		~LightingSwap();
		LightingSwap(const LightingSwap&) = delete;
		LightingSwap& operator=(const LightingSwap&) = delete;

		struct Saved;

	private:
		Saved* saved = nullptr;
	};
}
