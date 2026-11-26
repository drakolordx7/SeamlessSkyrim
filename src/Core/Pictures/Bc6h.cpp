#include "Core/Pictures/Bc6h.h"

#include <algorithm>
#include <cmath>

namespace Core::Pictures::Bc6h
{
	namespace
	{
		// The format's weights for a 4-bit index (of 64).
		constexpr int   kWeights[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };
		constexpr float kMaxBits = 31743.0f;  // 0x7BFF

		// The format's unquantize for a 10-bit unsigned endpoint.
		std::uint32_t Unquantize(std::uint32_t a_q)
		{
			return a_q == 0 ? 0u : a_q == 1023 ? 0xFFFFu : ((a_q << 16) + 0x8000u) >> 10;
		}

		// The half bits the format makes of two unquantized endpoints at a weight.
		std::uint32_t Interpolate(std::uint32_t a_e0, std::uint32_t a_e1, int a_weight)
		{
			const std::uint32_t mixed = (a_e0 * static_cast<std::uint32_t>(64 - a_weight) + a_e1 * static_cast<std::uint32_t>(a_weight) + 32u) >> 6;
			return (mixed * 31u) >> 6;
		}

		// An endpoint in half bits as the 10-bit code that comes nearest it (a code q decodes to
		// about 31 q + 15.5).
		std::uint32_t Quantize(float a_bits)
		{
			return static_cast<std::uint32_t>(std::clamp(std::floor((a_bits - 15.5f) / 31.0f + 0.5f), 0.0f, 1023.0f));
		}

		void Put(Block& a_block, int a_position, int a_count, std::uint32_t a_value)
		{
			for (int i = 0; i < a_count; ++i) {
				if ((a_value >> i) & 1u) {
					a_block.bytes[static_cast<std::size_t>(a_position + i) >> 3] |= static_cast<std::uint8_t>(1u << ((a_position + i) & 7));
				}
			}
		}

		std::uint32_t Get(const Block& a_block, int a_position, int a_count)
		{
			std::uint32_t value = 0;
			for (int i = 0; i < a_count; ++i) {
				value |= static_cast<std::uint32_t>((a_block.bytes[static_cast<std::size_t>(a_position + i) >> 3] >> ((a_position + i) & 7)) & 1) << i;
			}
			return value;
		}

		using V3 = std::array<float, 3>;
		float Dot(const V3& a, const V3& b)
		{
			return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		}
	}

	std::uint16_t HalfBits(float a_value)
	{
		if (!(a_value > 0.0f)) {
			return 0;
		}
		if (a_value >= 65504.0f) {
			return 0x7BFF;
		}
		if (a_value < 6.103515625e-5f) {
			return static_cast<std::uint16_t>(std::lround(a_value * 16777216.0f));  // subnormal: units of 2^-24 (1024 is the smallest normal)
		}
		int         exponent = 0;
		const float mantissa = std::frexp(a_value, &exponent);  // [0.5, 1)
		auto        field = exponent - 1 + 15;
		auto        fraction = static_cast<int>(std::lround((mantissa * 2.0f - 1.0f) * 1024.0f));
		if (fraction == 1024) {
			fraction = 0;
			++field;
		}
		return static_cast<std::uint16_t>(std::min((field << 10) | fraction, 0x7BFF));
	}

	float HalfValue(std::uint16_t a_bits)
	{
		const int exponent = (a_bits >> 10) & 31;
		const int fraction = a_bits & 1023;
		return exponent == 0 ? std::ldexp(static_cast<float>(fraction), -24) : std::ldexp(1.0f + static_cast<float>(fraction) / 1024.0f, exponent - 15);
	}

	Block Encode(const Texels& a_texels)
	{
		std::array<V3, 16> h;
		for (std::size_t i = 0; i < 16; ++i) {
			for (std::size_t c = 0; c < 3; ++c) {
				h[i][c] = static_cast<float>(HalfBits(a_texels[i][c]));
			}
		}

		// The block's principal axis, from the spread of its channels by four rounds of the
		// covariance matrix; its extremes along it are the first endpoints.
		V3 mean{}, low = h[0], high = h[0];
		for (const auto& t : h) {
			for (std::size_t c = 0; c < 3; ++c) {
				mean[c] += t[c] / 16.0f;
				low[c] = std::min(low[c], t[c]);
				high[c] = std::max(high[c], t[c]);
			}
		}
		float cov[3][3]{};
		for (const auto& t : h) {
			const V3 d{ t[0] - mean[0], t[1] - mean[1], t[2] - mean[2] };
			for (std::size_t a = 0; a < 3; ++a) {
				for (std::size_t b = 0; b < 3; ++b) {
					cov[a][b] += d[a] * d[b];
				}
			}
		}
		V3 axis{ high[0] - low[0], high[1] - low[1], high[2] - low[2] };
		for (int round = 0; round < 4; ++round) {
			const V3    next{ cov[0][0] * axis[0] + cov[0][1] * axis[1] + cov[0][2] * axis[2], cov[1][0] * axis[0] + cov[1][1] * axis[1] + cov[1][2] * axis[2],
				cov[2][0] * axis[0] + cov[2][1] * axis[1] + cov[2][2] * axis[2] };
			const float length = std::sqrt(Dot(next, next));
			if (length > 1e-6f) {
				axis = { next[0] / length, next[1] / length, next[2] / length };
			}
		}
		float tMin = 1e30f, tMax = -1e30f;
		for (const auto& t : h) {
			const float p = Dot({ t[0] - mean[0], t[1] - mean[1], t[2] - mean[2] }, axis);
			tMin = std::min(tMin, p);
			tMax = std::max(tMax, p);
		}
		V3 e0, e1;
		for (std::size_t c = 0; c < 3; ++c) {
			e0[c] = std::clamp(mean[c] + axis[c] * tMin, 0.0f, kMaxBits);
			e1[c] = std::clamp(mean[c] + axis[c] * tMax, 0.0f, kMaxBits);
		}

		// Twice: the indices the endpoints give, then the endpoints those indices fit best.
		for (int round = 0; round < 2; ++round) {
			const V3    d{ e1[0] - e0[0], e1[1] - e0[1], e1[2] - e0[2] };
			const float dd = Dot(d, d);
			if (dd < 1e-6f) {
				break;
			}
			float aa = 0, bb = 0, ab = 0;
			V3    ax{}, bx{};
			for (const auto& t : h) {
				const float s = Dot({ t[0] - e0[0], t[1] - e0[1], t[2] - e0[2] }, d) / dd;
				const int   index = std::clamp(static_cast<int>(std::floor(s * 15.0f + 0.5f)), 0, 15);
				const float w = static_cast<float>(kWeights[index]) / 64.0f;
				const float a = 1.0f - w;
				aa += a * a;
				bb += w * w;
				ab += a * w;
				for (std::size_t c = 0; c < 3; ++c) {
					ax[c] += a * t[c];
					bx[c] += w * t[c];
				}
			}
			const float det = aa * bb - ab * ab;
			if (std::abs(det) < 1e-4f) {
				break;
			}
			for (std::size_t c = 0; c < 3; ++c) {
				e0[c] = std::clamp((ax[c] * bb - bx[c] * ab) / det, 0.0f, kMaxBits);
				e1[c] = std::clamp((bx[c] * aa - ax[c] * ab) / det, 0.0f, kMaxBits);
			}
		}

		std::uint32_t q0[3], q1[3], u0[3], u1[3];
		for (std::size_t c = 0; c < 3; ++c) {
			q0[c] = Quantize(e0[c]);
			q1[c] = Quantize(e1[c]);
			u0[c] = Unquantize(q0[c]);
			u1[c] = Unquantize(q1[c]);
		}
		// Each texel takes the index whose decoded colour is nearest its own.
		std::uint32_t palette[16][3];
		for (std::size_t i = 0; i < 16; ++i) {
			for (std::size_t c = 0; c < 3; ++c) {
				palette[i][c] = Interpolate(u0[c], u1[c], kWeights[i]);
			}
		}
		int indices[16];
		for (std::size_t t = 0; t < 16; ++t) {
			float best = 1e30f;
			indices[t] = 0;
			for (std::size_t i = 0; i < 16; ++i) {
				float error = 0.0f;
				for (std::size_t c = 0; c < 3; ++c) {
					const float d = h[t][c] - static_cast<float>(palette[i][c]);
					error += d * d;
				}
				if (error < best) {
					best = error;
					indices[t] = static_cast<int>(i);
				}
			}
		}
		// The first texel's index has no top bit: if it needs it, the endpoints swap and the ramp runs the other way.
		if (indices[0] >= 8) {
			for (std::size_t c = 0; c < 3; ++c) {
				std::swap(q0[c], q1[c]);
			}
			for (auto& index : indices) {
				index = 15 - index;
			}
		}

		Block block;
		Put(block, 0, 5, 3);  // mode 11
		int position = 5;
		for (std::size_t c = 0; c < 3; ++c, position += 10) {
			Put(block, position, 10, q0[c]);
		}
		for (std::size_t c = 0; c < 3; ++c, position += 10) {
			Put(block, position, 10, q1[c]);
		}
		for (std::size_t t = 0; t < 16; ++t) {
			const int bits = t == 0 ? 3 : 4;
			Put(block, position, bits, static_cast<std::uint32_t>(indices[t]));
			position += bits;
		}
		return block;
	}

	Texels Decode(const Block& a_block)
	{
		Texels out{};
		if (Get(a_block, 0, 5) != 3) {
			return out;
		}
		std::uint32_t u0[3], u1[3];
		int           position = 5;
		for (std::size_t c = 0; c < 3; ++c, position += 10) {
			u0[c] = Unquantize(Get(a_block, position, 10));
		}
		for (std::size_t c = 0; c < 3; ++c, position += 10) {
			u1[c] = Unquantize(Get(a_block, position, 10));
		}
		for (std::size_t t = 0; t < 16; ++t) {
			const int bits = t == 0 ? 3 : 4;
			const int index = static_cast<int>(Get(a_block, position, bits));
			position += bits;
			for (std::size_t c = 0; c < 3; ++c) {
				out[t][c] = HalfValue(static_cast<std::uint16_t>(Interpolate(u0[c], u1[c], kWeights[index])));
			}
		}
		return out;
	}
}
