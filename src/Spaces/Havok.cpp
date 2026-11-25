#include "Spaces/RemoteCell.h"

#include "Diag/Telemetry.h"
#include "Spaces/Tuning.h"

namespace
{
	using Step_t = void (*)(RE::bhkWorld*, std::uint32_t);

	void Step(RE::bhkWorld* a_world, std::uint32_t a_arg)
	{
		// bhkWorld vfunc 0x32 = per-frame step (the only reader of the world's active flag).
		const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(a_world);
		reinterpret_cast<Step_t>(vtable[0x32])(a_world, a_arg);
	}
}

namespace Portal
{
	void RemoteCell::StepWorld(RE::bhkWorld* a_world)
	{
		// Only on a change: the lock is held by the step job for as long as it steps, and the main
		// thread waiting on it every frame would wait for the physics.
		if (a_world == stepping || cells.empty()) {
			return;
		}
		std::scoped_lock lock(stepLock);
		std::erase_if(stepWorlds, [&](const auto& a_entry) { return a_entry.first == cells.front(); });
		if (a_world) {
			stepWorlds.emplace_back(cells.front(), a_world);
		}
		stepping = a_world;
	}

	// Under stepLock: each remote world once, one after the other (Havok isn't built for two
	// worlds stepping at once), never the one the engine just stepped (a_stepped).
	// A street's world (a whole worldspace's) is stepped at the end of the frame whatever the
	// mode, a room's where the mode says: a_inJob tells which of the two places this is. Left
	// to the mode, the street was stepped only with Precision loaded (the mode it sets), and
	// without it the people seen out there from indoors hung in a falling pose.
	int RemoteCell::StepAll(RE::bhkWorld* a_stepped, bool a_inJob)
	{
		std::vector<RE::bhkWorld*> done;
		for (const auto& [cell, world] : stepWorlds) {
			if ((cell->IsInteriorCell() && Spaces::tuning.stepInJob) != a_inJob) {
				continue;
			}
			if (world && world != a_stepped && std::ranges::find(done, world) == done.end()) {
				Step(world, 0);
				done.push_back(world);
			}
		}
		if (!done.empty()) {
			++steppedFrames;
		}
		return static_cast<int>(done.size());
	}

	// The engine steps only the current space's world, in a job (ID 18870 / 18897: *ID 400440,
	// vfunc 0x32). Stepping the remote cell's world from the player update instead ran it
	// alongside that job; Havok isn't built for two worlds stepping at once, and as soon as
	// clutter in a freshly loaded cell was actually simulated (settling buckets and tankards),
	// memory got corrupted and the portal pass drew render passes that had been freed. Stepping
	// it right after the engine's world, in the same job, keeps the steps serialized.
	void RemoteCell::StepInEngineJob(RE::bhkWorld* a_world, std::uint32_t a_arg)
	{
		Step(a_world, a_arg);
		if (!Spaces::tuning.stepInJob) {
			return;
		}
		std::scoped_lock lock(stepLock);
		StepAll(a_world, true);
	}

	// With Precision loaded: its hook in the world step loop (ID 77851 + 0x407) takes a lock that
	// its own main-thread update also holds; a second world stepped inside the engine's step job
	// ran into that and the game froze. At the end of the frame (Main::Update after the jobs are
	// done) nothing else is in the physics or in Precision, and the remote world is stepped there.
	void RemoteCell::StepAtEndOfFrame()
	{
		const Diag::Scope scope("remote step");
		std::scoped_lock  lock(stepLock);
		const auto        started = std::chrono::steady_clock::now();
		// Through a city gate both worldspaces' cells are in one world, the player's: the engine
		// has stepped it this frame.
		auto*       here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		const int   worlds = StepAll(here ? here->GetbhkWorld() : nullptr, false);
		const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
		if (ms > 4.0f) {
			logger::info("portal: stepping {} remote world(s) took {:.1f} ms", worlds, ms);
		}
	}

	void RemoteCell::InstallStepHook(bool a_enabled)
	{
		stepEnabled = a_enabled;
		// `call qword ptr [rax+0x190]` (6 bytes) at +0x18 in both jobs; rcx/edx already hold the
		// world and the argument, so a 5-byte call to the thunk plus a nop replaces it.
		for (const auto id : { REL::RelocationID(18440, 18870), REL::RelocationID(18466, 18897) }) {
			REL::Relocation<std::uintptr_t> site{ id, 0x18 };
			const auto*                     code = reinterpret_cast<const std::uint8_t*>(site.address());
			if (code[0] != 0xFF || code[1] != 0x90 || *reinterpret_cast<const std::uint32_t*>(code + 2) != 0x190) {
				logger::error("portal: Havok step call in ID {} not found, remote cells won't be simulated", id.id());
				continue;
			}
			SKSE::GetTrampoline().write_call<5>(site.address(), StepInEngineJob);
			REL::safe_write(site.address() + 5, std::uint8_t{ 0x90 });
		}
	}
}
