#include "Skyrim/Plugin/Frame.h"

#include "Skyrim/Diag/Telemetry.h"
#include "Skyrim/Portal/CameraLimit.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/Travel.h"
#include "Skyrim/Render/Present.h"

namespace Plugin::Frame
{
	namespace
	{
		PreUpdate preUpdate = [](RE::PlayerCharacter*, float) {};

		struct PlayerUpdateHook
		{
			static void Thunk(RE::PlayerCharacter* a_this, float a_delta)
			{
				func(a_this, a_delta);
				preUpdate(a_this, a_delta);
				{
					const Diag::Scope scope("manager update");
					Portal::Manager::Update(a_delta);
					Portal::Travel::Update();
				}
				const auto approach = Portal::Manager::CameraLimit();
				Portal::Travel::Tick();
				const auto leave = Portal::Travel::CameraLimit();
				Portal::CameraLimit::Set(approach && leave ? std::optional(std::min(*approach, *leave)) : approach ? approach : leave);
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};
	}

	void Install()
	{
		// The player update drives the portals and the agent's walking; with neither there is
		// nothing for it to do (Portal::Manager::Install runs first and decides about portals).
		if (!Diag::AgentMode() && !Portal::Manager::HooksInstalled()) {
			logger::info("frame hooks: none needed");
			return;
		}
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_PlayerCharacter[0] };
		PlayerUpdateHook::func = vtable.write_vfunc(0xAD, PlayerUpdateHook::Thunk);
		Render::Present::Install();
		logger::info("frame hooks installed (Present hook: {})", Render::Present::Installed());
	}

	void SetPreUpdate(PreUpdate a_callback)
	{
		preUpdate = a_callback;
	}
}
