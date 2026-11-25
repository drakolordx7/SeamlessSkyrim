#include "Portal/Crossing.h"

#include "Doors/Geometry.h"
#include "Doors/Hold.h"
#include "Portal/CameraLimit.h"
#include "Portal/Gates.h"
#include "Portal/MergedView.h"
#include "Portal/PortalDoor.h"
#include "Portal/PortalManager.h"
#include "Portal/Selection.h"
#include "Portal/StreetMerge.h"
#include "Spaces/RemoteCell.h"

namespace Portal::Crossing
{
	namespace
	{
		float crossCooldown = 0.0f;
		// After handing a cell to the engine's door transition, don't grab it back while the
		// transition is still pending (the next frame re-attached it right before the engine took it).
		float attachHoldoff = 0.0f;

		// Merged, the player walks into the doorway until the camera has followed them through it;
		// but behind an exterior door is the building's own collision (its volume is solid). Whatever
		// blocks the way in is made non-collidable while the player is in the doorway.
		std::vector<Portal::Doors::OpenState> passage;
		float                                 passageCheck = 0.0f;

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
			const float     scale = RE::bhkWorld::GetWorldScale();
			RE::bhkPickData pick;
			pick.rayInput.from = RE::hkVector4(a_from.x * scale, a_from.y * scale, a_from.z * scale, 0.0f);
			pick.rayInput.to = RE::hkVector4(a_to.x * scale, a_to.y * scale, a_to.z * scale, 0.0f);
			pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kCharController);
			if (!a_world->PickObject(pick) || !pick.rayOutput.HasHit()) {
				return nullptr;
			}
			a_fraction = pick.rayOutput.hitFraction;
			return RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
		}

		void OpenPassage(RE::PlayerCharacter* a_player, const Portal::Doors::Mapping& a_mapping, float a_halfHeight)
		{
			auto* cell = a_player->GetParentCell();
			auto* world = cell ? cell->GetbhkWorld() : nullptr;
			if (!world) {
				return;
			}
			// Waist and knee height across the doorway, from just in front of it to a room's depth.
			const float floor = a_mapping.sourceCenter.z - a_halfHeight;
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
						// (Also the merged room's own collision copied into the world, MergeCollision: it
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

		// How far the camera's near plane is from the camera, and where the doorway's picture lies
		// (behind the panel for a swinging door, in front of it for a city gate, which keeps its
		// picture in front of its panel, Render::PortalPass::IsGate).
		float NearPlane()
		{
			const auto* worldCamera = RE::Main::WorldRootCamera();
			return worldCamera ? worldCamera->GetRuntimeData2().viewFrustum.fNear : 15.0f;
		}

		float PictureDepth(const Doors::Mapping& a_mapping, bool a_gate)
		{
			return a_gate ? -(a_mapping.halfDepth + 2.0f) : a_mapping.halfDepth + 2.0f;
		}
	}

	void Tick(float a_delta)
	{
		crossCooldown -= a_delta;
		attachHoldoff -= a_delta;
	}

	bool Ready()
	{
		return crossCooldown <= 0.0f;
	}

	bool AttachHeld()
	{
		return attachHoldoff > 0.0f;
	}

	void HoldAttach(float a_seconds)
	{
		attachHoldoff = std::max(attachHoldoff, a_seconds);
	}

	void EndHoldoff(float a_seconds)
	{
		attachHoldoff = a_seconds;
		Selection::ScanNow();
	}

	void Request(const Travel::CrossingRequest& a_request)
	{
		crossCooldown = 1.0f;
		attachHoldoff = 3.0f;
		Travel::Request(a_request);
	}

	void ThroughDoor(RE::TESObjectREFR* a_door)
	{
		crossCooldown = 1.0f;
		BeforeEngineTrip(a_door);
		Travel::ThroughDoor(a_door);
	}

	// The player is about to take a_door the engine's way (its loading screen): the engine gets the
	// cells it leads into, not ones attached under our scene.
	void BeforeEngineTrip(RE::TESObjectREFR* a_door)
	{
		const auto portal = Manager::CurrentPortal();
		if (portal && a_door->GetFormID() == portal->sourceDoor) {
			attachHoldoff = 3.0f;
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

	bool FeetAtDoorway(float a_feetZ, const Portal::Doors::Mapping& a_mapping)
	{
		const float bottom = a_mapping.sourceCenter.z - a_mapping.halfHeight;
		return a_feetZ >= bottom - 64.0f && a_feetZ <= bottom + a_mapping.halfHeight;
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
		// In portal mode the camera must still be in front of the portal quad (2 units in front of
		// the panel) when the crossing is carried out.
		constexpr float kCrossLead = 4.0f;
		constexpr float kPullInDistance = 400.0f;
		// Merged, the player behind the doorway is on the exterior's physics (no interior floor or
		// furniture there yet), so not for long, even with the camera turned to look back out.
		constexpr float kMergedMaxDepth = 250.0f;
		// The street merged into the interior: crossing out is as without merging; the doors are the merge's.
		const bool      merged = Manager::Space().Merged() && !StreetMerge::Merged();
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
		const auto  DepthOf = [&](const RE::NiPoint3& a_point) {
			const auto offset = a_point - mapping.sourceCenter;
			return offset.x * mapping.inward.x + offset.y * mapping.inward.y;  // < 0: in front
		};
		const auto  offset = a_player->GetPosition() - mapping.sourceCenter;
		const float depth = DepthOf(a_player->GetPosition());
		const float side = std::abs(offset.x * mapping.inward.y - offset.y * mapping.inward.x);
		// Behind the door plane counts too: someone who got past it while a crossing wasn't
		// possible yet (the cooldown) is standing in the void behind the doorway and must still be
		// carried through.
		if (depth < -kPullInDistance || depth > kMergedMaxDepth + 50.0f || side > mapping.halfWidth + 150.0f) {
			// Walked off the passage sideways: its building's collision back on under them.
			if (!passage.empty()) {
				ClosePassage();
			}
			return std::nullopt;
		}
		// Only at the doorway's own height. Walking over the flat roof of Whiterun's guard house by
		// the gate was within its door's width and depth, and carried the player inside. (In the
		// passage the player is held at the height they entered it with, HoldPassageHeight.)
		if (!FeetAtDoorway(a_player->GetPositionZ(), mapping)) {
			// A passage through the building (OpenPassage) must not stay open, its collision off,
			// under someone who isn't at the doorway.
			if (!passage.empty()) {
				ClosePassage();
			}
			return std::nullopt;
		}
		const float cameraDepth = DepthOf(CameraPosition());
		const bool  pullIn = !merged && !MergedView::Mergeable(a_portal.cell, a_player->GetParentCell());
		// Only right at the threshold: the building's collision is also the porch and the steps in
		// front of the door, and with it gone earlier the player sank into the ground on the way up.
		// Whatever the mode: a door can stand against a wall that closes the void behind it
		// (Riften's jail; the Hall of the Elements' door up to the Arcanaeum, 4 units behind the
		// door's face, with nothing merged), and the player stopped short of the crossing.
		// Only on the way in: arrived through the door at its marker 9 units in front of it, the
		// player was held 10 units above the floor they were set down over (HoldPassageHeight).
		// Facing in and moving, not getting nearer: someone already stopped by the wall gets
		// no nearer.
		const float yaw = a_player->GetAngleZ();
		const bool  walkingIn = a_player->IsMoving() && std::sin(yaw) * mapping.inward.x + std::cos(yaw) * mapping.inward.y > 0.5f;
		if (depth > -25.0f && side <= mapping.halfWidth + 20.0f && (walkingIn || !passage.empty())) {
			// Seconds, not frames: counted per frame it looked again twice as often at 120 fps.
			if (passage.empty() || (passageCheck -= a_delta) <= 0.0f) {
				passageCheck = 1.0f / 6.0f;  // what the per-frame count came to at 60 fps
				OpenPassage(a_player, mapping, mapping.halfHeight);
			}
		} else if (!passage.empty() && depth < -45.0f) {
			ClosePassage();
		}
		if (!passage.empty()) {
			HoldPassageHeight(a_player, true);
		}
		const bool  inDoorway = side <= mapping.halfWidth + 20.0f;
		// Merged: behind the door plane the player walks on the exterior's ground inside the
		// building's shell (it sinks or rises there), so only a short way; the camera is eased in
		// on the last steps so that it is through the doorway by then, never closer than
		// kMergedCameraMin (the player stays in the picture).
		constexpr float kMergedCrossDepth = 60.0f;
		constexpr float kMergedCameraMin = 55.0f;
		// The portal quad lies on the door's face, half its thickness in front of the centre the
		// depth is measured from: a thick door (a city gate) is crossed that much earlier, or the
		// camera would be through the quad, looking at the gate's planks, before the player is.
		const float     lead = kCrossLead + mapping.halfDepth;
		// A gate's arrival marker can be within that distance of it: the player, just arrived and
		// standing there, was carried straight back, every two seconds. Only someone walking
		// towards the doorway is carried through.
		static RE::FormID lastDoor = 0;
		static float      lastDepth = 0.0f;
		const bool        approaching = lastDoor == a_portal.sourceDoor && depth > lastDepth + 0.05f;
		lastDoor = a_portal.sourceDoor;
		lastDepth = depth;
		// In first person nothing holds the camera back from the doorway (the stop at its plane,
		// CameraLimit, is the third-person camera's), and it is at the head, a little ahead of the
		// body and bobbing with each step: it got to the doorway's picture before the player did,
		// and from beyond it the room's own view showed the door from its far side, flashing with
		// every step close to the door. The player is carried through as soon as the camera's near
		// plane would reach the picture (behind the panel for a swinging door, in front for a gate).
		bool cameraAtPicture = false;
		if (auto* playerCamera = RE::PlayerCamera::GetSingleton(); !merged && playerCamera && playerCamera->IsInFirstPerson()) {
			const float pictureDepth = PictureDepth(mapping, Gates::IsGate(a_player->GetParentCell(), a_portal.cell));
			const auto  cameraOffset = CameraPosition() - mapping.sourceCenter;
			const float cameraSide = std::abs(cameraOffset.x * mapping.inward.y - cameraOffset.y * mapping.inward.x);
			cameraAtPicture = cameraDepth + NearPlane() + 2.0f >= pictureDepth && cameraSide <= mapping.halfWidth + 20.0f && inDoorway && approaching;
		}
		const bool      cross = merged ? (depth >= 15.0f && cameraDepth >= 2.0f) || depth >= kMergedCrossDepth :
		                                 (depth >= -lead && inDoorway && (approaching || depth >= 0.0f)) || cameraAtPicture;
		if (crossCooldown <= 0.0f && cross) {
			logger::info("portal: crossing at depth {:.1f}, side {:.1f}, camera depth {:.1f}{}", depth, side, cameraDepth, merged ? " (merged)" : "");
			Request({ .portal = a_portal, .limitCamera = pullIn || merged, .cameraFloor = merged ? kMergedCameraMin : 0.0f });
		}
		if (merged) {
			return inDoorway || depth > 0.0f ? std::optional(kMergedCameraMin + std::max(0.0f, kMergedCrossDepth - depth)) : std::nullopt;
		}
		return pullIn ? std::optional(std::max(0.0f, -depth - lead)) : std::nullopt;
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
				const float pictureDepth = PictureDepth(*mapping, Gates::IsGate(a_here, a_ready->cell));
				Portal::CameraLimit::SetFirstPersonStop(mapping->sourceCenter, mapping->inward, mapping->halfWidth, pictureDepth - NearPlane() - 1.0f);
				return;
			}
		}
		Portal::CameraLimit::ClearPlane();
	}
}
