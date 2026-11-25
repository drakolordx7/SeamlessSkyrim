#pragma once

namespace Portal::Doors
{
	// The door reference, or throws if it has no 3D (not loaded).
	inline RE::TESObjectREFR* Door(RE::FormID a_id)
	{
		auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id);
		if (!door || !door->Get3D()) {
			throw std::runtime_error(std::format("door {:08X} has no 3D", a_id));
		}
		return door;
	}

	// The door at the other end of a_door's teleport, if it is loaded.
	inline RE::TESObjectREFR* LinkedOf(RE::TESObjectREFR* a_door)
	{
		auto* teleport = a_door ? a_door->extraList.GetByType<RE::ExtraTeleport>() : nullptr;
		return teleport && teleport->teleportData ? teleport->teleportData->linkedDoor.get().get() : nullptr;
	}

	// A door outdoors, also one whose exterior cell isn't loaded (a persistent street door while
	// the player is indoors, a game loaded there): it has no parent cell then, but a worldspace.
	inline bool Outdoors(RE::TESObjectREFR* a_door)
	{
		auto* cell = a_door->GetParentCell();
		return cell ? !cell->IsInteriorCell() : a_door->GetWorldspace() != nullptr;
	}

	// One door of a pair as the pair's rules see it: which door, and whether it stands outdoors. The
	// far door needn't be loaded (a stand-in's), so its kind is then told by the caller.
	struct Side
	{
		RE::FormID id;
		bool       outdoors;

		Side(RE::FormID a_id, bool a_outdoors) :
			id(a_id), outdoors(a_outdoors) {}
		Side(RE::TESObjectREFR* a_door) :
			id(a_door->GetFormID()), outdoors(Outdoors(a_door)) {}
	};
}
