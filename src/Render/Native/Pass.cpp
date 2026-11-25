#include "Render/Native/Pass.h"

#include "Render/Native/Native.h"
#include "Render/SceneQuery.h"
#include "Render/Wire.h"

namespace Render::Native::Pass
{
	namespace
	{
		RE::ShadowSceneNode* DrawnScene()
		{
			auto* accumulator = RE::BSShaderAccumulator::GetCurrentAccumulator();
			return accumulator ? accumulator->GetRuntimeData().activeShadowSceneNode : nullptr;
		}
	}

	bool InPortalScene()
	{
		auto* portalScene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[kPortalSceneSlot];
		return portalScene && DrawnScene() == portalScene;
	}

	bool InMainWorldPass()
	{
		const auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
		if (shadow.depthStencil != RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN) {
			return false;  // shadow maps and other targets
		}
		auto* world = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		return world && DrawnScene() == world;
	}

	bool IsMergedRoomShape(const RE::BSGeometry* a_shape)
	{
		auto* scene = DrawnScene();
		return scene && scene == RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0] && MergedDoorSet() && Scene().IsMergedShape(a_shape) && !Scene().IsPlayerShape(a_shape);
	}
}
