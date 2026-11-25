#include "Bridge/FrameHooks.h"

#include "Bridge/AgentMode.h"

#include "Plugin/Frame.h"
#include "Render/FlashGuard.h"
#include "Render/Present.h"

#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <directxtk/ScreenGrab.h>

namespace
{
	// ---- walk_to ---------------------------------------------------------------------------

	struct WalkState
	{
		FrameHooks::WalkRequest request;
		bool                    active = false;
		std::string             status = "idle";
		float                   elapsed = 0.0f;
		float                   stuckTime = 0.0f;
		RE::NiPoint3            lastPos;
	} walk;

	// Below this many units per second for a while we're walking into a wall or a closed door.
	constexpr float kStuckSpeed = 20.0f;
	constexpr float kStuckSeconds = 1.5f;

	void SteerPlayer(RE::PlayerCharacter* a_player, float a_delta)
	{
		auto* controls = RE::PlayerControls::GetSingleton();
		if (!walk.active || !controls) {
			return;
		}

		RE::NiPoint3 goal = walk.request.point;
		if (walk.request.target) {
			auto ref = walk.request.target.get();
			if (!ref) {
				FrameHooks::StopWalk("target reference no longer loaded");
				return;
			}
			goal = ref->GetPosition();
		}

		const auto  pos = a_player->GetPosition();
		const float dx = goal.x - pos.x;
		const float dy = goal.y - pos.y;
		const float distance = std::sqrt(dx * dx + dy * dy);
		walk.elapsed += a_delta;

		if (distance <= walk.request.stopDistance) {
			FrameHooks::StopWalk("arrived");
			return;
		}
		if (walk.elapsed >= walk.request.timeoutSeconds) {
			FrameHooks::StopWalk("timeout");
			return;
		}
		const float moved = std::hypot(pos.x - walk.lastPos.x, pos.y - walk.lastPos.y);
		walk.lastPos = pos;
		walk.stuckTime = (a_delta > 0.0f && moved / a_delta < kStuckSpeed) ? walk.stuckTime + a_delta : 0.0f;
		if (walk.stuckTime >= kStuckSeconds) {
			FrameHooks::StopWalk("stuck");
			return;
		}

		// Skyrim's yaw: 0 faces +Y (north), increasing clockwise.
		a_player->SetHeading(std::atan2(dx, dy));
		// Autorun is the engine's own "move forward" input, so movement goes through the normal
		// character controller, collision and animation instead of being faked.
		controls->data.autoMove = true;
		controls->data.running = walk.request.run;
	}

	// ---- screenshot -------------------------------------------------------------------------

	struct CaptureRequest
	{
		std::wstring                       path;
		std::shared_ptr<std::promise<json>> done;
	};

	std::mutex                    captureLock;
	std::optional<CaptureRequest> pendingCapture;

	// CaptureFrames: copies on the GPU while taking them (a frame each), written out after.
	struct FrameSequence
	{
		std::wstring                                        directory;
		int                                                 left = 0;
		std::chrono::steady_clock::time_point               start;
		std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> frames;
		std::vector<double>                                 ms;
	};
	FrameSequence frameSequence;  // under captureLock

	void TakeSequenceFrame(IDXGISwapChain* a_swapChain)
	{
		auto& sequence = frameSequence;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
		if (FAILED(a_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())))) {
			sequence = {};
			return;
		}
		Microsoft::WRL::ComPtr<ID3D11Device>        device;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
		backBuffer->GetDevice(device.GetAddressOf());
		device->GetImmediateContext(context.GetAddressOf());
		D3D11_TEXTURE2D_DESC desc{};
		backBuffer->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
		if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, copy.GetAddressOf()))) {
			context->CopyResource(copy.Get(), backBuffer.Get());
			sequence.frames.push_back(copy);
			sequence.ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sequence.start).count());
		}
		if (--sequence.left > 0) {
			return;
		}
		const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		for (std::size_t i = 0; i < sequence.frames.size(); ++i) {
			const auto path = std::format(L"{}\\frame_{:02}_{:05.0f}.jpg", sequence.directory, i, sequence.ms[i]);
			DirectX::SaveWICTextureToFile(context.Get(), sequence.frames[i].Get(), GUID_ContainerFormatJpeg, path.c_str(), &GUID_WICPixelFormat24bppBGR);
		}
		if (SUCCEEDED(coInit)) {
			CoUninitialize();
		}
		logger::info("frames: {} saved to {}", sequence.frames.size(), std::filesystem::path(sequence.directory).string());
		sequence = {};
	}

	json Capture(IDXGISwapChain* a_swapChain, const std::wstring& a_path)
	{
		ID3D11Texture2D* backBuffer = nullptr;
		if (FAILED(a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
			throw std::runtime_error("GetBuffer failed");
		}
		ID3D11Device*        device = nullptr;
		ID3D11DeviceContext* context = nullptr;
		backBuffer->GetDevice(&device);
		device->GetImmediateContext(&context);

		D3D11_TEXTURE2D_DESC desc{};
		backBuffer->GetDesc(&desc);

		// WIC needs COM on this thread; an already-initialized apartment is fine either way.
		const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		// Force 24-bit BGR: the back buffer may be 10-bit or HDR, which JPEG can't hold.
		const HRESULT saved = DirectX::SaveWICTextureToFile(context, backBuffer, GUID_ContainerFormatJpeg, a_path.c_str(), &GUID_WICPixelFormat24bppBGR);
		if (SUCCEEDED(coInit)) {
			CoUninitialize();
		}

		context->Release();
		device->Release();
		backBuffer->Release();
		if (FAILED(saved)) {
			throw std::runtime_error(std::format("SaveWICTextureToFile failed (0x{:08X}, format {})", static_cast<std::uint32_t>(saved), static_cast<int>(desc.Format)));
		}
		return { { "width", desc.Width }, { "height", desc.Height }, { "format", static_cast<int>(desc.Format) } };
	}

	// ---- brightness log -------------------------------------------------------------------------

	struct BrightnessSample
	{
		std::string time;  // as the log prints it
		float       ms;    // since the frame before
		float       value;
		bool        arrival;  // the first frame drawn in a new space (MarkArrivalFrame)
		bool        held;     // drawn but not shown (GuardFlashes)
	};
	std::mutex                    brightnessLock;
	bool                          brightnessOn = false;
	std::vector<BrightnessSample> brightness;
	constexpr std::size_t         kMaxBrightnessSamples = 20000;

	// A few rows across the screen, copied to a staging texture and read two frames later, so the
	// GPU isn't waited for: three textures in turn.
	using Render::FlashGuard::kRows;
	using Render::FlashGuard::RowAverage;
	struct Readback
	{
		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
		std::string                     time;
		float                           ms = 0.0f;
		bool                            arrival = false;
		bool                            held = false;
		bool                            filled = false;
	};
	std::array<Readback, 3> readbacks;
	std::size_t             readbackIndex = 0;
	std::optional<std::chrono::steady_clock::time_point> lastPresent;

	void SampleBrightness(IDXGISwapChain* a_swapChain, bool a_arrival, bool a_held)
	{
		const auto now = std::chrono::steady_clock::now();
		const float ms = lastPresent ? std::chrono::duration<float, std::milli>(now - *lastPresent).count() : 0.0f;
		lastPresent = now;

		Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
		if (FAILED(a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backBuffer.GetAddressOf())))) {
			return;
		}
		D3D11_TEXTURE2D_DESC desc{};
		backBuffer->GetDesc(&desc);
		const bool known = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
		                   desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
		if (!known || desc.SampleDesc.Count != 1) {
			logger::warn("brightness log: back buffer format {} ({} samples) not read; stopped", static_cast<int>(desc.Format), desc.SampleDesc.Count);
			brightnessOn = false;
			return;
		}
		Microsoft::WRL::ComPtr<ID3D11Device>        device;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
		backBuffer->GetDevice(device.GetAddressOf());
		device->GetImmediateContext(context.GetAddressOf());

		auto& slot = readbacks[readbackIndex++ % readbacks.size()];
		if (slot.filled) {
			D3D11_MAPPED_SUBRESOURCE map{};
			if (SUCCEEDED(context->Map(slot.texture.Get(), 0, D3D11_MAP_READ, 0, &map))) {
				const float value = RowAverage(map, desc.Width, desc.Format);
				context->Unmap(slot.texture.Get(), 0);
				std::scoped_lock lock(brightnessLock);
				if (brightness.size() < kMaxBrightnessSamples) {
					brightness.push_back({ slot.time, slot.ms, value, slot.arrival, slot.held });
				}
			}
		}
		D3D11_TEXTURE2D_DESC have{};
		if (slot.texture) {
			slot.texture->GetDesc(&have);
		}
		if (!slot.texture || have.Width != desc.Width || have.Format != desc.Format) {
			D3D11_TEXTURE2D_DESC staging{};
			staging.Width = desc.Width;
			staging.Height = static_cast<UINT>(std::size(kRows));
			staging.MipLevels = 1;
			staging.ArraySize = 1;
			staging.Format = desc.Format;
			staging.SampleDesc.Count = 1;
			staging.Usage = D3D11_USAGE_STAGING;
			staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			slot.texture = nullptr;
			if (FAILED(device->CreateTexture2D(&staging, nullptr, slot.texture.GetAddressOf()))) {
				brightnessOn = false;
				return;
			}
		}
		for (UINT row = 0; row < std::size(kRows); ++row) {
			const UINT     y = desc.Height / kRows[row];
			const D3D11_BOX box{ 0, y, 0, desc.Width, y + 1, 1 };
			context->CopySubresourceRegion(slot.texture.Get(), 0, 0, row, 0, backBuffer.Get(), 0, &box);
		}
		const auto wall = std::chrono::system_clock::now();
		slot.time = std::format("{:%H:%M:%S}", std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::milliseconds>(wall)));
		slot.ms = ms;
		slot.arrival = a_arrival;
		slot.held = a_held;
		slot.filled = true;
	}

	// Runs in the Present hook (Render::Present) after the flash guard: the brightness log, then the
	// screenshot and the frame sequence.
	void OnPresent(IDXGISwapChain* a_swapChain, bool a_arrival, bool a_held)
	{
		if (brightnessOn) {
			SampleBrightness(a_swapChain, a_arrival, a_held);
		}
		std::optional<CaptureRequest> request;
		{
			std::scoped_lock lock(captureLock);
			request.swap(pendingCapture);
			if (frameSequence.left > 0) {
				TakeSequenceFrame(a_swapChain);
			}
		}
		if (request) {
			// Before Present: afterwards a flip-model back buffer's contents are undefined.
			try {
				request->done->set_value(Capture(a_swapChain, request->path));
			} catch (...) {
				request->done->set_exception(std::current_exception());
			}
		}
	}
}

namespace FrameHooks
{
	void Register()
	{
		Plugin::Frame::SetPreUpdate(SteerPlayer);
		Render::Present::SetCallback(OnPresent);
	}

	void StartWalk(const WalkRequest& a_request)
	{
		walk = {};
		walk.request = a_request;
		walk.active = true;
		walk.status = "walking";
		walk.lastPos = RE::PlayerCharacter::GetSingleton()->GetPosition();
	}

	void StopWalk(std::string_view a_reason)
	{
		if (walk.active) {
			if (auto* controls = RE::PlayerControls::GetSingleton()) {
				controls->data.autoMove = false;
			}
		}
		walk.active = false;
		walk.status = a_reason;
	}

	json FrameStats()
	{
		return Plugin::Frame::FrameStats();
	}

	json WalkStatus()
	{
		return { { "active", walk.active }, { "status", walk.status }, { "elapsed", walk.elapsed } };
	}

	bool CaptureFrames(const std::wstring& a_directory, int a_frames)
	{
		std::scoped_lock lock(captureLock);
		if (!Render::Present::Installed()) {
			return false;
		}
		// Takes over from a run still being taken, whose frames are dropped.
		frameSequence = { a_directory, a_frames, std::chrono::steady_clock::now() };
		return true;
	}

	// Main thread only, like the crossings and merges that read them.
	std::optional<std::wstring> eventCaptureRoot;
	int                         eventCaptureCount = 0;

	void SetEventCapture(const std::wstring& a_directory)
	{
		eventCaptureRoot = a_directory;
		eventCaptureCount = 0;
	}

	void CaptureEventFrames(std::wstring_view a_event, int a_frames)
	{
		if (!eventCaptureRoot) {
			wchar_t directory[MAX_PATH];
			eventCaptureRoot = AgentMode::Enabled() && ::GetEnvironmentVariableW(L"SEAMLESS_TRAVEL_CAPTURE", directory, MAX_PATH) > 0 ? std::wstring(directory) : std::wstring();
		}
		if (eventCaptureRoot->empty()) {
			return;
		}
		{
			// A merge comes a few hundred ms into its crossing's frames. Starting anew there threw
			// the crossing's frames away, every time; one run of frames shows both.
			std::scoped_lock lock(captureLock);
			if (frameSequence.left > 0) {
				constexpr int kMostFrames = 60;  // each is a copy of the back buffer until written
				frameSequence.left = std::max(frameSequence.left, std::min(a_frames, kMostFrames - static_cast<int>(frameSequence.frames.size())));
				return;
			}
		}
		const auto directory = std::format(L"{}\\{}_{:02}", *eventCaptureRoot, a_event, ++eventCaptureCount);
		std::error_code error;
		std::filesystem::create_directories(directory, error);
		CaptureFrames(directory, a_frames);
	}

	void RecordBrightness(bool a_start)
	{
		std::scoped_lock lock(brightnessLock);
		if (a_start) {
			brightness.clear();
			for (auto& slot : readbacks) {
				slot.filled = false;
			}
			lastPresent.reset();
		}
		brightnessOn = a_start;
	}

	json BrightnessLog()
	{
		std::scoped_lock lock(brightnessLock);
		json rows = json::array();
		for (const auto& s : brightness) {
			rows.push_back({ s.time, std::round(s.ms * 10.0f) / 10.0f, std::round(s.value * 1000.0f) / 1000.0f, s.arrival, s.held });
		}
		return { { "recording", brightnessOn }, { "frames", rows } };
	}

	json CaptureScreenshot(const std::wstring& a_path, std::chrono::milliseconds a_timeout)
	{
		if (!Render::Present::Installed()) {
			throw std::runtime_error("screenshot hook not installed");
		}
		auto done = std::make_shared<std::promise<json>>();
		auto future = done->get_future();
		{
			std::scoped_lock lock(captureLock);
			if (pendingCapture) {
				throw std::runtime_error("a screenshot is already pending");
			}
			pendingCapture = CaptureRequest{ a_path, done };
		}
		if (future.wait_for(a_timeout) != std::future_status::ready) {
			std::scoped_lock lock(captureLock);
			pendingCapture.reset();
			throw std::runtime_error("no frame presented in time (game minimized?)");
		}
		return future.get();
	}
}
