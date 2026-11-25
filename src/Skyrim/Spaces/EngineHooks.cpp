#include "Skyrim/Spaces/RemoteCell.h"

#include "Skyrim/Diag/Probe.h"
#include "Skyrim/Engine/AttachCell.h"
#include "Skyrim/Engine/Hooks.h"
#include "Skyrim/Spaces/WalkOut.h"

namespace
{
	// The engine's attach (ID 18976) installs the cell's room graph in the world light scene
	// (ShadowSceneNode::SetPortalGraph), and a scene whose graph changes drops all its lights
	// (RemoveAllLights) for the engine to take in again, each with a cull of the scene for the
	// shapes it lights. Putting the world's graph back afterwards (another drop) and every light
	// back with AddLight: walking into Understone Keep, its 153 lights went through that twice and
	// the next frame took ~40 ms; the same for the street's lights on walking out, and on each
	// door approached. Attaching a remote cell, the world's graph is left as it is.
	struct AttachGraphHook
	{
		static void Thunk(RE::ShadowSceneNode* a_scene, RE::BSPortalGraph* a_graph)
		{
			if (!Engine::AttachingRemote()) {
				func(a_scene, a_graph);
			}
		}
		static inline REL::Relocation<decltype(Thunk)> func;
	};

	// Detaching an interior (ours on release, or the engine's on a change of space) puts it at
	// the front of the engine's buffer of recent interiors (ID 13435, TES::interiorBuffer) and
	// unloads the one that falls off the end (ID 13721). That can be a cell we still hold
	// attached: its 3D was detached from objRoot by the index it has under our root (0), which
	// took objRoot's first child, the temporary node manager, and the game crashed the next
	// frame in its update (ID 13292); a merged room vanished from the street. A held cell is
	// left loaded and only drops out of the buffer; its own release puts it back in.
	// ID 13721 has some 25 callers, and hooked at the buffer's eviction alone the same crash
	// came from another (walking about Dawnstar with the White Hall kept behind its door): the
	// hook sits at its entry.
	bool KeepHeld(RE::TESObjectCELL* a_cell)
	{
		if (!Spaces::RemoteCell::Holds(a_cell)) {
			return false;
		}
		logger::info("portal: {} kept loaded, not unloaded by the engine (held behind a door)", Probe::DescribeCell(a_cell));
		return true;
	}

	void InstallUnloadHook()
	{
		// Its first 9 bytes, test rdx, rdx / je to its ret, are redone by the stub.
		REL::Relocation<std::uintptr_t> unload{ REL::RelocationID(13623, 13721) };
		const auto*                     code = reinterpret_cast<const std::uint8_t*>(unload.address());
		std::int32_t                    toEnd = 0;
		std::memcpy(&toEnd, code + 5, sizeof(toEnd));
		if (code[0] != 0x48 || code[1] != 0x85 || code[2] != 0xD2 || code[3] != 0x0F || code[4] != 0x84 || code[9 + toEnd] != 0xC3) {
			logger::warn("portal: ID 13721 isn't as expected; held rooms can be unloaded by the engine (a crash)");
			return;
		}
		std::array<std::uint8_t, 59> stub{
			0x51, 0x52, 0x41, 0x50, 0x41, 0x51,  // push rcx, rdx, r8, r9
			0x48, 0x83, 0xEC, 0x28,              // sub rsp, 28h
			0x48, 0x89, 0xD1,                    // mov rcx, rdx (the cell)
			0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rax, KeepHeld
			0xFF, 0xD0,                          // call rax
			0x48, 0x83, 0xC4, 0x28,              // add rsp, 28h
			0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,  // pop r9, r8, rdx, rcx
			0x84, 0xC0,                          // test al, al
			0x75, 0x13,                          // jnz ret
			0x48, 0x85, 0xD2,                    // test rdx, rdx (the original's)
			0x74, 0x0E,                          // jz ret
			0xFF, 0x25, 0, 0, 0, 0,              // jmp [rip]
			0, 0, 0, 0, 0, 0, 0, 0,              //   ID 13721 + 9
			0xC3                                 // ret
		};
		const auto keep = reinterpret_cast<std::uintptr_t>(&KeepHeld);
		const auto resume = unload.address() + 9;
		std::memcpy(stub.data() + 15, &keep, sizeof(keep));
		std::memcpy(stub.data() + 50, &resume, sizeof(resume));
		auto& trampoline = SKSE::GetTrampoline();
		auto* copy = trampoline.allocate(stub.size());
		std::memcpy(copy, stub.data(), stub.size());
		trampoline.write_branch<5>(unload.address(), reinterpret_cast<std::uintptr_t>(copy));
	}
}

namespace Spaces
{
	bool RemoteCell::Holds(const RE::TESObjectCELL* a_cell)
	{
		return a_cell && std::ranges::find(held, a_cell) != held.end();
	}

	void RemoteCell::InstallGridHook()
	{
		InstallUnloadHook();
		REL::Relocation<std::uintptr_t> graphSite{ REL::RelocationID(18518, 18976), REL::Relocate(0x135, 0x140) };
		if (Hooks::CallsTo(graphSite.address(), REL::RelocationID(99741, 106385), "portal: room graph on attach")) {
			AttachGraphHook::func = SKSE::GetTrampoline().write_call<5>(graphSite.address(), AttachGraphHook::Thunk);
		}
		Spaces::WalkOut::Install();
	}
}
