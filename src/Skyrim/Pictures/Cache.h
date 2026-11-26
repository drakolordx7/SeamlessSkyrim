#pragma once

#include "Core/Pictures/CacheRules.h"
#include "Core/Pictures/Compress.h"
#include "Core/Pictures/FileFormat.h"
#include "Core/Pictures/Layers.h"
#include "Skyrim/Pictures/PictureSize.h"

#include <d3d11.h>

// The doorway pictures kept per door: in video memory, on disk (SeamlessSkyrim_Doorways: *.ssdw the
// image tier's, *.ssdl the depth image tier's layers; only the current version of the current load
// order is read, and the folder is held to the size budget), and what is waiting to be written there.
// A picture is BC6H colour with its mips and a half-float distance texture beside it, the same in
// memory and on disk (Core/Pictures/Compress.h).
namespace Pictures::Cache
{
	inline constexpr std::uint32_t kVersion = Core::Pictures::kVersion;
	inline constexpr auto          kRetake = Core::Pictures::kRetake;

	using Source = Core::Pictures::Files::Source;

	struct Picture
	{
		ID3D11Texture2D*                      texture = nullptr;  // BC6H, with mips
		ID3D11ShaderResourceView*             view = nullptr;
		ID3D11Texture2D*                      distance = nullptr;  // R16_FLOAT
		ID3D11ShaderResourceView*             distanceView = nullptr;
		Source                                source = Source::kVisit;
		std::uint32_t                         takenAt = 0;  // seconds since 1970
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
		ID3D11Texture2D*                      texture = nullptr;  // BC6H array, with mips
		ID3D11ShaderResourceView*             view = nullptr;
		ID3D11Texture2D*                      distance = nullptr;  // R16_FLOAT array
		ID3D11ShaderResourceView*             distanceView = nullptr;
		Source                                source = Source::kVisit;
		std::uint32_t                         takenAt = 0;
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
	// asked for at once, the read would wait for the GPU). The staging copies are of the picture's own
	// textures. layers 0: an image tier picture; else a depth image of that many layers.
	struct Readback
	{
		std::string                                   key;
		ID3D11Texture2D*                              colour = nullptr;
		ID3D11Texture2D*                              distance = nullptr;
		Source                                        source = Source::kVisit;
		std::uint32_t                                 takenAt = 0;
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

	// The textures of a picture of a_width x a_height (whole blocks): its colour chain and distances as
	// a file holds them (Files::TexelBytes), or none, to be copied into by Gpu::Compress. The draw
	// reads the colour's level the eye's ray stretches a texel to.
	bool MakeView(ID3D11Device* a_device, Picture& a_picture, const void* a_file, UINT a_width, UINT a_height);
	void ReleaseTexture(Picture& a_picture);

	// The door's depth image for this part of the day (loaded from disk the first time), if it has a
	// texture; it may be waiting for layers (Layered::Complete).
	Layered* FindLayered(ID3D11Device* a_device, RE::FormID a_door, bool a_outdoors);
	Layered& EntryLayered(const std::string& a_key);
	// The array textures of a_layers layers (a_file: each layer's chain and distances one after the
	// other, or none).
	bool MakeLayeredView(ID3D11Device* a_device, Layered& a_layered, const void* a_file, UINT a_width, UINT a_height, int a_layers);
	void ReleaseLayered(Layered& a_layered);

	// a_readback's staging copies made from the picture's textures (a_colour, a_distance), and queued.
	void               Queue(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11Texture2D* a_colour, ID3D11Texture2D* a_distance, Readback a_readback);
	[[nodiscard]] bool HasReadbacks();
	void               WriteReadbacks(ID3D11DeviceContext* a_context);

	// How bright the room behind a door is drawn with its own light; the rooms' with pictures
	// without one; none before any.
	[[nodiscard]] std::optional<float> RoomBrightness(RE::FormID a_door);

	// The brightness of the door's picture for a tier's exposure (Core::Pictures::Exposure); 0 = not
	// measured (yet).
	[[nodiscard]] float PictureBrightness(RE::FormID a_door, bool a_outdoors, bool a_layered);

	// Main thread: the door's pictures for this part of the day (both tiers) dropped from memory and disk.
	void Invalidate(RE::FormID a_door, bool a_outdoors);

	// Any thread: every picture deleted from disk (returns how many) and, at the main thread's next
	// use, from memory.
	int DeleteAll();

	// Main thread: the folder brought within the budget (Settings) if it is over it; at most every
	// half minute, and a file of the old naming (no load order in it) always goes.
	void EnforceBudget();

	// What the folder holds for the current load order (the bridge's pictures command): a door's file
	// of either tier for a part of the day, read from its header.
	struct Listing
	{
		std::string   door;   // "Skyrim.esm|01A6FB"
		std::string   part;   // "inside", or the part of the day
		bool          layered = false;
		int           layers = 0;
		Source        source = Source::kVisit;
		std::uint32_t takenAt = 0;
		std::uint64_t bytes = 0;
		bool          inMemory = false;
	};
	[[nodiscard]] std::vector<Listing> List();

	struct Counts
	{
		std::size_t inMemory;
		std::size_t layered;
		std::size_t readbacks;
	};
	[[nodiscard]] Counts Count();
}
