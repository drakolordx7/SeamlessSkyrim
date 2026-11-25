#pragma once

// HLSL the doorway's shaders share, and the constants they share it through. Each shader's source
// is its own declarations (cbuffer included), then the helpers it uses, then its code.
namespace Pictures::Hlsl
{
	// A portal pass's depth (its own near and far: the portal camera's near plane stands at the door,
	// a city gate's at the far gate) as the distance along the view axis.
	inline constexpr char kViewDistance[] = R"(
		float PortalViewDistance(float depth, float nearClip, float farClip)
		{
			return nearClip * farClip / max(farClip - depth * (farClip - nearClip), 1e-3);
		}
	)";

	// The composite fogs the world by view depth (not distance); a doorway at its own depth matches
	// the wall around it. What lies behind it was fogged by the portal pass or is in the picture.
	// Reads the shader's Fog, FogNear and FogFar (FogConstants).
	inline constexpr char kFogged[] = R"(
		float3 Fogged(float3 color, float depth)
		{
			if (Fog.w <= 0)
				return color;
			float f = min(Fog.w, pow(saturate(depth * Fog.y - Fog.x), Fog.z));
			return lerp(color, lerp(FogNear.rgb, FogFar.rgb, f), f);
		}
	)";

	// The world's fog as the engine uploads it (Engine::Fog::World), laid out for the cbuffers.
	struct FogConstants
	{
		float param[4];      // near / (far - near), 1 / (far - near), power, most it covers
		float nearColor[4];
		float farColor[4];

		static FogConstants From(const std::array<float, 12>& a_fog)
		{
			FogConstants out{};
			std::copy_n(a_fog.data(), 4, out.param);
			std::copy_n(a_fog.data() + 4, 3, out.nearColor);
			std::copy_n(a_fog.data() + 8, 3, out.farColor);
			return out;
		}
	};
}
