#include "Skyrim/Pictures/Cache.h"

#include "Skyrim/Engine/Paths.h"
#include "Skyrim/Render/D3D.h"

#include <DirectXPackedVector.h>

namespace Pictures::Cache
{
	namespace
	{
		constexpr std::uint32_t kMagic = 0x57445353;  // "SSDW"

		std::unordered_map<std::string, Picture> pictures;
		std::vector<Readback>                    readbacks;

		// Set by DeleteAll on the menu's thread; the pictures and readbacks are the main thread's.
		std::atomic<bool> forgetAllWanted;

		// "Skyrim.esm|01A6FB": the same door whatever the load order (as the door corrections).
		std::string DoorKey(RE::FormID a_door)
		{
			auto* form = RE::TESForm::LookupByID(a_door);
			auto* file = form ? form->GetFile(0) : nullptr;
			if (!file) {
				return std::format("{:08X}", a_door);
			}
			return std::format("{}|{:06X}", file->GetFilename(), file->IsLight() ? (a_door & 0xFFF) : (a_door & 0xFFFFFF));
		}

		constexpr std::array<const char*, 4> kParts{ "night", "dawn", "day", "dusk" };

		std::filesystem::path Folder()
		{
			return Paths::PluginDirectory() / "SeamlessSkyrim_Doorways";
		}

		std::filesystem::path FileOf(const std::string& a_key)
		{
			std::string name = a_key;
			for (auto& c : name) {
				if (c == '|' || c == '#' || c == '\\' || c == '/' || c == ':') {
					c = '_';
				}
			}
			return Folder() / Paths::Widen(name + ".ssdw");
		}

		// Below the darkest a texel is counted as: black (an unlit corner) would pull the mean to 0.
		constexpr float kDarkest = 1e-3f;

		// RoomBrightness of a picture's texels (RGBA16F, alpha how far behind the doorway).
		float Brightness(const void* a_texels, UINT a_width, UINT a_height, std::size_t a_rowPitch, float a_lightScale)
		{
			using DirectX::PackedVector::XMConvertHalfToFloat;
			double      sum = 0.0;
			std::size_t count = 0;
			for (UINT y = 0; y < a_height; ++y) {
				const auto* row = reinterpret_cast<const std::uint16_t*>(static_cast<const char*>(a_texels) + y * a_rowPitch);
				for (UINT x = 0; x < a_width; ++x) {
					const auto* texel = row + x * 4;
					// Nothing there (no wall of the room's beyond the capture camera's edge): not part of it.
					if (XMConvertHalfToFloat(texel[3]) >= 20000.0f) {
						continue;
					}
					const float luminance = 0.2126f * XMConvertHalfToFloat(texel[0]) + 0.7152f * XMConvertHalfToFloat(texel[1]) + 0.0722f * XMConvertHalfToFloat(texel[2]);
					sum += std::log(std::max(luminance, kDarkest));
					++count;
				}
			}
			return count ? static_cast<float>(std::exp(sum / count)) / a_lightScale : 0.0f;
		}

		// Pictures of doors not looked at for a while are let go; they are on disk for the next time.
		// Kept for the whole session, every door ever passed held its texture: a walk through a city
		// at the largest size took gigabytes of video memory.
		void ForgetUnused()
		{
			constexpr auto kKeptUnused = std::chrono::seconds(60);
			constexpr auto kLookedAtEvery = std::chrono::seconds(5);
			static std::chrono::steady_clock::time_point lookedAt;
			const auto                                   now = std::chrono::steady_clock::now();
			if (now - lookedAt < kLookedAtEvery) {
				return;
			}
			lookedAt = now;
			std::erase_if(pictures, [&](auto& a_entry) {
				auto& picture = a_entry.second;
				// One just taken is still being read back for its file.
				if (now - picture.used < kKeptUnused || std::ranges::contains(readbacks, a_entry.first, &Readback::key)) {
					return false;
				}
				ReleaseTexture(picture);
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
			for (auto& readback : readbacks) {
				readback.staging->Release();
			}
			readbacks.clear();
		}
	}

	int PartOfDay()
	{
		auto*       calendar = RE::Calendar::GetSingleton();
		const float hour = calendar ? calendar->GetHour() : 12.0f;
		return hour < 5.0f || hour >= 21.0f ? 0 : hour < 8.0f ? 1 : hour < 18.0f ? 2 : 3;
	}

	std::string KeyOf(RE::FormID a_door, bool a_outdoors, int a_part)
	{
		return DoorKey(a_door) + "#" + (a_outdoors ? kParts[a_part] : "inside");
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
			context->UpdateSubresource(a_picture.texture, 0, nullptr, a_texels, a_width * 8, 0);
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

	Picture* Find(ID3D11Device* a_device, RE::FormID a_door, bool a_outdoors)
	{
		ForgetAllIfDeleted();
		ForgetUnused();
		const int          part = PartOfDay();
		std::array<int, 4> order{ part, (part + 1) % 4, (part + 3) % 4, (part + 2) % 4 };
		for (int i = 0; i < (a_outdoors ? 4 : 1); ++i) {
			const auto key = KeyOf(a_door, a_outdoors, order[i]);
			auto&      picture = pictures[key];
			picture.used = std::chrono::steady_clock::now();
			if (!picture.view && !std::exchange(picture.diskTried, true)) {
				// Whatever size it was taken at: after the size is changed a door shows its old picture
				// until it is taken again, rather than the plain colour.
				std::ifstream file(FileOf(key), std::ios::binary);
				std::uint32_t header[4]{};
				// No larger than the largest ever written, the capture camera's at the largest size: a
				// damaged file's header asked for gigabytes of texels before its read could fail.
				const auto [widest, highest] = Camera::SizeFor(Camera::kRoomCamera.extent, Camera::kMaxHeight, Camera::kWidest);  // the room's is the wider
				if (file.read(reinterpret_cast<char*>(header), sizeof(header)) && header[0] == kMagic && header[1] >= 1 && header[1] <= kVersion && header[2] >= 1 &&
					header[2] <= widest && header[3] >= 1 && header[3] <= highest) {
					float          place[4]{};
					Camera::Extent extent = Camera::kDoorwayOnly;
					const bool     depth = header[1] >= 2;
					if (!depth || (file.read(reinterpret_cast<char*>(place), sizeof(place)) && file.read(reinterpret_cast<char*>(extent.data()), sizeof(extent)))) {
						std::vector<std::uint16_t> texels(std::size_t{ header[2] } * header[3] * 4);
						if (file.read(reinterpret_cast<char*>(texels.data()), texels.size() * sizeof(std::uint16_t)) && MakeView(a_device, picture, texels.data(), header[2], header[3])) {
							picture.depth = depth;
							picture.takenFrom = { place[0], place[1], place[2] };
							picture.extent = extent;
							// 0 in the pictures taken before it was written.
							picture.lightScale = place[3] > 0.0f ? place[3] : 1.0f;
							picture.version = header[1];
							if (!a_outdoors) {
								picture.brightness = Brightness(texels.data(), header[2], header[3], header[2] * 8, picture.lightScale);
							}
						}
					}
				}
			}
			if (picture.view) {
				return &picture;
			}
		}
		return nullptr;
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
		for (auto it = readbacks.begin(); it != readbacks.end();) {
			if (++it->frames < 3) {
				++it;
				continue;
			}
			D3D11_MAPPED_SUBRESOURCE mapped{};
			const HRESULT            result = a_context->Map(it->staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
			if (result == DXGI_ERROR_WAS_STILL_DRAWING && it->frames < 60) {
				++it;
				continue;
			}
			if (SUCCEEDED(result)) {
				const auto path = FileOf(it->key);
				std::error_code error;
				std::filesystem::create_directories(path.parent_path(), error);
				std::ofstream       file(path, std::ios::binary | std::ios::trunc);
				const std::uint32_t header[4]{ kMagic, kVersion, it->width, it->height };
				const float         place[4]{ it->takenFrom[0], it->takenFrom[1], it->takenFrom[2], it->lightScale };
				file.write(reinterpret_cast<const char*>(header), sizeof(header));
				file.write(reinterpret_cast<const char*>(place), sizeof(place));
				file.write(reinterpret_cast<const char*>(it->extent.data()), sizeof(it->extent));
				for (UINT row = 0; row < it->height; ++row) {
					file.write(static_cast<const char*>(mapped.pData) + row * mapped.RowPitch, it->width * 8);
				}
				// Taken again meanwhile: the newer one's texels decide.
				const auto found = pictures.find(it->key);
				if (found != pictures.end() && found->second.taken == it->taken && it->key.ends_with("#inside")) {
					found->second.brightness = Brightness(mapped.pData, it->width, it->height, mapped.RowPitch, it->lightScale);
				}
				a_context->Unmap(it->staging, 0);
				if (!file) {
					logger::warn("doorway snapshot: could not write {}", Paths::Narrow(path.wstring()));
				}
			}
			it->staging->Release();
			it = readbacks.erase(it);
		}
	}

	std::optional<float> RoomBrightness(RE::FormID a_door)
	{
		auto*       device = Render::D3D::Device();
		const auto* picture = device ? Find(device, a_door, false) : nullptr;
		if (picture && picture->brightness > 0.0f) {
			return picture->brightness;
		}
		// A room merged beside the shown one may never have had its picture taken: as bright as
		// the rooms that have, rather than as from inside.
		double sum = 0.0;
		int    count = 0;
		for (const auto& [key, other] : pictures) {
			if (other.brightness > 0.0f) {
				sum += std::log(other.brightness);
				++count;
			}
		}
		return count ? std::optional(static_cast<float>(std::exp(sum / count))) : std::nullopt;
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
			if (entry.path().extension() == L".ssdw" && std::filesystem::remove(entry.path(), removeError)) {
				++deleted;
			}
		}
		logger::info("doorway snapshot: {} pictures deleted", deleted);
		return deleted;
	}

	Counts Count()
	{
		std::size_t inMemory = 0;
		for (const auto& [key, picture] : pictures) {
			inMemory += picture.view ? 1 : 0;
		}
		return { inMemory, readbacks.size() };
	}
}
