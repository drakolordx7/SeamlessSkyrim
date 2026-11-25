#include "Render/Present.h"

#include "Diag/Telemetry.h"
#include "Portal/PortalManager.h"
#include "Render/FlashGuard.h"

#include <dxgi.h>

namespace Render::Present
{
	namespace
	{
		using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
		Present_t originalPresent = nullptr;

		Callback callback = [](IDXGISwapChain*, bool, bool) {};

		std::atomic<bool> arrivalPresent = false;

		HRESULT STDMETHODCALLTYPE PresentThunk(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags)
		{
			const bool arrival = arrivalPresent.exchange(false);
			const bool held = FlashGuard::Apply(a_swapChain);
			callback(a_swapChain, arrival, held);
			Diag::FrameEnd();
			// Time spent waiting here is the GPU (or frame generation) holding the main thread back.
			const Diag::Scope scope("present");
			return originalPresent(a_swapChain, a_syncInterval, a_flags);
		}

		void InstallPresentHook()
		{
			auto* window = RE::BSGraphics::Renderer::GetCurrentRenderWindow();
			auto* swapChain = window ? reinterpret_cast<IDXGISwapChain*>(window->swapChain) : nullptr;
			if (!swapChain) {
				logger::error("frame hooks: no swap chain, screenshots disabled");
				return;
			}
			// Patching the shared vtable chains with any other Present hook (Community Shaders, ENB
			// helpers): we call whatever was installed before us.
			auto*           vtable = *reinterpret_cast<std::uintptr_t**>(swapChain);
			constexpr auto kPresentIndex = 8;
			originalPresent = reinterpret_cast<Present_t>(vtable[kPresentIndex]);
			REL::safe_write(reinterpret_cast<std::uintptr_t>(&vtable[kPresentIndex]), reinterpret_cast<std::uintptr_t>(&PresentThunk));
		}
	}

	void Install()
	{
		// Only when something needs it (agent screenshots, portal compositing): installed during
		// normal play, the Present hook sat next to CS's frame-generation proxy and the image came
		// out blurred/doubled even with portals off.
		if (Diag::AgentMode() || Portal::Manager::Enabled()) {
			if (!originalPresent) {
				InstallPresentHook();
			}
		}
	}

	bool Installed()
	{
		return originalPresent != nullptr;
	}

	void MarkArrivalFrame()
	{
		arrivalPresent = true;
	}

	void SetCallback(Callback a_callback)
	{
		callback = a_callback;
	}
}
