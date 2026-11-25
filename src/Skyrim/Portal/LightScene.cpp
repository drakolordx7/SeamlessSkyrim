#include "Skyrim/Portal/LightScene.h"

#include "Skyrim/Render/Wire.h"

namespace
{
	// Main thread. The scene in slot 3, and the empty one registered there while no space is shown
	// (it never has a space under it, so it is safe to use again and again).
	RE::ShadowSceneNode*                            registered = nullptr;
	RE::NiPointer<RE::ShadowSceneNode>              idle;
	std::vector<RE::NiPointer<RE::ShadowSceneNode>> retired;
	// Every scene of ours carries this name (its interned data), so it can be recognised on any
	// thread without a list to lock.
	std::atomic<const char*> ourName = nullptr;

	bool IsUnder(const RE::NiAVObject* a_object, const RE::NiAVObject* a_ancestor)
	{
		for (auto* node = a_object; node; node = node->parent) {
			if (node == a_ancestor) {
				return true;
			}
		}
		return false;
	}

	template <class F>
	void ForEachLight(RE::ShadowSceneNode* a_scene, F&& a_func)
	{
		auto& data = a_scene->GetRuntimeData();
		for (auto& light : data.activeLights) {
			if (light) {
				a_func(light.get());
			}
		}
		for (auto& light : data.activeShadowLights) {
			if (light) {
				a_func(light.get());
			}
		}
	}
}

namespace Portal::LightScene
{
	RE::NiPointer<RE::ShadowSceneNode> Create()
	{
		// Same construction as the engine's own light scenes (Main init ID 36554, menu-3D
		// ID 52726): engine heap, ctor ID 106320 with the world scene's capacities.
		using ctor_t = RE::ShadowSceneNode* (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
		REL::Relocation<ctor_t>            ctor{ REL::RelocationID(99686, 106320) };
		RE::NiPointer<RE::ShadowSceneNode> scene(ctor(RE::malloc(0x308), 0x100, 4, 4, 0x4000, 0x80, 0x10));
		// Shaders look the active light scene up by this index (fog via ID 105637); only drawn once
		// registered there (Show).
		scene->GetRuntimeData().sceneGraphIndex = ::Render::kPortalSceneSlot;
		scene->name = "SeamlessPortalScene";
		ourName = scene->name.data();
		return scene;
	}

	void Show(RE::ShadowSceneNode* a_scene)
	{
		if (!a_scene) {
			if (!idle) {
				idle = Create();
			}
			a_scene = idle.get();
		}
		if (a_scene == registered) {
			return;
		}
		// Slot 3, kPortalSceneSlot (0 world, 1 menu-3D, 2/3 free): an unregistered index crashed the first frame. On
		// game load/reset the engine clears the lights of every registered scene (ID 13280).
		REL::Relocation<void (*)(std::uint32_t, RE::ShadowSceneNode*)> registerScene{ REL::RelocationID(98982, 105635) };
		registerScene(::Render::kPortalSceneSlot, a_scene);
		registered = a_scene;
	}

	RE::ShadowSceneNode* Get()
	{
		if (!registered) {
			Show(nullptr);
		}
		return registered;
	}

	bool Shown(const RE::ShadowSceneNode* a_scene)
	{
		return a_scene && a_scene == registered && a_scene != idle.get();
	}

	void Clear(RE::ShadowSceneNode* a_scene)
	{
		if (!a_scene) {
			return;
		}
		a_scene->RemoveAllLights();
		// Attaching a cell under this scene also registers its geometry, particles and actor roots
		// here (the scene is the parent). The engine's detach cleans up the scene it thinks owns
		// the cell (the world's), so these stayed behind and the next cell's portal pass crashed on
		// the released cell's stale passes.
		auto&               data = a_scene->GetRuntimeData();
		RE::BSSpinLockGuard locker(data.objectListLock);
		data.litGeometry.clear();
		data.particleNodes.clear();
		data.actorRootNodes.clear();
	}

	void Retire(RE::NiPointer<RE::ShadowSceneNode> a_scene)
	{
		if (!a_scene) {
			return;
		}
		Clear(a_scene.get());
		if (a_scene.get() == registered) {
			Show(nullptr);
		}
		retired.push_back(std::move(a_scene));
	}

	bool IsOurs(const RE::NiAVObject* a_top)
	{
		const char* name = ourName.load(std::memory_order_relaxed);
		return a_top && name && a_top->name.data() == name;
	}

	int Adopt(RE::ShadowSceneNode* a_scene, const RE::NiAVObject* a_cell3D)
	{
		auto* portal = a_scene;
		if (!portal || !a_cell3D) {
			return 0;
		}
		int moved = 0;
		// A street merged into an interior hangs in the world scene, where the engine registers one
		// of its lights again every frame; added to ours each time, the portal scene held it 1400
		// times after two minutes and the game crashed in the world scene's light list.
		std::unordered_set<const RE::BSLight*> ours;
		ForEachLight(portal, [&](RE::BSLight* a_light) { ours.insert(a_light); });
		for (auto* engineScene : RE::BSShaderManager::State::GetSingleton().shadowSceneNode) {
			if (!engineScene || engineScene == portal) {
				continue;
			}
			// Collect first: RemoveLight edits the arrays being iterated.
			std::vector<RE::NiPointer<RE::BSLight>> lights;
			ForEachLight(engineScene, [&](RE::BSLight* a_light) {
				if (a_light->light && IsUnder(a_light->light.get(), a_cell3D)) {
					lights.emplace_back(a_light);
				}
			});
			for (auto& light : lights) {
				engineScene->RemoveLight(light);
				if (ours.insert(light.get()).second) {
					portal->AddLight(light.get());
					++moved;
				}
			}
			// The cell the player just left gets its lights made anew on the attach (ID 19059), and
			// those wait in the scene's add queue until its next light update. Missed here, they
			// went live in the world scene for the frame of arrival: the room walked into was lit
			// by the one left behind as well (Jorrvaskr's hearth through its living quarters), the
			// whole screen flashed bright, and eye adaptation took a few frames to dim it again.
			std::vector<RE::NiPointer<RE::BSLight>> queued;
			{
				auto&               data = engineScene->GetRuntimeData();
				RE::BSSpinLockGuard locker(data.lightQueueLock);
				auto&               queue = data.lightQueueAdd;
				for (auto it = queue.begin(); it != queue.end();) {
					if (*it && (*it)->light && IsUnder((*it)->light.get(), a_cell3D)) {
						queued.push_back(*it);
						it = queue.erase(it);
					} else {
						++it;
					}
				}
			}
			for (auto& light : queued) {
				if (ours.insert(light.get()).second) {
					portal->AddLight(light.get());
					++moved;
				}
			}
			if (!queued.empty()) {
				logger::info("portal: {} light(s) taken from the engine scene's add queue", queued.size());
			}
		}
		return moved;
	}

	std::vector<RE::NiPointer<RE::BSLight>> WorldLights()
	{
		std::vector<RE::NiPointer<RE::BSLight>> lights;
		if (auto* world = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]) {
			ForEachLight(world, [&](RE::BSLight* a_light) { lights.emplace_back(a_light); });
		}
		return lights;
	}

	void PrepareForView()
	{
		ForEachLight(Get(), [](RE::BSLight* a_light) {
			a_light->frustrumCull = 0;  // 0 = visible, 255 = culled
			a_light->lodDimmer = 1.0f;
		});
	}

	void PrepareUnshownForView(RE::ShadowSceneNode* a_scene)
	{
		if (!a_scene || a_scene == registered) {
			return;
		}
		// What Main::RenderPlayerView does for every registered scene (ID 36559 + 0x194, through
		// ID 106337), with the same second argument.
		REL::Relocation<void (*)(RE::ShadowSceneNode*, void*)> updateLights{ REL::RelocationID(99701, 106335) };
		REL::Relocation<void***>                               viewer{ REL::RelocationID(528077, 415022) };
		void* const                                            argument = *viewer ? **viewer : nullptr;
		updateLights(a_scene, argument);
		ForEachLight(a_scene, [](RE::BSLight* a_light) {
			a_light->frustrumCull = 0;
			a_light->lodDimmer = 1.0f;
		});
	}

	json Describe(RE::ShadowSceneNode* a_scene, const RE::NiAVObject* a_cell3D)
	{
		if (!a_scene) {
			return nullptr;
		}
		auto&       data = a_scene->GetRuntimeData();
		int         cellLights = 0;
		std::size_t pairs = 0;
		ForEachLight(a_scene, [&](RE::BSLight* a_light) {
			if (a_light->light && a_cell3D && IsUnder(a_light->light.get(), a_cell3D)) {
				++cellLights;
			}
			pairs += a_light->geomList.size();
		});
		return {
			{ "sceneGraphIndex", data.sceneGraphIndex },
			{ "activeLights", data.activeLights.size() },
			{ "activeShadowLights", data.activeShadowLights.size() },
			{ "queuedAdd", data.lightQueueAdd.size() },
			{ "remoteCellLights", cellLights },
			{ "lightGeometryPairs", pairs },
			{ "litGeometry", data.litGeometry.size() },
			{ "particleNodes", data.particleNodes.size() },
			{ "actorRootNodes", data.actorRootNodes.size() },
		};
	}

	json DescribeAll(const RE::NiAVObject* a_cell3D)
	{
		json engine = json::array();
		for (auto* engineScene : RE::BSShaderManager::State::GetSingleton().shadowSceneNode) {
			engine.push_back(Describe(engineScene, a_cell3D));
		}
		// Diagnostics: raw values of the world scene's lights, to compare with the same lights
		// while they sit in the portal scene.
		json worldLights = json::array();
		if (auto* world = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]) {
			for (auto& bsLight : world->GetRuntimeData().activeLights) {
				auto* ni = bsLight ? bsLight->light.get() : nullptr;
				if (!ni || worldLights.size() >= 40) {
					continue;
				}
				auto& d = ni->GetLightRuntimeData();
				worldLights.push_back(std::format("pos=({:.0f},{:.0f},{:.0f}) radius=({:.0f},{:.0f},{:.0f}) fade={:.2f} bits={:X}", ni->world.translate.x, ni->world.translate.y,
					ni->world.translate.z, d.radius.x, d.radius.y, d.radius.z, d.fade, std::bit_cast<std::uint32_t>(d.ambient.red)));
			}
		}
		return { { "engine", engine }, { "portal", Describe(Get(), a_cell3D) }, { "retired", retired.size() }, { "worldLights", worldLights } };
	}
}
