#include "Skyrim/Doors/Hold.h"

#include "Skyrim/Doors/Geometry.h"
#include "Skyrim/Doors/Host.h"
#include "Skyrim/Portal/Pristine.h"

namespace Portal::Doors
{
	namespace
	{
		// One door can be held open twice at once: as the portal door of the space the player is in
		// and as the door they just came out of (Travel). Its original collision is recorded by the
		// first opener and restored when the last one lets go.
		struct Shared
		{
			int                                                                  opens = 0;
			int                                                                  hides = 0;
			int                                                                  passables = 0;
			int                                                                  walkables = 0;  // openers not solid (OpenState::solid)
			std::vector<std::pair<RE::NiPointer<RE::NiAVObject>, RE::COL_LAYER>> layers;
			std::vector<RE::NiPointer<RE::BSGeometry>>                           hiddenShapes;
			std::vector<RE::NiPointer<RE::BSGeometry>>                           behindShapes;
			// Taken out of the Havok world while the door is passable (HideBodies), and the 3D they
			// belong to: put back only into that.
			std::vector<RE::NiPointer<RE::bhkWorldObject>>                       removedBodies;
			RE::NiPointer<RE::NiAVObject>                                        bodiesOf;
			// The 3D all of the above was done to (see Rebuilt).
			RE::NiPointer<RE::NiAVObject>                                        root;
		};

		// The layer change alone didn't stop a city gate: Whiterun's gate kept the player 25 units
		// short of its doorway (in first person often just short enough for the crossing, which needs
		// the camera at the picture, not to happen), and it stopped rays as well.
		void HideBodies(RE::NiAVObject* a_door3D, Shared& a_shared)
		{
			a_shared.bodiesOf.reset(a_door3D);
			RE::BSVisit::TraverseScenegraphObjects(a_door3D, [&](RE::NiAVObject* a_object) {
				auto* collision = a_object->collisionObject ? a_object->collisionObject->AsBhkNiCollisionObject() : nullptr;
				if (collision && collision->body && collision->body->GetWorld1()) {
					a_shared.removedBodies.emplace_back(collision->body);
					collision->body->RemoveFromCurrentWorld();
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		void ShowBodies(RE::TESObjectREFR* a_door, Shared& a_shared)
		{
			auto* cell = a_door ? a_door->GetParentCell() : nullptr;
			auto* world = cell ? cell->GetbhkWorld() : nullptr;
			// Only into the 3D they came from, still standing in an attached cell: a cell let go in the
			// meantime has taken its world apart, and a reloaded door has bodies of its own.
			if (world && a_door->Get3D() == a_shared.bodiesOf.get() && cell->cellState.get() == RE::TESObjectCELL::CellState::kAttached) {
				for (auto& body : a_shared.removedBodies) {
					if (!body->GetWorld1()) {
						body->MoveToWorld(world);
					}
				}
			}
			a_shared.removedBodies.clear();
			a_shared.bodiesOf.reset();
		}
		std::unordered_map<RE::FormID, Shared> openDoors;

		// What stands behind the panel only to hide the void there would now block the view through
		// the doorway; it went round with the panel when that was swung as a whole.
		void CullBehindPanel(RE::TESObjectREFR* a_door, Shared& a_shared)
		{
			for (auto& shape : BehindPanel(a_door)) {
				if (!shape->GetAppCulled()) {
					Pristine::Hide(shape.get());
					a_shared.behindShapes.push_back(shape);
				}
			}
		}

		void MakeWalkable(RE::TESObjectREFR* a_door, RE::NiAVObject* a_door3D, Shared& a_shared)
		{
			RE::BSVisit::TraverseScenegraphObjects(a_door3D, [&](RE::NiAVObject* a_object) {
				if (a_object->collisionObject) {
					const auto layer = a_object->GetCollisionLayer();
					if (layer != RE::COL_LAYER::kUnidentified && layer != RE::COL_LAYER::kNonCollidable) {
						a_shared.layers.emplace_back(RE::NiPointer<RE::NiAVObject>(a_object), layer);
						a_object->SetCollisionLayer(RE::COL_LAYER::kNonCollidable);
						Pristine::LayerChanged(a_object, layer);
					} else {
						logger::info("portal: door {:08X}: collision on '{}' left as it is (layer {})", a_door->GetFormID(), a_object->name.c_str(), static_cast<int>(layer));
					}
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}

		// The leaves only: a frame in the door's mesh fills the wall around the panel (Riften's
		// RiftenRWDoorLoad01), and the other door's frame, standing in the same place, is drawn only
		// beyond the door plane and within the panel's rectangle. Hidden too, the doorway was a hole
		// in the wall wider than the opening. Culling a node alone left the panel on screen while
		// merged; the shapes are culled one by one, as for the player's body (CameraLimit).
		void HideShapes(RE::TESObjectREFR* a_door, Shared& a_shared)
		{
			for (const auto& leaf : LeafNodes(a_door)) {
				RE::BSVisit::TraverseScenegraphGeometries(leaf.get(), [&](RE::BSGeometry* a_shape) {
					if (!a_shape->GetAppCulled()) {
						Pristine::Hide(a_shape);
						a_shared.hiddenShapes.emplace_back(a_shape);
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
		}

		// The engine built the door's 3D anew while it was held open (disabled and enabled by a
		// script, its cell's 3D reloaded): the new panel stood in the doorway, drawn and solid, while
		// every opener took the door for open. What they asked for is done to the new 3D; the
		// recorded shapes and bodies were the old one's and went with it.
		void Rebuilt(RE::TESObjectREFR* a_door, RE::NiAVObject* a_door3D, Shared& a_shared)
		{
			// The old 3D may live on as what the next door of its model is copied from (Pristine).
			for (auto& shape : a_shared.behindShapes) {
				Pristine::Show(shape.get());
			}
			for (auto& shape : a_shared.hiddenShapes) {
				Pristine::Show(shape.get());
			}
			for (auto& [object, layer] : a_shared.layers) {
				object->SetCollisionLayer(layer);
				Pristine::LayerBack(object.get());
			}
			a_shared.behindShapes.clear();
			a_shared.layers.clear();
			a_shared.hiddenShapes.clear();
			a_shared.removedBodies.clear();
			a_shared.bodiesOf.reset();
			a_shared.root.reset(a_door3D);
			CullBehindPanel(a_door, a_shared);
			if (a_shared.walkables > 0) {
				MakeWalkable(a_door, a_door3D, a_shared);
			}
			if (a_shared.hides > 0) {
				HideShapes(a_door, a_shared);
			}
			if (a_shared.passables > 0) {
				HideBodies(a_door3D, a_shared);
			}
			logger::info("portal: door {:08X}'s 3D was rebuilt while open: opened again", a_door->GetFormID());
		}
	}

	bool IsOpen(RE::FormID a_door)
	{
		return openDoors.contains(a_door);
	}

	std::uint32_t HiddenByNoOpener(RE::TESObjectREFR* a_door)
	{
		const auto    open = openDoors.find(a_door->GetFormID());
		std::uint32_t count = 0;
		RE::BSVisit::TraverseScenegraphGeometries(a_door->Get3D(), [&](RE::BSGeometry* a_shape) {
			const auto ours = [&](const std::vector<RE::NiPointer<RE::BSGeometry>>& a_list) { return std::ranges::contains(a_list, a_shape, &RE::NiPointer<RE::BSGeometry>::get); };
			if (a_shape->GetAppCulled() && (open == openDoors.end() || (!ours(open->second.hiddenShapes) && !ours(open->second.behindShapes)))) {
				++count;
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		return count;
	}

	bool BodiesHidden(RE::FormID a_door)
	{
		const auto it = openDoors.find(a_door);
		return it != openDoors.end() && !it->second.removedBodies.empty();
	}

	namespace
	{
		// Standing open, not to be shut by using it: no prompt.
		bool PromptOff(RE::FormID a_door)
		{
			return GetHost().prompt(a_door) == DoorPrompt::kNone;
		}

		// TESObjectDOOR::GetActivateText (vfunc 0x4C) builds the "Open <destination>" line for the door
		// under the crosshair. For a door held open by a portal the text is left empty here, or says
		// "Close" for one the player opened (using it shuts it, DoorActivateHook).
		struct ActivateTextHook
		{
			static inline std::uint32_t calls = 0;
			static bool Thunk(RE::TESObjectDOOR* a_door, RE::TESObjectREFR* a_activator, RE::BSString& a_text)
			{
				++calls;
				auto* pick = RE::CrosshairPickData::GetSingleton();
				const auto target = pick ? pick->GetActiveTarget().get() : nullptr;
				if (target && PromptOff(target->GetFormID())) {
					a_text = "";
					return false;
				}
				const bool result = func(a_door, a_activator, a_text);
				if (target && GetHost().prompt(target->GetFormID()) == DoorPrompt::kClose) {
					auto*             settings = RE::GameSettingCollection::GetSingleton();
					auto*             open = settings->GetSetting("sOpen");
					auto*             close = settings->GetSetting("sClose");
					const std::string text = a_text.c_str();
					if (const auto at = text.find(open->GetString()); at != std::string::npos) {
						a_text = (text.substr(0, at) + close->GetString() + text.substr(at + std::strlen(open->GetString()))).c_str();
					}
				}
				return result;
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};
	}

	// The text hook above alone left the prompt on the HUD (measured: "Open Riverwood Trader"
	// with the crosshair on a hidden, open door), so the HUD is also told, the way the engine
	// tells it when the crosshair leaves an object.
	std::uint32_t HidePrompt()
	{
		auto*      pick = RE::CrosshairPickData::GetSingleton();
		const auto target = pick ? pick->GetActiveTarget().get() : nullptr;
		if (target && PromptOff(target->GetFormID())) {
			auto* ui = RE::UI::GetSingleton();
			auto  menu = ui ? ui->GetMenu(RE::HUDMenu::MENU_NAME) : nullptr;
			if (menu && menu->uiMovie) {
				RE::GFxValue text;
				if (menu->uiMovie->GetVariable(&text, "_root.HUDMovieBaseInstance.RolloverText.text") && text.IsString() && text.GetString()[0] != 0) {
					const RE::GFxValue args[2]{ false, "" };
					menu->uiMovie->Invoke("_root.HUDMovieBaseInstance.SetCrosshairTarget", nullptr, args, 2);
				}
			}
		}
		return ActivateTextHook::calls;
	}

	void InstallPromptHook()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_TESObjectDOOR[0] };
		ActivateTextHook::func = vtable.write_vfunc(0x4C, ActivateTextHook::Thunk);
	}

	void Open(RE::TESObjectREFR* a_door, OpenState& a_state, OpenOptions a_options)
	{
		auto* door3D = a_door ? a_door->Get3D() : nullptr;
		if (!door3D) {
			return;
		}
		if (const auto open = openDoors.find(a_door->GetFormID()); open != openDoors.end() && open->second.root.get() != door3D) {
			Rebuilt(a_door, door3D, open->second);
		}
		if (a_state.door == a_door->GetFormID() && a_state.hidden == a_options.hide && a_state.passable == a_options.passable && a_state.solid == a_options.solid) {
			return;
		}
		Close(a_state);
		a_state.door = a_door->GetFormID();
		a_state.solid = a_options.solid;
		auto& shared = openDoors[a_state.door];
		if (shared.opens++ == 0) {
			shared.root.reset(door3D);
			CullBehindPanel(a_door, shared);
		}
		if (!a_options.solid && shared.walkables++ == 0) {
			// The doorway can be walked through. Using the door is the portal's (EngineHooks' DoorActivateHook):
			// shut it, or nothing, never the ordinary transition with its loading screen.
			MakeWalkable(a_door, door3D, shared);
		}
		if (a_options.hide) {
			if (shared.hides++ == 0) {
				HideShapes(a_door, shared);
			}
			a_state.hidden = true;
		}
		if (a_options.passable) {
			if (shared.passables++ == 0) {
				HideBodies(door3D, shared);
			}
			a_state.passable = true;
		}
		logger::info("portal: door {:08X} open ({} collision objects, {} shape(s) behind the panel off, {} opener(s){}{})", a_state.door, shared.layers.size(), shared.behindShapes.size(), shared.opens, a_options.hide ? ", hidden" : "",
			a_options.passable ? std::format(", passable: {} bodies out", shared.removedBodies.size()) : "");
	}

	void Close(OpenState& a_state)
	{
		const auto it = a_state.door ? openDoors.find(a_state.door) : openDoors.end();
		if (it != openDoors.end()) {
			auto& shared = it->second;
			auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_state.door);
			if (a_state.hidden && --shared.hides == 0) {
				for (auto& shape : shared.hiddenShapes) {
					Pristine::Show(shape.get());
				}
				shared.hiddenShapes.clear();
			}
			if (a_state.passable && --shared.passables == 0) {
				ShowBodies(door, shared);
			}
			if (!a_state.solid && --shared.walkables == 0) {
				for (auto& [object, layer] : shared.layers) {
					object->SetCollisionLayer(layer);
					Pristine::LayerBack(object.get());
				}
				shared.layers.clear();
			}
			if (--shared.opens == 0) {
				for (auto& shape : shared.behindShapes) {
					Pristine::Show(shape.get());
				}
				openDoors.erase(it);
			}
		}
		a_state = {};
	}
}
