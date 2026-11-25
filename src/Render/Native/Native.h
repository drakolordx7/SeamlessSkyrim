#pragma once

// The native backend (see Render/Backend.h): seamless rendering with the game's own shaders.
namespace Render::Native
{
	// Hooks the shaders' per-draw setup and the device context's draw calls. False if something
	// it needs isn't there (then the backend is off).
	bool Install();
	[[nodiscard]] bool Ready();

	// NativeMask.cpp: the doorway mask and the portal pass's far-door clip.
	void SetDoor(const float* a_door);
	void SetGatePassage(float a_depth);
	void SetPortalClip(const float* a_plane);
	void SetPortalPass(bool a_active);
	[[nodiscard]] json DescribeMask();

	// NativeLighting.cpp: the light of the merged space and of the portal pass.
	void SetMergedLighting(const float* a_lighting);
	void SetMergedFog(const float* a_fog);
	void InstallLightingHooks();
	[[nodiscard]] bool MergedDoorSet();  // NativeMask.cpp: a space is merged in this frame
	[[nodiscard]] json DescribeLighting();

	// Called by NativeMask.cpp's hook on BSLightingShader::SetupGeometry around the engine's own:
	// changes what the engine reads for this pass, and Restore puts everything back.
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
