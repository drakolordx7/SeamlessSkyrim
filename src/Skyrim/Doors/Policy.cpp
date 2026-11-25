#include "Skyrim/Doors/Policy.h"

#include "Core/Doors/Policy.h"

#include "Skyrim/Doors/DoorUse.h"
#include "Skyrim/Doors/Host.h"
#include "Skyrim/Doors/Settings.h"

namespace
{
	Portal::Doors::Host host;
}

namespace Portal::Doors
{
	void SetHost(const Host& a_host)
	{
		host = a_host;
	}

	const Host& GetHost()
	{
		return host;
	}

	namespace
	{
		Core::Doors::PairFacts FactsOf(RE::TESObjectREFR* a_door, const Side& a_linked)
		{
			const Side self(a_door);
			return {
				.force = DoorUse::GetForce(),
				.doorOutdoors = self.outdoors,
				.linkedOutdoors = a_linked.outdoors,
				.alwaysOpen = AlwaysOpen(CorrectionDoor(self, a_linked)),
				.doorUsed = DoorUse::Used(self.id),
				.linkedUsed = DoorUse::Used(a_linked.id),
			};
		}
	}

	bool StreetShownLive(RE::TESObjectREFR* a_door, bool a_linkedOutdoors)
	{
		return Core::Doors::StreetShownLive(Outdoors(a_door), a_linkedOutdoors);
	}

	bool OpenFromAfar(RE::TESObjectREFR* a_door, const Side& a_linked)
	{
		return Core::Doors::OpenFromAfar(FactsOf(a_door, a_linked), [&] { return host.hasPicture(a_door->GetFormID(), a_linked.outdoors); });
	}

	bool StandsOpen(RE::TESObjectREFR* a_door, const Side& a_linked)
	{
		return Core::Doors::StandsOpen(
			FactsOf(a_door, a_linked), [&] { return host.hasPicture(a_door->GetFormID(), a_linked.outdoors); }, [&] { return host.spaceShown(a_door->GetFormID()); });
	}

	bool Wanted(RE::TESObjectREFR* a_door, const Side& a_linked)
	{
		return Core::Doors::Wanted(
			FactsOf(a_door, a_linked), [&] { return host.hasPicture(a_door->GetFormID(), a_linked.outdoors); }, [&] { return host.spaceShown(a_door->GetFormID()); });
	}

	RE::TESObjectREFR* UsedPairOf(RE::TESObjectREFR* a_door)
	{
		auto* here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		auto  linked = here ? host.portalLink(a_door, here) : nullptr;
		if (!linked || PortalOff(a_door->GetFormID()) || PortalOff(linked->GetFormID())) {
			return nullptr;
		}
		return linked.get();
	}

	bool ClosableDoor(RE::TESObjectREFR* a_door)
	{
		auto* linked = LinkedOf(a_door);
		return linked && DoorUse::Used(a_door->GetFormID()) && !StandsOpen(a_door, linked);
	}
}
