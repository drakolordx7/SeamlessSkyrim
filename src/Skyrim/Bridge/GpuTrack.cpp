#include "Skyrim/Bridge/GpuTrack.h"

#include <d3d11.h>
#include <d3dcommon.h>
#include <psapi.h>
#include <unordered_set>
#include <wrl/client.h>

namespace
{
	constexpr int kStackFrames = 40;

	struct Resource
	{
		std::uint64_t                     seq = 0;
		std::uint64_t                     bytes = 0;
		std::string                       kind;
		std::array<void*, kStackFrames>   stack{};
		std::uint16_t                     depth = 0;
		RE::NiSourceTexture*              source = nullptr;  // the engine texture it was made for
		void*                             rendererTexture = nullptr;  // its BSGraphics::Texture
	};
	// Who took and gave back references to each renderer texture (BSGraphics::Texture, counted at
	// +0x20; the renderer's vfuncs 0xD8 and 0xE0): the code calling, three frames deep.
	struct RefLog
	{
		std::unordered_map<std::string, int> added;
		std::unordered_map<std::string, int> released;
	};
	std::unordered_map<const void*, RefLog> refLogs;  // under lock
	thread_local std::vector<const void*>   madeNow;  // the resources made during MakeSourceHook

	std::recursive_mutex                        lock;  // recursive: a ComPtr taken under it calls AddRefHook
	std::unordered_map<const void*, Resource>   live;  // under lock
	std::atomic<std::uint64_t>                  nextSeq = 1;
	bool                                        started = false;
	// The engine textures alive (made through the renderer while tracking, not yet destroyed).
	std::unordered_set<const void*> sources;  // under lock
	thread_local RE::NiSourceTexture* making = nullptr;  // the renderer is making its texture now

	std::uint64_t BitsPerPixel(DXGI_FORMAT a_format)
	{
		switch (a_format) {
		case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
		case DXGI_FORMAT_R32G32B32A32_UINT:
		case DXGI_FORMAT_R32G32B32A32_SINT:
			return 128;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R16G16B16A16_UINT:
		case DXGI_FORMAT_R16G16B16A16_SNORM:
		case DXGI_FORMAT_R16G16B16A16_SINT:
		case DXGI_FORMAT_R32G32_TYPELESS:
		case DXGI_FORMAT_R32G32_FLOAT:
		case DXGI_FORMAT_R32G32_UINT:
		case DXGI_FORMAT_R32G32_SINT:
			return 64;
		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_R16_UINT:
		case DXGI_FORMAT_R16_SNORM:
		case DXGI_FORMAT_R16_SINT:
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R8G8_TYPELESS:
		case DXGI_FORMAT_R8G8_UNORM:
		case DXGI_FORMAT_R8G8_UINT:
		case DXGI_FORMAT_R8G8_SNORM:
		case DXGI_FORMAT_R8G8_SINT:
			return 16;
		case DXGI_FORMAT_R8_TYPELESS:
		case DXGI_FORMAT_R8_UNORM:
		case DXGI_FORMAT_R8_UINT:
		case DXGI_FORMAT_R8_SNORM:
		case DXGI_FORMAT_R8_SINT:
		case DXGI_FORMAT_A8_UNORM:
			return 8;
		default:
			return 32;
		}
	}

	// Bytes of a block of 4x4 texels, or 0 for a format that isn't block-compressed.
	std::uint64_t BlockBytes(DXGI_FORMAT a_format)
	{
		switch (a_format) {
		case DXGI_FORMAT_BC1_TYPELESS:
		case DXGI_FORMAT_BC1_UNORM:
		case DXGI_FORMAT_BC1_UNORM_SRGB:
		case DXGI_FORMAT_BC4_TYPELESS:
		case DXGI_FORMAT_BC4_UNORM:
		case DXGI_FORMAT_BC4_SNORM:
			return 8;
		case DXGI_FORMAT_BC2_TYPELESS:
		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC2_UNORM_SRGB:
		case DXGI_FORMAT_BC3_TYPELESS:
		case DXGI_FORMAT_BC3_UNORM:
		case DXGI_FORMAT_BC3_UNORM_SRGB:
		case DXGI_FORMAT_BC5_TYPELESS:
		case DXGI_FORMAT_BC5_UNORM:
		case DXGI_FORMAT_BC5_SNORM:
		case DXGI_FORMAT_BC6H_TYPELESS:
		case DXGI_FORMAT_BC6H_UF16:
		case DXGI_FORMAT_BC6H_SF16:
		case DXGI_FORMAT_BC7_TYPELESS:
		case DXGI_FORMAT_BC7_UNORM:
		case DXGI_FORMAT_BC7_UNORM_SRGB:
			return 16;
		default:
			return 0;
		}
	}

	std::uint64_t SurfaceBytes(DXGI_FORMAT a_format, std::uint64_t a_width, std::uint64_t a_height)
	{
		if (const auto block = BlockBytes(a_format)) {
			return ((a_width + 3) / 4) * ((a_height + 3) / 4) * block;
		}
		return a_width * a_height * BitsPerPixel(a_format) / 8;
	}

	// COM references taken and given back on each tracked texture and its shader resource views,
	// net, by the code calling AddRef/Release (outside Direct3D and the driver).
	std::unordered_map<const void*, std::unordered_map<std::string, int>> refNet;  // under lock
	std::unordered_map<const void*, const void*>                           views;   // view -> resource, under lock
	void WatchRefs(IUnknown* a_object);

	void __stdcall Destroyed(void* a_resource)
	{
		std::scoped_lock guard(lock);
		live.erase(a_resource);
		refNet.erase(a_resource);
	}

	void __stdcall ViewDestroyed(void* a_view)
	{
		std::scoped_lock guard(lock);
		views.erase(a_view);
		refNet.erase(a_view);
	}

	void Track(ID3D11DeviceChild* a_resource, std::uint64_t a_bytes, std::string a_kind)
	{
		Resource resource;
		resource.seq = nextSeq++;
		resource.bytes = a_bytes;
		resource.kind = std::move(a_kind);
		// Frame 0 is the hook itself.
		resource.depth = ::RtlCaptureStackBackTrace(2, kStackFrames, resource.stack.data(), nullptr);
		resource.source = making;
		if (making) {
			madeNow.push_back(a_resource);
		}
		ID3DDestructionNotifier* notifier = nullptr;
		if (FAILED(a_resource->QueryInterface(__uuidof(ID3DDestructionNotifier), reinterpret_cast<void**>(&notifier)))) {
			return;  // never dropped: not listed at all
		}
		{
			std::scoped_lock guard(lock);
			live[a_resource] = std::move(resource);
		}
		UINT id = 0;
		notifier->RegisterDestructionCallback(Destroyed, a_resource, &id);
		notifier->Release();
	}

	// The renderer's "make the texture of this NiSourceTexture" (the call at ID 70716 + 0x198, the
	// renderer singleton's vfunc 0xD0): textures the device makes meanwhile are that texture's.
	using MakeSource_t = void (*)(void*, RE::NiSourceTexture*);
	MakeSource_t makeSource = nullptr;
	void MakeSourceHook(void* a_renderer, RE::NiSourceTexture* a_texture)
	{
		{
			std::scoped_lock guard(lock);
			sources.insert(a_texture);
		}
		making = a_texture;
		madeNow.clear();
		makeSource(a_renderer, a_texture);
		making = nullptr;
		auto* rendererTexture = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(a_texture) + 0x48);
		std::scoped_lock guard(lock);
		for (const auto* resource : madeNow) {
			if (const auto it = live.find(resource); it != live.end()) {
				it->second.rendererTexture = rendererTexture;
			}
		}
		madeNow.clear();
	}

	std::string Callers()
	{
		void*      frames[4]{};
		const auto depth = ::RtlCaptureStackBackTrace(2, 4, frames, nullptr);
		std::string key;
		for (USHORT i = 0; i < depth; ++i) {
			HMODULE module = nullptr;
			::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(frames[i]), &module);
			wchar_t name[MAX_PATH]{};
			if (module) {
				::GetModuleFileNameW(module, name, MAX_PATH);
			}
			const std::filesystem::path path(name);
			key += std::format("{}+{:X} ", path.filename().string(), reinterpret_cast<std::uintptr_t>(frames[i]) - reinterpret_cast<std::uintptr_t>(module));
		}
		return key;
	}

	using TextureRef_t = void (*)(void*, void*);
	TextureRef_t addRefTexture = nullptr;
	TextureRef_t releaseTexture = nullptr;
	void AddRefTextureHook(void* a_renderer, void* a_texture)
	{
		if (a_texture) {
			auto key = Callers();
			std::scoped_lock guard(lock);
			++refLogs[a_texture].added[key];
		}
		addRefTexture(a_renderer, a_texture);
	}
	void ReleaseTextureHook(void* a_renderer, void* a_texture)
	{
		if (a_texture) {
			auto key = Callers();
			std::scoped_lock guard(lock);
			++refLogs[a_texture].released[key];
		}
		releaseTexture(a_renderer, a_texture);
	}

	using SourceDtor_t = void* (*)(RE::NiSourceTexture*, std::uint32_t);
	SourceDtor_t sourceDtor = nullptr;
	void* SourceDtorHook(RE::NiSourceTexture* a_texture, std::uint32_t a_flags)
	{
		{
			std::scoped_lock guard(lock);
			sources.erase(a_texture);
		}
		return sourceDtor(a_texture, a_flags);
	}

	using CreateBuffer_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
	using CreateTexture2D_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
	using CreateTexture3D_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE3D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D**);
	CreateBuffer_t    createBuffer = nullptr;
	CreateTexture2D_t createTexture2D = nullptr;
	CreateTexture3D_t createTexture3D = nullptr;

	HRESULT STDMETHODCALLTYPE CreateBufferHook(ID3D11Device* a_device, const D3D11_BUFFER_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Buffer** a_buffer)
	{
		const auto result = createBuffer(a_device, a_desc, a_data, a_buffer);
		if (SUCCEEDED(result) && a_buffer && *a_buffer) {
			Track(*a_buffer, a_desc->ByteWidth, std::format("buffer bind {:X} usage {}", a_desc->BindFlags, static_cast<int>(a_desc->Usage)));
		}
		return result;
	}

	HRESULT STDMETHODCALLTYPE CreateTexture2DHook(ID3D11Device* a_device, const D3D11_TEXTURE2D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture2D** a_texture)
	{
		const auto result = createTexture2D(a_device, a_desc, a_data, a_texture);
		if (SUCCEEDED(result) && a_texture && *a_texture) {
			std::uint64_t  bytes = 0;
			std::uint64_t  width = a_desc->Width, height = a_desc->Height;
			const UINT     mips = a_desc->MipLevels ? a_desc->MipLevels : 1;
			for (UINT mip = 0; mip < mips; ++mip) {
				bytes += SurfaceBytes(a_desc->Format, std::max<std::uint64_t>(1, width >> mip), std::max<std::uint64_t>(1, height >> mip));
			}
			bytes *= a_desc->ArraySize;
			Track(*a_texture, bytes, std::format("texture {}x{} format {} mips {} usage {}", a_desc->Width, a_desc->Height, static_cast<int>(a_desc->Format), a_desc->MipLevels, static_cast<int>(a_desc->Usage)));
			WatchRefs(*a_texture);
		}
		return result;
	}

	using CreateView_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
	CreateView_t createView = nullptr;
	HRESULT STDMETHODCALLTYPE CreateViewHook(ID3D11Device* a_device, ID3D11Resource* a_resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* a_desc, ID3D11ShaderResourceView** a_view)
	{
		const auto result = createView(a_device, a_resource, a_desc, a_view);
		if (SUCCEEDED(result) && a_view && *a_view) {
			bool tracked = false;
			{
				std::scoped_lock guard(lock);
				if (live.contains(a_resource)) {
					views[*a_view] = a_resource;
					tracked = true;
				}
			}
			if (tracked) {
				ID3DDestructionNotifier* notifier = nullptr;
				if (SUCCEEDED((*a_view)->QueryInterface(__uuidof(ID3DDestructionNotifier), reinterpret_cast<void**>(&notifier)))) {
					UINT id = 0;
					notifier->RegisterDestructionCallback(ViewDestroyed, *a_view, &id);
					notifier->Release();
				}
				WatchRefs(*a_view);
			}
		}
		return result;
	}

	HRESULT STDMETHODCALLTYPE CreateTexture3DHook(ID3D11Device* a_device, const D3D11_TEXTURE3D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture3D** a_texture)
	{
		const auto result = createTexture3D(a_device, a_desc, a_data, a_texture);
		if (SUCCEEDED(result) && a_texture && *a_texture) {
			Track(*a_texture, static_cast<std::uint64_t>(a_desc->Width) * a_desc->Height * a_desc->Depth * BitsPerPixel(a_desc->Format) / 8, std::format("texture3d {}x{}x{}", a_desc->Width, a_desc->Height, a_desc->Depth));
		}
		return result;
	}

	template <class T>
	T Patch(void** a_vtable, std::size_t a_index, T a_hook)
	{
		DWORD old = 0;
		::VirtualProtect(&a_vtable[a_index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
		auto* original = reinterpret_cast<T>(a_vtable[a_index]);
		a_vtable[a_index] = reinterpret_cast<void*>(a_hook);
		::VirtualProtect(&a_vtable[a_index], sizeof(void*), old, &old);
		return original;
	}

	struct Module
	{
		std::uintptr_t begin = 0;
		std::uintptr_t end = 0;
		std::string    name;
	};

	std::vector<Module> Modules()
	{
		std::vector<Module> modules;
		HMODULE             handles[1024];
		DWORD               needed = 0;
		if (!::K32EnumProcessModules(::GetCurrentProcess(), handles, sizeof(handles), &needed)) {
			return modules;
		}
		for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i) {
			MODULEINFO info{};
			wchar_t    path[MAX_PATH];
			if (::K32GetModuleInformation(::GetCurrentProcess(), handles[i], &info, sizeof(info)) && ::K32GetModuleBaseNameW(::GetCurrentProcess(), handles[i], path, MAX_PATH)) {
				const auto begin = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
				char       name[MAX_PATH];
				::WideCharToMultiByte(CP_UTF8, 0, path, -1, name, MAX_PATH, nullptr, nullptr);
				modules.push_back({ begin, begin + info.SizeOfImage, name });
			}
		}
		return modules;
	}

	// Direct3D, DXGI and the driver: every resource passes through them, so they say nothing.
	bool Plumbing(const std::string& a_module)
	{
		std::string lower = a_module;
		std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return lower.starts_with("d3d11") || lower.starts_with("dxgi") || lower.starts_with("nvwgf") || lower.starts_with("nvd3d") || lower.starts_with("amdxx") ||
		       lower.starts_with("igd") || lower == "kernelbase.dll" || lower == "ntdll.dll";
	}

	struct RefFunctions
	{
		void**                                 vtable = nullptr;
		ULONG(STDMETHODCALLTYPE* addRef)(IUnknown*) = nullptr;
		ULONG(STDMETHODCALLTYPE* release)(IUnknown*) = nullptr;
	};
	std::mutex                refFunctionsLock;
	std::vector<RefFunctions> refFunctions;  // under refFunctionsLock: one per vtable seen

	RefFunctions FunctionsOf(IUnknown* a_object)
	{
		auto**           vtable = *reinterpret_cast<void***>(a_object);
		std::scoped_lock guard(refFunctionsLock);
		return *std::ranges::find(refFunctions, vtable, &RefFunctions::vtable);
	}

	// The code taking or giving back a reference: the first three frames outside the plumbing.
	std::string RefCaller()
	{
		void*       frames[12]{};
		const auto  depth = ::RtlCaptureStackBackTrace(2, 12, frames, nullptr);
		std::string key;
		int         taken = 0;
		for (USHORT i = 0; i < depth && taken < 3; ++i) {
			HMODULE module = nullptr;
			::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(frames[i]), &module);
			char name[MAX_PATH]{};
			if (module) {
				::GetModuleFileNameA(module, name, MAX_PATH);
			}
			const std::string file = std::filesystem::path(name).filename().string();
			if (taken == 0 && Plumbing(file)) {
				continue;
			}
			key += std::format("{}+{:X} ", file, reinterpret_cast<std::uintptr_t>(frames[i]) - reinterpret_cast<std::uintptr_t>(module));
			++taken;
		}
		return key;
	}

	void NoteRef(IUnknown* a_object, int a_change)
	{
		{
			std::scoped_lock guard(lock);
			if (!live.contains(a_object) && !views.contains(a_object)) {
				return;
			}
		}
		auto             key = RefCaller();
		std::scoped_lock guard(lock);
		refNet[a_object][key] += a_change;
	}

	ULONG STDMETHODCALLTYPE AddRefHook(IUnknown* a_object)
	{
		NoteRef(a_object, 1);
		return FunctionsOf(a_object).addRef(a_object);
	}

	ULONG STDMETHODCALLTYPE ReleaseHook(IUnknown* a_object)
	{
		const auto functions = FunctionsOf(a_object);
		NoteRef(a_object, -1);
		return functions.release(a_object);
	}

	void WatchRefs(IUnknown* a_object)
	{
		auto**           vtable = *reinterpret_cast<void***>(a_object);
		std::scoped_lock guard(refFunctionsLock);
		if (std::ranges::find(refFunctions, vtable, &RefFunctions::vtable) != refFunctions.end()) {
			return;
		}
		RefFunctions functions{ vtable };
		functions.addRef = reinterpret_cast<decltype(functions.addRef)>(vtable[1]);
		functions.release = reinterpret_cast<decltype(functions.release)>(vtable[2]);
		refFunctions.push_back(functions);
		DWORD old = 0;
		::VirtualProtect(&vtable[1], 2 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
		vtable[1] = reinterpret_cast<void*>(&AddRefHook);
		vtable[2] = reinterpret_cast<void*>(&ReleaseHook);
		::VirtualProtect(&vtable[1], 2 * sizeof(void*), old, &old);
	}
}

namespace
{
	// Every 8-byte aligned place in readable committed memory holding one of a_values; at most
	// a_max per value. SEH: a region can be freed while it is read.
	int ScanRegion(const std::uintptr_t* a_begin, std::size_t a_count, const std::uintptr_t* a_values, int a_valueCount, std::uintptr_t* a_hits, int* a_hitCounts, int a_max)
	{
		int found = 0;
		__try {
			for (std::size_t i = 0; i < a_count; ++i) {
				const auto q = a_begin[i];
				for (int v = 0; v < a_valueCount; ++v) {
					if (q == a_values[v] && a_hitCounts[v] < a_max) {
						a_hits[v * a_max + a_hitCounts[v]++] = reinterpret_cast<std::uintptr_t>(&a_begin[i]);
						++found;
					}
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
		return found;
	}

	bool ReadQword(std::uintptr_t a_address, std::uintptr_t& a_value)
	{
		__try {
			a_value = *reinterpret_cast<const std::uintptr_t*>(a_address);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// The class of the object a vtable pointer belongs to (MSVC RTTI), or empty.
	std::string ClassOf(std::uintptr_t a_vtable, const std::vector<Module>& a_modules)
	{
		const auto inImage = [&](std::uintptr_t a_address) -> const Module* {
			for (const auto& module : a_modules) {
				if (a_address >= module.begin && a_address < module.end) {
					return &module;
				}
			}
			return nullptr;
		};
		const auto* image = inImage(a_vtable);
		std::uintptr_t locator = 0;
		if (!image || !ReadQword(a_vtable - 8, locator) || !inImage(locator)) {
			return {};
		}
		std::uintptr_t head = 0;
		if (!ReadQword(locator, head) || static_cast<std::uint32_t>(head) != 1) {
			return {};
		}
		std::uintptr_t rvas = 0;
		if (!ReadQword(locator + 12, rvas)) {
			return {};
		}
		const auto* locatorImage = inImage(locator);
		const auto  descriptor = locatorImage->begin + static_cast<std::uint32_t>(rvas);
		if (!inImage(descriptor)) {
			return {};
		}
		const char* name = reinterpret_cast<const char*>(descriptor + 16);
		if (std::strncmp(name, ".?A", 3) != 0) {
			return {};
		}
		return std::format("{} ({})", std::string(name, strnlen(name, 120)), image->name);
	}
}

namespace Bridge::GpuTrack
{
	void Start()
	{
		if (started) {
			return;
		}
		auto* context = reinterpret_cast<ID3D11DeviceContext*>(RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context);
		ID3D11Device* device = nullptr;
		context->GetDevice(&device);
		auto** vtable = *reinterpret_cast<void***>(device);
		createBuffer = Patch(vtable, 3, &CreateBufferHook);
		createTexture2D = Patch(vtable, 5, &CreateTexture2DHook);
		createTexture3D = Patch(vtable, 6, &CreateTexture3DHook);
		createView = Patch(vtable, 7, &CreateViewHook);
		device->Release();
		REL::Relocation<std::uintptr_t> renderer{ REL::ID(410530) };  // AE only: the tests' game
		auto*                           rendererObject = *reinterpret_cast<void***>(renderer.address());
		makeSource = Patch(reinterpret_cast<void**>(*rendererObject), 0xD0 / 8, &MakeSourceHook);
		addRefTexture = Patch(reinterpret_cast<void**>(*rendererObject), 0xD8 / 8, &AddRefTextureHook);
		releaseTexture = Patch(reinterpret_cast<void**>(*rendererObject), 0xE0 / 8, &ReleaseTextureHook);
		REL::Relocation<std::uintptr_t> sourceVtable{ RE::VTABLE_NiSourceTexture[0] };
		sourceDtor = Patch(reinterpret_cast<void**>(sourceVtable.address()), 0, &SourceDtorHook);
		started = true;
		logger::info("bridge: GPU resources tracked from now on");
	}

	// The device's resources made since a_since for an engine texture already destroyed: what still
	// keeps them.
	json OrphanHolders(std::uint64_t a_since, int a_samples)
	{
		std::vector<std::pair<std::uint64_t, Microsoft::WRL::ComPtr<ID3D11DeviceChild>>> candidates;
		{
			std::scoped_lock guard(lock);
			for (const auto& [resource, entry] : live) {
				if (entry.seq >= a_since && entry.source && !sources.contains(entry.source)) {
					candidates.emplace_back(entry.bytes, static_cast<ID3D11DeviceChild*>(const_cast<void*>(resource)));
				}
			}
		}
		std::ranges::sort(candidates, [](const auto& a, const auto& b) { return a.first > b.first; });
		if (static_cast<int>(candidates.size()) > a_samples) {
			candidates.resize(a_samples);
		}
		std::vector<std::uintptr_t> values;
		for (auto& [_, resource] : candidates) {
			values.push_back(reinterpret_cast<std::uintptr_t>(resource.Get()));
		}
		json result = json::array();
		const auto holders = Scan(values);
		for (std::size_t v = 0; v < values.size(); ++v) {
			const ULONG refs = (candidates[v].second->AddRef(), candidates[v].second->Release());
			json  refInfo;
			{
				std::scoped_lock guard(lock);
				const auto       it = live.find(candidates[v].second.Get());
				void*            rendererTexture = it != live.end() ? it->second.rendererTexture : nullptr;
				if (rendererTexture) {
					refInfo["count"] = *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::uintptr_t>(rendererTexture) + 0x20);
					const auto& log = refLogs[rendererTexture];
					refInfo["added"] = log.added;
					refInfo["released"] = log.released;
				}
			}
			json comRefs = json::object();
			{
				std::scoped_lock guard(lock);
				const auto       net = [](const std::unordered_map<std::string, int>& a_log) {
                    json kept = json::object();
                    for (const auto& [key, count] : a_log) {
                        if (count != 0) {
                            kept[key] = count;
                        }
                    }
                    return kept;
				};
				comRefs["texture"] = net(refNet[candidates[v].second.Get()]);
				for (const auto& [view, resource] : views) {
					if (resource == candidates[v].second.Get()) {
						auto*       unknown = static_cast<IUnknown*>(const_cast<void*>(view));
						unknown->AddRef();
						const ULONG count = unknown->Release();
						comRefs[std::format("view {:X} external refs {}", reinterpret_cast<std::uintptr_t>(view), count)] = net(refNet[view]);
					}
				}
			}
			result.push_back({ { "resource", std::format("{:X}", values[v]) }, { "refs", refs - 1 }, { "comRefs", comRefs }, { "mb", candidates[v].first / 1048576.0 }, { "rendererTexture", refInfo }, { "holders", holders[v] } });
		}
		return result;
	}

	json Holders(std::uint64_t a_since, int a_samples)
	{
		// Engine textures made since the mark and still there, the largest first.
		std::vector<std::pair<std::uint64_t, RE::NiPointer<RE::NiSourceTexture>>> candidates;
		{
			std::scoped_lock guard(lock);
			for (const auto& [_, resource] : live) {
				if (resource.seq >= a_since && resource.source && sources.contains(resource.source)) {
					candidates.emplace_back(resource.bytes, RE::NiPointer<RE::NiSourceTexture>(resource.source));
				}
			}
		}
		std::ranges::sort(candidates, [](const auto& a, const auto& b) { return a.first > b.first; });
		if (static_cast<int>(candidates.size()) > a_samples) {
			candidates.resize(a_samples);
		}
		std::vector<std::uintptr_t> values;
		for (auto& [_, texture] : candidates) {
			values.push_back(reinterpret_cast<std::uintptr_t>(texture.get()));
		}
		const auto holders = Scan(values);
		json       result = json::array();
		for (std::size_t v = 0; v < values.size(); ++v) {
			result.push_back({ { "address", std::format("{:X}", values[v]) }, { "texture", candidates[v].second->name.c_str() }, { "refs", candidates[v].second->GetRefCount() - 1 }, { "mb", candidates[v].first / 1048576.0 }, { "holders", holders[v] } });
		}
		return result;
	}

	std::vector<json> Scan(const std::vector<std::uintptr_t>& values)
	{
		constexpr int              kMax = 24;
		std::vector<std::uintptr_t> hits(values.size() * kMax);
		std::vector<int>            hitCounts(values.size());
		MEMORY_BASIC_INFORMATION    info{};
		for (std::uintptr_t address = 0x10000; ::VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)); address = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize) {
			const bool readable = info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
			                      (info.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
			if (readable && info.Type == MEM_PRIVATE) {
				ScanRegion(reinterpret_cast<const std::uintptr_t*>(info.BaseAddress), info.RegionSize / 8, values.data(), static_cast<int>(values.size()), hits.data(), hitCounts.data(), kMax);
			}
			if (reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize >= 0x7FFFFFFF0000) {
				break;
			}
		}
		const auto        modules = Modules();
		std::vector<json> result;
		for (std::size_t v = 0; v < values.size(); ++v) {
			json holders = json::array();
			for (int h = 0; h < hitCounts[v]; ++h) {
				const auto  at = hits[v * kMax + h];
				std::string owner;
				std::uintptr_t offset = 0;
				// The object holding it starts at the nearest vtable pointer before it.
				for (std::uintptr_t back = 0; back <= 0x400 && owner.empty(); back += 8) {
					std::uintptr_t q = 0;
					if (!ReadQword(at - back, q)) {
						break;
					}
					owner = ClassOf(q, modules);
					offset = back;
				}
				holders.push_back(owner.empty() ? std::format("{:X}: no object found", at) : std::format("{:X}: {} +{:X} (object {:X})", at, owner, offset, at - offset));
			}
			result.push_back(holders);
		}
		return result;
	}

	std::uint64_t Mark()
	{
		return nextSeq.load();
	}

	json Live(std::uint64_t a_since, int a_groups, int a_frames)
	{
		const auto modules = Modules();
		const auto describe = [&](void* a_address) -> std::optional<std::pair<std::string, std::uintptr_t>> {
			const auto address = reinterpret_cast<std::uintptr_t>(a_address);
			for (const auto& module : modules) {
				if (address >= module.begin && address < module.end) {
					return std::pair{ module.name, address - module.begin };
				}
			}
			return std::nullopt;
		};
		struct Group
		{
			std::uint64_t bytes = 0;
			std::uint64_t count = 0;
			std::uint64_t sourceAlive = 0;  // of its engine texture still there
			std::uint64_t sourceGone = 0;
			std::string   example;
			std::vector<std::string> names;
		};
		std::unordered_map<std::string, Group> groups;
		std::uint64_t                          total = 0, totalCount = 0;
		{
			std::scoped_lock guard(lock);
			for (const auto& [_, resource] : live) {
				if (resource.seq < a_since) {
					continue;
				}
				std::string key;
				int         taken = 0;
				for (std::uint16_t i = 0; i < resource.depth && taken < a_frames; ++i) {
					const auto where = describe(resource.stack[i]);
					if (!where) {
						key += "? <- ";
						++taken;
						continue;
					}
					if (taken == 0 && Plumbing(where->first)) {
						continue;
					}
					key += std::format("{}+{:X} <- ", where->first, where->second);
					++taken;
				}
				auto& group = groups[key];
				group.bytes += resource.bytes;
				++group.count;
				group.example = resource.kind;
				if (resource.source) {
					if (sources.contains(resource.source)) {
						++group.sourceAlive;
						if (group.names.size() < 6) {
							group.names.push_back(std::format("{} (refs {})", resource.source->name.c_str(), resource.source->GetRefCount()));
						}
					} else {
						++group.sourceGone;
					}
				}
				total += resource.bytes;
				++totalCount;
			}
		}
		std::vector<std::pair<std::string, Group>> sorted(groups.begin(), groups.end());
		std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
		json list = json::array();
		for (const auto& [key, group] : sorted) {
			if (static_cast<int>(list.size()) >= a_groups) {
				break;
			}
			list.push_back({ { "mb", group.bytes / 1048576.0 }, { "count", group.count }, { "sourceAlive", group.sourceAlive }, { "sourceGone", group.sourceGone }, { "names", group.names }, { "example", group.example }, { "stack", key } });
		}
		return { { "mark", Mark() }, { "since", a_since }, { "mb", total / 1048576.0 }, { "count", totalCount }, { "groups", list } };
	}
}
