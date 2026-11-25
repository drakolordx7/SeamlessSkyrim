#include "Skyrim/Doors/DoorUse.h"

#include "Skyrim/Doors/Geometry.h"
#include "Skyrim/Engine/SameSpace.h"

namespace
{
	std::atomic<Portal::DoorUse::Force> force{ Portal::DoorUse::Force::kNone };

	struct Entry
	{
		RE::FormID door;
		RE::FormID linked;
		float      left;  // seconds until it swings shut, counted only while nobody is in it
	};
	// Under usedLock: the player's use is the main thread's, the bridge's activate another's.
	std::recursive_mutex usedLock;
	std::vector<Entry>   used;

	std::mutex              actorUsesLock;
	std::vector<RE::FormID> actorUses;  // under actorUsesLock

	std::mutex              openLock;
	std::vector<RE::FormID> open;  // under openLock

	// Someone standing this close to a door (horizontally, at about its height) is in its doorway.
	constexpr float kDoorwayReach = 130.0f;
	constexpr float kDoorwayHeight = 200.0f;

	using Engine::SameSpace;

	bool InDoorway(RE::FormID a_door, const std::vector<RE::Actor*>& a_actors)
	{
		auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_door);
		if (!door || !door->Get3D()) {
			return false;
		}
		const auto center = Portal::Doors::Center(door);
		return std::ranges::any_of(a_actors, [&](RE::Actor* a_actor) {
			const auto offset = a_actor->GetPosition() - center;
			return SameSpace(a_actor->GetParentCell(), door->GetParentCell()) && offset.x * offset.x + offset.y * offset.y < kDoorwayReach * kDoorwayReach &&
			       std::abs(offset.z) < kDoorwayHeight;
		});
	}
}

namespace Portal::DoorUse
{
	Force GetForce()
	{
		return force.load();
	}

	void SetForce(Force a_force)
	{
		force = a_force;
	}

	bool Use(RE::FormID a_door, RE::FormID a_linked)
	{
		std::scoped_lock lock(usedLock);
		const auto it = std::ranges::find_if(used, [&](const Entry& a_used) { return a_used.door == a_door || a_used.linked == a_door; });
		if (it != used.end()) {
			it->left = kOpenSeconds;
			return false;
		}
		used.push_back({ a_door, a_linked, kOpenSeconds });
		logger::info("portal: door {:08X} used: opens", a_door);
		return true;
	}

	bool Shut(RE::FormID a_door)
	{
		std::scoped_lock lock(usedLock);
		if (std::erase_if(used, [&](const Entry& a_used) { return a_used.door == a_door || a_used.linked == a_door; }) == 0) {
			return false;
		}
		logger::info("portal: door {:08X} used: shuts", a_door);
		return true;
	}

	bool Used(RE::FormID a_door)
	{
		std::scoped_lock lock(usedLock);
		return std::ranges::any_of(used, [&](const Entry& a_used) { return a_used.door == a_door || a_used.linked == a_door; });
	}

	std::vector<RE::FormID> UsedDoors()
	{
		std::scoped_lock lock(usedLock);
		std::vector<RE::FormID> doors;
		for (const auto& entry : used) {
			doors.push_back(entry.door);
			doors.push_back(entry.linked);
		}
		return doors;
	}

	void PlayDoorSound(RE::FormID a_door, RE::FormID a_linked, bool a_open)
	{
		auto* here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		for (const auto id : { a_door, a_linked }) {
			auto* door = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
			auto* base = door ? door->GetBaseObject() : nullptr;
			auto* form = base ? base->As<RE::TESObjectDOOR>() : nullptr;
			auto* sound = form ? (a_open ? form->openSound : form->closeSound) : nullptr;
			if (!sound || !door->Get3D() || !SameSpace(door->GetParentCell(), here)) {
				continue;
			}
			RE::BSSoundHandle handle;
			if (RE::BSAudioManager::GetSingleton()->GetSoundHandle(handle, sound)) {
				handle.SetPosition(door->GetPosition());
				handle.SetObjectToFollow(door->Get3D());
				handle.Play();
			}
			return;
		}
	}

	void NoteActorUse(RE::FormID a_door)
	{
		std::scoped_lock lock(actorUsesLock);
		if (std::ranges::find(actorUses, a_door) == actorUses.end()) {
			actorUses.push_back(a_door);
		}
	}

	std::vector<RE::FormID> TakeActorUses()
	{
		std::scoped_lock lock(actorUsesLock);
		return std::exchange(actorUses, {});
	}

	std::vector<std::pair<RE::FormID, RE::FormID>> Update(float a_delta)
	{
		std::scoped_lock                               lock(usedLock);
		std::vector<std::pair<RE::FormID, RE::FormID>> shut;
		if (used.empty()) {
			return shut;
		}
		std::vector<RE::Actor*> actors{ RE::PlayerCharacter::GetSingleton() };
		for (auto& handle : RE::ProcessLists::GetSingleton()->highActorHandles) {
			if (auto actor = handle.get(); actor && !actor->IsDead()) {
				actors.push_back(actor.get());
			}
		}
		std::erase_if(used, [&](Entry& a_used) {
			if (InDoorway(a_used.door, actors) || InDoorway(a_used.linked, actors)) {
				a_used.left = kOpenSeconds;
				return false;
			}
			if ((a_used.left -= a_delta) > 0.0f) {
				return false;
			}
			logger::info("portal: door {:08X} swings shut, nobody in it for {:.0f} s", a_used.door, kOpenSeconds);
			shut.emplace_back(a_used.door, a_used.linked);
			return true;
		});
		return shut;
	}

	void PublishOpen(std::vector<RE::FormID> a_doors)
	{
		std::scoped_lock lock(openLock);
		open = std::move(a_doors);
	}

	bool StandsOpen(RE::FormID a_door)
	{
		std::scoped_lock lock(openLock);
		return std::ranges::find(open, a_door) != open.end();
	}

	void Clear()
	{
		{
			std::scoped_lock lock(usedLock);
			used.clear();
		}
		{
			std::scoped_lock lock(actorUsesLock);
			actorUses.clear();
		}
		PublishOpen({});
	}

	json Status()
	{
		std::scoped_lock lock(usedLock);
		json doors = json::array();
		for (const auto& entry : used) {
			doors.push_back({ { "door", std::format("{:08X}", entry.door) }, { "linked", std::format("{:08X}", entry.linked) }, { "left", entry.left } });
		}
		return { { "force", static_cast<int>(GetForce()) }, { "used", doors } };
	}
}
