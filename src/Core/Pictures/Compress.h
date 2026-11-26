#pragma once

#include <algorithm>
#include <cstdint>

// How a doorway picture is stored, in video memory and on disk. The colour is block compressed
// (BC6H, unsigned half float: the scene is HDR, so BC7's 8 bits would clip lamps and fire), with the
// mip chain the draw reads; the distance behind the doorway's plane, which the ray march reads
// texel by texel, is kept apart as a half float texture of its own (a block would blur a wall into a
// chair), without mips. Both are made on the GPU from the half-float RGBA target a picture is taken
// into, which is the only place the old 8 bytes a texel remain (kCaptureBytesPerTexel). Sizes are
// multiples of the block (SizeFor and SizeOf round up), so a texel row is a whole number of blocks.
namespace Core::Pictures::Compress
{
	inline constexpr std::uint32_t kBlockTexels = 4;
	inline constexpr std::uint32_t kBlockBytes = 16;
	// Distance behind the doorway: R16_FLOAT.
	inline constexpr std::uint32_t kDepthBytesPerTexel = 2;
	// The target a picture is taken into (colour, with the distance in alpha for the brightness read).
	inline constexpr std::uint32_t kCaptureBytesPerTexel = 8;

	[[nodiscard]] constexpr std::uint32_t Blocks(std::uint32_t a_texels)
	{
		return std::max(1u, (a_texels + kBlockTexels - 1) / kBlockTexels);
	}

	[[nodiscard]] constexpr std::uint32_t Level(std::uint32_t a_texels, std::uint32_t a_level)
	{
		return std::max(1u, a_texels >> a_level);
	}

	// Mip levels down to 1 x 1.
	[[nodiscard]] constexpr std::uint32_t MipCount(std::uint32_t a_width, std::uint32_t a_height)
	{
		std::uint32_t levels = 1;
		for (auto size = std::max(a_width, a_height); size > 1; size >>= 1) {
			++levels;
		}
		return levels;
	}

	// One level's colour: whole blocks.
	[[nodiscard]] constexpr std::uint64_t ColourLevelBytes(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_level)
	{
		return std::uint64_t{ Blocks(Level(a_width, a_level)) } * Blocks(Level(a_height, a_level)) * kBlockBytes;
	}

	// The colour with all its mips.
	[[nodiscard]] constexpr std::uint64_t ColourBytes(std::uint32_t a_width, std::uint32_t a_height)
	{
		std::uint64_t bytes = 0;
		for (std::uint32_t level = 0; level < MipCount(a_width, a_height); ++level) {
			bytes += ColourLevelBytes(a_width, a_height, level);
		}
		return bytes;
	}

	[[nodiscard]] constexpr std::uint64_t DepthBytes(std::uint32_t a_width, std::uint32_t a_height)
	{
		return std::uint64_t{ a_width } * a_height * kDepthBytesPerTexel;
	}

	// What one picture (or one view of a depth image) takes, held and on disk.
	[[nodiscard]] constexpr std::uint64_t PictureBytes(std::uint32_t a_width, std::uint32_t a_height)
	{
		return ColourBytes(a_width, a_height) + DepthBytes(a_width, a_height);
	}

	// A size rounded up to a whole block.
	[[nodiscard]] constexpr std::uint32_t RoundUp(std::uint32_t a_texels)
	{
		return Blocks(a_texels) * kBlockTexels;
	}
}
