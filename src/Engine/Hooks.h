#pragma once

#include <Windows.h>

#include <cstddef>
#include <string_view>

namespace Hooks
{
	// An offset that moves between builds: inside an engine function, or into a struct the plugin
	// checks CommonLib against. SE 1.5.97; AE before 1.7.99 (read off 1.6.1170); AE from 1.7.99
	// (read off 1.7.104), where the player's runtime data moved by 8 bytes (CommonLib's
	// RUNTIME_SSE_1_7_99) and functions moved with it.
	inline std::ptrdiff_t Offset(std::ptrdiff_t a_se, std::ptrdiff_t a_ae, std::ptrdiff_t a_ae1799)
	{
		if (!REL::Module::IsAE()) {
			return a_se;
		}
		return REL::Module::get().version() >= SKSE::RUNTIME_SSE_1_7_99 ? a_ae1799 : a_ae;
	}

	// Whether the call at a_site, which we are about to redirect, is the one we reversed: a
	// relative call to a_callee. Checking only for the call opcode accepted any call
	// at that spot (a different game build, a site moved by another mod's patch).
	//
	// A call that leads out of SkyrimSE.exe is another mod's hook on the same call (Community
	// Shaders has some of these sites: RenderWorld, SetupAndDrawPass). It is accepted: ours then
	// goes in front of it and calls it in place of the engine's function.
	inline bool CallsTo(std::uintptr_t a_site, REL::RelocationID a_callee, std::string_view a_what)
	{
		const auto* code = reinterpret_cast<const std::uint8_t*>(a_site);
		if (code[0] != 0xE8) {
			logger::error("hooks: {}: no call at the hook site", a_what);
			return false;
		}
		const auto target = a_site + 5 + *reinterpret_cast<const std::int32_t*>(code + 1);
		if (target == a_callee.address()) {
			return true;
		}
		static const auto [begin, end] = [] {
			const auto  base = REL::Module::get().base();
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			return std::pair{ base, base + nt->OptionalHeader.SizeOfImage };
		}();
		if (target < begin || target >= end) {
			logger::info("hooks: {}: the call is already redirected by another mod; chained", a_what);
			return true;
		}
		logger::error("hooks: {}: the call goes to {:X} instead of ID {}", a_what, target - begin, a_callee.id());
		return false;
	}
}
