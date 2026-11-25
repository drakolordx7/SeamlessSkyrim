#include "Plugin/Frame.h"

#include "Diag/Telemetry.h"
#include "Portal/CameraLimit.h"
#include "Portal/PortalManager.h"
#include "Portal/Travel.h"
#include "Render/Present.h"

namespace Plugin::Frame
{
	namespace
	{
		PreUpdate preUpdate = [](RE::PlayerCharacter*, float) {};

		// Diagnostics: wall-clock time between player updates (= frame time), last 120 frames.
		std::mutex                                           frameLock;
		std::array<float, 120>                               frameMs{};
		std::size_t                                          frameIndex = 0;
		std::optional<std::chrono::steady_clock::time_point> lastFrame;

		struct PlayerUpdateHook
		{
			static void Thunk(RE::PlayerCharacter* a_this, float a_delta)
			{
				{
					const auto now = std::chrono::steady_clock::now();
					std::scoped_lock lock(frameLock);
					if (lastFrame) {
						frameMs[frameIndex++ % frameMs.size()] = std::chrono::duration<float, std::milli>(now - *lastFrame).count();
					}
					lastFrame = now;
				}
				func(a_this, a_delta);
				preUpdate(a_this, a_delta);
				{
					const Diag::Scope scope("manager update");
					Portal::Manager::Update(a_delta);
					Portal::Travel::Update();
				}
				const auto approach = Portal::Manager::CameraLimit();
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

	json FrameStats()
	{
		std::scoped_lock lock(frameLock);
		const auto       count = std::min(frameIndex, frameMs.size());
		if (!count) {
			return nullptr;
		}
		std::vector<float> sorted(frameMs.begin(), frameMs.begin() + count);
		std::ranges::sort(sorted);
		const float sum = std::accumulate(sorted.begin(), sorted.end(), 0.0f);
		return { { "frames", count }, { "avgMs", sum / count }, { "medianMs", sorted[count / 2] }, { "p95Ms", sorted[count * 95 / 100] }, { "maxMs", sorted.back() } };
	}
}
