#pragma once

#include "Core/Pictures/Camera.h"
#include "Skyrim/Plugin/Settings.h"

namespace Pictures
{
	namespace Camera = Core::Pictures::Camera;

	// The size pictures are taken at, as the doorway's height in texels; the width is three
	// quarters of it. Small on purpose: shown blurred, a hundred texels across are all it needs. A
	// room door's picture (the capture camera's) is 2.4 MB on disk at the default size, 3.2 MB of
	// video memory with its mipmaps; sixteen times that at the largest.
	inline constexpr auto kHeightSetting = Plugin::Settings::Of(Plugin::Settings::Id::kDoorwayPictureHeight);
	inline constexpr int  kDefaultHeight = kHeightSetting.def;
	inline constexpr int  kMinHeight = kHeightSetting.min;
	inline constexpr int  kMaxHeight = kHeightSetting.max;

	// Pictures already taken keep their size until taken again (at once, when next seen).
	[[nodiscard]] int PictureHeight();
	void              SetPictureHeight(int a_height);
}
