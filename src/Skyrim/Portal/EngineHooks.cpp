#include "Skyrim/Portal/EngineHooks.h"

#include "Skyrim/Doors/DoorUse.h"
#include "Skyrim/Doors/Geometry.h"
#include "Skyrim/Doors/Hold.h"
#include "Skyrim/Doors/Policy.h"
#include "Skyrim/Engine/Hooks.h"
#include "Skyrim/Portal/Crossing.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/StandIns.h"

namespace Portal::EngineHooks
{
	namespace
	{
		// How far beyond the camera's distance from the player a door is reached (the engine's
		// activation reach is about this).
		constexpr float kUseReach = 180.0f;

		// An open doorway has no collision for the crosshair to find its door by (Doors::Open): a door
		// opened by a use is found by its doorway, which the crosshair passes through, to be shut.
		void PickOpenDoorway(RE::CrosshairPickData* a_pick, const RE::NiPoint3& a_origin, const RE::NiPoint3& a_direction)
		{
			auto*      player = RE::PlayerCharacter::GetSingleton();
			auto*      here = player->GetParentCell();
			const auto used = Portal::DoorUse::UsedDoors();
			if (used.empty() || !here) {
				return;
			}
			auto direction = a_direction;
			direction.Unitize();
			auto* const raw = reinterpret_cast<std::uint8_t*>(a_pick);
			auto&       target = *reinterpret_cast<RE::ObjectRefHandle*>(raw + 0x04);
			auto&       targetActor = *reinterpret_cast<RE::ObjectRefHandle*>(raw + 0x08);
			auto&       point = *reinterpret_cast<RE::NiPoint3*>(raw + 0x10);
			float       nearest = target.get() ? point.GetDistance(a_origin) : kUseReach + a_origin.GetDistance(player->GetPosition() + RE::NiPoint3{ 0.0f, 0.0f, 120.0f });
			RE::TESObjectREFR* found = nullptr;
			RE::NiPoint3       at;
			for (const auto id : used) {
				auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				if (!door || !door->Get3D() || !StandIns::InPlayerSpace(door, here) || !Portal::Doors::IsOpen(id) || !Portal::Doors::ClosableDoor(door)) {
					continue;
				}
				const auto  c = Portal::Doors::ShownQuad(door, a_origin);
				const auto  across = c[1] - c[0];
				const auto  up = c[2] - c[0];
				const auto  normal = across.Cross(up);
				const float facing = normal.Dot(direction);
				if (std::abs(facing) < 1e-4f) {
					continue;
				}
				const float distance = normal.Dot(c[0] - a_origin) / facing;
				if (distance <= 0.0f || distance >= nearest) {
					continue;
				}
				const auto  hit = a_origin + direction * distance;
				const float u = (hit - c[0]).Dot(across) / across.SqrLength();
				const float v = (hit - c[0]).Dot(up) / up.SqrLength();
				if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
					continue;
				}
				nearest = distance;
				found = door;
				at = hit;
			}
			if (found) {
				target = found->GetHandle();
				targetActor = {};
				point = at;
			}
		}

		// PlayerCharacter's crosshair update (ID 40620) calling its pick (ID 26127) at +0x1E7.
		struct CrosshairPickHook
		{
			static void Thunk(RE::CrosshairPickData* a_pick, RE::bhkWorld* a_world, RE::NiPoint3* a_origin, RE::NiPoint3* a_direction)
			{
				func(a_pick, a_world, a_origin, a_direction);
				if (a_pick && a_origin && a_direction) {
					PickOpenDoorway(a_pick, *a_origin, *a_direction);
				}
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		// The actor's 3D position update (ID 37995, every frame for each actor in high process, on the
		// job threads) hangs its 3D into its cell's node for actors whenever it hangs elsewhere (ID 19165,
		// which queues the attach). A merged room's people hang directly under their cell's 3D, where the
		// scene's cull reaches them (MergedView): the engine moved them back every frame, and they were
		// hung again every frame. Only MergedView hangs an actor there, so that alone tells them apart,
		// without asking the manager from a job thread. One walked out into the street has a new cell
		// and is hung into it as usual.
		struct ActorHangHook
		{
			static void Thunk(RE::TESObjectCELL* a_cell, RE::TESObjectREFR* a_ref, bool a_arg3, bool a_arg4)
			{
				auto* loaded = a_cell->GetRuntimeData().loadedData;
				auto* object = a_ref->Get3D();
				if (loaded && loaded->cell3D && object && object->parent == loaded->cell3D.get()) {
					return;
				}
				func(a_cell, a_ref, a_arg3, a_arg4);
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		// Using a seamless door opens it, or shuts it again if it was opened by a use; never the
		// engine's trip through it with a loading screen (walking through the open doorway is the
		// trip). One that stands open (a city gate, set to stand open always, all doors forced open)
		// can't be shut. Also right after its lock was picked, when the engine uses the door itself.
		struct DoorActivateHook
		{
			static bool Thunk(RE::TESObjectDOOR* a_door, RE::TESObjectREFR* a_target, RE::TESObjectREFR* a_activator, std::uint8_t a_arg3, RE::TESBoundObject* a_object, std::int32_t a_count)
			{
				if (a_target && a_activator == RE::PlayerCharacter::GetSingleton() && UseDoor(a_target)) {
					return true;
				}
				return func(a_door, a_target, a_activator, a_arg3, a_object, a_count);
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		class ActivateSink final : public RE::BSTEventSink<RE::TESActivateEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				// Opened or shut (DoorActivateHook), not travelled through.
				if (a_event && a_event->objectActivated && a_event->actionRef.get() == RE::PlayerCharacter::GetSingleton() && Portal::Doors::UsedPairOf(a_event->objectActivated.get())) {
					return RE::BSEventNotifyControl::kContinue;
				}
				// The player used a door the normal way.
				if (a_event && a_event->objectActivated && a_event->actionRef.get() == RE::PlayerCharacter::GetSingleton()) {
					Crossing::BeforeEngineTrip(a_event->objectActivated.get());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		} activateSink;

		class CellSink final : public RE::BSTEventSink<RE::BGSActorCellEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::BGSActorCellEvent* a_event, RE::BSTEventSource<RE::BGSActorCellEvent>*) override
			{
				// The player arrived in another space (a door transition, a teleport): the engine owns the
				// cells there now. Walking from one exterior cell into the next is not that.
				const auto portal = Manager::CurrentPortal();
				if (a_event && a_event->flags.all(RE::BGSActorCellEvent::CellFlag::kEnter) && portal) {
					const auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(a_event->cellID);
					// Through a city gate the player arrives in the portal's worldspace, not necessarily
					// in the very cell its door is in.
					const bool portalWorld = cell && portal->cell && !cell->IsInteriorCell() && !portal->cell->IsInteriorCell() &&
					                         cell->GetRuntimeData().worldSpace == portal->cell->GetRuntimeData().worldSpace;
					if (cell && (cell->IsInteriorCell() || cell == portal->cell || portalWorld)) {
						Portal::Manager::ReleaseAll("player entered another space");
						Crossing::Arrived();
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		} cellSink;
	}

	bool UseDoor(RE::TESObjectREFR* a_target)
	{
		auto* linked = Manager::Enabled() ? Portal::Doors::UsedPairOf(a_target) : nullptr;
		if (!linked) {
			return false;
		}
		const auto id = a_target->GetFormID();
		if (Portal::Doors::StandsOpen(a_target, linked)) {
			logger::info("portal: door {:08X} used: stands open", id);
		} else if (Portal::DoorUse::Used(id)) {
			if (Portal::DoorUse::Shut(id)) {
				Portal::DoorUse::PlayDoorSound(id, linked->GetFormID(), false);
			}
		} else if (Portal::DoorUse::Use(id, linked->GetFormID())) {
			Portal::DoorUse::PlayDoorSound(id, linked->GetFormID(), true);
		}
		return true;
	}

	void Install()
	{
		RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESActivateEvent>(&activateSink);
		{
			REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_TESObjectDOOR[0] };
			DoorActivateHook::func = vtable.write_vfunc(0x37, DoorActivateHook::Thunk);
		}
		{
			REL::Relocation<std::uintptr_t> pickSite{ REL::RelocationID(39534, 40620), Hooks::Offset(0x214, 0x1D5, 0x1E7) };
			if (Hooks::CallsTo(pickSite.address(), REL::RelocationID(25591, 26127), "portal: crosshair pick")) {
				CrosshairPickHook::func = SKSE::GetTrampoline().write_call<5>(pickSite.address(), CrosshairPickHook::Thunk);
			}
			REL::Relocation<std::uintptr_t> hangSite{ REL::RelocationID(36970, 37995), REL::Relocate(0x298, 0x279) };
			if (Hooks::CallsTo(hangSite.address(), REL::RelocationID(18683, 19165), "portal: actor hung into its cell")) {
				ActorHangHook::func = SKSE::GetTrampoline().write_call<5>(hangSite.address(), ActorHangHook::Thunk);
			}
		}
		RE::PlayerCharacter::GetSingleton()->AsBGSActorCellEventSource()->AddEventSink(&cellSink);
	}
}
