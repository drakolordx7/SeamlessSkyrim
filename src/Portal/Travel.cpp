#include "Portal/Travel.h"
#include "Spaces/RemoteCell.h"

#include "Diag/Telemetry.h"
#include "Render/FlashGuard.h"
#include "Engine/Hooks.h"
#include "Engine/Paths.h"
#include "Portal/CameraLimit.h"
#include "Portal/LightScene.h"
#include "Portal/NpcTravel.h"
#include "Portal/PortalManager.h"
#include "Diag/Probe.h"

namespace
{
	using Clock = std::chrono::steady_clock;

	// ID 13363 (TES: show/hide the loading screen) returns without showing it while this is set.
	REL::Relocation<bool*> skipLoadScreen{ REL::RelocationID(514187, 400337) };
	// Set while the loading screen is up (written by ID 13363).
	REL::Relocation<bool*> loadScreenUp{ REL::RelocationID(514186, 400336) };

	struct Trace
	{
		bool                     running = false;
		Clock::time_point        start;
		Clock::time_point        lastFrame;
		RE::FormID               fromCell = 0;
		RE::FormID               door = 0;
		std::uint32_t            frames = 0;
		double                   worstFrameMs = 0.0;
		double                   arrivedMs = -1.0;
		std::vector<std::string> events;
	};
	Trace                                  trace;
	json                                   lastResult;
	std::filesystem::path                  profileFile;       // diagnostics: the next transition is sampled (Sampler) into this file
	// Diagnostics, agent sessions only ([Portals] sProfileCrossingsTo): every crossing is sampled
	// for kProfileAllSeconds into a numbered file in this folder. The long frames come up to a few
	// seconds after the crossing, beyond the bridge command's one and a half.
	std::filesystem::path                  profileAllTo;
	constexpr double                       kProfileAllSeconds = 5.0;
	std::optional<Portal::Manager::Active> pending;
	bool                                   pendingLimitsCamera = true;
	float                                  pendingCameraFloor = 0.0f;
	bool                                   pendingExact = false;  // see Request
	std::optional<RE::NiPoint3>            exactArrival;          // see Begin
	constexpr float                        kArrivalStep = 12.0f;
	constexpr float                        kPlayerRadius = 20.0f;
	std::optional<float>                   arrivalYaw;  // heading to set once the teleport has run
	// The doorway the player came out of: the camera is let out again as they walk away from it.
	struct Arrival
	{
		RE::NiPoint3      center;
		RE::NiPoint3      inward;
		Clock::time_point at;
		RE::FormID        door = 0;  // the doorway came out of, kept open until the player is through
		bool              limitCamera = true;  // the camera was pulled in and is let out again
		float             cameraFloor = 0.0f;  // ... but never closer than this
		float             clear = 0.0f;  // depth at which the player no longer touches the door
		std::optional<float> firstDepth;  // where the player stood in the first frame after it
		std::optional<Clock::time_point> released;  // when the doorway behind got its own camera stop
	};
	std::optional<Arrival> arrival;
	bool                   leftExterior = false;  // the trip in progress began outdoors
	// After a trip through a city gate, the cells of the worldspace left are purged (Update) once
	// the loads the trip set off are done, and only then is the view back attached. Purged and
	// attached in the frame of arrival, with references of the new space still loading on worker
	// threads (the trip doesn't wait for them, WaitForLoadsHook), the engine crashed now and then
	// (ID 15328 in a loader task, ID 35567 under RemoteCell::AttachOne: the save's change-form
	// list both go through, half rebuilt).
	std::optional<std::chrono::steady_clock::time_point> purgeDeadline;
	constexpr std::chrono::milliseconds                   kPurgeWaitMax{ 4000 };

	// Whether the task pool (*ID 411121) has loads queued or running: what ID 75708 waits for
	// (its pending count, vfunc 0x80; its queue's, vfunc 0x18 of the object at +0xE0; and the
	// optional check at +0xF0).
	bool LoadsPending()
	{
		static REL::Relocation<std::byte**> pool{ REL::RelocationID(524542, 411121) };
		auto* taskPool = *pool;
		if (!taskPool) {
			return false;
		}
		using Count_t = std::int32_t (*)(void*);
		const auto Vfunc = [](void* a_object, std::size_t a_offset) { return reinterpret_cast<Count_t>((*reinterpret_cast<std::uintptr_t**>(a_object))[a_offset / 8]); };
		if (Vfunc(taskPool, 0x80)(taskPool) != 0) {
			return true;
		}
		if (auto* queue = *reinterpret_cast<void**>(taskPool + 0xE0); queue && Vfunc(queue, 0x18)(queue) != 0) {
			return true;
		}
		auto* extra = *reinterpret_cast<std::int32_t (**)()>(taskPool + 0xF0);
		return extra && extra() != 0;
	}
	std::recursive_mutex                   lock;

	double Ms(Clock::duration a_duration)
	{
		return std::chrono::duration<double, std::milli>(a_duration).count();
	}

	std::string OpenMenus()
	{
		auto*       ui = RE::UI::GetSingleton();
		std::string open;
		for (const std::string_view name : { RE::LoadingMenu::MENU_NAME, RE::FaderMenu::MENU_NAME, std::string_view("LoadWaitSpinner") }) {
			if (ui && ui->IsMenuOpen(name)) {
				open += std::format("{} ", name);
			}
		}
		return open;
	}

	void Note(const std::string& a_what)
	{
		const auto at = Ms(Clock::now() - trace.start);
		trace.events.push_back(std::format("{:7.1f} ms: {}", at, a_what));
		logger::info("travel: {:7.1f} ms: {}", at, a_what);
	}

	// The door the player came out of stands open behind them, like the one they walked into,
	// until they have stepped out of its way (CameraLimit): no collision (they are standing in its
	// doorway, the camera just behind them), not drawn.
	Portal::Doors::OpenState arrivalDoor;

	void SetArrivalDoorOpen(bool a_open)
	{
		if (a_open && arrival) {
			Portal::Doors::Open(RE::TESForm::LookupByID<RE::TESObjectREFR>(arrival->door), arrivalDoor, true);
		} else if (!a_open) {
			Portal::Doors::Close(arrivalDoor);
		}
	}

	// How far a door's collision reaches past its centre (a_center) towards a_inward, at a_height: a
	// ray from that side back through the door, to the first body of a thing (not a person, not the land). Not only
	// the door's own: a gate's thick arch may be a static of its own, and placed short of it the
	// player was pushed out of it just the same.
	std::optional<float> CollisionReach(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_center, const RE::NiPoint3& a_inward, float a_height)
	{
		auto* cell = a_door ? a_door->GetParentCell() : nullptr;
		auto* world = cell ? cell->GetbhkWorld() : nullptr;
		if (!world) {
			return std::nullopt;
		}
		constexpr float kSpan = 80.0f;
		const float     scale = RE::bhkWorld::GetWorldScale();
		RE::NiPoint3    from = a_center + a_inward * kSpan;
		RE::NiPoint3    to = a_center - a_inward * kSpan;
		from.z = to.z = a_height;
		for (int i = 0; i < 8; ++i) {
			RE::bhkPickData pick;
			pick.rayInput.from = RE::hkVector4(from.x * scale, from.y * scale, from.z * scale, 0.0f);
			pick.rayInput.to = RE::hkVector4(to.x * scale, to.y * scale, to.z * scale, 0.0f);
			pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kCharController);
			if (!world->PickObject(pick) || !pick.rayOutput.HasHit()) {
				return std::nullopt;
			}
			const auto hit = from + (to - from) * pick.rayOutput.hitFraction;
			auto*      ref = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
			if (ref && !ref->As<RE::Actor>()) {
				return (hit.x - a_center.x) * a_inward.x + (hit.y - a_center.y) * a_inward.y;
			}
			from = hit - a_inward;  // someone in the doorway, or the land rising there
			if ((from - to).Dot(a_inward) <= 0.0f) {
				return std::nullopt;
			}
		}
		return std::nullopt;
	}

	// Queues the engine's own door teleport, without its fade and loading screen, and points it at
	// the place the player is walking to on the other side instead of the door's marker.
	void Begin(const Portal::Manager::Active& a_portal, bool a_limitCamera)
	{
		auto*                  player = RE::PlayerCharacter::GetSingleton();
		auto*                  door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_portal.sourceDoor);
		auto*                  base = door && door->GetBaseObject() ? door->GetBaseObject()->As<RE::TESObjectDOOR>() : nullptr;
		Portal::Doors::Mapping mapping;
		try {
			mapping = Portal::Doors::Compute(a_portal);
		} catch (const std::exception& e) {
			logger::warn("travel: door pair unusable: {}", e.what());
			return;
		}
		if (!base) {
			return;
		}
		trace = {};
		trace.running = true;
		trace.start = trace.lastFrame = Clock::now();
		trace.door = door->GetFormID();
		Diag::AddCount("crossing", 1.0);
		// The white frame came 2-3 frames after the crossing; a second's worth leaves room for slow ones.
		::Render::FlashGuard::Guard(60);
		Diag::CaptureEventFrames(L"crossing", 16);
		if (!profileFile.empty()) {
			Diag::SamplerStart(profileFile, 1.5);
			profileFile.clear();
		} else if (!profileAllTo.empty() && !Diag::SamplerRunning()) {
			static int crossings = 0;
			const auto file = profileAllTo / std::format("crossing_{:02}.txt", ++crossings);
			Diag::SamplerStart(file, kProfileAllSeconds);
			Note(std::format("sampling the main thread into {}", file.string()));
		}
		trace.fromCell = player->GetParentCell() ? player->GetParentCell()->GetFormID() : 0;
		Note(std::format("crossing door {:08X} from {} (menus: {})", trace.door, Probe::DescribeCell(player->GetParentCell()), OpenMenus()));

		// The cell goes back to the engine first: the teleport attaches it as the current cell.
		Portal::Manager::ReleaseAll("player walked through the doorway", 3.0f, a_portal.cell && !a_portal.cell->IsInteriorCell());

		*skipLoadScreen = true;
		// What activating a load door does for the player (TESObjectDOOR::HandleOpen, ID 17926):
		// start a fade to black whose callback then calls ID 17924, which queues the teleport
		// (PlayerCharacter::queuedTargetLoc, ID 40442). Doors flagged "automatic" skip the fade and
		// call ID 17924 directly; an open doorway has no reason to wait either.
		REL::Relocation<void (*)(RE::TESObjectDOOR*, RE::RefHandle*)> teleportThroughDoor{ REL::RelocationID(17523, 17924) };
		RE::RefHandle                                                handle = door->GetHandle().native_handle();
		auto&                                                        queued = player->GetPlayerRuntimeData().queuedTargetLoc;
		teleportThroughDoor(base, &handle);
		// Everything below overwrites the queued teleport and treats the next frame as the arrival.
		SEAMLESS_ASSERT(queued.isValid);

		// The queued teleport puts the player on the door's marker. Seamless: they come out where
		// they walked in, mapped through the door pair, facing the same way relative to the door.
		auto&      queuedPos = queued.location;
		auto&      queuedAngle = queued.angle;
		const auto marker = queuedPos;

		RE::NiPoint3 inwardInside = mapping.toLinked.rotate * mapping.inward;
		inwardInside.z = 0.0f;
		inwardInside.Unitize();
		// A little further in than the exact mapping, so the camera, right at the player, starts
		// inside the room rather than in the doorway's plane.
		queuedPos = mapping.toLinked * player->GetPosition() + (pendingExact ? RE::NiPoint3{} : inwardInside * kArrivalStep);
		if (!pendingExact) {
			// Carried through before reaching the door (in first person as soon as the camera reaches
			// the doorway's picture, PortalManager's Approach), the mapped position is still short
			// of the far door: inside the far building's wall. A little past its plane instead.
			constexpr float kPastFarDoor = 2.0f;
			const auto      farCenter = mapping.toLinked * mapping.sourceCenter;
			float           wanted = kPastFarDoor;
			// A city gate is thick, its collision reaching well past the plane: come out there, the
			// engine pushed the player out of it to whichever side was nearer, and put on the gate's
			// far side (in front of Whiterun's gate, walking out of the city) the next step carried
			// them through again. Past its collision instead, by about the player's radius.
			auto* here = player->GetParentCell();
			if (here && !here->IsInteriorCell() && a_portal.cell && !a_portal.cell->IsInteriorCell()) {
				if (const auto reach = CollisionReach(RE::TESForm::LookupByID<RE::TESObjectREFR>(a_portal.linkedDoor), farCenter, inwardInside, queuedPos.z + 60.0f)) {
					wanted = std::max(wanted, *reach + kPlayerRadius);
				}
			}
			const float past = (queuedPos.x - farCenter.x) * inwardInside.x + (queuedPos.y - farCenter.y) * inwardInside.y;
			if (past < wanted) {
				queuedPos += inwardInside * (wanted - past);
			}
		}
		// Height: walking into a merged doorway, the player is on the exterior's ground behind the
		// door (inside the building's shell), which needn't be level with the room's floor; arriving
		// 160 units up put them on Alvor's upper floor. Near the door the floor is the marker's.
		exactArrival.reset();
		if (!pendingExact) {
			queuedPos.z = std::clamp(queuedPos.z, marker.z - 16.0f, marker.z + 48.0f);
		} else if (a_portal.cell && !a_portal.cell->IsInteriorCell()) {
			// Walking out of a room into its merged copy: the place is inside the house's shell,
			// solid until the room is merged and the passage opened after the teleport. Sent there,
			// the engine looked for a free place for 1.2 s and put the player on the ground under
			// the house. They go to the door's marker, and are moved once the room is there.
			exactArrival = queuedPos + RE::NiPoint3{ 0.0f, 0.0f, 4.0f };
			queuedPos = marker;
		}
		auto* from = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		leftExterior = from && !from->IsInteriorCell();
		arrival = Arrival{ mapping.toLinked * mapping.sourceCenter, inwardInside, Clock::now(), a_portal.linkedDoor, a_limitCamera, pendingCameraFloor,
			mapping.halfDepth + kPlayerRadius };
		queuedAngle = { player->GetAngleX(), 0.0f, player->GetAngleZ() + mapping.yawDelta };
		arrivalYaw = queuedAngle.z;
		Note(std::format("teleport queued: ({:.0f},{:.0f},{:.0f}) yaw {:.1f} instead of marker ({:.0f},{:.0f},{:.0f})", queuedPos.x,
			queuedPos.y, queuedPos.z, RE::rad_to_deg(queuedAngle.z), marker.x, marker.y, marker.z));
	}

	bool seamlessTeleport = false;  // true while ID 40438 carries out a teleport we queued

	// ID 40438 + 0x927: after a door teleport it asks BGSSaveLoadManager (ID 35769) for the
	// "autosave on travel". Saving stalls the next frames, which in a doorway you just walk
	// through is a stutter; trips with a loading screen still save.
	struct TravelAutosaveHook
	{
		static void Thunk(void* a_manager, std::uint32_t a_flags)
		{
			if (seamlessTeleport) {
				Note("autosave for this trip skipped");
				return;
			}
			func(a_manager, a_flags);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// Diagnostics: each part of the stutter can be put back to compare (travel_options).
	bool skipLoadWait = true;
	bool skipLoadScreenClose = true;

	// ID 40438 + 0x8B2: after a door teleport, waits until the task pool (*ID 411121, ID 75708)
	// has finished every queued load, i.e. the new space is complete before the loading screen
	// goes away. Going outside that was ~470 ms of a ~610 ms stall. Seamless, the remaining
	// references stream in over the next frames like they do while walking around outdoors.
	struct WaitForLoadsHook
	{
		static std::uint64_t Thunk(void* a_taskPool, std::uint32_t a_level)
		{
			if (seamlessTeleport && skipLoadWait) {
				Note("waiting for queued loads skipped");
				return 0;
			}
			return func(a_taskPool, a_level);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// The change into an exterior (ID 13317) sets the worldspace up anew when the game's grid isn't
	// set up yet (the first trip outdoors after loading a save made indoors), and then waits for
	// the task pool twice (+0x4A0, +0x4D5), until the whole grid around the player has loaded: the
	// first walk out took 1.2 to 1.4 s, a frozen frame. The cells around the door are attached
	// already (handed over from the doorway's view, RemoteCell::Release); the rest streams in over
	// the next frames, as in the trips after it.
	struct GridLoadWaitHook
	{
		static std::uint64_t Thunk(void* a_taskPool, std::uint32_t a_level)
		{
			if (seamlessTeleport && skipLoadWait && !Portal::RemoteCell::HandedOver().empty()) {
				Note("waiting for the outdoor grid to load skipped");
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
			if (seamlessTeleport && skipLoadScreenClose && !a_show && !*loadScreenUp) {
				Note("closing the loading screen skipped");
				return false;
			}
			return func(a_show, a_arg2, a_arg3, a_arg4);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// TES::SetCurrentCell (ID 13316) + 0x273: when no loading screen is up it draws one more frame
	// (Main::Draw, ID 36555) of the space being left, in the middle of the transition: ~15 ms and
	// a frame nobody should see (the next one is already drawn from the other side).
	bool skipExtraFrame = true;

	// The lights of the space walked into are queued on arrival, and the world scene's light update
	// takes each in with a cull of the scene for the shapes it lights (~0.27 ms a light): Understone
	// Keep's ~150 were ~40 ms of the teleport's frame. For a few frames after a seamless trip the
	// update takes the nearest ones first, a few per frame, and the rest wait in the queue.
	// Our light scenes too: the space walked out of is attached again in the frame of arrival, and
	// taking in its lights (Riften's jail ~175, Understone Keep's ~150) was 20-35 ms of it. Only
	// after a trip, when that space is behind the player: a room shown at its door while the player
	// looks at it takes its lights in at once, or they came on one after another in the doorway.
	constexpr int     kLightsPerUpdate = 32;
	constexpr int     kSpreadFrames = 30;
	int               spreadFrames = 0;
	bool              spreadLights = true;
	std::atomic<int>  lightsHeld = 0;  // diagnostics: held back in the last update

	template <class F>
	void UpdateLightsSpread(RE::ShadowSceneNode* a_scene, void* a_arg, F&& a_update)
	{
		const bool world = a_scene == RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		if (!spreadLights || spreadFrames <= 0 || (!world && !Portal::LightScene::IsOurs(a_scene))) {
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
				Note(std::format("{} light(s) left queued for the next frames", lightsHeld.load()));
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

	struct ExtraFrameHook
	{
		static void Thunk(void* a_main)
		{
			if (seamlessTeleport && skipExtraFrame) {
				Note("extra frame inside SetCurrentCell skipped");
				return;
			}
			func(a_main);
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// Hair and cloth simulated by SMP (hdtSMP64 / FSMP) keep their last positions for a few frames
	// after a teleport, drawn stretched from the old place to the new one. SMP resets its
	// simulation when the loading screen closes, which a seamless trip never shows; it is told
	// the same way, with the menu events alone.
	//
	// Only SMP's own listeners hear them: sent to all, other mods did their loading-screen work on
	// every trip (FasterTextures, seen in the exit's teleport frame).
	void ResetClothPhysics()
	{
		static const auto smp = []() -> std::pair<std::uintptr_t, std::uintptr_t> {
			auto* module = ::GetModuleHandleW(L"hdtsmp64.dll");
			if (!module) {
				return {};
			}
			const auto  base = reinterpret_cast<std::uintptr_t>(module);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			return { base, base + nt->OptionalHeader.SizeOfImage };
		}();
		auto* ui = RE::UI::GetSingleton();
		auto* source = ui ? ui->GetEventSource<RE::MenuOpenCloseEvent>() : nullptr;
		if (!source || !smp.first) {
			return;
		}
		const RE::MenuOpenCloseEvent opened{ RE::LoadingMenu::MENU_NAME, true };
		const RE::MenuOpenCloseEvent closed{ RE::LoadingMenu::MENU_NAME, false };
		RE::BSSpinLockGuard          locker(source->lock);
		for (auto* sink : source->sinks) {
			const auto vtable = sink ? *reinterpret_cast<const std::uintptr_t*>(sink) : 0;
			if (vtable >= smp.first && vtable < smp.second) {
				sink->ProcessEvent(&opened, source);
				sink->ProcessEvent(&closed, source);
			}
		}
	}

	// Main::Update (ID 36564) processes the player's queued teleport as its first step
	// (+0x6E: ID 40438). Only where the end-of-frame call isn't found (EndOfFrameHook) is a
	// crossing carried out here, at the start of the frame after it was detected.
	bool endOfFrameHooked = false;
	std::atomic<bool> arrivalFrame = false;  // the next frame drawn is the first in the new space

	struct ProcessTeleportHook
	{
		static void Thunk(RE::PlayerCharacter* a_player)
		{
			std::scoped_lock guard(lock);
			if (pending && !endOfFrameHooked) {
				const auto portal = *pending;
				pending.reset();
				Begin(portal, pendingLimitsCamera);
			}
			const auto before = Clock::now();
			seamlessTeleport = arrivalYaw.has_value();
			// The teleport resets the third-person camera to the player's head, from where it eases
			// back out over the next frames (unseen behind a loading screen). Its distance and
			// offsets are carried over instead.
			struct CameraPose
			{
				RE::NiPoint3 expected, actual;
				float        target, current, pitch;
				float        yawTarget, yawCurrent;
			};
			std::optional<CameraPose> cameraPose;
			auto*                     camera = RE::PlayerCamera::GetSingleton();
			// On a horse or a dragon too: their camera states are third-person states.
			auto*                     thirdPerson = Portal::CameraLimit::InThirdPerson() ? static_cast<RE::ThirdPersonState*>(camera->currentState.get()) : nullptr;
			if (seamlessTeleport && thirdPerson) {
				cameraPose = CameraPose{ thirdPerson->posOffsetExpected, thirdPerson->posOffsetActual, thirdPerson->targetZoomOffset, thirdPerson->currentZoomOffset,
					thirdPerson->pitchZoomOffset, thirdPerson->targetYaw, thirdPerson->currentYaw };
			}
			const auto  cameraOffset = camera->cameraRoot->world.translate - a_player->GetPosition();
			const float yawBefore = a_player->GetAngleZ();
			{
				const Diag::Scope scope(seamlessTeleport ? "teleport" : "teleport step");
				func(a_player);
			}
			if (std::exchange(seamlessTeleport, false)) {
				// The engine only resets the flag after its own short teleports. Ours is done now;
				// left set until the trace ended (3 s), a save loaded or a fast travel in that time
				// went without a loading screen.
				*skipLoadScreen = false;
			}
			if (cameraPose && arrivalYaw && !(arrival && arrival->limitCamera)) {
				// The offset turns with the player (heading: forward = (sin, cos)).
				const float turn = *arrivalYaw - yawBefore;
				const float c = std::cos(turn), sn = std::sin(turn);
				Portal::CameraLimit::Carry({ cameraOffset.x * c + cameraOffset.y * sn, -cameraOffset.x * sn + cameraOffset.y * c, cameraOffset.z }, 0.6f);
			}
			if (cameraPose && camera->currentState.get() == thirdPerson) {
				thirdPerson->posOffsetExpected = cameraPose->expected;
				thirdPerson->posOffsetActual = cameraPose->actual;
				thirdPerson->targetZoomOffset = cameraPose->target;
				thirdPerson->currentZoomOffset = cameraPose->current;
				thirdPerson->pitchZoomOffset = cameraPose->pitch;
			}
			if (arrivalYaw) {
				Note(std::format("teleport step took {:.1f} ms", Ms(Clock::now() - before)));
				// ID 40744 doesn't keep the angles we queued (it faces the player along the marker);
				// set them now, then let the camera catch up before this frame is drawn instead of
				// showing one frame from where it was left.
				a_player->SetAngle({ a_player->GetAngleX(), 0.0f, *arrivalYaw });
				// The third-person camera eases its own heading towards the player's (currentYaw
				// follows targetYaw). Turned with the player by the trip, the camera started up to
				// 20 degrees behind and swung round over the first frames in the new space: the
				// view jerked right after walking through. Its headings turn with the player too.
				if (cameraPose && camera->currentState.get() == thirdPerson) {
					const float turn = *arrivalYaw - yawBefore;
					thirdPerson->targetYaw = cameraPose->yawTarget + turn;
					thirdPerson->currentYaw = cameraPose->yawCurrent + turn;
				}
				Note(std::format("heading set to {:.1f}, now {:.1f}", RE::rad_to_deg(*arrivalYaw), RE::rad_to_deg(a_player->GetAngleZ())));
				const Diag::Scope arrivalScope("arrival");
				arrivalYaw.reset();
				// Before the space just left is attached below, which forgets them.
				Portal::RemoteCell::HangHandedPersistent();
				// The space just left is attached (and merged, with the doorway passable) before the
				// camera is placed: otherwise the camera, behind the player in that doorway, collides
				// with the building's shell and starts at the player's head for two frames.
				Portal::Manager::EndHoldoff();
				if (auto* cell = a_player->GetParentCell(); cell && !cell->IsInteriorCell()) {
					if (leftExterior) {
						// Through a city gate, from one worldspace into another: the engine takes the
						// cells of the worldspace just left half apart (their land's 3D and most
						// buildings are gone, the small things stay), and attached again like that
						// the view back through the gate was a town of floating barrels. Purged from
						// the cell buffer (the console's "pcb"), they load again in full: once the trip's
						// loads are done (Update), and the view back isn't attached before.
						purgeDeadline = Clock::now() + kPurgeWaitMax;
						Portal::Manager::EndHoldoff(std::chrono::duration<float>(kPurgeWaitMax).count() + 1.0f);
					} else {
						Portal::Manager::Update(0.0f);
						if (exactArrival) {
							a_player->SetPosition(*exactArrival, true);
							// The camera (below) is placed from the player's head; left where the
							// marker put it until the frame's scene update, the frame of walking out
							// into a room was drawn from the door outside: a frame of bright street.
							a_player->Update3DPosition(true);
							for (const bool firstPerson : { false, true }) {
								if (auto* root = a_player->Get3D(firstPerson)) {
									RE::NiUpdateData update;
									root->Update(update);
								}
							}
							Note(std::format("moved into the merged room at ({:.0f},{:.0f},{:.0f})", exactArrival->x, exactArrival->y, exactArrival->z));
							exactArrival.reset();
							Portal::Manager::Update(0.0f);  // in the room now: its house's passage opened
						}
					}
				}
				// The player's 3D at the place of arrival before the camera is placed from its head: left
				// where the teleport put it until the frame's scene update, the first frame in the new
				// space was drawn from behind the player's head, their hair in the picture.
				a_player->Update3DPosition(true);
				for (const bool firstPerson : { false, true }) {
					if (auto* root = a_player->Get3D(firstPerson)) {
						RE::NiUpdateData update;
						root->Update(update);
					}
				}
				// The sky (the sun's and the ambient light's colours) is updated at the start of a
				// frame, before the crossing: the first frame in the new space was lit with the old
				// one's. Into a room from the street, the room was lit by the outdoor sun for a frame.
				{
					static REL::Relocation<void (*)(RE::Sky*, float)> updateSky{ REL::RelocationID(25682, 26229) };
					if (auto* sky = RE::Sky::GetSingleton()) {
						updateSky(sky, 0.0f);
						auto* scene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
						auto* sun = scene && scene->GetRuntimeData().sunLight ? scene->GetRuntimeData().sunLight->light.get() : nullptr;
						if (sun) {
							const auto& diffuse = sun->GetLightRuntimeData().diffuse;
							Note(std::format("sky updated: sun ({:.2f},{:.2f},{:.2f})", diffuse.red, diffuse.green, diffuse.blue));
						}
					}
				}
				const auto cameraStart = Clock::now();
				RE::PlayerCamera::GetSingleton()->Update();
				// The image space follows the camera; set before the camera had moved (by the
				// Manager::Update above, from the door outside), a room walked out into was drawn for
				// one frame with the outdoors' and flashed white.
				Portal::Manager::UpdateImageSpace();
				Note(std::format("image space blended: {}", Portal::Manager::ToneBlendOn()));
				// Taking the space over, the engine fades its references in (behind a loading screen,
				// unseen): the room or street the player had been looking into faded out and in again.
				Portal::NpcTravel::ShowAtOnce(a_player->GetParentCell(), 2.0f);
				// A walk the bridge is steering would turn the player back towards its old goal.
				Diag::StopWalk("travelled through a door");
				SetArrivalDoorOpen(true);
				// Indoors the view back out isn't needed in the frame of arrival (the camera faces
				// into the room), and attaching the street costs ~15 ms on top of the engine's own
				// ~35 ms for the change of space: a few frames later the two don't add up to one
				// long frame. Unless the street is merged into the room: the street's houses stand
				// around the room then and shade it, and the frames before were lit without them.
				// (Through a city gate the view back waits for the purge, Update.)
				if (!purgeDeadline) {
					auto* cell = a_player->GetParentCell();
					Portal::Manager::EndHoldoff(cell && cell->IsInteriorCell() && !Portal::Manager::MergeEnabled() ? 0.15f : 0.0f);
				}
				ResetClothPhysics();
				arrivalFrame = true;
				Note(std::format("camera updated ({:.1f} ms)", Ms(Clock::now() - cameraStart)));
			}
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};
	// Main::Update (ID 36564), the call returning to +0xAC4 (SE: the call at +0x5E8): the wait for
	// the frame's jobs (ID 35409), after the frame has been drawn. A crossing is carried out here, at the very end of the frame it was detected in,
	// rather than at the start of the next one: everything the engine updates once per frame for
	// the space the player is in (sky and ambient, image space, cloth physics, the camera) then
	// runs for the new space before its first frame is drawn. Done at the start of a frame, that
	// first frame was drawn with the old space's lighting.
	// The doorway come out of means nothing in a world the game has since replaced (a save loaded,
	// fast travel, a door taken with its loading screen): measured against it there, the camera was
	// held at the player's head, their body hidden, wherever they stood on the far side of its plane.
	void ForgetArrival()
	{
		std::scoped_lock guard(lock);
		SetArrivalDoorOpen(false);
		arrival.reset();
		pending.reset();
		exactArrival.reset();
	}

	// Every trip of the game's own shows its loading screen; a seamless one never does (skipLoadScreen).
	struct LoadingScreenSink : RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event->opening && a_event->menuName == RE::LoadingMenu::MENU_NAME) {
				ForgetArrival();
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	struct EndOfFrameHook
	{
		static void Thunk()
		{
			func();
			Portal::RemoteCell::StepAtEndOfFrame();
			std::unique_lock guard(lock);
			if (pending) {
				const auto portal = *pending;
				pending.reset();
				{
					const Diag::Scope scope("crossing begin");
					Begin(portal, pendingLimitsCamera);
				}
				guard.unlock();
				ProcessTeleportHook::Thunk(RE::PlayerCharacter::GetSingleton());
			}
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

}

namespace Portal::Travel
{
	void Install()
	{
		if (Diag::AgentMode()) {
			wchar_t folder[MAX_PATH]{};
			const auto ini = Paths::PluginDirectory() / "SeamlessSkyrim.ini";
			if (::GetPrivateProfileStringW(L"Portals", L"sProfileCrossingsTo", L"", folder, MAX_PATH, ini.c_str()) > 0) {
				profileAllTo = folder;
				std::error_code error;
				std::filesystem::create_directories(profileAllTo, error);
			}
		}
		// The engine's teleport (ID 40438) reads its queue at +0x650 and the valid flag at +0x692 (SE:
		// +0x640, +0x682; AE before 1.7.99: +0x648, +0x68A); CommonLib's layout must agree, or Begin
		// writes elsewhere.
		auto* player = RE::PlayerCharacter::GetSingleton();
		SEAMLESS_ASSERT(reinterpret_cast<std::uintptr_t>(&player->GetPlayerRuntimeData().queuedTargetLoc.isValid) - reinterpret_cast<std::uintptr_t>(player) == Hooks::Offset(0x682, 0x68A, 0x692));
		REL::Relocation<std::uintptr_t> site{ REL::RelocationID(35565, 36564), REL::Relocate(0x53, 0x6E) };
		if (!Hooks::CallsTo(site.address(), REL::RelocationID(39366, 40438), "travel: teleport")) {
			logger::error("travel: walking through doors uses the normal transition");
			return;
		}
		ProcessTeleportHook::func = SKSE::GetTrampoline().write_call<5>(site.address(), ProcessTeleportHook::Thunk);
		static LoadingScreenSink loadingScreen;
		RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(&loadingScreen);
		REL::Relocation<std::uintptr_t> autosave{ REL::RelocationID(39366, 40438), REL::Relocate(0x897, 0x927) };
		if (Hooks::CallsTo(autosave.address(), REL::RelocationID(34859, 35769), "travel: autosave")) {
			TravelAutosaveHook::func = SKSE::GetTrampoline().write_call<5>(autosave.address(), TravelAutosaveHook::Thunk);
		}
		const REL::RelocationID         kWaitForLoads(73972, 75708);
		REL::Relocation<std::uintptr_t> waitForLoads{ REL::RelocationID(39366, 40438), REL::Relocate(0x822, 0x8B2) };
		REL::Relocation<std::uintptr_t> closeLoadScreen{ REL::RelocationID(39366, 40438), REL::Relocate(0x8C3, 0x952) };
		const auto                      callsTo = [](std::uintptr_t a_site, REL::RelocationID a_id) { return Hooks::CallsTo(a_site, a_id, std::format("travel: call to ID {}", a_id.id())); };
		if (callsTo(waitForLoads.address(), kWaitForLoads)) {
			WaitForLoadsHook::func = SKSE::GetTrampoline().write_call<5>(waitForLoads.address(), WaitForLoadsHook::Thunk);
		} else {
			logger::warn("travel: load wait call not found, crossings keep waiting for loads");
		}
		for (const auto offset : REL::Relocate(std::array{ 0x402, 0x437 }, std::array{ 0x4A0, 0x4D5 })) {
			REL::Relocation<std::uintptr_t> gridWait{ REL::RelocationID(13172, 13317), offset };
			if (callsTo(gridWait.address(), kWaitForLoads)) {
				GridLoadWaitHook::func = SKSE::GetTrampoline().write_call<5>(gridWait.address(), GridLoadWaitHook::Thunk);
			}
		}
		if (callsTo(closeLoadScreen.address(), REL::RelocationID(13214, 13363))) {
			CloseLoadScreenHook::func = SKSE::GetTrampoline().write_call<5>(closeLoadScreen.address(), CloseLoadScreenHook::Thunk);
		} else {
			logger::warn("travel: loading screen close call not found");
		}
		REL::Relocation<std::uintptr_t> endOfFrame{ REL::RelocationID(35565, 36564), Hooks::Offset(0x5E8, 0xAAD, 0xABF) };
		if (callsTo(endOfFrame.address(), REL::RelocationID(34554, 35409))) {
			EndOfFrameHook::func = SKSE::GetTrampoline().write_call<5>(endOfFrame.address(), EndOfFrameHook::Thunk);
			endOfFrameHooked = true;
		} else {
			logger::warn("travel: end-of-frame call not found, crossings are carried out at the start of a frame");
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

	bool ArrivalFramePending()
	{
		return arrivalFrame.load();
	}

	bool ConsumeArrivalFrame()
	{
		return arrivalFrame.exchange(false);
	}

	RE::FormID ArrivalDoor()
	{
		std::scoped_lock guard(lock);
		return arrival ? arrival->door : 0;
	}

	json SetOptions(const json& a_options)
	{
		skipLoadWait = a_options.value("skipLoadWait", skipLoadWait);
		skipLoadScreenClose = a_options.value("skipLoadScreenClose", skipLoadScreenClose);
		skipExtraFrame = a_options.value("skipExtraFrame", skipExtraFrame);
		spreadLights = a_options.value("spreadLights", spreadLights);
		return { { "skipLoadWait", skipLoadWait }, { "skipLoadScreenClose", skipLoadScreenClose }, { "skipExtraFrame", skipExtraFrame },
			{ "spreadLights", spreadLights } };
	}

	void ThroughDoorTo(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_position, float a_yaw)
	{
		auto* base = a_door && a_door->GetBaseObject() ? a_door->GetBaseObject()->As<RE::TESObjectDOOR>() : nullptr;
		SEAMLESS_ASSERT(base);
		REL::Relocation<void (*)(RE::TESObjectDOOR*, RE::RefHandle*)> teleportThroughDoor{ REL::RelocationID(17523, 17924) };
		RE::RefHandle handle = a_door->GetHandle().native_handle();
		auto&         queued = RE::PlayerCharacter::GetSingleton()->GetPlayerRuntimeData().queuedTargetLoc;
		teleportThroughDoor(base, &handle);
		SEAMLESS_ASSERT(queued.isValid);
		queued.location = a_position;
		queued.angle = { 0.0f, 0.0f, a_yaw };
		logger::info("travel: after loading, through door {:08X} to ({:.0f},{:.0f},{:.0f})", a_door->GetFormID(), a_position.x, a_position.y, a_position.z);
	}

	void ThroughDoor(RE::TESObjectREFR* a_door)
	{
		auto* base = a_door->GetBaseObject()->As<RE::TESObjectDOOR>();
		SEAMLESS_ASSERT(base);
		REL::Relocation<void (*)(RE::TESObjectDOOR*, RE::RefHandle*)> teleportThroughDoor{ REL::RelocationID(17523, 17924) };
		RE::RefHandle handle = a_door->GetHandle().native_handle();
		teleportThroughDoor(base, &handle);
		SEAMLESS_ASSERT(RE::PlayerCharacter::GetSingleton()->GetPlayerRuntimeData().queuedTargetLoc.isValid);
	}

	void Request(const Manager::Active& a_portal, bool a_limitCamera, float a_cameraFloor, bool a_exact)
	{
		std::scoped_lock guard(lock);
		pending = a_portal;
		pendingLimitsCamera = a_limitCamera;
		pendingCameraFloor = a_cameraFloor;
		pendingExact = a_exact;
	}

	void Update()
	{
		Diag::SamplerSetMainThread();
		std::scoped_lock guard(lock);
		if (purgeDeadline && (!LoadsPending() || Clock::now() >= *purgeDeadline)) {
			const bool timedOut = Clock::now() >= *purgeDeadline;
			purgeDeadline.reset();
			RE::TES::GetSingleton()->PurgeBufferedCells();
			Portal::Manager::EndHoldoff();
			Note(std::format("cells of the worldspace left purged{}", timedOut ? " (loads still pending)" : ""));
		}
		if (!trace.running) {
			return;
		}
		const auto now = Clock::now();
		const auto frameMs = Ms(now - trace.lastFrame);
		trace.lastFrame = now;
		++trace.frames;
		trace.worstFrameMs = std::max(trace.worstFrameMs, frameMs);
		auto*      player = RE::PlayerCharacter::GetSingleton();
		auto*      cell = player->GetParentCell();
		const auto menus = OpenMenus();
		if (frameMs > 40.0 || *loadScreenUp || !menus.empty()) {
			Note(std::format("frame {} took {:.1f} ms, load screen flag {}, menus: {}", trace.frames, frameMs, *loadScreenUp, menus));
		}
		if (trace.arrivedMs < 0.0 && cell && cell->GetFormID() != trace.fromCell) {
			trace.arrivedMs = Ms(now - trace.start);
			const auto pos = player->GetPosition();
			Note(std::format("arrived in {} at ({:.0f},{:.0f},{:.0f}) yaw {:.1f}", Probe::DescribeCell(cell), pos.x, pos.y, pos.z, RE::rad_to_deg(player->GetAngleZ())));
		}
		if (Ms(now - trace.start) > 3000.0) {
			trace.running = false;
			lastResult = {
				{ "door", std::format("{:08X}", trace.door) },
				{ "arrivedMs", trace.arrivedMs },
				{ "frames", trace.frames },
				{ "worstFrameMs", trace.worstFrameMs },
				{ "events", trace.events },
			};
			Note(std::format("trace done: arrived after {:.1f} ms, worst frame {:.1f} ms", trace.arrivedMs, trace.worstFrameMs));
		}
	}

	std::optional<float> CameraLimit()
	{
		std::scoped_lock guard(lock);
		if (!arrival) {
			return std::nullopt;
		}
		auto*       player = RE::PlayerCharacter::GetSingleton();
		const auto  offset = player->GetPosition() - arrival->center;
		const float depth = offset.x * arrival->inward.x + offset.y * arrival->inward.y;  // > 0: inside
		if (depth > 400.0f || Clock::now() - arrival->at > std::chrono::seconds(20)) {
			SetArrivalDoorOpen(false);
			arrival.reset();
			return std::nullopt;
		}
		// Out of the door's way, or turning back to it, the player gets its collision back, and the
		// door is drawn again. Walkable until the portal back through it was ready (up to seconds
		// outdoors, where the view back waits for the purge), the player could turn round and walk
		// back through: past a city gate into the street worldspace's low-detail copy of the city,
		// left there with no crossing back. Hidden as long, a door walked out of stood open with no
		// leaves in it. Turning back while still touching it, the player is pushed out of it on the
		// side they stand on, the one they came out on.
		if (arrivalDoor.door) {
			if (!arrival->firstDepth) {
				arrival->firstDepth = depth;  // the hold begins with the teleport, not with the request
			}
			constexpr float kTurnedBack = 4.0f;
			if (depth >= arrival->clear || depth < *arrival->firstDepth - kTurnedBack) {
				SetArrivalDoorOpen(false);
			}
		}
		// Once the portal back out through it is ready (drawn or merged), the door is held swung
		// open by the portal: hidden on top of that, the room's doorway had no door at all from
		// inside for as long as the player stayed near it.
		if (arrivalDoor.door) {
			if (const auto ready = Portal::Manager::ReadyPortal(); ready && ready->sourceDoor == arrival->door) {
				SetArrivalDoorOpen(false);
			}
		}
		if (!arrival->limitCamera) {
			return std::nullopt;
		}
		// Let out again as the player walks away from the doorway (see PortalManager's pull-in).
		// Only until the doorway behind is shown again: from then on the portal's own camera stop
		// (CameraLimit::SetPlane) keeps the camera on this side of it. Held by depth alone, the
		// camera stayed at the player's head in a small room for as long as they stood near the
		// door (up to 20 s, then jumped out); it is let out at a steady pace instead.
		const float held = std::max(arrival->cameraFloor, depth - kArrivalStep);
		if (!arrival->released && Portal::Manager::ReadyPortal() && !Portal::Manager::Merged()) {
			arrival->released = Clock::now();
		}
		if (!arrival->released) {
			return held;
		}
		constexpr float kLetOutPerSecond = 500.0f;
		const float     let = held + kLetOutPerSecond * std::chrono::duration<float>(Clock::now() - *arrival->released).count();
		if (let > 1000.0f) {
			arrival->limitCamera = false;
			return std::nullopt;
		}
		return let;
	}

	void CloseArrivalDoor()
	{
		std::scoped_lock guard(lock);
		// `arrival` itself stays: the camera is still let out as before; only the door is shut.
		SetArrivalDoorOpen(false);
	}

	void ForgetTrip()
	{
		std::scoped_lock guard(lock);
		ForgetArrival();
		// The cells it was to purge are gone with the game they were loaded in; left to run, it
		// ended the holdoff that keeps the loaded game's first frames free of attaches.
		purgeDeadline.reset();
	}

	void ProfileNext(const std::filesystem::path& a_file)
	{
		std::scoped_lock guard(lock);
		profileFile = a_file;
	}

	json Status()
	{
		std::scoped_lock guard(lock);
		return { { "running", trace.running }, { "pending", pending.has_value() }, { "last", lastResult } };
	}
}
