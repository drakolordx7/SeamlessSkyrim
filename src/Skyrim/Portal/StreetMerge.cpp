#include "Skyrim/Portal/StreetMerge.h"

#include "Skyrim/Diag/Telemetry.h"
#include "Skyrim/Doors/Pair.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/StreetLOD.h"
#include "Skyrim/Portal/WaterReflections.h"
#include "Skyrim/Render/Backend.h"
#include "Skyrim/Spaces/Placement.h"
#include "Skyrim/Spaces/RemoteCell.h"

namespace Portal::StreetMerge
{
	namespace
	{
		// The street's distant land, objects and trees (BGSTerrainManager's nodes under "LODRoot"),
		// shown in the interior and moved with the street.
		struct MergedLOD
		{
			RE::NiPointer<RE::NiNode>                                   root;
			RE::NiPointer<RE::NiNode>                                   parent;  // its own (the world scene)
			RE::NiTransform                                             local;
			std::vector<std::pair<RE::NiPointer<RE::NiAVObject>, bool>> culled;
			// The persistent references' nodes (RemoteCell::AdoptPersistent) moved and hung in with the
			// street, and where they were.
			struct Moved
			{
				RE::NiPointer<RE::NiAVObject> node;
				RE::NiPointer<RE::NiNode>     parent;
				RE::NiTransform               local;
			};
			std::vector<Moved>        persistent;
			RE::NiTransform           toSource;
			RE::NiPointer<RE::NiNode> branch;  // the interior's, where the street hangs (null: under the world scene)
			Portal::MovedBounds       bounds;  // of the LOD root (the engine's outdoor cells with it) and the persistent nodes
		};
		std::optional<MergedLOD> mergedLOD;

		void HangInto(RE::NiNode* a_branch, RE::NiAVObject* a_object)
		{
			RE::NiPointer<RE::NiAVObject> keep(a_object);
			const auto                    world = a_object->world;
			if (a_object->parent) {
				a_object->parent->DetachChild(a_object);
			}
			a_branch->AttachChild(a_object, true);
			a_object->local = a_branch->world.Invert() * world;
			RE::NiUpdateData placed;
			a_object->Update(placed);
		}

		// Indoors the engine draws the sky's dome only in an interior that shows the sky (Sky::mode
		// kSkyDomeOnly: the Bannered Mare). Elsewhere (Understone Keep) the sky is in interior mode and
		// culled, and the street merged in had none behind it, only the clear colour. Such an interior
		// is given the dome the way the engine gives it to one that shows the sky (Sky::SetMode), for
		// as long as the street is merged.
		struct SkyModeChange
		{
			RE::TESObjectCELL* interior;
			RE::Sky::Mode      before;
		};
		std::optional<SkyModeChange> skyModeChange;

		void SetSkyMode(RE::Sky::Mode a_mode)
		{
			static REL::Relocation<void (*)(RE::Sky*, std::uint32_t)> setMode{ REL::RelocationID(25679, 26222) };
			setMode(RE::Sky::GetSingleton(), static_cast<std::uint32_t>(a_mode));
		}

		void ShowSkyDome()
		{
			auto* sky = RE::Sky::GetSingleton();
			if (skyModeChange || sky->mode.get() != RE::Sky::Mode::kInterior) {
				return;
			}
			skyModeChange = SkyModeChange{ RE::PlayerCharacter::GetSingleton()->GetParentCell(), sky->mode.get() };
			SetSkyMode(RE::Sky::Mode::kSkyDomeOnly);
		}

		void RestoreSkyMode()
		{
			if (!skyModeChange) {
				return;
			}
			// Only in the interior it was changed for: walked out, the engine has set the outdoors' mode.
			if (RE::PlayerCharacter::GetSingleton()->GetParentCell() == skyModeChange->interior && RE::Sky::GetSingleton()->mode.get() == RE::Sky::Mode::kSkyDomeOnly) {
				SetSkyMode(skyModeChange->before);
			}
			skyModeChange.reset();
		}
	}

	void EveryFrame()
	{
		Portal::WaterReflections::Update();
		// Community Shaders' skylighting starts afresh around the street (its probes are kept by
		// position, and the player's jumps from the interior's coordinates to the street's) and
		// fades in over seconds: the street under roofs and porches was far too bright, darkening
		// slowly. A load door hides that behind its loading screen; here it is settled at once.
		auto*       cell = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		static bool wasInside = false;
		const bool  inside = cell && cell->IsInteriorCell();
		if (cell && wasInside && !inside) {
			::Render::Backend::SettleSkylighting();
		}
		if (cell) {
			wasInside = inside;
		}
	}

	void PlaceLOD(const Active& a_portal, RE::TESObjectCELL* a_here)
	{
		if (const auto mapping = Doors::TryCompute(a_portal); mapping && a_here->IsInteriorCell() && !a_portal.cell->IsInteriorCell()) {
			Portal::StreetLOD::Show(a_portal.cell, mapping->toLinked, false);
		}
	}

	void HideLOD()
	{
		Portal::StreetLOD::Hide();
	}

	bool Merged()
	{
		return Manager::Space().Merged() && Manager::Space().Cell() && !Manager::Space().Cell()->IsInteriorCell();
	}

	// A town's houses, porches and paving are persistent references: their 3D hangs in a node of
	// the worldspace's persistent cell, not in the cells merged with the street, and moved with
	// none of them. Walked out and back in (the engine's grid had them, and took them off when
	// the player went inside), Belethor's porch and the market's paving were missing from the view
	// out, the bare land showing in their place. Nodes adopted later are taken on here as well.
	void MergePersistent()
	{
		if (!mergedLOD) {
			return;
		}
		int taken = 0;
		for (auto& node : Manager::Space().BorrowedPersistent()) {
			const bool known = std::ranges::any_of(mergedLOD->persistent, [&](const MergedLOD::Moved& a_moved) { return a_moved.node == node; });
			// Only while it still hangs where AdoptPersistent put it. 3D we loaded (PersistentIn) can
			// be hung into its cell by the engine afterwards (a mod's persistent references placed in
			// the street's cell): it moves with the cell, and moved here as well it stood the move's
			// distance beyond, out of sight (Markarth's mines: barrels, crates, pipes, scaffolding).
			if (known || node->parent != Manager::Space().Scene()) {
				continue;
			}
			mergedLOD->persistent.push_back({ node, RE::NiPointer<RE::NiNode>(node->parent), node->local });
			mergedLOD->bounds.Capture(node.get());
			mergedLOD->bounds.UnfixBeforeMove(node.get());
			node->local = mergedLOD->toSource * node->local;
			// HangInto keeps the node where it is drawn now: not updated, that was still the street's
			// place, and the moved ground and rocks stood there, out of the doorway's view.
			RE::NiUpdateData update;
			node->Update(update);
			mergedLOD->bounds.Move(node.get(), mergedLOD->toSource);
			if (mergedLOD->branch) {
				HangInto(mergedLOD->branch.get(), node.get());
			}
			Portal::SettleMotion(node.get());
			++taken;
		}
		if (taken) {
			logger::info("portal: {} node(s) of persistent references merged with the street", taken);
		}
	}

	void Show(const RE::NiTransform& a_toSource, RE::TESObjectREFR* a_door)
	{
		Portal::StreetLOD::Show(Manager::Space().Cell(), a_toSource.Invert(), true);
		ShowSkyDome();
		auto* land = RE::BGSTerrainManager::GetLODRoot().get();
		auto* root = land ? land->parent : nullptr;
		if (!root || !root->parent) {
			logger::warn("portal: no LOD root to show in the interior");
			return;
		}
		MergedLOD state{ RE::NiPointer<RE::NiNode>(root), RE::NiPointer<RE::NiNode>(root->parent), root->local, {} };
		for (auto* node : { RE::BGSTerrainManager::GetLODLandRoot().get(), RE::BGSTerrainManager::GetLODObjectRoot().get(), RE::BGSTerrainManager::GetTreeNode().get() }) {
			if (node) {
				state.culled.emplace_back(RE::NiPointer<RE::NiAVObject>(node), node->GetAppCulled());
				node->SetAppCulled(false);
			}
		}
		// The LOD root's parent is the engine's objects root (TES::objRoot), which holds the outdoor
		// grid's cells as well when the player walked in from the street: everything in it moves.
		const auto moveBy = a_toSource * root->local.Invert();
		state.bounds.Capture(root);
		state.bounds.UnfixBeforeMove(root);
		root->local = a_toSource;
		// Updated before HangInto below, which keeps it where it is drawn now.
		RE::NiUpdateData update;
		root->Update(update);
		state.bounds.Move(root, moveBy);
		// Hung into the interior's statics branch (the one holding the most shapes), the street and
		// the LOD are culled into the main view with the interior's own contents. Left under the
		// world scene, only the sky was drawn through the doorway.
		// An interior built of rooms (Riften's jail) is culled through its portal graph: what lies
		// outside its rooms is tested against the view through the rooms' portals, and the street
		// beyond the door is outside all of them (only the sky, the water and the street's
		// particles were drawn). There the street hangs in the room of the door it is seen through,
		// and is culled with that room's view.
		auto* here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		auto* loaded = here ? here->GetRuntimeData().loadedData : nullptr;
		auto* interior3D = loaded && loaded->cell3D ? loaded->cell3D->AsNode() : nullptr;
		RE::NiNode* branch = nullptr;
		std::size_t most = 0;
		for (auto* up = a_door && a_door->Get3D() ? a_door->Get3D()->parent : nullptr; up && !branch; up = up->parent) {
			if (auto* room = netimmerse_cast<RE::BSMultiBoundRoom*>(up)) {
				branch = room;
				most = room->GetChildren().size();
			}
		}
		if (interior3D && !branch) {
			for (auto& child : interior3D->GetChildren()) {
				auto* node = child ? child->AsNode() : nullptr;
				if (node && node->GetChildren().size() > most) {
					most = node->GetChildren().size();
					branch = node;
				}
			}
		}
		// In a room, the tree LOD (464 instanced shapes looking out of Mistveil Keep at Riften) came
		// into the sun's shadow map (Interior Sun) twice now and then; that batch's list then looped,
		// its last pass leading back to its first, and the game hung drawing it. Not through the
		// shadow maps' own culls of the LOD (left out, it still came in), nor through the main view's
		// lists (the shapes never pass through them): what adds them twice isn't known. Without the
		// distant trees the view out lacks only those (the street's own trees are real ones).
		if (netimmerse_cast<RE::BSMultiBoundRoom*>(branch)) {
			RE::BGSTerrainManager::GetTreeNode()->SetAppCulled(true);
		}
		if (branch) {
			HangInto(branch, root);
			for (auto* cell : Manager::Space().Cells()) {
				auto* cellLoaded = cell->GetRuntimeData().loadedData;
				if (auto* cell3D = cellLoaded ? cellLoaded->cell3D.get() : nullptr) {
					HangInto(branch, cell3D);
				}
			}
			state.branch.reset(branch);
		}
		state.toSource = a_toSource;
		logger::info("portal: distant LOD shown in the interior{}", branch ? std::format(", it and the street hung into the interior's {} of {} objects", netimmerse_cast<RE::BSMultiBoundRoom*>(branch) ? "room at the door" : "branch", most) : std::string());
		// The upscaler's motion vectors: moved in one step, the distant land and objects reached back to
		// where they were drawn outdoors, and with DLSS the view out was low resolution and shimmered.
		Portal::SettleMotion(root);
		mergedLOD = std::move(state);
		MergePersistent();
	}

	void Hide()
	{
		const Diag::Scope scope("unmerge LOD");
		HideLOD();
		RestoreSkyMode();
		if (!mergedLOD) {
			return;
		}
		for (auto& [node, parent, local] : mergedLOD->persistent | std::views::reverse) {
			if (node->parent != parent.get()) {
				if (node->parent) {
					node->parent->DetachChild(node.get());
				}
				parent->AttachChild(node.get(), true);
			}
			node->local = local;
			RE::NiUpdateData update;
			node->Update(update);
			Portal::RecomputeBounds(node.get());
			Portal::SettleMotion(node.get());
		}
		for (auto& [node, culled] : mergedLOD->culled) {
			node->SetAppCulled(culled);
		}
		auto& root = mergedLOD->root;
		if (root->parent != mergedLOD->parent.get()) {
			if (root->parent) {
				root->parent->DetachChild(root.get());
			}
			mergedLOD->parent->AttachChild(root.get(), true);
		}
		root->local = mergedLOD->local;
		RE::NiUpdateData update;
		root->Update(update);
		Portal::RecomputeBounds(root.get());
		Portal::SettleMotion(root.get());
		mergedLOD->bounds.Restore();
		mergedLOD.reset();
	}

	std::vector<RE::NiAVObject*> Roots()
	{
		std::vector<RE::NiAVObject*> roots;
		if (mergedLOD) {
			roots.push_back(mergedLOD->root.get());
			for (auto& moved : mergedLOD->persistent) {
				roots.push_back(moved.node.get());
			}
		}
		return roots;
	}

	void UnfixBounds(RE::NiAVObject* a_root)
	{
		if (mergedLOD) {
			mergedLOD->bounds.Unfix(a_root);
		}
	}

	std::optional<RE::NiPoint3> RootAt()
	{
		return mergedLOD ? std::optional(mergedLOD->root->world.translate) : std::nullopt;
	}
}
