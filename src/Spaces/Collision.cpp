#include "Spaces/RemoteCell.h"

#include "Diag/Probe.h"

namespace
{
	// A rotation matrix (column vectors, as NiMatrix3 and Havok both use) as Havok's quaternion
	// (x, y, z, w).
	RE::hkQuaternion ToQuaternion(const float a_m[3][3])
	{
		float      x, y, z, w;
		const float trace = a_m[0][0] + a_m[1][1] + a_m[2][2];
		if (trace > 0.0f) {
			const float s = std::sqrt(trace + 1.0f) * 2.0f;
			w = 0.25f * s;
			x = (a_m[2][1] - a_m[1][2]) / s;
			y = (a_m[0][2] - a_m[2][0]) / s;
			z = (a_m[1][0] - a_m[0][1]) / s;
		} else if (a_m[0][0] > a_m[1][1] && a_m[0][0] > a_m[2][2]) {
			const float s = std::sqrt(1.0f + a_m[0][0] - a_m[1][1] - a_m[2][2]) * 2.0f;
			w = (a_m[2][1] - a_m[1][2]) / s;
			x = 0.25f * s;
			y = (a_m[0][1] + a_m[1][0]) / s;
			z = (a_m[0][2] + a_m[2][0]) / s;
		} else if (a_m[1][1] > a_m[2][2]) {
			const float s = std::sqrt(1.0f + a_m[1][1] - a_m[0][0] - a_m[2][2]) * 2.0f;
			w = (a_m[0][2] - a_m[2][0]) / s;
			x = (a_m[0][1] + a_m[1][0]) / s;
			y = 0.25f * s;
			z = (a_m[1][2] + a_m[2][1]) / s;
		} else {
			const float s = std::sqrt(1.0f + a_m[2][2] - a_m[0][0] - a_m[1][1]) * 2.0f;
			w = (a_m[1][0] - a_m[0][1]) / s;
			x = (a_m[0][2] + a_m[2][0]) / s;
			y = (a_m[1][2] + a_m[2][1]) / s;
			z = 0.25f * s;
		}
		RE::hkQuaternion q;
		q.vec = RE::hkVector4(x, y, z, w);
		return q;
	}

	// a_copy put where a_original is, carried through a_map (one space's coordinates into the other's).
	void PlaceMapped(RE::bhkRigidBody* a_copy, RE::bhkRigidBody* a_original, const RE::NiTransform& a_map)
	{
		const float     scale = RE::bhkWorld::GetWorldScale();
		RE::hkTransform original;
		a_original->GetTransform(original);
		const RE::hkVector4* columns[3] = { &original.rotation.col0, &original.rotation.col1, &original.rotation.col2 };
		float                rotated[3][3];
		for (int column = 0; column < 3; ++column) {
			const auto& c = columns[column]->quad.m128_f32;
			for (int row = 0; row < 3; ++row) {
				rotated[row][column] = a_map.rotate.entry[row][0] * c[0] + a_map.rotate.entry[row][1] * c[1] + a_map.rotate.entry[row][2] * c[2];
			}
		}
		const auto&        t = original.translation.quad.m128_f32;
		const RE::NiPoint3 at = a_map.rotate * RE::NiPoint3{ t[0], t[1], t[2] } + a_map.translate * scale;
		RE::hkVector4      position(at.x, at.y, at.z, 0.0f);
		auto               rotation = ToQuaternion(rotated);
		a_copy->SetPositionAndRotation(position, rotation);
	}

	// A body of a skeleton's bones to stand in for its owner: the spine's, or any.
	RE::bhkRigidBody* BoneBody(RE::NiAVObject* a_root)
	{
		RE::bhkRigidBody* any = nullptr;
		RE::bhkRigidBody* spine = nullptr;
		if (!a_root) {
			return nullptr;
		}
		RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
			auto* collision = a_object->collisionObject ? netimmerse_cast<RE::bhkNiCollisionObject*>(a_object->collisionObject.get()) : nullptr;
			auto* body = collision && collision->body ? collision->body->AsBhkRigidBody() : nullptr;
			if (!body || !body->GetRigidBody()) {
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			any = any ? any : body;
			if (std::string_view(a_object->name.c_str()).find("Spine") != std::string_view::npos) {
				spine = body;
				return RE::BSVisit::BSVisitControl::kStop;
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		return spine ? spine : any;
	}
}

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

	void RemoteCell::UpdateStandIns(const RE::NiTransform& a_toSource, RE::bhkWorld* a_world)
	{
		auto* roomWorld = Cell() ? Cell()->GetbhkWorld() : nullptr;
		if (!roomWorld || !a_world) {
			return;
		}
		// The room's people stand in the room moved to its door (Merge), and their bones' bodies,
		// driven from the bones, are where they are seen: in the player's world's coordinates
		// already. The player's body is mapped into the room's.
		const RE::NiTransform          alreadyThere;
		std::unordered_set<RE::FormID> present;
		for (auto* cell : cells) {
			for (auto* actor : ActorsIn(cell)) {
				auto* body = actor->IsDead() ? nullptr : BoneBody(actor->Get3D());
				if (!body) {
					continue;
				}
				present.insert(actor->GetFormID());
				auto& standIn = actorStandIns[actor->GetFormID()];
				if (standIn.original.get() != body) {
					// New, or the actor's 3D was built anew.
					if (standIn.copy) {
						standIn.copy->RemoveFromCurrentWorld();
					}
					standIn = MakeStandIn(body, a_world, alreadyThere);
					standIn.original.reset(body);  // also when none could be made: not tried again every frame
					const auto* rigid = body->GetRigidBody();
					RE::hkTransform from, to;
					body->GetTransform(from);
					if (standIn.copy) {
						standIn.copy->GetTransform(to);
					}
					const float inv = 1.0f / RE::bhkWorld::GetWorldScale();
					logger::info("portal: stand-in for {:08X} '{}' in the world: {} (bone body motion {}, layer {}) at ({:.0f},{:.0f},{:.0f}) -> ({:.0f},{:.0f},{:.0f}), actor at ({:.0f},{:.0f},{:.0f})",
						actor->GetFormID(), actor->GetDisplayFullName(), standIn.copy ? "made" : "none", rigid ? static_cast<int>(rigid->motion.type.get()) : -1,
						rigid ? static_cast<int>(rigid->collidable.broadPhaseHandle.collisionFilterInfo.GetCollisionLayer()) : -1, from.translation.quad.m128_f32[0] * inv,
						from.translation.quad.m128_f32[1] * inv, from.translation.quad.m128_f32[2] * inv, to.translation.quad.m128_f32[0] * inv, to.translation.quad.m128_f32[1] * inv,
						to.translation.quad.m128_f32[2] * inv, actor->GetPositionX(), actor->GetPositionY(), actor->GetPositionZ());
				} else if (standIn.copy) {
					PlaceMapped(standIn.copy.get(), body, alreadyThere);
				}
			}
		}
		std::erase_if(actorStandIns, [&](auto& a_entry) {
			auto& [id, standIn] = a_entry;
			if (present.contains(id)) {
				return false;
			}
			if (standIn.copy) {
				standIn.copy->RemoveFromCurrentWorld();
			}
			return true;
		});
		auto*      body = BoneBody(RE::PlayerCharacter::GetSingleton()->Get3D(false));
		const auto toRoom = a_toSource.Invert();
		if (body && playerStandIn.original.get() == body && playerStandIn.copy) {
			PlaceMapped(playerStandIn.copy.get(), body, toRoom);
		} else {
			if (playerStandIn.copy) {
				playerStandIn.copy->RemoveFromCurrentWorld();
			}
			playerStandIn = body ? MakeStandIn(body, roomWorld, toRoom) : StandIn{};
		}
	}

	void RemoteCell::ClearStandIns()
	{
		for (auto& [id, standIn] : actorStandIns) {
			if (standIn.copy) {
				standIn.copy->RemoveFromCurrentWorld();
			}
		}
		actorStandIns.clear();
		if (playerStandIn.copy) {
			playerStandIn.copy->RemoveFromCurrentWorld();
		}
		playerStandIn = {};
	}

	void RemoteCell::MergeCollision(const RE::NiTransform& a_toSource, RE::bhkWorld* a_world, RE::FormID a_skip, const RE::NiPoint3& a_doorCenter, const RE::NiPoint3& a_inward)
	{
		SEAMLESS_ASSERT(a_world);
		UnmergeCollision();
		int         skipped = 0, inWorld = 0, behindDoor = 0;
		mergedWorld = a_world;
		const float scale = RE::bhkWorld::GetWorldScale();
		for (auto* cell : cells) {
			cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				auto* object = a_ref ? a_ref->Get3D() : nullptr;
				if (!object || a_ref->As<RE::Actor>() || a_ref->GetFormID() == a_skip || a_ref->IsDisabled()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				RE::BSVisit::TraverseScenegraphObjects(object, [&](RE::NiAVObject* a_object) {
					auto* collision = a_object->collisionObject ? netimmerse_cast<RE::bhkNiCollisionObject*>(a_object->collisionObject.get()) : nullptr;
					auto* body = collision && collision->body ? collision->body->AsBhkRigidBody() : nullptr;
					auto* rigid = body ? body->GetRigidBody() : nullptr;
					if (!rigid) {
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					// Only what stays put. Loose things are stilled while merged (see Merge) and
					// would need a body of their own that moves back with them.
					const auto motion = rigid->motion.type.get();
					if (motion != RE::hkpMotion::MotionType::kFixed && motion != RE::hkpMotion::MotionType::kKeyframed) {
						++skipped;
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					bool straddles = false;  // across the doorway, see SetDoorwayPiecesPresent
					// Behind the room's door, where the outdoors is when merged, rooms have a box with a
					// closed door painted in it (what the opened door would show). Copied, it stood
					// just outside the house's door: walking out of the room, the player stopped there.
					{
						RE::hkAabb box;
						body->GetAabbWorldspace(box);
						float              deepest = -FLT_MAX, shallowest = FLT_MAX, left = FLT_MAX, right = -FLT_MAX;
						const RE::NiPoint3 across{ a_inward.y, -a_inward.x, 0.0f };
						for (int corner = 0; corner < 8; ++corner) {
							const RE::NiPoint3 at{ ((corner & 1) ? box.max : box.min).quad.m128_f32[0] / scale, ((corner & 2) ? box.max : box.min).quad.m128_f32[1] / scale,
								((corner & 4) ? box.max : box.min).quad.m128_f32[2] / scale };
							deepest = std::max(deepest, (at - a_doorCenter).Dot(a_inward));
							shallowest = std::min(shallowest, (at - a_doorCenter).Dot(a_inward));
							left = std::min(left, (at - a_doorCenter).Dot(across));
							right = std::max(right, (at - a_doorCenter).Dot(across));
						}
						if (deepest < 1.0f) {
							++behindDoor;
							return RE::BSVisit::BSVisitControl::kContinue;
						}
						straddles = shallowest < -1.0f && left < 0.0f && right > 0.0f;
					}
					RE::NiPointer<RE::bhkRigidBody> copy(static_cast<RE::bhkRigidBody*>(body->Clone()));
					SEAMLESS_ASSERT(copy && copy->GetRigidBody());
					PlaceMapped(copy.get(), body, a_toSource);
					copy->MoveToWorld(a_world);
					inWorld += copy->GetWorld1() == a_world->GetWorld1() ? 1 : 0;
					if (straddles) {
						doorwayPieces.push_back(copy);
					}
					mergedBodies.push_back(std::move(copy));
					return RE::BSVisit::BSVisitControl::kContinue;
				});
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		logger::info("portal: {} collision bodies of the merged room copied into the world, {} of them in it, {} across its doorway ({} loose ones and {} behind its door left out)", mergedBodies.size(), inWorld, doorwayPieces.size(), skipped, behindDoor);
	}

	void RemoteCell::SetDoorwayPiecesPresent(bool a_present)
	{
		if (a_present == doorwayPiecesPresent || !mergedWorld) {
			return;
		}
		doorwayPiecesPresent = a_present;
		for (auto& body : doorwayPieces) {
			if (a_present) {
				body->MoveToWorld(mergedWorld);
			} else {
				body->RemoveFromCurrentWorld();
			}
		}
	}

	void RemoteCell::UnmergeCollision()
	{
		ClearStandIns();
		for (auto& body : mergedBodies) {
			body->RemoveFromCurrentWorld();
		}
		mergedBodies.clear();
		doorwayPieces.clear();
		doorwayPiecesPresent = true;
		mergedWorld = nullptr;
	}

	// A copy of a bone's body in a_world, which character controllers there run into like into
	// a door: only the layer, none of the skeleton's own filter groups.
	RemoteCell::StandIn RemoteCell::MakeStandIn(RE::bhkRigidBody* a_original, RE::bhkWorld* a_world, const RE::NiTransform& a_map)
	{
		RE::NiPointer<RE::bhkRigidBody> copy(static_cast<RE::bhkRigidBody*>(a_original->Clone()));
		auto*                           rigid = copy ? copy->GetRigidBody() : nullptr;
		if (!rigid) {
			return {};
		}
		const auto motion = rigid->motion.type.get();
		if (motion != RE::hkpMotion::MotionType::kKeyframed && motion != RE::hkpMotion::MotionType::kFixed) {
			return {};  // a body of its own would fall
		}
		*reinterpret_cast<std::uint32_t*>(&rigid->collidable.broadPhaseHandle.collisionFilterInfo) = static_cast<std::uint32_t>(RE::COL_LAYER::kAnimStatic);
		PlaceMapped(copy.get(), a_original, a_map);
		copy->MoveToWorld(a_world);
		return { copy, RE::NiPointer<RE::bhkRigidBody>(a_original) };
	}
}
