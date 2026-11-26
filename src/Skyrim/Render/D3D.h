#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>

// The renderer's device and context, and what every piece of our own drawing needs: state that goes
// back as it was, shader compilation, the matrix and clip math.
namespace Render::D3D
{
	[[nodiscard]] ID3D11DeviceContext* Context();
	// The engine holds the device for the whole session; the reference taken here isn't kept.
	[[nodiscard]] ID3D11Device* Device();

	template <class T>
	void Release(T*& a_object)
	{
		if (a_object) {
			a_object->Release();
			a_object = nullptr;
		}
	}

	// The fraction of its target the main view is drawn into (1 without an upscaler or dynamic
	// resolution).
	[[nodiscard]] std::array<float, 2> RenderScale();

	// Compiles a_source's a_entry, or returns null with the compiler's message in a_errors. What a
	// failure means is the caller's: our own sky and doorway shaders are asserted, the pictures'
	// and the meter's switch themselves off.
	[[nodiscard]] ID3DBlob* Compile(std::string_view a_source, const char* a_name, const char* a_entry, const char* a_profile, UINT a_flags, std::string* a_errors);

	// Fills a_out with the inverse of the 4x4 matrix a_m; false when it has none.
	bool Invert(const double a_m[16], double a_out[16]);

	// a_point in clip space: the camera-relative position (the point less a_eye) through the row
	// vector view-projection matrix a_viewProj, as the engine's vertex shaders project.
	[[nodiscard]] std::array<float, 4> ClipOf(const float* a_viewProj, const RE::NiPoint3& a_eye, const RE::NiPoint3& a_point);

	// The engine tracks what it has bound; everything the draw changes goes back exactly as it was.
	class SavedState
	{
	public:
		explicit SavedState(ID3D11DeviceContext* a_context);
		~SavedState();
		SavedState(const SavedState&) = delete;
		SavedState& operator=(const SavedState&) = delete;

	private:
		ID3D11DeviceContext*      context;
		ID3D11RenderTargetView*   targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView*   depthView = nullptr;
		ID3D11DepthStencilState*  depthState = nullptr;
		UINT                      stencilRef = 0;
		ID3D11BlendState*         blendState = nullptr;
		FLOAT                     blendFactor[4]{};
		UINT                      sampleMask = 0;
		ID3D11RasterizerState*    rasterState = nullptr;
		D3D11_VIEWPORT            viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		UINT                      viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		ID3D11VertexShader*       vertexShader = nullptr;
		ID3D11PixelShader*        pixelShader = nullptr;
		ID3D11GeometryShader*     geometryShader = nullptr;
		ID3D11HullShader*         hullShader = nullptr;
		ID3D11DomainShader*       domainShader = nullptr;
		ID3D11InputLayout*        layout = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY  topology{};
		ID3D11Buffer*             vertexBuffer = nullptr;
		UINT                      stride = 0, offset = 0;
		ID3D11ShaderResourceView* textures[4]{};
		// The compute stage (the pictures' compression binds one texture, one target, one buffer).
		ID3D11ComputeShader*       computeShader = nullptr;
		ID3D11ShaderResourceView*  computeTexture = nullptr;
		ID3D11UnorderedAccessView* computeTarget = nullptr;
		ID3D11Buffer*              computeConstants = nullptr;
		ID3D11SamplerState*       sampler = nullptr;
		ID3D11Buffer*             psConstants = nullptr;
		ID3D11Buffer*             vsConstants = nullptr;
	};

	// Writes a_size bytes into a dynamic buffer.
	void Upload(ID3D11DeviceContext* a_context, ID3D11Buffer* a_buffer, const void* a_data, std::size_t a_size);
}
