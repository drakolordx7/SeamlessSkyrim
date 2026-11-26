#include "Skyrim/Render/FlashGuard.h"

#include "Skyrim/Render/D3D.h"

#include <wrl/client.h>

namespace Render::FlashGuard
{
	float RowAverage(const D3D11_MAPPED_SUBRESOURCE& a_map, UINT a_width, DXGI_FORMAT a_format)
	{
		double sum = 0.0;
		for (UINT row = 0; row < std::size(kRows); ++row) {
			const auto* pixels = reinterpret_cast<const std::uint32_t*>(static_cast<const std::uint8_t*>(a_map.pData) + row * a_map.RowPitch);
			for (UINT x = 0; x < a_width; ++x) {
				const std::uint32_t p = pixels[x];
				if (a_format == DXGI_FORMAT_R10G10B10A2_UNORM) {
					sum += ((p & 0x3FF) + ((p >> 10) & 0x3FF) + ((p >> 20) & 0x3FF)) / (3.0 * 1023.0);
				} else {
					sum += ((p & 0xFF) + ((p >> 8) & 0xFF) + ((p >> 16) & 0xFF)) / (3.0 * 255.0);
				}
			}
		}
		return static_cast<float>(sum / (a_width * std::size(kRows)));
	}

	namespace
	{
		// The first walk outdoors in a session drew one frame almost entirely white, two or three frames
		// after the crossing (the engine's own: also with this plugin's image-space blend off; behind a
		// loading screen it is never seen). For a short while after each crossing a frame far brighter
		// than the one shown before it is replaced by that one before it is presented. Not by skipping
		// the Present: with Presents skipped the white lasted as many frames as were skipped (something
		// in the engine or a mod only moves on with each Present).
		//
		// All on the GPU. Reading the frame's brightness back to decide on the CPU made the main thread
		// wait for the GPU to finish the frame, every frame of the guard: on a GPU-bound game that
		// emptied the render queue (60-150 ms a frame, a second or two after every crossing). Now the
		// frame is copied, one thread group averages a few rows of the copy and decides (the state it
		// keeps between frames lives in a buffer), and a fullscreen triangle writes either the copy or
		// the last frame shown into the back buffer. The CPU only reads the buffer a few frames later,
		// for the log.
		constexpr float kFlashJump = 0.3f;  // of the average brightness, 0..1
		// Only a frame washed out to near white: a real step in brightness (the street after a dark room
		// on the first walk outdoors, 0.23 to 0.53) was held for six frames and then shown anyway.
		constexpr float kFlashBrightness = 0.8f;
		constexpr int   kMaxHeldInRow = 6;  // a scene really that bright is shown after this many

		constexpr char kHlsl[] = R"(
cbuffer Guard : register(b0)
{
	uint4  dims;    // width, the three rows
	float4 limits;  // jump, brightness, most held in a row, reset
};
Texture2D<float4>          cur     : register(t0);
Texture2D<float4>          last    : register(t1);
StructuredBuffer<float4>   stateIn : register(t2);
RWStructuredBuffer<float4> state   : register(u0);  // [0] = shown, held, flag, value; [1] = replaced in all, frames, 0, 0
groupshared float sums[256];

[numthreads(256, 1, 1)]
void CS(uint t : SV_GroupIndex)
{
	float s = 0;
	[unroll] for (uint r = 0; r < 3; ++r) {
		for (uint x = t; x < dims.x; x += 256) {
			float3 c = cur.Load(int3(x, dims[r + 1], 0)).rgb;
			s += (c.r + c.g + c.b) / 3.0;
		}
	}
	sums[t] = s;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint o = 128; o > 0; o >>= 1) {
		if (t < o) sums[t] += sums[t + o];
		GroupMemoryBarrierWithGroupSync();
	}
	if (t == 0) {
		float  v = sums[0] / (dims.x * 3.0);
		float4 st = state[0];
		float4 tally = state[1];
		if (limits.w > 0.5) { st = float4(-1, 0, 0, 0); tally = float4(0, 0, 0, 0); }
		bool flash = st.x >= 0 && v - st.x > limits.x && v > limits.y && st.y < limits.z;
		state[0] = float4(flash ? st.x : v, flash ? st.y + 1 : 0, flash ? 1 : 0, v);
		state[1] = float4(tally.x + (flash ? 1 : 0), tally.y + 1, 0, 0);
	}
}

float4 VS(uint id : SV_VertexID) : SV_Position
{
	float2 p = float2((id << 1) & 2, id & 2);
	return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PS(float4 pos : SV_Position) : SV_Target
{
	int3 p = int3(pos.xy, 0);
	return stateIn[0].z > 0.5 ? last.Load(p) : cur.Load(p);
}
)";

		struct Constants
		{
			std::uint32_t dims[4];
			float         limits[4];
		};

		struct Pipeline
		{
			ID3D11ComputeShader*       cs = nullptr;
			ID3D11VertexShader*        vs = nullptr;
			ID3D11PixelShader*         ps = nullptr;
			ID3D11Buffer*              constants = nullptr;
			ID3D11Buffer*              state = nullptr;
			ID3D11ShaderResourceView*  stateSrv = nullptr;
			ID3D11UnorderedAccessView* stateUav = nullptr;
			ID3D11Buffer*              staging[3]{};
			ID3D11DepthStencilState*   depth = nullptr;
			ID3D11RasterizerState*     raster = nullptr;
			ID3D11BlendState*          blend = nullptr;
		} pipeline;

		// What depends on the back buffer: its size and format.
		struct Targets
		{
			D3D11_TEXTURE2D_DESC      desc{};
			ID3D11Texture2D*          cur = nullptr;
			ID3D11ShaderResourceView* curSrv = nullptr;
			ID3D11Texture2D*          last = nullptr;
			ID3D11ShaderResourceView* lastSrv = nullptr;
			ID3D11Texture2D*          backBuffer = nullptr;  // the one the view below is of (not owned)
			ID3D11RenderTargetView*   rtv = nullptr;
		} targets;

		std::atomic<int>           window = 60;  // the most frames a Guard watches (SetWindow)
		std::atomic<int>           guardFrames = 0;
		std::atomic<bool>          guardReset = false;
		std::atomic<float>         jumpLimit = kFlashJump;
		std::atomic<float>         brightnessLimit = kFlashBrightness;  // SetLimits
		std::uint32_t              frameNumber = 0;
		std::uint32_t              replacedSeen = 0;  // of the GPU's tally, as last read
		std::atomic<std::uint32_t> replacedTotal = 0;

		template <class T>
		void Drop(T*& a_object)
		{
			if (a_object) {
				a_object->Release();
				a_object = nullptr;
			}
		}

		void EnsurePipeline(ID3D11Device* a_device)
		{
			if (pipeline.cs) {
				return;
			}
			std::string errors;
			auto*       cs = Render::D3D::Compile(kHlsl, "SeamlessFlashGuard", "CS", "cs_5_0", 0, &errors);
			auto*       vs = Render::D3D::Compile(kHlsl, "SeamlessFlashGuard", "VS", "vs_5_0", 0, &errors);
			auto*       ps = Render::D3D::Compile(kHlsl, "SeamlessFlashGuard", "PS", "ps_5_0", 0, &errors);
			if (!cs || !vs || !ps) {
				logger::critical("flash guard: shader failed: {}", errors);
			}
			SEAMLESS_ASSERT(cs && vs && ps);
			D3D11_BUFFER_DESC constants{ sizeof(Constants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
			D3D11_BUFFER_DESC state{ 2 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16 };
			D3D11_BUFFER_DESC staging{ 2 * 16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16 };
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_UNKNOWN;
			srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srv.Buffer.NumElements = 2;
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = DXGI_FORMAT_UNKNOWN;
			uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uav.Buffer.NumElements = 2;
			D3D11_DEPTH_STENCIL_DESC depth{};
			D3D11_RASTERIZER_DESC    raster{};
			raster.FillMode = D3D11_FILL_SOLID;
			raster.CullMode = D3D11_CULL_NONE;
			raster.DepthClipEnable = TRUE;
			D3D11_BLEND_DESC blend{};
			blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			bool made = SUCCEEDED(a_device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &pipeline.cs)) &&
			            SUCCEEDED(a_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &pipeline.vs)) &&
			            SUCCEEDED(a_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pipeline.ps)) &&
			            SUCCEEDED(a_device->CreateBuffer(&constants, nullptr, &pipeline.constants)) && SUCCEEDED(a_device->CreateBuffer(&state, nullptr, &pipeline.state)) &&
			            SUCCEEDED(a_device->CreateShaderResourceView(pipeline.state, &srv, &pipeline.stateSrv)) &&
			            SUCCEEDED(a_device->CreateUnorderedAccessView(pipeline.state, &uav, &pipeline.stateUav)) && SUCCEEDED(a_device->CreateDepthStencilState(&depth, &pipeline.depth)) &&
			            SUCCEEDED(a_device->CreateRasterizerState(&raster, &pipeline.raster)) && SUCCEEDED(a_device->CreateBlendState(&blend, &pipeline.blend));
			for (auto*& buffer : pipeline.staging) {
				made = made && SUCCEEDED(a_device->CreateBuffer(&staging, nullptr, &buffer));
			}
			SEAMLESS_ASSERT(made);
			cs->Release();
			vs->Release();
			ps->Release();
		}

		void EnsureTargets(ID3D11Device* a_device, ID3D11Texture2D* a_backBuffer, const D3D11_TEXTURE2D_DESC& a_desc)
		{
			if (targets.cur && targets.desc.Width == a_desc.Width && targets.desc.Height == a_desc.Height && targets.desc.Format == a_desc.Format && targets.backBuffer == a_backBuffer) {
				return;
			}
			Drop(targets.curSrv);
			Drop(targets.cur);
			Drop(targets.lastSrv);
			Drop(targets.last);
			Drop(targets.rtv);
			D3D11_TEXTURE2D_DESC copy{};
			copy.Width = a_desc.Width;
			copy.Height = a_desc.Height;
			copy.MipLevels = 1;
			copy.ArraySize = 1;
			copy.Format = a_desc.Format;
			copy.SampleDesc.Count = 1;
			copy.Usage = D3D11_USAGE_DEFAULT;
			copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			const bool made = SUCCEEDED(a_device->CreateTexture2D(&copy, nullptr, &targets.cur)) && SUCCEEDED(a_device->CreateShaderResourceView(targets.cur, nullptr, &targets.curSrv)) &&
			                  SUCCEEDED(a_device->CreateTexture2D(&copy, nullptr, &targets.last)) && SUCCEEDED(a_device->CreateShaderResourceView(targets.last, nullptr, &targets.lastSrv)) &&
			                  SUCCEEDED(a_device->CreateRenderTargetView(a_backBuffer, nullptr, &targets.rtv));
			SEAMLESS_ASSERT(made);
			targets.desc = a_desc;
			targets.backBuffer = a_backBuffer;
			guardReset = true;
		}

		// The GPU's tally of replaced frames, read from the copy made a few frames ago; never waits.
		void ReadTally(ID3D11DeviceContext* a_context)
		{
			auto*                    staging = pipeline.staging[(frameNumber + 1) % 3];
			D3D11_MAPPED_SUBRESOURCE map{};
			if (a_context->Map(staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map) != S_OK) {
				return;
			}
			const auto* values = static_cast<const float*>(map.pData);
			const auto  replaced = static_cast<std::uint32_t>(values[4]);
			const float value = values[3];
			a_context->Unmap(staging, 0);
			if (replaced != replacedSeen) {
				logger::info("flash guard: {} frame(s) replaced by the one before (the latest of brightness {:.2f})", replaced - replacedSeen, value);
				replacedSeen = replaced;
				replacedTotal = replaced;
			}
		}

		void Watch(IDXGISwapChain* a_swapChain)
		{
			Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
			if (FAILED(a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.GetAddressOf())))) {
				return;
			}
			D3D11_TEXTURE2D_DESC desc{};
			backBuffer->GetDesc(&desc);
			const bool known = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
			if (!known || desc.SampleDesc.Count != 1) {
				return;
			}
			Microsoft::WRL::ComPtr<ID3D11Device>        device;
			Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
			backBuffer->GetDevice(device.GetAddressOf());
			device->GetImmediateContext(context.GetAddressOf());
			EnsurePipeline(device.Get());
			EnsureTargets(device.Get(), backBuffer.Get(), desc);

			Constants constants{};
			constants.dims[0] = desc.Width;
			for (UINT row = 0; row < std::size(kRows); ++row) {
				constants.dims[1 + row] = desc.Height / kRows[row];
			}
			constants.limits[0] = jumpLimit;
			constants.limits[1] = brightnessLimit;
			constants.limits[2] = static_cast<float>(kMaxHeldInRow);
			constants.limits[3] = guardReset.exchange(false) ? 1.0f : 0.0f;
			Render::D3D::Upload(context.Get(), pipeline.constants, &constants, sizeof(constants));

			const Render::D3D::SavedState saved(context.Get());
			context->CopyResource(targets.cur, backBuffer.Get());
			// The frame's brightness, and whether it is a flash.
			ID3D11ShaderResourceView*  noViews[3]{};
			ID3D11UnorderedAccessView* uav = pipeline.stateUav;
			context->CSSetShader(pipeline.cs, nullptr, 0);
			context->CSSetConstantBuffers(0, 1, &pipeline.constants);
			context->CSSetShaderResources(0, 1, &targets.curSrv);
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->Dispatch(1, 1, 1);
			ID3D11UnorderedAccessView* noUav = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
			context->CSSetShaderResources(0, 1, noViews);
			// The frame, or the one shown before it.
			ID3D11RenderTargetView*   rtv = targets.rtv;
			ID3D11ShaderResourceView* views[3] = { targets.curSrv, targets.lastSrv, pipeline.stateSrv };
			context->OMSetRenderTargets(1, &rtv, nullptr);
			const D3D11_VIEWPORT viewport{ 0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1 };
			context->RSSetViewports(1, &viewport);
			context->OMSetDepthStencilState(pipeline.depth, 0);
			context->OMSetBlendState(pipeline.blend, nullptr, 0xFFFFFFFF);
			context->RSSetState(pipeline.raster);
			context->IASetInputLayout(nullptr);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->VSSetShader(pipeline.vs, nullptr, 0);
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			context->GSSetShader(nullptr, nullptr, 0);
			context->PSSetShader(pipeline.ps, nullptr, 0);
			context->PSSetShaderResources(0, 3, views);
			context->Draw(3, 0);
			context->PSSetShaderResources(0, 3, noViews);
			// What is shown is what the next frame is compared with.
			context->CopyResource(targets.last, backBuffer.Get());
			++frameNumber;
			context->CopyResource(pipeline.staging[frameNumber % 3], pipeline.state);
			ReadTally(context.Get());
		}
	}

	void Guard(int a_frames)
	{
		guardReset = true;
		guardFrames = std::min(a_frames, window.load());
	}

	void SetWindow(int a_frames)
	{
		window = a_frames;
	}

	void SetLimits(float a_jump, float a_brightness)
	{
		jumpLimit = a_jump;
		brightnessLimit = a_brightness;
	}

	json Status()
	{
		return { { "window", window.load() }, { "framesLeft", guardFrames.load() }, { "replacedInAll", replacedTotal.load() }, { "jump", jumpLimit.load() }, { "brightness", brightnessLimit.load() } };
	}

	bool Apply(IDXGISwapChain* a_swapChain)
	{
		if (guardFrames > 0) {
			--guardFrames;
			Watch(a_swapChain);
		}
		return false;
	}
}
