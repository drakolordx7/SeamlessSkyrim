#include "Skyrim/Portal/Crossing.h"

#include "Core/Crossing/Approach.h"

#include "Skyrim/Doors/Geometry.h"
#include "Skyrim/Doors/Hold.h"
#include "Skyrim/Engine/CoreConvert.h"
#include "Skyrim/Engine/Havok.h"
#include "Skyrim/Portal/CameraLimit.h"
#include "Skyrim/Portal/Gates.h"
#include "Skyrim/Portal/MergedView.h"
#include "Skyrim/Portal/PortalDoor.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/Selection.h"
#include "Skyrim/Portal/StreetMerge.h"
#include "Skyrim/Spaces/RemoteCell.h"

namespace Portal::Crossing
{
	namespace
	{
		Core::Crossing::Timers        timers;
		Core::Crossing::ApproachState approach;

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

		void OpenPassage(RE::PlayerCharacter* a_player, const Portal::Doors::Mapping& a_mapping)
		{
			auto* cell = a_player->GetParentCell();
			auto* world = cell ? cell->GetbhkWorld() : nullptr;
			if (!world) {
				return;
			}
			// Waist and knee height across the doorway, from just in front of it to a room's depth.
			const float floor = a_mapping.sourceCenter.z - a_mapping.halfHeight;
			for (const float height : { 70.0f, 30.0f }) {
				auto from = a_mapping.sourceCenter - a_mapping.inward * 20.0f;
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
					Portal::Doors::Open(blocker, passage.emplace_back());
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
		Selection::ScanNow();
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
			if (const auto kept = Selection::FindKept(cell); kept != Selection::Kept().end()) {
				Selection::LetGoKept(kept, "player took a door into it");
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
			logger::info("portal: crossing at depth {:.1f}, side {:.1f}, camera depth {:.1f}{}", decision.depth, decision.side, decision.cameraDepth, merged ? " (merged)" : "");
			Request({ .portal = a_portal, .limitCamera = decision.limitCamera, .cameraFloor = decision.cameraFloor });
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
