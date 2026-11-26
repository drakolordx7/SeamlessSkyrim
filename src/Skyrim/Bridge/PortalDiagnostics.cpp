#include "Skyrim/Bridge/PortalDiagnostics.h"

#include "Skyrim/Bridge/CommandSupport.h"
#include "Skyrim/Diag/Probe.h"
#include "Skyrim/Doors/DoorUse.h"
#include "Skyrim/Doors/Hold.h"
#include "Skyrim/Doors/Pair.h"
#include "Skyrim/Graph/DoorScan.h"
#include "Skyrim/Pictures/Cache.h"
#include "Skyrim/Pictures/DoorwaySnapshot.h"
#include "Skyrim/Portal/Crossing.h"
#include "Skyrim/Portal/LightOcclusion.h"
#include "Skyrim/Portal/LightScene.h"
#include "Skyrim/Portal/MergedCull.h"
#include "Skyrim/Portal/MergedView.h"
#include "Skyrim/Portal/NpcTravel.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/Schedule.h"
#include "Skyrim/Portal/Selection.h"
#include "Skyrim/Portal/StandIns.h"
#include "Skyrim/Portal/StreetMerge.h"
#include "Skyrim/Render/Enb.h"
#include "Skyrim/Render/Native/Native.h"
#include "Skyrim/Render/SceneMeter.h"
#include "Skyrim/Spaces/RemoteCell.h"

using namespace Portal;

namespace Bridge::PortalDiagnostics
{
	json DescribeAdaptation()
	{
		json rooms = json::array();
		if (const auto room = MergedView::Shown(); room && room->door) {
			const auto brightness = Pictures::Cache::RoomBrightness(room->door);
			rooms.push_back({ { "door", std::format("{:08X}", room->door) }, { "brightness", brightness ? json(*brightness) : json() },
				{ "lightScale", 1.0f + (MergedView::StreetEyeScale(room->door) - 1.0f) * room->away }, { "away", room->away } });
		}
		const auto scene = ::Render::SceneMeter::Brightness();
		return { { "sceneBrightness", scene ? json(*scene) : json() }, { "rooms", rooms }, { "streetEye", true } };
	}

	json MergedShapesReport(const RE::NiPoint3& a_near, float a_far)
	{
		const auto shapes = MergedView::Shapes();
		json out = json::array();
		for (auto* shape : shapes) {
			auto* geometry = const_cast<RE::BSGeometry*>(shape);
			const auto& world = geometry->world.translate;
			int farBones = 0, bones = 0;
			if (auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get()) {
				bones = static_cast<int>(skin->skinData ? skin->skinData->bones : 0);
				for (int i = 0; i < bones; ++i) {
					if (auto* bone = skin->bones[i]; bone && bone->world.translate.GetDistance(a_near) > a_far) {
						++farBones;
					}
				}
			}
			const bool isFar = world.GetDistance(a_near) > a_far;
			if (isFar || farBones) {
				auto* top = geometry->parent;
				while (top && top->parent && !top->GetUserData()) {
					top = top->parent;
				}
				auto* ref = top ? top->GetUserData() : nullptr;
				out.push_back(std::format("{} '{}' ref {:08X} pos ({:.0f} {:.0f} {:.0f}) bones {}/{} far", geometry->GetRTTI() ? geometry->GetRTTI()->name : "?", geometry->name.c_str(),
					ref ? ref->GetFormID() : 0, world.x, world.y, world.z, farBones, bones));
			}
		}
		return out;
	}

	json PutPiecesOutOfPlace(float a_fraction)
	{
		int seen = 0, moved = 0;
		for (auto* cell : Manager::Space().Cells()) {
			cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				auto* base = a_ref && !a_ref->IsDisabled() && !a_ref->As<RE::Actor>() ? a_ref->GetBaseObject() : nullptr;
				auto* object = base && base->Is(RE::FormType::Static, RE::FormType::MovableStatic, RE::FormType::Furniture, RE::FormType::Container, RE::FormType::Flora) ? a_ref->Get3D() : nullptr;
				if (!object || (seen++ % 100) >= static_cast<int>(a_fraction * 100.0f)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				++moved;
				// What the loader computes for a 3D with no parent yet: each node's world is its parent's within the piece.
				const auto place = [](auto&& self, RE::NiAVObject* a_object, const RE::NiTransform& a_parent) -> void {
					a_object->world = a_parent * a_object->local;
					a_object->previousWorld = a_object->world;
					if (auto* node = a_object->AsNode()) {
						for (auto& child : node->GetChildren()) {
							if (child) {
								self(self, child.get(), a_object->world);
							}
						}
					}
				};
				place(place, object, RE::NiTransform());
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		return { { "pieces", seen }, { "putOutOfPlace", moved } };
	}

	json SceneCheck(float a_radius)
	{
		// Things that stand still where their reference stands (no physics, no animation, not doors,
		// which the portal hides or swings): what a merge or a hand-over that went wrong leaves
		// hanging nowhere or in the wrong place.
		const auto still3D = [](RE::TESObjectREFR* a_ref) -> RE::NiAVObject* {
			auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
			if (!base || a_ref->IsDisabled() || a_ref->IsDeleted() || a_ref->As<RE::Actor>() ||
				!base->Is(RE::FormType::Static, RE::FormType::MovableStatic, RE::FormType::Tree, RE::FormType::Flora, RE::FormType::Furniture, RE::FormType::Container) ||
				static_cast<RE::TESObject*>(base)->IsMarker()) {
				return nullptr;
			}
			return a_ref->Get3D();
		};
		// In an interior built of rooms, what lies outside them hangs nowhere and is drawn through
		// the graph of the player's cell (ENGINE.md, Culling).
		auto*      playerLoaded = RE::PlayerCharacter::GetSingleton()->GetParentCell()->GetRuntimeData().loadedData;
		const auto* graph = playerLoaded ? playerLoaded->portalGraph.get() : nullptr;
		const auto inScene = [&](const RE::NiAVObject* a_object) {
			while (a_object->parent) {
				a_object = a_object->parent;
			}
			const auto listed = [&](const auto& a_list) { return std::ranges::any_of(a_list, [&](const auto& a_child) { return a_child.get() == a_object; }); };
			return a_object == RE::Main::WorldRootNode() || (graph && (listed(graph->alwaysRenderChildren) || listed(graph->pendingAlwaysRenderAdds)));
		};
		struct Part
		{
			int                      checked = 0;
			std::vector<std::string> problems;
		};
		// a_toWorld: where the space's own coordinates are drawn.
		const auto checkRef = [&](Part& a_part, RE::TESObjectREFR* a_ref, const RE::NiTransform& a_toWorld) {
			auto* object = still3D(a_ref);
			if (!object) {
				return;
			}
			++a_part.checked;
			const auto expected = a_toWorld * a_ref->GetPosition();
			const auto& at = object->world.translate;
			auto*       model = a_ref->GetBaseObject()->As<RE::TESModel>();
			const auto  name = std::format("{:08X} '{}'", a_ref->GetFormID(), model ? model->GetModel() : "");
			if (!inScene(object)) {
				a_part.problems.push_back(std::format("{} hangs nowhere", name));
			} else if (at.GetDistance(expected) > 50.0f) {
				a_part.problems.push_back(std::format("{} drawn at ({:.0f} {:.0f} {:.0f}), belongs at ({:.0f} {:.0f} {:.0f})", name, at.x, at.y, at.z, expected.x, expected.y, expected.z));
			}
		};
		// A door drawn without its leaves (Pristine): shapes hidden that no opener hid.
		const auto checkDoor = [&](Part& a_part, RE::TESObjectREFR* a_ref) {
			auto* base = a_ref->GetBaseObject();
			if (!base || !base->Is(RE::FormType::Door) || a_ref->IsDisabled() || !a_ref->Get3D() || static_cast<RE::TESObject*>(base)->IsMarker()) {
				return;
			}
			++a_part.checked;
			if (const auto hidden = Doors::HiddenByNoOpener(a_ref)) {
				a_part.problems.push_back(std::format("door {:08X}: {} shape(s) hidden by no opener", a_ref->GetFormID(), hidden));
			}
		};
		const auto checkCellRefs = [&](Part& a_part, RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_center, const RE::NiTransform& a_toWorld) {
			a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				if (a_ref && a_ref->GetPosition().GetDistance(a_center) <= a_radius) {
					checkRef(a_part, a_ref, a_toWorld);
					checkDoor(a_part, a_ref);
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		};
		const auto partJson = [](const Part& a_part) {
			const auto shown = std::min<std::size_t>(a_part.problems.size(), 40);
			return json{ { "checked", a_part.checked }, { "problemCount", a_part.problems.size() },
				{ "problems", std::vector<std::string>(a_part.problems.begin(), a_part.problems.begin() + shown) } };
		};

		// The player's own space, drawn where it stands.
		auto*      player = RE::PlayerCharacter::GetSingleton();
		auto*      here = player->GetParentCell();
		const auto center = player->GetPosition();
		Part       own;
		std::vector<RE::TESObjectCELL*> ownCells{ here };
		if (auto* tes = RE::TES::GetSingleton(); !here->IsInteriorCell() && tes->gridCells) {
			ownCells.clear();
			for (std::uint32_t x = 0; x < tes->gridCells->length; ++x) {
				for (std::uint32_t y = 0; y < tes->gridCells->length; ++y) {
					if (auto* cell = tes->gridCells->GetCell(x, y); cell && cell->IsAttached()) {
						ownCells.push_back(cell);
					}
				}
			}
			if (auto* world = here->GetRuntimeData().worldSpace; world && world->persistentCell) {
				ownCells.push_back(world->persistentCell);
			}
		}
		for (auto* cell : ownCells) {
			checkCellRefs(own, cell, center, RE::NiTransform());
		}
		json result{ { "cell", Probe::DescribeCell(here) }, { "here", partJson(own) }, { "merged", nullptr } };
		bool ok = own.problems.empty();

		// The space merged behind the door, drawn moved to the door.
		const auto portal = Manager::CurrentPortal();
		if (portal && Manager::Space().Merged()) {
			const auto toSource = MergedView::PlacementOf(*portal, Doors::Compute(*portal)).toSource;
			auto*      linked = RE::TESForm::LookupByID<RE::TESObjectREFR>(portal->linkedDoor);
			const auto linkedAt = linked->GetPosition();
			const bool street = !Manager::Space().Cell()->IsInteriorCell();
			Part       merged;
			for (auto* cell : Manager::Space().Cells()) {
				checkCellRefs(merged, cell, linkedAt, toSource);
			}
			if (auto* world = street ? Manager::Space().Cell()->GetRuntimeData().worldSpace : nullptr; world && world->persistentCell) {
				checkCellRefs(merged, world->persistentCell, linkedAt, toSource);
			}
			if (street) {
				// The land of each cell, by where its quads are drawn (horizontally: the move turns
				// only about the vertical).
				for (auto* cell : Manager::Space().Cells()) {
					auto* land = cell->GetRuntimeData().cellLand;
					auto* data = land ? land->loadedData : nullptr;
					auto* coordinates = cell->GetCoordinates();
					const auto cellCenter = toSource * RE::NiPoint3{ (coordinates->cellX + 0.5f) * 4096.0f, (coordinates->cellY + 0.5f) * 4096.0f, 0.0f };
					for (int quad = 0; data && quad < 4; ++quad) {
						auto* mesh = data->mesh[quad];
						if (!mesh) {
							continue;
						}
						++merged.checked;
						const auto& c = mesh->worldBound.center;
						if (!inScene(mesh)) {
							merged.problems.push_back(std::format("land of {:08X} quad {} hangs nowhere", cell->GetFormID(), quad));
						} else if (std::hypot(c.x - cellCenter.x, c.y - cellCenter.y) > 3000.0f) {
							merged.problems.push_back(std::format("land of {:08X} quad {} drawn around ({:.0f} {:.0f}), belongs around ({:.0f} {:.0f})", cell->GetFormID(), quad, c.x, c.y, cellCenter.x, cellCenter.y));
						}
					}
				}
				if (const auto rootAt = StreetMerge::RootAt(); !rootAt) {
					merged.problems.push_back("the distant LOD isn't shown with the street");
				} else if (rootAt->GetDistance(toSource.translate) > 1.0f) {
					const auto& at = *rootAt;
					merged.problems.push_back(std::format("the distant LOD drawn at ({:.0f} {:.0f} {:.0f}), belongs at ({:.0f} {:.0f} {:.0f})", at.x, at.y, at.z, toSource.translate.x,
						toSource.translate.y, toSource.translate.z));
				}
			}
			ok &= merged.problems.empty();
			result["merged"] = partJson(merged);
			result["merged"]["space"] = Probe::DescribeCell(Manager::Space().Cell());
			result["merged"]["street"] = street;
		}
		result["ok"] = ok;
		return result;
	}

	json Status()
	{
		const auto portal = Manager::CurrentPortal();
		return {
			{ "enabled", Manager::Enabled() },
			{ "merged", Manager::Merged() },
			{ "mergeEnabled", MergedView::MergeEnabled() },
			{ "doorUse", Portal::DoorUse::Status() },
			{ "standIns", [] {
				 json out = json::array();
				 for (const auto& [id, standIn] : StandIns::Doorways()) {
					 out.push_back({ { "door", std::format("{:08X}", id) }, { "swinging", std::format("{:08X}", standIn.swinging) }, { "swing", standIn.swing.Amount() },
						 { "fade", standIn.fade }, { "handedOver", standIn.handedOver }, { "outdoors", standIn.outdoors }, { "cameThrough", standIn.cameThrough } });
				 }
				 return out;
			 }() },
			{ "doorwayPictures", Pictures::Describe() },
			{ "remoteStepping", Spaces::RemoteCell::SteppingEnabled() },
			{ "active", portal ? json{ { "sourceDoor", std::format("{:08X}", portal->sourceDoor) }, { "linkedDoor", std::format("{:08X}", portal->linkedDoor) }, { "mapping", [&] {
										 try {
											 const auto m = Portal::Doors::Compute(*portal);
											 return json{ { "center", { m.sourceCenter.x, m.sourceCenter.y, m.sourceCenter.z } }, { "inward", { m.inward.x, m.inward.y } },
												 { "halfWidth", m.halfWidth }, { "halfDepth", m.halfDepth }, { "yawDelta", m.yawDelta } };
										 } catch (const std::exception& e) {
											 return json(e.what());
										 }
									 }() } }
								: json(nullptr) },
			{ "remote", DescribeSpace(Manager::Space()) },
			{ "lod", [] {
				 const auto count = [](RE::NiNode* a_node) {
					 int shapes = 0;
					 if (a_node) {
						 RE::BSVisit::TraverseScenegraphGeometries(a_node, [&](RE::BSGeometry*) {
							 ++shapes;
							 return RE::BSVisit::BSVisitControl::kContinue;
						 });
					 }
					 return a_node ? shapes : -1;
				 };
				 const auto* active = RE::BGSTerrainManager::GetActiveManager();
				 auto* tes = RE::TES::GetSingleton();
				 return json{ { "active", active && active->worldSpace ? active->worldSpace->GetFormEditorID() : "" }, { "grid", { tes->currentGridX, tes->currentGridY } },
					 { "staticDataLoaded", active && active->staticDataLoaded }, { "land", count(RE::BGSTerrainManager::GetLODLandRoot().get()) },
					 { "objects", count(RE::BGSTerrainManager::GetLODObjectRoot().get()) }, { "trees", count(RE::BGSTerrainManager::GetTreeNode().get()) } };
			 }() },
			{ "lastEvent", Manager::LastEvent() },
			// The shown space's people and their process levels, as the kept spaces' ("warm").
			{ "shownActors", Manager::Space().Attached() ? Manager::Space().DescribeActors() : json(nullptr) },
			{ "lightScenes", LightScene::DescribeAll(Manager::Space().Cell3D()) },
			// The spaces kept loaded out of sight (tier 3 of the scheduler).
			{ "warm", [] {
				 json out = json::array();
				 for (const auto& space : Schedule::Kept()) {
					 out.push_back({ { "cell", Probe::DescribeCell(space.Cell()) }, { "healthy", space.Healthy() }, { "fullyLoaded", space.FullyLoaded() },
						 { "lights", space.AdoptedLights() }, { "paused", space.Paused() }, { "actors", space.DescribeActors() } });
				 }
				 return json{ { "max", Schedule::LoadedRooms() }, { "spaces", out } };
			 }() },
			// The door graph's counts and the scheduler's state (see also the graph_summary command).
			{ "graph", Graph::Summary() },
			{ "schedule", Schedule::Status() },
			{ "crossingPhase", Crossing::PhaseName() },
			{ "lightOcclusion", LightOcclusion::Describe() },
			{ "mergedCull", MergedCull::Describe() },
			{ "render", json{ { "ready", ::Render::Native::Ready() }, { "enb", ::Render::Enb::Describe() }, { "mask", ::Render::Native::DescribeMask() },
								{ "lighting", ::Render::Native::DescribeLighting() } } },
			{ "npcTravel", NpcTravel::Status() },
			// Which Havok world the engine steps (*ID 400440) against the player's and the remote one.
			{ "havok", [] {
				 static REL::Relocation<RE::bhkWorld**> current{ REL::RelocationID(514280, 400440) };
				 auto*      here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
				 return json{ { "stepped", Bridge::Support::Address(*current) }, { "playerCell", Bridge::Support::Address(here ? here->GetbhkWorld() : nullptr) },
					 { "remote", Bridge::Support::Address(Manager::Space().Cell() ? Manager::Space().Cell()->GetbhkWorld() : nullptr) } };
			 }() },
		};
	}

	json RenderPasses(const Spaces::RemoteCell& a_space, bool a_drop)
	{
		static const auto [begin, end] = [] {
			const auto& module = REL::Module::get();
			return std::pair{ module.base(), module.base() + module.segment(REL::Segment::rdata).offset() + module.segment(REL::Segment::rdata).size() };
		}();
		int shapes = 0, lists = 0, passes = 0, stale = 0, foreign = 0, dropped = 0;
		int         ownFade = 0, otherFade = 0;
		std::string sample;
		const auto check = [&](RE::BSShaderProperty::RenderPassArray& a_list, RE::BSGeometry* a_shape) {
			if (!a_list.head) {
				return;
			}
			++lists;
			bool bad = false;
			int  guard = 0;
			for (auto* pass = a_list.head; pass && guard < 64; pass = pass->next, ++guard) {
				++passes;
				const auto vtable = pass->shader ? *reinterpret_cast<const std::uintptr_t*>(pass->shader) : 0;
				if (vtable < begin || vtable >= end) {
					++stale;
					bad = true;
					break;
				}
				if (pass->geometry != a_shape) {
					++foreign;
					bad = true;
					break;
				}
			}
			if (bad && a_drop) {
				a_list.head = nullptr;
				++dropped;
			}
		};
		const auto visit = [&](RE::NiAVObject* a_root) {
			RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* a_shape) {
				++shapes;
				if (auto* shader = a_shape->GetGeometryRuntimeData().shaderProperty.get()) {
					RE::BSFadeNode* owner = nullptr;
					for (auto* up = a_shape->parent; up && !owner; up = up->parent) {
						owner = up->AsFadeNode();
					}
					if (shader->fadeNode != owner) {
						++otherFade;
						if (sample.empty()) {
							sample = std::format("'{}' fadeNode {:X} owner {:X} '{}'", a_shape->name.c_str(), reinterpret_cast<std::uintptr_t>(shader->fadeNode),
								reinterpret_cast<std::uintptr_t>(owner), owner ? owner->name.c_str() : "");
						}
					} else {
						++ownFade;
					}
					check(shader->renderPassList, a_shape);
					check(shader->debugRenderPassList, a_shape);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		};
		nlohmann::json tree = nlohmann::json::array();
		for (auto* cell : a_space.Cells()) {
			if (auto* cell3D = Spaces::RemoteCell::Cell3D(cell)) {
				visit(cell3D);
				std::string line = std::format("{:08X} flags {:X} parent '{}'{}:", cell->GetFormID(), cell3D->GetFlags().underlying(), cell3D->parent ? cell3D->parent->name.c_str() : "",
					LightScene::IsOurs(cell3D->parent) ? " (portal scene)" : "");
				if (auto* node = cell3D->AsNode()) {
					for (auto& child : node->GetChildren()) {
						auto* childNode = child ? child->AsNode() : nullptr;
						int below = 0;
						if (child) {
							RE::BSVisit::TraverseScenegraphGeometries(child.get(), [&](RE::BSGeometry*) {
								++below;
								return RE::BSVisit::BSVisitControl::kContinue;
							});
						}
						line += child ? std::format(" [{} {:X} n={} shapes={}]", child->name.c_str(), child->GetFlags().underlying(), childNode ? childNode->GetChildren().size() : 0, below) : " [null]";
					}
				}
				tree.push_back(line);
			}
		}
		for (const auto& top : a_space.BorrowedPersistent()) {
			visit(top.get());
		}
		return { { "shapes", shapes }, { "lists", lists }, { "passes", passes }, { "stale", stale }, { "foreign", foreign }, { "dropped", dropped }, { "cells", tree },
			{ "ownFade", ownFade }, { "otherFade", otherFade }, { "sample", sample } };
	}

	json DescribeSpace(const Spaces::RemoteCell& a_space)
	{
		auto* cell = a_space.Cell();
		if (!cell) {
			return nullptr;
		}
		json actors = json::array();
		for (auto* actor : Spaces::RemoteCell::ActorsIn(cell)) {
			const auto pos = actor->GetPosition();
			actors.push_back({
				{ "name", actor->GetDisplayFullName() },
				{ "has3D", actor->Get3D() != nullptr },
				{ "pos", { pos.x, pos.y, pos.z } },
			});
		}
		return {
			{ "cell", Probe::DescribeCell(cell) },
			{ "neighbours", a_space.Cells().size() - 1 },
			{ "healthy", a_space.Healthy() },
			{ "fullyLoaded", a_space.FullyLoaded() },
			{ "loadState", [cell] {
				 auto* loaded = cell->GetRuntimeData().loadedData;
				 return loaded ? json{ { "refsFullyLoaded", loaded->refsFullyLoaded }, { "queued", loaded->queuedRefCount }, { "criticalQueued", loaded->criticalQueuedRefCount }, { "distantQueued", loaded->queuedDistantRefCount } } : json(nullptr);
			 }() },
			{ "adoptedLights", a_space.AdoptedLights() },
			{ "fade", [&a_space] {
				 // Diagnostics: BSFadeNodes by current fade (0 = invisible, 1 = fully shown).
				 std::uint32_t hidden = 0, partial = 0, shown = 0;
				 std::function<void(RE::NiAVObject*)> visit = [&](RE::NiAVObject* a_object) {
					 if (!a_object) {
						 return;
					 }
					 if (auto* fade = a_object->AsFadeNode()) {
						 const float value = fade->GetRuntimeData().currentFade;
						 (value <= 0.0f ? hidden : value >= 1.0f ? shown : partial)++;
					 }
					 if (auto* node = a_object->AsNode()) {
						 for (auto& child : node->GetChildren()) {
							 visit(child.get());
						 }
					 }
				 };
				 visit(a_space.Cell3D());
				 return json{ { "hidden", hidden }, { "partial", partial }, { "shown", shown } };
			 }() },
			{ "steppedFrames", Spaces::RemoteCell::SteppedFrames() },
			{ "actors", actors },
		};
	}
}
