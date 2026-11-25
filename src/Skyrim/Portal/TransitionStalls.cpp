#include "Skyrim/Portal/TransitionStalls.h"

#include "Skyrim/Diag/Telemetry.h"
#include "Skyrim/Engine/Hooks.h"
#include "Skyrim/Engine/LoadScreen.h"
#include "Skyrim/Portal/LightScene.h"
#include "Skyrim/Portal/TravelTrace.h"

namespace
{
	bool seamlessTeleport = false;

	// ID 40438 + 0x927: after a door teleport it asks BGSSaveLoadManager (ID 35769) for the
	// "autosave on travel". Saving stalls the next frames, which in a doorway you just walk
	// through is a stutter; trips with a loading screen still save.
	struct TravelAutosaveHook
	{
		static void Thunk(void* a_manager, std::uint32_t a_flags)
		{
			if (seamlessTeleport) {
				Portal::TravelTrace::Note("autosave for this trip skipped");
				return;
			}
			func(a_manager, a_flags);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// ID 40438 + 0x8B2: after a door teleport, waits until the task pool (*ID 411121, ID 75708)
	// has finished every queued load, i.e. the new space is complete before the loading screen
	// goes away. Going outside that was ~470 ms of a ~610 ms stall. Seamless, the remaining
	// references stream in over the next frames like they do while walking around outdoors.
	struct WaitForLoadsHook
	{
		static std::uint64_t Thunk(void* a_taskPool, std::uint32_t a_level)
		{
			if (seamlessTeleport) {
				Portal::TravelTrace::Note("waiting for queued loads skipped");
				return 0;
			}
			return func(a_taskPool, a_level);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// ID 40438 + 0x952: closes the loading screen (ID 13363 with "show" false), which ends with
	// two Sleep(30) calls, 60 ms of every trip. With the loading screen never shown (ID 400337
	// makes the "show" call return straight away) there is nothing to close.
	struct CloseLoadScreenHook
	{
		static bool Thunk(bool a_show, void* a_arg2, bool a_arg3, bool a_arg4)
		{
			if (seamlessTeleport && !a_show && !Engine::LoadScreenUp()) {
				Portal::TravelTrace::Note("closing the loading screen skipped");
				return false;
			}
			return func(a_show, a_arg2, a_arg3, a_arg4);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// TES::SetCurrentCell (ID 13316) + 0x273: when no loading screen is up it draws one more frame
	// (Main::Draw, ID 36555) of the space being left, in the middle of the transition: ~15 ms and
	// a frame nobody should see (the next one is already drawn from the other side).
	struct ExtraFrameHook
	{
		static void Thunk(void* a_main)
		{
			if (seamlessTeleport) {
				Portal::TravelTrace::Note("extra frame inside SetCurrentCell skipped");
				return;
			}
			func(a_main);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// The lights of the space walked into are queued on arrival, and the world scene's light update
	// takes each in with a cull of the scene for the shapes it lights (~0.27 ms a light): Understone
	// Keep's ~150 were ~40 ms of the teleport's frame. For a few frames after a seamless trip the
	// update takes the nearest ones first, a few per frame, and the rest wait in the queue.
	// Our light scenes too: the space walked out of is attached again in the frame of arrival, and
	// taking in its lights (Riften's jail ~175, Understone Keep's ~150) was 20-35 ms of it. Only
	// after a trip, when that space is behind the player: a room shown at its door while the player
	// looks at it takes its lights in at once, or they came on one after another in the doorway.
	constexpr int    kLightsPerUpdate = 32;
	constexpr int    kSpreadFrames = 30;
	int              spreadFrames = 0;
	std::atomic<int> lightsHeld = 0;  // held back in the last update

	template <class F>
	void UpdateLightsSpread(RE::ShadowSceneNode* a_scene, void* a_arg, F&& a_update)
	{
		const bool world = a_scene == RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		if (spreadFrames <= 0 || (!world && !Portal::LightScene::IsOurs(a_scene))) {
			a_update(a_scene, a_arg);
			return;
		}
		auto&                                   data = a_scene->GetRuntimeData();
		std::vector<RE::NiPointer<RE::BSLight>> held;
		std::unordered_set<const RE::BSLight*>  removing;
		{
			RE::BSSpinLockGuard locker(data.lightQueueLock);
			auto&               queue = data.lightQueueAdd;
			if (queue.size() > kLightsPerUpdate) {
				const auto at = RE::PlayerCharacter::GetSingleton()->GetPosition();
				// The arrival queues each light twice; the update takes in the first and, for the
				// second, walks the scene for the first's shapes again.
				std::vector<RE::NiPointer<RE::BSLight>> all;
				std::unordered_set<const RE::NiLight*>  queued;
				for (auto& light : queue) {
					if (!light || !light->light || queued.insert(light->light.get()).second) {
						all.push_back(light);
					}
				}
				const auto distance = [&](const RE::NiPointer<RE::BSLight>& a_light) {
					return a_light && a_light->light ? a_light->light->world.translate.GetSquaredDistance(at) : 0.0f;
				};
				std::ranges::stable_sort(all, {}, distance);
				queue.clear();
				for (std::size_t i = 0; i < all.size(); ++i) {
					if (i < kLightsPerUpdate) {
						queue.push_back(all[i]);
					} else {
						held.push_back(all[i]);
					}
				}
			}
			for (auto& light : data.lightQueueRemove) {
				removing.insert(light.get());
			}
		}
		lightsHeld = static_cast<int>(held.size());
		a_update(a_scene, a_arg);
		if (!held.empty()) {
			RE::BSSpinLockGuard locker(data.lightQueueLock);
			for (auto& light : held) {
				// Removed meanwhile (the space left again): not taken in later.
				if (!removing.contains(light.get())) {
					data.lightQueueAdd.push_back(light);
				}
			}
		}
	}

	// In the teleport (ID 40744): the arrival's lights.
	struct TeleportLightsHook
	{
		static void Thunk(RE::ShadowSceneNode* a_scene, void* a_arg)
		{
			if (seamlessTeleport) {
				spreadFrames = kSpreadFrames;
			}
			UpdateLightsSpread(a_scene, a_arg, [](RE::ShadowSceneNode* a_s, void* a_a) { func(a_s, a_a); });
			if (lightsHeld > 0) {
				Portal::TravelTrace::Note(std::format("{} light(s) left queued for the next frames", lightsHeld.load()));
			}
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// Every frame (ID 106337, for each registered light scene).
	struct FrameLightsHook
	{
		static void Thunk(RE::ShadowSceneNode* a_scene, void* a_arg)
		{
			const bool        world = a_scene == RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
			const Diag::Scope scope(world ? "lights: world" : "lights: other");
			UpdateLightsSpread(a_scene, a_arg, [](RE::ShadowSceneNode* a_s, void* a_a) { func(a_s, a_a); });
			if (world && spreadFrames > 0) {
				--spreadFrames;
			}
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};
}

namespace Portal::TransitionStalls
{
	void SetSeamless(bool a_seamless)
	{
		seamlessTeleport = a_seamless;
	}

	bool Seamless()
	{
		return seamlessTeleport;
	}

	void Install()
	{
		const auto callsTo = [](std::uintptr_t a_site, REL::RelocationID a_id) { return Hooks::CallsTo(a_site, a_id, std::format("travel: call to ID {}", a_id.id())); };
		REL::Relocation<std::uintptr_t> autosave{ REL::RelocationID(39366, 40438), REL::Relocate(0x897, 0x927) };
		if (callsTo(autosave.address(), REL::RelocationID(34859, 35769))) {
			TravelAutosaveHook::func = SKSE::GetTrampoline().write_call<5>(autosave.address(), TravelAutosaveHook::Thunk);
		}
		const REL::RelocationID         kWaitForLoads(73972, 75708);
		REL::Relocation<std::uintptr_t> waitForLoads{ REL::RelocationID(39366, 40438), REL::Relocate(0x822, 0x8B2) };
		REL::Relocation<std::uintptr_t> closeLoadScreen{ REL::RelocationID(39366, 40438), REL::Relocate(0x8C3, 0x952) };
		if (callsTo(waitForLoads.address(), kWaitForLoads)) {
			WaitForLoadsHook::func = SKSE::GetTrampoline().write_call<5>(waitForLoads.address(), WaitForLoadsHook::Thunk);
		} else {
			logger::warn("travel: load wait call not found, crossings keep waiting for loads");
		}
		if (callsTo(closeLoadScreen.address(), REL::RelocationID(13214, 13363))) {
			CloseLoadScreenHook::func = SKSE::GetTrampoline().write_call<5>(closeLoadScreen.address(), CloseLoadScreenHook::Thunk);
		} else {
			logger::warn("travel: loading screen close call not found");
		}
		const REL::RelocationID         kUpdateLights(99701, 106335);
		REL::Relocation<std::uintptr_t> teleportLights{ REL::RelocationID(39657, 40744), REL::Relocate(0x3DC, 0x3D2) };
		REL::Relocation<std::uintptr_t> frameLights{ REL::RelocationID(99703, 106337), REL::Relocate(0x2B, 0x2B) };
		if (callsTo(teleportLights.address(), kUpdateLights) && callsTo(frameLights.address(), kUpdateLights)) {
			TeleportLightsHook::func = SKSE::GetTrampoline().write_call<5>(teleportLights.address(), TeleportLightsHook::Thunk);
			FrameLightsHook::func = SKSE::GetTrampoline().write_call<5>(frameLights.address(), FrameLightsHook::Thunk);
		}
		REL::Relocation<std::uintptr_t> extraFrame{ REL::RelocationID(13171, 13316), REL::Relocate(0x24C, 0x273) };
		if (callsTo(extraFrame.address(), REL::RelocationID(35556, 36555))) {
			ExtraFrameHook::func = SKSE::GetTrampoline().write_call<5>(extraFrame.address(), ExtraFrameHook::Thunk);
		} else {
			logger::warn("travel: Main::Draw call in SetCurrentCell not found");
		}
	}
}
