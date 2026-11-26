#include "Skyrim/Pictures/Cache.h"

#include "Core/Pictures/Bc6h.h"
#include "Core/Pictures/Budget.h"
#include "Core/Pictures/FileFormat.h"
#include "Skyrim/Engine/Paths.h"
#include "Skyrim/Render/D3D.h"

namespace Pictures::Cache
{
	namespace
	{
		namespace Files = Core::Pictures::Files;
		namespace Z = Core::Pictures::Compress;

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

		// The brightness of a picture, read off its compressed colour (BC6H mode 11, which is all our
		// compressor makes) and its distances: every fourth row and column is plenty for a geometric
		// mean, and the first texel of a block is one of them.
		float Brightness(const std::uint8_t* a_blocks, const std::uint16_t* a_distances, UINT a_width, UINT a_height, float a_lightScale)
		{
			Core::Pictures::BrightnessMean mean;
			const UINT                     across = Z::Blocks(a_width);
			for (UINT by = 0; by < Z::Blocks(a_height); ++by) {
				for (UINT bx = 0; bx < across; ++bx) {
					const UINT x = bx * 4, y = by * 4;
					if (x >= a_width || y >= a_height) {
						continue;
					}
					// Nothing there (no wall of the room's beyond the capture camera's edge, the sky): not part of it.
					if (Core::Pictures::Bc6h::HalfValue(a_distances[static_cast<std::size_t>(y) * a_width + x]) >= Core::Pictures::kNothingThere) {
						continue;
					}
					Core::Pictures::Bc6h::Block block;
					std::memcpy(block.bytes.data(), a_blocks + (static_cast<std::size_t>(by) * across + bx) * Z::kBlockBytes, Z::kBlockBytes);
					const auto texel = Core::Pictures::Bc6h::Decode(block)[0];
					mean.Add(Core::Pictures::Luminance(texel[0], texel[1], texel[2]));
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
				readback.colour->Release();
				readback.distance->Release();
			}
			readbacks.clear();
		}

		// Its file's last use, so that the budget lets go of what is not looked at.
		void Touch(const std::filesystem::path& a_path)
		{
			std::error_code error;
			std::filesystem::last_write_time(a_path, std::filesystem::file_time_type::clock::now(), error);
		}

		// A file's body: the colour chain and distances after the header.
		std::vector<std::byte> ReadBody(std::ifstream& a_file, std::uint64_t a_bytes)
		{
			std::vector<std::byte> body(a_bytes);
			if (!a_file.read(reinterpret_cast<char*>(body.data()), static_cast<std::streamsize>(a_bytes))) {
				body.clear();
			}
			return body;
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
			const auto body = ReadBody(file, Files::TexelBytes(header));
			if (body.empty() || !MakeView(a_device, a_picture, body.data(), header.width, header.height)) {
				return;
			}
			a_picture.takenFrom = { header.takenFrom[0], header.takenFrom[1], header.takenFrom[2] };
			std::ranges::copy(header.extent, a_picture.extent.begin());
			a_picture.lightScale = header.lightScale;
			a_picture.brightness = header.brightness;
			a_picture.source = static_cast<Source>(header.source);
			a_picture.takenAt = header.takenAt;
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
			const auto body = ReadBody(file, Files::TexelBytes(header));
			if (body.empty() || !MakeLayeredView(a_device, a_layered, body.data(), header.width, header.height, static_cast<int>(header.layers))) {
				return;
			}
			for (int i = 0; i < Layers::kMaxLayers; ++i) {
				a_layered.places[i] = { header.places[i][0], header.places[i][1], header.places[i][2] };
			}
			a_layered.captured = (1u << header.layers) - 1u;
			a_layered.lightScale = header.lightScale;
			a_layered.brightness = header.brightness;
			a_layered.source = static_cast<Source>(header.source);
			a_layered.takenAt = header.takenAt;
			Touch(path);
		}

		// A picture's textures: BC6H colour of a_layers slices with their mips, the distances beside it. a_file
		// holds each slice's colour chain then distances (Files), or nothing.
		bool MakeTextures(ID3D11Device* a_device, UINT a_width, UINT a_height, UINT a_layers, const void* a_file, ID3D11Texture2D*& a_colour, ID3D11ShaderResourceView*& a_colourView,
			ID3D11Texture2D*& a_distance, ID3D11ShaderResourceView*& a_distanceView, bool a_array)
		{
			const UINT           mips = Z::MipCount(a_width, a_height);
			D3D11_TEXTURE2D_DESC colour{ a_width, a_height, mips, a_layers, DXGI_FORMAT_BC6H_UF16, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
			D3D11_TEXTURE2D_DESC distance{ a_width, a_height, 1, a_layers, DXGI_FORMAT_R16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
			std::vector<D3D11_SUBRESOURCE_DATA> colourData, distanceData;
			if (a_file) {
				const auto sliceBytes = Z::PictureBytes(a_width, a_height);
				for (UINT slice = 0; slice < a_layers; ++slice) {
					const auto*   base = static_cast<const std::byte*>(a_file) + slice * sliceBytes;
					std::uint64_t offset = 0;
					for (UINT level = 0; level < mips; ++level) {
						colourData.push_back({ base + offset, Z::Blocks(Z::Level(a_width, level)) * Z::kBlockBytes, 0 });
						offset += Z::ColourLevelBytes(a_width, a_height, level);
					}
					distanceData.push_back({ base + offset, a_width * Z::kDepthBytesPerTexel, 0 });
				}
			}
			// A depth image's textures are arrays whatever the number of layers (the shader reads an array).
			const auto view = [&](DXGI_FORMAT a_format, UINT a_levels) {
				D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
				desc.Format = a_format;
				desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray = { 0, a_levels, 0, a_layers };
				return desc;
			};
			const auto colourView = view(DXGI_FORMAT_BC6H_UF16, mips), distanceView = view(DXGI_FORMAT_R16_FLOAT, 1);
			const bool made = SUCCEEDED(a_device->CreateTexture2D(&colour, a_file ? colourData.data() : nullptr, &a_colour)) &&
			                  SUCCEEDED(a_device->CreateShaderResourceView(a_colour, a_array ? &colourView : nullptr, &a_colourView)) &&
			                  SUCCEEDED(a_device->CreateTexture2D(&distance, a_file ? distanceData.data() : nullptr, &a_distance)) &&
			                  SUCCEEDED(a_device->CreateShaderResourceView(a_distance, a_array ? &distanceView : nullptr, &a_distanceView));
			if (!made) {
				// Kept without its view, the picture counted as never taken (Capture's retake test asks
				// for the view) while its texture stopped MakeView being tried again: captured, written
				// and logged every frame.
				Render::D3D::Release(a_distanceView);
				Render::D3D::Release(a_distance);
				Render::D3D::Release(a_colourView);
				Render::D3D::Release(a_colour);
			}
			return made;
		}

		// The file of a picture or depth image as written: the header, then each slice's colour chain
		// and distances. Read back from the staging copies, a subresource at a time.
		struct Staged
		{
			std::vector<std::uint8_t>  firstColour;     // slice 0, level 0: the brightness is read off it
			std::vector<std::uint16_t> firstDistances;  // and slice 0's distances
		};

		// Writes the body of a_readback to a_file: false if the GPU hasn't finished with it yet.
		bool WriteBody(ID3D11DeviceContext* a_context, const Readback& a_readback, std::ofstream& a_file, Staged& a_staged)
		{
			const UINT slices = a_readback.layers > 0 ? static_cast<UINT>(a_readback.layers) : 1u;
			const UINT mips = Z::MipCount(a_readback.width, a_readback.height);
			for (UINT slice = 0; slice < slices; ++slice) {
				for (UINT level = 0; level < mips; ++level) {
					D3D11_MAPPED_SUBRESOURCE mapped{};
					// The first read says whether the copy is done; the rest of the same copy then are.
					const auto flags = slice == 0 && level == 0 ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0u;
					if (FAILED(a_context->Map(a_readback.colour, D3D11CalcSubresource(level, slice, mips), D3D11_MAP_READ, flags, &mapped))) {
						return false;
					}
					const UINT rows = Z::Blocks(Z::Level(a_readback.height, level));
					const UINT rowBytes = Z::Blocks(Z::Level(a_readback.width, level)) * Z::kBlockBytes;
					for (UINT row = 0; row < rows; ++row) {
						const auto* data = static_cast<const char*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch;
						a_file.write(data, rowBytes);
						if (slice == 0 && level == 0) {
							a_staged.firstColour.insert(a_staged.firstColour.end(), data, data + rowBytes);
						}
					}
					a_context->Unmap(a_readback.colour, D3D11CalcSubresource(level, slice, mips));
				}
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (FAILED(a_context->Map(a_readback.distance, slice, D3D11_MAP_READ, 0, &mapped))) {
					return false;
				}
				for (UINT row = 0; row < a_readback.height; ++row) {
					const auto* data = static_cast<const char*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch;
					a_file.write(data, a_readback.width * Z::kDepthBytesPerTexel);
					if (slice == 0) {
						const auto* texels = reinterpret_cast<const std::uint16_t*>(data);
						a_staged.firstDistances.insert(a_staged.firstDistances.end(), texels, texels + a_readback.width);
					}
				}
				a_context->Unmap(a_readback.distance, slice);
			}
			return true;
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

	bool MakeView(ID3D11Device* a_device, Picture& a_picture, const void* a_file, UINT a_width, UINT a_height)
	{
		if (!MakeTextures(a_device, a_width, a_height, 1, a_file, a_picture.texture, a_picture.view, a_picture.distance, a_picture.distanceView, false)) {
			return false;
		}
		a_picture.width = a_width;
		a_picture.height = a_height;
		return true;
	}

	void ReleaseTexture(Picture& a_picture)
	{
		Render::D3D::Release(a_picture.distanceView);
		Render::D3D::Release(a_picture.distance);
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

	bool MakeLayeredView(ID3D11Device* a_device, Layered& a_layered, const void* a_file, UINT a_width, UINT a_height, int a_layers)
	{
		if (!MakeTextures(a_device, a_width, a_height, static_cast<UINT>(a_layers), a_file, a_layered.texture, a_layered.view, a_layered.distance, a_layered.distanceView, true)) {
			return false;
		}
		a_layered.width = a_width;
		a_layered.height = a_height;
		a_layered.layers = a_layers;
		return true;
	}

	void ReleaseLayered(Layered& a_layered)
	{
		Render::D3D::Release(a_layered.distanceView);
		Render::D3D::Release(a_layered.distance);
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

	void Queue(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11Texture2D* a_colour, ID3D11Texture2D* a_distance, Readback a_readback)
	{
		D3D11_TEXTURE2D_DESC colour{}, distance{};
		a_colour->GetDesc(&colour);
		a_distance->GetDesc(&distance);
		colour.Usage = distance.Usage = D3D11_USAGE_STAGING;
		colour.BindFlags = distance.BindFlags = 0;
		colour.CPUAccessFlags = distance.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(a_device->CreateTexture2D(&colour, nullptr, &a_readback.colour))) {
			return;
		}
		if (FAILED(a_device->CreateTexture2D(&distance, nullptr, &a_readback.distance))) {
			a_readback.colour->Release();
			return;
		}
		a_context->CopyResource(a_readback.colour, a_colour);
		a_context->CopyResource(a_readback.distance, a_distance);
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
			const bool isLayered = it->layers > 0;
			const auto path = FileOf(it->key, isLayered);
			std::error_code error;
			std::filesystem::create_directories(path.parent_path(), error);
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			// The header's size is the same whatever it says: written first as a blank, again when the brightness is known.
			const auto headerSize = isLayered ? sizeof(Files::LayeredHeader) : sizeof(Files::ImageHeader);
			const std::vector<char> blank(headerSize);
			file.write(blank.data(), static_cast<std::streamsize>(blank.size()));
			Staged     staged;
			const bool written = WriteBody(a_context, *it, file, staged);
			if (!written) {
				// Still being drawn: asked again next frame (the first read decides; nothing of the file is
				// kept); given up on after a second.
				file.close();
				std::filesystem::remove(path, error);
				if (it->frames < 60) {
					++it;
					continue;
				}
			}
			if (written) {
				// The first (the middle view of a depth image) is what its brightness is read off.
				const float brightness = Brightness(staged.firstColour.data(), staged.firstDistances.data(), it->width, it->height, it->lightScale);
				file.seekp(0);
				if (isLayered) {
					Files::LayeredHeader header;
					header.width = it->width;
					header.height = it->height;
					header.layers = static_cast<std::uint32_t>(it->layers);
					header.loadOrder = LoadOrder();
					header.lightScale = it->lightScale;
					header.brightness = brightness;
					header.takenAt = it->takenAt;
					header.source = static_cast<std::uint32_t>(it->source);
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
					header.takenAt = it->takenAt;
					header.source = static_cast<std::uint32_t>(it->source);
					file.write(reinterpret_cast<const char*>(&header), sizeof(header));
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
				wrote = true;
				if (!file) {
					logger::warn("doorway snapshot: could not write {}", Paths::Narrow(path.wstring()));
				}
			}
			it->colour->Release();
			it->distance->Release();
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

	void Invalidate(RE::FormID a_door, bool a_outdoors)
	{
		const auto key = KeyOf(a_door, a_outdoors, a_outdoors ? PartOfDay() : 0);
		if (const auto found = pictures.find(key); found != pictures.end()) {
			ReleaseTexture(found->second);
			pictures.erase(found);
		}
		if (const auto found = layered.find(key); found != layered.end()) {
			ReleaseLayered(found->second);
			layered.erase(found);
		}
		std::error_code error;
		std::filesystem::remove(FileOf(key, false), error);
		std::filesystem::remove(FileOf(key, true), error);
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

	std::vector<Listing> List()
	{
		std::vector<Listing> out;
		const auto           suffix = std::format("@{:08x}", static_cast<std::uint32_t>(LoadOrder()));
		std::error_code      error;
		for (const auto& entry : std::filesystem::directory_iterator(Folder(), error)) {
			const auto extension = entry.path().extension();
			const bool isLayered = extension == L".ssdl";
			if (!isLayered && extension != L".ssdw") {
				continue;
			}
			// "Skyrim.esm_01A6FB_day@1234abcd": the stem of the key (FileName), the hash after the @.
			const auto stem = Paths::Narrow(entry.path().stem().wstring());
			if (!stem.ends_with(suffix)) {
				continue;
			}
			const auto key = stem.substr(0, stem.size() - suffix.size());
			const auto partAt = key.rfind('_');
			const auto idAt = partAt == std::string::npos || partAt == 0 ? std::string::npos : key.rfind('_', partAt - 1);
			if (idAt == std::string::npos) {
				continue;
			}
			Listing listing;
			listing.door = key.substr(0, idAt) + "|" + key.substr(idAt + 1, partAt - idAt - 1);
			listing.part = key.substr(partAt + 1);
			listing.layered = isLayered;
			std::error_code sizeError;
			listing.bytes = entry.file_size(sizeError);
			std::ifstream file(entry.path(), std::ios::binary);
			const auto [widest, highest] = Largest(isLayered);
			if (isLayered) {
				Files::LayeredHeader header;
				if (!file.read(reinterpret_cast<char*>(&header), sizeof(header)) || !Files::Valid(header, LoadOrder(), widest, highest)) {
					continue;
				}
				listing.layers = static_cast<int>(header.layers);
				listing.source = static_cast<Source>(header.source);
				listing.takenAt = header.takenAt;
			} else {
				Files::ImageHeader header;
				if (!file.read(reinterpret_cast<char*>(&header), sizeof(header)) || !Files::Valid(header, LoadOrder(), widest, highest)) {
					continue;
				}
				listing.source = static_cast<Source>(header.source);
				listing.takenAt = header.takenAt;
			}
			// What is in memory is the newer of the two (a picture just taken is in memory before its file is written).
			const auto fullKey = key.substr(0, idAt) + "|" + key.substr(idAt + 1, partAt - idAt - 1) + "#" + listing.part;
			if (isLayered) {
				const auto found = layered.find(fullKey);
				listing.inMemory = found != layered.end() && found->second.view;
			} else {
				const auto found = pictures.find(fullKey);
				listing.inMemory = found != pictures.end() && found->second.view;
			}
			out.push_back(std::move(listing));
		}
		return out;
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
