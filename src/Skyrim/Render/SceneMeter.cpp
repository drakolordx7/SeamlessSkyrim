#include "Skyrim/Render/SceneMeter.h"

#include "Skyrim/Render/D3D.h"

namespace Render::SceneMeter
{
	namespace
	{
		// One group of 16x16 threads, each taking 4x4 points of a 64x64 grid over the viewport; read
		// back through a ring of staging copies so the GPU is never waited for.
		struct Meter
		{
			ID3D11ComputeShader*                  shader = nullptr;
			ID3D11Buffer*                         constants = nullptr;
			ID3D11Texture2D*                      result = nullptr;
			ID3D11UnorderedAccessView*            resultView = nullptr;
			std::array<ID3D11Texture2D*, 4>       staging{};
			std::array<bool, 4>                   pending{};
			std::size_t                           next = 0;
			bool                                  failed = false;
			float                                 adapted = 0.0f;  // 0: no reading yet
			std::chrono::steady_clock::time_point read{};
		} meter;

		// An eye takes about this long to adapt to a brighter or darker view (the low end of real
		// eyes' light adaptation; a doorway shouldn't brighten and dim as the head turns).
		constexpr float kAdaptSeconds = 1.5f;

		bool EnsureMeter(ID3D11Device* a_device)
		{
			if (meter.shader || meter.failed) {
				return !meter.failed;
			}
			static const char source[] = R"(
				Texture2D<float4>   Scene : register(t0);
				RWTexture2D<float>  Out : register(u0);
				cbuffer Meter : register(b0) { float4 Size; };  // xy: the viewport in pixels
				groupshared float sums[256];
				[numthreads(16, 16, 1)]
				void CS(uint3 thread : SV_GroupThreadID, uint index : SV_GroupIndex)
				{
					float sum = 0;
					for (uint y = 0; y < 4; ++y) {
						for (uint x = 0; x < 4; ++x) {
							float2 at = (float2(thread.xy * 4 + uint2(x, y)) + 0.5) / 64;
							float3 color = Scene.Load(int3(at * Size.xy, 0)).rgb;
							sum += log(max(dot(color, float3(0.2126, 0.7152, 0.0722)), 1e-3));
						}
					}
					sums[index] = sum;
					GroupMemoryBarrierWithGroupSync();
					for (uint half = 128; half > 0; half >>= 1) {
						if (index < half)
							sums[index] += sums[index + half];
						GroupMemoryBarrierWithGroupSync();
					}
					if (index == 0)
						Out[uint2(0, 0)] = exp(sums[0] / 4096);
				}
			)";
			std::string errors;
			auto*       blob = D3D::Compile(source, "SeamlessMeter", "CS", "cs_5_0", 0, &errors);
			meter.failed = !blob;
			if (!blob) {
				logger::error("scene brightness: shader failed: {}", errors);
			}
			if (!meter.failed) {
				D3D11_BUFFER_DESC    constants{ sizeof(float) * 4, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
				D3D11_TEXTURE2D_DESC result{ 1, 1, 1, 1, DXGI_FORMAT_R32_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0 };
				D3D11_TEXTURE2D_DESC staging{ 1, 1, 1, 1, DXGI_FORMAT_R32_FLOAT, { 1, 0 }, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0 };
				meter.failed = FAILED(a_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &meter.shader)) ||
				               FAILED(a_device->CreateBuffer(&constants, nullptr, &meter.constants)) || FAILED(a_device->CreateTexture2D(&result, nullptr, &meter.result)) ||
				               FAILED(a_device->CreateUnorderedAccessView(meter.result, nullptr, &meter.resultView));
				for (auto*& copy : meter.staging) {
					meter.failed = meter.failed || FAILED(a_device->CreateTexture2D(&staging, nullptr, &copy));
				}
				if (meter.failed) {
					logger::error("scene brightness: could not create the meter");
				}
			}
			if (blob) {
				blob->Release();
			}
			return !meter.failed;
		}

		void Adapt(float a_reading)
		{
			const auto now = std::chrono::steady_clock::now();
			if (meter.adapted <= 0.0f) {
				meter.adapted = a_reading;
			} else {
				// In log space, as the eye adapts: halving and doubling take as long.
				const float seconds = std::chrono::duration<float>(now - meter.read).count();
				const float toward = 1.0f - std::exp(-seconds / kAdaptSeconds);
				meter.adapted = std::exp(std::log(meter.adapted) + (std::log(a_reading) - std::log(meter.adapted)) * toward);
			}
			meter.read = now;
		}
	}

	void Measure(void* a_scene, float a_width, float a_height)
	{
		auto* context = D3D::Context();
		auto* device = D3D::Device();
		if (!context || !device || !a_scene || !EnsureMeter(device)) {
			return;
		}
		auto* copy = meter.staging[meter.next];
		if (meter.pending[meter.next]) {
			D3D11_MAPPED_SUBRESOURCE mapped{};
			const HRESULT            result = context->Map(copy, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
			if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
				return;  // the GPU is behind: the next frame
			}
			if (SUCCEEDED(result)) {
				const float reading = *static_cast<const float*>(mapped.pData);
				context->Unmap(copy, 0);
				if (std::isfinite(reading) && reading > 0.0f) {
					Adapt(reading);
				}
			}
			meter.pending[meter.next] = false;
		}
		const float size[4]{ a_width, a_height, 0.0f, 0.0f };
		D3D::Upload(context, meter.constants, size, sizeof(size));
		// The scene may still be bound as a target, and a view of it then reads as nothing.
		const D3D::SavedState   saved(context);
		ID3D11RenderTargetView* noTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, noTargets, nullptr);
		ID3D11ComputeShader*       savedShader = nullptr;
		ID3D11ShaderResourceView*  savedView = nullptr;
		ID3D11UnorderedAccessView* savedOut = nullptr;
		ID3D11Buffer*              savedConstants = nullptr;
		context->CSGetShader(&savedShader, nullptr, nullptr);
		context->CSGetShaderResources(0, 1, &savedView);
		context->CSGetUnorderedAccessViews(0, 1, &savedOut);
		context->CSGetConstantBuffers(0, 1, &savedConstants);
		auto* scene = static_cast<ID3D11ShaderResourceView*>(a_scene);
		context->CSSetShader(meter.shader, nullptr, 0);
		context->CSSetShaderResources(0, 1, &scene);
		context->CSSetUnorderedAccessViews(0, 1, &meter.resultView, nullptr);
		context->CSSetConstantBuffers(0, 1, &meter.constants);
		context->Dispatch(1, 1, 1);
		context->CSSetShader(savedShader, nullptr, 0);
		context->CSSetShaderResources(0, 1, &savedView);
		context->CSSetUnorderedAccessViews(0, 1, &savedOut, nullptr);
		context->CSSetConstantBuffers(0, 1, &savedConstants);
		for (IUnknown* object : std::initializer_list<IUnknown*>{ savedShader, savedView, savedOut, savedConstants }) {
			if (object) {
				object->Release();
			}
		}
		context->CopyResource(copy, meter.result);
		meter.pending[meter.next] = true;
		meter.next = (meter.next + 1) % meter.staging.size();
	}

	std::optional<float> Brightness()
	{
		return meter.adapted > 0.0f ? std::optional(meter.adapted) : std::nullopt;
	}
}
