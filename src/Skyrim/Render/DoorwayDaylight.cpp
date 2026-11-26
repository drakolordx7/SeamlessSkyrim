#include "Skyrim/Render/DoorwayDaylight.h"

#include "Skyrim/Render/D3D.h"
#include "Skyrim/Render/DaylightHlsl.h"
#include "Skyrim/Render/MainView.h"

namespace Render::DoorwayDaylight
{
	namespace
	{
		std::mutex           stateLock;
		std::optional<State> state;  // under stateLock

		// cbuffer Daylight (DaylightHlsl.h).
		struct Constants
		{
			float inverseViewProj[16];
			float viewProj[16];
			float viewSize[4];
			float center[4];
			float inward[4];
			float right[4];
			float sunToward[4];
			float sunTint[4];
			float skyTint[4];
		};
		static_assert(sizeof(Constants) == 60 * sizeof(float));

		struct Pipeline
		{
			ID3D11VertexShader*      vertexShader = nullptr;
			ID3D11PixelShader*       pixelShader = nullptr;
			ID3D11Buffer*            constants = nullptr;
			ID3D11DepthStencilState* depthState = nullptr;
			ID3D11RasterizerState*   rasterState = nullptr;
			ID3D11BlendState*        blendState = nullptr;
		} pipeline;

		void EnsurePipeline(ID3D11Device* a_device)
		{
			if (pipeline.vertexShader) {
				return;
			}
			namespace D = Core::Daylight;
			const std::string source = std::format(
										   "#define SEAMLESS_REACH {}\n#define SEAMLESS_DEPTH_FADE {}\n#define SEAMLESS_EDGE_BASE {}\n#define SEAMLESS_EDGE_GROWTH {}\n"
										   "#define SEAMLESS_GRAZING_LOW {}\n#define SEAMLESS_GRAZING_HIGH {}\n",
										   D::kReach, D::kDepthFade, D::kEdgeBase, D::kEdgeGrowth, D::kGrazingLow, D::kGrazingHigh) +
			                           DaylightHlsl::kSource;
			std::string errors;
			ID3DBlob*   vs = D3D::Compile(source, "SeamlessDoorwayDaylight", "VS", "vs_5_0", 0, &errors);
			ID3DBlob*   ps = D3D::Compile(source, "SeamlessDoorwayDaylight", "PS", "ps_5_0", 0, &errors);
			// Our own shader: one that can't be made is our bug, not a state to draw around.
			if (!vs || !ps) {
				logger::critical("portal: SeamlessDoorwayDaylight failed: {}", errors);
			}
			SEAMLESS_ASSERT(vs && ps);
			D3D11_BUFFER_DESC        buffer{ sizeof(Constants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
			D3D11_DEPTH_STENCIL_DESC depth{};  // no depth test or write: the pass reads the depth itself
			D3D11_RASTERIZER_DESC    raster{};
			raster.FillMode = D3D11_FILL_SOLID;
			raster.CullMode = D3D11_CULL_NONE;
			raster.DepthClipEnable = TRUE;
			// target = target * (1 + gain): the pixel shader's colour is the gain.
			D3D11_BLEND_DESC blend{};
			blend.RenderTarget[0] = { TRUE, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD,
				D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE };
			const bool made = SUCCEEDED(a_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &pipeline.vertexShader)) &&
			                  SUCCEEDED(a_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pipeline.pixelShader)) &&
			                  SUCCEEDED(a_device->CreateBuffer(&buffer, nullptr, &pipeline.constants)) && SUCCEEDED(a_device->CreateDepthStencilState(&depth, &pipeline.depthState)) &&
			                  SUCCEEDED(a_device->CreateRasterizerState(&raster, &pipeline.rasterState)) && SUCCEEDED(a_device->CreateBlendState(&blend, &pipeline.blendState));
			SEAMLESS_ASSERT(made);
			vs->Release();
			ps->Release();
		}

		// A colour at luminance 1 (its hue), or black for a colour with no light in it.
		void Tint(const Core::Render::Color& a_color, float* a_out)
		{
			const float luminance = Core::Daylight::Luminance(a_color.red, a_color.green, a_color.blue);
			const float scale = luminance > 1e-4f ? 1.0f / luminance : 0.0f;
			a_out[0] = a_color.red * scale;
			a_out[1] = a_color.green * scale;
			a_out[2] = a_color.blue * scale;
		}

		void Put(float* a_out, const Core::Vec3& a_vector, float a_w)
		{
			a_out[0] = a_vector.x;
			a_out[1] = a_vector.y;
			a_out[2] = a_vector.z;
			a_out[3] = a_w;
		}
	}

	void Set(const std::optional<State>& a_state)
	{
		std::scoped_lock lock(stateLock);
		state = a_state;
	}

	void Prewarm(ID3D11Device* a_device)
	{
		EnsurePipeline(a_device);
	}

	json Draw()
	{
		std::optional<State> current;
		{
			std::scoped_lock lock(stateLock);
			current = state;
		}
		if (!current) {
			return {};
		}
		namespace D = Core::Daylight;
		const auto& c = *current;
		const auto  gains = D::GainsOf(D::Luminance(c.sun.red, c.sun.green, c.sun.blue), D::Luminance(c.sky.red, c.sky.green, c.sky.blue), c.towardSun.z, c.open);
		if (gains.sun <= 0.0f && gains.spill <= 0.0f) {
			return {};
		}

		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto& sceneColor = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		auto& sceneDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto* sceneRTV = reinterpret_cast<ID3D11RenderTargetView*>(sceneColor.RTV);
		auto* depthSRV = reinterpret_cast<ID3D11ShaderResourceView*>(sceneDepth.depthSRV);
		SEAMLESS_ASSERT(sceneRTV && depthSRV);  // the engine's own targets, the ones the doorway quad is drawn into and tested against
		auto* context = D3D::Context();
		EnsurePipeline(D3D::Device());

		D3D11_TEXTURE2D_DESC sceneDesc{};
		reinterpret_cast<ID3D11Texture2D*>(sceneColor.texture)->GetDesc(&sceneDesc);
		const auto [scaleX, scaleY] = D3D::RenderScale();

		// The main view as the world was drawn with it (camera-relative, row vectors, jittered), as
		// the doorway quad uses it.
		auto&      mainView = CurrentMainView();
		const auto mainEye = mainView.posAdjust.getEye();
		const auto view = mainView.cameraData.getEye();
		double     matrix[16], inverse[16];
		std::copy_n(reinterpret_cast<const float*>(&view.viewProjMat), 16, matrix);
		const bool invertible = D3D::Invert(matrix, inverse);
		SEAMLESS_ASSERT(invertible);
		Constants constants{};
		for (int i = 0; i < 16; ++i) {
			constants.inverseViewProj[i] = static_cast<float>(inverse[i]);
		}
		std::memcpy(constants.viewProj, &view.viewProjMat, sizeof(constants.viewProj));
		constants.viewSize[0] = sceneDesc.Width * scaleX;
		constants.viewSize[1] = sceneDesc.Height * scaleY;
		const Core::Vec3 eye{ mainEye.x, mainEye.y, mainEye.z };
		Put(constants.center, c.opening.center - eye, c.opening.halfWidth);
		Put(constants.inward, c.opening.inward, c.opening.halfHeight);
		Put(constants.right, c.opening.right, 0.0f);
		Put(constants.sunToward, c.towardSun, gains.sun);
		Tint(c.sun, constants.sunTint);
		constants.sunTint[3] = gains.spill;
		Tint(c.sky, constants.skyTint);
		D3D::Upload(context, pipeline.constants, &constants, sizeof(constants));

		const D3D::SavedState saved(context);
		// Without the depth view bound: the pass reads the depth as a texture.
		context->OMSetRenderTargets(1, &sceneRTV, nullptr);
		const D3D11_VIEWPORT viewport{ 0, 0, constants.viewSize[0], constants.viewSize[1], 0, 1 };
		context->RSSetViewports(1, &viewport);
		context->OMSetDepthStencilState(pipeline.depthState, 0);
		context->OMSetBlendState(pipeline.blendState, nullptr, 0xFFFFFFFF);
		context->RSSetState(pipeline.rasterState);
		context->VSSetShader(pipeline.vertexShader, nullptr, 0);
		context->PSSetShader(pipeline.pixelShader, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->PSSetConstantBuffers(0, 1, &pipeline.constants);
		context->PSSetShaderResources(0, 1, &depthSRV);
		context->Draw(3, 0);
		// Not left bound while the depth view comes back (the saved state puts the old targets back).
		ID3D11ShaderResourceView* none = nullptr;
		context->PSSetShaderResources(0, 1, &none);
		return { { "sunGain", gains.sun }, { "spillGain", gains.spill } };
	}
}
