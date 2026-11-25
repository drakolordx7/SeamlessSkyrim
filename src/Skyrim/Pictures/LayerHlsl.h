#pragma once

#include "Core/Pictures/Layers.h"

// The depth image tier's march in HLSL: Core::Pictures::Layers::March, line for line. The constants
// come from Core; the shader that uses it declares Layers (t1), the layers' cbuffer fields
// (LayerInfo, AxisAcross, AxisUp, LayerPos, Origin, Inward) and Linear.
namespace Pictures::Hlsl
{
	[[nodiscard]] inline std::string LayerConstants()
	{
		namespace L = Core::Pictures::Layers;
		return std::format(
			"static const int   kSteps = {};\n"
			"static const int   kRefine = {};\n"
			"static const float kReach = {:.6f};\n"
			"static const float kFarFraction = {:.6f};\n"
			"static const float kToleranceAbs = {:.6f};\n"
			"static const float kToleranceRel = {:.6f};\n"
			"static const float kNoSurface = {:.6f};\n"
			"static const float kS0 = {:.6f}, kS1 = {:.6f}, kT0 = {:.6f}, kT1 = {:.6f};\n"
			"static const float kNearZ = {:.6f};\n",
			L::kSteps, L::kRefine, L::kReach, L::kFarFraction, L::kToleranceAbs, L::kToleranceRel, L::kNoSurface, L::kS0, L::kS1, L::kT0, L::kT1, L::kNearZ);
	}

	inline constexpr char kLayerMarch[] = R"(
		Texture2DArray<float4> Layers : register(t1);

		// 0: not in the layer's picture, 1: it sees past this point, 2: its surface is here, 3: hidden
		// from it by a nearer surface (Layers::VoteOf).
		int VoteOf(int layer, float3 x, bool exact, out float2 uv)
		{
			uv = 0;
			float3 rel = x - LayerPos[layer].xyz;
			if (rel.z <= kNearZ)
				return 0;
			float2 st = rel.xy / rel.z;
			if (st.x < kS0 || st.x > kS1 || st.y < kT0 || st.y > kT1)
				return 0;
			uv = float2((st.x - kS0) / (kS1 - kS0), 1 - (st.y - kT0) / (kT1 - kT0));
			int2  texel = int2(min(uv * LayerInfo.yz, LayerInfo.yz - 1));
			float stored = Layers.Load(int4(texel, layer, 0)).a;
			if (stored >= kNoSurface)
				return 1;
			float residual = rel.z - (stored - LayerPos[layer].z);
			float tolerance = exact ? 0 : kToleranceAbs + kToleranceRel * rel.z;
			return residual < -tolerance ? 1 : (residual > tolerance ? 3 : 2);
		}
		float ResidualOf(int layer, float3 x, float2 uv)
		{
			float3 rel = x - LayerPos[layer].xyz;
			int2   texel = int2(min(uv * LayerInfo.yz, LayerInfo.yz - 1));
			return abs(rel.z - (Layers.Load(int4(texel, layer, 0)).a - LayerPos[layer].z));
		}
		struct LayerLook { bool covered; bool solid; int layer; float2 uv; };
		LayerLook LookAt(float3 x, bool exact)
		{
			LayerLook look;
			look.covered = false;
			look.solid = false;
			look.layer = -1;
			look.uv = 0;
			int   nOn = 0, nFree = 0, nBehind = 0;
			float best = 1e30;
			[unroll] for (int i = 0; i < 3; ++i) {
				if (i < (int)LayerInfo.x) {
					float2 uv;
					int    vote = VoteOf(i, x, exact, uv);
					if (vote != 0) {
						look.covered = true;
						nOn += vote == 2 ? 1 : 0;
						nFree += vote == 1 ? 1 : 0;
						nBehind += vote == 3 ? 1 : 0;
						if (vote >= 2) {
							float residual = ResidualOf(i, x, uv);
							if (residual < best) {
								best = residual;
								look.layer = i;
								look.uv = uv;
							}
						}
					}
				}
			}
			look.solid = (nOn > 0 && nOn >= nFree) || (nOn == 0 && nBehind > 0 && nFree == 0);
			return look;
		}
		float3 AtPath(float3 origin, float3 dir, float p)
		{
			return origin + dir * (kReach * p / (1 - p) / dir.z);
		}
		// The eye's ray from the doorway's plane inward against the layers' depths. false: it left what
		// the layers cover (or the doorway's opening, before they begin) without meeting a surface;
		// the image tier draws that pixel. a_pixelAngle: the angle one pixel spans, for the mip.
		bool MarchLayers(float3 world, float3 dir, float pixelAngle, out float3 color, out float3 hitWorld, out float behind)
		{
			color = 0;
			hitWorld = world;
			behind = 0;
			float3 d3 = world - Origin.xyz;
			float3 origin = float3(dot(d3, AxisAcross.xyz), dot(d3, AxisUp.xyz), 0);
			float3 ray = float3(dot(dir, AxisAcross.xyz), dot(dir, AxisUp.xyz), max(dot(dir, Inward.xyz), 0.05));
			bool   wasCovered = false;
			bool   found = false;
			bool   gaveUp = false;
			float  before = 0;
			float  hitAt = 0;
			[loop] for (int step = 1; step <= kSteps; ++step) {
				float p = kFarFraction * step / kSteps;
				float3 x = AtPath(origin, ray, p);
				LayerLook look = LookAt(x, false);
				if (!look.covered) {
					if (wasCovered || x.x < 0 || x.x > AxisAcross.w || x.y < 0 || x.y > AxisUp.w) {
						gaveUp = true;
						break;
					}
					before = p;
				} else {
					wasCovered = true;
					if (look.solid) {
						found = true;
						hitAt = p;
						break;
					}
					before = p;
				}
			}
			if (!found || gaveUp)
				return false;
			float low = before, high = hitAt;
			[unroll] for (int r = 0; r < kRefine; ++r) {
				float middle = 0.5 * (low + high);
				if (LookAt(AtPath(origin, ray, middle), true).solid)
					high = middle;
				else
					low = middle;
			}
			float3 local = AtPath(origin, ray, high);
			LayerLook hit = LookAt(local, false);
			if (hit.layer < 0)
				return false;
			hitWorld = Origin.xyz + AxisAcross.xyz * local.x + AxisUp.xyz * local.y + Inward.xyz * local.z;
			behind = local.z;
			// The mip the picture is read at: a pixel's footprint at the hit against a texel's there.
			float footprint = length(hitWorld) * pixelAngle;
			float texel = (local.z - LayerPos[hit.layer].z) * (kS1 - kS0) / LayerInfo.y;
			float level = clamp(log2(max(footprint / max(texel, 1e-3), 1)), 0, 6);
			color = Layers.SampleLevel(Linear, float3(hit.uv, hit.layer), level).rgb;
			return true;
		}
	)";
}
