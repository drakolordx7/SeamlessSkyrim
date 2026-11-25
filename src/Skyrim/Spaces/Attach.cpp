#include "Skyrim/Spaces/RemoteCell.h"

#include "Skyrim/Diag/Probe.h"
#include "Skyrim/Diag/Telemetry.h"
#include "Skyrim/Doors/Hold.h"
#include "Skyrim/Engine/AttachCell.h"
#include "Skyrim/Engine/Fade.h"
#include "Skyrim/Portal/LightScene.h"
#include "Skyrim/Portal/Pristine.h"
#include "Skyrim/Spaces/Tuning.h"
#include "Skyrim/Spaces/WalkOut.h"

namespace
{
	using CellState = RE::TESObjectCELL::CellState;

	// The land's own fade-in: BSLightingShaderProperty + 0x108 of each land quad, which the depth
	// prepass takes as the strength of its dithered alpha mask (ID 107762 + 0xB84, AlphaTestRef.w,
	// for multi-texture landscape). The engine raises it from 0 only for the cells of its own grid.
	// At 0 the prepass dithered the whole quad away and the lighting pass (depth test EQUAL) drew
	// nothing there: through the doorway the distant land showed where the street's ground was.
	void ShowLand(RE::TESObjectCELL* a_cell)
	{
		auto* land = a_cell && a_cell->IsExteriorCell() ? a_cell->GetRuntimeData().cellLand : nullptr;
		auto* data = land ? land->loadedData : nullptr;
		for (int quad = 0; data && quad < 4; ++quad) {
			if (!data->mesh[quad]) {
				continue;
			}
			RE::BSVisit::TraverseScenegraphGeometries(data->mesh[quad], [](RE::BSGeometry* a_shape) {
				if (auto* property = netimmerse_cast<RE::BSLightingShaderProperty*>(a_shape->GetGeometryRuntimeData().shaderProperty.get())) {
					property->unk108 = 1.0f;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}
	}

	// Whether the engine animates a_cell's references itself: only for the interior the player is
	// in, or else for the grid's cells (TES, ID 13325).
	bool AnimatedByEngine(const RE::TESObjectCELL* a_cell)
	{
		auto* tes = RE::TES::GetSingleton();
		if (tes->interiorCell) {
			return a_cell == tes->interiorCell;
		}
		for (std::uint32_t x = 0; x < tes->gridCells->length; ++x) {
			for (std::uint32_t y = 0; y < tes->gridCells->length; ++y) {
				if (tes->gridCells->GetCell(x, y) == a_cell) {
					return true;
				}
			}
		}
		return false;
	}

	// What the engine does each frame for the cells the player is in, over the cell's animated
	// references (LOADED_CELL_DATA +0x40): their animation graphs (ID 18900), and a selective update
	// of their 3D by the frame's time, which runs their controllers and so their particle systems.
	// Without it the fire in Understone Keep's braziers stood still behind the doorway (SE) or never
	// started (AE). The engine's per-reference update (ID 18894) isn't called for that: it skipped
	// these references (it also checks the 3D's room against the player's visible rooms).
	void AnimateReferences(RE::TESObjectCELL* a_cell)
	{
		if (AnimatedByEngine(a_cell)) {
			return;
		}
		static REL::Relocation<void (*)(RE::TESObjectCELL*)> animate{ REL::RelocationID(18469, 18900) };
		static REL::Relocation<const float*>                 frameTime{ REL::RelocationID(514188, 400333) };
		animate(a_cell);
		for (const auto& handle : a_cell->GetRuntimeData().loadedData->unk040) {
			const auto ref = handle.get();
			if (auto* object = ref ? ref->Get3D2() : nullptr) {
				// Anew for each, with ID 18894's flags: the update writes flags back into it.
				RE::NiUpdateData data{ *frameTime, RE::NiUpdateData::Flag::kNone };
				reinterpret_cast<std::uint32_t&>(data.flags) = 2;
				object->UpdateSelectedDownwardPass(data, 0);
			}
		}
	}
}

namespace Portal
{
	std::vector<RE::Actor*> RemoteCell::ActorsIn(const RE::TESObjectCELL* a_cell)
	{
		std::vector<RE::Actor*> actors;
		a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
			auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
			if (actor && !actor->IsPlayerRef()) {
				actors.push_back(actor);
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		return actors;
	}

	RE::NiAVObject* RemoteCell::Cell3D(const RE::TESObjectCELL* a_cell)
	{
		auto* loaded = a_cell ? a_cell->GetRuntimeData().loadedData : nullptr;
		return loaded ? loaded->cell3D.get() : nullptr;
	}

	RE::NiAVObject* RemoteCell::Cell3D() const
	{
		return Cell3D(Cell());
	}

	bool RemoteCell::Attachable(const RE::TESObjectCELL* a_cell)
	{
		// Attached/Attaching by the engine means it's (becoming) a current cell; never steal it.
		const auto state = a_cell ? a_cell->cellState.get() : CellState::kAttached;
		return a_cell && a_cell != RE::PlayerCharacter::GetSingleton()->GetParentCell() && state != CellState::kAttached && state != CellState::kAttaching &&
		       state != CellState::kDetaching;
	}

	RemoteCell::PersistentByCell RemoteCell::PersistentIn(const std::vector<RE::TESObjectCELL*>& a_cells, std::unordered_set<RE::FormID>& a_queued, int& a_newlyQueued)
	{
		PersistentByCell found;
		auto*            first = a_cells.empty() ? nullptr : a_cells.front();
		auto*            world = first && !first->IsInteriorCell() ? first->GetRuntimeData().worldSpace : nullptr;
		if (!world || !world->persistentCell) {
			return found;
		}
		const auto key = [](std::int32_t a_x, std::int32_t a_y) { return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_x)) << 32) | static_cast<std::uint32_t>(a_y); };
		std::unordered_map<std::uint64_t, const RE::TESObjectCELL*> byCoordinates;
		for (auto* cell : a_cells) {
			if (auto* coordinates = cell->IsInteriorCell() ? nullptr : cell->GetCoordinates()) {
				byCoordinates.emplace(key(coordinates->cellX, coordinates->cellY), cell);
			}
		}
		// The engine's model loader, as TESObjectREFR::Enable (ID 19800) queues a reference's 3D:
		// read on its loader threads, set on the main thread when done.
		static REL::Relocation<void**>                                                     modelLoader{ REL::RelocationID(514183, 400332) };
		static REL::Relocation<void (*)(void*, RE::TESObjectREFR*, std::uint32_t, bool)> queueReference{ REL::RelocationID(12910, 13057) };
		static REL::Relocation<std::uint32_t (*)(RE::TES*, RE::TESObjectCELL*, std::uint32_t)> loadPriority{ REL::RelocationID(13176, 13321) };
		int queued = 0, loadedHere = 0;
		world->persistentCell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
			if (!a_ref) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			const auto pos = a_ref->GetPosition();
			const auto cell = byCoordinates.find(key(static_cast<std::int32_t>(std::floor(pos.x / 4096.0f)), static_cast<std::int32_t>(std::floor(pos.y / 4096.0f))));
			if (cell == byCoordinates.end()) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			// A game loaded indoors after being outdoors in the same session leaves them without 3D
			// (a fresh start doesn't): Belethor's house with its porch, the paving of Whiterun's
			// market were missing from the view out, and with no collision under the door the
			// player walking out was put 135 units down in the stream below the porch. Queued here
			// for the engine's loader: loaded on the main thread, 137 of them around Whiterun's gate
			// stopped the game for 0.8 s. The queue counts each in its cell's queued references
			// (ID 19151), which the readiness in Update waits for, and the loader counts it off when
			// it is done, with 3D or without (a model it can't load).
			// Not doors: merged with the street, a door's 3D stands where the interior shows it, and
			// the doorway mapped from it sent the player walking out of Belethor's to the far side
			// of Whiterun.
			if (!a_ref->Get3D()) {
				auto* base = a_ref->GetBaseObject();
				auto* model = base ? base->As<RE::TESModel>() : nullptr;
				if (!model || !model->GetModel() || !model->GetModel()[0] || base->Is(RE::FormType::Door) || a_ref->As<RE::Actor>() || a_ref->IsDisabled() || a_ref->IsDeleted() ||
					static_cast<RE::TESObject*>(base)->IsMarker()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				// Once per attach: queued again at every look, one the loader finished without 3D would
				// be counted in its cell anew each time, and the space never counted as loaded.
				// A reference still filed under a cell without loaded data (the worldspace's persistent
				// cell) has nowhere to be counted (ID 19151 crashed on it): loaded here.
				auto* parent = a_ref->GetParentCell();
				if (parent && parent->GetRuntimeData().loadedData) {
					if (a_queued.insert(a_ref->GetFormID()).second) {
						queueReference(*modelLoader, a_ref, loadPriority(RE::TES::GetSingleton(), parent, 0), false);
						++queued;
					}
					return RE::BSContainer::ForEachResult::kContinue;
				}
				a_ref->Load3D(false);
				if (!a_ref->Get3D()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				++loadedHere;
			}
			found[cell->second].push_back(a_ref->GetFormID());
			return RE::BSContainer::ForEachResult::kContinue;
		});
		a_newlyQueued = queued;
		if (loadedHere) {
			logger::info("portal: 3D of {} persistent reference(s) without a loaded cell loaded at once", loadedHere);
		}
		return found;
	}

	int RemoteCell::AdoptPersistent(RE::TESObjectCELL* a_cell, const PersistentByCell& a_persistent)
	{
		// Outdoors, persistent references (a house, its porch: Riverwood's Trader) belong to the
		// worldspace's persistent cell; the grid attaches their 3D to the cell they stand in. When the
		// player goes inside, the engine takes them off it (some frames after the cell change), and
		// the cell alone left a hole where the building was. Their 3D still exists, unparented:
		// hang it under the cell again. Repeated from Update for the ones taken off later.
		auto* cell3D = Cell3D(a_cell) ? Cell3D(a_cell)->AsNode() : nullptr;
		const auto refs = a_persistent.find(a_cell);
		if (!cell3D || refs == a_persistent.end()) {
			return 0;
		}
		auto* root = scene.get();
		auto* havokWorld = a_cell->GetbhkWorld();
		int   restored = 0;
		int   hung = 0;
		for (const auto id : refs->second) {
			auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
			auto* object = ref ? ref->Get3D() : nullptr;
			if (!object) {
				continue;
			}
			// A town's people are persistent: not in the cell's own list, so not raised to the high
			// process with the cell's actors (AttachOne). Left in the low process the engine gives
			// them while the player is indoors, they moved in a jump every few seconds and didn't
			// animate, seen through the doorway.
			if (auto* actor = ref->As<RE::Actor>(); actor && Spaces::tuning.stepExterior && !actor->IsDead()) {
				auto* process = actor->GetActorRuntimeData().currentProcess;
				if (!process || !process->InHighProcess()) {
					actor->MoveToHigh();
				}
			}
			// The 3D may hang in a node the engine took off the scene as a whole (the persistent
			// cell's own); that node is what goes under the portal root.
			RE::NiAVObject* top = object;
			while (top->parent) {
				top = top->parent;
			}
			if (top != root && top->AsNode() != RE::Main::WorldRootNode()) {
				root->AttachChild(top, true);
				borrowedPersistent.emplace_back(top);
				++hung;
			}
			// Their collision went with them: the engine took their bodies out of the worldspace's
			// Havok world too. Whiterun's market street is one (WRMainRoadMarket): simulated from
			// indoors, the people walking on it sank through it to the land 100 units below, and
			// were seen up to their heads in the ground, also after walking out. Put back in.
			// Not a door held open passable, whose bodies were taken out on purpose (a city gate's).
			if (havokWorld && !ref->As<RE::Actor>() && !Portal::Doors::BodiesHidden(ref->GetFormID())) {
				RE::BSVisit::TraverseScenegraphObjects(object, [&](RE::NiAVObject* a_object) {
					auto* collision = a_object->collisionObject ? netimmerse_cast<RE::bhkNiCollisionObject*>(a_object->collisionObject.get()) : nullptr;
					auto* body = collision ? collision->body.get() : nullptr;
					// (Nor one kept out of the gate's passage, ClearPassage.)
					if (body && !body->GetWorld1() && std::ranges::none_of(passageCleared, [&](const auto& a_cleared) { return a_cleared.get() == body; })) {
						body->MoveToWorld(havokWorld);
						++restored;
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
		}
		if (restored) {
			logger::info("portal: {} collision object(s) of persistent references put back into {:08X}'s Havok world", restored, a_cell->GetFormID());
		}
		return hung;
	}


	void RemoteCell::AttachOne(RE::TESObjectCELL* a_cell)
	{
		// The engine's attach installs the cell's room/portal occlusion graph in the *world* light
		// scene (ShadowSceneNode::SetPortalGraph on State slot 0). Left there, the main view culled
		// with the remote cell's rooms: actors outside them (the player, every NPC) vanished, and the
		// main view accumulated the remote cell's geometry too, colliding with the portal pass's
		// render passes and crashing. AttachGraphHook (EngineHooks.cpp) keeps the engine from
		// installing it. Setting the world's graph again afterwards is what undid it before the hook
		// existed, and still does where the hook's call site isn't found; with the hook it is the
		// graph the scene has, which drops nothing (only a different one does). Deliberately no
		// graph on the portal scene: a light scene with a graph expects the culling process to
		// carry a portal-graph entry (ID 106322), which our cull context doesn't, and it crashed;
		// without one the remote cell's rooms render fine.
		auto* worldScene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		auto* worldGraph = worldScene ? worldScene->GetRuntimeData().portalGraph : nullptr;
		Engine::AttachCell(a_cell, scene.get());
		// The land's meshes are built only when the engine loads a cell into its own grid (IDs 13285
		// and 13427 call this right after 19063), which a remote cell never goes through: a street
		// read from disk while the player is indoors (a game loaded in a room) had no ground through
		// the doorway until the player had been outside once. A land already built is left as it is.
		// Not GetLand(): it makes an empty land for a cell whose own is not loaded, built flat at
		// the engine's "no data" height of -27000 units.
		auto* land = a_cell->IsExteriorCell() ? a_cell->GetRuntimeData().cellLand : nullptr;
		if (land && land->data.flags.any(RE::OBJ_LAND::Flag::kVertexNormals_HeightMap, RE::OBJ_LAND::Flag::kVertexColors, RE::OBJ_LAND::Flag::kLayers, RE::OBJ_LAND::Flag::kMPCD)) {
			land->LoadVertices(true, true);
		}
		ShowLand(a_cell);
		if (worldScene) {
			worldScene->SetPortalGraph(worldGraph);
		}
		cells.push_back(a_cell);
		held.push_back(a_cell);

		// High process so the engine runs full AI, animation and movement for its NPCs; it does
		// not demote them again while the cell stays attached.
		for (auto* actor : ActorsIn(a_cell)) {
			if (!actor->IsDead()) {
				actor->MoveToHigh();
			}
		}
		// A light ref creates its light when its 3D loads. In a cell that is still loaded from an
		// earlier visit nothing loads, and the lights the engine removed when the player left were
		// never made again: the portal showed it unlit. TES::SetCurrentCell recreates them with
		// ID 19059 (cell, true), which runs ID 19678 (CreatePointLight) for each reference with 3D;
		// they land in the world scene and are adopted like any other.
		REL::Relocation<void (*)(RE::TESObjectCELL*, bool)> attachRefLights{ REL::RelocationID(18590, 19059) };
		attachRefLights(a_cell, true);
		adoptedLights += LightScene::Adopt(scene.get(), Cell3D(a_cell));
	}

	bool RemoteCell::Attach(RE::TESObjectCELL* a_cell, bool a_toMerge, bool a_show)
	{
		if (!cells.empty() || !Attachable(a_cell)) {
			return false;
		}
		scene = LightScene::Create();
		if (a_show) {
			LightScene::Show(scene.get());
		}
		generation = ++lastGeneration;
		adoptedLights = 0;
		persistentQueued.clear();
		persistentCheck = 0.0f;
		roundNext = 0;
		roundPersistent.clear();
		Spaces::WalkOut::Forget();
		handedPersistent.clear();
		// A cell still loaded from the player's visit (they just came through the door) has nothing
		// left to build; the long wait is for cells whose 3D is being loaded now.
		bool preloaded = Cell3D(a_cell) != nullptr;
		// Attaching a cell, and fetching an exterior's neighbours, emptied the world scene of its
		// lights (25 -> 0 in a lit room: seen from inside, every candle and fire went dark for
		// good, since nothing makes them again while the cell stays loaded), through the change of
		// its room graph that AttachGraphHook now prevents. Whatever still goes is put back.
		const auto worldLights = LightScene::WorldLights();
		// The door's cell can be in memory without having been read from the plugin files: a game
		// loaded indoors holds the exterior cells of persistent references (the door) as shells, with
		// no land and no temporary references. Read like its neighbours below (a no-op when it was).
		if (!a_cell->IsInteriorCell()) {
			auto* world = a_cell->GetRuntimeData().worldSpace;
			auto* coordinates = a_cell->GetCoordinates();
			if (world && coordinates && world->LoadCell(static_cast<std::int16_t>(coordinates->cellX), static_cast<std::int16_t>(coordinates->cellY)) != a_cell) {
				logger::warn("portal: {} is not the worldspace's cell at its coordinates", Probe::DescribeCell(a_cell));
			}
		}
		AttachOne(a_cell);
		// Outdoors the doorway looks out over more than the one cell its door is in: an exterior
		// cell's land and objects only reach to its border, so the neighbours are attached too.
		if (!a_cell->IsInteriorCell()) {
			auto* world = a_cell->GetRuntimeData().worldSpace;
			auto* coordinates = a_cell->GetCoordinates();
			if (world && coordinates) {
				for (int dy = -kNeighbourRadius; dy <= kNeighbourRadius; ++dy) {
					for (int dx = -kNeighbourRadius; dx <= kNeighbourRadius; ++dx) {
						if (!dx && !dy) {
							continue;
						}
						// Loaded from disk if needed (a game loaded indoors); the 3D then loads like
						// the door's own cell's, and the portal waits for all of them.
						auto* neighbour = world->LoadCell(static_cast<std::int16_t>(coordinates->cellX + dx), static_cast<std::int16_t>(coordinates->cellY + dy));
						if (neighbour && Attachable(neighbour)) {
							preloaded &= Cell3D(neighbour) != nullptr;
							AttachOne(neighbour);
						}
					}
				}
			}
		}
		prewarmCells.clear();
		prewarmLand = nullptr;
		worldMapClouds.reset();
		handedWorldMapClouds.reset();
		prewarmModel = a_toMerge && !a_cell->IsInteriorCell();
		if (prewarmModel) {
			const auto* coordinates = a_cell->GetCoordinates();
			const int   radius = static_cast<int>(RE::GetINISetting("uGridsToLoad:General")->data.u) / 2;
			for (int dy = -radius; dy <= radius; ++dy) {
				for (int dx = -radius; dx <= radius; ++dx) {
					prewarmCells.emplace_back(static_cast<std::int16_t>(coordinates->cellX + dx), static_cast<std::int16_t>(coordinates->cellY + dy));
				}
			}
		}
		// All the cells' persistent references in one pass over the worldspace's persistent cell
		// (thousands of references in a city): done per cell, it was most of an attach.
		{
			const Diag::Scope scope("attach persistent");
			int               newlyQueued = 0;
			const auto        persistent = PersistentIn(cells, persistentQueued, newlyQueued);
			if (newlyQueued) {
				logger::info("portal: 3D of {} persistent reference(s) queued for loading", newlyQueued);
			}
			for (auto* cell : cells) {
				// Their lights (a lantern on a house) come with them.
				if (AdoptPersistent(cell, persistent)) {
					adoptedLights += LightScene::Adopt(scene.get(), Cell3D(cell));
				}
			}
		}
		if (auto* worldScene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]) {
			const auto kept = LightScene::WorldLights();
			// A light of the attached cells left the world scene because it was adopted into the
			// portal's (a cell the player left a moment ago can still have its lights there); put
			// back, it would be registered in both.
			const auto ofRemote = [&](const RE::NiPointer<RE::BSLight>& a_light) {
				for (const RE::NiAVObject* node = a_light->light.get(); node; node = node->parent) {
					if (std::ranges::any_of(cells, [&](RE::TESObjectCELL* a_cell) { return Cell3D(a_cell) == node; })) {
						return true;
					}
				}
				return false;
			};
			int restored = 0;
			for (const auto& light : worldLights) {
				if (std::ranges::find(kept, light) == kept.end() && !ofRemote(light)) {
					worldScene->AddLight(light.get());
					++restored;
				}
			}
			if (restored) {
				logger::info("portal: {} world light(s) put back after the attach", restored);
			}
		}
		// An interior is merged and must be there in the frame of arrival (the camera is standing in
		// it). An exterior goes through the portal pass, which has drawn stale render passes of
		// cells attached in the very frame the player left them: it waits a moment. Not so a street
		// merged into the interior just walked into: until it was, the room was lit without the
		// street's houses around it (Warmaiden's with the daylight of the lights outside its door
		// on the floor), and changed half a second after walking in.
		quietRequired = !preloaded ? kQuietSecondsRequired : a_cell->IsInteriorCell() || a_toMerge ? kQuietSecondsPreloaded : Spaces::tuning.quietSecondsLeftCell;
		steppedFrames = 0;
		quietSeconds = 0.0f;
		// Per remote space: carried over, a queue the previous cell left stuck at the same count
		// made the next one count as loaded before it had loaded anything.
		lastQueued = 0;
		stuckSeconds = 0.0f;
		ready = false;
		readySeconds = 0.0f;
		itemsChecked = false;
		groundFound = false;
		groundCheck = 0.0f;
		logger::info("portal: attached {} (+{} neighbouring cells, {} lights)", Probe::DescribeCell(a_cell), cells.size() - 1, adoptedLights);
		return true;
	}


	bool RemoteCell::Healthy() const
	{
		if (merged) {
			return !cells.empty();
		}
		auto* root = scene.get();
		return root && !cells.empty() && std::ranges::all_of(cells, [&](RE::TESObjectCELL* a_cell) {
			auto* cell3D = Cell3D(a_cell);
			return a_cell->cellState.get() == CellState::kAttached && cell3D && cell3D->parent == root;
		});
	}

	void RemoteCell::SetParked(bool a_parked)
	{
		if (!a_parked) {
			for (auto& object : parked) {
				object->SetAppCulled(false);
			}
			parked.clear();
			return;
		}
		if (!merged || !parked.empty()) {
			return;
		}
		for (auto* cell : cells) {
			// Only what isn't hidden already, so that showing it again undoes nothing of the engine's.
			if (auto* cell3D = Cell3D(cell); cell3D && !cell3D->GetAppCulled()) {
				cell3D->SetAppCulled(true);
				parked.emplace_back(cell3D);
			}
		}
	}

	void RemoteCell::HangAlwaysRender(RE::TESObjectCELL* a_cell)
	{
		auto* loaded = a_cell->GetRuntimeData().loadedData;
		auto* graph = loaded ? loaded->portalGraph.get() : nullptr;
		auto* cell3D = Cell3D(a_cell) ? Cell3D(a_cell)->AsNode() : nullptr;
		if (!graph || !cell3D) {
			return;
		}
		auto holder = std::ranges::find_if(alwaysRenderHolders, [&](const auto& a_holder) { return a_holder->parent == cell3D; });
		int  hung = 0;
		// Also those still waiting to be taken in: the graph takes in 3D finished after the attach
		// only while it is the scene's (Mistveil Keep's steps, loaded a moment late).
		std::vector<RE::NiAVObject*> objects;
		for (const auto* list : { &graph->alwaysRenderChildren, &graph->pendingAlwaysRenderAdds }) {
			for (const auto& object : *list) {
				objects.push_back(object.get());
			}
		}
		for (const auto& removed : graph->pendingAlwaysRenderRemoves) {
			std::erase(objects, removed.get());
		}
		for (auto* object : objects) {
			if (!object || object->parent) {
				continue;
			}
			if (holder == alwaysRenderHolders.end()) {
				RE::NiPointer<RE::NiNode> node(RE::NiNode::Create());
				node->name = "SeamlessAlwaysRender";
				// At the end: the main view's cull goes by the cell node's slots and leaves some
				// out (ID 36643), a free slot among them.
				cell3D->AttachChild(node.get(), false);
				alwaysRenderHolders.push_back(node);
				holder = std::prev(alwaysRenderHolders.end());
			}
			(*holder)->AttachChild(object, true);
			++hung;
		}
		if (hung) {
			RE::NiUpdateData update;
			(*holder)->Update(update);
			logger::info("portal: {} always-drawn object(s) of {} hung into its 3D", hung, Probe::DescribeCell(a_cell));
		}
	}

	void RemoteCell::UnhangAlwaysRender()
	{
		// Back as the engine keeps them, unparented, where they stand in their cell.
		for (auto& holder : alwaysRenderHolders) {
			std::vector<RE::NiPointer<RE::NiAVObject>> children;
			for (auto& child : holder->GetChildren()) {
				if (child) {
					children.push_back(child);
				}
			}
			for (auto& child : children) {
				holder->DetachChild(child.get());
				RE::NiUpdateData update;
				child->Update(update);
			}
			if (holder->parent) {
				holder->parent->DetachChild(holder.get());
			}
		}
		alwaysRenderHolders.clear();
	}

	void RemoteCell::Release(bool a_handOver)
	{
		if (cells.empty()) {
			return;
		}
		// Hidden, the cell would stay so once the engine has it again (the player walking in).
		SetParked(false);
		// Before the cells go: handed over, they are the player's own again.
		RestorePassage();
		UnhangAlwaysRender();
		auto* tes = RE::TES::GetSingleton();
		auto* engineRoot = tes ? tes->objRoot : nullptr;
		// A merged street (merged into the interior being left) is put back under the portal root,
		// unmoved, below and then handed over like any other.
		const bool handOver = a_handOver && Spaces::tuning.handOverStreet && engineRoot && !cells.front()->IsInteriorCell();
		// The step job may be running right now; it must be done with this world first.
		StepWorld(nullptr);
		auto* root = scene.get();
		auto* here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		if (std::exchange(merged, false)) {
			const Diag::Scope scope("release: unmerge cells");
			// Back under the portal root, unmoved, so the normal release below applies.
			for (auto* cell : cells) {
				RE::NiPointer<RE::NiAVObject> cell3D(Cell3D(cell));
				if (cell3D && cell3D->parent && cell3D->parent != root && cell != here) {
					cell3D->parent->DetachChild(cell3D.get());
					cell3D->local = RE::NiTransform();
					root->AttachChild(cell3D.get(), true);
					RE::NiUpdateData update;
					cell3D->Update(update);
					RecomputeBounds(cell3D.get());
					SettleMotion(cell3D.get());
				}
			}
		}
		// Only once the cells are back in place. Driven by their bodies again before the move back,
		// the update above read each loose item's body where the merge had put it, thousands of units
		// below the room's floor, and the engine took it for fallen out of the world (ID 19826 ->
		// ID 19799): an item dropped in the room was deleted, its form ID cleared, on walking out
		// and back in.
		for (auto& [object, flags] : stilledClutter) {
			if (auto* collision = object->collisionObject ? netimmerse_cast<RE::bhkNiCollisionObject*>(object->collisionObject.get()) : nullptr) {
				*reinterpret_cast<std::uint32_t*>(&collision->flags) = flags;
				Pristine::FlagsBack(collision);
			}
		}
		stilledClutter.clear();
		RestoreBounds();
		// Our lights are copies of the cells' light refs; entering a cell normally re-creates them
		// in the world scene, so they must not stay registered here too.
		LightScene::Clear(scene.get());
		// Persistent references' 3D goes back to how the engine left it (unparented); the grid
		// attaches it again when the player comes back out. Only if it is still ours: if the
		// engine has hung it into the world again meanwhile (the player came back out another
		// way), taking it off its parent would leave a hole where the building is.
		handedPersistent.clear();
		for (auto& object : borrowedPersistent) {
			if (object->parent == root) {
				root->DetachChild(object.get());
			}
			// A reference's own 3D, not a node of the engine's that holds several: one we loaded
			// (PersistentIn), which the engine's attach of its cell on walking out leaves where it is.
			// Not only when handed over: let go first and then walked out the engine's way (the door
			// walked into before the street was there, or shut and opened again quickly), Riften's
			// canal walk and houses outside Beggar's Row were gone and the player fell into the canal.
			if (auto* ref = RE::TESObjectREFR::FindReferenceFor3D(object.get()); ref && ref->Get3D() == object.get()) {
				handedPersistent.push_back(ref->GetFormID());
			}
		}
		borrowedPersistent.clear();
		Spaces::WalkOut::Forget();
		if (handOver) {
			handedWorldMapClouds = std::move(worldMapClouds);
		}
		worldMapClouds.reset();
		const Diag::Scope detachScope("release: detach");
		for (auto* cell : cells | std::views::reverse) {
			auto* cell3D = Cell3D(cell);
			if (handOver && cell3D && cell3D->parent == root && cell->cellState.get() == CellState::kAttached) {
				RE::NiPointer<RE::NiAVObject> keep(cell3D);
				root->DetachChild(cell3D);
				engineRoot->AttachChild(cell3D, true);
				Spaces::WalkOut::Hand(cell);
			} else if (cell3D && cell3D->parent == root && cell != here) {
				// Off our root *before* the engine detach: detaching with cell3D still under our root
				// crashed seconds later (S2f). If the engine has meanwhile re-parented the cell (the
				// player walked in normally), it owns it now and we only forget it.
				root->DetachChild(cell3D);
				Engine::DetachCell(cell);
			} else {
				logger::info("portal: forgot {} (taken over by the engine)", Probe::DescribeCell(cell));
			}
		}
		for (auto* cell : cells) {
			std::erase(held, cell);
		}
		logger::info("portal: released {} (+{} neighbouring cells, {} handed over attached)", Probe::DescribeCell(cells.front()), cells.size() - 1, Spaces::WalkOut::HandedOver().size());
		cells.clear();
		roundNext = 0;
		roundPersistent.clear();
		fadeForced = false;
		adoptedLights = 0;
		LightScene::Retire(std::move(scene));
	}

	void RemoteCell::HangHandedPersistent()
	{
		// Loaded by us while the player was indoors (a game loaded in a room after being outdoors in
		// the same session), their 3D hung under the street. Handed over, it hung nowhere: the
		// engine attaching their cell on the way out hangs only the 3D it loads itself, and Markarth's
		// ground and rocks outside the Smelter Overseer's House were gone, only their collision left.
		// ID 19165 is what the engine's cell attach (ID 18976, through ID 19187) puts each one in with.
		REL::Relocation<void (*)(RE::TESObjectCELL*, RE::TESObjectREFR*, bool, bool)> attachRef3D{ REL::RelocationID(18683, 19165) };
		int hung = 0;
		std::erase_if(handedPersistent, [&](RE::FormID a_id) {
			auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_id);
			auto* object = ref ? ref->Get3D() : nullptr;
			if (!object || object->parent) {
				return true;
			}
			// Kept for when the engine has attached it (after its loading screen, some frames on).
			// (ID 19165 crashed on a cell without 3D.)
			auto* cell = ref->GetParentCell();
			if (!Cell3D(cell) || cell->cellState.get() != CellState::kAttached) {
				return false;
			}
			attachRef3D(cell, ref, true, false);
			++hung;
			return true;
		});
		if (hung) {
			logger::info("portal: {} persistent reference(s) hung into their cells after walking out, {} waiting for their cell", hung, handedPersistent.size());
		}
	}

	void RemoteCell::Update(float a_delta)
	{
		const Diag::Scope scope("remote update");
		if (!Healthy()) {
			StepWorld(nullptr);
			return;
		}
		// Readiness. refsFullyLoaded is useless here: the engine sets it (ID 19146, which also fires
		// TESCellFullyLoadedEvent) only from its per-frame update of *current* cells (ID 18895). The
		// load queues alone aren't enough either: right after the attach they are still 0 because
		// nothing has been queued yet, and drawing then crashed on half-built geometry. So require
		// them to stay empty for a while.
		// The persistent references queued for their 3D (PersistentIn) are counted in their cells too.
		bool quiet = std::ranges::all_of(cells, [](RE::TESObjectCELL* a_cell) {
			auto* loaded = a_cell->GetRuntimeData().loadedData;
			return loaded && loaded->queuedRefCount == 0 && loaded->criticalQueuedRefCount == 0;
		});
		// A reference can stay queued for good in a cell the player isn't in (seen with actors of
		// a random encounter standing at Whiterun's gate: no 3D, ever); the doorway then never
		// opened. Queues that haven't moved for a while count as done.
		int queued = 0;
		for (auto* cell : cells) {
			if (auto* loaded = cell->GetRuntimeData().loadedData) {
				queued += static_cast<int>(loaded->queuedRefCount) + static_cast<int>(loaded->criticalQueuedRefCount);
			} else {
				queued = -1;
				break;
			}
		}
		stuckSeconds = queued > 0 && queued == lastQueued ? stuckSeconds + a_delta : 0.0f;
		lastQueued = queued;
		quiet |= stuckSeconds > 5.0f;
		quietSeconds = quiet ? quietSeconds + a_delta : 0.0f;
		// Ready once, ready until released. References loading later (someone walking into the
		// street) are drawn once the engine has attached their finished 3D; counting them as "not
		// loaded" again blanked the whole doorway until the queues had been quiet for a while.
		if (!ready && quietSeconds >= quietRequired) {
			ready = true;
			logger::info("portal: remote cell ready");
		}
		if (ready && (readySeconds += a_delta) > 2.0f && !std::exchange(itemsChecked, true)) {
			CheckLooseItems();
		}
		int late = 0;
		// Every quarter of a second each cell is looked over (Check, below): one cell per frame,
		// a round at a time. All nine of a street in one frame took ~2 ms, four times a second,
		// felt as judder. While the space is still loading every cell is looked over every frame.
		const bool loading = quietSeconds < 3.0f;
		fadeForced = loading;
		// Nobody sees it: lights animated and a world stepped for no one were most of what a kept
		// room cost each frame. Still loading, it carries on, or it would be found half-built when
		// woken. Lights created meanwhile are taken in once it is woken.
		if (paused && !loading) {
			StepWorld(nullptr);
			return;
		}
		if (roundNext == 0 && (persistentCheck -= a_delta) <= 0.0f) {
			persistentCheck = 0.25f;
			roundNext = cells.size();
			int newlyQueued = 0;
			roundPersistent = PersistentIn(cells, persistentQueued, newlyQueued);
		}
		RE::TESObjectCELL* checked = nullptr;
		if (roundNext > 0) {
			checked = cells[cells.size() - roundNext--];
			AdoptPersistent(checked, roundPersistent);
		}
		for (auto* cell : cells) {
			// The engine fades references in only in the cells the player is in; in a remote cell
			// freshly loaded 3D would stay invisible (after loading a game indoors, most of the
			// village outside the door was missing).
			if (cell == checked || loading) {
				if (cell->IsInteriorCell() && cell != RE::PlayerCharacter::GetSingleton()->GetParentCell()) {
					HangAlwaysRender(cell);
				}
				if (auto* cell3D = Cell3D(cell)) {
					Engine::ForceFadeIn(cell3D);
				}
				if (!cell->IsInteriorCell()) {
					StillLooseObjects(cell);
					ShowLand(cell);
				}
			}
			AnimateReferences(cell);
			// A merged street's lights were adopted before the merge. In the world scene the engine
			// registers one of them anew every frame; taking each into ours, the world scene's queued
			// removal of the one before crashed (ID 106335 -> 106342).
			if (!(merged && !cell->IsInteriorCell())) {
				late += LightScene::Adopt(scene.get(), Cell3D(cell));
			}
			// A street merged into an interior hangs in the world scene: the light update there queued
			// a new light in the world's light scene every frame, which was adopted here, and the
			// world scene's queued removal of the one before crashed (ID 106335 -> 106342).
			if (merged && !cell->IsInteriorCell()) {
				continue;
			}
			// The engine only animates the lights of references in current cells; a light that
			// flickers or pulses (most of a Lux interior) starts with a negative fade as "not
			// updated yet" and stays dark until its reference's light update runs.
			cell->ForEachReference([](RE::TESObjectREFR* a_ref) {
				auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
				if (base && base->Is(RE::FormType::Light) && a_ref->Get3D()) {
					a_ref->UpdateRefLight();
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		if (late) {
			adoptedLights += late;
			logger::info("portal: adopted {} late light(s), {} in total", late, adoptedLights);
		}
		// The Havok step itself happens in the engine's step job (see StepHook) or at the end of
		// the frame; this only says which world to step. An exterior cell's world is the whole
		// worldspace's, which the engine leaves alone while the player is indoors (see Spaces::Tuning::stepExterior).
		auto* primary = cells.front();
		// The street seen from indoors is only simulated once there is ground under its NPCs and
		// loose items: stepped from the moment it was attached, they fell through it (people
		// vanishing, weapons and food missing from the street). Looked for twice a second until found.
		if (!primary->IsInteriorCell() && !groundFound && ready && (groundCheck -= a_delta) <= 0.0f) {
			groundCheck = 0.5f;
			groundFound = GroundUnderStreet();
			if (groundFound) {
				logger::info("portal: ground under the street; simulating it");
			}
		}
		// Not in the frames around the merge, which are long already.
		if (ready && readySeconds > 1.0f && (prewarmModel || prewarmLand || !prewarmCells.empty())) {
			Prewarm();
		}
		StepWorld(stepEnabled && (primary->IsInteriorCell() || (Spaces::tuning.stepExterior && groundFound)) ? primary->GetbhkWorld() : nullptr);
	}

	void RemoteCell::Prewarm()
	{
		const Diag::Scope scope("prewarm");
		auto* world = cells.front()->GetRuntimeData().worldSpace;
		// The change into an exterior loads the world map's clouds (ID 53142 + 0x272: Demand into a
		// global emptied indoors), ~25 ms from disk on the first walk out. Held here, the engine's
		// demand finds the model in the database. The path as ID 20495 picks it.
		if (std::exchange(prewarmModel, false)) {
			const char* path = R"(Meshes\Sky\SkyrimWorldMapCloudBank.nif)";
			if (auto* parent = world->parentWorld; parent && world->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseMapData) && !parent->model.empty()) {
				path = parent->model.c_str();
			} else if (!world->model.empty()) {
				path = world->model.c_str();
			}
			RE::BSModelDB::Demand(path, worldMapClouds, {});
			return;
		}
		// The engine's grid set up around the arrival cell (ID 13317 -> 13430 -> 13427 per cell)
		// reads each cell not yet in memory from the plugin files, attaches it, and loads and
		// builds every cell's land: ~110 ms on the first walk out of a shop after loading a game
		// (a city worldspace's cells take their land from the parent's, which AttachOne leaves
		// unloaded). Done here beforehand, the cell in one frame and its land in the next: 1-13 ms
		// each, depending on the disk. A cell beyond the street's is attached and detached again,
		// as one the player walked past: the engine keeps it loaded (kUnattached) and only
		// attaches it again. Kept attached and handed over, drawn merged into the interior they
		// cost ~7 ms a frame (Belethor's); only read from the files, never attached, the AI update
		// crashed on them (ID 36031, also after loading a game).
		if (auto* cell = std::exchange(prewarmLand, nullptr)) {
			// As 13427 does: GetLand makes the cell's land (from the parent worldspace's, or an
			// empty one where there is none), and LoadVertices leaves a land already built as it is.
			cell->GetLand()->LoadVertices(true, true);
			return;
		}
		const auto [x, y] = prewarmCells.back();
		prewarmCells.pop_back();
		auto* cell = world->LoadCell(x, y);
		if (!cell) {
			return;  // beyond the worldspace's edge
		}
		const auto state = cell->cellState.get();
		if (state == CellState::kUnloaded || state == CellState::kDataLoaded) {
			Engine::AttachCell(cell, scene.get());
			// Off our root before the engine's detach, as in Release.
			scene->DetachChild(Cell3D(cell));
			Engine::DetachCell(cell);
		}
		prewarmLand = cell;
	}

	void RemoteCell::Show()
	{
		// Lights adopted while the scene wasn't registered wait in its add queue; registered, the
		// engine takes them in on its next light update. (Their light-geometry pairs stay empty,
		// unlike a space attached shown; merged rooms are lit by distance in the CS patch, and
		// looked the same.)
		LightScene::Show(scene.get());
	}

	void RemoteCell::StillLooseObjects(RE::TESObjectCELL* a_cell)
	{
		int stilled = 0, replaced = 0;
		a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
			auto* object = a_ref ? a_ref->Get3D() : nullptr;
			if (!object || a_ref->As<RE::Actor>()) {
				return RE::BSContainer::ForEachResult::kContinue;  // skeletons keep their ragdoll and controller ties
			}
			bool newlyStilled = false;
			RE::BSVisit::TraverseScenegraphObjects(object, [&](RE::NiAVObject* a_object) {
				auto* collision = a_object->collisionObject ? netimmerse_cast<RE::bhkNiCollisionObject*>(a_object->collisionObject.get()) : nullptr;
				if (collision && collision->flags.any(RE::bhkNiCollisionObject::Flag::kActive)) {
					stilledClutter.emplace_back(RE::NiPointer<RE::NiAVObject>(a_object), collision->flags.underlying());
					Pristine::FlagsChanged(collision, collision->flags.underlying());
					collision->flags.reset(RE::bhkNiCollisionObject::Flag::kActive, RE::bhkNiCollisionObject::Flag::kSyncOnUpdate);
					newlyStilled = true;
					++stilled;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			// Where the item is recorded, 3D and body both (a warp moves the body too): already
			// moved by its body before it was stilled, and, handed back to the engine on walking out,
			// the body would carry it off again from wherever it was left.
			// Merged (into an interior), the 3D stands at the mapped place, never at the recorded one.
			if (newlyStilled || (!merged && object->world.translate.GetDistance(a_ref->GetPosition()) > 50.0f)) {
				a_ref->Update3DPosition(true);
				++replaced;
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		if (stilled || replaced) {
			logger::info("portal: {} collision object(s) in {:08X} made visual only, {} reference(s) placed where they are recorded", stilled, a_cell->GetFormID(), replaced);
		}
	}

	void RemoteCell::CheckLooseItems()
	{
		int total = 0, with3D = 0, fallen = 0, loaded = 0, stillMissing = 0, skippedQueued = 0;
		int details = 0;
		for (auto* cell : cells) {
			auto* data = cell->GetRuntimeData().loadedData;
			// Something still queued for this cell may be these very items, which the engine is about
			// to load: loading them a second time here could leave two 3D behind.
			const bool queued = !data || data->queuedRefCount != 0 || data->criticalQueuedRefCount != 0;
			cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
				if (!base || !base->IsInventoryObject() || a_ref->IsDisabled() || a_ref->IsDeleted()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				++total;
				auto*      object = a_ref->Get3D();
				const auto pos = a_ref->GetPosition();
				if (object) {
					++with3D;
					if (object->world.translate.GetDistance(pos) > 200.0f) {
						++fallen;
						if (details++ < 10) {
							logger::info("portal:   item {:08X} '{}': 3D at ({:.0f},{:.0f},{:.0f}), the item at ({:.0f},{:.0f},{:.0f})", a_ref->GetFormID(), base->GetName(),
								object->world.translate.x, object->world.translate.y, object->world.translate.z, pos.x, pos.y, pos.z);
						}
					}
				} else if (queued) {
					++skippedQueued;
				} else {
					a_ref->Load3D(false);
					a_ref->Get3D() ? ++loaded : ++stillMissing;
					if (details++ < 10) {
						logger::info("portal:   item {:08X} '{}' at ({:.0f},{:.0f},{:.0f}) had no 3D: {}", a_ref->GetFormID(), base->GetName(), pos.x, pos.y, pos.z,
							a_ref->Get3D() ? "loaded now" : "still none");
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		logger::info("portal: loose items in {}: {} total, {} with 3D ({} of them far from the item), {} loaded now, {} still without 3D, {} left to the engine's queue",
			Probe::DescribeCell(Cell()), total, with3D, fallen, loaded, stillMissing, skippedQueued);

	}

	namespace
	{
		// Whether the Havok world has something to stand on below a_pos (within 200 units).
		bool GroundBelow(RE::bhkWorld* a_world, const RE::NiPoint3& a_pos)
		{
			const float     scale = RE::bhkWorld::GetWorldScale();
			RE::bhkPickData pick;
			pick.rayInput.from = RE::hkVector4(a_pos.x * scale, a_pos.y * scale, (a_pos.z + 50.0f) * scale, 0.0f);
			pick.rayInput.to = RE::hkVector4(a_pos.x * scale, a_pos.y * scale, (a_pos.z - 200.0f) * scale, 0.0f);
			pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kCharController);
			RE::BSReadLockGuard lock(a_world->worldLock);
			return a_world->PickObject(pick) && pick.rayOutput.HasHit();
		}
	}

	bool RemoteCell::GroundUnderStreet()
	{
		auto* world = Cell()->GetbhkWorld();
		if (!world) {
			return false;
		}
		// Where NPCs and loose items are recorded to be: what would fall if the world were stepped.
		// Most of them, not all: one soldier standing where no ray finds ground (a wall walk, a
		// ledge) kept the whole street from ever being simulated.
		int probed = 0, grounded = 0;
		for (auto* cell : cells) {
			cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
				auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
				const bool falls = (actor && !actor->IsDead()) || (base && base->IsInventoryObject() && !a_ref->IsDisabled() && a_ref->Get3D());
				if (falls) {
					++probed;
					grounded += GroundBelow(world, a_ref->GetPosition()) ? 1 : 0;
				}
				return probed >= 12 ? RE::BSContainer::ForEachResult::kStop : RE::BSContainer::ForEachResult::kContinue;
			});
			if (probed >= 12) {
				break;
			}
		}
		const bool ground = grounded * 4 >= probed * 3;
		if (!ground) {
			logger::info("portal: ground under only {} of {} NPC(s) and item(s) in the street; not simulated yet", grounded, probed);
		}
		return ground;
	}

}
