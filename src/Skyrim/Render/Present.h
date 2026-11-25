#pragma once

struct IDXGISwapChain;

// The swap chain's Present hook: the flash guard, the bridge's brightness log and captures, and
// the perf frame end all run inside it, in that order.
namespace Render::Present
{
	// Installed during normal play only when portals are on: with nothing needing it, the hook sat
	// next to CS's frame-generation proxy and the image came out blurred/doubled even with portals
	// off. Agent mode needs it for screenshots. Call at kDataLoaded.
	void Install();
	[[nodiscard]] bool Installed();

	// The next frame presented is the first in a new space.
	void MarkArrivalFrame();

	// Called with each frame about to be presented, after the flash guard: a_arrival is the first
	// frame in a new space, a_held a frame the guard replaced. Register before Install.
	using Callback = void (*)(IDXGISwapChain* a_swapChain, bool a_arrival, bool a_held);
	void SetCallback(Callback a_callback);
}
