#include "Portal/ResetClothPhysics.h"

namespace Portal
{
	// Only SMP's own listeners hear the events: sent to all, other mods did their loading-screen
	// work on every trip (FasterTextures, seen in the exit's teleport frame).
	void ResetClothPhysics()
	{
		static const auto smp = []() -> std::pair<std::uintptr_t, std::uintptr_t> {
			auto* module = ::GetModuleHandleW(L"hdtsmp64.dll");
			if (!module) {
				return {};
			}
			const auto  base = reinterpret_cast<std::uintptr_t>(module);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			return { base, base + nt->OptionalHeader.SizeOfImage };
		}();
		auto* ui = RE::UI::GetSingleton();
		auto* source = ui ? ui->GetEventSource<RE::MenuOpenCloseEvent>() : nullptr;
		if (!source || !smp.first) {
			return;
		}
		const RE::MenuOpenCloseEvent opened{ RE::LoadingMenu::MENU_NAME, true };
		const RE::MenuOpenCloseEvent closed{ RE::LoadingMenu::MENU_NAME, false };
		RE::BSSpinLockGuard          locker(source->lock);
		for (auto* sink : source->sinks) {
			const auto vtable = sink ? *reinterpret_cast<const std::uintptr_t*>(sink) : 0;
			if (vtable >= smp.first && vtable < smp.second) {
				sink->ProcessEvent(&opened, source);
				sink->ProcessEvent(&closed, source);
			}
		}
	}
}
