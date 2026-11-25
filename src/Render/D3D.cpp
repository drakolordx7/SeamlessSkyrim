#include "Render/D3D.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace Render::D3D
{
	ID3D11DeviceContext* Context()
	{
		return reinterpret_cast<ID3D11DeviceContext*>(RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context);
	}

	ID3D11Device* Device()
	{
		auto*         context = Context();
		ID3D11Device* device = nullptr;
		if (context) {
			context->GetDevice(&device);
		}
		if (device) {
			device->Release();
		}
		return device;
	}

	std::array<float, 2> RenderScale()
	{
		const auto& state = RE::BSGraphics::State::GetSingleton()->GetRuntimeData();
		return { state.dynamicResolutionWidthRatio > 0.0f ? state.dynamicResolutionWidthRatio : 1.0f,
			state.dynamicResolutionHeightRatio > 0.0f ? state.dynamicResolutionHeightRatio : 1.0f };
	}

	ID3DBlob* Compile(std::string_view a_source, const char* a_name, const char* a_entry, const char* a_profile, UINT a_flags, std::string* a_errors)
	{
		ID3DBlob* code = nullptr;
		ID3DBlob* errors = nullptr;
		if (FAILED(::D3DCompile(a_source.data(), a_source.size(), a_name, nullptr, nullptr, a_entry, a_profile, a_flags, 0, &code, &errors))) {
			if (code) {
				code->Release();
				code = nullptr;
			}
		}
		if (errors) {
			if (a_errors) {
				*a_errors = static_cast<const char*>(errors->GetBufferPointer());
			}
			errors->Release();
		}
		return code;
	}

	bool Invert(const double m[16], double out[16])
	{
		double inv[16];
		inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
		inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
		inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
		inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
		inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
		inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
		inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
		inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
		inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
		inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
		inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
		inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
		inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
		inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
		inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
		inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
		const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
		if (std::abs(det) < 1e-30) {
			return false;
		}
		for (int i = 0; i < 16; ++i) {
			out[i] = inv[i] / det;
		}
		return true;
	}

	std::array<float, 4> ClipOf(const float* a_viewProj, const RE::NiPoint3& a_eye, const RE::NiPoint3& a_point)
	{
		const float          v[3] = { a_point.x - a_eye.x, a_point.y - a_eye.y, a_point.z - a_eye.z };
		std::array<float, 4> clip{};
		for (int column = 0; column < 4; ++column) {
			clip[column] = v[0] * a_viewProj[column] + v[1] * a_viewProj[4 + column] + v[2] * a_viewProj[8 + column] + a_viewProj[12 + column];
		}
		return clip;
	}

	SavedState::SavedState(ID3D11DeviceContext* a_context) :
		context(a_context)
	{
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depthView);
		context->OMGetDepthStencilState(&depthState, &stencilRef);
		context->OMGetBlendState(&blendState, blendFactor, &sampleMask);
		context->RSGetState(&rasterState);
		context->RSGetViewports(&viewportCount, viewports);
		context->VSGetShader(&vertexShader, nullptr, nullptr);
		context->PSGetShader(&pixelShader, nullptr, nullptr);
		context->GSGetShader(&geometryShader, nullptr, nullptr);
		context->HSGetShader(&hullShader, nullptr, nullptr);
		context->DSGetShader(&domainShader, nullptr, nullptr);
		context->IAGetInputLayout(&layout);
		context->IAGetPrimitiveTopology(&topology);
		context->IAGetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
		context->PSGetShaderResources(0, 2, textures);
		context->PSGetSamplers(0, 1, &sampler);
		context->PSGetConstantBuffers(0, 1, &psConstants);
		context->VSGetConstantBuffers(0, 1, &vsConstants);
	}

	SavedState::~SavedState()
	{
		context->PSSetShaderResources(0, 2, textures);
		context->PSSetSamplers(0, 1, &sampler);
		context->PSSetConstantBuffers(0, 1, &psConstants);
		context->VSSetConstantBuffers(0, 1, &vsConstants);
		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depthView);
		context->OMSetDepthStencilState(depthState, stencilRef);
		context->OMSetBlendState(blendState, blendFactor, sampleMask);
		context->RSSetState(rasterState);
		context->RSSetViewports(viewportCount, viewports);
		context->VSSetShader(vertexShader, nullptr, 0);
		context->PSSetShader(pixelShader, nullptr, 0);
		context->GSSetShader(geometryShader, nullptr, 0);
		context->HSSetShader(hullShader, nullptr, 0);
		context->DSSetShader(domainShader, nullptr, 0);
		context->IASetInputLayout(layout);
		context->IASetPrimitiveTopology(topology);
		context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
		for (auto* target : targets) {
			if (target) {
				target->Release();
			}
		}
		for (IUnknown* object : std::initializer_list<IUnknown*>{ depthView, depthState, blendState, rasterState, vertexShader, pixelShader, geometryShader, hullShader, domainShader,
				 layout, vertexBuffer, textures[0], textures[1], sampler, psConstants, vsConstants }) {
			if (object) {
				object->Release();
			}
		}
	}

	void Upload(ID3D11DeviceContext* a_context, ID3D11Buffer* a_buffer, const void* a_data, std::size_t a_size)
	{
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(a_context->Map(a_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, a_data, a_size);
			a_context->Unmap(a_buffer, 0);
		}
	}
}
