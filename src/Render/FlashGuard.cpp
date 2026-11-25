#include "Render/FlashGuard.h"

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
		// loading screen it is never seen). For a short while after each crossing every frame's
		// brightness is read before it is shown, waiting for the GPU (a few rows, about a millisecond),
		// and a frame far brighter than the one shown before it is replaced by that one. Not by skipping
		// the Present: with Presents skipped the white lasted as many frames as were skipped (something
		// in the engine or a mod only moves on with each Present).
		constexpr float kFlashJump = 0.3f;  // of the average brightness, 0..1
		// Only a frame washed out to near white: a real step in brightness (the street after a dark room
		// on the first walk outdoors, 0.23 to 0.53) was held for six frames and then shown anyway.
		constexpr float kFlashBrightness = 0.8f;
		constexpr int   kMaxHeldInRow = 6;  // a scene really that bright is shown after this many
		std::atomic<int>                        guardFrames = 0;
		std::atomic<bool>                       guardReset = false;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> guardTexture;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> guardLastShown;  // copy of the last frame shown
		float                                   guardShown = -1.0f;  // brightness of the last frame shown
		int                                     guardHeld = 0;

		bool HoldFlash(IDXGISwapChain* a_swapChain)
		{
			if (guardReset.exchange(false)) {
				guardShown = -1.0f;
				guardHeld = 0;
			}
			Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
			if (FAILED(a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.GetAddressOf())))) {
				return false;
			}
			D3D11_TEXTURE2D_DESC desc{};
			backBuffer->GetDesc(&desc);
			const bool known = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
			if (!known || desc.SampleDesc.Count != 1) {
				return false;
			}
			Microsoft::WRL::ComPtr<ID3D11Device>        device;
			Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
			backBuffer->GetDevice(device.GetAddressOf());
			device->GetImmediateContext(context.GetAddressOf());
			D3D11_TEXTURE2D_DESC have{};
			if (guardTexture) {
				guardTexture->GetDesc(&have);
			}
			if (!guardTexture || have.Width != desc.Width || have.Format != desc.Format) {
				D3D11_TEXTURE2D_DESC staging{};
				staging.Width = desc.Width;
				staging.Height = static_cast<UINT>(std::size(kRows));
				staging.MipLevels = 1;
				staging.ArraySize = 1;
				staging.Format = desc.Format;
				staging.SampleDesc.Count = 1;
				staging.Usage = D3D11_USAGE_STAGING;
				staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				guardTexture = nullptr;
				if (FAILED(device->CreateTexture2D(&staging, nullptr, guardTexture.GetAddressOf()))) {
					return false;
				}
			}
			for (UINT row = 0; row < std::size(kRows); ++row) {
				const UINT      y = desc.Height / kRows[row];
				const D3D11_BOX box{ 0, y, 0, desc.Width, y + 1, 1 };
				context->CopySubresourceRegion(guardTexture.Get(), 0, 0, row, 0, backBuffer.Get(), 0, &box);
			}
			D3D11_MAPPED_SUBRESOURCE map{};
			if (FAILED(context->Map(guardTexture.Get(), 0, D3D11_MAP_READ, 0, &map))) {
				return false;
			}
			const float value = RowAverage(map, desc.Width, desc.Format);
			context->Unmap(guardTexture.Get(), 0);
			if (guardShown >= 0.0f && guardLastShown && value - guardShown > kFlashJump && value > kFlashBrightness && guardHeld < kMaxHeldInRow) {
				++guardHeld;
				logger::info("flash guard: a frame of brightness {:.2f} after {:.2f} replaced by the one before", value, guardShown);
				context->CopyResource(backBuffer.Get(), guardLastShown.Get());
				return true;
			}
			D3D11_TEXTURE2D_DESC copyDesc{};
			if (guardLastShown) {
				guardLastShown->GetDesc(&copyDesc);
			}
			if (!guardLastShown || copyDesc.Width != desc.Width || copyDesc.Height != desc.Height || copyDesc.Format != desc.Format) {
				D3D11_TEXTURE2D_DESC copy = desc;
				copy.Usage = D3D11_USAGE_DEFAULT;
				copy.BindFlags = 0;
				copy.CPUAccessFlags = 0;
				copy.MiscFlags = 0;
				guardLastShown = nullptr;
				if (FAILED(device->CreateTexture2D(&copy, nullptr, guardLastShown.GetAddressOf()))) {
					guardLastShown = nullptr;
				}
			}
			if (guardLastShown) {
				context->CopyResource(guardLastShown.Get(), backBuffer.Get());
			}
			guardShown = value;
			guardHeld = 0;
			return false;
		}
	}

	void Guard(int a_frames)
	{
		guardReset = true;
		guardFrames = a_frames;
	}

	bool Apply(IDXGISwapChain* a_swapChain)
	{
		if (guardFrames > 0) {
			--guardFrames;
			return HoldFlash(a_swapChain);
		}
		return false;
	}
}
