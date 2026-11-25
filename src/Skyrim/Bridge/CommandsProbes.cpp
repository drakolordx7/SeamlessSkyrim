#include "Skyrim/Bridge/Commands.h"

#include "Skyrim/Bridge/CommandSupport.h"
#include "Skyrim/Bridge/FrameHooks.h"
#include "Skyrim/Bridge/GpuTrack.h"
#include "Skyrim/Bridge/Perf.h"
#include "Skyrim/Bridge/PortalDiagnostics.h"
#include "Skyrim/Bridge/ReverseEngineering.h"
#include "Skyrim/Bridge/Sampler.h"
#include "Skyrim/Diag/Probe.h"
#include "Skyrim/Doors/Hold.h"
#include "Skyrim/Engine/Havok.h"
#include "Skyrim/Portal/LightScene.h"
#include "Skyrim/Portal/MergedView.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Render/Frame.h"
#include "Skyrim/Render/SpaceLighting.h"

using namespace Bridge::Support;

namespace
{
	json DoorTarget(RE::TESObjectREFR* a_door)
	{
		auto* teleport = a_door->extraList.GetByType<RE::ExtraTeleport>();
		if (!teleport || !teleport->teleportData) {
			return nullptr;
		}
		auto linked = teleport->teleportData->linkedDoor.get();
		json target = { { "linkedDoor", linked ? Hex(linked->GetFormID()) : "" } };
		if (linked) {
			target["cell"] = CellJson(linked->GetParentCell());
			if (auto* world = linked->GetWorldspace()) {
				target["worldspace"] = Hex(world->GetFormID());
			}
		}
		return target;
	}

	// ---- commands ---------------------------------------------------------------------------

	json RefsNear(const json& a_args)
	{
		auto*       player = RequirePlayer();
		const float radius = a_args.value("radius", 2000.0f);
		const auto  kind = a_args.value("kind", std::string("all"));
		const auto  limit = a_args.value("limit", 50u);

		struct Entry
		{
			float distance;
			json  data;
		};
		std::vector<Entry> entries;
		RE::TES::GetSingleton()->ForEachReferenceInRange(player, radius, [&](RE::TESObjectREFR* a_ref) {
			auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
			if (!base || a_ref == player || a_ref->IsDisabled()) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			const bool isDoor = base->Is(RE::FormType::Door);
			const bool isActor = a_ref->Is(RE::FormType::ActorCharacter);
			if ((kind == "doors" && !isDoor) || (kind == "actors" && !isActor)) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			const float distance = player->GetPosition().GetDistance(a_ref->GetPosition());
			json        data = {
                { "id", Hex(a_ref->GetFormID()) },
                { "base", Hex(base->GetFormID()) },
                { "type", RE::FormTypeToString(base->GetFormType()) },
                { "name", a_ref->GetDisplayFullName() },
                { "pos", Vec(a_ref->GetPosition()) },
                { "distance", distance },
			};
			if (isDoor) {
				data["teleport"] = DoorTarget(a_ref);
				data["locked"] = a_ref->IsLocked();
			}
			entries.push_back({ distance, std::move(data) });
			return RE::BSContainer::ForEachResult::kContinue;
		});

		std::ranges::sort(entries, {}, &Entry::distance);
		json result = json::array();
		for (std::size_t i = 0; i < entries.size() && i < limit; ++i) {
			result.push_back(std::move(entries[i].data));
		}
		return { { "count", entries.size() }, { "refs", result } };
	}

	// Diagnostics for NPCs that are loaded but not drawn after a seamless entry: everything that
	// decides whether an actor's 3D reaches the main view.
	json Actors3D(const json& a_args)
	{
		auto* player = RequirePlayer();
		auto* cell = player->GetParentCell();
		if (a_args.contains("cell")) {
			cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(ParseFormID(a_args.at("cell")));
			if (!cell) {
				throw std::runtime_error("no such cell");
			}
		}
		auto*      loaded = cell->GetRuntimeData().loadedData;
		auto*      worldScene = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		auto*      worldGraph = worldScene ? worldScene->GetRuntimeData().portalGraph : nullptr;
		auto*      cellGraph = loaded ? loaded->portalGraph.get() : nullptr;
		auto*      cell3D = loaded ? loaded->cell3D.get() : nullptr;
		auto*      worldRoot = RE::Main::WorldRootNode();
		json       actors = json::array();
		cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
			auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
			if (!actor || actor->IsPlayerRef()) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			auto* object = actor->Get3D();
			json  entry = {
                { "id", Hex(actor->GetFormID()) },
                { "name", actor->GetDisplayFullName() },
                { "disabled", actor->IsDisabled() },
                { "dead", actor->IsDead() },
                { "distance", player->GetPosition().GetDistance(actor->GetPosition()) },
                { "high", actor->GetActorRuntimeData().currentProcess && actor->GetActorRuntimeData().currentProcess->InHighProcess() },
                { "has3D", object != nullptr },
                { "room", [&] {
					 auto* room = actor->extraList.GetByType<RE::ExtraRoom>();
					 return room && room->room ? json(room->room->name.c_str()) : json(nullptr);
				 }() },
			};
			if (object) {
				json chain = json::array();
				bool underCell = false;
				for (auto* node = object; node; node = node->parent) {
					underCell |= node == cell3D;
					const auto& b = node->worldBound;
					chain.push_back(std::format("{}{} flags {:X} bound ({:.0f},{:.0f},{:.0f}) r {:.0f}", node->name.c_str(), node->GetAppCulled() ? " [culled]" : "",
						node->GetFlags().underlying(), b.center.x, b.center.y, b.center.z, b.radius));
				}
				RE::NiAVObject* top = object;
				while (top->parent) {
					top = top->parent;
				}
				auto* fade = object->AsFadeNode();
				entry["chain"] = chain;
				entry["underCell3D"] = underCell;
				entry["underWorldRoot"] = top == worldRoot;
				if (fade) {
					const auto& f = fade->GetRuntimeData();
					entry["fade"] = { f.unk128, f.unk12C, f.currentFade, f.unk134, f.unk138, f.unk13C, f.unk140, f.unk144, f.unk148, f.unk14C, f.unk150, f.unk152, f.unk153, f.unk154,
						f.unk155 };
				}
				entry["fadeAmount"] = object->GetFadeAmount();
				// Whether the skeleton is being posed: a bone's world position and when it was updated.
				entry["rootWorld"] = Vec(object->world.translate - actor->GetPosition());
				if (auto* spine = object->GetObjectByName("NPC Spine [Spn0]")) {
					entry["spineWorld"] = Vec(spine->world.translate - actor->GetPosition());
					entry["spineUpdated"] = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(spine) + 0x104);
				}
				entry["rootUpdated"] = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(object) + 0x104);
				entry["boundRadius"] = object->worldBound.radius;
				entry["boundCenterOffset"] = Vec(object->worldBound.center - actor->GetPosition());
				// Shapes under the actor that a cull would skip on their own flags.
				std::uint32_t shapes = 0, culledShapes = 0, mergedShapes = 0;
				json          skins = json::array();
				RE::BSVisit::TraverseScenegraphGeometries(object, [&](RE::BSGeometry* a_geometry) {
					++shapes;
					culledShapes += a_geometry->GetAppCulled() ? 1 : 0;
					mergedShapes += (Portal::MergedView::RoomOf(a_geometry) >= 0) ? 1 : 0;
					if (auto* shader = a_geometry->GetGeometryRuntimeData().shaderProperty.get()) {
						skins.push_back(std::format("{}: flags {:X} shader alpha {:.2f} material alpha {:.2f}", a_geometry->name.c_str(), a_geometry->GetFlags().underlying(),
							shader->alpha, shader->QMaterialAlpha()));
					}
					auto* skin = a_geometry->GetGeometryRuntimeData().skinInstance.get();
					if (auto* dismember = skin ? netimmerse_cast<RE::BSDismemberSkinInstance*>(skin) : nullptr) {
						const auto& d = dismember->GetRuntimeData();
						int         visible = 0;
						for (int i = 0; i < d.numPartitions; ++i) {
							visible += d.partitions[i].visible ? 1 : 0;
						}
						skins.push_back(std::format("{}: {}/{} visible, unk98 {}", a_geometry->name.c_str(), visible, d.numPartitions, d.unk98));
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
				entry["skins"] = skins;
				entry["shapes"] = shapes;
				entry["culledShapes"] = culledShapes;
				entry["mergedShapes"] = mergedShapes;
				entry["flags"] = std::format("{:X}", object->GetFlags().underlying());
			}
			actors.push_back(std::move(entry));
			return RE::BSContainer::ForEachResult::kContinue;
		});
		return {
			{ "cell", CellJson(cell) },
			{ "worldSceneGraph", Address(worldGraph) },
			{ "cellGraph", Address(cellGraph) },
			{ "cellRooms", cellGraph ? cellGraph->rooms.size() : 0 },
			{ "actors", actors },
		};
	}

	json NodeJson(RE::NiAVObject* a_object)
	{
		if (!a_object) {
			return nullptr;
		}
		auto* node = a_object->AsNode();
		std::size_t children = 0;
		if (node) {
			for (const auto& child : node->GetChildren()) {
				children += child ? 1 : 0;
			}
		}
		const auto& at = a_object->world.translate;
		return { { "name", a_object->name.c_str() }, { "culled", a_object->GetAppCulled() }, { "children", children },
			{ "parent", a_object->parent ? a_object->parent->name.c_str() : "" }, { "at", { at.x, at.y, at.z } } };
	}

	// What of the outdoors still exists while the player is in an interior: the question deciding
	// whether the renderer can keep drawing the outdoors as the world there (docs/APPROACHES.md, 9).
	json OutdoorsProbe(const json&)
	{
		json  result;
		auto* tes = RE::TES::GetSingleton();
		if (tes) {
			const auto* world = tes->GetRuntimeData2().worldSpace;
			std::size_t attached = 0, gridCount = 0;
			if (tes->gridCells) {
				for (std::uint32_t x = 0; x < tes->gridCells->length; ++x) {
					for (std::uint32_t y = 0; y < tes->gridCells->length; ++y) {
						if (auto* cell = tes->gridCells->GetCell(x, y)) {
							++gridCount;
							attached += cell->IsAttached() ? 1 : 0;
						}
					}
				}
			}
			result["tes"] = { { "worldSpace", world ? world->GetFormEditorID() : "" }, { "interiorCell", tes->interiorCell ? Hex(tes->interiorCell->GetFormID()) : "" },
				{ "gridCells", gridCount }, { "gridAttached", attached }, { "objRoot", NodeJson(tes->objRoot) }, { "lodLandRoot", NodeJson(tes->lodLandRoot) } };
		}
		if (auto* sky = RE::Sky::GetSingleton()) {
			result["sky"] = { { "mode", static_cast<int>(sky->mode.get()) }, { "root", NodeJson(sky->root.get()) },
				{ "weather", sky->currentWeather ? Hex(sky->currentWeather->GetFormID()) : "" }, { "hour", sky->currentGameHour },
				{ "calendarHour", RE::Calendar::GetSingleton() ? RE::Calendar::GetSingleton()->GetHour() : -1.0f } };
			const auto Rgb = [](const RE::NiColor& a_color) { return json::array({ a_color.red, a_color.green, a_color.blue }); };
			using Color = RE::TESWeather::ColorTypes;
			result["sky"]["sunlight"] = Rgb(sky->skyColor[Color::kSunlight]);
			result["sky"]["ambient"] = Rgb(sky->skyColor[Color::kAmbient]);
			result["sky"]["skyUpper"] = Rgb(sky->skyColor[Color::kSkyUpper]);
			json dalc = json::array();
			for (auto& axis : sky->directionalAmbientColors) {
				dalc.push_back({ Rgb(axis[0]), Rgb(axis[1]) });
			}
			result["sky"]["dalc"] = dalc;
			if (auto* sun = sky->sun; sun && sun->light) {
				result["sky"]["sunLight"] = Rgb(sun->light->GetLightRuntimeData().diffuse);
				result["sky"]["sunFade"] = sun->light->GetLightRuntimeData().fade;
			}
			result["lighting"] = ::Render::SpaceLighting::OutdoorLightingProbe();
			// How many of the distant land's, objects' and trees' shapes count as the merged street's.
			json lodMarked = json::object();
			const std::pair<const char*, RE::NiAVObject*> lodParts[] = { { "land", RE::BGSTerrainManager::GetLODLandRoot().get() },
				{ "objects", RE::BGSTerrainManager::GetLODObjectRoot().get() }, { "trees", RE::BGSTerrainManager::GetTreeNode().get() } };
			for (const auto& [name, node] : lodParts) {
				int total = 0, marked = 0;
				std::map<std::string, int> properties;
				if (node) {
					RE::BSVisit::TraverseScenegraphGeometries(node, [&](RE::BSGeometry* a_shape) {
						++total;
						marked += (Portal::MergedView::RoomOf(a_shape) >= 0) ? 1 : 0;
						auto* property = a_shape->GetGeometryRuntimeData().shaderProperty.get();
						++properties[property ? property->GetRTTI() ? property->GetRTTI()->name : "?" : "none"];
						return RE::BSVisit::BSVisitControl::kContinue;
					});
				}
				lodMarked[name] = { { "shapes", total }, { "marked", marked }, { "properties", properties } };
			}
			result["lodMarked"] = lodMarked;
			// The engine's sun shadow light: whether it exists and is among the active shadow
			// lights, its direction, and whether it shares the world's sun light.
			if (auto* world = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]) {
				auto& data = world->GetRuntimeData();
				json  shadow = { { "exists", data.sunShadowDirLight != nullptr }, { "activeShadowLights", data.activeShadowLights.size() } };
				if (auto* dir = data.sunShadowDirLight) {
					const auto& v = dir->GetShadowDirectionalLightRuntimeData().sunVector;
					shadow["sunVector"] = { v.x, v.y, v.z };
					shadow["active"] = std::ranges::any_of(data.activeShadowLights, [&](const auto& a_light) { return a_light.get() == dir; });
					shadow["sameLightAsSun"] = data.sunLight && dir->light == data.sunLight->light;
					shadow["cascades"] = dir->GetRuntimeData().shadowmapDescriptors.size();
				}
				if (auto* sun = data.sunLight ? data.sunLight->light.get() : nullptr) {
					const auto& r = sun->world.rotate;
					shadow["sunLightDirection"] = { r.entry[0][0], r.entry[1][0], r.entry[2][0] };
				}
				result["sunShadow"] = shadow;
			}
		}
		const auto TerrainJson = [](const RE::BGSTerrainManager* a_manager) -> json {
			if (!a_manager) {
				return nullptr;
			}
			return { { "worldSpace", a_manager->worldSpace ? a_manager->worldSpace->GetFormEditorID() : "" }, { "hasLOD", a_manager->hasLOD },
				{ "staticDataLoaded", a_manager->staticDataLoaded }, { "rootNode", a_manager->rootNode != nullptr },
				{ "cull", { a_manager->cachedCullState.cullRoot, a_manager->cachedCullState.cullLand, a_manager->cachedCullState.cullTrees, a_manager->cachedCullState.cullObjects } } };
		};
		result["terrain"] = { { "active", TerrainJson(RE::BGSTerrainManager::GetActiveManager()) },
			{ "lodRoot", NodeJson(RE::BGSTerrainManager::GetLODRoot().get()) }, { "lodLandRoot", NodeJson(RE::BGSTerrainManager::GetLODLandRoot().get()) },
			{ "lodObjectRoot", NodeJson(RE::BGSTerrainManager::GetLODObjectRoot().get()) }, { "water", NodeJson(RE::BGSTerrainManager::GetWaterNode().get()) },
			{ "trees", NodeJson(RE::BGSTerrainManager::GetTreeNode().get()) } };
		// The worldspace the street belongs to (the active portal's other side, when outdoors).
		if (const auto portal = Portal::Manager::ReadyPortal(); portal && portal->cell && portal->cell->GetRuntimeData().worldSpace) {
			auto* world = portal->cell->GetRuntimeData().worldSpace;
			result["streetWorld"] = { { "editorId", world->GetFormEditorID() }, { "terrain", TerrainJson(world->terrainManager) },
				{ "terrainOfParent", TerrainJson(world->GetTerrainManager()) } };
		}
		json scene = json::array();
		if (auto* ssn = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]) {
			for (const auto& child : ssn->GetChildren()) {
				if (child) {
					json entry = NodeJson(child.get());
					if (auto* node = child->AsNode()) {
						json inner = json::array();
						for (const auto& grandchild : node->GetChildren()) {
							if (grandchild) {
								inner.push_back(NodeJson(grandchild.get()));
							}
						}
						entry["childNodes"] = inner;
					}
					scene.push_back(entry);
				}
			}
		}
		result["worldScene"] = scene;
		// What is drawn in the doorway: every reference whose 3D stands near the portal's door, in
		// the player's cell and the remote cells (merged ones are where they are drawn).
		if (const auto portal = Portal::Manager::ReadyPortal()) {
			if (auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(portal->sourceDoor)) {
				const auto center = door->GetPosition();
				json       nearDoor = json::array();
				std::vector<RE::TESObjectCELL*> cells(Portal::Manager::RemoteCells().begin(), Portal::Manager::RemoteCells().end());
				cells.push_back(door->GetParentCell());
				for (auto* cell : cells) {
					cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
						auto* object = a_ref ? a_ref->Get3D() : nullptr;
						if (object && object->world.translate.GetDistance(center) < 150.0f) {
							const auto& at = object->world.translate;
							// Why a shape is or isn't drawn: the flags of every node down to it, its fade and material.
							json shapes = json::array();
							RE::BSVisit::TraverseScenegraphGeometries(object, [&](RE::BSGeometry* a_shape) {
								if (shapes.size() >= 4) {
									return RE::BSVisit::BSVisitControl::kStop;
								}
								std::string chain;
								for (auto* up = static_cast<RE::NiAVObject*>(a_shape); up; up = up->parent) {
									chain += std::format("{}:{:X} ", up->name.c_str(), up->GetFlags().underlying());
									if (auto* fade = up->AsFadeNode()) {
										chain += std::format("[fade {:.2f}] ", fade->GetRuntimeData().currentFade);
									}
								}
								auto* property = a_shape->GetGeometryRuntimeData().shaderProperty.get();
								shapes.push_back(std::format("'{}' alpha {:.2f} flags {:X} | {}", a_shape->name.c_str(), property ? property->alpha : -1.0f,
									property ? property->flags.underlying() : 0, chain));
								return RE::BSVisit::BSVisitControl::kContinue;
							});
							nearDoor.push_back({ { "ref", Hex(a_ref->GetFormID()) }, { "base", Hex(a_ref->GetBaseObject() ? a_ref->GetBaseObject()->GetFormID() : 0) },
								{ "cell", Hex(cell->GetFormID()) }, { "culled", object->GetAppCulled() }, { "disabled", a_ref->IsDisabled() }, { "at", { at.x, at.y, at.z } },
								{ "shapes", shapes } });
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
				}
				result["doorway"] = nearDoor;
			}
			// The remote space's references near its own door that can't be drawn where they should
			// be: no 3D, 3D not in the scene graph or culled somewhere above it, faded out, or a bound
			// left behind. The worldspace's persistent references too (a town's houses and paving),
			// which aren't in the cells' own lists. Then each cell's land.
			if (auto* linked = RE::TESForm::LookupByID<RE::TESObjectREFR>(portal->linkedDoor)) {
				const auto center = linked->GetPosition();
				json       missing = json::array();
				const auto Check = [&](RE::TESObjectREFR* a_ref) {
					if (!a_ref || a_ref->IsDisabled() || a_ref->GetPosition().GetDistance(center) > 1500.0f || missing.size() >= 60) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					auto*       object = a_ref->Get3D();
					auto*       base = a_ref->GetBaseObject();
					std::string why;
					if (!object) {
						why = "no 3D";
					} else {
						// Culled at its own root is the engine's doing for markers (base IDs below 0x800).
						const bool      marker = !base || base->GetFormID() < 0x800 || std::string_view(object->name.c_str()).find("Marker") != std::string_view::npos;
						RE::NiAVObject* top = object;
						for (auto* up = marker ? object->parent : object; up; up = up->parent) {
							if (up->GetAppCulled()) {
								why = std::format("culled at '{}'", up->name.c_str());
								break;
							}
							top = up;
						}
						if (why.empty() && top != RE::Main::WorldRootNode()) {
							why = std::format("not in the scene graph (top '{}')", top->name.c_str());
						}
						if (auto* fade = object->AsFadeNode(); why.empty() && fade && fade->GetRuntimeData().currentFade < 0.99f) {
							why = std::format("faded to {:.2f}", fade->GetRuntimeData().currentFade);
						}
						if (const auto& bound = object->worldBound; why.empty() && bound.center.GetDistance(object->world.translate) > bound.radius + 300.0f) {
							why = std::format("bound elsewhere: center ({:.0f},{:.0f},{:.0f}) radius {:.0f}, object at ({:.0f},{:.0f},{:.0f}), flags {:X}", bound.center.x, bound.center.y,
								bound.center.z, bound.radius, object->world.translate.x, object->world.translate.y, object->world.translate.z, object->GetFlags().underlying());
						}
					}
					if (!why.empty()) {
						missing.push_back(std::format("{:08X} {:08X} '{}': {}", a_ref->GetFormID(), base ? base->GetFormID() : 0, base ? base->GetFormEditorID() : "", why));
					}
					return RE::BSContainer::ForEachResult::kContinue;
				};
				auto* world = linked->GetParentCell() && !linked->GetParentCell()->IsInteriorCell() ? linked->GetParentCell()->GetRuntimeData().worldSpace : nullptr;
				if (world && world->persistentCell) {
					world->persistentCell->ForEachReference(Check);
				}
				for (auto* cell : Portal::Manager::RemoteCells()) {
					cell->ForEachReference(Check);
					auto*       land = cell->GetRuntimeData().cellLand;
					auto*       landData = land ? land->loadedData : nullptr;
					std::string quads;
					for (int quad = 0; quad < 4; ++quad) {
						auto*       mesh = landData ? landData->mesh[quad] : nullptr;
						std::string state = mesh ? "ok" : "none";
						if (mesh) {
							RE::NiAVObject* top = mesh;
							for (RE::NiAVObject* up = mesh; up; up = up->parent) {
								if (up->GetAppCulled()) {
									state = std::format("culled at '{}'", up->name.c_str());
									break;
								}
								top = up;
							}
							if (state == "ok" && top != RE::Main::WorldRootNode()) {
								state = std::format("top '{}'", top->name.c_str());
							}
						}
						quads += std::format("{}{}", quad ? ", " : "", state);
					}
					missing.push_back(std::format("land of {:08X}: {}", cell->GetFormID(), quads));
				}
				result["remoteNotDrawn"] = missing;
			}
		}
		return result;
	}

	// Diagnostics: the distant LOD's shapes within 6000 units of the player (in the street's
	// coordinates when the street is shown indoors), what culls them, and how many of their cells'
	// segments are hidden (cells whose own houses are loaded).
	json LODNear()
	{
		json  result = json::array();
		auto* lodRoot = RE::BGSTerrainManager::GetLODRoot().get();
		auto* root = lodRoot ? lodRoot->parent : nullptr;
		if (!root) {
			return result;
		}
		const auto toRoot = root->world.Invert();
		const auto point = toRoot * RE::PlayerCharacter::GetSingleton()->GetPosition();
		if (auto* tes = RE::TES::GetSingleton()) {
			result.push_back(std::format("point ({:.0f},{:.0f}) grid ({},{})", point.x, point.y, tes->currentGridX, tes->currentGridY));
		}
		for (auto* top : { RE::BGSTerrainManager::GetLODObjectRoot().get(), RE::BGSTerrainManager::GetLODLandRoot().get() }) {
			if (!top) {
				continue;
			}
			RE::BSVisit::TraverseScenegraphGeometries(top, [&](RE::BSGeometry* a_shape) {
				const auto c = toRoot * a_shape->worldBound.center;
				if (std::hypot(c.x - point.x, c.y - point.y) > a_shape->worldBound.radius + 6000.0f || result.size() > 80) {
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				std::string chain;
				for (RE::NiAVObject* up = a_shape; up && up != top; up = up->parent) {
					chain += std::format("'{}'{}{} < ", up->name.c_str(), up->GetRTTI() ? std::format(" {}", up->GetRTTI()->GetName()) : "", up->GetAppCulled() ? " CULLED" : "");
				}
				std::string segments;
				if (auto* sub = netimmerse_cast<RE::BSSubIndexTriShape*>(a_shape)) {
					const auto& data = sub->GetSubIndexedTrishapeRuntimeData();
					int hidden = 0;
					for (std::uint32_t i = 0; data.segmentData && i < data.numSegments; ++i) {
						hidden += data.segmentData[i].unkFlags == 0 ? 1 : 0;
					}
					segments = std::format(" segments {} hidden {}", data.numSegments, hidden);
				}
				result.push_back(std::format("({:.0f},{:.0f}) r{:.0f}{}: {}", c.x, c.y, a_shape->worldBound.radius, segments, chain));
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}
		return result;
	}

	// Diagnostics: distant-object segments (one per cell) still drawn for cells whose own 3D is
	// attached, within 6 cells of the player (in the street's coordinates when it is shown
	// indoors): both are drawn there, the LOD fighting the houses.
	json LODDoubles()
	{
		json  result = json::array();
		auto* manager = RE::BGSTerrainManager::GetActiveManager();
		auto* lodRoot = RE::BGSTerrainManager::GetLODRoot().get();
		auto* root = lodRoot ? lodRoot->parent : nullptr;
		auto* tes = RE::TES::GetSingleton();
		auto* world = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
		if (!manager || !manager->rootNode || !root || !world) {
			return result;
		}
		const auto point = root->world.Invert() * RE::PlayerCharacter::GetSingleton()->GetPosition();
		const int  px = static_cast<int>(std::floor(point.x / 4096.0f)), py = static_cast<int>(std::floor(point.y / 4096.0f));
		const auto level = manager->segmentedBlockLevel;
		int        drawn = 0, hidden = 0, blocks = 0, shapes = 0, segments = 0, unattached = 0;
		std::function<void(RE::BGSTerrainNode*)> visit = [&](RE::BGSTerrainNode* a_node) {
			const int lod = static_cast<int>(a_node->GetLODLevel());
			if (a_node->baseCellX > px + 6 || a_node->baseCellX + lod < px - 6 || a_node->baseCellY > py + 6 || a_node->baseCellY + lod < py - 6) {
				return;
			}
			if (static_cast<std::uint32_t>(lod) == level) {
				++blocks;
				auto* block = a_node->objects ? reinterpret_cast<RE::BGSDistantObjectBlock*>(a_node->objects->block) : nullptr;
				auto* node = block ? block->node.get() : nullptr;
				if (!node) {
					return;
				}
				// Under the block's node, each in a multibound node of its own.
				RE::BSVisit::TraverseScenegraphGeometries(node, [&](RE::BSGeometry* a_shape) {
					auto* sub = netimmerse_cast<RE::BSSubIndexTriShape*>(a_shape);
					const auto* data = sub ? &sub->GetSubIndexedTrishapeRuntimeData() : nullptr;
					shapes += data ? 1 : 0;
					for (std::uint32_t i = 0; data && data->segmentData && i < data->numSegments && i < level * level; ++i) {
						const auto& segment = data->segmentData[i];
						if (segment.unkTriCount == 0) {
							continue;
						}
						++segments;
						const auto x = static_cast<std::int16_t>(a_node->baseCellX + i / level), y = static_cast<std::int16_t>(a_node->baseCellY + i % level);
						const auto it = world->cellMap.find(RE::CellID(y, x));
						auto*      cell = it != world->cellMap.end() ? it->second : nullptr;
						if (!cell || cell->cellState.get() != RE::TESObjectCELL::CellState::kAttached) {
							++unattached;
							continue;
						}
						if (segment.unkFlags == 0) {
							++hidden;
						} else if (++drawn <= 30) {
							result.push_back(std::format("drawn over attached cell ({},{}) {:08X} '{}': '{}' segment {} of block ({},{}) culled {}", x, y, cell->GetFormID(),
								cell->GetFormEditorID(), sub->name.c_str(), i, a_node->baseCellX, a_node->baseCellY, sub->GetAppCulled()));
						}
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
				return;
			}
			if (a_node->children) {
				auto* kids = reinterpret_cast<RE::BGSTerrainNode*>(a_node->children);
				for (int k = 0; k < 4; ++k) {
					visit(&kids[k]);
				}
			}
		};
		visit(manager->rootNode);
		result.push_back(std::format("player cell ({},{}), root ({},{}) level {}, block level {}: {} block(s), {} shape(s), {} segment(s) with triangles, {} of cells not attached; of attached cells {} drawn, {} hidden",
			px, py, manager->rootNode->baseCellX, manager->rootNode->baseCellY, manager->rootNode->GetLODLevel(), level, blocks, shapes, segments, unattached, drawn, hidden));
		return result;
	}

	json DumpWorld(const json&)
	{
		for (const auto& line : LODDoubles()) {
			logger::info("lod double: {}", line.get<std::string>());
		}
		for (const auto& line : LODNear()) {
			logger::info("lod near: {}", line.get<std::string>());
		}
		Probe::DumpWorldState("bridge request");
		logger::info("outdoors probe: {}", OutdoorsProbe({}).dump());
		const auto passes = Bridge::PortalDiagnostics::RenderPasses(Portal::Manager::Space(), false);
		logger::info("remote render passes: {}", passes.dump());
		return { { "logged", true } };
	}
}

namespace Bridge
{
	void AddProbeCommands(CommandTable& a_commands)
	{
		a_commands.insert({
			{ "refs_near", { RefsNear } },
			// Tests: what the player's Havok world has between two points (from, to; "room" as in ray_down).
			{ "ray", { [](const json& a) {
				 auto* player = RequirePlayer();
				 auto* world = player->GetParentCell() ? player->GetParentCell()->GetbhkWorld() : nullptr;
				 if (a.value("room", false)) {
					 const auto& cells = Portal::Manager::RemoteCells();
					 world = cells.empty() ? nullptr : cells.front()->GetbhkWorld();
				 }
				 if (!world) {
					 throw std::runtime_error("no Havok world");
				 }
				 const auto&     f = a.at("from");
				 const auto&     t = a.at("to");
				 const auto hit = Engine::Havok::Ray(world, { f.at(0).get<float>(), f.at(1).get<float>(), f.at(2).get<float>() }, { t.at(0).get<float>(), t.at(1).get<float>(), t.at(2).get<float>() }, RE::COL_LAYER::kCharController);
				 if (!hit) {
					 return json{ { "hit", false } };
				 }
				 auto* ref = hit->Ref();
				 return json{ { "hit", true }, { "fraction", hit->fraction },
					 { "layer", static_cast<int>(hit->collidable->GetCollisionLayer()) },
					 { "ref", ref ? std::format("{:08X} cell {:08X}", ref->GetFormID(), ref->GetParentCell() ? ref->GetParentCell()->GetFormID() : 0) : "" } };
			 } } },
			// Tests: what the player's Havok world has under a point (the player's by default).
			{ "ray_down", { [](const json& a) {
				 auto* player = RequirePlayer();
				 auto* world = player->GetParentCell() ? player->GetParentCell()->GetbhkWorld() : nullptr;
				 // "room": in the Havok world of the space behind the portal's door instead.
				 if (a.value("room", false)) {
					 const auto& cells = Portal::Manager::RemoteCells();
					 world = cells.empty() ? nullptr : cells.front()->GetbhkWorld();
				 }
				 if (!world) {
					 throw std::runtime_error("no Havok world");
				 }
				 RE::NiPoint3 at = player->GetPosition();
				 if (a.contains("pos")) {
					 const auto& p = a.at("pos");
					 at = { p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>() };
				 }
				 const auto hit = Engine::Havok::Ray(world, { at.x, at.y, at.z + 100.0f }, { at.x, at.y, at.z - 1000.0f }, RE::COL_LAYER::kCharController);
				 if (!hit) {
					 return json{ { "hit", false } };
				 }
				 auto* ref = hit->Ref();
				 return json{ { "hit", true }, { "z", at.z + 100.0f - 1100.0f * hit->fraction },
					 { "ref", ref ? std::format("{:08X} cell {:08X}", ref->GetFormID(), ref->GetParentCell() ? ref->GetParentCell()->GetFormID() : 0) : "" } };
			 } } },
			// Tests: GPU textures and buffers still alive, grouped by the code that made them. The
			// first call starts tracking ({"start": true}); "since": a mark from an earlier call, to
			// list only what was made after it (what a trip left behind); "groups", "frames".
			{ "gpu_live", { [](const json& a) {
				 Bridge::GpuTrack::Start();
				 return Bridge::GpuTrack::Live(a.value("since", 0ull), a.value("groups", 15), a.value("frames", 8));
			 } } },
			{ "gpu_holders", { [](const json& a) {
				 Bridge::GpuTrack::Start();
				 if (a.value("orphans", false)) {
					 return json{ { "resources", Bridge::GpuTrack::OrphanHolders(a.value("since", 0ull), a.value("samples", 4)) } };
				 }
				 return json{ { "textures", Bridge::GpuTrack::Holders(a.value("since", 0ull), a.value("samples", 4)) } };
			 }, false } },
			// Tests: where in memory each of "values" (hex addresses) is kept, and the class of the
			// object keeping it: to follow a leaked object to what holds it, one step at a time.
			{ "mem_holders", { [](const json& a) {
				 std::vector<std::uintptr_t> values;
				 for (const auto& value : a.at("values")) {
					 values.push_back(std::stoull(value.get<std::string>(), nullptr, 16));
				 }
				 const auto holders = Bridge::GpuTrack::Scan(values);
				 json       result = json::object();
				 for (std::size_t i = 0; i < values.size(); ++i) {
					 result[std::format("{:X}", values[i])] = holders[i];
				 }
				 return result;
			 }, false } },
			{ "dump_world", { DumpWorld } },
			{ "outdoors_probe", { OutdoorsProbe } },
			{ "actors_3d", { Actors3D } },
			// Diagnostics: the world scene's graph's occlusion shapes. "list"; "clear": its list emptied
			// (kept aside); "restore": put back. The list: first, last, count at the graph's +0x10, each
			// node next, prev, shape (as ShadowSceneNode::SetPortalGraph, ID 106385, walks it).
			{ "world_occluders", { [](const json& a) {
				 struct Node
				 {
					 Node*                 next;
					 Node*                 prev;
					 RE::BSOcclusionShape* shape;
				 };
				 struct List
				 {
					 Node*         first;
					 Node*         last;
					 std::uint32_t count;
				 };
				 static std::optional<std::pair<List*, List>> aside;
				 auto* world = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
				 auto* graph = world->GetRuntimeData().portalGraph;
				 auto* list = graph ? reinterpret_cast<List*>(reinterpret_cast<std::uintptr_t>(graph) + 0x10) : nullptr;
				 const auto action = a.value("action", std::string("list"));
				 if (action == "clear" && list && !aside) {
					 aside = std::pair{ list, *list };
					 *list = {};
				 } else if (action == "restore" && aside) {
					 *aside->first = aside->second;
					 aside.reset();
				 }
				 json shapes = json::array();
				 for (auto* node = list ? list->first : nullptr; node; node = node->next) {
					 const auto& t = node->shape->translation;
					 shapes.push_back(std::format("{} ({:.0f} {:.0f} {:.0f}) state {}", node->shape->IsOcclusionPlane() ? "plane" : "box", t.x, t.y, t.z, node->shape->visibilityState));
				 }
				 return json{ { "graph", Address(graph) }, { "count", list ? list->count : 0 }, { "aside", aside.has_value() }, { "shapes", shapes } };
			 } } },
			// Diagnostics: references (temporary and persistent) within a radius of a point in a
			// worldspace, with where their 3D hangs.
			{ "refs_at", { [](const json& a) {
				 auto*       world = RE::TESForm::LookupByID<RE::TESWorldSpace>(ParseFormID(a.at("world")));
				 const auto& p = a.at("pos");
				 const RE::NiPoint3 at{ p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>() };
				 const float radius = a.value("radius", 400.0f);
				 if (!world) {
					 throw std::runtime_error("no such worldspace");
				 }
				 json        refs = json::array();
				 auto*       persistentLoaded = world->persistentCell ? world->persistentCell->GetRuntimeData().loadedData : nullptr;
				 const auto* persistent3D = persistentLoaded ? persistentLoaded->cell3D.get() : nullptr;
				 std::vector<RE::TESObjectCELL*> parents;
				 const auto  visit = [&](RE::TESObjectCELL* a_cell, const char* a_kind) {
					 if (!a_cell) {
						 return;
					 }
					 a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
						 if (!a_ref || a_ref->GetPosition().GetDistance(at) > radius) {
							 return RE::BSContainer::ForEachResult::kContinue;
						 }
						 auto*           object = a_ref->Get3D();
						 if (auto* parentCell = a_ref->GetParentCell(); parentCell && std::ranges::find(parents, parentCell) == parents.end()) {
							 parents.push_back(parentCell);
						 }
						 RE::NiAVObject* top = object;
						 bool            inPersistent3D = false;
						 while (top && top->parent) {
							 top = top->parent;
							 inPersistent3D |= top == persistent3D;
						 }
						 auto* model = a_ref->GetBaseObject() ? a_ref->GetBaseObject()->As<RE::TESModel>() : nullptr;
						 refs.push_back(std::format("{}{} {:08X} '{}' at ({:.0f} {:.0f} {:.0f}) 3D {} top '{}'{} flags {:X} parentCell {:08X}", a_kind, a_ref->Is(RE::FormType::ActorCharacter) ? "A" : "", a_ref->GetFormID(),
							 model ? model->GetModel() : "", a_ref->GetPositionX(), a_ref->GetPositionY(), a_ref->GetPositionZ(), object != nullptr, top ? top->name.c_str() : "",
							 inPersistent3D ? " (in the persistent cell's 3D)" : "", object ? object->GetFlags().underlying() : 0, a_ref->GetParentCell() ? a_ref->GetParentCell()->GetFormID() : 0));
						 return RE::BSContainer::ForEachResult::kContinue;
					 });
				 };
				 visit(world->persistentCell, "persistent");
				 const auto x = static_cast<std::int16_t>(std::floor(at.x / 4096.0f)), y = static_cast<std::int16_t>(std::floor(at.y / 4096.0f));
				 const auto it = world->cellMap.find(RE::CellID(y, x));
				 visit(it != world->cellMap.end() ? it->second : nullptr, "cell");
				 // The worldspace's persistent cell: where the engine hangs its references' 3D outdoors.
				 json persistent = nullptr;
				 if (auto* cell = world->persistentCell) {
					 auto* loaded = cell->GetRuntimeData().loadedData;
					 auto* cell3D = loaded ? loaded->cell3D.get() : nullptr;
					 json  slots = json::array();
					 if (auto* node = cell3D ? cell3D->AsNode() : nullptr) {
						 for (auto& child : node->GetChildren()) {
							 slots.push_back(child && child->AsNode() ? static_cast<int>(child->AsNode()->GetChildren().size()) : -1);
						 }
					 }
					 persistent = { { "cell", std::format("{:08X}", cell->GetFormID()) }, { "state", static_cast<int>(cell->cellState.get()) }, { "has3D", cell3D != nullptr },
						 { "parent", cell3D && cell3D->parent ? cell3D->parent->name.c_str() : "" }, { "slots", slots } };
				 }
				 // The cells the references found say they are in, and where each one's 3D hangs.
				 json parentCells = json::array();
				 for (auto* cell : parents) {
					 auto* loaded = cell->GetRuntimeData().loadedData;
					 auto* cell3D = loaded ? loaded->cell3D.get() : nullptr;
					 auto* cellWorld = cell->IsInteriorCell() ? nullptr : cell->GetRuntimeData().worldSpace;
					 parentCells.push_back(std::format("{} world {:08X}{} 3D parent '{}'", Probe::DescribeCell(cell), cellWorld ? cellWorld->GetFormID() : 0,
						 cellWorld && cellWorld->persistentCell == cell ? " (its persistent cell)" : "", cell3D && cell3D->parent ? cell3D->parent->name.c_str() : "-"));
				 }
				 return json{ { "refs", refs }, { "persistentCell", persistent }, { "parentCells", parentCells } };
			 } } },
			// References of the remote cells whose 3D exists but hangs nowhere: loaded, never drawn.
			{ "orphans", { [](const json& a) {
				 json found = json::array();
				 for (auto* cell : Portal::Manager::RemoteCells()) {
					 cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
						 auto* object = a_ref ? a_ref->Get3D() : nullptr;
						 if (object && !object->parent && found.size() < 100) {
							 auto* model = a_ref->GetBaseObject() ? a_ref->GetBaseObject()->As<RE::TESModel>() : nullptr;
							 found.push_back(std::format("{:08X} '{}' cell {:08X} disabled {} formFlags {:X} 3D {:X} flags {:X} refcount {}", a_ref->GetFormID(), model ? model->GetModel() : "",
								 cell->GetFormID(), a_ref->IsDisabled(), a_ref->GetFormFlags(), reinterpret_cast<std::uintptr_t>(object), object->GetFlags().underlying(),
								 object->GetRefCount()));
						 }
						 return RE::BSContainer::ForEachResult::kContinue;
					 });
				 }
				 json graphs = json::array();
				 std::vector<RE::TESObjectCELL*> withGraphs(Portal::Manager::RemoteCells());
				 if (auto* extra = a.contains("cell") ? RE::TESForm::LookupByID<RE::TESObjectCELL>(ParseFormID(a.at("cell"))) : nullptr) {
					 withGraphs.push_back(extra);
				 }
				 for (auto* cell : withGraphs) {
					 auto* loaded = cell->GetRuntimeData().loadedData;
					 if (auto* graph = loaded ? loaded->portalGraph.get() : nullptr) {
						 graphs.push_back(std::format("{:08X}: {} rooms, {} always drawn, {} to add, {} to remove", cell->GetFormID(), graph->rooms.size(), graph->alwaysRenderChildren.size(),
							 graph->pendingAlwaysRenderAdds.size(), graph->pendingAlwaysRenderRemoves.size()));
						 for (const auto* list : { &graph->alwaysRenderChildren, &graph->pendingAlwaysRenderAdds, &graph->pendingAlwaysRenderRemoves }) {
							 for (const auto& object : *list) {
								 auto* owner = object ? object->GetUserData() : nullptr;
								 graphs.push_back(std::format("  {} '{}' owner {:08X} parent '{}'", list == &graph->alwaysRenderChildren ? "always" : list == &graph->pendingAlwaysRenderAdds ? "add" : "remove",
									 object ? object->name.c_str() : "", owner ? owner->GetFormID() : 0, object && object->parent ? object->parent->name.c_str() : "-"));
							 }
						 }
					 }
				 }
				 return json{ { "orphans", found }, { "graphs", graphs } };
			 } } },
			// The nodes of a cell's 3D whose bound is wider than {radius} (default 8000), with their
			// widest children: one stray child (a node left at the origin) makes every bound above it
			// span the world, and the engine's per-light walk of the scene can't skip any of it.
			{ "wide_bounds", { [](const json& a) {
				 auto*       cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(ParseFormID(a.at("cell")));
				 auto*       loaded = cell ? cell->GetRuntimeData().loadedData : nullptr;
				 auto*       root = loaded ? loaded->cell3D.get() : nullptr;
				 const float wide = a.value("radius", 8000.0f);
				 json        found = json::array();
				 if (!root) {
					 return json{ { "error", "no 3D" } };
				 }
				 const auto describe = [](const RE::NiAVObject* a_object) {
					 const auto& b = a_object->worldBound;
					 return std::format("'{}' {} ({:.0f},{:.0f},{:.0f}) r {:.0f}", a_object->name.c_str(), a_object->GetRTTI() ? a_object->GetRTTI()->name : "?", b.center.x, b.center.y,
						 b.center.z, b.radius);
				 };
				 RE::BSVisit::TraverseScenegraphObjects(root, [&](RE::NiAVObject* a_object) {
					 if (a_object->worldBound.radius < wide || found.size() >= 60) {
						 return RE::BSVisit::BSVisitControl::kContinue;
					 }
					 std::vector<const RE::NiAVObject*> children;
					 if (auto* node = a_object->AsNode()) {
						 for (auto& child : node->GetChildren()) {
							 if (child) {
								 children.push_back(child.get());
							 }
						 }
					 }
					 // The children farthest from this node's centre: where the width comes from.
					 std::ranges::sort(children, std::greater{}, [&](const RE::NiAVObject* c) { return c->worldBound.center.GetDistance(a_object->worldBound.center) + c->worldBound.radius; });
					 json farthest = json::array();
					 for (std::size_t i = 0; i < std::min<std::size_t>(children.size(), 3); ++i) {
						 farthest.push_back(describe(children[i]));
					 }
					 found.push_back({ { "node", describe(a_object) }, { "children", children.size() }, { "farthest", farthest } });
					 return RE::BSVisit::BSVisitControl::kContinue;
				 });
				 return json{ { "cell3D", describe(root) }, { "wide", found } };
			 } } },
			// Where nodes whose name contains {name} hang, in the world's scene and the portal's:
			// a piece of a room missing from one of them.
			{ "find_nodes", { [](const json& a) {
				 const auto needle = a.at("name").get<std::string>();
				 json       found = json::array();
				 const auto search = [&](RE::NiAVObject* a_root, const char* a_where) {
					 if (!a_root) {
						 return;
					 }
					 RE::BSVisit::TraverseScenegraphObjects(a_root, [&](RE::NiAVObject* a_object) {
						 if (found.size() < 50 && std::string_view(a_object->name.c_str()).find(needle) != std::string_view::npos) {
							 std::string chain = a_where;
							 for (auto* up = a_object->parent; up; up = up->parent) {
								 chain += std::format(" < {} '{}' {:X}", up->GetRTTI() ? up->GetRTTI()->name : "?", up->name.c_str(), up->GetFlags().underlying());
							 }
							 const auto& at = a_object->world.translate;
							 found.push_back(std::format("{} '{}' {:X} at ({:.0f} {:.0f} {:.0f}){}", a_object->GetRTTI() ? a_object->GetRTTI()->name : "?", a_object->name.c_str(),
								 a_object->GetFlags().underlying(), at.x, at.y, at.z, chain));
						 }
						 return RE::BSVisit::BSVisitControl::kContinue;
					 });
				 };
				 search(RE::Main::WorldRootNode(), "world");
				 search(Portal::LightScene::Get(), "portal");
				 return json{ { "found", found } };
			 } } },
			// Diagnostics: the 3D of one reference, node by node (flags, fade, what each shape would draw).
			{ "ref_3d", { [](const json& a) {
				 auto* ref = RequireRef(a);
				 auto* base = ref->GetBaseObject();
				 auto* model = base ? base->As<RE::TESModel>() : nullptr;
				 json  nodes = json::array();
				 if (auto* root = ref->Get3D()) {
					 RE::BSVisit::TraverseScenegraphObjects(root, [&](RE::NiAVObject* a_object) {
						 int depth = 0;
						 for (auto* up = a_object; up && up != root; up = up->parent) {
							 ++depth;
						 }
						 std::string line = std::format("{}{} '{}' flags {:X}", std::string(depth * 2, ' '), a_object->GetRTTI() ? a_object->GetRTTI()->name : "?", a_object->name.c_str(),
							 a_object->GetFlags().underlying());
						 if (auto* fade = a_object->AsFadeNode()) {
							 const auto& data = fade->GetRuntimeData();
							 line += std::format(" fade {:.2f} {:.2f} cur {:.2f} | {:X} {:X} {:X} {:.2f} {:X} {:X} {:.2f} | {:X} {:X} {:X} {:X} {:X} {:X}", data.unk128, data.unk12C, data.currentFade,
								 data.unk134, data.unk138, data.unk13C, data.unk140, data.unk144, data.unk148, data.unk14C, data.unk150, data.unk152, data.unk153, data.unk154, data.unk155, data.unk156);
						 }
						 if (auto* shape = a_object->AsGeometry()) {
							 auto* shader = shape->GetGeometryRuntimeData().shaderProperty.get();
							 line += std::format(" shader {} alpha {:.2f} flags {:X} passes {}", shader != nullptr, shader ? shader->alpha : -1.0f,
								 shader ? shader->flags.underlying() : 0, shader && shader->renderPassList.head != nullptr);
						 }
						 line += std::format(" at ({:.0f} {:.0f} {:.0f}) x{:.2f} prev ({:.0f} {:.0f} {:.0f})", a_object->world.translate.x, a_object->world.translate.y, a_object->world.translate.z,
							 a_object->world.scale, a_object->previousWorld.translate.x, a_object->previousWorld.translate.y, a_object->previousWorld.translate.z);
						 // A model's axes needn't be the world's: a door modelled lying down and stood up
						 // by a node's rotation.
						 const auto& r = a_object->local.rotate.entry;
						 line += std::format(" local ({:.0f} {:.0f} {:.0f}) rot [{:.2f} {:.2f} {:.2f} | {:.2f} {:.2f} {:.2f} | {:.2f} {:.2f} {:.2f}]", a_object->local.translate.x,
							 a_object->local.translate.y, a_object->local.translate.z, r[0][0], r[0][1], r[0][2], r[1][0], r[1][1], r[1][2], r[2][0], r[2][1], r[2][2]);
						 if (auto* shape = a_object->AsGeometry()) {
							 if (auto* shader = shape->GetGeometryRuntimeData().shaderProperty.get()) {
								 line += " raw";
								 const auto* bytes = reinterpret_cast<const std::uint32_t*>(shader);
								 for (int i = 0x28 / 4; i < 0x160 / 4; ++i) {
									 line += std::format(" {:X}", bytes[i]);
								 }
								 line += std::format(" bound ({:.0f} {:.0f} {:.0f}) r{:.0f}", shape->worldBound.center.x, shape->worldBound.center.y, shape->worldBound.center.z, shape->worldBound.radius);
							 }
						 }
						 // Where the physics has the node's body, beside where the node is drawn: a
						 // sign swinging wildly or torn off its bracket is a body far from its node.
						 if (auto* collision = a_object->collisionObject ? a_object->collisionObject->AsBhkNiCollisionObject() : nullptr) {
							 auto* body = collision->body ? collision->body->AsBhkRigidBody() : nullptr;
							 auto* rigid = body ? body->GetRigidBody() : nullptr;
							 line += std::format(" | collision {} flags {:X}", collision->GetRTTI()->name, collision->flags.underlying());
							 if (rigid) {
								 const float scale = RE::bhkWorld::GetWorldScale();
								 const auto& at = rigid->motion.motionState.transform.translation;
								 line += std::format(" body motion {} at ({:.0f} {:.0f} {:.0f}) constraints {} world {:X}", static_cast<int>(rigid->motion.type.get()), at.quad.m128_f32[0] / scale,
									 at.quad.m128_f32[1] / scale, at.quad.m128_f32[2] / scale, body->unk28.size(), reinterpret_cast<std::uintptr_t>(rigid->world));
							 }
						 }
						 nodes.push_back(line);
						 return RE::BSVisit::BSVisitControl::kContinue;
					 });
				 }
				 std::string chain;
				 // With each node's slot in its parent: the engine's cull of a cell's 3D goes by slot
				 // (ID 36643 leaves out slots 0, 1 and 4, and the first child of slot 6).
				 const auto slotOf = [](const RE::NiAVObject* a_object) {
					 int slot = 0;
					 if (a_object->parent) {
						 for (const auto& child : a_object->parent->GetChildren()) {
							 if (child.get() == a_object) {
								 return slot;
							 }
							 ++slot;
						 }
					 }
					 return -1;
				 };
				 for (const auto* below = ref->Get3D(); below && below->parent; below = below->parent) {
					 const auto* up = below->parent;
					 chain += std::format(" #{} < {} '{}' {:X} bound ({:.0f} {:.0f} {:.0f}) r{:.0f}", slotOf(below), up->GetRTTI() ? up->GetRTTI()->name : "?", up->name.c_str(),
						 up->GetFlags().underlying(), up->worldBound.center.x, up->worldBound.center.y, up->worldBound.center.z, up->worldBound.radius);
					 if (!up->parent) {
						 chain += up == RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0] ? " (WORLD scene)" : std::format(" (root {:X})", reinterpret_cast<std::uintptr_t>(up));
					 }
				 }
				 return json{ { "model", model ? model->GetModel() : "" }, { "refFlags", ref->GetFormFlags() }, { "nodes", nodes }, { "chain", chain } };
			 } } },
			{ "frame_stats", { [](const json&) { return FrameHooks::FrameStats(); } } },
			{ "perf", { Perf::Command, false } },
			// {"start": true} clears and records; {"start": false} stops; no argument just reads.
			{ "brightness", { [](const json& a) {
				 if (a.contains("start")) {
					 FrameHooks::RecordBrightness(a.at("start").get<bool>());
				 }
				 return FrameHooks::BrightnessLog();
			 } } },
			// Diagnostics: the player's third-person geometry and whether each one is culled.
			{ "player_parts", { [](const json&) {
				 json  parts = json::array();
				 auto* root = RE::PlayerCharacter::GetSingleton()->Get3D(false);
				 if (root) {
					 RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
						 bool culled = false;
						 for (RE::NiAVObject* node = a_geometry; node; node = node->parent) {
							 culled |= node->GetAppCulled();
						 }
						 parts.push_back(std::format("{}{}", a_geometry->name.c_str(), culled ? " [culled]" : ""));
						 return RE::BSVisit::BSVisitControl::kContinue;
					 });
				 }
				 auto* first = RE::PlayerCharacter::GetSingleton()->Get3D(true);
				 return json{ { "rootCulled", root && root->GetAppCulled() }, { "rootFlags", root ? std::format("{:08X}", root->GetFlags().underlying()) : "" },
							  { "firstFlags", first ? std::format("{:08X}", first->GetFlags().underlying()) : "" }, { "parts", parts }, { "firstPersonSame", first == root } };
			 } } },
			{ "actor_cull", { [](const json&) { return ::Render::Frame::ActorCullStats(); } } },
			// Diagnostics: what the HUD's activation prompt says right now (screenshots don't show
			// the HUD), and what is under the crosshair.
			{ "hud_prompt", { [](const json&) {
				 json  result;
				 auto* ui = RE::UI::GetSingleton();
				 auto  menu = ui ? ui->GetMenu(RE::HUDMenu::MENU_NAME) : nullptr;
				 if (menu && menu->uiMovie) {
					 const auto read = [&](const char* a_path) -> json {
						 RE::GFxValue value;
						 if (!menu->uiMovie->GetVariable(&value, a_path)) {
							 return nullptr;
						 }
						 if (value.IsString()) {
							 return value.GetString();
						 }
						 if (value.IsNumber()) {
							 return value.GetNumber();
						 }
						 if (value.IsBool()) {
							 return value.GetBool();
						 }
						 return "?";
					 };
					 result["text"] = read("_root.HUDMovieBaseInstance.RolloverText.text");
					 result["htmlText"] = read("_root.HUDMovieBaseInstance.RolloverText.htmlText");
					 result["alpha"] = read("_root.HUDMovieBaseInstance.RolloverText._alpha");
					 result["visible"] = read("_root.HUDMovieBaseInstance.RolloverText._visible");
					 result["buttonAlpha"] = read("_root.HUDMovieBaseInstance.RolloverButton_tf._alpha");
					 result["buttonVisible"] = read("_root.HUDMovieBaseInstance.RolloverButton_tf._visible");
				 }
				 auto* pick = RE::CrosshairPickData::GetSingleton();
				 auto  target = pick ? pick->GetActiveTarget().get() : nullptr;
				 result["textHookCalls"] = Portal::Doors::HidePrompt();
				 result["target"] = target ? std::format("{:08X}", target->GetFormID()) : "";
				 return result;
			 } } },
			// Diagnostics: the sky's precipitation (rain, snow) and where its geometry hangs.
			{ "precip", { [](const json&) {
				 auto* sky = RE::Sky::GetSingleton();
				 auto* precip = sky ? sky->precip : nullptr;
				 json  result{ { "mode", sky ? static_cast<int>(sky->mode.underlying()) : -1 }, { "precip", precip != nullptr },
					 { "density", precip ? json{ precip->currentParticleDensity, precip->lastParticleDensity, precip->lastCubeSize } : json() },
					 { "weather", sky && sky->currentWeather ? std::format("{:08X}", sky->currentWeather->GetFormID()) : "" } };
				 if (precip) {
					 for (const auto& [name, geometry] : { std::pair{ "current", precip->currentPrecip.get() }, std::pair{ "last", precip->lastPrecip.get() } }) {
						 if (!geometry) {
							 result[name] = nullptr;
							 continue;
						 }
						 std::string chain;
						 for (auto* up = geometry->parent; up; up = up->parent) {
							 chain += std::format(" < '{}'", up->name.c_str());
						 }
						 auto* shader = geometry->GetGeometryRuntimeData().shaderProperty.get();
						 if (auto* particle = netimmerse_cast<RE::BSParticleShaderProperty*>(shader); particle && particle->particleEmitter) {
							 auto* emitter = static_cast<RE::BSParticleShaderCubeEmitter*>(particle->particleEmitter);
							 result[std::string(name) + "Emitter"] = json{ { "alpha", emitter->alpha }, { "count", emitter->particleCount }, { "density", emitter->density },
								 { "cubeSize", emitter->cubeSize }, { "type", static_cast<int>(emitter->emitterType.underlying()) } };
						 }
						 result[name] = std::format("'{}' {} flags {:X} at ({:.0f},{:.0f},{:.0f}) culled {} shader {}{}", geometry->name.c_str(), geometry->GetRTTI() ? geometry->GetRTTI()->name : "?",
							 geometry->GetFlags().underlying(), geometry->world.translate.x, geometry->world.translate.y, geometry->world.translate.z, geometry->GetAppCulled(),
							 shader && shader->GetRTTI() ? shader->GetRTTI()->name : "-", chain);
					 }
				 }
				 return result;
			 } } },
			// Diagnostics: the directional light of each registered light scene, and the player cell's
			// directional settings (to check how rotation XY/Z map to a direction).
			{ "lighting", { [](const json&) {
				 json scenes = json::array();
				 for (auto* scene : RE::BSShaderManager::State::GetSingleton().shadowSceneNode) {
					 auto* sun = scene ? scene->GetRuntimeData().sunLight : nullptr;
					 auto* light = sun ? skyrim_cast<RE::NiDirectionalLight*>(sun->light.get()) : nullptr;
					 if (!light) {
						 scenes.push_back(nullptr);
						 continue;
					 }
					 const auto& r = light->GetLightRuntimeData();
					 const auto& rt = scene->GetRuntimeData();
					 scenes.push_back({ { "diffuse", { r.diffuse.red, r.diffuse.green, r.diffuse.blue } }, { "fade", r.fade }, { "direction", Vec(light->GetWorldDirection()) },
						 { "lights", rt.activeLights.size() }, { "shadowLights", rt.activeShadowLights.size() } });
				 }
				 auto* cell = RequirePlayer()->GetParentCell();
				 auto* own = cell->IsInteriorCell() ? cell->GetLighting() : nullptr;
				 auto* lt = cell->GetRuntimeData().lightingTemplate;
				 return json{
					 { "scenes", scenes },
					 { "cell", CellJson(cell) },
					 { "directional", own ? json{ { "xy", own->directionalXY }, { "z", static_cast<std::int32_t>(own->directionalZ) }, { "fade", own->directionalFade },
											   { "inherit", own->lightingTemplateInheritanceFlags.underlying() }, { "templateXY", lt ? lt->data.directionalXY : 0 },
											   { "templateZ", lt ? static_cast<std::int32_t>(lt->data.directionalZ) : 0 }, { "templateFade", lt ? lt->data.directionalFade : 0.0f } }
										: json(nullptr) },
				 };
			 } } },
			// The water system's lists: each water object with the reference its shape belongs to, and
			// each reflection with the material it holds and whether a listed water object still has it.
			// TESWaterObject: +0x20 shape, +0x40 reflection; TESWaterReflections: +0x14 plane, +0x40
			// material (from TESWaterSystem::AddWater / RemoveWater, IDs 32179 / 32182).
			{ "water", { [](const json&) {
				 auto*       system = RE::TESWaterSystem::GetSingleton();
				 const auto  at = [](const void* a_object, std::size_t a_offset) { return *reinterpret_cast<void* const*>(reinterpret_cast<const std::byte*>(a_object) + a_offset); };
				 const auto  refCount = [](const void* a_object) { return *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::byte*>(a_object) + 8); };
				 std::vector<const void*> materials;
				 json                     objects = json::array();
				 for (auto& object : system->waterObjects) {
					 auto* shape = static_cast<RE::NiAVObject*>(at(object.get(), 0x20));
					 auto* geometry = shape ? shape->AsGeometry() : nullptr;
					 auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
					 if (property) {
						 materials.push_back(property->material);
					 }
					 RE::TESObjectREFR* ref = nullptr;
					 for (auto* node = shape; node && !ref; node = node->parent) {
						 ref = node->GetUserData();
					 }
					 auto*       cell = ref ? ref->GetParentCell() : nullptr;
					 std::string above;
					 for (auto* node = shape ? shape->parent : nullptr; node && !ref; node = node->parent) {
						 above += std::format(" < {} '{}'", node->GetRTTI() ? node->GetRTTI()->name : "?", node->name.c_str());
					 }
					 objects.push_back(std::format("{:X} shape {:X} '{}' refs {} in scene {}{} | ref {:08X} {} | reflection {:X} material {:X}", reinterpret_cast<std::uintptr_t>(object.get()),
						 reinterpret_cast<std::uintptr_t>(shape), shape ? shape->name.c_str() : "", shape ? refCount(shape) : 0, shape && shape->parent != nullptr, above, ref ? ref->GetFormID() : 0,
						 cell ? Probe::DescribeCell(cell) : "", reinterpret_cast<std::uintptr_t>(at(object.get(), 0x40)), reinterpret_cast<std::uintptr_t>(property ? property->material : nullptr)));
				 }
				 json reflections = json::array();
				 for (auto& reflection : system->waterReflections) {
					 const auto* plane = reinterpret_cast<const float*>(reinterpret_cast<const std::byte*>(reflection.get()) + 0x14);
					 const void* material = at(reflection.get(), 0x40);
					 reflections.push_back(std::format("{:X} refs {} plane ({:.2f} {:.2f} {:.2f} {:.1f}) material {:X}{}", reinterpret_cast<std::uintptr_t>(reflection.get()), refCount(reflection.get()),
						 plane[0], plane[1], plane[2], plane[3], reinterpret_cast<std::uintptr_t>(material), std::ranges::find(materials, material) != materials.end() ? "" : " (no water object has it)"));
				 }
				 return json{ { "enabled", system->enabled }, { "worldSpace", system->worldSpace ? system->worldSpace->GetFormEditorID() : "" }, { "objects", objects },
					 { "reflections", reflections } };
			 } } },
			// Read-only and independent of game state, so they work even during loading screens.
			{ "re_export_ids", { ReverseEngineering::ExportIDs, false } },
			{ "re_read", { ReverseEngineering::ReadMemory, false } },
			{ "re_call", { ReverseEngineering::CallFunction } },
			// Samples the main thread now, for {seconds} (default 3), into {file}: a steady view, where
			// travel_profile starts at a crossing.
			// Off the main thread: it samples the main thread from its own, also when that one hangs.
			{ "profile", { [](const json& a) {
				 const auto file = a.at("file").get<std::string>();
				 // Said, not passed over: a hang reported "sampled into" a file that never appeared.
				 if (!Sampler::Start(std::filesystem::path(file), a.value("seconds", 3.0))) {
					 throw std::runtime_error("not sampling: a sample is already running, or the main thread isn't known yet (no player update so far)");
				 }
				 return json{ { "file", file } };
			 }, false } },
		});
	}
}
