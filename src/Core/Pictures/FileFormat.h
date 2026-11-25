#pragma once

#include <cstdint>

#include "Core/Pictures/CacheRules.h"
#include "Core/Pictures/Layers.h"

// What the files of the picture cache start with. Both are followed by half-float RGBA texels
// (alpha: how far behind the door's plane the surface is): the image by one picture, the layered
// depth image by its layers one after the other, each width x height. A file that is of another
// version, another load order or too large to be real is taken again.
namespace Core::Pictures::Files
{
	// .ssdw: the image tier's picture.
	struct ImageHeader
	{
		std::uint32_t magic = kMagic;
		std::uint32_t version = kVersion;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint64_t loadOrder = 0;
		float         takenFrom[3]{};  // a doorway place (Camera::DoorwayPlace)
		float         lightScale = 1.0f;
		float         extent[4]{};  // the part of the door's plane covered (Camera::Extent)
		float         brightness = 0.0f;  // the picture's, with lightScale divided out; 0 = unknown
		float         reserved[3]{};
	};
	static_assert(sizeof(ImageHeader) == 72);

	// .ssdl: the depth image tier's layers.
	struct LayeredHeader
	{
		std::uint32_t magic = kMagic;
		std::uint32_t version = kVersion;
		std::uint32_t width = 0;  // of one layer
		std::uint32_t height = 0;
		std::uint32_t layers = 0;
		std::uint32_t reserved0 = 0;
		std::uint64_t loadOrder = 0;
		float         lightScale = 1.0f;
		float         brightness = 0.0f;
		float         reserved1[2]{};
		float         places[Layers::kMaxLayers][3]{};  // Layers::Place of each
		float         reserved2 = 0.0f;
	};
	static_assert(sizeof(LayeredHeader) == 88);

	// a_widest, a_highest: the largest a layer or picture was ever written at, so that a damaged
	// header can't ask for gigabytes.
	[[nodiscard]] bool Valid(const ImageHeader& a_header, std::uint64_t a_loadOrder, std::uint32_t a_widest, std::uint32_t a_highest);
	[[nodiscard]] bool Valid(const LayeredHeader& a_header, std::uint64_t a_loadOrder, std::uint32_t a_widest, std::uint32_t a_highest);

	// The texels' size in bytes after the header.
	[[nodiscard]] std::uint64_t TexelBytes(const ImageHeader& a_header);
	[[nodiscard]] std::uint64_t TexelBytes(const LayeredHeader& a_header);
}
