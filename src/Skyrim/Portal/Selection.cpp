#include "Skyrim/Portal/Selection.h"

#include "Skyrim/Doors/Policy.h"
#include "Skyrim/Engine/CoreConvert.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/Schedule.h"

namespace Portal::Selection
{
	namespace
	{
		float                           doorOpenRadius = static_cast<float>(Plugin::Settings::Of(Plugin::Settings::Id::kDoorOpenDistance).def);
		Core::Selection::HeadingTracker headingTracker;

		std::optional<DoorPair> NearestDoorPair(float a_radius)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* here = player->GetParentCell();
			if (!here) {
				return std::nullopt;
			}
			std::optional<DoorPair> best;
			float                   bestDistance = 0.0f;
			const auto              origin = player->GetPosition();
			RE::TES::GetSingleton()->ForEachReferenceInRange(player, a_radius, [&](RE::TESObjectREFR* a_ref) {
				if (const auto linked = PortalLink(a_ref, here)) {
					const float distance = origin.GetDistance(a_ref->GetPosition());
					if (!best || distance < bestDistance) {
						best = DoorPair{ a_ref->GetFormID(), linked->GetFormID() };
						bestDistance = distance;
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return best;
		}
	}

	void Install()
	{
		SetDoorOpenDistance(static_cast<float>(Plugin::Settings::Int(Plugin::Settings::Id::kDoorOpenDistance)));
	}

	float DoorOpenDistance()
	{
		return doorOpenRadius;
	}

	void SetDoorOpenDistance(float a_distance)
	{
		const auto& reach = Plugin::Settings::Of(Plugin::Settings::Id::kDoorOpenDistance);
		doorOpenRadius = std::clamp(a_distance, static_cast<float>(reach.min), static_cast<float>(reach.max));
	}

	float NearRadius()
	{
		return Core::Selection::NearRadius(doorOpenRadius);
	}

	float ReleaseRadius()
	{
		return NearRadius() + kReleaseBeyond;
	}

	void TrackHeading(const RE::NiPoint3& a_position)
	{
		headingTracker.Track(ToCore(a_position));
	}

	const Core::Vec3& Heading()
	{
		return headingTracker.Heading();
	}

	float Reach(const RE::NiPoint3& a_origin, RE::TESObjectREFR* a_door, RE::TESObjectREFR* a_linked)
	{
		return Core::Selection::Reach(a_origin.GetDistance(a_door->GetPosition()), Portal::Doors::Outdoors(a_door) && Portal::Doors::Outdoors(a_linked));
	}

	float Rank(const RE::NiPoint3& a_origin, RE::TESObjectREFR* a_door, RE::TESObjectREFR* a_linked)
	{
		return Core::Selection::Rank(Reach(a_origin, a_door, a_linked), headingTracker.Heading(), ToCore(a_door->GetPosition() - a_origin));
	}

	// Portal doors are load doors into another space: outside -> interior, interior -> outside or
	// another interior, and from one worldspace into another (a walled city's gate). Exterior doors
	// within one worldspace aren't: both ends are in the scene already.
	RE::NiPointer<RE::TESObjectREFR> PortalLink(RE::TESObjectREFR* a_ref, RE::TESObjectCELL* a_here)
	{
		auto* base = a_ref->GetBaseObject();
		if (!base || !base->Is(RE::FormType::Door) || a_ref->IsDisabled() || !a_ref->Get3D() || Doors::LockedShut(a_ref) || Schedule::IsFallback(a_ref->GetFormID())) {
			return nullptr;
		}
		// A marker door is no doorway: unseen, used by scripts (Riften's prison marker, which takes
		// an arrested player to the jail). Its portal stood in the open street, next to the keep.
		if (static_cast<RE::TESObject*>(base)->IsMarker()) {
			return nullptr;
		}
		auto* teleport = a_ref->extraList.GetByType<RE::ExtraTeleport>();
		auto  linked = teleport && teleport->teleportData ? teleport->teleportData->linkedDoor.get() : nullptr;
		if (!linked) {
			return nullptr;
		}
		// A persistent exterior door whose cell isn't loaded (a game loaded indoors) has no
		// parent cell yet; it stands outdoors in its worldspace.
		auto*       cell = linked->GetParentCell();
		auto* const linkedWorld = cell ? (cell->IsInteriorCell() ? nullptr : cell->GetRuntimeData().worldSpace) : linked->GetWorldspace();
		const bool  sameWorld = linkedWorld && !a_here->IsInteriorCell() && linkedWorld == a_here->GetRuntimeData().worldSpace;
		if ((!cell && !linkedWorld) || cell == a_here || sameWorld) {
			return nullptr;
		}
		return linked;
	}

	std::optional<DoorPair> DoorPairAt(float a_radius)
	{
		const auto portal = Manager::CurrentPortal();
		const auto nearby = NearestDoorPair(a_radius);
		if (!portal) {
			return nearby;
		}
		auto*      door = RE::TESForm::LookupByID<RE::TESObjectREFR>(portal->sourceDoor);
		auto*      nearest = nearby ? RE::TESForm::LookupByID<RE::TESObjectREFR>(nearby->sourceDoor) : nullptr;
		const auto origin = RE::PlayerCharacter::GetSingleton()->GetPosition();
		if (nearest && (!door || origin.GetDistance(nearest->GetPosition()) < origin.GetDistance(door->GetPosition()))) {
			return nearby;
		}
		return DoorPair{ portal->sourceDoor, portal->linkedDoor };
	}
}
