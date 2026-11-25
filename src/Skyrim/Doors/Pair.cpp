#include "Skyrim/Doors/Pair.h"

#include "Core/Doors/Mapping.h"
#include "Skyrim/Doors/Geometry.h"
#include "Skyrim/Doors/Settings.h"
#include "Skyrim/Engine/CoreConvert.h"

namespace Portal::Doors
{
	namespace
	{
		// The source door's teleport marker is where you appear inside and which way you face; the
		// linked door's marker is where you appear outside, facing away from the door.
		float MarkerYaw(RE::TESObjectREFR* a_door)
		{
			auto* teleport = a_door->extraList.GetByType<RE::ExtraTeleport>();
			if (!teleport || !teleport->teleportData) {
				throw std::runtime_error(std::format("door {:08X} has no teleport marker", a_door->GetFormID()));
			}
			return teleport->teleportData->rotation.z;
		}

		Core::Doors::PairDoor Measure(RE::TESObjectREFR* a_door)
		{
			const auto size = Size(a_door);
			return { ToCore(Center(a_door)), ToCore(PanelNormal(a_door)), MarkerYaw(a_door), size.halfWidth, size.halfDepth, size.halfHeight, Outdoors(a_door) };
		}
	}

	Mapping Compute(RE::TESObjectREFR* a_source, RE::TESObjectREFR* a_linked)
	{
		// The door meshes' own rotations can't give the facing: the meshes aren't placed consistently.
		// Only one mesh (by its model: the two sides' base doors can be two forms) is a gate placed
		// at two sizes; two different gates' heights say nothing about their scale.
		const auto modelOf = [](RE::TESObjectREFR* a_door) { return a_door->GetBaseObject()->As<RE::TESModel>()->GetModel(); };
		const bool sameModel = _stricmp(modelOf(a_source), modelOf(a_linked)) == 0;

		Core::Doors::Correction correction;
		auto*                   key = CorrectionDoor(a_source, a_linked);
		correction.degrees = Correction(key->GetFormID());
		correction.shift = ToCore(Shift(key->GetFormID()));
		correction.keyIsSource = key == a_source;

		const auto mapped = Core::Doors::MapPair(Measure(a_source), Measure(a_linked), sameModel, correction);
		Mapping    mapping;
		mapping.toLinked = ToNi(mapped.toLinked);
		mapping.sourceCenter = ToNi(mapped.sourceCenter);
		mapping.inward = ToNi(mapped.inward);
		mapping.yawDelta = mapped.yawDelta;
		mapping.halfWidth = mapped.halfWidth;
		mapping.halfDepth = mapped.halfDepth;
		mapping.halfHeight = mapped.halfHeight;
		return mapping;
	}

	std::optional<Mapping> TryCompute(const Active& a_portal)
	{
		auto* source = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_portal.sourceDoor);
		auto* linked = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_portal.linkedDoor);
		if (!source || !source->Get3D() || !linked || !linked->Get3D()) {
			return std::nullopt;
		}
		return Compute(source, linked);
	}
}
