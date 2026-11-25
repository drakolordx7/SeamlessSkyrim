#include "Skyrim/Portal/MergedView.h"

#include "Core/Crossing/Approach.h"
#include "Core/Render/DoorwayPlanes.h"

#include "Skyrim/Diag/Telemetry.h"
#include "Skyrim/Doors/DoorUse.h"
#include "Skyrim/Doors/Geometry.h"
#include "Skyrim/Doors/Hold.h"
#include "Skyrim/Doors/Policy.h"
#include "Skyrim/Doors/Settings.h"
#include "Skyrim/Doors/Swing.h"
#include "Skyrim/Engine/Fade.h"
#include "Skyrim/Pictures/Cache.h"
#include "Skyrim/Plugin/Settings.h"
#include "Skyrim/Portal/Crossing.h"
#include "Skyrim/Portal/ImageSpaceBlend.h"
#include "Skyrim/Portal/LightOcclusion.h"
#include "Skyrim/Portal/LightScene.h"
#include "Skyrim/Portal/NpcTravel.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/Pristine.h"
#include "Skyrim/Portal/ShapeSet.h"
#include "Skyrim/Portal/StandIns.h"
#include "Skyrim/Portal/StreetMerge.h"
#include "Skyrim/Render/Frame.h"
#include "Skyrim/Render/Native/Native.h"
#include "Skyrim/Render/SceneMeter.h"
#include "Skyrim/Render/SceneQuery.h"
#include "Skyrim/Render/SpaceLighting.h"

namespace Portal::MergedView
{
	namespace
	{
		bool mergeEnabled = true;
		// Seen from indoors, the street merged into the interior (docs/APPROACHES.md, 9).
		// [Portals] bMergeOutdoors.
		bool mergeOutdoors = true;
		// [Portals] bPauseUnseenRooms: a kept room (loaded out of sight) is paused: not stepped, lights
		// not updated, not drawn, its people not on screen. Facing away, each kept room cost about 1 ms a frame.
		bool pauseUnseen = true;
		// The shapes of a merged space, listed anew now and then (NPCs change clothes, things get
		// dropped), a few roots per frame: done whole in one frame it was ~3 ms twice a second with
		// a street merged in, felt as judder. Each root's bounds the engine fixed since the merge are
		// unfixed on the way (see MovedBounds::Unfix).
		struct MarkPass
		{
			struct Root
			{
				RE::NiPointer<RE::NiAVObject> node;
				const RE::TESObjectCELL*      cell = nullptr;  // a merged cell's 3D; null: the distant land's
			};
			std::vector<Root>                          roots;
			std::size_t                                next = 0;
			Portal::ShapeSet                           shapes;
			std::vector<RE::NiPointer<RE::BSGeometry>> strays;
			RE::NiPoint3                               doorway;  // stray bones are measured from it
		};

		// What a room merged into the world needs while it is drawn there, besides its cells
		// (RemoteCell): its doors, its listed shapes, its light, its people's 3D.
		struct MergedRoom
		{
			bool                    marked = false;  // listed at least once in this merge (MarkMergedShapes)
			float                   markTimer = 0.0f;
			std::optional<MarkPass> markPass;
			Portal::ShapeSet        shapes;  // main thread only (see Published)
			// Skinned shapes hidden for their stray bones (see HasStrayBones).
			std::vector<RE::NiPointer<RE::BSGeometry>> strays;
			// Shader properties whose "casts shadows" was taken off while merged (FinishMarkPass).
			std::vector<RE::NiPointer<RE::BSShaderProperty>> unshadowed;
			float                                lightingTimer = 0.0f;
			std::optional<Render::SpaceLight>    lighting;
			RE::FormID                           door = 0;         // the door in the player's space it is merged behind
			float                                away = 0.0f;  // how far it is drawn as an eye used to the street sees it (FarWeight)
			// Actor 3D moved out of its cell's actor node while merged, with the node to put it back in.
			std::vector<std::pair<RE::NiPointer<RE::NiAVObject>, RE::NiPointer<RE::NiNode>>> actors;
			float                                seconds = 0.0f;  // since the merge began
			Portal::Doors::OpenState             sourceDoor;
			Portal::Doors::OpenState             linkedDoor;
			Portal::Doors::Swing                 swing;  // of the swung door (swungDoor)
			RE::FormID                           swungDoor = 0;  // which of the pair that is (Doors::SwingingDoor)
			std::vector<Portal::Doors::OpenState> extraDoors;
			bool                                 extraScanned = false;
		};
		// The shown room's: always there (fresh while nothing is merged).
		MergedRoom shownRoom;

		// What the renderer reads, off the manager's update. The room (IsMergedShape) is set again
		// (Publish) whenever it is merged or let go. These, the room's shapes and the player's shapes
		// are written in the manager's update and read by the renderer, both on the main thread, so
		// without a lock: IsMergedShape is asked several times for every draw. The shown cells are read by the actor cull (ID 40335,
		// ActorCullHook), which also runs off the main thread.
		struct Published
		{
			const MergedRoom* room = nullptr;
			// The street is merged into an interior: the water drawn is the street's
			// (IsMergedShape).
			bool                                      streetMerged = false;
			std::unordered_set<const RE::BSGeometry*> playerShapes;
			std::mutex                                shownCellsLock;
			std::vector<const RE::TESObjectCELL*>     shownCells;  // under shownCellsLock
		} published;

		// Someone who walked out of the merged room: their 3D must not stay under its (moved)
		// cell, or it is drawn tens of thousands of units away until the engine happens to
		// rebuild it; out on the street they were invisible for a second. It goes into the
		// actors' node of the cell they are in now.
		void ReturnLeavers(MergedRoom& a_room, const Spaces::RemoteCell& a_space)
		{
			std::erase_if(a_room.actors, [&](auto& a_entry) {
				auto& [object, holder] = a_entry;
				auto* owner = object->GetUserData();
				auto* cell = owner ? owner->GetParentCell() : nullptr;
				if (!cell || std::ranges::find(a_space.Cells(), cell) != a_space.Cells().end()) {
					return false;
				}
				auto* loaded = cell->GetRuntimeData().loadedData;
				auto* cellNode = loaded && loaded->cell3D ? loaded->cell3D->AsNode() : nullptr;
				auto* mergedNode = holder->parent;
				if (cellNode && mergedNode && object->parent == mergedNode) {
					// The same slot as the holder's in its own cell (the node of the actors).
					std::uint16_t slot = 0;
					for (auto& child : mergedNode->GetChildren()) {
						if (child.get() == holder.get()) {
							break;
						}
						++slot;
					}
					auto* actors = slot < cellNode->GetChildren().size() && cellNode->GetChildren()[slot] ? cellNode->GetChildren()[slot]->AsNode() : nullptr;
					if (actors) {
						mergedNode->DetachChild(object.get());
						actors->AttachChild(object.get(), true);
						RE::NiUpdateData update;
						object->Update(update);
					}
				}
				return true;
			});
		}

		// A cell attached again right after the player left it (walking out through the doorway)
		// fades its references in over a second, so the room the camera is standing in is see-through.
		// It has been on screen all along (it was the current cell), so it is shown at full opacity.
		void SkipFadeIn(const Spaces::RemoteCell& a_space)
		{
			for (auto* cell : a_space.Cells()) {
				auto* loaded = cell->GetRuntimeData().loadedData;
				if (auto* cell3D = loaded ? loaded->cell3D.get() : nullptr) {
					Engine::ForceFadeIn(cell3D);
				}
			}
		}

		// Some doorways have a second door in the same frame (Riverwood Trader: 00070E69, not a
		// load door); with the doorway open in the picture it would stand in it.
		void HideDoorsInDoorway(MergedRoom& a_room, RE::TESObjectREFR* a_source, const RE::NiPoint3& a_center)
		{
			auto* cell = a_source->GetParentCell();
			auto* world = cell ? cell->GetRuntimeData().worldSpace : nullptr;
			const auto visit = [&](RE::TESObjectCELL* a_cell) {
				if (!a_cell) {
					return;
				}
				a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
					auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
					if (a_ref && a_ref != a_source && base && base->Is(RE::FormType::Door) && a_ref->Get3D() &&
						a_ref->GetPosition().GetDistance(a_center) < 120.0f &&
						std::ranges::none_of(a_room.extraDoors, [&](const Doors::OpenState& a_open) { return a_open.door == a_ref->GetFormID(); })) {
						Doors::Open(a_ref, a_room.extraDoors.emplace_back(), { .hide = true });
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			};
			visit(cell);
			if (world && world->persistentCell != cell) {
				visit(world->persistentCell);
			}
		}

		// Skinned shapes whose bones are driven by something that doesn't know about the merge (SMP
		// hair and cloth physics keep simulating where the cell used to be) are stretched from the
		// moved actor back to the cell's own coordinates: one long streak across the view. They are
		// hidden while any of their bones is far from the doorway.
		bool HasStrayBones(RE::BSGeometry* a_shape, const RE::NiPoint3& a_near)
		{
			auto*      skin = a_shape->GetGeometryRuntimeData().skinInstance.get();
			const auto count = skin && skin->skinData ? skin->skinData->bones : 0;
			for (std::uint32_t i = 0; i < count; ++i) {
				if (auto* bone = skin->bones[i]; bone && bone->world.translate.GetDistance(a_near) > 4000.0f) {
					return true;
				}
			}
			return false;
		}

		void ShowStrayShapes(MergedRoom& a_room)
		{
			for (auto& shape : a_room.strays) {
				Portal::Pristine::Show(shape.get());
			}
			a_room.strays.clear();
		}

		// a_street: the street merged into an interior, whose distant land and objects belong to it.
		MarkPass StartMarkPass(const Spaces::RemoteCell& a_space, bool a_street, const RE::NiPoint3& a_near)
		{
			MarkPass pass;
			pass.doorway = a_near;
			// The distant land and objects belong to the street: seen only through the doorway.
			if (a_street) {
				for (auto* root : StreetMerge::Roots()) {
					pass.roots.push_back({ RE::NiPointer<RE::NiAVObject>(root), nullptr });
				}
			}
			for (auto* cell : a_space.Cells()) {
				auto* loaded = cell->GetRuntimeData().loadedData;
				if (auto* cell3D = loaded ? loaded->cell3D.get() : nullptr) {
					pass.roots.push_back({ RE::NiPointer<RE::NiAVObject>(cell3D), cell });
				}
			}
			return pass;
		}

		// True once every root is done; roots are taken until a_budgetMs is spent (one at least).
		bool StepMarkPass(Spaces::RemoteCell& a_space, MarkPass& a_pass, double a_budgetMs)
		{
			const auto started = std::chrono::steady_clock::now();
			while (a_pass.next < a_pass.roots.size()) {
				const auto& root = a_pass.roots[a_pass.next++];
				if (root.cell) {
					a_space.UnfixNewBounds(root.cell);
				} else {
					StreetMerge::UnfixBounds(root.node.get());
				}
				RE::BSVisit::TraverseScenegraphGeometries(root.node.get(), [&](RE::BSGeometry* a_shape) {
					a_pass.shapes.insert(a_shape);
					if (root.cell && !a_shape->GetAppCulled() && HasStrayBones(a_shape, a_pass.doorway)) {
						a_pass.strays.emplace_back(a_shape);
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
				if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() >= a_budgetMs) {
					break;
				}
			}
			return a_pass.next >= a_pass.roots.size();
		}

		// People walking through the doorway belong to the room or the world.
		void FinishMarkPass(MarkPass& a_pass, bool a_street)
		{
			auto& room = shownRoom;
			auto& shapes = a_pass.shapes;
			ShowStrayShapes(room);
			for (auto& shape : a_pass.strays) {
				Portal::Pristine::Hide(shape.get());
			}
			room.strays = std::move(a_pass.strays);
			{
				std::vector<RE::NiAVObject*> withWorld, withRoom;
				Portal::NpcTravel::MergedOverrides(withWorld, withRoom);
				for (auto* object : withWorld) {
					RE::BSVisit::TraverseScenegraphGeometries(object, [&](RE::BSGeometry* a_shape) {
						shapes.erase(a_shape);
						return RE::BSVisit::BSVisitControl::kContinue;
					});
				}
				for (auto* object : withRoom) {
					RE::BSVisit::TraverseScenegraphGeometries(object, [&](RE::BSGeometry* a_shape) {
						shapes.insert(a_shape);
						return RE::BSVisit::BSVisitControl::kContinue;
					});
				}
			}
			std::unordered_set<const RE::BSGeometry*> player;
			auto*                                     pc = RE::PlayerCharacter::GetSingleton();
			for (auto* root : { pc->Get3D(false), pc->Get3D(true) }) {
				if (root) {
					RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_shape) {
						player.insert(a_shape);
						return RE::BSVisit::BSVisitControl::kContinue;
					});
				}
			}
			// A merged room stands in the world at its door, and is often bigger than the house around
			// it: it cast the sun's shadow onto the street. It has its own lighting and no part in the
			// world's shadows: its shapes don't cast any while merged. (Leaving their shadow passes out
			// at the shader property, vfunc 0x2B, instead left the room undrawn altogether.)
			// The street merged into an interior keeps its shadows: they are the sun's shadows on it.
			if (!a_street) {
				for (const auto* shape : shapes) {
					auto* property = shape->GetGeometryRuntimeData().shaderProperty.get();
					if (property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kCastShadows)) {
						property->flags.reset(RE::BSShaderProperty::EShaderPropertyFlag::kCastShadows);
						room.unshadowed.emplace_back(property);
					}
				}
			} else {
				for (auto& property : room.unshadowed) {
					property->flags.set(RE::BSShaderProperty::EShaderPropertyFlag::kCastShadows);
				}
				room.unshadowed.clear();
			}
			room.shapes = std::move(shapes);
			published.playerShapes = std::move(player);
		}

		// a_mark false: the merge ended, everything marked goes back. True: listed anew at once, in
		// this frame.
		void MarkMergedShapes(Spaces::RemoteCell* a_space, bool a_street, bool a_mark, const RE::NiPoint3& a_near = {})
		{
			auto& room = shownRoom;
			room.markPass.reset();
			if (a_mark && a_space) {
				auto pass = StartMarkPass(*a_space, a_street, a_near);
				StepMarkPass(*a_space, pass, std::numeric_limits<double>::infinity());
				FinishMarkPass(pass, a_street);
				return;
			}
			ShowStrayShapes(room);
			for (auto& property : room.unshadowed) {
				property->flags.set(RE::BSShaderProperty::EShaderPropertyFlag::kCastShadows);
			}
			room.unshadowed.clear();
			room.shapes.clear();
		}

		// Seen from the street, how far a room is drawn as an eye used to the street sees it rather
		// than as from inside: 0 within a few steps of its doorway, where walking in has to find the
		// room just as it is inside, 1 from across the street. a_inward: into the room.
		float FarWeight(const RE::NiPoint3& a_center, const RE::NiPoint3& a_inward)
		{
			constexpr float kNear = 250.0f;
			constexpr float kFar = 700.0f;
			const auto      offset = Crossing::CameraPosition() - a_center;
			if (offset.x * a_inward.x + offset.y * a_inward.y >= 0.0f) {
				return 0.0f;
			}
			return Core::SmoothStep(std::clamp((offset.Length() - kNear) / (kFar - kNear), 0.0f, 1.0f));
		}

		// The shown room, every frame it is drawn: it can be the street merged into an interior and
		// have the camera in it. a_space: the space it is merged from. a_wanted: its door swings
		// towards open. Its cull is added to a_culls.
		void UpdateMerged(Spaces::RemoteCell& a_space, const Active& a_portal, bool a_wanted, float a_delta, std::optional<MergedCull::Room>& a_cull)
		{
			auto& mergedRoom = shownRoom;
			const auto found = Doors::TryCompute(a_portal);
			if (!found) {
				return;
			}
			const auto& mapping = *found;
			auto*       source = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_portal.sourceDoor);
			auto*       linked = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_portal.linkedDoor);
			// The room is placed anew, live, when its place no longer follows from this door pair: a
			// door correction changed (the in-game menu), or the room was merged through another of
			// its doors (an inn's kitchen door, then shown through its front door). Left where the
			// first door put it, it stood off the doorway by however much it misfits its house.
			if (const auto placement = PlacementOf(a_portal, mapping); !a_space.Placed().SamePlace(placement)) {
				a_space.Remap(placement);
			}
			// Both door panels out of the way: the doorway is open in the picture. Of the two doors
			// standing in the same place, the one in the space the door opens into (outwards by
			// default, Doors::SwingingDoor) swings open into it; the other one goes. The same from
			// either side, and from before the space behind was there (the stand-in's door).
			const bool street = StreetMerge::Merged();
			auto*      room = street ? source : linked;
			const auto intoRoom = street ? RE::NiPoint3{ -mapping.inward.x, -mapping.inward.y, 0.0f } : mapping.inward;
			auto*      swung = Doors::SwingingDoor(source, linked) == source->GetFormID() ? source : linked;
			const auto settings = Doors::CorrectionDoor(source, linked)->GetFormID();
			if (mergedRoom.swungDoor && mergedRoom.swungDoor != swung->GetFormID()) {
				Doors::RestoreLeaves(mergedRoom.swungDoor);  // turned round in the menu
			}
			mergedRoom.swungDoor = swung->GetFormID();
			if (mergedRoom.swing.Shut()) {
				StandIns::TakeOverDoor(mergedRoom.swing, swung->GetFormID());
			}
			Doors::Swing::Reclaim(swung->GetFormID());
			const bool wasShut = mergedRoom.swing.Step(swung, a_wanted, a_delta);  // see PortalDoor::Update
			if (a_wanted && wasShut && Doors::OpenFromAfar(source, linked)) {
				mergedRoom.swing.SnapOpen();
			}
			// Shut, the doorway is a door again: solid, nothing to walk into, and the door standing in it
			// is the player's space's. The merged space is drawn only beyond the doorway's plane, and its
			// door shut on that plane wasn't drawn at all: from inside Belethor's, the street door
			// (the one that swings) left the doorway open on the street. Shut, both stand in one place.
			const bool shut = mergedRoom.swing.Shut();
			auto*      shown = shut ? source : swung;
			Doors::Open(source, mergedRoom.sourceDoor, { .hide = shown != source, .solid = shut });
			Doors::Open(linked, mergedRoom.linkedDoor, { .hide = shown != linked, .solid = shut });
			mergedRoom.swing.Turn(swung, swung, swung == room ? intoRoom : intoRoom * -1.0f, settings);
			if (!std::exchange(mergedRoom.extraScanned, true)) {
				HideDoorsInDoorway(mergedRoom, source, mapping.sourceCenter);
			}
			// At once in the merge's first frame (and after someone walked through the doorway,
			// RefreshShapes); otherwise twice a second, spread over frames (MarkPass).
			if (!mergedRoom.marked) {
				mergedRoom.marked = true;
				mergedRoom.markTimer = 0.5f;
				if (street) {
					StreetMerge::MergePersistent();
				}
				const Diag::Scope scope("mark shapes");
				MarkMergedShapes(&a_space, street, true, mapping.sourceCenter);
			} else if (!mergedRoom.markPass && (mergedRoom.markTimer -= a_delta) <= 0.0f) {
				mergedRoom.markTimer = 0.5f;
				if (street) {
					StreetMerge::MergePersistent();
				}
				mergedRoom.markPass = StartMarkPass(a_space, street, mapping.sourceCenter);
			}
			if (mergedRoom.markPass && StepMarkPass(a_space, *mergedRoom.markPass, 0.4)) {
				FinishMarkPass(*mergedRoom.markPass, street);
				mergedRoom.markPass.reset();
			}
			// Is the camera in the merged room? Behind the door plane alone doesn't say so: so is
			// the ground beside and behind the house, and from there the room was drawn in place
			// of the world. It is in the room when it is behind the plane and either the player
			// is in the doorway's passage (the camera follows them in), or the player is in front
			// of the door and seen by the camera through the doorway (they just walked out). Both at
			// the doorway's height: on the guard house's flat roof by Whiterun's gate, horizontally
			// "in the passage", the room was drawn and the world around it left out.
			bool cameraInside = false;
			{
				const auto doorway = Doors::DoorwayOf(mapping);
				const auto depthOf = [&](const RE::NiPoint3& a_point) { return Core::Crossing::DepthOf(doorway, ToCore(a_point)); };
				const auto sideOf = [&](const RE::NiPoint3& a_point) { return Core::Crossing::SideOf(doorway, ToCore(a_point)); };
				const auto  camera = Crossing::CameraPosition();
				const auto  player = RE::PlayerCharacter::GetSingleton()->GetPosition();
				const float cameraDepth = depthOf(camera), playerDepth = depthOf(player);
				if (cameraDepth > 0.0f) {
					if (playerDepth >= 0.0f) {
						cameraInside = sideOf(player) <= mapping.halfWidth + 40.0f && playerDepth <= 320.0f && Core::Crossing::FeetAtDoorway(player.z, doorway);
					} else {
						// Where the line from the camera to the player passes the door plane: in the
						// doorway's opening, not above it.
						const auto hit = camera + (player - camera) * (cameraDepth / (cameraDepth - playerDepth));
						cameraInside = sideOf(hit) <= mapping.halfWidth + 10.0f && std::abs(hit.z - mapping.sourceCenter.z) <= mapping.halfHeight + 10.0f;
					}
				}
			}
			// The mask's plane lies a little beyond the door's middle (Render::Doorway::kMargin): a panel
			// shut on it was drawn neither as the player's space nor as the merged one. Shut, the doorway
			// is masked from just behind the panel (the player's door stands in it), or the shut panel was
			// cut away with the space behind it, the sky showing. Just opening or about to shut, from
			// just in front of it when the merged space's door is the one swinging, or it showed the
			// street through itself for a tenth of a second (from inside Belethor's). Not further open:
			// the camera, walking through, came to stand beyond the plane before the crossing, and the
			// room wasn't masked at all. (Not masked at all while shut, the renderer took the room for
			// the world's: drawn through the house's walls once the door opened again.)
			constexpr float kNearlyShut = 0.3f;
			const float     clear = mapping.halfDepth + static_cast<float>(Core::Doorway::kMargin) + 1.0f;
			const float     shift = mergedRoom.swing.Shut() ? clear : swung == linked && mergedRoom.swing.Amount() < kNearlyShut ? -clear : 0.0f;
			const auto      center = mapping.sourceCenter + mapping.inward * shift;
			const auto door = Render::UprightDoor(center, mapping.inward, mapping.halfWidth, mapping.halfHeight, cameraInside);
			Render::Native::SetDoor(door);
			{
				MergedCull::Room cull;
				cull.door = door;
				cull.cells = a_space.Cells();
				// The merged space's door stands where the player's space's does: from outside it is
				// the door seen, shut or swinging, through the doorway or not.
				if (auto* roomDoor = linked->Get3D()) {
					cull.keep.emplace_back(roomDoor);
				}
				cull.doorShut = mergedRoom.swing.Shut();
				cull.street = street;
				if (cull.street) {
					for (auto* root : StreetMerge::Roots()) {
						cull.roots.emplace_back(root);
					}
				}
				a_cull = std::move(cull);
			}
			// (Not when the remote cells' own update did the same this frame, still loading.)
			if ((mergedRoom.seconds += a_delta) < kShowAtOnceSeconds && !a_space.FadeForced()) {
				SkipFadeIn(a_space);
			}
			// The NPCs of a merged room. Their 3D hangs in the cell's actor node, which the scene's
			// cull leaves out (the engine draws the actors of the player's cells itself, one by one;
			// for a merged cell its per-actor pass reports them visible, yet nothing is drawn). Hung
			// directly under the cell's 3D they are culled and drawn like everything else in the
			// room; they go back where they were when the merge ends. The engine's "not visible"
			// flag and drawn bits are overridden as well (see ActorCullHook in Render/Frame.cpp).
			// The engine would hang them back into the actors' node every frame (ActorHangHook keeps
			// it from doing so). Listed once each: listed every time they were hung, the list grew by
			// the room's people every frame (28000 entries after a minute in Whiterun's market).
			for (auto* cell : a_space.Cells()) {
				auto* loaded = cell->GetRuntimeData().loadedData;
				auto* cellNode = loaded && loaded->cell3D ? loaded->cell3D->AsNode() : nullptr;
				cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
					auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
					if (auto* object = actor ? actor->Get3D() : nullptr) {
						::Render::Frame::ShowActor(actor);
						if (cellNode && object->parent && object->parent != cellNode && object->parent->parent == cellNode) {
							if (std::ranges::find(mergedRoom.actors, object, [](const auto& a_entry) { return a_entry.first.get(); }) == mergedRoom.actors.end()) {
								mergedRoom.actors.emplace_back(RE::NiPointer<RE::NiAVObject>(object), RE::NiPointer<RE::NiNode>(object->parent));
							}
							object->parent->DetachChild(object);
							cellNode->AttachChild(object, true);
						}
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
			ReturnLeavers(mergedRoom, a_space);
			if ((mergedRoom.lightingTimer -= a_delta) <= 0.0f) {
				mergedRoom.lightingTimer = 1.0f;  // the cell's lighting doesn't change, the world's state might be reset
				mergedRoom.lighting = ::Render::SpaceLighting::MergedLighting(a_portal.cell, mapping.toLinked.Invert().rotate);
			}
			{
				// Seen from across the street, the room is drawn as an eye used to the street sees
				// it; from a few steps away as from inside (FarWeight). The street merged into a room
				// is drawn as it is.
				mergedRoom.door = a_portal.sourceDoor;
				const float away = street || cameraInside ? 0.0f : FarWeight(mapping.sourceCenter, mapping.inward);
				mergedRoom.away = away;
				auto lighting = mergedRoom.lighting;
				if (lighting) {
					lighting->scale = 1.0f + (StreetEyeScale(a_portal.sourceDoor) - 1.0f) * away;
					// Outdoors the engine multiplies the sun by the image space's sunlight scale (1.7 on a
					// clear day in Whiterun) before the shaders get it; the weather's colour alone left
					// the street a dull grey with barely visible shadows, next to the bright street
					// outside.
					if (street) {
						const auto* outdoors = ImageSpaceBlend::Outdoors();
						const float scale = outdoors ? outdoors->hdr.sunlightScale : 1.0f;
						lighting->sun.red *= scale;
						lighting->sun.green *= scale;
						lighting->sun.blue *= scale;
					}
				}
				Render::Native::SetMergedLighting(lighting);
				// The merged space is fogged as its own: seen from the street, the hall of a keep a
				// few thousand units deep stood in the street's haze, bluish, where the doorway's
				// picture before the merge (and the hall once walked in) had the keep's own.
				Render::Native::SetMergedFog(street ? ::Render::SpaceLighting::MergedFog() : ::Render::SpaceLighting::CellFog(a_portal.cell));
			}
		}

		// The merged room no longer drawn (let go): everything its drawing changed goes back. Its
		// cells stay merged until released (RemoteCell::Release).
		void EndMerged()
		{
			auto& mergedRoom = shownRoom;
			for (auto& [object, holder] : mergedRoom.actors) {
				// Only if it is still where it was put (the engine may have reloaded or moved it).
				if (object->parent && holder->parent == object->parent) {
					object->parent->DetachChild(object.get());
					holder->AttachChild(object.get(), true);
				}
			}
			mergedRoom.actors.clear();
			StreetMerge::Hide();
			Render::Native::SetMergedFog(std::nullopt);
			MergedCull::Set(std::nullopt);
			Render::Native::SetDoor(std::nullopt);
			Render::Native::SetMergedLighting(std::nullopt);
			mergedRoom.door = 0;
			mergedRoom.away = 0.0f;
			mergedRoom.lightingTimer = 0.0f;
			mergedRoom.seconds = 0.0f;
			MarkMergedShapes(nullptr, false, false);
			// The room's door, the one swung (UpdateMerged), left where it is to be taken over.
			for (const auto* state : { &mergedRoom.sourceDoor, &mergedRoom.linkedDoor }) {
				if (state->door && !state->hidden) {
					mergedRoom.swing.LetGo(state->door);
				}
			}
			mergedRoom.swing.Reset();
			mergedRoom.swungDoor = 0;
			Doors::Close(mergedRoom.sourceDoor);
			Doors::Close(mergedRoom.linkedDoor);
			for (auto& open : mergedRoom.extraDoors) {
				Doors::Close(open);
			}
			mergedRoom.extraDoors.clear();
			mergedRoom.extraScanned = false;
			mergedRoom.markTimer = 0.0f;
			mergedRoom.marked = false;
		}

		class RenderedScene final : public ::Render::SceneQuery
		{
		public:
			bool IsMergedShape(const RE::BSGeometry* a_shape) const override { return MergedView::IsMergedShape(a_shape); }
			bool IsPlayerShape(const RE::BSGeometry* a_shape) const override { return MergedView::IsPlayerShape(a_shape); }

			const std::vector<RE::TESObjectCELL*>& RemoteCells() const override { return Manager::RemoteCells(); }
		} sceneQuery;
	}

	void Install()
	{
		::Render::RegisterScene(sceneQuery);
		mergeEnabled = Plugin::Settings::Bool(Plugin::Settings::Id::kMerge);
		mergeOutdoors = Plugin::Settings::Bool(Plugin::Settings::Id::kMergeOutdoors);
		pauseUnseen = Plugin::Settings::Bool(Plugin::Settings::Id::kPauseUnseenRooms);
	}

	void SetMergeEnabled(bool a_enabled)
	{
		mergeEnabled = a_enabled;
	}

	bool MergeEnabled()
	{
		return mergeEnabled;
	}

	bool MergeSupported()
	{
		return mergeEnabled && Render::Native::Ready();
	}

	bool Mergeable(const RE::TESObjectCELL* a_here, const RE::TESObjectCELL* a_behindDoor)
	{
		if (!MergeSupported() || !a_here || !a_behindDoor) {
			return false;
		}
		return (!a_here->IsInteriorCell() && a_behindDoor->IsInteriorCell()) || (mergeOutdoors && a_here->IsInteriorCell() && !a_behindDoor->IsInteriorCell());
	}

	// How bright an eye used to the view sees a_door's room, against the room as drawn from
	// inside. Each space is drawn as an eye adapted to it sees it; for an eye that adapts a third
	// of the way (exposure as luminance^-1/3), what is drawn goes as luminance^2/3, and the
	// exposures adapted to the two differ by the square root of their drawn brightnesses' ratio.
	// Adapting half way (the ratio itself) left a dim inn's doorway across the market black.
	// No brighter than from inside: at night, when the street is the darker, the room is left
	// as it is.
	float StreetEyeScale(RE::FormID a_door)
	{
		const auto scene = ::Render::SceneMeter::Brightness();
		const auto room = Pictures::Cache::RoomBrightness(a_door);
		return scene && room ? std::min(1.0f, std::sqrt(*room / *scene)) : 1.0f;
	}

	bool PauseUnseenRooms()
	{
		return pauseUnseen;
	}

	void SetPauseUnseenRooms(bool a_pause)
	{
		pauseUnseen = a_pause;
	}

	bool KeptRoomPaused()
	{
		return pauseUnseen;
	}

	Spaces::RemoteCell::Placement PlacementOf(const Active& a_portal, const Portal::Doors::Mapping& a_mapping)
	{
		using RE::TESForm;
		auto*      source = TESForm::LookupByID<RE::TESObjectREFR>(a_portal.sourceDoor);
		auto*      linked = TESForm::LookupByID<RE::TESObjectREFR>(a_portal.linkedDoor);
		const auto key = Portal::Doors::CorrectionDoor(source, linked)->GetFormID();
		auto       toSource = a_mapping.toLinked.Invert();
		// A pair's two doors can be the same door placed at different sizes: Understone Keep's is 0.9
		// on the street, 1.0 inside. Seen from inside, the street's doorway was smaller than the
		// keep's, and through the keep's the street's facade around it showed, where the keep's own
		// door frame stands. The street seen from inside is drawn larger by their ratio, around its
		// door, so that the two doorways are one. Far off it looks the same (as much larger as it is
		// further away); close to the doorway it shrinks back on walking out. Only the street.
		if (!a_portal.cell->IsInteriorCell() && source->GetBaseObject() == linked->GetBaseObject()) {
			const float ratio = Portal::Doors::Size(source).halfHeight / Portal::Doors::Size(linked).halfHeight;
			if (std::abs(ratio - 1.0f) > 0.01f) {
				const auto      center = Portal::Doors::Center(linked);
				RE::NiTransform grow;
				grow.scale = ratio;
				grow.translate = center - center * ratio;
				toSource = toSource * grow;
			}
		}
		return { toSource, a_portal.sourceDoor, Portal::Doors::Correction(key), Portal::Doors::Shift(key) };
	}

	void UpdateShown(const Active& a_portal, bool a_wanted, float a_delta, std::optional<MergedCull::Room>& a_cull)
	{
		UpdateMerged(Manager::Space(), a_portal, a_wanted, a_delta, a_cull);
	}

	void EndShown()
	{
		if (Manager::Space().Merged()) {
			const Diag::Scope scope("release: end merged");
			EndMerged();
		}
		shownRoom = MergedRoom{};
	}

	void Publish()
	{
		published.room = Manager::Space().Merged() ? &shownRoom : nullptr;
		published.streetMerged = StreetMerge::Merged();
	}

	void PublishShownCells(bool a_shown)
	{
		std::scoped_lock lock(published.shownCellsLock);
		published.shownCells.clear();
		if (a_shown) {
			published.shownCells.assign(Manager::Space().Cells().begin(), Manager::Space().Cells().end());
		}
	}

	void UpdateLightWorlds(const std::optional<Active>& a_ready)
	{
		// Merged, the room is drawn at the door (RemoteCell::Merge moves its 3D by the inverse
		// door mapping) while its Havok world stays where the cell has it.
		RE::NiTransform                         toPhysics;
		std::optional<LightOcclusion::RoomDoor> roomDoor;
		if (const auto mapping = a_ready ? Doors::TryCompute(*a_ready) : std::nullopt) {
			roomDoor = LightOcclusion::RoomDoor{ mapping->toLinked * mapping->sourceCenter, mapping->toLinked.rotate * mapping->inward, mapping->halfWidth, mapping->halfHeight };
			if (Manager::Space().Merged()) {
				toPhysics = mapping->toLinked;
			}
		}
		if (a_ready && Manager::Space().Merged()) {
			// Lit from its own light scene, which the engine doesn't cull or fade for this view.
			LightScene::PrepareForView();
		}
		auto* remoteCell = a_ready ? Manager::Space().Cell() : nullptr;
		LightOcclusion::SetWorld(remoteCell ? remoteCell->GetbhkWorld() : nullptr, toPhysics, roomDoor, remoteCell && remoteCell->IsInteriorCell());
	}

	void RefreshShapes()
	{
		shownRoom.marked = false;
	}

	std::vector<RE::TESObjectCELL*> MergedCells()
	{
		std::vector<RE::TESObjectCELL*> cells;
		if (Manager::Space().Merged()) {
			cells = Manager::Space().Cells();
		}
		return cells;
	}

	bool IsMergedShape(const RE::BSGeometry* a_shape)
	{
		if (published.room && published.room->shapes.contains(a_shape)) {
			return true;
		}
		// Water hangs under the water system, not in its cell's 3D, and only the space the player
		// is in has any: with the street merged into an interior it is the street's. Masked as the
		// player's, the street's canal running under the houses showed in the Ratway's doorway, over
		// the steps down behind it.
		return published.streetMerged && netimmerse_cast<RE::BSWaterShaderProperty*>(a_shape->GetGeometryRuntimeData().shaderProperty.get());
	}

	bool IsShownCell(const RE::TESObjectCELL* a_cell)
	{
		std::scoped_lock lock(published.shownCellsLock);
		return std::ranges::find(published.shownCells, a_cell) != published.shownCells.end();
	}

	bool IsPlayerShape(const RE::BSGeometry* a_shape)
	{
		return published.playerShapes.contains(a_shape);
	}

	float ShownAmount()
	{
		return shownRoom.swing.Amount();
	}

	bool ShownShut()
	{
		return shownRoom.swing.Shut();
	}

	float RoomLightScale(RE::FormID a_door)
	{
		auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_door);
		if (!door || !door->Get3D()) {
			return 1.0f;
		}
		const auto  inward = StandIns::AwayFrom(door, Crossing::CameraPosition());
		const float away = FarWeight(Portal::Doors::ShownCenter(door), inward);
		return 1.0f + (StreetEyeScale(a_door) - 1.0f) * away;
	}

	std::optional<RoomInfo> Shown()
	{
		if (!Manager::Space().Merged()) {
			return std::nullopt;
		}
		return RoomInfo{ shownRoom.door, shownRoom.swing.Amount(), shownRoom.shapes.size(), shownRoom.away };
	}

	std::vector<const RE::BSGeometry*> Shapes()
	{
		std::vector<const RE::BSGeometry*> shapes;
		if (published.room) {
			for (const auto* shape : published.room->shapes) {
				shapes.push_back(shape);
			}
		}
		return shapes;
	}
}
