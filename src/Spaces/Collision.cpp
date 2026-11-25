#include "Spaces/RemoteCell.h"

#include "Diag/Probe.h"

namespace Portal
{
	void RemoteCell::ClearPassage(const RE::NiPoint3& a_center, const RE::NiPoint3& a_inward, float a_halfWidth, float a_halfHeight)
	{
		// Looked for twice a second for as long as the passage is held: what loads later is taken
		// out too. Looked for every frame until something was found and never again after, a gate
		// with nothing in its passage walked every reference of the street each frame, and one
		// whose buildings came in after the first was left with them in the way.
		const auto now = std::chrono::steady_clock::now();
		if (cells.empty() || now - passageLooked < std::chrono::milliseconds(500)) {
			return;
		}
		passageLooked = now;
		passageWorld = cells.front()->GetbhkWorld();
		if (!passageWorld) {
			return;
		}
		const auto clearedBefore = passageCleared.size();
		// The passage: from well in front of the doorway (the player's side) to a little past it,
		// as wide and high as the doorway with some room.
		constexpr float kFront = 200.0f, kBehind = 30.0f, kMargin = 60.0f;
		const RE::NiPoint3 side{ a_inward.y, -a_inward.x, 0.0f };
		const auto         touches = [&](const RE::NiBound& a_bound) {
			const auto  offset = a_bound.center - a_center;
			const float depth = offset.x * a_inward.x + offset.y * a_inward.y;
			const float across = std::abs(offset.x * side.x + offset.y * side.y);
			const float up = std::abs(offset.z);
			const float r = a_bound.radius;
			return depth + r > -kFront && depth - r < kBehind && across - r < a_halfWidth + kMargin && up - r < a_halfHeight + kMargin;
		};
		const auto clear = [&](RE::NiAVObject* a_root) {
			RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
				auto* collision = a_object->collisionObject ? a_object->collisionObject->AsBhkNiCollisionObject() : nullptr;
				auto* body = collision ? collision->body.get() : nullptr;
				// Only what stands on the player's side. A piece reaching into the passage from the far
				// side is what the far side's people stand on: Whiterun's road and gate wall, taken out
				// for their bounds touching the passage, let the guards behind the gate fall through.
				const auto  offset = a_object->worldBound.center - a_center;
				const float depth = offset.x * a_inward.x + offset.y * a_inward.y;
				if (body && body->GetWorld1() && depth < 0.0f && touches(a_object->worldBound)) {
					passageCleared.emplace_back(body);
					body->RemoveFromCurrentWorld();
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		};
		for (auto* cell : cells) {
			cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				auto* object = a_ref ? a_ref->Get3D() : nullptr;
				// Not the people (they stand on it), nor doors (Doors::Open), nor the land.
				if (object && !a_ref->As<RE::Actor>() && !(a_ref->GetBaseObject() && a_ref->GetBaseObject()->Is(RE::FormType::Door)) && touches(object->worldBound)) {
					clear(object);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		for (auto& top : borrowedPersistent) {
			if (top && touches(top->worldBound)) {
				clear(top.get());
			}
		}
		if (passageCleared.size() != clearedBefore) {
			logger::info("portal: {} body(ies) of {} kept out of the gate's passage", passageCleared.size(), Probe::DescribeCell(cells.front()));
		}
	}

	void RemoteCell::RestorePassage()
	{
		for (auto& body : passageCleared) {
			if (passageWorld && !body->GetWorld1()) {
				body->MoveToWorld(passageWorld);
			}
		}
		passageCleared.clear();
		passageWorld = nullptr;
		passageLooked = {};
	}
}
