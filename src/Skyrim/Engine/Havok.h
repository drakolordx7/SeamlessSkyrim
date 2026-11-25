#pragma once

namespace Engine::Havok
{
	struct RayHit
	{
		float                fraction;  // of the way from the ray's start to its end
		const RE::hkpCollidable* collidable;

		// The reference the body belongs to; null for the land and the like.
		[[nodiscard]] RE::TESObjectREFR* Ref() const { return RE::TESHavokUtilities::FindCollidableRef(*collidable); }
	};

	// The first body of a_layer's collision filter that the line a_from to a_to (game units) meets.
	// The caller holds the world's lock where the world may be stepping.
	[[nodiscard]] inline std::optional<RayHit> Ray(RE::bhkWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, RE::COL_LAYER a_layer)
	{
		const float     scale = RE::bhkWorld::GetWorldScale();
		RE::bhkPickData pick;
		pick.rayInput.from = RE::hkVector4(a_from.x * scale, a_from.y * scale, a_from.z * scale, 0.0f);
		pick.rayInput.to = RE::hkVector4(a_to.x * scale, a_to.y * scale, a_to.z * scale, 0.0f);
		pick.rayInput.filterInfo.SetCollisionLayer(a_layer);
		if (!a_world->PickObject(pick) || !pick.rayOutput.HasHit()) {
			return std::nullopt;
		}
		return RayHit{ pick.rayOutput.hitFraction, pick.rayOutput.rootCollidable };
	}
}
