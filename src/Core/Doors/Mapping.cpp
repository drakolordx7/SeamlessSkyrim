#include "Core/Doors/Mapping.h"

#include <cmath>

namespace Core::Doors
{
	namespace
	{
		Transform TurnAround(float a_radians, const Vec3& a_center)
		{
			Transform turn;
			turn.rotate = Mat3::TurnZ(a_radians);
			turn.translate = a_center - turn.rotate * a_center;
			return turn;
		}

		// A marker isn't always square to its door (one faces 48 degrees along the street), so only
		// the sign of the marker against the panel normal is used.
		float Facing(const PairDoor& a_door, float a_markerYaw)
		{
			const float yaw = std::atan2(a_door.panelNormal.x, a_door.panelNormal.y);
			return std::cos(yaw - a_markerYaw) >= 0.0f ? yaw : yaw + kPi;
		}

		Transform Frame(float a_yaw, const Vec3& a_origin)
		{
			Transform t;
			t.rotate = Mat3::FacingYaw(a_yaw);
			t.translate = a_origin;
			return t;
		}
	}

	Mapping MapPair(const PairDoor& a_source, const PairDoor& a_linked, bool a_sameModel, const Correction& a_correction)
	{
		// Walking into the source door is facing opposite to how you come out of the linked one.
		const float inYaw = Facing(a_source, a_linked.markerYaw + kPi);
		const float outYaw = Facing(a_linked, a_source.markerYaw);
		const auto  outside = Frame(inYaw, a_source.center);
		const auto  inside = Frame(outYaw, a_linked.center);

		Transform grow;
		if (a_source.outdoors && a_linked.outdoors && a_sameModel) {
			const float ratio = a_linked.halfHeight / a_source.halfHeight;
			if (std::abs(ratio - 1.0f) > 0.01f) {
				grow.scale = ratio;
			}
		}
		Mapping mapping;
		mapping.toLinked = inside * grow * outside.Invert();
		mapping.sourceCenter = a_source.center;
		mapping.inward = { std::sin(inYaw), std::cos(inYaw), 0.0f };

		// Seen from the door that holds the correction the other side's picture turns; from the
		// other side, the way back turns the opposite way.
		if (a_correction.degrees != 0.0f) {
			const float turn = DegToRad(a_correction.degrees);
			if (a_correction.keyIsSource) {
				mapping.toLinked = TurnAround(turn, a_linked.center) * mapping.toLinked;
			} else {
				mapping.toLinked = mapping.toLinked * TurnAround(-turn, a_source.center);
			}
		}
		const auto& shift = a_correction.shift;
		if (shift.x != 0.0f || shift.y != 0.0f || shift.z != 0.0f) {
			const float yaw = a_correction.keyIsSource ? inYaw : outYaw + kPi;  // walking into the key door
			const Vec3  in{ std::sin(yaw), std::cos(yaw), 0.0f };
			const Vec3  right{ in.y, -in.x, 0.0f };
			Transform   move;
			move.translate = right * shift.x + in * shift.y + Vec3{ 0.0f, 0.0f, shift.z };
			if (a_correction.keyIsSource) {
				mapping.toLinked = mapping.toLinked * move.Invert();
			} else {
				mapping.toLinked = move * mapping.toLinked;
			}
		}
		// Read off the mapping itself, not summed from the angles: a hand correction turned the
		// two apart by twice its angle.
		const auto ahead = mapping.toLinked.rotate * Vec3{ 0.0f, 1.0f, 0.0f };
		mapping.yawDelta = std::atan2(ahead.x, ahead.y);
		mapping.halfWidth = a_source.halfWidth;
		mapping.halfDepth = a_source.halfDepth;
		mapping.halfHeight = a_source.halfHeight;
		return mapping;
	}
}
