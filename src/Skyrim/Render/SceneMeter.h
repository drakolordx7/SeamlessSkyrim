#pragma once

// Brightness as an eye judges a scene: the geometric mean of the luminance, in the main scene's
// units (before tone mapping). Skyrim has no lasting eye adaptation (the tone map's exposure is
// the ratio of two adapted copies of the same average, 1 once settled), so a room seen from a
// sunlit street came out as bright as from inside it; the room lights MergedView::RoomLightScale
// works out compare these instead.
namespace Render::SceneMeter
{
	// Render, after the world is drawn: measures a_scene (an ID3D11ShaderResourceView of the main
	// scene), read back a few frames later. a_width, a_height: its viewport in pixels.
	void Measure(void* a_scene, float a_width, float a_height);

	// The player's view, as an eye takes a second or so to adapt to it; none before the first reading.
	[[nodiscard]] std::optional<float> Brightness();
}
