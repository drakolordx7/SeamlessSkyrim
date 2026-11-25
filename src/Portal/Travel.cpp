#include "Portal/Travel.h"

#include "Diag/Telemetry.h"
#include "Engine/Hooks.h"
#include "Engine/LoadScreen.h"
#include "Portal/CameraLimit.h"
#include "Portal/NpcTravel.h"
#include "Portal/PortalManager.h"
#include "Portal/ResetClothPhysics.h"
#include "Portal/TransitionStalls.h"
#include "Portal/TravelTrace.h"
#include "Render/FlashGuard.h"
#include "Spaces/RemoteCell.h"

namespace
{
	using Clock = std::chrono::steady_clock;
	using Portal::TravelTrace::Note;

	std::optional<Portal::Travel::CrossingRequest> pending;
	std::optional<RE::NiPoint3>                    exactArrival;  // see Begin
	constexpr float                                kArrivalStep = 12.0f;
	constexpr float                                kPlayerRadius = 20.0f;
	std::optional<float>                           arrivalYaw;  // heading to set once the teleport has run
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

	// The door the player came out of stands open behind them, like the one they walked into,
	// until they have stepped out of its way (CameraLimit): no collision (they are standing in its
	// doorway, the camera just behind them), not drawn.
	Portal::Doors::OpenState arrivalDoor;

	void SetArrivalDoorOpen(bool a_open)
	{
		if (a_open && arrival) {
			Portal::Doors::Open(RE::TESForm::LookupByID<RE::TESObjectREFR>(arrival->door), arrivalDoor, { .hide = true });
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

	// Queues the engine's own door teleport for the player through a_door (what activating a load door
	// does, TESObjectDOOR::HandleOpen, ID 17926: a fade to black whose callback calls ID 17924, which
	// queues the teleport, PlayerCharacter::queuedTargetLoc, ID 40442; doors flagged "automatic"
	// skip the fade and call ID 17924 directly). Returns the queued target, for the caller to point
	// somewhere else than the door's marker.
	auto& QueueTeleport(RE::TESObjectREFR* a_door)
	{
		auto* base = a_door->GetBaseObject()->As<RE::TESObjectDOOR>();
		SEAMLESS_ASSERT(base);
		REL::Relocation<void (*)(RE::TESObjectDOOR*, RE::RefHandle*)> teleportThroughDoor{ REL::RelocationID(17523, 17924) };
		RE::RefHandle                                                handle = a_door->GetHandle().native_handle();
		auto&                                                        queued = RE::PlayerCharacter::GetSingleton()->GetPlayerRuntimeData().queuedTargetLoc;
		teleportThroughDoor(base, &handle);
		SEAMLESS_ASSERT(queued.isValid);
		return queued;
	}

	// Queues the engine's own door teleport, without its fade and loading screen, and points it at
	// the place the player is walking to on the other side instead of the door's marker.
	void Begin(const Portal::Travel::CrossingRequest& a_request)
	{
		const auto&            portal = a_request.portal;
		auto*                  player = RE::PlayerCharacter::GetSingleton();
		auto*                  door = RE::TESForm::LookupByID<RE::TESObjectREFR>(portal.sourceDoor);
		Portal::Doors::Mapping mapping;
		try {
			mapping = Portal::Doors::Compute(portal);
		} catch (const std::exception& e) {
			logger::warn("travel: door pair unusable: {}", e.what());
			return;
		}
		Portal::TravelTrace::Begin(door->GetFormID(), player->GetParentCell());
		// The white frame came 2-3 frames after the crossing; a second's worth leaves room for slow ones.
		::Render::FlashGuard::Guard(60);

		// The cell goes back to the engine first: the teleport attaches it as the current cell.
		Portal::Manager::ReleaseAll("player walked through the doorway", 3.0f, portal.cell && !portal.cell->IsInteriorCell());

		Engine::SkipLoadScreen() = true;
		// An open doorway has no reason to wait for the door's fade either. Everything below
		// overwrites the queued teleport and treats the next frame as the arrival.
		auto& queued = QueueTeleport(door);

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
		queuedPos = mapping.toLinked * player->GetPosition() + (a_request.exact ? RE::NiPoint3{} : inwardInside * kArrivalStep);
		if (!a_request.exact) {
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
			if (here && !here->IsInteriorCell() && portal.cell && !portal.cell->IsInteriorCell()) {
				if (const auto reach = CollisionReach(RE::TESForm::LookupByID<RE::TESObjectREFR>(portal.linkedDoor), farCenter, inwardInside, queuedPos.z + 60.0f)) {
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
		if (!a_request.exact) {
			queuedPos.z = std::clamp(queuedPos.z, marker.z - 16.0f, marker.z + 48.0f);
		} else if (portal.cell && !portal.cell->IsInteriorCell()) {
			// Walking out of a room into its merged copy: the place is inside the house's shell,
			// solid until the room is merged and the passage opened after the teleport. Sent there,
			// the engine looked for a free place for 1.2 s and put the player on the ground under
			// the house. They go to the door's marker, and are moved once the room is there.
			exactArrival = queuedPos + RE::NiPoint3{ 0.0f, 0.0f, 4.0f };
			queuedPos = marker;
		}
		auto* from = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		leftExterior = from && !from->IsInteriorCell();
		arrival = Arrival{ mapping.toLinked * mapping.sourceCenter, inwardInside, Clock::now(), portal.linkedDoor, a_request.limitCamera, a_request.cameraFloor,
			mapping.halfDepth + kPlayerRadius };
		queuedAngle = { player->GetAngleX(), 0.0f, player->GetAngleZ() + mapping.yawDelta };
		arrivalYaw = queuedAngle.z;
		Note(std::format("teleport queued: ({:.0f},{:.0f},{:.0f}) yaw {:.1f} instead of marker ({:.0f},{:.0f},{:.0f})", queuedPos.x,
			queuedPos.y, queuedPos.z, RE::rad_to_deg(queuedAngle.z), marker.x, marker.y, marker.z));
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
				const auto request = *pending;
				pending.reset();
				Begin(request);
			}
			const auto before = Clock::now();
			Portal::TransitionStalls::SetSeamless(arrivalYaw.has_value());
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
			if (Portal::TransitionStalls::Seamless() && thirdPerson) {
				cameraPose = CameraPose{ thirdPerson->posOffsetExpected, thirdPerson->posOffsetActual, thirdPerson->targetZoomOffset, thirdPerson->currentZoomOffset,
					thirdPerson->pitchZoomOffset, thirdPerson->targetYaw, thirdPerson->currentYaw };
			}
			const auto  cameraOffset = camera->cameraRoot->world.translate - a_player->GetPosition();
			const float yawBefore = a_player->GetAngleZ();
			{
				const Diag::Scope scope(Portal::TransitionStalls::Seamless() ? "teleport" : "teleport step");
				func(a_player);
			}
			if (Portal::TransitionStalls::Seamless()) {
				Portal::TransitionStalls::SetSeamless(false);
				// The engine only resets the flag after its own short teleports. Ours is done now;
				// left set until the trace ended (3 s), a save loaded or a fast travel in that time
				// went without a loading screen.
				Engine::SkipLoadScreen() = false;
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
				Portal::ResetClothPhysics();
				arrivalFrame = true;
				Note(std::format("camera updated ({:.1f} ms)", Ms(Clock::now() - cameraStart)));
			}
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};
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

	// Every trip of the game's own shows its loading screen; a seamless one never does (Engine::SkipLoadScreen).
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

	// Main::Update (ID 36564), the call returning to +0xAC4 (SE: the call at +0x5E8): the wait for
	// the frame's jobs (ID 35409), after the frame has been drawn. A crossing is carried out here, at
	// the very end of the frame it was detected in, rather than at the start of the next one:
	// everything the engine updates once per frame for the space the player is in (sky and ambient,
	// image space, cloth physics, the camera) then runs for the new space before its first frame is
	// drawn. Done at the start of a frame, that first frame was drawn with the old space's lighting.
	struct EndOfFrameHook
	{
		static void Thunk()
		{
			func();
			Portal::RemoteCell::StepAtEndOfFrame();
			std::unique_lock guard(lock);
			if (pending) {
				const auto request = *pending;
				pending.reset();
				{
					const Diag::Scope scope("crossing begin");
					Begin(request);
				}
				guard.unlock();
				ProcessTeleportHook::Thunk(RE::PlayerCharacter::GetSingleton());
			}
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// How far the camera may trail the player this frame (see Tick).
	std::optional<float> cameraLimit;

	// Moves the arrival on (the door given back, the camera let out) and returns the limit.
	std::optional<float> ArrivalCameraLimit()
	{
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
}

namespace Portal::Travel
{
	void Install()
	{
		TravelTrace::Install();
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
		TransitionStalls::Install();
		REL::Relocation<std::uintptr_t> endOfFrame{ REL::RelocationID(35565, 36564), Hooks::Offset(0x5E8, 0xAAD, 0xABF) };
		if (Hooks::CallsTo(endOfFrame.address(), REL::RelocationID(34554, 35409), "travel: call to ID 35409")) {
			EndOfFrameHook::func = SKSE::GetTrampoline().write_call<5>(endOfFrame.address(), EndOfFrameHook::Thunk);
			endOfFrameHooked = true;
		} else {
			logger::warn("travel: end-of-frame call not found, crossings are carried out at the start of a frame");
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

	void ThroughDoorTo(RE::TESObjectREFR* a_door, const RE::NiPoint3& a_position, float a_yaw)
	{
		auto& queued = QueueTeleport(a_door);
		queued.location = a_position;
		queued.angle = { 0.0f, 0.0f, a_yaw };
		logger::info("travel: after loading, through door {:08X} to ({:.0f},{:.0f},{:.0f})", a_door->GetFormID(), a_position.x, a_position.y, a_position.z);
	}

	void ThroughDoor(RE::TESObjectREFR* a_door)
	{
		QueueTeleport(a_door);
	}

	void Request(const CrossingRequest& a_request)
	{
		std::scoped_lock guard(lock);
		pending = a_request;
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
		TravelTrace::Update();
	}

	void Tick()
	{
		std::scoped_lock guard(lock);
		cameraLimit = ArrivalCameraLimit();
	}

	std::optional<float> CameraLimit()
	{
		std::scoped_lock guard(lock);
		return cameraLimit;
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

	json Status()
	{
		std::scoped_lock guard(lock);
		return { { "running", TravelTrace::Running() }, { "pending", pending.has_value() }, { "last", TravelTrace::Last() } };
	}
}
