#pragma once

#include "Core/Pictures/Camera.h"
#include "Core/Pictures/Layers.h"
#include "Skyrim/Plugin/Settings.h"

namespace Pictures
{
	namespace Camera = Core::Pictures::Camera;
	namespace Layers = Core::Pictures::Layers;

	// The size pictures are taken at, as the doorway's height in texels; the width is three
	// quarters of it. The image tier is drawn sharp, so this is the picture's real resolution (the
	// portal pass it is taken from has the screen's: at 1080p about 600 texels across the doorway
	// is all there is to take). A room door's picture (the capture camera's, which reaches past the
	// doorway) is 9.6 MB on disk at the default size, a third more in video memory with its
	// mipmaps; four times that at 1024. A depth image's layers are as many rows each.
	inline constexpr auto kHeightSetting = Plugin::Settings::Of(Plugin::Settings::Id::kDoorwayPictureHeight);
	inline constexpr int  kDefaultHeight = kHeightSetting.def;
	inline constexpr int  kMinHeight = kHeightSetting.min;
	inline constexpr int  kMaxHeight = kHeightSetting.max;

	// Pictures already taken keep their size until taken again (at once, when next seen).
	[[nodiscard]] int PictureHeight();
	void              SetPictureHeight(int a_height);

	// The disk budget of the picture cache (Settings), in bytes.
	[[nodiscard]] std::uint64_t CacheBudgetBytes();
	// How many views a doorway's depth image has (Settings); 0 = none, the image tier alone.
	[[nodiscard]] int DepthLayers();
}
