#include "Skyrim/Portal/WaterReflections.h"

namespace Portal::WaterReflections
{
	namespace
	{
		// Layouts from TESWaterSystem::AddWater / RemoveWater (IDs 32179 / 32182).
		constexpr std::size_t kObjectShape = 0x20;
		constexpr std::size_t kReflectionMaterial = 0x40;

		template <class T>
		T* At(const void* a_object, std::size_t a_offset)
		{
			return *reinterpret_cast<T* const*>(reinterpret_cast<const std::byte*>(a_object) + a_offset);
		}

		struct Held
		{
			const void*                         reflection;
			const RE::BSShaderMaterial*         material;
			RE::NiPointer<RE::BSShaderProperty> property;  // holds the material
		};
		std::vector<Held> held;
	}

	void Update()
	{
		auto*                    system = RE::TESWaterSystem::GetSingleton();
		std::vector<std::size_t> seen;
		{
			// AddLODWater runs in the LOD job, on a worker thread.
			RE::BSSpinLockGuard guard(system->lock);
			for (auto& reflection : system->waterReflections) {
				const auto* material = At<RE::BSShaderMaterial>(reflection.get(), kReflectionMaterial);
				const auto  kept = std::ranges::find_if(held, [&](const Held& a_held) { return a_held.reflection == reflection.get() && a_held.material == material; });
				if (kept != held.end()) {
					seen.push_back(static_cast<std::size_t>(kept - held.begin()));
					continue;
				}
				for (auto& object : system->waterObjects) {
					auto* shape = At<RE::NiAVObject>(object.get(), kObjectShape);
					auto* geometry = shape ? shape->AsGeometry() : nullptr;
					auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
					if (property && property->material == material) {
						held.push_back({ reflection.get(), material, RE::NiPointer<RE::BSShaderProperty>(property) });
						seen.push_back(held.size() - 1);
						break;
					}
				}
			}
		}
		// Let go outside the water system's lock: the last reference frees the material, under the
		// material manager's own.
		std::vector<Held> kept;
		for (const auto i : seen) {
			kept.push_back(std::move(held[i]));
		}
		held = std::move(kept);
	}
}
