#include "Skyrim/Pictures/Composite.h"

#include "Skyrim/Engine/Fog.h"
#include "Skyrim/Pictures/Hlsl.h"
#include "Skyrim/Render/D3D.h"
#include "Skyrim/Render/MainView.h"
#include "Skyrim/Render/PortalPass.h"

namespace Pictures::Composite
{
	namespace
	{
		// cbuffer Motion.
		struct Constants
		{
			float              inverseViewProj[16];  // of the frame as drawn (jittered)
			float              viewProj[16];         // unjittered
			float              previousViewProj[16];  // unjittered, relative to the previous eye
			float              eyeDelta[4];           // this frame's eye - the previous frame's; w: portal near
			float              viewSize[4];           // the scene's viewport in pixels; z: see-through; w: portal far
			Hlsl::FogConstants fog;                   // the player's space's, as the engine's FogParam
		};

		static_assert(sizeof(Constants) == 68 * sizeof(float));

		struct Pipeline
		{
			ID3D11VertexShader*      vertexShader = nullptr;
			ID3D11PixelShader*       pixelShader = nullptr;
			ID3D11InputLayout*       layout = nullptr;
			ID3D11Buffer*            vertices = nullptr;
			ID3D11DepthStencilState* depthState = nullptr;
			ID3D11RasterizerState*   rasterState = nullptr;
			ID3D11BlendState*        blendState = nullptr;
			ID3D11Buffer*            motion = nullptr;  // constants for the doorway's motion vectors
		} quad;

		constexpr char kDeclarations[] = R"(
			Texture2D<float4> Portal : register(t0);
			Texture2D<float>  PortalDepth : register(t1);
			cbuffer Motion : register(b0)
			{
				row_major float4x4 InverseViewProj;      // of the frame as drawn (jittered)
				row_major float4x4 ViewProj;             // unjittered
				row_major float4x4 PreviousViewProj;     // unjittered, relative to the previous eye
				float4             EyeDelta;             // this frame's eye - the previous frame's; w: portal near
				float4             ViewSize;             // the scene's viewport in pixels; z: see-through; w: portal far
				float4             Fog;                  // the player's space's, as the engine's FogParam
				float4             FogNear;
				float4             FogFar;
			};
			struct Vertex { float4 position : SV_Position; float2 scale : TEXCOORD0; float depth : TEXCOORD1; };
			struct Pixel { float4 color : SV_Target0; float2 motion : SV_Target1; };
			Vertex VS(float4 position : POSITION, float2 scale : TEXCOORD0) { Vertex v; v.position = position; v.scale = scale; v.depth = position.w; return v; }
		)";

		constexpr char kCode[] = R"(
			// The scene is drawn into the top-left part of its target when a render scale below 1 is
			// in use (dynamic resolution, upscalers). The live picture is drawn the same way (scale
			// 1); one drawn into its whole target (a diagnostic file) is spread over the scene's part.
			Pixel PS(Vertex v)
			{
				Pixel p;
				int3 texel = int3(v.position.xy * v.scale, 0);
				p.color = float4(Fogged(Portal.Load(texel).rgb, v.depth), 1);
				// What is seen through the doorway moves on screen like static geometry at the
				// portal image's depth (the portal camera is the main camera mapped rigidly).
				float2 ndc = float2(v.position.x / ViewSize.x * 2 - 1, 1 - v.position.y / ViewSize.y * 2);
				// The distance along the view axis from the portal camera's own depth (its near plane
				// is at the door, not where the main view's is), then the point on this pixel's ray.
				float depth = PortalDepth.Load(texel);
				// A city gate: where the portal pass drew nothing (sky, or beyond the few cells it
				// has), the player's own worldspace shows through the doorway. Both worldspaces
				// share their coordinates, distant land and sky, which the portal pass has not.
				if (ViewSize.z > 0.5 && depth >= 1.0)
					discard;
				float viewDistance = PortalViewDistance(depth, EyeDelta.w, ViewSize.w);
				float4 world = mul(float4(ndc, 0.5, 1), InverseViewProj);
				world /= world.w;
				world.xyz *= viewDistance / mul(world, ViewProj).w;
				float4 now = mul(world, ViewProj);
				float4 before = mul(float4(world.xyz + EyeDelta.xyz, 1), PreviousViewProj);
				p.motion = float2(-0.5, 0.5) * (now.xy / now.w - before.xy / before.w);
				return p;
			}
		)";

		void EnsurePipeline(ID3D11Device* a_device)
		{
			if (quad.vertexShader) {
				return;
			}
			const std::string source = std::string(kDeclarations) + Hlsl::kFogged + Hlsl::kViewDistance + kCode;
			std::string       errors;
			ID3DBlob*         vs = Render::D3D::Compile(source, "SeamlessPortalQuad", "VS", "vs_5_0", 0, &errors);
			ID3DBlob*         ps = Render::D3D::Compile(source, "SeamlessPortalQuad", "PS", "ps_5_0", 0, &errors);
			// Our own shaders and states: one that can't be made is our bug, not a state to draw around.
			if (!vs || !ps) {
				logger::critical("portal: SeamlessPortalQuad failed: {}", errors);
			}
			SEAMLESS_ASSERT(vs && ps);
			const D3D11_INPUT_ELEMENT_DESC elements[]{
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};
			D3D11_BUFFER_DESC        buffer{ sizeof(float) * 6 * 4, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
			D3D11_BUFFER_DESC        motionBuffer{ sizeof(Constants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
			D3D11_DEPTH_STENCIL_DESC depth{};
			depth.DepthEnable = TRUE;
			depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
			D3D11_RASTERIZER_DESC raster{};
			raster.FillMode = D3D11_FILL_SOLID;
			raster.CullMode = D3D11_CULL_NONE;
			raster.DepthClipEnable = TRUE;
			D3D11_BLEND_DESC blend{};
			blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			const bool made = SUCCEEDED(a_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &quad.vertexShader)) &&
			                  SUCCEEDED(a_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &quad.pixelShader)) &&
			                  SUCCEEDED(a_device->CreateInputLayout(elements, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &quad.layout)) &&
			                  SUCCEEDED(a_device->CreateBuffer(&buffer, nullptr, &quad.vertices)) && SUCCEEDED(a_device->CreateDepthStencilState(&depth, &quad.depthState)) &&
			                  SUCCEEDED(a_device->CreateRasterizerState(&raster, &quad.rasterState)) && SUCCEEDED(a_device->CreateBlendState(&blend, &quad.blendState)) &&
			                  SUCCEEDED(a_device->CreateBuffer(&motionBuffer, nullptr, &quad.motion));
			SEAMLESS_ASSERT(made);
			vs->Release();
			ps->Release();
		}
	}

	void Prewarm(ID3D11Device* a_device)
	{
		EnsurePipeline(a_device);
	}

	json Draw(const Portal::Active& a_portal)
	{
		using Render::PortalPass::ShownTargets;
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto& rendererData = renderer->GetRuntimeData();
		auto* context = Render::D3D::Context();
		auto& sceneColor = rendererData.renderTargets[RE::RENDER_TARGETS::kMAIN];
		auto& sceneDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto& portal = ShownTargets().color;
		auto* sceneRTV = reinterpret_cast<ID3D11RenderTargetView*>(sceneColor.RTV);
		auto* sceneDSV = reinterpret_cast<ID3D11DepthStencilView*>(sceneDepth.views[0]);
		auto* portalSRV = reinterpret_cast<ID3D11ShaderResourceView*>(portal.SRV);
		auto* portalDepthSRV = reinterpret_cast<ID3D11ShaderResourceView*>(ShownTargets().depth.depthSRV);
		SEAMLESS_ASSERT(portalSRV);  // our own target, made like the main scene's (PortalPass::EnsureOwnTargets)
		if (!sceneRTV || !sceneDSV) {
			throw std::runtime_error("scene render target missing");
		}
		EnsurePipeline(Render::D3D::Device());

		D3D11_TEXTURE2D_DESC sceneDesc{}, portalDesc{};
		reinterpret_cast<ID3D11Texture2D*>(sceneColor.texture)->GetDesc(&sceneDesc);
		reinterpret_cast<ID3D11Texture2D*>(portal.texture)->GetDesc(&portalDesc);
		if (sceneDesc.Width != portalDesc.Width || sceneDesc.Height != portalDesc.Height) {
			static bool reported = false;
			if (!std::exchange(reported, true)) {
				logger::warn("portal: scene target is {}x{} but the portal image is {}x{}; the portal will be misaligned", sceneDesc.Width, sceneDesc.Height, portalDesc.Width,
					portalDesc.Height);
			}
		}

		// Clip-space corners with the main view's own matrix (camera-relative, row vectors), so the
		// quad sits exactly where the engine drew the door frame, jitter included.
		auto& mainView = Render::CurrentMainView();
		const auto  mainEye = mainView.posAdjust.getEye();
		const auto  eye = RE::NiPoint3{ mainEye.x, mainEye.y, mainEye.z };
		const auto  view = mainView.cameraData.getEye();
		const auto* m = reinterpret_cast<const float*>(&view.viewProjMat);
		const auto  corners = Render::PortalPass::DoorwayQuad(a_portal, RE::Main::WorldRootCamera()->world.translate);
		const auto [scaleX, scaleY] = Render::D3D::RenderScale();
		float       clip[4][6];
		for (int i = 0; i < 4; ++i) {
			std::ranges::copy(Render::D3D::ClipOf(m, eye, corners[i]), clip[i]);
			clip[i][4] = ShownTargets().pictureWidth / (sceneDesc.Width * scaleX);
			clip[i][5] = ShownTargets().pictureHeight / (sceneDesc.Height * scaleY);
		}

		const Render::D3D::SavedState saved(context);

		// The portal image may still be bound as a render target from the portal pass.
		ID3D11RenderTargetView* noTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, noTargets, nullptr);
		const D3D11_VIEWPORT viewport{ 0, 0, sceneDesc.Width * scaleX, sceneDesc.Height * scaleY, 0, 1 };
		const UINT           stride = sizeof(float) * 6, offset = 0;
		// Motion vectors for the doorway, when the portal's depth can be read (the engine's depth
		// copy, which ours is made like, has a view for that).
		auto*      motionRTV = reinterpret_cast<ID3D11RenderTargetView*>(rendererData.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].RTV);
		const bool withMotion = motionRTV && portalDepthSRV;
		{
			Constants constants{};
			double    current[16], inverse[16];
			std::copy_n(reinterpret_cast<const float*>(&view.viewProjMat), 16, current);
			if (withMotion && Render::D3D::Invert(current, inverse)) {
				for (int i = 0; i < 16; ++i) {
					constants.inverseViewProj[i] = static_cast<float>(inverse[i]);
				}
				std::memcpy(constants.viewProj, &view.viewProjMatrixUnjittered, sizeof(constants.viewProj));
				std::memcpy(constants.previousViewProj, &view.previousViewProjMatrixUnjittered, sizeof(constants.previousViewProj));
				const auto previousEye = mainView.previousPosAdjust.getEye();
				constants.eyeDelta[0] = eye.x - previousEye.x;
				constants.eyeDelta[1] = eye.y - previousEye.y;
				constants.eyeDelta[2] = eye.z - previousEye.z;
			}
			constants.eyeDelta[3] = Render::PortalPass::NearClip();
			constants.viewSize[3] = Render::PortalPass::FarClip();
			auto* here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
			constants.viewSize[2] = portalDepthSRV && here && !here->IsInteriorCell() && a_portal.cell && !a_portal.cell->IsInteriorCell() ? 1.0f : 0.0f;
			constants.viewSize[0] = sceneDesc.Width * scaleX;
			constants.viewSize[1] = sceneDesc.Height * scaleY;
			constants.fog = Hlsl::FogConstants::From(Engine::Fog::World());
			Render::D3D::Upload(context, quad.motion, &constants, sizeof(constants));
		}
		ID3D11RenderTargetView* quadTargets[2] = { sceneRTV, withMotion ? motionRTV : nullptr };
		context->OMSetRenderTargets(withMotion ? 2 : 1, quadTargets, sceneDSV);
		context->PSSetConstantBuffers(0, 1, &quad.motion);
		context->PSSetShaderResources(1, 1, &portalDepthSRV);
		context->OMSetDepthStencilState(quad.depthState, 0);
		context->OMSetBlendState(quad.blendState, nullptr, 0xFFFFFFFF);
		context->RSSetState(quad.rasterState);
		context->RSSetViewports(1, &viewport);
		context->VSSetShader(quad.vertexShader, nullptr, 0);
		context->PSSetShader(quad.pixelShader, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->IASetInputLayout(quad.layout);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		context->IASetVertexBuffers(0, 1, &quad.vertices, &stride, &offset);
		{
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(quad.vertices, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				std::memcpy(mapped.pData, clip, sizeof(clip));
				context->Unmap(quad.vertices, 0);
				context->PSSetShaderResources(0, 1, &portalSRV);
				context->Draw(4, 0);
			}
		}
		return { { "composited", true }, { "renderScale", { scaleX, scaleY } }, { "sceneSize", { sceneDesc.Width, sceneDesc.Height } }, { "sceneFormat", static_cast<int>(sceneDesc.Format) },
			{ "portalFormat", static_cast<int>(portalDesc.Format) } };
	}
}
