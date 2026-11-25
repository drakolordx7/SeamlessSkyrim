#include "Doors/Pair.h"

#include "Doors/Geometry.h"
#include "Doors/Settings.h"

namespace Portal::Doors
{
	namespace
	{
		RE::NiTransform TurnAround(float a_radians, const RE::NiPoint3& a_center)
		{
			const float     c = std::cos(a_radians), s = std::sin(a_radians);
			RE::NiTransform turn;
			turn.rotate = RE::NiMatrix3({ c, -s, 0.0f }, { s, c, 0.0f }, { 0.0f, 0.0f, 1.0f });
			turn.translate = a_center - turn.rotate * a_center;
			return turn;
		}
	}

	Mapping Compute(RE::TESObjectREFR* a_source, RE::TESObjectREFR* a_linked)
	{
		// The source door's teleport marker is where you appear inside and which way you face; the
		// linked door's marker is where you appear outside, facing away from the door.
		const auto yawOf = [](RE::TESObjectREFR* a_door) {
			auto* teleport = a_door->extraList.GetByType<RE::ExtraTeleport>();
			if (!teleport || !teleport->teleportData) {
				throw std::runtime_error(std::format("door {:08X} has no teleport marker", a_door->GetFormID()));
			}
			return teleport->teleportData->rotation.z;
		};
		// A doorway is mapped square onto the other: each door's facing is its panel's normal, the
		// marker only picks which of its two sides. Markers aren't always square to their door: the one
		// inside Solitude's gate faces 48 degrees up the street, and the outside was shown that much
		// turned; the Smelter Overseer's House's street door 17, Whiterun's shops 4 to 11, turned back
		// by hand before. (The door meshes' own rotations can't be used for this: the meshes aren't
		// placed consistently.)
		const auto facing = [](RE::TESObjectREFR* a_door, float a_markerYaw) {
			const auto  normal = PanelNormal(a_door);
			const float yaw = std::atan2(normal.x, normal.y);
			return std::cos(yaw - a_markerYaw) >= 0.0f ? yaw : yaw + RE::NI_PI;
		};
		// A frame whose local +y is the heading `a_yaw` (Skyrim: forward = (sin, cos)), +z up.
		const auto frame = [](float a_yaw, const RE::NiPoint3& a_origin) {
			const float     s = std::sin(a_yaw), c = std::cos(a_yaw);
			RE::NiTransform t;
			t.rotate = RE::NiMatrix3({ c, s, 0.0f }, { -s, c, 0.0f }, { 0.0f, 0.0f, 1.0f });
			t.translate = a_origin;
			return t;
		};
		auto* const source = a_source;
		auto* const linked = a_linked;
		// Outside: walking into the source door = facing opposite to how you come out of it.
		const float inYaw = facing(source, yawOf(linked) + RE::NI_PI);
		const float outYaw = facing(linked, yawOf(source));
		const auto  outside = frame(inYaw, Center(source));
		const auto  inside = frame(outYaw, Center(linked));

		// A gate's two doors can be one mesh placed at different sizes: Whiterun's is 10% larger on
		// the city side. Seen through either, the other side's gateway didn't fill the one it was seen
		// in (its arch lower, its threshold higher). Mapped scaled by their ratio around the doorway,
		// the two gateways are one: the camera behind stands that much nearer (or further from) the
		// far gate, which looks the same as that side drawn larger. A street seen from a room is
		// scaled where it is merged instead (PlacementOf). Only one mesh (by its model: the two sides'
		// base doors can be two forms): two different gates'
		// heights say nothing about their scale, and a scaled mapping also moves where the player
		// and the people walking through come out. Within 1% it is the measurement's noise.
		RE::NiTransform grow;
		const auto modelOf = [](RE::TESObjectREFR* a_door) { return a_door->GetBaseObject()->As<RE::TESModel>()->GetModel(); };
		if (Outdoors(source) && Outdoors(linked) && _stricmp(modelOf(source), modelOf(linked)) == 0) {
			const float ratio = Size(linked).halfHeight / Size(source).halfHeight;
			if (std::abs(ratio - 1.0f) > 0.01f) {
				grow.scale = ratio;
			}
		}
		Mapping mapping;
		mapping.toLinked = inside * grow * outside.Invert();
		mapping.sourceCenter = Center(source);
		mapping.inward = { std::sin(inYaw), std::cos(inYaw), 0.0f };
		// The hand correction turns the inside around its own door: seen from the door that holds
		// it, the other side's picture turns; from the other side, the way back turns the opposite way.
		auto* key = CorrectionDoor(source, linked);
		if (const float degrees = Correction(key->GetFormID()); degrees != 0.0f) {
			const float turn = RE::deg_to_rad(degrees);
			if (key == source) {
				mapping.toLinked = TurnAround(turn, Center(linked)) * mapping.toLinked;
			} else {
				mapping.toLinked = mapping.toLinked * TurnAround(-turn, Center(source));
			}
		}
		// The shift moves the inside's picture in the key door's space (from there, toLinked reads
		// a point d further back; the way back adds d again).
		if (const auto shift = Shift(key->GetFormID()); shift.x != 0.0f || shift.y != 0.0f || shift.z != 0.0f) {
			const float  yaw = key == source ? inYaw : outYaw + RE::NI_PI;  // walking into the key door
			const RE::NiPoint3 in{ std::sin(yaw), std::cos(yaw), 0.0f };
			const RE::NiPoint3 right{ in.y, -in.x, 0.0f };
			RE::NiTransform move;
			move.translate = right * shift.x + in * shift.y + RE::NiPoint3{ 0.0f, 0.0f, shift.z };
			if (key == source) {
				mapping.toLinked = mapping.toLinked * move.Invert();
			} else {
				mapping.toLinked = move * mapping.toLinked;
			}
		}
		// Read off the mapping itself (where heading 0 ends up), so a walker arrives facing the way
		// the doorway showed: kept as a sum next to it, a hand correction turned the two apart by
		// twice its angle.
		const auto ahead = mapping.toLinked.rotate * RE::NiPoint3{ 0.0f, 1.0f, 0.0f };
		mapping.yawDelta = std::atan2(ahead.x, ahead.y);
		const auto size = Size(source);
		mapping.halfWidth = size.halfWidth;
		mapping.halfDepth = size.halfDepth;
		mapping.halfHeight = size.halfHeight;
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
