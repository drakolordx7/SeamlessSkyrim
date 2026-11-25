#pragma once

#include <d3d11.h>
#include <dxgi.h>

// The first walk outdoors in a session drew one frame almost entirely white (the engine's own);
// for a short while after each crossing a frame far brighter than the one shown before it is
// replaced by that one at Present.
namespace Render::FlashGuard
{
	// The next a_frames frames: one far brighter than the frame shown before it is not shown (a white
	// flash the engine draws once in a while right after a change of space). See HoldFlash.
	void Guard(int a_frames);

	// Present hook, main thread: counts a guarded frame down and holds it if it is a flash. True
	// when the frame was replaced.
	bool Apply(IDXGISwapChain* a_swapChain);

	// A few rows across the screen (the row at height/kRows[i]), their average brightness 0..1.
	inline constexpr UINT kRows[] = { 4, 3, 2 };
	float                 RowAverage(const D3D11_MAPPED_SUBRESOURCE& a_map, UINT a_width, DXGI_FORMAT a_format);
}
