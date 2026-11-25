#include "Skyrim/Pictures/PictureSize.h"

namespace Pictures
{
	namespace
	{
		// The menu sets it on the render thread; Capture reads it on the main thread.
		std::atomic<int> pictureHeight{ kDefaultHeight };
	}

	int PictureHeight()
	{
		return pictureHeight.load();
	}

	void SetPictureHeight(int a_height)
	{
		// A multiple of 4, so that the width (three quarters) is whole.
		pictureHeight = std::clamp(a_height, kMinHeight, kMaxHeight) / 4 * 4;
	}
}
