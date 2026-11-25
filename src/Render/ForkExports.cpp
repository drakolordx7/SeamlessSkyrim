#include "Render/ForkExports.h"

#include "Portal/LightOcclusion.h"
#include "Render/SceneQuery.h"

extern "C"
{
	bool SeamlessPortal_IsMergedShape(RE::BSGeometry* a_shape)
	{
		return Render::Scene().IsMergedShape(a_shape);
	}

	int SeamlessPortal_MergedRoomOf(RE::BSGeometry* a_shape)
	{
		return Render::Scene().MergedRoomOf(a_shape);
	}

	RE::ShadowSceneNode* SeamlessPortal_MergedRoomScene(int a_slot)
	{
		return Render::Scene().MergedRoomScene(a_slot);
	}

	bool SeamlessPortal_IsUnmaskedShape(RE::BSGeometry* a_shape)
	{
		return Render::Scene().IsPlayerShape(a_shape);
	}

	float SeamlessPortal_LightVisibility(RE::BSGeometry* a_geometry, RE::NiLight* a_light)
	{
		return Portal::LightOcclusion::Visibility(a_geometry, a_light);
	}
}
