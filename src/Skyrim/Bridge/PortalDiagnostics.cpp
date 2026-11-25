#include "Skyrim/Bridge/PortalDiagnostics.h"

#include "Skyrim/Diag/Probe.h"
#include "Skyrim/Doors/DoorUse.h"
#include "Skyrim/Doors/Hold.h"
#include "Skyrim/Doors/Pair.h"
#include "Skyrim/Pictures/DoorwaySnapshot.h"
#include "Skyrim/Portal/LightOcclusion.h"
#include "Skyrim/Portal/LightScene.h"
#include "Skyrim/Portal/MergedCull.h"
#include "Skyrim/Portal/MergedView.h"
#include "Skyrim/Portal/NpcTravel.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/Selection.h"
#include "Skyrim/Portal/StandIns.h"
#include "Skyrim/Portal/StreetMerge.h"
#include "Skyrim/Render/Backend.h"
#include "Skyrim/Render/SceneMeter.h"
#include "Skyrim/Spaces/RemoteCell.h"

using namespace Portal;

namespace Bridge::PortalDiagnostics
{
	json DescribeAdaptation()
	{
		json rooms = json::array();
		for (const auto& room : MergedView::Rooms()) {
			if (room.door) {
				const auto brightness = Portal::Snapshot::RoomBrightness(room.door);
				rooms.push_back({ { "door", std::format("{:08X}", room.door) }, { "brightness", brightness ? json(*brightness) : json() },
					{ "lightScale", 1.0f + (MergedView::StreetEyeScale(room.door) - 1.0f) * room.daylight }, { "daylight", room.daylight } });
			}
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
			{ "doorwayPictures", Portal::Snapshot::Describe() },
			{ "remoteStepping", RemoteCell::SteppingEnabled() },
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
			{ "remote", Manager::Space().Describe() },
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
			{ "lightScenes", LightScene::DescribeAll(Manager::Space().Cell3D()) },
			{ "warm", [] {
				 json out = json::array();
				 for (const auto& space : Selection::Kept()) {
					 out.push_back({ { "cell", Probe::DescribeCell(space.Cell()) }, { "healthy", space.Healthy() }, { "fullyLoaded", space.FullyLoaded() },
						 { "lights", space.AdoptedLights() }, { "paused", space.Paused() } });
				 }
				 return json{ { "max", Selection::ShownRooms() }, { "distance", Selection::kWarmDistance }, { "sparse", Selection::Sparse() }, { "reach", Selection::WarmReach() },
					 { "activate", Selection::ActivateRadius() }, { "spaces", out }, { "targets", Selection::TargetCount() } };
			 }() },
			{ "shownRooms", [] {
				 json beside = json::array();
				 for (const auto& room : MergedView::Rooms()) {
					 if (!room.slot) {
						 continue;
					 }
					 const auto* space = Selection::KeptSpace(room.cell);
					 beside.push_back({ { "cell", Probe::DescribeCell(room.cell) }, { "door", std::format("{:08X}", room.sourceDoor) }, { "slot", room.slot },
						 { "swing", room.swing }, { "shapes", room.shapes }, { "seen", room.seen }, { "paused", room.paused },
						 { "lightScene", space ? LightScene::Describe(space->Scene(), space->Cell3D()) : json(nullptr) } });
				 }
				 json parked = json::array();
				 for (const auto& space : Selection::Kept()) {
					 if (space.Parked()) {
						 parked.push_back(Probe::DescribeCell(space.Cell()));
					 }
				 }
				 return json{ { "setting", Selection::ShownRooms() }, { "pauseUnseen", MergedView::PauseUnseenRooms() }, { "beside", beside }, { "parked", parked },
					 { "backendRooms", ::Render::Backend::MergedRooms() } };
			 }() },
			{ "lightOcclusion", LightOcclusion::Describe() },
			{ "mergedCull", MergedCull::Describe() },
			{ "render", ::Render::Backend::Describe() },
			{ "npcTravel", NpcTravel::Status() },
			// Which Havok world the engine steps (*ID 400440) against the player's and the remote one.
			{ "havok", [] {
				 static REL::Relocation<RE::bhkWorld**> current{ REL::RelocationID(514280, 400440) };
				 auto*      here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
				 const auto ptr = [](const void* a_p) { return std::format("{:X}", reinterpret_cast<std::uintptr_t>(a_p)); };
				 return json{ { "stepped", ptr(*current) }, { "playerCell", ptr(here ? here->GetbhkWorld() : nullptr) },
					 { "remote", ptr(Manager::Space().Cell() ? Manager::Space().Cell()->GetbhkWorld() : nullptr) } };
			 }() },
		};
	}
}
