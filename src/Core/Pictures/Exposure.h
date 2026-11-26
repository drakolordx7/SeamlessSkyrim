#pragma once

#include <algorithm>
#include <cmath>

// A picture seen with the viewer's exposure. A picture keeps its space as it was drawn from inside
// (its own lights, as an eye adapted to it sees it); the viewer's eye is adapted to what they are
// in. Skyrim has no lasting eye adaptation of its own, so a dim room's picture came out as bright
// from a sunlit street as from inside it.
namespace Core::Pictures::Exposure
{
	// An eye adapts part of the way: with exposure as luminance^-(1/3), what is drawn goes as
	// luminance^(2/3), and the exposures adapted to two scenes differ by the square root of their
	// brightness ratio (the same rule Portal's MergedView::StreetEyeScale gives a merged room;
	// adapting the whole way left a dim inn's doorway across the market black).
	inline constexpr float kExponent = 0.5f;
	// Never black: a room far darker than the street it is seen from still shows its lamps.
	inline constexpr float kDarkest = 0.05f;

	// The factor a picture's colour is drawn with. a_picture: its brightness as taken (the
	// geometric mean of its texels' luminance, with the scale it was taken under divided out);
	// a_view: the viewer's, as Render::SceneMeter gives it (adapted over a second or so). Zero
	// or unknown (not measured yet) is 1. a_brightest: 1 draws a space no brighter than it is
	// from inside (the room seen from a dim street is left as it is).
	[[nodiscard]] inline float ViewScale(float a_picture, float a_view, float a_brightest = 1.0f)
	{
		if (!(a_picture > 0.0f) || !(a_view > 0.0f)) {
			return 1.0f;
		}
		return std::clamp(std::pow(a_picture / a_view, kExponent), kDarkest, a_brightest);
	}
}
