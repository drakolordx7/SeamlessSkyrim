#include "Skyrim/Portal/Gates.h"

#include "Core/Render/DoorwayPlanes.h"

#include "Skyrim/Doors/Geometry.h"
#include "Skyrim/Doors/Hold.h"
#include "Skyrim/Doors/Pair.h"
#include "Skyrim/Doors/Policy.h"
#include "Skyrim/Doors/Settings.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/StandIns.h"
#include "Skyrim/Render/Backend.h"
#include "Skyrim/Render/Wire.h"
#include "Skyrim/Spaces/RemoteCell.h"

namespace Portal::Gates
{
	namespace
	{
		// Gates this close are one gateway seen from either side (Whiterun's, 20 units apart, one mesh
		// at two sizes). Solitude's stand 370 apart, the player's worldspace walling the passage up
		// between them.
		constexpr float kApart = 150.0f;
		// The mask reaches a little past the far gate: the gatehouse's wall stands about its plane.
		constexpr float kPastFarGate = 50.0f;

		// The far gate, while the portal's door is open on a gate (HoldFar).
		Doors::OpenState openFarGate;
		bool             gateMasked = false;  // room 0's doorway is the city gate's (UpdateMask)

		// How far behind a_gate (along a_inward) its far gate stands. By the references' origins: the
		// far gate's 3D isn't loaded yet. Both gates are placed the same way about their meshes, so
		// their origins stand apart as their doorways do.
		float Depth(RE::TESObjectREFR* a_gate, RE::FormID a_farGate, const RE::NiPoint3& a_inward)
		{
			return (RE::TESForm::LookupByID<RE::TESObjectREFR>(a_farGate)->GetPosition() - a_gate->GetPosition()).Dot(a_inward);
		}

		// Before the city behind a gate is there, the player's own worldspace shows through it, its
		// low-detail copy of the city (StandIns). Where the two gates stand apart, that
		// worldspace's side of the gatehouse lies between them: Solitude's walls its passage up. Masked
		// up to the far gate, the copy of the street beyond shows. The nearest such gate, as there is
		// one mask.
		std::optional<float> StandInGatePassage(Render::DoorRect& a_door)
		{
			const auto         eye = RE::PlayerCharacter::GetSingleton()->GetPosition();
			RE::TESObjectREFR* gate = nullptr;
			RE::TESObjectREFR* farGate = nullptr;
			for (const auto& [id, standIn] : StandIns::Doorways()) {
				auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				if (!standIn.gate || standIn.picture || !door->Get3D() || (gate && eye.GetDistance(door->GetPosition()) >= eye.GetDistance(gate->GetPosition()))) {
					continue;
				}
				gate = door;
				farGate = RE::TESForm::LookupByID<RE::TESObjectREFR>(standIn.linked);
			}
			if (!gate) {
				return std::nullopt;
			}
			const auto  inward = StandIns::AwayFrom(gate, eye);
			const float depth = Depth(gate, farGate->GetFormID(), inward);
			// Close gates: the player's worldspace has its own passage behind them, and masked, the
			// passage's ceiling opened onto the sky.
			if (depth <= kApart) {
				return std::nullopt;
			}
			const auto  center = Doors::Center(gate);
			const auto  size = Doors::Size(gate);
			a_door = Render::UprightDoor(center, inward, size.halfWidth, size.halfHeight, false);
			return depth + kPastFarGate;
		}
	}

	bool IsGate(const RE::TESObjectCELL* a_here, const RE::TESObjectCELL* a_behind)
	{
		return a_here && !a_here->IsInteriorCell() && a_behind && !a_behind->IsInteriorCell();
	}

	bool ShowsPicture(RE::TESObjectREFR* a_door, RE::FormID a_linked, bool a_linkedOutdoors, bool a_gate)
	{
		return !a_gate || Portal::Doors::PictureFromAfar(Portal::Doors::CorrectionDoor(Portal::Doors::Side(a_door), { a_linked, a_linkedOutdoors }));
	}

	bool SwingsIntoFar(RE::TESObjectREFR* a_gate, RE::FormID a_farGate, const RE::NiPoint3& a_away)
	{
		return Portal::Doors::SwingingDoor({ a_gate->GetFormID(), true }, { a_farGate, true }) == a_farGate && Depth(a_gate, a_farGate, a_away) <= kApart;
	}

	bool SkipsPicture(const Active& a_portal, bool a_outdoors, bool a_gate)
	{
		// A city gate only when it shows one (StandIns): a picture nobody sees is megabytes on disk.
		if (a_gate && !Portal::Doors::PictureFromAfar(Portal::Doors::CorrectionDoor({ a_portal.sourceDoor, true }, { a_portal.linkedDoor, true }))) {
			return true;
		}
		// Nor the street from inside: shown only live (StreetShownLive).
		return a_outdoors && !a_gate;
	}

	void HoldFar(const Active& a_portal, RE::TESObjectREFR* a_linked, bool a_farSwings)
	{
		// Walking out of Whiterun the player stood on its housing, 25 units short of the doorway,
		// once the view out was there, and wasn't carried through. The far gate is drawn by the
		// portal pass: swung open when it is the one that swings, else hidden.
		Portal::Doors::Open(a_linked, openFarGate, { .hide = !a_farSwings, .passable = true });
		if (const auto mapping = Portal::Doors::TryCompute(a_portal)) {
			Manager::Space().ClearPassage(mapping->sourceCenter, mapping->inward, mapping->halfWidth, mapping->halfHeight);
		}
	}

	void ReleaseFar()
	{
		Portal::Doors::Close(openFarGate);
		Manager::Space().RestorePassage();
	}

	// Through a city gate the portal pass draws the other worldspace, and where it drew nothing
	// (the sky, beyond its few cells) the world's own shows: the two share their coordinates,
	// distant land and sky. Where the two gates stand apart, the world's own side of the
	// gatehouse lies in between, and showed instead (Solitude's: a brick wall where the sky
	// was). The gate is masked like a merged room's doorway, in room 0, which a gate's portal
	// leaves free; the world's shapes as far as the portal pass draws the other side: its cells
	// reach a whole cell past the far gate in every direction (RemoteCell::kNeighbourRadius).
	// Masked only up to the far gate, the world's outer arch around it stood in the sky as an
	// outline, the part of it beyond the far gate's plane.
	void UpdateMask(RE::TESObjectCELL* a_here)
	{
		std::optional<float> passage;
		Render::DoorRect     door;
		const auto           portal = Manager::ActivePortal();
		if (portal && ::Render::Backend::CanMaskGate() && IsGate(a_here, portal->cell)) {
			if (const auto mapping = Doors::TryCompute(*portal)) {
				const auto  farGate = mapping->toLinked * mapping->sourceCenter - mapping->sourceCenter;
				const float depth = farGate.x * mapping->inward.x + farGate.y * mapping->inward.y;
				if (depth > Core::Doorway::kMargin) {
					constexpr float kCellSize = 4096.0f;
					passage = depth + kCellSize * Portal::RemoteCell::kNeighbourRadius;
					door = Render::UprightDoor(mapping->sourceCenter, mapping->inward, mapping->halfWidth, mapping->halfHeight, false);
				}
			}
		}
		// Room 0 is a merged room's while one is.
		if (!passage && !Manager::Space().Merged() && ::Render::Backend::CanMaskGate() && !a_here->IsInteriorCell()) {
			passage = StandInGatePassage(door);
		}
		if (passage) {
			::Render::Backend::SetMergedDoor(0, door);
			::Render::Backend::SetGatePassage(*passage);
			gateMasked = true;
		} else if (std::exchange(gateMasked, false)) {
			if (!Manager::Space().Merged()) {
				::Render::Backend::SetMergedDoor(0, std::nullopt);
			}
			::Render::Backend::SetGatePassage(0.0f);
		}
	}

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
}
