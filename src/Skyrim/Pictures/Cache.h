#pragma once

#include "Core/Pictures/CacheRules.h"
#include "Core/Pictures/Layers.h"
#include "Skyrim/Pictures/PictureSize.h"

#include <d3d11.h>

// The doorway pictures kept per door: in video memory, on disk (SeamlessSkyrim_Doorways: *.ssdw the
// image tier's, *.ssdl the depth image tier's layers; only the current version of the current load
// order is read, and the folder is held to the size budget), and what is waiting to be written there.
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
		Camera::DoorwayPlace takenFrom{};
		Camera::Extent       extent = Camera::kDoorwayOnly;
		float                lightScale = 1.0f;  // Source::lightScale when taken
		float                brightness = 0.0f;  // as the file says (RoomBrightness); 0 until measured
	};

	// A doorway's layered depth image: one array texture, a layer per view (Layers.h), with mipmaps.
	// The layers are taken one portal pass at a time; it is drawn once all are there.
	struct Layered
	{
		ID3D11Texture2D*                      texture = nullptr;
		ID3D11ShaderResourceView*             view = nullptr;
		std::chrono::steady_clock::time_point taken{};
		std::chrono::steady_clock::time_point used{};
		bool                                  diskTried = false;
		UINT                                  width = 0;  // of a layer
		UINT                                  height = 0;
		int                                   layers = 0;     // how many views it is to have
		std::uint32_t                         captured = 0;   // bit per layer taken so far
		std::array<Layers::Place, Layers::kMaxLayers> places{};
		float                                 lightScale = 1.0f;
		float                                 brightness = 0.0f;

		[[nodiscard]] bool Complete() const { return view && layers > 0 && captured == (1u << layers) - 1u; }
	};

	// Pictures taken a few frames ago, read back and written to disk (the copy is done by then;
	// asked for at once, the read would wait for the GPU). layers 0: an image tier picture (staging
	// is a plain texture); else a depth image (staging is an array of that many layers).
	struct Readback
	{
		std::string                                   key;
		ID3D11Texture2D*                              staging = nullptr;
		UINT                                          width = 0;
		UINT                                          height = 0;
		Camera::DoorwayPlace                          takenFrom{};
		Camera::Extent                                extent{};
		float                                         lightScale = 1.0f;
		int                                           layers = 0;
		std::array<Layers::Place, Layers::kMaxLayers> places{};
		int                                           frames = 0;
		std::chrono::steady_clock::time_point         taken{};  // the picture's Picture::taken
	};

	// "Skyrim.esm|01A6FB#inside": the same door whatever the load order (as the door corrections);
	// the outdoors by the part of the day it is seen in, an interior looks the same at any hour.
	[[nodiscard]] int         PartOfDay();
	[[nodiscard]] std::string KeyOf(RE::FormID a_door, bool a_outdoors, int a_part);

	// The load order's hash (Core::Pictures::LoadOrderHash), once the data is loaded.
	[[nodiscard]] std::uint64_t LoadOrder();

	// The picture under a_key, made if there is none.
	Picture& Entry(const std::string& a_key);

	// The door's picture for this part of the day, loaded from disk the first time it is asked
	// for; another part's when this one was never taken (a door first passed at night).
	Picture* Find(ID3D11Device* a_device, RE::FormID a_door, bool a_outdoors);
	// Only this part of the day's.
	Picture* FindExact(ID3D11Device* a_device, RE::FormID a_door, bool a_outdoors);

	// With mipmaps: the draw reads the level the eye's ray stretches a texel to.
	bool MakeView(ID3D11Device* a_device, Picture& a_picture, const void* a_texels, UINT a_width, UINT a_height);
	void ReleaseTexture(Picture& a_picture);

	// The door's depth image for this part of the day (loaded from disk the first time), if it has a
	// texture; it may be waiting for layers (Layered::Complete).
	Layered* FindLayered(ID3D11Device* a_device, RE::FormID a_door, bool a_outdoors);
	Layered& EntryLayered(const std::string& a_key);
	// An array texture of a_layers layers (a_texels: all of them, one after the other, or none).
	bool MakeLayeredView(ID3D11Device* a_device, Layered& a_layered, const void* a_texels, UINT a_width, UINT a_height, int a_layers);
	void ReleaseLayered(Layered& a_layered);

	void               Queue(Readback a_readback);
	[[nodiscard]] bool HasReadbacks();
	void               WriteReadbacks(ID3D11DeviceContext* a_context);

	// How bright the room behind a door is drawn with its own light; the rooms' with pictures
	// without one; none before any.
	[[nodiscard]] std::optional<float> RoomBrightness(RE::FormID a_door);

	// The brightness of the door's picture for a tier's exposure (Core::Pictures::Exposure); 0 = not
	// measured (yet).
	[[nodiscard]] float PictureBrightness(RE::FormID a_door, bool a_outdoors, bool a_layered);

	// Any thread: every picture deleted from disk (returns how many) and, at the main thread's next
	// use, from memory.
	int DeleteAll();

	// Main thread: the folder brought within the budget (Settings) if it is over it; at most every
	// half minute, and a file of the old naming (no load order in it) always goes.
	void EnforceBudget();

	struct Counts
	{
		std::size_t inMemory;
		std::size_t layered;
		std::size_t readbacks;
	};
	[[nodiscard]] Counts Count();
}
