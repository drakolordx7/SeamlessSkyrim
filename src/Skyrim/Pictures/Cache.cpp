#include "Skyrim/Pictures/Cache.h"

#include "Core/Pictures/Budget.h"
#include "Core/Pictures/FileFormat.h"
#include "Skyrim/Engine/Paths.h"
#include "Skyrim/Render/D3D.h"

#include <DirectXPackedVector.h>

namespace Pictures::Cache
{
	namespace
	{
		namespace Files = Core::Pictures::Files;

		std::unordered_map<std::string, Picture> pictures;
		std::unordered_map<std::string, Layered> layered;
		std::vector<Readback>                    readbacks;

		// Set by DeleteAll on the menu's thread; the pictures and readbacks are the main thread's.
		std::atomic<bool> forgetAllWanted;

		// "Skyrim.esm|01A6FB": the same door whatever the load order (as the door corrections).
		std::string DoorKey(RE::FormID a_door)
		{
			auto* form = RE::TESForm::LookupByID(a_door);
			auto* file = form ? form->GetFile(0) : nullptr;
			if (!file) {
				return Core::Pictures::DoorKeyWithoutPlugin(a_door);
			}
			return Core::Pictures::DoorKey(file->GetFilename(), a_door, file->IsLight());
		}

		std::filesystem::path Folder()
		{
			return Paths::PluginDirectory() / "SeamlessSkyrim_Doorways";
		}

		std::filesystem::path FileOf(const std::string& a_key, bool a_layered)
		{
			return Folder() / Paths::Widen(Core::Pictures::FileName(a_key, LoadOrder(), a_layered));
		}

		// The largest a picture or layer was ever written at (the largest size setting): no more than
		// that is read, a damaged file's header asked for gigabytes of texels before its read could fail.
		std::pair<std::uint32_t, std::uint32_t> Largest(bool a_layered)
		{
			if (a_layered) {
				return Layers::SizeOf(kMaxHeight);
			}
			return Camera::SizeFor(Camera::kRoomCamera.extent, kMaxHeight, Camera::kWidest);  // the room's is the wider
		}

		// The brightness of a picture's texels (RGBA16F, alpha how far behind the doorway): every
		// fourth row and column is plenty for a geometric mean.
		float Brightness(const void* a_texels, UINT a_width, UINT a_height, std::size_t a_rowPitch, float a_lightScale)
		{
			using DirectX::PackedVector::XMConvertHalfToFloat;
			constexpr UINT                 kStride = 4;
			Core::Pictures::BrightnessMean mean;
			for (UINT y = 0; y < a_height; y += kStride) {
				const auto* row = reinterpret_cast<const std::uint16_t*>(static_cast<const char*>(a_texels) + y * a_rowPitch);
				for (UINT x = 0; x < a_width; x += kStride) {
					const auto* texel = row + x * 4;
					// Nothing there (no wall of the room's beyond the capture camera's edge, the sky): not part of it.
					if (XMConvertHalfToFloat(texel[3]) >= Core::Pictures::kNothingThere) {
						continue;
					}
					mean.Add(Core::Pictures::Luminance(XMConvertHalfToFloat(texel[0]), XMConvertHalfToFloat(texel[1]), XMConvertHalfToFloat(texel[2])));
				}
			}
			return mean.Result(a_lightScale);
		}

		// Pictures of doors not looked at for a while are let go; they are on disk for the next time.
		// Kept for the whole session, every door ever passed held its texture: a walk through a city
		// at the largest size took gigabytes of video memory.
		void ForgetUnused()
		{
			static std::chrono::steady_clock::time_point lookedAt;
			const auto                                   now = std::chrono::steady_clock::now();
			if (now - lookedAt < Core::Pictures::kLookedAtEvery) {
				return;
			}
			lookedAt = now;
			const auto beingWritten = [&](const std::string& a_key) { return std::ranges::contains(readbacks, a_key, &Readback::key); };
			std::erase_if(pictures, [&](auto& a_entry) {
				auto& picture = a_entry.second;
				// One just taken is still being read back for its file.
				if (!Core::Pictures::ShouldForget(now - picture.used, beingWritten(a_entry.first))) {
					return false;
				}
				ReleaseTexture(picture);
				return true;
			});
			std::erase_if(layered, [&](auto& a_entry) {
				auto& entry = a_entry.second;
				if (!Core::Pictures::ShouldForget(now - entry.used, beingWritten(a_entry.first))) {
					return false;
				}
				ReleaseLayered(entry);
				return true;
			});
		}

		// After DeleteAll: kept in memory, a door went on showing its old picture, and a readback still
		// in flight wrote its file back.
		void ForgetAllIfDeleted()
		{
			if (!forgetAllWanted.exchange(false)) {
				return;
			}
			for (auto& [key, picture] : pictures) {
				ReleaseTexture(picture);
			}
			pictures.clear();
			for (auto& [key, entry] : layered) {
				ReleaseLayered(entry);
			}
			layered.clear();
			for (auto& readback : readbacks) {
				readback.staging->Release();
			}
			readbacks.clear();
		}

		// Its file's last use, so that the budget lets go of what is not looked at.
		void Touch(const std::filesystem::path& a_path)
		{
			std::error_code error;
			std::filesystem::last_write_time(a_path, std::filesystem::file_time_type::clock::now(), error);
		}

		void LoadImage(ID3D11Device* a_device, const std::string& a_key, Picture& a_picture, bool a_outdoors)
		{
			const auto    path = FileOf(a_key, false);
			std::ifstream file(path, std::ios::binary);
			Files::ImageHeader header;
			const auto [widest, highest] = Largest(false);
			if (!file.read(reinterpret_cast<char*>(&header), sizeof(header)) || !Files::Valid(header, LoadOrder(), widest, highest)) {
				return;
			}
			std::vector<std::uint16_t> texels(Files::TexelBytes(header) / sizeof(std::uint16_t));
			if (!file.read(reinterpret_cast<char*>(texels.data()), texels.size() * sizeof(std::uint16_t)) || !MakeView(a_device, a_picture, texels.data(), header.width, header.height)) {
				return;
			}
			a_picture.takenFrom = { header.takenFrom[0], header.takenFrom[1], header.takenFrom[2] };
			std::ranges::copy(header.extent, a_picture.extent.begin());
			a_picture.lightScale = header.lightScale;
			a_picture.brightness = header.brightness;
			(void)a_outdoors;
			Touch(path);
		}

		void LoadLayered(ID3D11Device* a_device, const std::string& a_key, Layered& a_layered)
		{
			const auto    path = FileOf(a_key, true);
			std::ifstream file(path, std::ios::binary);
			Files::LayeredHeader header;
			const auto [widest, highest] = Largest(true);
			if (!file.read(reinterpret_cast<char*>(&header), sizeof(header)) || !Files::Valid(header, LoadOrder(), widest, highest)) {
				return;
			}
			std::vector<std::uint16_t> texels(Files::TexelBytes(header) / sizeof(std::uint16_t));
			if (!file.read(reinterpret_cast<char*>(texels.data()), texels.size() * sizeof(std::uint16_t)) ||
				!MakeLayeredView(a_device, a_layered, texels.data(), header.width, header.height, static_cast<int>(header.layers))) {
				return;
			}
			for (int i = 0; i < Layers::kMaxLayers; ++i) {
				a_layered.places[i] = { header.places[i][0], header.places[i][1], header.places[i][2] };
			}
			a_layered.captured = (1u << header.layers) - 1u;
			a_layered.lightScale = header.lightScale;
			a_layered.brightness = header.brightness;
			Touch(path);
		}
	}

	int PartOfDay()
	{
		auto* calendar = RE::Calendar::GetSingleton();
		return Core::Pictures::PartOfDay(calendar ? calendar->GetHour() : 12.0f);
	}

	std::string KeyOf(RE::FormID a_door, bool a_outdoors, int a_part)
	{
		return Core::Pictures::KeyOf(DoorKey(a_door), a_outdoors, a_part);
	}

	std::uint64_t LoadOrder()
	{
		static std::optional<std::uint64_t> hash;
		if (!hash) {
			auto*                    handler = RE::TESDataHandler::GetSingleton();
			std::vector<std::string> names;
			const auto*              mods = handler->GetLoadedMods();
			for (int i = 0; i < handler->GetLoadedModCount(); ++i) {
				names.emplace_back(mods[i]->GetFilename());
			}
			const auto* lights = handler->GetLoadedLightMods();
			for (int i = 0; i < handler->GetLoadedLightModCount(); ++i) {
				names.emplace_back(lights[i]->GetFilename());
			}
			hash = Core::Pictures::LoadOrderHash(names);
			logger::info("doorway pictures: load order of {} plugins, hash {:08x}", names.size(), static_cast<std::uint32_t>(*hash));
		}
		return *hash;
	}

	Picture& Entry(const std::string& a_key)
	{
		return pictures[a_key];
	}

	bool MakeView(ID3D11Device* a_device, Picture& a_picture, const void* a_texels, UINT a_width, UINT a_height)
	{
		D3D11_TEXTURE2D_DESC desc{ a_width, a_height, 0, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0,
			D3D11_RESOURCE_MISC_GENERATE_MIPS };
		if (FAILED(a_device->CreateTexture2D(&desc, nullptr, &a_picture.texture))) {
			return false;
		}
		if (FAILED(a_device->CreateShaderResourceView(a_picture.texture, nullptr, &a_picture.view))) {
			// Kept without its view, the picture counted as never taken (Capture's retake test asks
			// for the view) while its texture stopped MakeView being tried again: captured, written
			// and logged every frame.
			Render::D3D::Release(a_picture.texture);
			return false;
		}
		if (a_texels) {
			auto* context = Render::D3D::Context();
			context->UpdateSubresource(a_picture.texture, 0, nullptr, a_texels, a_width * Camera::kBytesPerTexel, 0);
			context->GenerateMips(a_picture.view);
		}
		a_picture.width = a_width;
		a_picture.height = a_height;
		return true;
	}

	void ReleaseTexture(Picture& a_picture)
	{
		Render::D3D::Release(a_picture.view);
		Render::D3D::Release(a_picture.texture);
	}

	Picture* FindExact(ID3D11Device* a_device, RE::FormID a_door, bool a_outdoors)
	{
		ForgetAllIfDeleted();
		ForgetUnused();
		EnforceBudget();
		const auto key = KeyOf(a_door, a_outdoors, a_outdoors ? PartOfDay() : 0);
		auto&      picture = pictures[key];
		picture.used = std::chrono::steady_clock::now();
		if (!picture.view && !std::exchange(picture.diskTried, true)) {
			LoadImage(a_device, key, picture, a_outdoors);
		}
		return picture.view ? &picture : nullptr;
	}

	Picture* Find(ID3D11Device* a_device, RE::FormID a_door, bool a_outdoors)
	{
		ForgetAllIfDeleted();
		ForgetUnused();
		EnforceBudget();
		const int  part = PartOfDay();
		const auto order = Core::Pictures::SearchOrder(part);
		for (int i = 0; i < (a_outdoors ? 4 : 1); ++i) {
			const auto key = KeyOf(a_door, a_outdoors, order[i]);
			auto&      picture = pictures[key];
			picture.used = std::chrono::steady_clock::now();
			if (!picture.view && !std::exchange(picture.diskTried, true)) {
				// Whatever size it was taken at: after the size is changed a door shows its old picture
				// until it is taken again, rather than the plain colour.
				LoadImage(a_device, key, picture, a_outdoors);
			}
			if (picture.view) {
				return &picture;
			}
		}
		return nullptr;
	}

	Layered& EntryLayered(const std::string& a_key)
	{
		return layered[a_key];
	}

	bool MakeLayeredView(ID3D11Device* a_device, Layered& a_layered, const void* a_texels, UINT a_width, UINT a_height, int a_layers)
	{
		D3D11_TEXTURE2D_DESC desc{ a_width, a_height, 0, static_cast<UINT>(a_layers), DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, D3D11_RESOURCE_MISC_GENERATE_MIPS };
		if (FAILED(a_device->CreateTexture2D(&desc, nullptr, &a_layered.texture))) {
			return false;
		}
		if (FAILED(a_device->CreateShaderResourceView(a_layered.texture, nullptr, &a_layered.view))) {
			Render::D3D::Release(a_layered.texture);
			return false;
		}
		a_layered.width = a_width;
		a_layered.height = a_height;
		a_layered.layers = a_layers;
		if (a_texels) {
			auto*                context = Render::D3D::Context();
			D3D11_TEXTURE2D_DESC made{};
			a_layered.texture->GetDesc(&made);
			const auto layerBytes = std::size_t{ a_width } * a_height * Camera::kBytesPerTexel;
			for (int layer = 0; layer < a_layers; ++layer) {
				context->UpdateSubresource(a_layered.texture, D3D11CalcSubresource(0, layer, made.MipLevels), nullptr, static_cast<const char*>(a_texels) + layer * layerBytes,
					a_width * Camera::kBytesPerTexel, 0);
			}
			context->GenerateMips(a_layered.view);
		}
		return true;
	}

	void ReleaseLayered(Layered& a_layered)
	{
		Render::D3D::Release(a_layered.view);
		Render::D3D::Release(a_layered.texture);
		a_layered.captured = 0;
	}

	Layered* FindLayered(ID3D11Device* a_device, RE::FormID a_door, bool a_outdoors)
	{
		ForgetAllIfDeleted();
		ForgetUnused();
		EnforceBudget();
		const auto key = KeyOf(a_door, a_outdoors, a_outdoors ? PartOfDay() : 0);
		auto&      entry = layered[key];
		entry.used = std::chrono::steady_clock::now();
		if (!entry.view && !std::exchange(entry.diskTried, true)) {
			LoadLayered(a_device, key, entry);
		}
		return entry.view ? &entry : nullptr;
	}

	void Queue(Readback a_readback)
	{
		readbacks.push_back(std::move(a_readback));
	}

	bool HasReadbacks()
	{
		return !readbacks.empty();
	}

	void WriteReadbacks(ID3D11DeviceContext* a_context)
	{
		ForgetAllIfDeleted();
		bool wrote = false;
		for (auto it = readbacks.begin(); it != readbacks.end();) {
			if (++it->frames < 3) {
				++it;
				continue;
			}
			const bool         isLayered = it->layers > 0;
			const int          count = isLayered ? it->layers : 1;
			D3D11_MAPPED_SUBRESOURCE mapped[Layers::kMaxLayers]{};
			HRESULT                  result = S_OK;
			int                      mappedCount = 0;
			for (int layer = 0; layer < count && SUCCEEDED(result); ++layer) {
				result = a_context->Map(it->staging, layer, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped[layer]);
				mappedCount += SUCCEEDED(result);
			}
			if (result == DXGI_ERROR_WAS_STILL_DRAWING && it->frames < 60) {
				for (int layer = 0; layer < mappedCount; ++layer) {
					a_context->Unmap(it->staging, layer);
				}
				++it;
				continue;
			}
			if (SUCCEEDED(result)) {
				const auto path = FileOf(it->key, isLayered);
				std::error_code error;
				std::filesystem::create_directories(path.parent_path(), error);
				std::ofstream file(path, std::ios::binary | std::ios::trunc);
				// The first (the middle view of a depth image) is what its brightness is read off.
				const float brightness = Brightness(mapped[0].pData, it->width, it->height, mapped[0].RowPitch, it->lightScale);
				if (isLayered) {
					Files::LayeredHeader header;
					header.width = it->width;
					header.height = it->height;
					header.layers = static_cast<std::uint32_t>(it->layers);
					header.loadOrder = LoadOrder();
					header.lightScale = it->lightScale;
					header.brightness = brightness;
					for (int i = 0; i < Layers::kMaxLayers; ++i) {
						std::ranges::copy(it->places[i], header.places[i]);
					}
					file.write(reinterpret_cast<const char*>(&header), sizeof(header));
				} else {
					Files::ImageHeader header;
					header.width = it->width;
					header.height = it->height;
					header.loadOrder = LoadOrder();
					std::ranges::copy(it->takenFrom, header.takenFrom);
					header.lightScale = it->lightScale;
					std::ranges::copy(it->extent, header.extent);
					header.brightness = brightness;
					file.write(reinterpret_cast<const char*>(&header), sizeof(header));
				}
				for (int layer = 0; layer < count; ++layer) {
					for (UINT row = 0; row < it->height; ++row) {
						file.write(static_cast<const char*>(mapped[layer].pData) + row * mapped[layer].RowPitch, it->width * Camera::kBytesPerTexel);
					}
				}
				// Taken again meanwhile: the newer one's texels decide.
				if (isLayered) {
					const auto found = layered.find(it->key);
					if (found != layered.end() && found->second.taken == it->taken) {
						found->second.brightness = brightness;
					}
				} else {
					const auto found = pictures.find(it->key);
					if (found != pictures.end() && found->second.taken == it->taken) {
						found->second.brightness = brightness;
					}
				}
				for (int layer = 0; layer < count; ++layer) {
					a_context->Unmap(it->staging, layer);
				}
				wrote = true;
				if (!file) {
					logger::warn("doorway snapshot: could not write {}", Paths::Narrow(path.wstring()));
				}
			} else {
				for (int layer = 0; layer < mappedCount; ++layer) {
					a_context->Unmap(it->staging, layer);
				}
			}
			it->staging->Release();
			it = readbacks.erase(it);
		}
		if (wrote) {
			EnforceBudget();
		}
	}

	std::optional<float> RoomBrightness(RE::FormID a_door)
	{
		const auto* picture = FindExact(Render::D3D::Device(), a_door, false);
		if (picture && picture->brightness > 0.0f) {
			return picture->brightness;
		}
		// A room that never had its picture taken: as bright as the rooms that have, rather than as
		// from inside.
		std::vector<float> known;
		for (const auto& [key, other] : pictures) {
			if (other.brightness > 0.0f && key.ends_with("#inside")) {
				known.push_back(other.brightness);
			}
		}
		return known.empty() ? std::nullopt : std::optional(Core::Pictures::GeometricMean(known));
	}

	float PictureBrightness(RE::FormID a_door, bool a_outdoors, bool a_layered)
	{
		if (a_layered) {
			const auto* entry = FindLayered(Render::D3D::Device(), a_door, a_outdoors);
			return entry ? entry->brightness : 0.0f;
		}
		const auto* picture = Find(Render::D3D::Device(), a_door, a_outdoors);
		return picture ? picture->brightness : 0.0f;
	}

	int DeleteAll()
	{
		forgetAllWanted = true;
		int             deleted = 0;
		std::error_code error;
		for (const auto& entry : std::filesystem::directory_iterator(Folder(), error)) {
			// One being read this moment stays (Windows won't delete an open file); it is a valid
			// picture, only older.
			std::error_code removeError;
			const auto      extension = entry.path().extension();
			if ((extension == L".ssdw" || extension == L".ssdl") && std::filesystem::remove(entry.path(), removeError)) {
				++deleted;
			}
		}
		logger::info("doorway snapshot: {} pictures deleted", deleted);
		return deleted;
	}

	void EnforceBudget()
	{
		static std::optional<std::chrono::steady_clock::time_point> checked;
		const auto                                                  now = std::chrono::steady_clock::now();
		if (checked && now - *checked < std::chrono::seconds(30)) {
			return;
		}
		checked = now;
		std::vector<std::filesystem::path> paths;
		std::vector<Core::Pictures::Budget::File> files;
		std::error_code                           error;
		for (const auto& entry : std::filesystem::directory_iterator(Folder(), error)) {
			const auto extension = entry.path().extension();
			if (extension != L".ssdw" && extension != L".ssdl") {
				continue;
			}
			std::error_code      removeError;
			// The naming before the load order was in it: never read again.
			if (entry.path().stem().wstring().find(L'@') == std::wstring::npos) {
				std::filesystem::remove(entry.path(), removeError);
				continue;
			}
			std::error_code sizeError;
			const auto      bytes = entry.file_size(sizeError);
			const auto      time = entry.last_write_time(sizeError);
			paths.push_back(entry.path());
			files.push_back({ sizeError ? 0 : bytes, time.time_since_epoch().count() });
		}
		const auto victims = Core::Pictures::Budget::Evict(files, CacheBudgetBytes());
		for (const auto index : victims) {
			std::error_code removeError;
			std::filesystem::remove(paths[index], removeError);
		}
		if (!victims.empty()) {
			logger::info("doorway pictures: {} files deleted to keep to the {} MB budget", victims.size(), CacheBudgetBytes() >> 20);
		}
	}

	Counts Count()
	{
		std::size_t inMemory = 0, layeredCount = 0;
		for (const auto& [key, picture] : pictures) {
			inMemory += picture.view ? 1 : 0;
		}
		for (const auto& [key, entry] : layered) {
			layeredCount += entry.view ? 1 : 0;
		}
		return { inMemory, layeredCount, readbacks.size() };
	}
}
