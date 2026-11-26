#pragma once

#include <array>
#include <cstdint>

// The colour compressor, as plain maths: BC6H (unsigned half float) mode 11, one subset, two 10-bit
// endpoints and a 4-bit index per texel. The compute shader in Skyrim/Pictures/Gpu.cpp does the same
// steps; this is what the shader is checked against, and the decoder is the format's own (D3D's BC6H
// decode of the blocks it makes is what the picture is drawn from).
//
// Everything is done in the half float's bit pattern as a number (0 .. 0x7BFF), which is also what
// the format interpolates: a pattern is about linear in the log of the value, so a ramp of
// brightness comes out even and a lamp doesn't pull its wall's colour up.
namespace Core::Pictures::Bc6h
{
	using Texels = std::array<std::array<float, 3>, 16>;  // linear RGB, row by row
	struct Block
	{
		std::array<std::uint8_t, 16> bytes{};
	};

	// A float as a finite unsigned half's bits (negative and NaN are 0, past 65504 is 0x7BFF).
	[[nodiscard]] std::uint16_t HalfBits(float a_value);
	[[nodiscard]] float         HalfValue(std::uint16_t a_bits);

	[[nodiscard]] Block Encode(const Texels& a_texels);
	// A block of this encoder's (mode 11); all zero for any other mode.
	[[nodiscard]] Texels Decode(const Block& a_block);
}
