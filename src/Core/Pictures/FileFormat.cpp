#include "Core/Pictures/FileFormat.h"

#include "Core/Pictures/Compress.h"

namespace Core::Pictures::Files
{
	bool Valid(const ImageHeader& a_header, std::uint64_t a_loadOrder, std::uint32_t a_widest, std::uint32_t a_highest)
	{
		const std::uint32_t words[4]{ a_header.magic, a_header.version, a_header.width, a_header.height };
		return HeaderValid(words, a_widest, a_highest) && a_header.loadOrder == a_loadOrder;
	}

	bool Valid(const LayeredHeader& a_header, std::uint64_t a_loadOrder, std::uint32_t a_widest, std::uint32_t a_highest)
	{
		const std::uint32_t words[4]{ a_header.magic, a_header.version, a_header.width, a_header.height };
		return HeaderValid(words, a_widest, a_highest) && a_header.loadOrder == a_loadOrder && a_header.layers >= 1 && a_header.layers <= static_cast<std::uint32_t>(Layers::kMaxLayers);
	}

	std::uint64_t TexelBytes(const ImageHeader& a_header)
	{
		return Compress::PictureBytes(a_header.width, a_header.height);
	}

	std::uint64_t TexelBytes(const LayeredHeader& a_header)
	{
		return Compress::PictureBytes(a_header.width, a_header.height) * a_header.layers;
	}
}
