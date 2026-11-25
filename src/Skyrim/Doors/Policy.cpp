#include "Skyrim/Doors/Policy.h"

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

	bool StreetShownLive(RE::TESObjectREFR* a_door, bool a_linkedOutdoors)
	{
		return !Outdoors(a_door) && a_linkedOutdoors;
	}

	bool OpenFromAfar(RE::TESObjectREFR* a_door, const Side& a_linked)
	{
		using DoorUse::Force;
		switch (DoorUse::GetForce()) {
		case Force::kAllOpen:
			return true;
		case Force::kAllShut:
			return false;
		case Force::kNone:
			break;
		}
		const Side self(a_door);
		const auto key = CorrectionDoor(self, a_linked);
		return (self.outdoors && a_linked.outdoors) ||
		       (!StreetShownLive(a_door, a_linked.outdoors) && AlwaysOpen(key) && host.hasPicture(self.id, a_linked.outdoors));
	}

	bool StandsOpen(RE::TESObjectREFR* a_door, const Side& a_linked)
	{
		if (OpenFromAfar(a_door, a_linked)) {
			return true;
		}
		const auto key = CorrectionDoor(Side(a_door), a_linked);
		return DoorUse::GetForce() == DoorUse::Force::kNone && AlwaysOpen(key) && host.spaceShown(a_door->GetFormID());
	}

	bool Wanted(RE::TESObjectREFR* a_door, const Side& a_linked)
	{
		return StandsOpen(a_door, a_linked) || DoorUse::Used(a_door->GetFormID()) || DoorUse::Used(a_linked.id);
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
