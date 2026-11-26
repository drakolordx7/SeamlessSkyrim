#include "Skyrim/Portal/Crossing.h"

#include "Core/Crossing/Approach.h"
#include "Core/Crossing/Sequence.h"

#include "Skyrim/Doors/Geometry.h"
#include "Skyrim/Doors/Hold.h"
#include "Skyrim/Graph/DoorScan.h"
#include "Skyrim/Engine/CoreConvert.h"
#include "Skyrim/Engine/Havok.h"
#include "Skyrim/Portal/CameraLimit.h"
#include "Skyrim/Portal/Gates.h"
#include "Skyrim/Portal/MergedView.h"
#include "Skyrim/Portal/PortalDoor.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/Schedule.h"
#include "Skyrim/Portal/Selection.h"
#include "Skyrim/Portal/StreetMerge.h"
#include "Skyrim/Spaces/RemoteCell.h"

namespace Portal::Crossing
{
	namespace
	{
		Core::Crossing::Timers        timers;
		Core::Crossing::ApproachState approach;
		// The order of one crossing and its timeouts (Core/Crossing/Sequence.h): what is done is here.
		Core::Crossing::Sequence      sequence;
		// The crossing the player's step into the doorway asks for, while the sequence answers it.
		std::optional<Travel::CrossingRequest> request;

		// Merged, the player walks into the doorway until the camera has followed them through it;
		// but behind an exterior door is the building's own collision (its volume is solid). Whatever
		// blocks the way in is made non-collidable while the player is in the doorway.
		std::vector<Portal::Doors::OpenState> passage;

		// With the building's collision off, its floor (the threshold, often the porch too) is gone as
		// well, and the player dropped up to 90 units onto the ground under the building. While the
		// passage is open they are held at the height they had when it opened: no gravity, and no
		// falling as far as the engine's fall time and damage are concerned.
		std::optional<float> passageGravity;  // the controller's own value, to put back
		float                passageFloor = 0.0f;
		bool                 passageHeldThisFrame = false;

		void HoldPassageHeight(RE::PlayerCharacter* a_player, bool a_hold)
		{
			auto* controller = a_player->GetCharController();
			if (!controller) {
				// The saved value is kept for when the controller is back (a crossing's teleport can take
				// it away for a frame). Dropped here, the next hold saved the 0 it had set itself, and the
				// player walked out of the doorway with no gravity for good.
				return;
			}
			if (a_hold) {
				passageHeldThisFrame = true;
				if (!passageGravity) {
					passageGravity = controller->gravity;
					passageFloor = a_player->GetPositionZ();
				}
				controller->gravity = 0.0f;
				controller->fallTime = 0.0f;
				controller->fallStartHeight = passageFloor;
				RE::hkVector4 velocity;
				controller->GetLinearVelocityImpl(velocity);
				if (velocity.quad.m128_f32[2] < 0.0f) {
					velocity.quad.m128_f32[2] = 0.0f;
					controller->SetLinearVelocityImpl(velocity);
				}
			} else if (passageGravity) {
				controller->gravity = *passageGravity;
				passageGravity.reset();
			}
		}

		RE::TESObjectREFR* Blocker(RE::bhkWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float& a_fraction)
		{
			const auto hit = Engine::Havok::Ray(a_world, a_from, a_to, RE::COL_LAYER::kCharController);
			if (!hit) {
				return nullptr;
			}
			a_fraction = hit->fraction;
			return hit->Ref();
		}

		// Something of the player's own space (not a person, not the door held open) stands in the
		// way within a_reach units ahead, towards the doorway, at waist height: a wall closing the
		// doorway from this side (Core::Crossing::Approach::blockedAhead).
		bool BlockedAhead(RE::PlayerCharacter* a_player, const Portal::Doors::Mapping& a_mapping, float a_reach)
		{
			auto* cell = a_player->GetParentCell();
			auto* world = cell ? cell->GetbhkWorld() : nullptr;
			if (!world) {
				return false;
			}
			auto from = a_player->GetPosition();
			from.z = a_mapping.sourceCenter.z - a_mapping.halfHeight + 70.0f;
			float fraction = 1.0f;
			auto* blocker = Blocker(world, from, from + a_mapping.inward * a_reach, fraction);
			return blocker && blocker != a_player && !blocker->As<RE::Actor>() && !Portal::Doors::IsOpen(blocker->GetFormID());
		}

		void OpenPassage(RE::PlayerCharacter* a_player, const Portal::Doors::Mapping& a_mapping)
		{
			auto* cell = a_player->GetParentCell();
			auto* world = cell ? cell->GetbhkWorld() : nullptr;
			if (!world) {
				return;
			}
			// Waist and knee height across the doorway, from just in front of it to a room's depth.
			const float floor = a_mapping.sourceCenter.z - a_mapping.halfHeight;
			// From the player if they stand further back, held by a wall closer to the doorway than
			// the stand-off allows them to get (Approach's kBlockedReach): it stands between.
			const float back = std::max(20.0f, -(a_player->GetPosition() - a_mapping.sourceCenter).Dot(a_mapping.inward));
			for (const float height : { 70.0f, 30.0f }) {
				auto from = a_mapping.sourceCenter - a_mapping.inward * back;
				auto to = a_mapping.sourceCenter + a_mapping.inward * 250.0f;
				from.z = to.z = floor + height;
				for (int attempt = 0; attempt < 8; ++attempt) {
					float fraction = 1.0f;
					auto* blocker = Blocker(world, from, to, fraction);
					if (!blocker || blocker == a_player) {
						break;
					}
					const bool ofRoom = std::ranges::find(Manager::Space().Cells(), blocker->GetParentCell()) != Manager::Space().Cells().end();
					if (Portal::Doors::IsOpen(blocker->GetFormID()) || ofRoom) {
						// (Or the space behind the doorway, which shares the Havok world at a city gate: it
						// is meant to be there.)
						// A door held open can still stop the ray (Whiterun's doors have a body the layer
						// change doesn't reach) though not the player: what blocks the way is behind
						// it, and with the ray ending here it was never found (the Bannered Mare and the
						// houses around it couldn't be entered). The search goes on just past the hit: the
						// wall closing Sadri's Used Wares' doorway in Windhelm stands 2 units behind its
						// door's back face, and a step of 2 started the next ray on it, which missed it.
						from = from + (to - from) * fraction + a_mapping.inward * 0.1f;
						if ((to - from).Dot(a_mapping.inward) < 5.0f) {
							break;
						}
						continue;
					}
					if (blocker->As<RE::Actor>()) {
						// Someone standing in the doorway: they block it as anywhere else (made
						// non-collidable they could be walked through). Looked for again shortly.
						break;
					}
					logger::info("portal: doorway passage through {:08X} '{}'", blocker->GetFormID(), blocker->GetBaseObject() ? blocker->GetBaseObject()->GetName() : "");
					// Out of the Havok world, not just on the non-collidable layer: Riften's building
					// shells (the Bee and Barb, Haelga's Bunkhouse) kept the player 25 units short of
					// them after the layer change, the doorway never crossed.
					Portal::Doors::Open(blocker, passage.emplace_back(), { .passable = true });
				}
			}
		}

		void Run(const std::vector<Core::Crossing::Action>& a_actions, Core::Crossing::Phase a_before)
		{
			using Core::Crossing::ActionKind;
			for (const auto& action : a_actions) {
				switch (action.kind) {
				case ActionKind::PrewarmSpace:
					// The scheduler holds the space behind an open door that is near; nothing more to ask of it.
					logger::info("crossing: preparing the space behind door {:08X}", Graph::DoorForm(sequence.Current().door));
					break;
				case ActionKind::CancelPrewarm:
				case ActionKind::OpenDoor:  // the door swings by itself (a stand-in, then the portal)
				case ActionKind::StartCover:
				case ActionKind::EndCover:
					break;
				case ActionKind::Teleport:
					if (request) {
						Crossing::Request(*std::exchange(request, std::nullopt));
					} else {
						// Nothing to carry out (the player is not in the doorway): the engine's own door.
						logger::warn("crossing: asked to cross with no request");
						Run(sequence.Handle({ Core::Crossing::EventKind::Failed }), Core::Crossing::Phase::Cross);
					}
					break;
				case ActionKind::ReleaseSpace:
					// Let go when the player's cell changed (Manager::ReleaseAll).
					Run(sequence.Handle({ Core::Crossing::EventKind::Released }), Core::Crossing::Phase::Release);
					break;
				case ActionKind::VanillaDoor: {
					const auto form = Graph::DoorForm(action.door);
					Schedule::Fallback(form, a_before == Core::Crossing::Phase::Cross ? "the crossing did not arrive in time" : "the space behind it was not ready in time");
					if (a_before == Core::Crossing::Phase::Cross) {
						// In the doorway and not carried through: the engine's own door, with its loading screen.
						if (auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(form)) {
							ThroughDoor(door);
						}
					} else {
						// No one is taking it yet: the door is an ordinary door from now on.
						Run(sequence.Handle({ Core::Crossing::EventKind::Crossed }), Core::Crossing::Phase::Fallback);
					}
					break;
				}
				}
			}
		}

		// How far the camera's near plane is from the camera.
		float NearPlane()
		{
			const auto* worldCamera = RE::Main::WorldRootCamera();
			return worldCamera ? worldCamera->GetRuntimeData2().viewFrustum.fNear : 15.0f;
		}
	}

	void Tick(float a_delta)
	{
		timers.Tick(a_delta);
	}

	void Drive(float a_delta)
	{
		using namespace Core::Crossing;
		const auto  before = sequence.GetPhase();
		const auto& ahead = Schedule::ApproachedDoorway();
		const auto  portal = Manager::CurrentPortal();
		// The player stepped away from the door the crossing was prepared for.
		if (sequence.Busy() && before >= Phase::Approach && before <= Phase::Commit && (!ahead || ahead->target.door != sequence.Current().door)) {
			logger::info("crossing: dropped, the player left door {:08X}", Graph::DoorForm(sequence.Current().door));
			Run(sequence.Handle({ EventKind::Abort }), before);
		}
		if (ahead && !sequence.Busy()) {
			Run(sequence.Handle({ EventKind::Begin, ahead->target }), Phase::Idle);
			Run(sequence.Handle({ EventKind::Near }), Phase::Approach);
		}
		// The space behind the door is shown and loaded; the door stands open.
		if (ahead && portal && portal->sourceDoor == ahead->door) {
			if (sequence.GetPhase() == Phase::Prewarm && Manager::ReadyPortal()) {
				Run(sequence.Handle({ EventKind::Prewarmed }), Phase::Prewarm);
			}
			constexpr float kOpen = 0.9f;
			const bool      open = Manager::Merged() ? MergedView::ShownAmount() >= kOpen : PortalDoor::Holds(portal->sourceDoor) && PortalDoor::Amount() >= kOpen;
			if (sequence.GetPhase() == Phase::Open && open) {
				Run(sequence.Handle({ EventKind::DoorOpened }), Phase::Open);
			}
		}
		const auto phase = sequence.GetPhase();
		Run(sequence.Tick(a_delta), phase);
	}

	void Arrived()
	{
		using namespace Core::Crossing;
		const auto phase = sequence.GetPhase();
		Run(sequence.Handle({ EventKind::Crossed }), phase);
	}

	std::string_view PhaseName()
	{
		return Core::Crossing::Name(sequence.GetPhase());
	}

	bool Ready()
	{
		return timers.Ready();
	}

	bool AttachHeld()
	{
		return timers.AttachHeld();
	}

	void HoldAttach(float a_seconds)
	{
		timers.HoldAttach(a_seconds);
	}

	void EndHoldoff(float a_seconds)
	{
		timers.EndHoldoff(a_seconds);
	}

	void Request(const Travel::CrossingRequest& a_request)
	{
		timers.CrossingRequested();
		Travel::Request(a_request);
	}

	void ThroughDoor(RE::TESObjectREFR* a_door)
	{
		timers.EngineTripBegins();
		BeforeEngineTrip(a_door);
		Travel::ThroughDoor(a_door);
	}

	// The player is about to take a_door the engine's way (its loading screen): the engine gets the
	// cells it leads into, not ones attached under our scene.
	void BeforeEngineTrip(RE::TESObjectREFR* a_door)
	{
		const auto portal = Manager::CurrentPortal();
		if (portal && a_door->GetFormID() == portal->sourceDoor) {
			timers.EngineTripThroughPortal();
			Portal::Manager::ReleaseAll("player took the portal door the engine's way");
		}
		// A door into a space kept out of sight leads the engine into a cell attached under our
		// scene: it is let go first.
		auto* teleport = a_door->extraList.GetByType<RE::ExtraTeleport>();
		auto  linked = teleport && teleport->teleportData ? teleport->teleportData->linkedDoor.get() : nullptr;
		if (auto* cell = linked ? linked->GetParentCell() : nullptr) {
			if (Schedule::FindKept(cell) != Schedule::Kept().end()) {
				Schedule::LetGoKept(cell, "player took a door into it");
				HoldAttach(3.0f);
			}
		}
	}

	RE::NiPoint3 CameraPosition()
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera || !camera->cameraRoot) {
			return RE::PlayerCharacter::GetSingleton()->GetPosition();
		}
		// From the local transform the camera's Update just set: the world one follows only in the
		// frame's scene update, and right after a crossing it was still at the door left behind (the
		// image space chosen for there: a room walked out into flashed white for one frame).
		const auto* root = camera->cameraRoot.get();
		return root->parent ? root->parent->world * root->local.translate : root->local.translate;
	}

	// When to carry the player through, and what the camera may do meanwhile:
	// - merged (outside, the room drawn in the world): the player walks into the doorway and the
	//   crossing waits until the camera has followed them through it, so both are inside when the
	//   space changes;
	// - portal pass with a mergeable space on the other side (inside, going out): crossing as the
	//   player reaches the plane; the camera, still inside, is then in the merged room;
	// - portal pass otherwise (interior to interior): a camera left on the far side of a portal
	//   can't be drawn well (the doorway quad's "camera behind" composite, Pictures/Composite.cpp),
	//   so it is pulled in to reach the doorway together with the player (Portal::CameraLimit).
	std::optional<float> Approach(RE::PlayerCharacter* a_player, const Active& a_portal, float a_delta)
	{
		const bool merged = Manager::Space().Merged() && !StreetMerge::Merged();
		if (!merged && !StreetMerge::Merged() && !PortalDoor::Holds(a_portal.sourceDoor)) {
			return std::nullopt;
		}
		// A shut door is walked into, not through: the building behind it is left solid, and the
		// player isn't held at the doorway's height against it.
		if (Manager::DoorShut()) {
			if (!passage.empty()) {
				ClosePassage();
			}
			return std::nullopt;
		}
		const auto found = Portal::Doors::TryCompute(a_portal);
		if (!found) {
			return std::nullopt;
		}
		const auto& mapping = *found;
		auto*       playerCamera = RE::PlayerCamera::GetSingleton();
		const auto  position = a_player->GetPosition();
		const auto  cameraPosition = CameraPosition();
		const auto  yaw = a_player->GetAngleZ();
		const bool  firstPerson = !merged && playerCamera && playerCamera->IsInFirstPerson();
		// Only where a held-back player could be: between the passage's usual reach and the blocked one.
		const float depthNow = Core::Crossing::DepthOf(Doors::DoorwayOf(mapping), ToCore(position));
		const bool  mayBeBlocked = passage.empty() && depthNow < -Core::Crossing::kPassageOpenDepth && depthNow > -Core::Crossing::kBlockedReach;
		const auto  decision = approach.Decide({
			.door = a_portal.sourceDoor,
			.doorway = Doors::DoorwayOf(mapping),
			.player = ToCore(position),
			.playerYaw = yaw,
			.playerMoving = a_player->IsMoving(),
			.camera = ToCore(cameraPosition),
			.delta = a_delta,
			.merged = merged,
			.pullIn = !merged && !MergedView::Mergeable(a_portal.cell, a_player->GetParentCell()),
			.firstPerson = firstPerson,
			.gate = firstPerson && Gates::IsGate(a_player->GetParentCell(), a_portal.cell),
			.nearPlane = firstPerson ? NearPlane() : 0.0f,
			.passageOpen = !passage.empty(),
			.blockedAhead = mayBeBlocked && BlockedAhead(a_player, mapping, std::min(Core::Crossing::kStandOff, -depthNow)),
		});
		if (decision.closePassage && !passage.empty()) {
			ClosePassage();
		}
		if (!decision.active) {
			return std::nullopt;
		}
		if (decision.openPassage) {
			OpenPassage(a_player, mapping);
		}
		if (!passage.empty()) {
			HoldPassageHeight(a_player, true);
		}
		if (Ready() && decision.cross) {
			using namespace Core::Crossing;
			logger::info("portal: crossing at depth {:.1f}, side {:.1f}, camera depth {:.1f}{}", decision.depth, decision.side, decision.cameraDepth, merged ? " (merged)" : "");
			request = Travel::CrossingRequest{ .portal = a_portal, .limitCamera = decision.limitCamera, .cameraFloor = decision.cameraFloor };
			// The portal is shown and its door is not shut: the way is ready whatever the sequence has
			// been told so far, so it is brought up to the threshold (its timeouts still guard the rest).
			const auto  door = Graph::DoorOf(a_portal.sourceDoor);
			const auto& graph = Graph::Get();
			Target      target;
			if (door != Core::Graph::kNoDoor && door < graph.DoorCount()) {
				const auto& d = graph.GetDoor(door);
				const auto& way = graph.GetDoorway(d.doorway);
				target = { d.doorway, door, d.space, way.Other(d.space) };
			}
			for (const auto step : { EventKind::Begin, EventKind::Near, EventKind::Prewarmed, EventKind::DoorOpened }) {
				const auto phase = sequence.GetPhase();
				const bool due = (step == EventKind::Begin && phase == Phase::Idle) || (step == EventKind::Near && phase == Phase::Approach) ||
				                 (step == EventKind::Prewarmed && phase == Phase::Prewarm) || (step == EventKind::DoorOpened && phase == Phase::Open);
				if (due) {
					Run(sequence.Handle({ step, target }), phase);
				}
			}
			const auto phase = sequence.GetPhase();
			Run(sequence.Handle({ EventKind::AtThreshold }), phase);
			// Not carried out (the sequence is busy with the last crossing): the player is in the doorway
			// still next frame, and asks again.
			request.reset();
		}
		return decision.cameraLimit;
	}

	void ClosePassage()
	{
		HoldPassageHeight(RE::PlayerCharacter::GetSingleton(), false);
		for (auto& open : passage) {
			Portal::Doors::Close(open);
		}
		passage.clear();
	}

	void EndFrame(RE::PlayerCharacter* a_player)
	{
		if (!std::exchange(passageHeldThisFrame, false)) {
			HoldPassageHeight(a_player, false);
		}
	}

	void UpdateCameraPlane(RE::TESObjectCELL* a_here, const std::optional<Active>& a_ready)
	{
		if (a_ready) {
			if (const auto mapping = Doors::TryCompute(*a_ready); mapping && (!Manager::Space().Merged() || StreetMerge::Merged())) {
				Portal::CameraLimit::SetPlane(mapping->sourceCenter - mapping->inward * mapping->halfDepth, mapping->inward, mapping->halfWidth);
				// Short of the doorway's picture by the camera's near distance (and a unit beyond
				// where walking in carries the player through, Approach, so that still happens).
				const float pictureDepth = Core::Crossing::PictureDepth(mapping->halfDepth, Gates::IsGate(a_here, a_ready->cell));
				Portal::CameraLimit::SetFirstPersonStop(mapping->sourceCenter, mapping->inward, mapping->halfWidth, Core::Crossing::FirstPersonStop(pictureDepth, NearPlane()));
				return;
			}
		}
		Portal::CameraLimit::ClearPlane();
	}
}
