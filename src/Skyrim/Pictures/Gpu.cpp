#include "Skyrim/Pictures/Gpu.h"

#include "Core/Pictures/Compress.h"
#include "Skyrim/Pictures/LayerHlsl.h"
#include "Skyrim/Render/D3D.h"

namespace Pictures::Gpu
{
	namespace
	{
		struct Pipeline
		{
			ID3D11VertexShader*      captureVS = nullptr;
			ID3D11PixelShader*       capturePS = nullptr;
			ID3D11Buffer*            captureConstants = nullptr;
			ID3D11Texture2D*         target = nullptr;  // at the size pictures are taken at now (EnsureTarget): RGBA16F with mips
			ID3D11RenderTargetView*  targetView = nullptr;
			ID3D11ShaderResourceView* targetMips = nullptr;  // all its mips, for GenerateMips
			std::vector<ID3D11ShaderResourceView*> targetLevels;  // one per mip, what the compression reads
			ID3D11Texture2D*         distance = nullptr;  // R16_FLOAT: how far behind the doorway, as the capture writes it beside the colour
			ID3D11RenderTargetView*  distanceView = nullptr;
			ID3D11Texture2D*         blocks = nullptr;  // R32G32B32A32_UINT, a texel a BC6H block: what the compression writes, copied into the picture
			ID3D11UnorderedAccessView* blocksView = nullptr;
			ID3D11ComputeShader*     compressCS = nullptr;
			ID3D11Buffer*            compressConstants = nullptr;
			UINT                     targetWidth = 0;
			UINT                     targetHeight = 0;
			ID3D11VertexShader*      drawVS = nullptr;
			ID3D11PixelShader*       drawPS = nullptr;
			ID3D11InputLayout*       layout = nullptr;
			ID3D11Buffer*            vertices = nullptr;
			ID3D11Buffer*            drawConstants = nullptr;
			ID3D11SamplerState*      sampler = nullptr;
			ID3D11DepthStencilState* depthWrite = nullptr;
			ID3D11DepthStencilState* depthTest = nullptr;
			ID3D11DepthStencilState* noDepth = nullptr;
			ID3D11BlendState*        blend = nullptr;
			ID3D11BlendState*        opaque = nullptr;
			ID3D11RasterizerState*   raster = nullptr;
			bool                     failed = false;
		} pipeline;

		// The capture's targets and what the compression works in.
		void ReleaseTarget()
		{
			for (auto*& level : pipeline.targetLevels) {
				Render::D3D::Release(level);
			}
			pipeline.targetLevels.clear();
			Render::D3D::Release(pipeline.blocksView);
			Render::D3D::Release(pipeline.blocks);
			Render::D3D::Release(pipeline.distanceView);
			Render::D3D::Release(pipeline.distance);
			Render::D3D::Release(pipeline.targetMips);
			Render::D3D::Release(pipeline.targetView);
			Render::D3D::Release(pipeline.target);
			pipeline.targetWidth = pipeline.targetHeight = 0;
		}

		// Capture: each texel of the picture is the doorway's rectangle at that place (a depth image's
		// layer: the view's own image), as the frame shows it, averaged over 4x4 points (a texel
		// covers about a pixel at the default size). Alpha: how far behind the doorway the surface
		// seen there is.
		constexpr char kCaptureDeclarations[] = R"(
			Texture2D<float4> Scene : register(t0);
			Texture2D<float>  Depth : register(t1);
			SamplerState      Linear : register(s0);
			cbuffer Capture : register(b0)
			{
				float4 Corner0;  // clip space: the picture's bottom
				float4 Corner1;  // bottom, across
				float4 Corner2;  // top
				// xy: the scene's viewport in pixels; zw: a viewport pixel's size in the texture (the
				// main scene's viewport is the top-left part of its texture at a render scale below
				// 1; the portal pass's image always fills its target).
				float4 Sizes;
				float4 Out;      // xy: the picture's size
				row_major float4x4 InverseViewProj;
				float4 WColumn;  // a camera-relative point's distance along the view axis: dot(p, xyz) + w
				float4 Origin;   // the doorway's bottom corner, camera-relative
				float4 Inward;   // the doorway's normal, away from the eye
				// x, y: the portal pass's near and far; z: 1 = Depth is the portal pass's; w: the capture
				// camera's distance to the doorway (looking along its normal; negative: from behind it, a
				// depth image's view), 0 = the player's view
				float4 Portal;
			};
)";
		constexpr char kCaptureCode[] = R"(
			// The sky and what is too far to tell apart from it; within half precision.
			static const float kFar = 30000;
			float4 VS(uint id : SV_VertexID) : SV_Position
			{
				float2 p = float2((id << 1) & 2, id & 2);
				return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
			}
			struct Captured { float4 color : SV_Target0; float distance : SV_Target1; };
			float Behind(float2 ndc, float2 pixel)
			{
				float2 size;
				Depth.GetDimensions(size.x, size.y);
				float  depth = Depth.Load(int3(clamp(pixel * Sizes.zw * size, 0, size - 1), 0));
				float3 world;
				if (Portal.z > 0.5) {
					// As the doorway quad reads it (Composite): the distance along the view axis, then
					// the point on this pixel's ray.
					if (depth >= 1)
						return kFar;
					float  viewDistance = PortalViewDistance(depth, Portal.x, Portal.y);
					if (Portal.w != 0)
						return clamp(viewDistance - Portal.w, 1, kFar);
					float4 p = mul(float4(ndc, 0.5, 1), InverseViewProj);
					p.xyz /= p.w;
					world = p.xyz * viewDistance / (dot(p.xyz, WColumn.xyz) + WColumn.w);
				} else {
					if (depth >= 0.999999)
						return kFar;
					float4 p = mul(float4(ndc, depth, 1), InverseViewProj);
					world = p.xyz / p.w;
				}
				// In front of the doorway (its frame, a leaf swung towards the eye): at it.
				return clamp(dot(world - Origin.xyz, Inward.xyz), 1, kFar);
			}
			struct Captured
			{
				float4 color : SV_Target0;     // rgb, and the distance in alpha (the brightness read)
				float  distance : SV_Target1;  // the same distance, for the picture's R16_FLOAT
			};
			Captured PS(float4 position : SV_Position)
			{
				Captured o;
				float3 sum = 0;
				float3 nearSum = 0;
				float  nearest = kFar;
				float  count = 0;
				[unroll] for (int i = 0; i < 4; ++i)
				[unroll] for (int j = 0; j < 4; ++j) {
					float2 uv = (floor(position.xy) + (float2(i, j) + 0.5) / 4) / Out.xy;
					float4 clip = Corner0 + uv.x * (Corner1 - Corner0) + (1 - uv.y) * (Corner2 - Corner0);
					float2 ndc = clip.xy / clip.w;
					float2 pixel = float2((ndc.x * 0.5 + 0.5) * Sizes.x, (0.5 - ndc.y * 0.5) * Sizes.y);
					float3 color = Scene.SampleLevel(Linear, pixel * Sizes.zw, 0).rgb;
					float  behind = Behind(ndc, pixel);
					sum += color;
					if (behind < kFar) {
						nearSum += color;
						nearest = min(nearest, behind);
						++count;
					}
				}
				if (count == 0) {
					o.color = float4(max(sum / 16, 0), kFar);
					o.distance = kFar;
					return o;
				}
				// The sky left out where anything else is: through a gate the real sky is shown where
				// the picture has none (Draw), and a roof's edge averaged with the portal pass's flat
				// sky colour stood in it as a blue fringe. The nearest of a texel's surfaces is its depth,
				// not their mean: an edge's texel is the thing in front, as the sharp picture has it, and
				// a lamp in front of a wall far behind it isn't pulled away to the wall.
				o.color = float4(max(nearSum / count, 0), nearest);
				o.distance = nearest;
				return o;
			}
)";

		// Compress: one thread a 4x4 block of the capture's colour into BC6H mode 11 (one subset, two 10-bit
		// endpoints, 4-bit indices), Core::Pictures::Bc6h::Encode step for step: the endpoints from the
		// block's principal axis in half-float bit patterns (the numbers the format interpolates), fitted
		// twice by least squares, then each texel takes the index whose decoded colour is nearest. A level
		// of the capture's mips at a time; the blocks go to a uint texture the picture's BC6H texture is
		// copied from.
		constexpr char kCompressCode[] = R"(
			Texture2D<float4>   Source : register(t0);
			RWTexture2D<uint4>  Blocks : register(u0);
			cbuffer Compress : register(b0)
			{
				uint4 Size;  // xy: the level's size in texels; zw: in blocks
			};
			static const uint kWeights[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };
			static const float kMaxBits = 31743.0;
			uint Unquantize(uint q) { return q == 0 ? 0 : (q == 1023 ? 0xFFFF : ((q << 16) + 0x8000) >> 10); }
			uint Interpolate(uint e0, uint e1, uint w) { uint m = (e0 * (64 - w) + e1 * w + 32) >> 6; return (m * 31) >> 6; }
			uint Quantize(float bits) { return (uint)clamp(floor((bits - 15.5) / 31.0 + 0.5), 0.0, 1023.0); }
			float HalfBits(float v) { return (float)f32tof16(!(v > 0.0) ? 0.0 : min(v, 65504.0)); }
			void Put(inout uint4 block, uint position, uint count, uint value)
			{
				uint word = position >> 5, offset = position & 31;
				value &= (1u << count) - 1;
				block[word] |= value << offset;
				if (offset + count > 32)
					block[word + 1] |= value >> (32 - offset);
			}
			[numthreads(8, 8, 1)]
			void CS(uint3 id : SV_DispatchThreadID)
			{
				if (id.x >= Size.z || id.y >= Size.w)
					return;
				float3 h[16];
				float3 mean = 0, low = 1e30, high = -1e30;
				[unroll] for (uint i = 0; i < 16; ++i) {
					int2   p = min(int2(id.xy * 4) + int2(i & 3, i >> 2), int2(Size.xy) - 1);
					float3 c = Source.Load(int3(p, 0)).rgb;
					h[i] = float3(HalfBits(c.r), HalfBits(c.g), HalfBits(c.b));
					mean += h[i] / 16.0;
					low = min(low, h[i]);
					high = max(high, h[i]);
				}
				float3x3 cov = 0;
				[unroll] for (uint k = 0; k < 16; ++k) {
					float3 d = h[k] - mean;
					cov += float3x3(d.x * d, d.y * d, d.z * d);
				}
				float3 axis = high - low;
				[unroll] for (uint iteration = 0; iteration < 4; ++iteration) {
					float3 next = mul(cov, axis);
					float  length2 = dot(next, next);
					if (length2 > 1e-12)
						axis = next * rsqrt(length2);
				}
				float tMin = 1e30, tMax = -1e30;
				[unroll] for (uint m = 0; m < 16; ++m) {
					float p = dot(h[m] - mean, axis);
					tMin = min(tMin, p);
					tMax = max(tMax, p);
				}
				float3 e0 = clamp(mean + axis * tMin, 0, kMaxBits);
				float3 e1 = clamp(mean + axis * tMax, 0, kMaxBits);
				bool   fitting = true;
				[unroll] for (uint fit = 0; fit < 2; ++fit) {
					float3 d = e1 - e0;
					float  dd = dot(d, d);
					if (fitting && dd >= 1e-6) {
						float  aa = 0, bb = 0, ab = 0;
						float3 ax = 0, bx = 0;
						[unroll] for (uint n = 0; n < 16; ++n) {
							float s = dot(h[n] - e0, d) / dd;
							int   index = clamp((int)floor(s * 15.0 + 0.5), 0, 15);
							float w = (float)kWeights[index] / 64.0;
							float a = 1.0 - w;
							aa += a * a;
							bb += w * w;
							ab += a * w;
							ax += a * h[n];
							bx += w * h[n];
						}
						float det = aa * bb - ab * ab;
						if (abs(det) >= 1e-4) {
							e0 = clamp((ax * bb - bx * ab) / det, 0, kMaxBits);
							e1 = clamp((bx * aa - ax * ab) / det, 0, kMaxBits);
						} else {
							fitting = false;
						}
					} else {
						fitting = false;
					}
				}
				uint3 q0 = uint3(Quantize(e0.x), Quantize(e0.y), Quantize(e0.z));
				uint3 q1 = uint3(Quantize(e1.x), Quantize(e1.y), Quantize(e1.z));
				uint3 u0 = uint3(Unquantize(q0.x), Unquantize(q0.y), Unquantize(q0.z));
				uint3 u1 = uint3(Unquantize(q1.x), Unquantize(q1.y), Unquantize(q1.z));
				float3 palette[16];
				[unroll] for (uint r = 0; r < 16; ++r)
					palette[r] = float3(Interpolate(u0.x, u1.x, kWeights[r]), Interpolate(u0.y, u1.y, kWeights[r]), Interpolate(u0.z, u1.z, kWeights[r]));
				uint indices[16];
				[unroll] for (uint t = 0; t < 16; ++t) {
					float best = 1e30;
					indices[t] = 0;
					[unroll] for (uint j = 0; j < 16; ++j) {
						float3 d = h[t] - palette[j];
						float  error = dot(d, d);
						if (error < best) {
							best = error;
							indices[t] = j;
						}
					}
				}
				if (indices[0] >= 8) {
					uint3 swap = q0;
					q0 = q1;
					q1 = swap;
					[unroll] for (uint z = 0; z < 16; ++z)
						indices[z] = 15 - indices[z];
				}
				uint4 block = 0;
				Put(block, 0, 5, 3);
				Put(block, 5, 10, q0.x);
				Put(block, 15, 10, q0.y);
				Put(block, 25, 10, q0.z);
				Put(block, 35, 10, q1.x);
				Put(block, 45, 10, q1.y);
				Put(block, 55, 10, q1.z);
				uint position = 65;
				[unroll] for (uint w = 0; w < 16; ++w) {
					uint bits = w == 0 ? 3 : 4;
					Put(block, position, bits, indices[w]);
					position += bits;
				}
				Blocks[id.xy] = block;
			}
)";

		// Draw: the doorway's picture (or the plain colour) in the doorway, as the space behind it in
		// relief. The image tier marches the eye's ray against the one picture's depth; the depth
		// image tier (LayerInfo.x layers) against the layers', and where it finds nothing the image
		// tier's answer stands. The doorway's own depth is kept (written, so what is drawn later is
		// hidden behind it as by a wall there); the motion is the surface's seen through it.
		constexpr char kDrawDeclarations[] = R"(
				Texture2D<float4> Picture : register(t0);          // BC6H colour with its mips
				Texture2D<float>  PictureDistance : register(t2);  // how far behind the doorway, R16_FLOAT
				SamplerState      Linear : register(s0);  // anisotropic
				cbuffer Draw : register(b0)
				{
					row_major float4x4 ViewProj;          // as drawn (jittered)
					row_major float4x4 ViewProjUnjittered;
					row_major float4x4 PreviousViewProj;  // unjittered, relative to the previous eye
					float4             EyeDelta;          // this frame's eye - the previous frame's
					float4             Fog;               // as the engine's FogParam
					float4             FogNear;
					float4             FogFar;
					float4             Plain;             // rgb: the colour without a picture; w: 1 = the picture
					float4             Params;            // x: alpha; yz: a texel's size
					float4             Origin;            // xyz: the doorway's bottom corner, camera-relative; w: 1 = the picture has depth
					float4             Across;            // xyz: its bottom edge; w: 1 / its length squared
					float4             Up;                // xyz: its side going up; w: 1 / its length squared
					float4             Inward;            // xyz: its normal, away from the eye; w: how far behind it the picture was taken from (negative)
					float4             TakenFrom;         // xyz: where, camera-relative; w: how far behind the doorway the plain colour stands
					float4             Look;              // x: Adaptation::Kind; y: daylight; z: 1 = the outdoors behind; w: the colour's scale (exposure, light now against when taken)
					float4             Extent;            // the part of the door's plane the picture covers, as Extent
					float4             Gate;              // x: 1 = a city gate (the same sky on both sides)
					float4             LayerInfo;         // x: layers of the depth image (0 = none); yz: a layer's size in texels
					float4             AxisAcross;        // xyz: the doorway's bottom edge, unit; w: its width in units
					float4             AxisUp;            // xyz: its side going up, unit; w: its height in units
					float4             LayerPos[3];       // xyz: each view's place in the doorway's units
				};
)";
		constexpr char kDrawCode[] = R"(
			static const float kFar = 30000;
			struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; float3 world : TEXCOORD1; float depth : TEXCOORD2; };
			Vertex VS(float3 world : POSITION, float2 uv : TEXCOORD0)
			{
				Vertex v;
				v.position = mul(float4(world, 1), ViewProj);
				v.uv = uv;
				v.world = world;
				v.depth = v.position.w;
				return v;
			}
			struct Pixel { float4 color : SV_Target0; float2 motion : SV_Target1; };
			float2 PictureUV(float3 p)
			{
				float3 d = p - Origin.xyz;
				float2 inDoorway = float2(dot(d, Across.xyz) * Across.w, dot(d, Up.xyz) * Up.w);
				return float2((inDoorway.x - Extent.x) / (Extent.y - Extent.x), 1 - (inDoorway.y - Extent.z) / (Extent.w - Extent.z));
			}
			// The nearest texel's: a depth read between a wall's and a chair's made a slope between them
			// that rays landed on, in streaks along every edge.
			float StoredBehind(float2 uv)
			{
				if (Plain.w < 0.5)
					return TakenFrom.w;
				// A picture without depth: everything at the doorway, drawn flat as before.
				if (Origin.w < 0.5)
					return 0;
				return PictureDistance.Load(int3(clamp(uv / Params.yz, 0, 1 / Params.yz - 1), 0));
			}
			// Along the eye's ray past the doorway, the place its points are seen at from where the
			// picture was taken moves in a straight line across the picture: from where the ray
			// meets the doorway (t = 0) towards where it would at infinite distance (t = 1). t is
			// also how far behind the doorway a point is, as h / (h + the distance the picture was
			// taken from), so a stored distance converts to it directly.
			float TOf(float a_behind) { return a_behind / (a_behind - Inward.w); }
			Pixel PS(Vertex v)
			{
				Pixel  p;
				float3 dir = normalize(v.world);
				// The angle a pixel spans, for the depth image's mip.
				float  pixelAngle = 0.5 * (length(ddx(dir)) + length(ddy(dir)));
				// Grazing the doorway, infinitely far is off any picture; it is cut off at the sides anyway.
				float  facing = max(dot(dir, Inward.xyz), 0.05);
				float2 uvNear = PictureUV(v.world);
				float2 uvFar = PictureUV(TakenFrom.xyz + dir * (-Inward.w / facing));
				float2 delta = uvFar - uvNear;
				// Past the picture's sides is what the doorway's sides hid where it was taken from: the
				// ray takes the colour at the side it leaves by.
				float  end = 1;
				[unroll] for (int axis = 0; axis < 2; ++axis) {
					if (delta[axis] > 1e-6)
						end = min(end, (1 - uvNear[axis]) / delta[axis]);
					else if (delta[axis] < -1e-6)
						end = min(end, -uvNear[axis] / delta[axis]);
				}
				end = saturate(end);
				float before = 0;
				float t = end;
				bool  hit = false;
				[loop] for (int i = 1; i <= 24; ++i) {
					float at = end * i / 24;
					float surface = TOf(StoredBehind(uvNear + delta * at));
					// Through a gate the eye looks under the gatehouse's arches from lower down than
					// the picture was taken: the ray comes at an arch from behind, where the picture
					// has the arch over the street it passes on to. As a solid reaching back forever
					// the arch was drawn out over that street in streaks; a near thing is taken as a
					// shell a few steps deep. A room's walls stay solid.
					if (Gate.x > 0.5 && at - surface > 2.0 * end / 24) {
						before = at;
						continue;
					}
					if (at >= surface) {
						hit = true;
						t = at;
						[unroll] for (int j = 0; j < 5; ++j) {
							float middle = 0.5 * (before + t);
							if (middle >= TOf(StoredBehind(uvNear + delta * middle)))
								t = middle;
							else
								before = middle;
						}
						break;
					}
					before = at;
				}
				t = min(t, 0.9995);
				float2 uv = uvNear + delta * t;
				float  behind = min(t * -Inward.w / (1 - t), kFar);
				float3 world = v.world + dir * (behind / facing);
				// As the live gate (Composite's doorway quad): where the far side has nothing, the
				// player's own worldspace shows, with the real sky. The portal pass's is a flat colour,
				// and a picture's sky is of another hour and weather. A ray that met nothing in the
				// picture has left it past what was taken: the world there is the nearest guess.
				if (Gate.x > 0.5 && Plain.w > 0.5 && (!hit || behind >= kFar * 0.5))
					discard;

				float3 color;
				if (Plain.w > 0.5) {
					// Sharp: the picture's own texels, filtered by how big the screen shows them (the
					// anisotropic filter follows the stretch of the ray's march across the picture).
					color = Picture.SampleGrad(Linear, uv, ddx(uv), ddy(uv)).rgb;
				} else if (Look.z > 0.5) {
					// The outdoors: lighter up towards the sky.
					color = Plain.rgb * lerp(1.2, 0.85, saturate(uv.y));
				} else {
					// A room: its floor lit from the doorway, its ceiling dark.
					color = Plain.rgb * lerp(0.6, 1.2, saturate(uv.y));
				}
				bool pictured = Plain.w > 0.5;
				// The depth image, where the layers cover the ray: its surface, in parallax.
				if (LayerInfo.x > 0.5) {
					float3 layerColor, layerWorld;
					float  layerBehind;
					if (MarchLayers(v.world, dir, pixelAngle, layerColor, layerWorld, layerBehind)) {
						color = layerColor;
						world = layerWorld;
						behind = layerBehind;
						pictured = true;
					}
				}

				// The colour as bright as the real space would be (and, for a picture, as an eye adapted
				// to the viewer's space sees it: Pictures' exposure). Only the plain colour is made as an
				// eye used to the player's space sees the other, eased out as it fades over the real
				// space (which is drawn without it): the doorway brightens or dims into it like an eye
				// adjusting.
				color *= Look.w;
				float  adapt = pictured ? 0 : Look.y * Params.x;
				float  luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
				if (Look.x > 0.5 && Look.x < 1.5) {
					// A room from the street by day: dim, dimmer the deeper in, but its lamps, fire
					// and windows still bright.
					float lit = saturate(luminance / 1.5);
					float dark = 0.6 * lerp(1, exp(-behind / 800), 0.5);
					color *= lerp(1, lerp(dark, 1, lit), adapt);
				} else if (Look.x > 1.5) {
					// The street from a room by day: a little washed out, and bright enough for the
					// bloom to glow around the doorway.
					color = lerp(color, luminance.xxx, 0.25 * adapt) * lerp(1, 1.6, adapt);
				}

				// The picture has the fog between where it was taken and what it shows already; the
				// eye's own is up to the doorway.
				p.color = float4(Fogged(color, v.depth), Params.x);
				float4 now = mul(float4(world, 1), ViewProjUnjittered);
				float4 then = mul(float4(world + EyeDelta.xyz, 1), PreviousViewProj);
				p.motion = float2(-0.5, 0.5) * (now.xy / now.w - then.xy / then.w);
				return p;
			}
)";
	}

	bool EnsurePipeline(ID3D11Device* a_device)
	{
		if (pipeline.captureVS || pipeline.failed) {
			return !pipeline.failed;
		}
		const std::string captureSource = std::string(kCaptureDeclarations) + Hlsl::kViewDistance + kCaptureCode;
		const std::string drawSource = std::string(kDrawDeclarations) + Hlsl::LayerConstants() + Hlsl::kFogged + Hlsl::kLayerMarch + kDrawCode;
		std::vector<ID3DBlob*> blobs;
		const auto             compile = [&](const std::string& a_source, const char* a_entry, const char* a_profile) -> ID3DBlob* {
			std::string errors;
			auto*       out = Render::D3D::Compile(a_source, "SeamlessDoorway", a_entry, a_profile, 0, &errors);
			if (!out) {
				logger::error("doorway snapshot: shader {} failed: {}", a_entry, errors);
				pipeline.failed = true;
			} else {
				blobs.push_back(out);
			}
			return out;
		};
		auto* compressCS = compile(kCompressCode, "CS", "cs_5_0");
		auto* captureVS = compile(captureSource, "VS", "vs_5_0");
		auto* capturePS = compile(captureSource, "PS", "ps_5_0");
		auto* drawVS = compile(drawSource, "VS", "vs_5_0");
		auto* drawPS = compile(drawSource, "PS", "ps_5_0");
		if (!pipeline.failed) {
			const D3D11_INPUT_ELEMENT_DESC elements[]{
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};
			D3D11_BUFFER_DESC    vertices{ sizeof(float) * 5 * 4, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
			D3D11_BUFFER_DESC    captureConstants{ sizeof(CaptureConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
			D3D11_BUFFER_DESC    compressConstants{ sizeof(float) * 4, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
			D3D11_BUFFER_DESC    drawConstants{ sizeof(DrawConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
			D3D11_SAMPLER_DESC   sampler{};
			sampler.Filter = D3D11_FILTER_ANISOTROPIC;
			sampler.MaxAnisotropy = 8;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampler.MaxLOD = D3D11_FLOAT32_MAX;
			D3D11_DEPTH_STENCIL_DESC depthWrite{};
			depthWrite.DepthEnable = TRUE;
			depthWrite.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			depthWrite.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
			D3D11_DEPTH_STENCIL_DESC depthTest = depthWrite;
			depthTest.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			D3D11_DEPTH_STENCIL_DESC noDepth{};
			D3D11_BLEND_DESC         blend{};
			blend.IndependentBlendEnable = TRUE;
			blend.RenderTarget[0].BlendEnable = TRUE;
			blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
			blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
			blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
			blend.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			D3D11_BLEND_DESC opaque{};
			opaque.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			D3D11_RASTERIZER_DESC raster{};
			raster.FillMode = D3D11_FILL_SOLID;
			raster.CullMode = D3D11_CULL_NONE;
			raster.DepthClipEnable = TRUE;
			pipeline.failed =
				FAILED(a_device->CreateComputeShader(compressCS->GetBufferPointer(), compressCS->GetBufferSize(), nullptr, &pipeline.compressCS)) ||
				FAILED(a_device->CreateBuffer(&compressConstants, nullptr, &pipeline.compressConstants)) ||
				FAILED(a_device->CreateVertexShader(captureVS->GetBufferPointer(), captureVS->GetBufferSize(), nullptr, &pipeline.captureVS)) ||
				FAILED(a_device->CreatePixelShader(capturePS->GetBufferPointer(), capturePS->GetBufferSize(), nullptr, &pipeline.capturePS)) ||
				FAILED(a_device->CreateVertexShader(drawVS->GetBufferPointer(), drawVS->GetBufferSize(), nullptr, &pipeline.drawVS)) ||
				FAILED(a_device->CreatePixelShader(drawPS->GetBufferPointer(), drawPS->GetBufferSize(), nullptr, &pipeline.drawPS)) ||
				FAILED(a_device->CreateInputLayout(elements, 2, drawVS->GetBufferPointer(), drawVS->GetBufferSize(), &pipeline.layout)) ||
				FAILED(a_device->CreateBuffer(&vertices, nullptr, &pipeline.vertices)) || FAILED(a_device->CreateBuffer(&captureConstants, nullptr, &pipeline.captureConstants)) ||
				FAILED(a_device->CreateBuffer(&drawConstants, nullptr, &pipeline.drawConstants)) || FAILED(a_device->CreateSamplerState(&sampler, &pipeline.sampler)) ||
				FAILED(a_device->CreateDepthStencilState(&depthWrite, &pipeline.depthWrite)) || FAILED(a_device->CreateDepthStencilState(&depthTest, &pipeline.depthTest)) ||
				FAILED(a_device->CreateDepthStencilState(&noDepth, &pipeline.noDepth)) || FAILED(a_device->CreateBlendState(&blend, &pipeline.blend)) ||
				FAILED(a_device->CreateBlendState(&opaque, &pipeline.opaque)) || FAILED(a_device->CreateRasterizerState(&raster, &pipeline.raster));
			if (pipeline.failed) {
				logger::error("doorway snapshot: could not create the pipeline");
			}
		}
		for (auto* blob : blobs) {
			blob->Release();
		}
		return !pipeline.failed;
	}

	bool Failed()
	{
		return pipeline.failed;
	}

	bool EnsureTarget(ID3D11Device* a_device, UINT a_width, UINT a_height)
	{
		if (pipeline.targetView && pipeline.targetWidth == a_width && pipeline.targetHeight == a_height) {
			return true;
		}
		ReleaseTarget();
		// Sizes are whole blocks (Core::Pictures::Compress::RoundUp).
		const UINT levels = Core::Pictures::Compress::MipCount(a_width, a_height);
		const auto blocksAcross = Core::Pictures::Compress::Blocks(a_width), blocksDown = Core::Pictures::Compress::Blocks(a_height);
		D3D11_TEXTURE2D_DESC target{ a_width, a_height, levels, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0,
			D3D11_RESOURCE_MISC_GENERATE_MIPS };
		D3D11_TEXTURE2D_DESC distance{ a_width, a_height, 1, 1, DXGI_FORMAT_R16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
		D3D11_TEXTURE2D_DESC blocks{ blocksAcross, blocksDown, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0 };
		bool                 made = SUCCEEDED(a_device->CreateTexture2D(&target, nullptr, &pipeline.target)) && SUCCEEDED(a_device->CreateRenderTargetView(pipeline.target, nullptr, &pipeline.targetView)) &&
		                            SUCCEEDED(a_device->CreateShaderResourceView(pipeline.target, nullptr, &pipeline.targetMips)) && SUCCEEDED(a_device->CreateTexture2D(&distance, nullptr, &pipeline.distance)) &&
		                            SUCCEEDED(a_device->CreateRenderTargetView(pipeline.distance, nullptr, &pipeline.distanceView)) && SUCCEEDED(a_device->CreateTexture2D(&blocks, nullptr, &pipeline.blocks)) &&
		                            SUCCEEDED(a_device->CreateUnorderedAccessView(pipeline.blocks, nullptr, &pipeline.blocksView));
		for (UINT level = 0; made && level < levels; ++level) {
			D3D11_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			view.Texture2D = { level, 1 };
			ID3D11ShaderResourceView* levelView = nullptr;
			made = SUCCEEDED(a_device->CreateShaderResourceView(pipeline.target, &view, &levelView));
			pipeline.targetLevels.push_back(levelView);
		}
		if (!made) {
			logger::error("doorway snapshot: could not create a {}x{} target", a_width, a_height);
			ReleaseTarget();
			return false;
		}
		pipeline.targetWidth = a_width;
		pipeline.targetHeight = a_height;
		return true;
	}

	ID3D11Texture2D* Target()
	{
		return pipeline.target;
	}

	bool Compress(ID3D11DeviceContext* a_context, ID3D11Texture2D* a_colour, ID3D11Texture2D* a_distance, UINT a_slice)
	{
		D3D11_TEXTURE2D_DESC colour{};
		a_colour->GetDesc(&colour);
		namespace Z = Core::Pictures::Compress;
		const Render::D3D::SavedState saved(a_context);
		a_context->GenerateMips(pipeline.targetMips);
		a_context->CSSetShader(pipeline.compressCS, nullptr, 0);
		a_context->CSSetConstantBuffers(0, 1, &pipeline.compressConstants);
		ID3D11ShaderResourceView*  noTexture = nullptr;
		ID3D11UnorderedAccessView* noTarget = nullptr;
		for (UINT level = 0; level < colour.MipLevels; ++level) {
			const UINT width = Z::Level(pipeline.targetWidth, level), height = Z::Level(pipeline.targetHeight, level);
			const UINT blocksAcross = Z::Blocks(width), blocksDown = Z::Blocks(height);
			const UINT size[4] = { width, height, blocksAcross, blocksDown };
			Render::D3D::Upload(a_context, pipeline.compressConstants, size, sizeof(size));
			a_context->CSSetShaderResources(0, 1, &pipeline.targetLevels[level]);
			a_context->CSSetUnorderedAccessViews(0, 1, &pipeline.blocksView, nullptr);
			a_context->Dispatch((blocksAcross + 7) / 8, (blocksDown + 7) / 8, 1);
			a_context->CSSetUnorderedAccessViews(0, 1, &noTarget, nullptr);
			a_context->CSSetShaderResources(0, 1, &noTexture);
			// A block of the texture is a texel of the uint one: copied by blocks.
			const D3D11_BOX box{ 0, 0, 0, blocksAcross, blocksDown, 1 };
			a_context->CopySubresourceRegion(a_colour, D3D11CalcSubresource(level, a_slice, colour.MipLevels), 0, 0, 0, pipeline.blocks, 0, &box);
		}
		a_context->CopySubresourceRegion(a_distance, a_slice, 0, 0, 0, pipeline.distance, 0, nullptr);
		return true;
	}

	void Capture(ID3D11DeviceContext* a_context, const CaptureConstants& a_constants, ID3D11ShaderResourceView* a_scene, ID3D11ShaderResourceView* a_depth, UINT a_width, UINT a_height)
	{
		const Render::D3D::SavedState saved(a_context);
		Render::D3D::Upload(a_context, pipeline.captureConstants, &a_constants, sizeof(a_constants));
		// The scene may still be bound as a target.
		ID3D11RenderTargetView* noTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		a_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, noTargets, nullptr);
		ID3D11RenderTargetView* captureTargets[2] = { pipeline.targetView, pipeline.distanceView };
		a_context->OMSetRenderTargets(2, captureTargets, nullptr);
		a_context->OMSetDepthStencilState(pipeline.noDepth, 0);
		a_context->OMSetBlendState(pipeline.opaque, nullptr, 0xFFFFFFFF);
		a_context->RSSetState(pipeline.raster);
		const D3D11_VIEWPORT viewport{ 0, 0, static_cast<float>(a_width), static_cast<float>(a_height), 0, 1 };
		a_context->RSSetViewports(1, &viewport);
		a_context->VSSetShader(pipeline.captureVS, nullptr, 0);
		a_context->PSSetShader(pipeline.capturePS, nullptr, 0);
		a_context->GSSetShader(nullptr, nullptr, 0);
		a_context->HSSetShader(nullptr, nullptr, 0);
		a_context->DSSetShader(nullptr, nullptr, 0);
		a_context->IASetInputLayout(nullptr);
		a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ID3D11ShaderResourceView* sources[2]{ a_scene, a_depth };
		a_context->PSSetShaderResources(0, 2, sources);
		a_context->PSSetSamplers(0, 1, &pipeline.sampler);
		a_context->PSSetConstantBuffers(0, 1, &pipeline.captureConstants);
		a_context->Draw(3, 0);
		ID3D11ShaderResourceView* none[2]{};
		a_context->PSSetShaderResources(0, 2, none);
	}

	void Draw(ID3D11DeviceContext* a_context, const DrawConstants& a_constants, const float (&a_vertices)[4][5], const DrawTargets& a_targets, const DrawTextures& a_textures, bool a_opaque)
	{
		const Render::D3D::SavedState saved(a_context);
		Render::D3D::Upload(a_context, pipeline.vertices, a_vertices, sizeof(a_vertices));
		Render::D3D::Upload(a_context, pipeline.drawConstants, &a_constants, sizeof(a_constants));
		// Opaque, it stands in for the doorway like geometry (depth and motion); fading out, it only
		// lies over what the real space drew there.
		ID3D11RenderTargetView* noTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		a_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, noTargets, nullptr);
		ID3D11RenderTargetView* targets[2] = { a_targets.scene, a_opaque ? a_targets.motion : nullptr };
		a_context->OMSetRenderTargets(a_opaque && a_targets.motion ? 2 : 1, targets, a_targets.depth);
		a_context->OMSetDepthStencilState(a_opaque ? pipeline.depthWrite : pipeline.depthTest, 0);
		a_context->OMSetBlendState(pipeline.blend, nullptr, 0xFFFFFFFF);
		a_context->RSSetState(pipeline.raster);
		const D3D11_VIEWPORT viewport{ 0, 0, a_targets.width, a_targets.height, 0, 1 };
		a_context->RSSetViewports(1, &viewport);
		a_context->VSSetShader(pipeline.drawVS, nullptr, 0);
		a_context->PSSetShader(pipeline.drawPS, nullptr, 0);
		a_context->GSSetShader(nullptr, nullptr, 0);
		a_context->HSSetShader(nullptr, nullptr, 0);
		a_context->DSSetShader(nullptr, nullptr, 0);
		a_context->IASetInputLayout(pipeline.layout);
		a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		const UINT stride = sizeof(float) * 5, offset = 0;
		a_context->IASetVertexBuffers(0, 1, &pipeline.vertices, &stride, &offset);
		// t0 the picture's colour, t1 the depth image's, t2 and t3 their distances.
		ID3D11ShaderResourceView* sources[4]{ a_textures.picture, a_textures.layers, a_textures.pictureDistance, a_textures.layerDistance };
		a_context->PSSetShaderResources(0, 4, sources);
		a_context->PSSetSamplers(0, 1, &pipeline.sampler);
		a_context->PSSetConstantBuffers(0, 1, &pipeline.drawConstants);
		a_context->VSSetConstantBuffers(0, 1, &pipeline.drawConstants);
		a_context->Draw(4, 0);
	}
}
