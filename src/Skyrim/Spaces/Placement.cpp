#include "Skyrim/Spaces/Placement.h"
#include "Skyrim/Spaces/RemoteCell.h"

#include "Skyrim/Portal/Pristine.h"

namespace
{
	// The smallest sphere around both (NiBound::Merge).
	void MergeBound(RE::NiBound& a_into, const RE::NiBound& a_other)
	{
		if (a_other.radius <= 0.0f) {
			return;
		}
		if (a_into.radius <= 0.0f) {
			a_into = a_other;
			return;
		}
		const auto  offset = a_other.center - a_into.center;
		const float distance = offset.Length();
		if (distance + a_other.radius <= a_into.radius) {
			return;
		}
		if (distance + a_into.radius <= a_other.radius) {
			a_into = a_other;
			return;
		}
		const float radius = (distance + a_into.radius + a_other.radius) * 0.5f;
		a_into.center += offset * ((radius - a_into.radius) / distance);
		a_into.radius = radius;
	}
}

namespace Portal
{
	// Moving a whole cell (merging it, or putting it back) changes every object's world transform
	// in one step, and static objects are never updated again: their "previous" transform, which
	// the shaders turn into motion vectors, stays where the cell came from, tens of thousands of
	// units away. The upscaler then has no history for the room (grainy), and frame generation
	// draws warped, layered copies of it. The previous transform is set to the current one.
	void SettleMotion(RE::NiAVObject* a_root)
	{
		int stale = 0;
		RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
			stale += a_object->previousWorld.translate.GetDistance(a_object->world.translate) > 10.0f ? 1 : 0;
			a_object->previousWorld = a_object->world;
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		logger::info("portal: {} object(s) had a previous transform from before the move", stale);
	}

	void RemoteCell::Remap(const Placement& a_placement)
	{
		SEAMLESS_ASSERT(merged);
		placed = a_placement;
		const auto& toSource = a_placement.toSource;
		RestoreBounds();
		for (auto* cell : movedCells) {
			if (auto* cell3D = Cell3D(cell)) {
				movedBounds.Capture(cell3D);
				movedBounds.UnfixBeforeMove(cell3D);
				cell3D->local = toSource;
				RE::NiUpdateData update;
				cell3D->Update(update);
				SettleMotion(cell3D);
			}
		}
		MoveBounds(toSource);
	}

	// By hand: NiNode's own UpdateWorldBound does nothing (the scene update merges the children's
	// bounds as it goes, and leaves fixed ones alone).
	void RecomputeBounds(RE::NiAVObject* a_root)
	{
		if (!a_root) {
			return;
		}
		if (auto* geometry = a_root->AsGeometry()) {
			const auto& model = geometry->GetModelData().modelBound;
			a_root->worldBound.center = a_root->world * model.center;
			a_root->worldBound.radius = model.radius * a_root->world.scale;
			return;
		}
		auto* node = a_root->AsNode();
		if (!node) {
			return;
		}
		RE::NiBound bound{};
		bound.center = a_root->world.translate;
		bound.radius = 0.0f;
		// A switch node (a coin bag, a mushroom: picked or not) updates only the child shown, and the
		// other keeps the world transform it never got, at the origin; the switch node's bound still
		// takes it in. Moved to a door, every such bound, and every room above one, spanned the map
		// from there to the origin, and the engine's walk of the scene for each light it adds (on
		// crossing, ~150 of them in Understone Keep) could skip none of it.
		auto* switchNode = netimmerse_cast<RE::NiSwitchNode*>(a_root);
		auto  index = 0;
		for (auto& child : node->GetChildren()) {
			if (child && switchNode && index != switchNode->index) {
				RE::NiUpdateData update;
				child->Update(update);
			}
			++index;
			if (child) {
				RecomputeBounds(child.get());
				MergeBound(bound, child->worldBound);
			}
		}
		a_root->worldBound = bound;
	}

	void MovedBounds::Capture(RE::NiAVObject* a_root)
	{
		RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
			auto* node = a_object->AsMultiBoundNode();
			auto* bound = node ? node->GetRuntimeData().multiBound.get() : nullptr;
			auto* shape = bound ? bound->data.get() : nullptr;
			const auto type = shape ? shape->GetType() : 0;
			if (type == 1 || type == 2) {  // 1 axis-aligned box, 2 oriented box
				auto* box = static_cast<RE::BSMultiBoundAABB*>(shape);
				Saved saved{ RE::NiPointer<RE::BSMultiBoundShape>(shape), box->center, box->size, {} };
				if (type == 2) {
					saved.rotation = static_cast<RE::BSMultiBoundOBB*>(shape)->rotation;
				}
				moved.push_back(std::move(saved));
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
	}

	void MovedBounds::UnfixBeforeMove(RE::NiAVObject* a_root)
	{
		RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
			if (a_object->GetFlags().any(RE::NiAVObject::Flag::kFixedBound)) {
				a_object->GetFlags().reset(RE::NiAVObject::Flag::kFixedBound);
				fixed.emplace_back(a_object);
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
	}

	void MovedBounds::Move(RE::NiAVObject* a_root, const RE::NiTransform& a_move)
	{
		RecomputeBounds(a_root);
		// The scene update puts a cell's box around its contents itself. Captured after an update
		// and moved again, cells' boxes were saved already in the interior's coordinates and put
		// back like that: the street's ground in front of the Bannered Mare was culled from some
		// angles. Boxes it leaves where they were (an interior's rooms) are moved by hand.
		const auto& r = a_move.rotate;
		for (std::size_t i = unmoved; i < moved.size(); ++i) {
			const auto& saved = moved[i];
			auto*       box = static_cast<RE::BSMultiBoundAABB*>(saved.shape.get());
			if (box->center.GetDistance(saved.center) > 0.5f) {
				continue;
			}
			box->center = a_move * saved.center;
			// Scaled too: a street merged into an interior is drawn larger (PlacementOf).
			const auto sz = saved.size * a_move.scale;
			if (saved.shape->GetType() == 2) {
				static_cast<RE::BSMultiBoundOBB*>(saved.shape.get())->rotation = r * saved.rotation;
				box->size = sz;
			} else {
				// An axis-aligned box can't turn: the one around the turned box.
				for (int k = 0; k < 3; ++k) {
					box->size[k] = std::abs(r.entry[k][0]) * sz.x + std::abs(r.entry[k][1]) * sz.y + std::abs(r.entry[k][2]) * sz.z;
				}
			}
		}
		unmoved = moved.size();
	}

	void MovedBounds::Unfix(RE::NiAVObject* a_root)
	{
		const auto before = fixed.size();
		UnfixBeforeMove(a_root);
		if (fixed.size() != before) {
			RecomputeBounds(a_root);
		}
	}

	void MovedBounds::Restore()
	{
		for (auto& object : fixed) {
			object->GetFlags().set(RE::NiAVObject::Flag::kFixedBound);
		}
		fixed.clear();
		for (auto& saved : moved) {
			auto* box = static_cast<RE::BSMultiBoundAABB*>(saved.shape.get());
			box->center = saved.center;
			box->size = saved.size;
			if (saved.shape->GetType() == 2) {
				static_cast<RE::BSMultiBoundOBB*>(saved.shape.get())->rotation = saved.rotation;
			}
		}
		moved.clear();
		unmoved = 0;
	}

	void RemoteCell::MoveBounds(const RE::NiTransform& a_toSource)
	{
		for (auto* cell : movedCells) {
			if (auto* cell3D = Cell3D(cell)) {
				movedBounds.Move(cell3D, a_toSource);
			}
		}
		if (movedBounds.Multibounds()) {
			logger::info("portal: {} multibound(s) moved with the merged cells", movedBounds.Multibounds());
		}
		if (movedBounds.Fixed()) {
			logger::info("portal: {} fixed bound(s) of the merged cells recomputed", movedBounds.Fixed());
		}
	}

	void RemoteCell::UnfixNewBounds(const RE::TESObjectCELL* a_cell)
	{
		if (!merged) {
			return;
		}
		if (auto* cell3D = Cell3D(a_cell)) {
			movedBounds.Unfix(cell3D);
		}
	}

	void RemoteCell::RestoreBounds()
	{
		movedBounds.Restore();
	}

	bool RemoteCell::StillCollision(RE::NiAVObject* a_object)
	{
		auto* collision = a_object->collisionObject ? netimmerse_cast<RE::bhkNiCollisionObject*>(a_object->collisionObject.get()) : nullptr;
		if (!collision || !collision->flags.any(RE::bhkNiCollisionObject::Flag::kActive)) {
			return false;
		}
		stilledClutter.emplace_back(RE::NiPointer<RE::NiAVObject>(a_object), collision->flags.underlying());
		Pristine::FlagsChanged(collision, collision->flags.underlying());
		collision->flags.reset(RE::bhkNiCollisionObject::Flag::kActive, RE::bhkNiCollisionObject::Flag::kSyncOnUpdate);
		return true;
	}

	json RemoteCell::Merge(const Placement& a_placement)
	{
		placed = a_placement;
		const auto& toSource = a_placement.toSource;
		auto* here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		auto* here3D = Cell3D(here);
		auto* worldParent = here3D ? here3D->parent : nullptr;
		if (!worldParent || cells.empty()) {
			throw std::runtime_error("nothing to merge");
		}
		auto* root = scene.get();
		int   moved = 0;
		RestoreBounds();
		movedCells.clear();
		for (auto* cell : cells) {
			RE::NiPointer<RE::NiAVObject> cell3D(Cell3D(cell));
			if (cell3D && cell3D->parent == root) {
				movedCells.push_back(cell);
				movedBounds.Capture(cell3D.get());
				movedBounds.UnfixBeforeMove(cell3D.get());
				root->DetachChild(cell3D.get());
				worldParent->AttachChild(cell3D.get(), true);
				cell3D->local = toSource;
				// Loose objects (a mace on a shelf, cheese, plates) take their place from their
				// physics body, in the cell's own coordinates: they stayed behind, tens of thousands
				// of units away, and the shelves of the merged room were empty. While merged their
				// collision objects don't drive the 3D, which then follows the cell like the rest.
				RE::BSVisit::TraverseScenegraphObjects(cell3D.get(), [&](RE::NiAVObject* a_object) {
					if (!a_object->collisionObject) {
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					// Skeletons keep their ragdoll and controller ties.
					bool ofActor = false;
					for (auto* up = a_object; up && up != cell3D.get() && !ofActor; up = up->parent) {
						auto* owner = up->GetUserData();
						ofActor = owner && owner->Is(RE::FormType::ActorCharacter);
					}
					if (!ofActor) {
						StillCollision(a_object);
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
				RE::NiUpdateData update;
				cell3D->Update(update);
				SettleMotion(cell3D.get());
				++moved;
			}
		}
		MoveBounds(toSource);
		// The room's lights stay in the portal's light scene: the patched Community Shaders lights the
		// merged shapes from there (SetupPortalStrictLights). Handed to the world's, they lit the
		// street around the door, through walls, took shadow slots from the town's own lights (its
		// shadows changed as doors were passed) and stayed behind after the room was let go.
		merged = true;
		return { { "cells", moved }, { "parent", worldParent->name.c_str() } };
	}
}
