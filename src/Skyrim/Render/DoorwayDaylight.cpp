#include "Skyrim/Render/DoorwayDaylight.h"

#include "Core/Lighting/StreetShade.h"
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
			float shade[4];  // the sun's luminance, the sky's, the street's shadow strength
		};
		static_assert(sizeof(Constants) == 64 * sizeof(float));

		struct Pipeline
		{
			ID3D11VertexShader*      vertexShader = nullptr;
			ID3D11PixelShader*       pixelShader = nullptr;
			ID3D11PixelShader*       shadowShader = nullptr;  // PSShadow: the street's sun shadow
			ID3D11Buffer*            constants = nullptr;
			ID3D11DepthStencilState* depthState = nullptr;
			ID3D11RasterizerState*   rasterState = nullptr;
			ID3D11BlendState*        blendState = nullptr;
			ID3D11BlendState*        shadowBlend = nullptr;
		} pipeline;

		void EnsurePipeline(ID3D11Device* a_device)
		{
			if (pipeline.vertexShader) {
				return;
			}
			namespace D = Core::Daylight;
			namespace S = Core::StreetShade;
			const std::string source = std::format(
										   "#define SEAMLESS_REACH {}\n#define SEAMLESS_DEPTH_FADE {}\n#define SEAMLESS_EDGE_BASE {}\n#define SEAMLESS_EDGE_GROWTH {}\n"
										   "#define SEAMLESS_GRAZING_LOW {}\n#define SEAMLESS_GRAZING_HIGH {}\n"
										   "#define SEAMLESS_STREET_STEPS {}\n#define SEAMLESS_STREET_REACH {}\n#define SEAMLESS_PATCH_STEPS {}\n#define SEAMLESS_PATCH_REACH {}\n"
										   "#define SEAMLESS_OCCLUDER_MIN {}\n#define SEAMLESS_THICKNESS_BASE {}\n#define SEAMLESS_THICKNESS_PER_UNIT {}\n#define SEAMLESS_HIT {}\n"
										   "#define SEAMLESS_AMBIENT_FLOOR {}\n",
										   D::kReach, D::kDepthFade, D::kEdgeBase, D::kEdgeGrowth, D::kGrazingLow, D::kGrazingHigh, S::kSteps, S::kReach, S::kPatchSteps, S::kPatchReach, S::kOccluderMin,
										   S::kThicknessBase, S::kThicknessPerUnit, S::kHit, S::kAmbientFloor) +
			                           DaylightHlsl::kSource;
			std::string errors;
			ID3DBlob*   vs = D3D::Compile(source, "SeamlessDoorwayDaylight", "VS", "vs_5_0", 0, &errors);
			ID3DBlob*   ps = D3D::Compile(source, "SeamlessDoorwayDaylight", "PS", "ps_5_0", 0, &errors);
			ID3DBlob*   shadow = D3D::Compile(source, "SeamlessDoorwayDaylight", "PSShadow", "ps_5_0", 0, &errors);
			// Our own shader: one that can't be made is our bug, not a state to draw around.
			if (!vs || !ps || !shadow) {
				logger::critical("portal: SeamlessDoorwayDaylight failed: {}", errors);
			}
			SEAMLESS_ASSERT(vs && ps && shadow);
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
			// target = target * shader colour (the street's shade).
			D3D11_BLEND_DESC shadowBlend{};
			shadowBlend.RenderTarget[0] = { TRUE, D3D11_BLEND_ZERO, D3D11_BLEND_SRC_COLOR, D3D11_BLEND_OP_ADD, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD,
				D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE };
			const bool made = SUCCEEDED(a_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &pipeline.vertexShader)) &&
			                  SUCCEEDED(a_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pipeline.pixelShader)) &&
			                  SUCCEEDED(a_device->CreateBuffer(&buffer, nullptr, &pipeline.constants)) && SUCCEEDED(a_device->CreateDepthStencilState(&depth, &pipeline.depthState)) &&
			                  SUCCEEDED(a_device->CreateRasterizerState(&raster, &pipeline.rasterState)) && SUCCEEDED(a_device->CreateBlendState(&blend, &pipeline.blendState)) &&
			                  SUCCEEDED(a_device->CreatePixelShader(shadow->GetBufferPointer(), shadow->GetBufferSize(), nullptr, &pipeline.shadowShader)) &&
			                  SUCCEEDED(a_device->CreateBlendState(&shadowBlend, &pipeline.shadowBlend));
			SEAMLESS_ASSERT(made);
			vs->Release();
			ps->Release();
			shadow->Release();
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
		const float sunLuminance = D::Luminance(c.sun.red, c.sun.green, c.sun.blue);
		const float shadeStrength = c.streetMerged ? Core::StreetShade::Strength(c.towardSun.z, c.open) : 0.0f;
		const bool  patch = gains.sun > 0.0f || gains.spill > 0.0f;
		const auto  eyeAt = CurrentMainView().posAdjust.getEye();
		// The camera is on the interior's side of the doorway: a street merged in, while the player
		// isn't through it yet, is the engine's own and has its shadows.
		const bool shade = shadeStrength > 0.0f && c.opening.inward.Dot(Core::Vec3{ eyeAt.x, eyeAt.y, eyeAt.z } - c.opening.center) > 0.0f;
		if (!patch && !shade) {
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
		constants.shade[0] = sunLuminance;
		constants.shade[1] = D::Luminance(c.sky.red, c.sky.green, c.sky.blue);
		constants.shade[2] = shadeStrength;
		D3D::Upload(context, pipeline.constants, &constants, sizeof(constants));

		const D3D::SavedState saved(context);
		// Without the depth view bound: the pass reads the depth as a texture.
		context->OMSetRenderTargets(1, &sceneRTV, nullptr);
		const D3D11_VIEWPORT viewport{ 0, 0, constants.viewSize[0], constants.viewSize[1], 0, 1 };
		context->RSSetViewports(1, &viewport);
		context->OMSetDepthStencilState(pipeline.depthState, 0);
		context->RSSetState(pipeline.rasterState);
		context->VSSetShader(pipeline.vertexShader, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->PSSetConstantBuffers(0, 1, &pipeline.constants);
		context->PSSetShaderResources(0, 1, &depthSRV);
		if (patch) {
			context->OMSetBlendState(pipeline.blendState, nullptr, 0xFFFFFFFF);
			context->PSSetShader(pipeline.pixelShader, nullptr, 0);
			context->Draw(3, 0);
		}
		if (shade) {
			context->OMSetBlendState(pipeline.shadowBlend, nullptr, 0xFFFFFFFF);
			context->PSSetShader(pipeline.shadowShader, nullptr, 0);
			context->Draw(3, 0);
		}
		// Not left bound while the depth view comes back (the saved state puts the old targets back).
		ID3D11ShaderResourceView* none = nullptr;
		context->PSSetShaderResources(0, 1, &none);
		return { { "sunGain", patch ? gains.sun : 0.0f }, { "spillGain", patch ? gains.spill : 0.0f }, { "streetShade", shade ? shadeStrength : 0.0f } };
	}
}
