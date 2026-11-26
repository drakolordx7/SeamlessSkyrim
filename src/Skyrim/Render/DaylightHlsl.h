#pragma once

// The doorway daylight pass's shader (Render/DoorwayDaylight.cpp). One full-screen triangle; every
// pixel is rebuilt from the scene's depth, tested against the doorway, and given a gain by which the
// scene colour is brightened (the blend multiplies the target by 1 + gain). SunLit and SkyView are
// Core::Daylight's (src/Core/Lighting/Daylight.h) line for line; the tuning constants come from there as
// #defines in front of this text (the defaults here are for tools/check-hlsl.ps1, which compiles
// every shader string with fxc).
namespace Render::DaylightHlsl
{
	inline constexpr char kSource[] = R"(
#ifndef SEAMLESS_REACH
#define SEAMLESS_REACH 1400.0
#define SEAMLESS_DEPTH_FADE 8.0
#define SEAMLESS_EDGE_BASE 6.0
#define SEAMLESS_EDGE_GROWTH 0.02
#define SEAMLESS_GRAZING_LOW 0.02
#define SEAMLESS_GRAZING_HIGH 0.12
#define SEAMLESS_STREET_STEPS 12
#define SEAMLESS_STREET_REACH 700.0
#define SEAMLESS_PATCH_STEPS 8
#define SEAMLESS_PATCH_REACH 500.0
#define SEAMLESS_OCCLUDER_MIN 6.0
#define SEAMLESS_THICKNESS_BASE 40.0
#define SEAMLESS_THICKNESS_PER_UNIT 0.35
#define SEAMLESS_HIT 0.6
#define SEAMLESS_AMBIENT_FLOOR 0.05
#endif
#define SHADOW_STEPS 8

Texture2D<float> SceneDepth : register(t0);

cbuffer Daylight : register(b0)
{
	row_major float4x4 InverseViewProj;  // of the frame as drawn (jittered), camera-relative
	row_major float4x4 ViewProj;
	float4             ViewSize;    // the scene's viewport in pixels
	float4             DoorCenter;  // xyz: the doorway's centre relative to the camera; w: half width
	float4             DoorInward;  // xyz: from the street into the interior; w: half height
	float4             DoorRight;   // xyz
	float4             SunToward;   // xyz: unit vector to the sun; w: sun gain
	float4             SunTint;     // rgb: the sun's colour at luminance 1; a: spill gain
	float4             SkyTint;     // rgb: the sky's ambient colour at luminance 1
	float4             Shade;       // x: the sun's luminance; y: the sky's; z: the street's shadow strength
};

struct Vertex { float4 position : SV_Position; };

Vertex VS(uint id : SV_VertexID)
{
	Vertex v;
	float2 uv = float2((id << 1) & 2, id & 2);
	v.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	return v;
}

float Smooth(float low, float high, float x)
{
	float t = saturate((x - low) / (high - low));
	return t * t * (3.0 - 2.0 * t);
}

// The camera-relative position of the scene at a pixel.
float3 Reconstruct(float2 pixel, float depth)
{
	float2 ndc = float2(pixel.x / ViewSize.x * 2 - 1, 1 - pixel.y / ViewSize.y * 2);
	float4 p = mul(float4(ndc, depth, 1), InverseViewProj);
	return p.xyz / p.w;
}

// Core::Daylight::SunLit.
float SunLit(float3 rel, float depth)
{
	float along = dot(SunToward.xyz, DoorInward.xyz);
	if (depth <= 0 || along >= -SEAMLESS_GRAZING_LOW)
		return 0;
	float t = depth / -along;
	if (t >= SEAMLESS_REACH)
		return 0;
	float3 hit = rel + SunToward.xyz * t;
	float edge = SEAMLESS_EDGE_BASE + SEAMLESS_EDGE_GROWTH * t;
	float u = abs(dot(hit, DoorRight.xyz));
	float v = abs(hit.z);
	float inside = (1 - Smooth(DoorCenter.w - edge, DoorCenter.w, u)) * (1 - Smooth(DoorInward.w - edge, DoorInward.w, v));
	return inside * (1 - Smooth(0.75 * SEAMLESS_REACH, SEAMLESS_REACH, t)) * Smooth(0, SEAMLESS_DEPTH_FADE, depth) * Smooth(SEAMLESS_GRAZING_LOW, SEAMLESS_GRAZING_HIGH, -along);
}

// Core::Daylight::SkyView (a_normal of length 0: facing unknown).
float SkyView(float3 rel, float3 normal)
{
	float r2 = max(dot(rel, rel), 100.0);
	float r = sqrt(r2);
	float atOpening = dot(rel, DoorInward.xyz) / r;
	if (atOpening <= 0)
		return 0;
	float atSurface = dot(normal, normal) > 0 ? max(-dot(rel, normal) / r, 0) : 0.5;
	float area = 4 * DoorCenter.w * DoorInward.w;
	return saturate(area * atOpening * atSurface / (3.14159265 * r2 + area));
}

// What the room or interior in between hides of the way from the surface to the doorway, from the
// depth the camera sees: samples along the sun's ray that lie just behind a surface in front of them.
float RayLit(float3 p, float3 n, float t, float2 pixel)
{
	float  noise = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
	float3 start = p + n * 3.0;
	float3 end = p + SunToward.xyz * (t * 0.97);
	float  lit = 1;
	[unroll] for (int i = 0; i < SHADOW_STEPS; ++i)
	{
		float3 q = lerp(start, end, (i + noise) / SHADOW_STEPS);
		float4 clip = mul(float4(q, 1), ViewProj);
		if (clip.w <= 0)
			continue;
		float2 ndc = clip.xy / clip.w;
		float2 px = float2((ndc.x * 0.5 + 0.5) * ViewSize.x, (0.5 - ndc.y * 0.5) * ViewSize.y);
		if (px.x < 0 || px.y < 0 || px.x >= ViewSize.x || px.y >= ViewSize.y)
			continue;
		float3 scene = Reconstruct(px, SceneDepth.Load(int3(px, 0)));
		float  gap = length(q) - length(scene);  // above 0: the sample is behind a surface the camera sees
		if (gap > 6.0 && gap < 70.0)
			lit *= 0.35;
	}
	return lit;
}

// Whether the camera sees something at q's pixel, and how far q lies behind it (above 0: behind
// the surface seen there). Core::StreetShade's gapAt.
bool GapAt(float3 q, out float gap)
{
	gap = 0;
	float4 clip = mul(float4(q, 1), ViewProj);
	if (clip.w <= 0)
		return false;
	float2 ndc = clip.xy / clip.w;
	float2 px = float2((ndc.x * 0.5 + 0.5) * ViewSize.x, (0.5 - ndc.y * 0.5) * ViewSize.y);
	if (px.x < 0 || px.y < 0 || px.x >= ViewSize.x || px.y >= ViewSize.y)
		return false;
	float3 scene = Reconstruct(px, SceneDepth.Load(int3(px, 0)));
	gap = length(q) - length(scene);
	return true;
}

// Core::StreetShade::Hit.
float OccluderHit(float gap, float along)
{
	float thickness = SEAMLESS_THICKNESS_BASE + SEAMLESS_THICKNESS_PER_UNIT * along;
	return Smooth(SEAMLESS_OCCLUDER_MIN, SEAMLESS_OCCLUDER_MIN * 1.7, gap) * (1 - Smooth(0.7 * thickness, thickness, gap));
}

// Core::StreetShade::Lit: the share of the sun's light that reaches start along the sun's ray.
float MarchLit(float3 start, float rayLength, float noise, int steps)
{
	float lit = 1;
	[loop] for (int i = 0; i < steps; ++i)
	{
		float x = (i + noise) / steps;
		float t = rayLength * x * x;
		float gap;
		if (GapAt(start + SunToward.xyz * t, gap))
			lit *= 1 - SEAMLESS_HIT * OccluderHit(gap, t);
	}
	return lit;
}

float PixelNoise(float2 pixel)
{
	return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float4 PS(Vertex v) : SV_Target
{
	float  depth = SceneDepth.Load(int3(v.position.xy, 0));
	float3 p = Reconstruct(v.position.xy, depth);
	// Before any test that can leave: the derivatives need the whole quad.
	float3 n = cross(ddx(p), ddy(p));
	float3 rel = p - DoorCenter.xyz;
	float  inside = dot(rel, DoorInward.xyz);
	if (inside <= 0)
		discard;
	float nl = length(n);
	n = nl > 1e-4 ? n / nl : float3(0, 0, 0);
	if (dot(n, p) > 0)
		n = -n;  // facing the camera

	float3 gain = 0;
	if (SunToward.w > 0)
	{
		float lit = SunLit(rel, inside);
		if (lit > 0)
		{
			float along = dot(SunToward.xyz, DoorInward.xyz);
			float cosine = nl > 1e-4 ? saturate(dot(n, SunToward.xyz)) : 0.7;
			float t = inside / -along;
			float ray = RayLit(p, n, t, v.position.xy);
			// Past the doorway, the street's own geometry in the sun's way (the eave, the house opposite).
			ray *= MarchLit(p + SunToward.xyz * t, SEAMLESS_PATCH_REACH, PixelNoise(v.position.xy), SEAMLESS_PATCH_STEPS);
			gain += SunTint.rgb * (SunToward.w * lit * cosine * ray);
		}
	}
	if (SunTint.a > 0)
		gain += SkyTint.rgb * (SunTint.a * SkyView(rel, n));
	if (!any(gain))
		discard;
	return float4(gain, 0);
}

// The street merged into an interior has no sun shadow of its own: its pixels seen through the doorway
// are darkened where the march to the sun meets something the camera sees. The result is the factor
// the target is multiplied by (blend: ZERO, SRC_COLOR). For a camera inside the interior only.
float4 PSShadow(Vertex v) : SV_Target
{
	float  depth = SceneDepth.Load(int3(v.position.xy, 0));
	float3 p = Reconstruct(v.position.xy, depth);
	float3 n = cross(ddx(p), ddy(p));
	float3 rel = p - DoorCenter.xyz;
	float  side = dot(rel, DoorInward.xyz);
	if (side >= -2.0 || depth >= 1.0)
		discard;
	// Only what is seen through the doorway: the ray from the camera (the origin) to the pixel
	// crosses the doorway's plane inside the rectangle.
	float planeAt = dot(DoorCenter.xyz, DoorInward.xyz);
	float alongRay = dot(p, DoorInward.xyz);
	if (alongRay >= -1e-3)
		discard;
	float3 hit = p * (planeAt / alongRay) - DoorCenter.xyz;
	if (abs(dot(hit, DoorRight.xyz)) > DoorCenter.w + 3.0 || abs(hit.z) > DoorInward.w + 3.0)
		discard;
	float nl = length(n);
	n = nl > 1e-4 ? n / nl : float3(0, 0, 0);
	if (dot(n, p) > 0)
		n = -n;
	float cosine = nl > 1e-4 ? saturate(dot(n, SunToward.xyz)) : 0.7;
	float sun = Shade.x * cosine;
	float share = sun / (sun + Shade.y + SEAMLESS_AMBIENT_FLOOR);
	if (share <= 0.01)
		discard;
	float lit = MarchLit(p + n * 4.0, SEAMLESS_STREET_REACH, PixelNoise(v.position.xy), SEAMLESS_STREET_STEPS);
	float factor = 1 - Shade.z * share * (1 - lit);
	if (factor >= 0.999)
		discard;
	return float4(factor, factor, factor, 1);
}
)";
}
