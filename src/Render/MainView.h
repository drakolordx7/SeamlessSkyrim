#pragma once

namespace Render
{
	using ShadowStateData = std::remove_reference_t<decltype(RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData())>;

	// The main view's camera as the renderer held it when Main::RenderWorld began, which the doorway's
	// picture is placed with. When the world is done the renderer may hold another camera: every
	// other frame something inside the world pass (a camera at (0,0,1) looking elsewhere) left its
	// own there. The quad placed with it was behind that camera, so it wasn't drawn at all, and the
	// doorway showed what the room has behind its door (a box with a closed door) every second
	// frame: the flashing seen standing at a door in first person.
	struct MainView
	{
		decltype(std::declval<ShadowStateData>().posAdjust)         posAdjust;
		decltype(std::declval<ShadowStateData>().previousPosAdjust) previousPosAdjust;
		decltype(std::declval<ShadowStateData>().cameraData)        cameraData;
	};

	// Render thread, as Main::RenderWorld begins.
	void CaptureMainView();
	[[nodiscard]] MainView& CurrentMainView();
}
