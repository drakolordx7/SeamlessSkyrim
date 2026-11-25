#pragma once

#include "Core/Pictures/CacheRules.h"
#include "Skyrim/Pictures/PictureSize.h"

#include <d3d11.h>

// The doorway pictures kept per door: in video memory, on disk (SeamlessSkyrim_Doorways/*.ssdw,
// versioned), and what is waiting to be written there.
namespace Pictures::Cache
{
	inline constexpr std::uint32_t kVersion = Core::Pictures::kVersion;
	inline constexpr auto          kRetake = Core::Pictures::kRetake;

	struct Picture
	{
		ID3D11Texture2D*                      texture = nullptr;
		ID3D11ShaderResourceView*             view = nullptr;
		std::chrono::steady_clock::time_point taken{};
		std::chrono::steady_clock::time_point used{};  // last drawn or taken (ForgetUnused)
		bool                                  diskTried = false;
		// Its own: one loaded from disk or taken before the size was changed keeps the old one.
		UINT                 width = 0;
		UINT                 height = 0;
		bool                 depth = false;  // false: a version 1 picture
		Camera::DoorwayPlace takenFrom{};
		Camera::Extent       extent = Camera::kDoorwayOnly;
		float                lightScale = 1.0f;  // Source::lightScale when taken
		std::uint32_t        version = kVersion;
		float                brightness = 0.0f;  // RoomBrightness; 0 until its texels have been on the CPU
	};

	// Pictures taken a few frames ago, read back and written to disk (the copy is done by then;
	// asked for at once, the read would wait for the GPU).
	struct Readback
	{
		std::string                           key;
		ID3D11Texture2D*                      staging = nullptr;
		UINT                                  width = 0;
		UINT                                  height = 0;
		Camera::DoorwayPlace                  takenFrom{};
		Camera::Extent                        extent{};
		float                                 lightScale = 1.0f;
		int                                   frames = 0;
		std::chrono::steady_clock::time_point taken{};  // the picture's Picture::taken
	};

	// "Skyrim.esm|01A6FB#inside": the same door whatever the load order (as the door corrections);
	// the outdoors by the part of the day it is seen in, an interior looks the same at any hour.
	[[nodiscard]] int         PartOfDay();
	[[nodiscard]] std::string KeyOf(RE::FormID a_door, bool a_outdoors, int a_part);

	// The picture under a_key, made if there is none.
	Picture& Entry(const std::string& a_key);

	// The door's picture for this part of the day, loaded from disk the first time it is asked
	// for; another part's when this one was never taken (a door first passed at night).
	Picture* Find(ID3D11Device* a_device, RE::FormID a_door, bool a_outdoors);

	// With mipmaps: where the eye's ray stretches a row of texels across the screen (leaving the
	// picture at its side, or sliding off a near thing's edge onto the room behind it) the draw
	// blurs by how far, from a smaller mip. Sampled sharp there, the row showed as streaks.
	bool MakeView(ID3D11Device* a_device, Picture& a_picture, const void* a_texels, UINT a_width, UINT a_height);
	void ReleaseTexture(Picture& a_picture);

	void Queue(Readback a_readback);
	[[nodiscard]] bool HasReadbacks();
	void               WriteReadbacks(ID3D11DeviceContext* a_context);

	// How bright the room behind a door is drawn with its own light; the rooms' with pictures
	// without one; none before any.
	[[nodiscard]] std::optional<float> RoomBrightness(RE::FormID a_door);

	// Any thread: every picture deleted from disk (returns how many) and, at the main thread's next
	// use, from memory.
	int DeleteAll();

	struct Counts
	{
		std::size_t inMemory;
		std::size_t readbacks;
	};
	[[nodiscard]] Counts Count();
}
