#include "Skyrim/Pictures/Gpu.h"

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
			ID3D11Texture2D*         target = nullptr;  // at the size pictures are taken at now (EnsureTarget)
			ID3D11RenderTargetView*  targetView = nullptr;
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

		// Capture: each texel of the picture is the doorway's rectangle at that place, as the frame
		// shows it, averaged over 4x4 points (at the default size the doorway covers
		// dozens of pixels per texel). Alpha: how far behind the doorway the surface seen there is.
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
				// camera's distance to the doorway (looking along its normal), 0 = the player's view
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
					if (Portal.w > 0)
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
			float4 PS(float4 position : SV_Position) : SV_Target
			{
				float3 sum = 0;
				float3 nearSum = 0;
				float  nearness = 0;
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
						nearness += 1 / behind;
						++count;
					}
				}
				if (count == 0)
					return float4(max(sum / 16, 0), kFar);
				// The sky left out where anything else is: through a gate the real sky is shown where
				// the picture has none (Draw), and a roof's edge averaged with the portal pass's flat
				// sky colour stood in it as a blue fringe. Averaged as nearness: a lamp in front of a
				// wall far behind it isn't pulled away to the wall.
				return float4(max(nearSum / count, 0), count / nearness);
			}
)";

		// Draw: the picture blurred (or the plain colour) in the doorway, as the space behind it in
		// relief. The doorway's own depth is kept (written, so what is drawn later is hidden behind
		// it as by a wall there); the motion is the surface's seen through it.
		constexpr char kDrawDeclarations[] = R"(
				Texture2D<float4> Picture : register(t0);
				SamplerState      Linear : register(s0);
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
					float4             Look;              // x: Adaptation::Kind; y: daylight; z: 1 = the outdoors behind; w: the picture's light now against when taken
					float4             Extent;            // the part of the door's plane the picture covers, as Extent
					float4             Gate;              // x: 1 = a city gate (the same sky on both sides)
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
			// Cubic B-spline filtering from four bilinear reads (GPU Gems 2, ch. 20). Bilinear alone,
			// magnified ten times and more, showed every texel as a patch with creases at its edges;
			// the B-spline is smooth across them.
			// At mip a_level (fractional: the B-spline of the two around it, blended).
			float3 SampleSmoothAt(float2 uv, float a_level)
			{
				float2 texel = Params.yz * exp2(a_level);
				float2 p = uv / texel - 0.5;
				float2 i = floor(p);
				float2 f = p - i;
				float2 w0 = (1 - f) * (1 - f) * (1 - f) / 6;
				float2 w1 = (4 - 6 * f * f + 3 * f * f * f) / 6;
				float2 w3 = f * f * f / 6;
				float2 w2 = 1 - w0 - w1 - w3;
				float2 g0 = w0 + w1;
				float2 g1 = w2 + w3;
				float2 t0 = (i + 0.5 - 1 + w1 / g0) * texel;
				float2 t1 = (i + 0.5 + 1 + w3 / g1) * texel;
				return g0.y * (g0.x * Picture.SampleLevel(Linear, t0, a_level).rgb + g1.x * Picture.SampleLevel(Linear, float2(t1.x, t0.y), a_level).rgb) +
				       g1.y * (g0.x * Picture.SampleLevel(Linear, float2(t0.x, t1.y), a_level).rgb + g1.x * Picture.SampleLevel(Linear, t1, a_level).rgb);
			}
			float3 SampleSmooth(float2 uv, float a_level)
			{
				float below = floor(a_level);
				return lerp(SampleSmoothAt(uv, below), SampleSmoothAt(uv, below + 1), a_level - below);
			}
			float2 PictureUV(float3 p)
			{
				float3 d = p - Origin.xyz;
				float2 inDoorway = float2(dot(d, Across.xyz) * Across.w, dot(d, Up.xyz) * Up.w);
				return float2((inDoorway.x - Extent.x) / (Extent.y - Extent.x), 1 - (inDoorway.y - Extent.z) / (Extent.w - Extent.z));
			}
			float StoredBehind(float2 uv)
			{
				if (Plain.w < 0.5)
					return TakenFrom.w;
				// A picture without depth: everything at the doorway, drawn flat as before.
				if (Origin.w < 0.5)
					return 0;
				// Through a gate a roof or an arch stands against the sky: read between them, the
				// depth made a slope to the sky that rays landed on, in dashes along every edge.
				if (Gate.x > 0.5)
					return Picture.Load(int3(clamp(uv / Params.yz, 0, 1 / Params.yz - 1), 0)).a;
				return Picture.SampleLevel(Linear, uv, 0).a;
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
					// How stretched the picture is here: across the screen the texels it reads run
					// along one line and stand still along the other where the ray leaves the
					// picture at its side, or slides off a near thing's edge onto the room behind.
					// Sharp, a row of texels drawn out so read as streaks; blurred by how far, from
					// a smaller mip, as a smear. Seen straight or at a slant (stretched twice or so)
					// it stays sharp.
					float2 alongX = ddx(uv) / Params.yz;
					float2 alongY = ddy(uv) / Params.yz;
					float  major = max(length(alongX), length(alongY));
					float  minor = abs(alongX.x * alongY.y - alongX.y * alongY.x) / max(major, 1e-6);
					float  level = major > 1e-6 ? clamp(log2(major / max(minor, major * 1e-3)) - 1.5, 0, 4) : 0;
					float2 step = Params.yz * exp2(level);
					// A little more blur on top: what changed since the picture was taken stays vague.
					float3 sum = 0;
					[unroll] for (int x = -1; x <= 1; ++x)
					[unroll] for (int y = -1; y <= 1; ++y) {
						sum += (2 - abs(x)) * (2 - abs(y)) * SampleSmooth(uv + float2(x, y) * step, level);
					}
					color = sum / 16;
				} else if (Look.z > 0.5) {
					// The outdoors: lighter up towards the sky.
					color = Plain.rgb * lerp(1.2, 0.85, saturate(uv.y));
				} else {
					// A room: its floor lit from the doorway, its ceiling dark.
					color = Plain.rgb * lerp(0.6, 1.2, saturate(uv.y));
				}

				// A picture as bright as the real space is now. Only the plain colour is made as an eye
				// used to the player's space sees the other, eased out as it fades over the real space
				// (which is drawn without it): the doorway brightens or dims into it like an eye
				// adjusting. A picture got that too, and came out brighter or darker than the real
				// space, its light already in it (taken in the same high dynamic range as the wall
				// around it).
				color *= Look.w;
				float  adapt = Plain.w > 0.5 ? 0 : Look.y * Params.x;
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
		const std::string drawSource = std::string(kDrawDeclarations) + Hlsl::kFogged + kDrawCode;
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
			D3D11_BUFFER_DESC    drawConstants{ sizeof(DrawConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
			D3D11_SAMPLER_DESC   sampler{};
			sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
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
		Render::D3D::Release(pipeline.targetView);
		Render::D3D::Release(pipeline.target);
		D3D11_TEXTURE2D_DESC target{ a_width, a_height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
		if (FAILED(a_device->CreateTexture2D(&target, nullptr, &pipeline.target)) || FAILED(a_device->CreateRenderTargetView(pipeline.target, nullptr, &pipeline.targetView))) {
			logger::error("doorway snapshot: could not create a {}x{} target", a_width, a_height);
			Render::D3D::Release(pipeline.target);
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

	void Capture(ID3D11DeviceContext* a_context, const CaptureConstants& a_constants, ID3D11ShaderResourceView* a_scene, ID3D11ShaderResourceView* a_depth, UINT a_width, UINT a_height)
	{
		const Render::D3D::SavedState saved(a_context);
		Render::D3D::Upload(a_context, pipeline.captureConstants, &a_constants, sizeof(a_constants));
		// The scene may still be bound as a target.
		ID3D11RenderTargetView* noTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		a_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, noTargets, nullptr);
		a_context->OMSetRenderTargets(1, &pipeline.targetView, nullptr);
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

	void Draw(ID3D11DeviceContext* a_context, const DrawConstants& a_constants, const float (&a_vertices)[4][5], const DrawTargets& a_targets, ID3D11ShaderResourceView* a_picture, bool a_opaque)
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
		a_context->PSSetShaderResources(0, 1, &a_picture);
		a_context->PSSetSamplers(0, 1, &pipeline.sampler);
		a_context->PSSetConstantBuffers(0, 1, &pipeline.drawConstants);
		a_context->VSSetConstantBuffers(0, 1, &pipeline.drawConstants);
		a_context->Draw(4, 0);
	}
}
