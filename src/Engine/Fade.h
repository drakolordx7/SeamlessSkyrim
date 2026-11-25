#pragma once

namespace Engine
{
	// Every fade node under a_root shown at full opacity. The engine fades references in only in
	// the cells the player is in, and out and in again around its own transitions.
	inline void ForceFadeIn(RE::NiAVObject* a_root)
	{
		RE::BSVisit::TraverseScenegraphObjects(a_root, [](RE::NiAVObject* a_object) {
			if (auto* fade = a_object->AsFadeNode()) {
				fade->GetRuntimeData().currentFade = 1.0f;
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
	}
}
