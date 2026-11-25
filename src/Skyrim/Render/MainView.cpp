#include "Skyrim/Render/MainView.h"

namespace Render
{
	namespace
	{
		MainView mainView;
	}

	void CaptureMainView()
	{
		const auto& state = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
		mainView = { state.posAdjust, state.previousPosAdjust, state.cameraData };
	}

	MainView& CurrentMainView()
	{
		return mainView;
	}
}
