#include "Skyrim/Portal/MergedView.h"

#include "Skyrim/Diag/Probe.h"
#include "Skyrim/Diag/Telemetry.h"
#include "Skyrim/Doors/DoorUse.h"
#include "Skyrim/Doors/Geometry.h"
#include "Skyrim/Doors/Hold.h"
#include "Skyrim/Doors/Policy.h"
#include "Skyrim/Doors/Settings.h"
#include "Skyrim/Doors/Swing.h"
#include "Skyrim/Engine/Fade.h"
#include "Skyrim/Pictures/DoorwaySnapshot.h"
#include "Skyrim/Plugin/Settings.h"
#include "Skyrim/Portal/Crossing.h"
#include "Skyrim/Portal/ImageSpaceBlend.h"
#include "Skyrim/Portal/LightOcclusion.h"
#include "Skyrim/Portal/LightScene.h"
#include "Skyrim/Portal/NpcTravel.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/Pristine.h"
#include "Skyrim/Portal/Selection.h"
#include "Skyrim/Portal/ShapeSet.h"
#include "Skyrim/Portal/StandIns.h"
#include "Skyrim/Portal/StreetMerge.h"
#include "Skyrim/Render/Backend.h"
#include "Skyrim/Render/DoorwayPlanes.h"
#include "Skyrim/Render/Frame.h"
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
		// [Portals] bPauseUnseenRooms: a kept room nobody can see (kept out of sight, or merged beside
		// the shown one with its doorway off screen) is paused: not stepped, lights not updated, not
		// drawn, its people not on screen. Facing away, each kept room cost about 1 ms a frame.
		bool pauseUnseen = true;
		// Out of sight this long before a room merged beside the shown one pauses, so that looking
		// around past its doorway doesn't pause and wake it every few frames. Waking is at once.
		constexpr float kPauseAfterSeconds = 0.5f;

		constexpr float kSeenMargin = 0.15f;
		// Whether the doorway (its opening, at the door's plane) is on screen or close to it. Walls in
		// between aren't considered.
		bool DoorwayInView(const Portal::Doors::Mapping& a_mapping, float a_margin)
		{
			auto* camera = RE::Main::WorldRootCamera();
			if (!camera) {
				return true;
			}
			const RE::NiPoint3 right{ a_mapping.inward.y, -a_mapping.inward.x, 0.0f };
			const RE::NiPoint3 up{ 0.0f, 0.0f, 1.0f };
			float              minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX;
			int                behind = 0;
			for (const float sx : { -1.0f, 1.0f }) {
				for (const float sz : { -1.0f, 1.0f }) {
					const auto corner = a_mapping.sourceCenter + right * (sx * a_mapping.halfWidth) + up * (sz * a_mapping.halfHeight);
					float      x = 0.0f, y = 0.0f, z = 0.0f;
					camera->WorldPtToScreenPt3(corner, x, y, z, 1e-5f);
					if (z < 0.0f) {
						++behind;
						continue;
					}
					minX = std::min(minX, x);
					maxX = std::max(maxX, x);
					minY = std::min(minY, y);
					maxY = std::max(maxY, y);
				}
			}
			if (behind == 4) {
				return false;
			}
			if (behind > 0) {
				return true;  // partly behind the camera: the doorway is right at it
			}
			return maxX >= -a_margin && minX <= 1.0f + a_margin && maxY >= -a_margin && minY <= 1.0f + a_margin;
		}

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
			float                                daylight = 0.0f;  // as sent (Render::Backend::SetMergedDaylight)
			// Actor 3D moved out of its cell's actor node while merged, with the node to put it back in.
			std::vector<std::pair<RE::NiPointer<RE::NiAVObject>, RE::NiPointer<RE::NiNode>>> actors;
			float                                seconds = 0.0f;  // since the merge began
			Portal::Doors::OpenState             sourceDoor;
			Portal::Doors::OpenState             linkedDoor;
			Portal::Doors::Swing                 swing;  // of the swung door (swungDoor)
			RE::FormID                           swungDoor = 0;  // which of the pair that is (Doors::SwingingDoor)
			std::vector<Portal::Doors::OpenState> extraDoors;
			bool                                 extraScanned = false;
			// A room merged beside the shown one (slots 1..3): the kept space it is merged from
			// (Selection), the door pair it is seen through, and whether its doorway was in the last
			// player view (BeforePlayerView) or is paused.
			struct Beside
			{
				RE::TESObjectCELL* cell = nullptr;
				RE::FormID         sourceDoor = 0;
				RE::FormID         linkedDoor = 0;
				bool               seen = true;
				float              unseen = 0.0f;   // seconds since it was
				bool               paused = false;  // its cells parked meanwhile
			} beside;
		};
		// The merged rooms by slot with the render backend. Slot 0, the shown room's, is always there
		// (fresh while nothing is merged); a room beside the shown one is in 1..3 while it is merged.
		std::array<std::optional<MergedRoom>, ::Render::kMaxRooms> rooms = [] {
			std::array<std::optional<MergedRoom>, ::Render::kMaxRooms> a;
			a[0].emplace();
			return a;
		}();

		// What the renderer reads, off the manager's update. The rooms by slot (RoomOf, SceneOf) are
		// set again (Publish) whenever a room is merged, handed from one role to the other, or let
		// go. These, each room's shapes and the player's shapes are written in the manager's update
		// and read by the renderer, both on the main thread, so without a lock: RoomOf is asked
		// several times for every draw. The shown cells are read by the actor cull (ID 40335,
		// ActorCullHook), which also runs off the main thread.
		struct Published
		{
			std::array<const MergedRoom*, ::Render::kMaxRooms>    rooms{};
			std::array<RE::ShadowSceneNode*, ::Render::kMaxRooms> scenes{};
			// The street is merged into an interior (room 0): the water drawn is the street's
			// (RoomOf).
			bool                                      streetMerged = false;
			std::unordered_set<const RE::BSGeometry*> playerShapes;
			std::mutex                                shownCellsLock;
			std::vector<const RE::TESObjectCELL*>     shownCells;  // under shownCellsLock
		} published;

		// Someone who walked out of the merged room: their 3D must not stay under its (moved)
		// cell, or it is drawn tens of thousands of units away until the engine happens to
		// rebuild it; out on the street they were invisible for a second. It goes into the
		// actors' node of the cell they are in now. Also while the room is paused: they walk on.
		void ReturnLeavers(MergedRoom& a_room, const Portal::RemoteCell& a_space)
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
					std::uint32_t slot = 0;
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
		void SkipFadeIn(const Portal::RemoteCell& a_space)
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

		// The engine's sun shadow light shares the interior's directional light, so its shadows fall
		// the way the interior's light shines (in Belethor's: low, from the north-east). While the
		// street is merged in, the shadow light gets a light of its own pointed like the outdoor sun.
		// The interior's shapes keep the interior's direction; the shader leaves them unshadowed
		// (Community Shaders, SeamlessParams.w).
		RE::NiPointer<RE::NiDirectionalLight> streetSun;
		RE::NiPointer<RE::NiLight>            shadowLightOwn;  // the shadow light's own, to put back
		RE::BSShadowDirectionalLight*         shadowLightSwapped = nullptr;

		void AimSunShadow(const RE::NiPoint3* a_towardSun)
		{
			auto* world = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
			auto* shadow = world ? world->GetRuntimeData().sunShadowDirLight : nullptr;
			if (!a_towardSun || !shadow) {
				if (shadowLightSwapped && shadowLightSwapped == shadow && shadow->light == streetSun) {
					shadow->light = shadowLightOwn;
				}
				shadowLightSwapped = nullptr;
				shadowLightOwn.reset();
				return;
			}
			if (!streetSun) {
				streetSun.reset(RE::NiDirectionalLight::Create());
			}
			if (shadow->light != streetSun) {
				shadowLightOwn = shadow->light;
				shadowLightSwapped = shadow;
				if (shadowLightOwn) {
					streetSun->GetLightRuntimeData() = shadowLightOwn->GetLightRuntimeData();
				}
				shadow->light = streetSun;
			}
			// A directional light shines along its local +X.
			RE::NiPoint3 along = *a_towardSun * -1.0f;
			along.Unitize();
			RE::NiPoint3 side = along.UnitCross({ 0.0f, 0.0f, 1.0f });
			if (side.Length() < 0.5f) {
				side = { 1.0f, 0.0f, 0.0f };
			}
			const RE::NiPoint3 up = side.UnitCross(along);
			auto&              m = streetSun->local.rotate;
			for (int row = 0; row < 3; ++row) {
				m.entry[row][0] = along[row];
				m.entry[row][1] = side[row];
				m.entry[row][2] = up[row];
			}
			RE::NiUpdateData update;
			streetSun->Update(update);
			// The shadow light keeps its own copy of the direction and takes it from its light only
			// when the engine next updates it: for the first fifth of a second after the merge the
			// street's shadows fell from the sun as it stood before the street was turned to fit the
			// room.
			shadow->GetShadowDirectionalLightRuntimeData().sunVector = along;
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
		MarkPass StartMarkPass(const Portal::RemoteCell& a_space, bool a_street, const RE::NiPoint3& a_near)
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
		bool StepMarkPass(Portal::RemoteCell& a_space, MarkPass& a_pass, double a_budgetMs)
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

		// Slot 0, the shown room's: people walking through its doorway belong to it or the world.
		void FinishMarkPass(int a_slot, MarkPass& a_pass, bool a_street)
		{
			auto& room = *rooms[a_slot];
			auto& shapes = a_pass.shapes;
			ShowStrayShapes(room);
			for (auto& shape : a_pass.strays) {
				Portal::Pristine::Hide(shape.get());
			}
			room.strays = std::move(a_pass.strays);
			if (a_slot == 0) {
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
		void MarkMergedShapes(int a_slot, Portal::RemoteCell* a_space, bool a_street, bool a_mark, const RE::NiPoint3& a_near = {})
		{
			auto& room = *rooms[a_slot];
			room.markPass.reset();
			if (a_mark && a_space) {
				auto pass = StartMarkPass(*a_space, a_street, a_near);
				StepMarkPass(*a_space, pass, std::numeric_limits<double>::infinity());
				FinishMarkPass(a_slot, pass, a_street);
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
			const float t = std::clamp((offset.Length() - kNear) / (kFar - kNear), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}

		// The slot (1..3) a room merged beside the shown one is in, by its kept space's cell; 0 when
		// it isn't.
		int SlotOfBeside(const RE::TESObjectCELL* a_cell)
		{
			for (int slot = 1; slot < ::Render::kMaxRooms; ++slot) {
				if (rooms[slot] && rooms[slot]->beside.cell == a_cell) {
					return slot;
				}
			}
			return 0;
		}

		int BesideWanted()
		{
			return std::max(0, std::min(Selection::ShownRooms(), ::Render::Backend::MergedRooms()) - 1);
		}

		int FreeSlot()
		{
			for (int slot = 1; slot < ::Render::Backend::MergedRooms(); ++slot) {
				if (!rooms[slot]) {
					return slot;
				}
			}
			return 0;
		}

		// One merged room, every frame it is drawn. a_slot: its slot with the render backend, 0 for
		// the shown room, which alone can be the street merged into an interior and have the camera in
		// it. a_space: the space it is merged from. a_wanted: its door swings towards open. Its cull is
		// added to a_culls.
		void UpdateMerged(int a_slot, Portal::RemoteCell& a_space, const Active& a_portal, bool a_wanted, float a_delta, std::vector<MergedCull::Room>& a_culls)
		{
			auto& mergedRoom = *rooms[a_slot];
			const auto found = Doors::TryCompute(a_portal);
			if (!found) {
				return;
			}
			const auto& mapping = *found;
			auto*       source = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_portal.sourceDoor);
			auto*       linked = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_portal.linkedDoor);
			const bool primary = a_slot == 0;
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
			const bool street = primary && StreetMerge::Merged();
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
				MarkMergedShapes(a_slot, &a_space, street, true, mapping.sourceCenter);
			} else if (!mergedRoom.markPass && (mergedRoom.markTimer -= a_delta) <= 0.0f) {
				mergedRoom.markTimer = 0.5f;
				if (street) {
					StreetMerge::MergePersistent();
				}
				mergedRoom.markPass = StartMarkPass(a_space, street, mapping.sourceCenter);
			}
			if (mergedRoom.markPass && StepMarkPass(a_space, *mergedRoom.markPass, 0.4)) {
				FinishMarkPass(a_slot, *mergedRoom.markPass, street);
				mergedRoom.markPass.reset();
			}
			// Is the camera in the merged room? Behind the door plane alone doesn't say so: so is
			// the ground beside and behind the house, and from there the room was drawn in place
			// of the world. It is in the room when it is behind the plane and either the player
			// is in the doorway's passage (the camera follows them in), or the player is in front
			// of the door and seen by the camera through the doorway (they just walked out). Both at
			// the doorway's height: on the guard house's flat roof by Whiterun's gate, horizontally
			// "in the passage", the room was drawn and the world around it left out.
			// Only the shown room is walked into; the camera is never in one merged beside it.
			bool cameraInside = false;
			if (primary) {
				const auto depthOf = [&](const RE::NiPoint3& a_point) {
					return (a_point.x - mapping.sourceCenter.x) * mapping.inward.x + (a_point.y - mapping.sourceCenter.y) * mapping.inward.y;
				};
				const auto sideOf = [&](const RE::NiPoint3& a_point) {
					return std::abs((a_point.x - mapping.sourceCenter.x) * mapping.inward.y - (a_point.y - mapping.sourceCenter.y) * mapping.inward.x);
				};
				const auto  camera = Crossing::CameraPosition();
				const auto  player = RE::PlayerCharacter::GetSingleton()->GetPosition();
				const float cameraDepth = depthOf(camera), playerDepth = depthOf(player);
				if (cameraDepth > 0.0f) {
					if (playerDepth >= 0.0f) {
						cameraInside = sideOf(player) <= mapping.halfWidth + 40.0f && playerDepth <= 320.0f && Crossing::FeetAtDoorway(player.z, mapping);
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
			const float     clear = mapping.halfDepth + static_cast<float>(::Render::Doorway::kMargin) + 1.0f;
			const float     shift = mergedRoom.swing.Shut() ? clear : swung == linked && mergedRoom.swing.Amount() < kNearlyShut ? -clear : 0.0f;
			const auto      center = mapping.sourceCenter + mapping.inward * shift;
			const auto door = Render::DoorRect::Upright(center, mapping.inward, mapping.halfWidth, mapping.halfHeight, cameraInside);
			::Render::Backend::SetMergedDoor(a_slot, door);
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
				a_culls.push_back(std::move(cull));
			}
			// The street merged into the interior gets the sun's light and shadows, through Community
			// Shaders' Interior Sun (the engine's sun shadow light, on as for an interior flagged for it).
			if (primary) {
				::Render::Backend::SetInteriorSun(street);
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
				// it, with the street's light falling in through its doorway; from a few steps away
				// as from inside (FarWeight). The street merged into a room is drawn as it is.
				mergedRoom.door = a_portal.sourceDoor;
				const float away = street || cameraInside ? 0.0f : FarWeight(mapping.sourceCenter, mapping.inward);
				mergedRoom.daylight = away;
				{
					// What comes in is the street as seen through the doorway: as bright as the eye
					// finds the view, tinted as the outdoors' light on a surface facing out of it.
					Render::Daylight daylight{};
					const auto       tint = away > 0.0f ? ::Render::SpaceLighting::OutdoorAmbient(mapping.inward * -1.0f) : std::nullopt;
					const auto       view = ::Render::SceneMeter::Brightness();
					const float      luminance = tint ? 0.2126f * tint->red + 0.7152f * tint->green + 0.0722f * tint->blue : 0.0f;
					if (view && luminance > 0.0f) {
						const float scale = *view * away / luminance;
						daylight.radiance = { tint->red * scale, tint->green * scale, tint->blue * scale };
					}
					::Render::Backend::SetMergedDaylight(a_slot, daylight);
				}
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
				::Render::Backend::SetMergedLighting(a_slot, lighting);
				if (primary) {
					if (street && lighting) {
						AimSunShadow(&lighting->towardSun);
					} else {
						AimSunShadow(nullptr);
					}
					// The merged space is fogged as its own: seen from the street, the hall of a keep a
					// few thousand units deep stood in the street's haze, bluish, where the doorway's
					// picture before the merge (and the hall once walked in) had the keep's own. One fog
					// for all merged rooms (the shaders take one): the shown room's.
					const auto fog = street ? ::Render::SpaceLighting::MergedFog() : ::Render::SpaceLighting::CellFog(a_portal.cell);
					::Render::Backend::SetMergedFog(fog);
					// Up to the doorway it is the street's air.
					const auto front = street ? std::nullopt : ::Render::SpaceLighting::MergedFog();
					::Render::Backend::SetFrontFog(front);
				}
			}
		}

		// A merged room no longer drawn (let go, or no longer among those shown): everything its
		// drawing changed goes back. Its cells stay merged until released (RemoteCell::Release).
		void EndMerged(int a_slot)
		{
			auto& mergedRoom = *rooms[a_slot];
			for (auto& [object, holder] : mergedRoom.actors) {
				// Only if it is still where it was put (the engine may have reloaded or moved it).
				if (object->parent && holder->parent == object->parent) {
					object->parent->DetachChild(object.get());
					holder->AttachChild(object.get(), true);
				}
			}
			mergedRoom.actors.clear();
			if (a_slot == 0) {
				StreetMerge::Hide();
				::Render::Backend::SetInteriorSun(false);
				::Render::Backend::SetMergedFog(std::nullopt);
				::Render::Backend::SetFrontFog(std::nullopt);
				AimSunShadow(nullptr);
			}
			// Set again by the next update for the rooms still merged.
			MergedCull::Set({});
			::Render::Backend::SetMergedDoor(a_slot, std::nullopt);
			::Render::Backend::SetMergedLighting(a_slot, std::nullopt);
			::Render::Backend::SetMergedDaylight(a_slot, std::nullopt);
			mergedRoom.door = 0;
			mergedRoom.daylight = 0.0f;
			mergedRoom.lightingTimer = 0.0f;
			mergedRoom.seconds = 0.0f;
			MarkMergedShapes(a_slot, nullptr, false, false);
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

		// Whether a line of sight from the camera, as it is now, could pass the room's doorway: the
		// doorway on screen or near it, and the camera in front of the door (beside or behind the
		// house none does). Walls in between aren't considered.
		bool DoorwaySeen(const MergedRoom::Beside& a_room, const Portal::RemoteCell& a_space)
		{
			auto* camera = RE::Main::WorldRootCamera();
			if (!camera) {
				return true;
			}
			const auto mapping = Doors::TryCompute(Active{ a_room.sourceDoor, a_room.linkedDoor, a_room.cell, a_space.Generation() });
			if (!mapping) {
				return true;
			}
			const auto      offset = camera->world.translate - mapping->sourceCenter;
			constexpr float kBehindSlack = 10.0f;
			return offset.x * mapping->inward.x + offset.y * mapping->inward.y < kBehindSlack && DoorwayInView(*mapping, kSeenMargin);
		}

		// Paused, the room keeps everything it is drawn with (doors swung, shapes listed, its
		// doorway and light with the renderer), so it is drawn again the moment it wakes. Its cells
		// are parked: the engine's cull doesn't walk them.
		void SetBesidePaused(MergedRoom::Beside& a_room, Portal::RemoteCell& a_space, bool a_paused)
		{
			a_room.paused = a_paused;
			a_space.SetParked(a_paused);
			a_space.SetPaused(a_paused);
			logger::info("portal: {} {} (doorway {})", Probe::DescribeCell(a_room.cell), a_paused ? "paused" : "woken", a_paused ? "out of sight" : "in sight");
		}

		class RenderedScene final : public ::Render::SceneQuery
		{
		public:
			bool IsMergedShape(const RE::BSGeometry* a_shape) const override { return RoomOf(a_shape) >= 0; }
			bool IsPlayerShape(const RE::BSGeometry* a_shape) const override { return MergedView::IsPlayerShape(a_shape); }
			int  MergedRoomOf(const RE::BSGeometry* a_shape) const override { return RoomOf(a_shape); }

			RE::ShadowSceneNode* MergedRoomScene(int a_slot) const override { return SceneOf(a_slot); }

			const std::vector<RE::TESObjectCELL*>& RemoteCells() const override { return Manager::RemoteCells(); }
		} sceneQuery;

		// Not drawn any more: its door let go (a stand-in takes it over, picture and all), its cells
		// left out of the cull until it is shown again or let go.
		void HideBeside(int a_slot, std::string_view a_why)
		{
			auto* space = Selection::KeptSpace(rooms[a_slot]->beside.cell);
			logger::info("portal: {} no longer shown beside the nearest door ({})", Probe::DescribeCell(rooms[a_slot]->beside.cell), a_why);
			EndMerged(a_slot);
			if (space) {
				space->SetParked(true);
			}
			// Destroyed only once the renderer can't find it any more.
			auto dying = std::move(rooms[a_slot]);
			rooms[a_slot].reset();
			Publish();
		}
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
		return mergeEnabled && ::Render::Backend::CanMerge();
	}

	// Merging puts an interior into the exterior's scene. The other way round (the street, its land
	// and the distant LOD put into an interior's scene) is the mergeOutdoors prototype: indoors the
	// LOD stays loaded, only hidden, and the sky's dome stays in the scene (bridge outdoors_probe).
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
		const auto room = Portal::Snapshot::RoomBrightness(a_door);
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

	bool KeptRoomPaused(const RE::TESObjectCELL* a_cell)
	{
		const int slot = SlotOfBeside(a_cell);
		return pauseUnseen && (!slot || rooms[slot]->beside.paused);
	}

	Portal::RemoteCell::Placement PlacementOf(const Active& a_portal, const Portal::Doors::Mapping& a_mapping)
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

	void UpdateShown(const Active& a_portal, bool a_wanted, float a_delta, std::vector<MergedCull::Room>& a_culls)
	{
		UpdateMerged(0, Manager::Space(), a_portal, a_wanted, a_delta, a_culls);
	}

	void UpdateBeside(RE::TESObjectCELL* a_here, float a_delta, std::vector<MergedCull::Room>& a_culls)
	{
		struct Pick
		{
			Portal::RemoteCell* space;
			RE::FormID          door;
			RE::FormID          linked;
			float               distance;
		};
		std::vector<Pick> picks;
		const int         wanted = MergeSupported() && !a_here->IsInteriorCell() ? BesideWanted() : 0;
		const auto        origin = RE::PlayerCharacter::GetSingleton()->GetPosition();
		for (auto& space : Selection::Kept()) {
			auto* cell = space.Cell();
			if (wanted <= 0 || !cell ||!cell->IsInteriorCell() || !space.Healthy() || !space.FullyLoaded() || !Mergeable(a_here, cell)) {
				continue;
			}
			// By the door nearest the player into it: one doorway per room, its 3D can be in one place.
			const auto* target = Selection::TargetOf(cell->GetFormID());
			auto*       door = target ? RE::TESForm::LookupByID<RE::TESObjectREFR>(target->door) : nullptr;
			auto*       linked = target ? RE::TESForm::LookupByID<RE::TESObjectREFR>(target->linked) : nullptr;
			if (!door || !linked || !door->Get3D() || !linked->Get3D() || (door->IsLocked() && DoorUse::GetForce() != DoorUse::Force::kAllOpen)) {
				continue;
			}
			// Drawn while its door is to be open (Wanted), and on while it swings shut.
			const int   shown = SlotOfBeside(cell);
			const float distance = origin.GetDistance(door->GetPosition());
			if (!Portal::Doors::Wanted(door, linked) && (!shown || rooms[shown]->beside.sourceDoor != door->GetFormID() || rooms[shown]->swing.Shut())) {
				continue;
			}
			picks.push_back({ &space, door->GetFormID(), linked->GetFormID(), distance });
		}
		std::ranges::sort(picks, {}, &Pick::distance);
		if (std::cmp_greater(picks.size(), wanted)) {
			picks.resize(wanted);
		}
		for (int slot = 1; slot < ::Render::kMaxRooms; ++slot) {
			if (!rooms[slot]) {
				continue;
			}
			const auto& beside = rooms[slot]->beside;
			const bool  picked = std::ranges::any_of(picks, [&](const Pick& a_pick) { return a_pick.space->Cell() == beside.cell && a_pick.door == beside.sourceDoor; });
			if (!picked) {
				HideBeside(slot, wanted > 0 ? "door out of reach or others nearer" : "none to be shown");
			}
		}
		// One merge a frame at most: each is a long frame.
		bool mergedOne = false;
		for (const auto& pick : picks) {
			auto* cell = pick.space->Cell();
			if (SlotOfBeside(cell)) {
				continue;
			}
			const int slot = FreeSlot();
			if (!slot) {
				break;
			}
			const Active portal{ pick.door, pick.linked, cell, pick.space->Generation() };
			try {
				// Paused as a room kept out of sight: the merge's update below takes its lights in.
				pick.space->SetPaused(false);
				if (!pick.space->Merged()) {
					if (mergedOne) {
						continue;
					}
					mergedOne = true;
					const auto started = std::chrono::steady_clock::now();
					const auto result = pick.space->Merge(PlacementOf(portal, Doors::Compute(portal)));
					pick.space->Update(0.0f);
					logger::info("portal: merged {} beside the nearest door ({}, {:.1f} ms)", Probe::DescribeCell(cell), result.dump(),
						std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count());
				} else {
					pick.space->SetParked(false);
					logger::info("portal: {} shown beside the nearest door again", Probe::DescribeCell(cell));
				}
			} catch (const std::exception& e) {
				logger::warn("portal: could not merge {} beside the nearest door: {}", Probe::DescribeCell(cell), e.what());
				continue;
			}
			rooms[slot].emplace();
			rooms[slot]->beside = { cell, pick.door, pick.linked };
			Publish();
		}
		for (int slot = 1; slot < ::Render::kMaxRooms; ++slot) {
			if (!rooms[slot]) {
				continue;
			}
			auto& beside = rooms[slot]->beside;
			auto* space = Selection::KeptSpace(beside.cell);
			if (!space) {
				continue;
			}
			beside.unseen = beside.seen ? 0.0f : beside.unseen + a_delta;
			if (const bool pause = pauseUnseen && beside.unseen >= kPauseAfterSeconds; pause != beside.paused) {
				SetBesidePaused(beside, *space, pause);
			}
			if (beside.paused) {
				ReturnLeavers(*rooms[slot], *space);
				continue;
			}
			auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(beside.sourceDoor);
			auto* linked = RE::TESForm::LookupByID<RE::TESObjectREFR>(beside.linkedDoor);
			const bool open = door && linked && Portal::Doors::Wanted(door, linked);
			UpdateMerged(slot, *space, Active{ beside.sourceDoor, beside.linkedDoor, beside.cell, space->Generation() }, open, a_delta, a_culls);
		}
	}

	void EndShown()
	{
		if (Manager::Space().Merged()) {
			const Diag::Scope scope("release: end merged");
			EndMerged(0);
		}
		rooms[0].emplace();
	}

	void TakeBeside(const RE::TESObjectCELL* a_cell)
	{
		const int slot = SlotOfBeside(a_cell);
		if (!slot) {
			return;
		}
		::Render::Backend::SetMergedDoor(slot, std::nullopt);
		::Render::Backend::SetMergedLighting(slot, std::nullopt);
		auto* cell = rooms[slot]->beside.cell;
		rooms[0] = std::move(rooms[slot]);
		rooms[slot].reset();
		Publish();
		logger::info("portal: {} goes on as the shown room", Probe::DescribeCell(cell));
	}

	int ReserveBesideSlot(RE::TESObjectCELL* a_here, const Active& a_portal)
	{
		if (BesideWanted() <= 0 || !Manager::Space().Merged() || StreetMerge::Merged() || a_here->IsInteriorCell() || !a_portal.cell || !a_portal.cell->IsInteriorCell() ||
			!Manager::Space().FullyLoaded()) {
			return 0;
		}
		if (!FreeSlot()) {
			// Every slot taken: the farthest of them makes room (the shown room's door was the nearest).
			const auto origin = RE::PlayerCharacter::GetSingleton()->GetPosition();
			int        farthest = 0;
			float      farthestDistance = 0.0f;
			for (int slot = 1; slot < ::Render::kMaxRooms; ++slot) {
				if (!rooms[slot]) {
					continue;
				}
				auto*       door = RE::TESForm::LookupByID<RE::TESObjectREFR>(rooms[slot]->beside.sourceDoor);
				const float distance = door ? origin.GetDistance(door->GetPosition()) : FLT_MAX;
				if (!farthest || distance > farthestDistance) {
					farthest = slot;
					farthestDistance = distance;
				}
			}
			HideBeside(farthest, "the shown room goes beside");
		}
		return FreeSlot();
	}

	void MoveShownBeside(int a_slot, const Active& a_portal)
	{
		::Render::Backend::SetMergedDoor(0, std::nullopt);
		::Render::Backend::SetMergedLighting(0, std::nullopt);
		RemoteCell space;
		std::swap(space, Manager::Space());
		Selection::Kept().push_back(std::move(space));
		rooms[a_slot] = std::move(rooms[0]);
		rooms[a_slot]->beside = { a_portal.cell, a_portal.sourceDoor, a_portal.linkedDoor };
		rooms[0].emplace();
		Publish();
	}

	void HideBesideOf(const RE::TESObjectCELL* a_cell, std::string_view a_why)
	{
		if (const int slot = SlotOfBeside(a_cell)) {
			HideBeside(slot, a_why);
		}
	}

	bool BesideDoor(RE::FormID a_door)
	{
		for (int slot = 1; slot < ::Render::kMaxRooms; ++slot) {
			if (rooms[slot] && rooms[slot]->beside.sourceDoor == a_door) {
				return true;
			}
		}
		return false;
	}

	void Publish()
	{
		published.rooms.fill(nullptr);
		published.scenes.fill(nullptr);
		published.streetMerged = StreetMerge::Merged();
		if (Manager::Space().Merged()) {
			published.rooms[0] = &*rooms[0];
			published.scenes[0] = Manager::Space().Scene();
		}
		for (int slot = 1; slot < ::Render::kMaxRooms; ++slot) {
			if (rooms[slot]) {
				if (auto* space = Selection::KeptSpace(rooms[slot]->beside.cell)) {
					published.rooms[slot] = &*rooms[slot];
					published.scenes[slot] = space->Scene();
				}
			}
		}
	}

	void PublishShownCells(bool a_shown)
	{
		std::scoped_lock lock(published.shownCellsLock);
		published.shownCells.clear();
		if (a_shown) {
			published.shownCells.assign(Manager::Space().Cells().begin(), Manager::Space().Cells().end());
		}
		// Their people are drawn as the shown room's are (ActorCullHook); a paused room's are off
		// screen, as the engine finds them.
		for (int slot = 1; slot < ::Render::kMaxRooms; ++slot) {
			if (!rooms[slot]) {
				continue;
			}
			if (const auto* space = Selection::KeptSpace(rooms[slot]->beside.cell); space && !rooms[slot]->beside.paused) {
				published.shownCells.insert(published.shownCells.end(), space->Cells().begin(), space->Cells().end());
			}
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
			// Lit from its own light scene by the patched Community Shaders, which the engine
			// doesn't cull or fade for this view.
			LightScene::PrepareForView();
		}
		auto* remoteCell = a_ready ? Manager::Space().Cell() : nullptr;
		LightOcclusion::SetWorld(0, remoteCell ? remoteCell->GetbhkWorld() : nullptr, toPhysics, roomDoor, remoteCell && remoteCell->IsInteriorCell());
		// Each room merged beside it in its own Havok world, from its own door.
		for (int slot = 1; slot < ::Render::kMaxRooms; ++slot) {
			auto*      space = rooms[slot] ? Selection::KeptSpace(rooms[slot]->beside.cell) : nullptr;
			const auto mapping = space ? Doors::TryCompute(Active{ rooms[slot]->beside.sourceDoor, rooms[slot]->beside.linkedDoor, rooms[slot]->beside.cell, space->Generation() }) : std::nullopt;
			if (!mapping) {
				LightOcclusion::SetWorld(slot, nullptr);
				continue;
			}
			LightOcclusion::SetWorld(slot, rooms[slot]->beside.cell->GetbhkWorld(), mapping->toLinked,
				LightOcclusion::RoomDoor{ mapping->toLinked * mapping->sourceCenter, mapping->toLinked.rotate * mapping->inward, mapping->halfWidth, mapping->halfHeight }, true);
		}
	}

	void BeforePlayerView()
	{
		for (int slot = 1; slot < ::Render::kMaxRooms; ++slot) {
			if (!rooms[slot]) {
				continue;
			}
			auto& beside = rooms[slot]->beside;
			auto* space = Selection::KeptSpace(beside.cell);
			if (!space) {
				continue;
			}
			// With the camera as this view has it: it moved after the update that paused rooms.
			beside.seen = !pauseUnseen || DoorwaySeen(beside, *space);
			if (beside.paused && beside.seen) {
				// Turned towards it faster than the update noticed: woken for this view already, drawn
				// as it was paused (left out of this frame's cull, so whole), not missing.
				beside.unseen = 0.0f;
				SetBesidePaused(beside, *space, false);
				std::scoped_lock lock(published.shownCellsLock);
				published.shownCells.insert(published.shownCells.end(), space->Cells().begin(), space->Cells().end());
			}
			if (!beside.paused) {
				LightScene::PrepareUnshownForView(space->Scene());
			}
		}
	}

	void RefreshShapes()
	{
		rooms[0]->marked = false;
	}

	std::vector<RE::TESObjectCELL*> MergedCells()
	{
		std::vector<RE::TESObjectCELL*> cells;
		if (Manager::Space().Merged()) {
			cells = Manager::Space().Cells();
		}
		for (int slot = 1; slot < ::Render::kMaxRooms; ++slot) {
			if (!rooms[slot]) {
				continue;
			}
			if (const auto* space = Selection::KeptSpace(rooms[slot]->beside.cell); space && !rooms[slot]->beside.paused) {
				cells.insert(cells.end(), space->Cells().begin(), space->Cells().end());
			}
		}
		return cells;
	}

	int RoomOf(const RE::BSGeometry* a_shape)
	{
		for (int slot = 0; slot < static_cast<int>(published.rooms.size()); ++slot) {
			if (published.rooms[slot] && published.rooms[slot]->shapes.contains(a_shape)) {
				return slot;
			}
		}
		// Water hangs under the water system, not in its cell's 3D, and only the space the player
		// is in has any: with the street merged into an interior it is the street's, with rooms
		// merged into the street the street's too, the player's space. Masked as the player's, the
		// street's canal running under the houses showed in the Ratway's doorway, over the steps
		// down behind it.
		if (published.streetMerged && netimmerse_cast<RE::BSWaterShaderProperty*>(a_shape->GetGeometryRuntimeData().shaderProperty.get())) {
			return 0;
		}
		// The weather's rain and snow too: they follow the camera into the interior, and fall only in
		// the street seen through the doorway.
		if (published.streetMerged) {
			const auto* precip = RE::Sky::GetSingleton()->precip;
			if (a_shape == precip->currentPrecip.get() || a_shape == precip->lastPrecip.get()) {
				return 0;
			}
		}
		return -1;
	}

	RE::ShadowSceneNode* SceneOf(int a_slot)
	{
		return a_slot >= 0 && a_slot < static_cast<int>(published.scenes.size()) ? published.scenes[a_slot] : nullptr;
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
		return rooms[0]->swing.Amount();
	}

	bool ShownShut()
	{
		return rooms[0]->swing.Shut();
	}

	std::vector<std::pair<RE::FormID, RE::FormID>> OpenBesideDoors(float a_open)
	{
		std::vector<std::pair<RE::FormID, RE::FormID>> doors;
		for (int slot = 1; slot < ::Render::kMaxRooms; ++slot) {
			if (rooms[slot] && rooms[slot]->swing.Amount() >= a_open) {
				doors.emplace_back(rooms[slot]->beside.sourceDoor, rooms[slot]->beside.linkedDoor);
			}
		}
		return doors;
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

	std::vector<RoomInfo> Rooms()
	{
		std::vector<RoomInfo> info;
		for (int slot = 0; slot < ::Render::kMaxRooms; ++slot) {
			if (!rooms[slot] || (slot == 0 && !Manager::Space().Merged())) {
				continue;
			}
			const auto& room = *rooms[slot];
			info.push_back({ slot, room.beside.cell, room.door, room.beside.sourceDoor, room.swing.Amount(), room.shapes.size(), room.daylight, room.beside.seen, room.beside.paused });
		}
		return info;
	}

	std::vector<const RE::BSGeometry*> Shapes()
	{
		std::vector<const RE::BSGeometry*> shapes;
		for (const auto* room : published.rooms) {
			if (room) {
				for (const auto* shape : room->shapes) {
					shapes.push_back(shape);
				}
			}
		}
		return shapes;
	}
}
