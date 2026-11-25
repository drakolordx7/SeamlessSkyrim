#include "Pictures/DoorwaySnapshot.h"

#include "Engine/Paths.h"

#include <DirectXPackedVector.h>
#include <d3d11.h>
#include <d3dcompiler.h>

namespace
{
	// Small on purpose: shown blurred, a hundred texels across are all it needs. A room door's
	// picture (the capture camera's) is 2.4 MB on disk at that size, 3.2 MB of video memory with its
	// mipmaps. Larger in the menu ([Portals] iDoorwayPictureHeight) for a sharper picture, at the
	// cost of both: sixteen times that at the largest size.
	constexpr int kDefaultHeight = 128;
	constexpr int kMinHeight = 32;
	constexpr int kMaxHeight = 512;
	// The menu sets it on the render thread; Capture reads it on the main thread.
	std::atomic<int> pictureHeight{ kDefaultHeight };
	// A doorway's shape: three quarters as wide as high, or wider (a double door, a city gate: at
	// three quarters its picture had a third or less of the texels across that it had up, the
	// street seen through Solitude's gate smeared sideways). Its own shape there, in sixteenths.
	constexpr float kNarrowest = 0.75f;
	constexpr float kWidest = 3.0f;
	float AspectOf(const std::array<RE::NiPoint3, 4>& a_corners)
	{
		const float aspect = (a_corners[1] - a_corners[0]).Length() / (a_corners[2] - a_corners[0]).Length();
		return std::clamp(std::round(aspect * 16.0f) / 16.0f, kNarrowest, kWidest);
	}
	UINT WidthFor(UINT a_height) { return a_height * 3 / 4; }
	constexpr std::uint32_t kMagic = 0x57445353;  // "SSDW"
	// 2: alpha is how far behind the doorway each texel's surface is, and the header is followed by
	// where the picture was taken from and the part of the door's plane it covers. A version 1
	// picture is drawn flat until it is taken again. 3: as 2, taken without the room's people. 4:
	// without the room's front wall (a capture camera's of an older version is taken again). 5: the
	// sky left out of a texel's average where anything else is in it; a gate's from further back.
	constexpr std::uint32_t kVersion = 5;
	// A picture is taken again at most this often (the room changes; the light outdoors does).
	constexpr auto kRetake = std::chrono::seconds(20);

	// A place in the doorway's own terms: across and up as fractions of its width and height from
	// the bottom corner, and the distance behind it (negative in front). The door's corners move
	// with the door between sessions; where the picture was taken from has to move with them.
	using DoorwayPlace = std::array<float, 3>;

	// The plain colour stands this far behind the doorway, seen from in front of its middle: a
	// back wall at a room's depth, the outdoors further off. Something for the eye to look past.
	constexpr DoorwayPlace kPlainEye{ 0.5f, 0.55f, -150.0f };
	constexpr float        kPlainBehindRoom = 400.0f;
	constexpr float        kPlainBehindOutdoors = 3000.0f;

	// The part of the door's plane a picture covers, in the doorway's widths and heights from its
	// bottom corner: left, right, bottom, top.
	using Extent = std::array<float, 4>;
	constexpr Extent kDoorwayOnly{ 0.0f, 1.0f, 0.0f, 1.0f };
	// The capture camera's: one and a quarter doorway widths to either side, three quarters of its
	// height above, seen from in front of its middle. Stepping aside shows the room's side beyond
	// the doorway's edge, and coming close its ceiling; rays at up to about 55 degrees through the
	// doorway land in it. Not below: under the threshold is under the floor. From 80 units out
	// (a 130 degree view) the room beyond a few steps came out in a patch of thirty texels in the
	// middle, a dark smear in the doorway.
	struct CameraShape
	{
		Extent extent;
		float  back;  // how far in front of the doorway it stands
		bool   atEye;  // at the height of the eye it is taken for, not the doorway's middle
	};
	constexpr CameraShape kRoomCamera{ { -1.25f, 2.25f, 0.0f, 1.75f }, 150.0f, false };
	// A city gate's is seen from down the road, and what is behind it is a street thousands of
	// units deep: from the room's place the city beyond the gatehouse came out in a patch of ninety
	// texels, drawn nine times larger on screen from where a gate is walked up to. From about that
	// far back the far side keeps its size; the narrower view still covers a player at the gate.
	// At the road's eye height: Whiterun's road is below the gate's threshold, and from the
	// doorway's middle the gatehouse's inner arch hid the street that the eye sees under it (the
	// eye's rays ran off the picture's top, its edge drawn out in streaks).
	constexpr CameraShape kGateCamera{ { -0.75f, 1.75f, 0.0f, 1.75f }, 1000.0f, true };
	const CameraShape&    CameraOf(bool a_gate) { return a_gate ? kGateCamera : kRoomCamera; }

	// a_eyeUp: the eye's height in the doorway's heights (Frame::PlaceOf).
	DoorwayPlace PlaceOf(const CameraShape& a_shape, float a_eyeUp)
	{
		return { 0.5f, a_shape.atEye ? std::clamp(a_eyeUp, 0.0f, 1.0f) : 0.5f, -a_shape.back };
	}
	// Its texels per doorway against a picture taken with the player's view: taken once and kept,
	// and seen at more of an angle (stretched further).
	constexpr float kCameraDensity = 2.0f;

	// A picture's size in texels: the doorway at the size set ([Portals] iDoorwayPictureHeight) and
	// its shape (AspectOf), times the extent.
	std::pair<UINT, UINT> SizeFor(const Extent& a_extent, int a_doorwayHeight, float a_aspect)
	{
		const float density = a_extent != kDoorwayOnly ? kCameraDensity : 1.0f;
		return { static_cast<UINT>(std::lround(a_doorwayHeight * a_aspect * density * (a_extent[1] - a_extent[0]))),
			static_cast<UINT>(std::lround(a_doorwayHeight * density * (a_extent[3] - a_extent[2]))) };
	}

	struct Picture
	{
		ID3D11Texture2D*                      texture = nullptr;
		ID3D11ShaderResourceView*             view = nullptr;
		std::chrono::steady_clock::time_point taken{};
		std::chrono::steady_clock::time_point used{};  // last drawn or taken (ForgetUnused)
		bool                                  diskTried = false;
		// Its own: one loaded from disk or taken before the size was changed keeps the old one.
		UINT         width = 0;
		UINT         height = 0;
		bool         depth = false;  // false: a version 1 picture
		DoorwayPlace takenFrom{};
		Extent        extent = kDoorwayOnly;
		float         lightScale = 1.0f;  // Source::lightScale when taken
		std::uint32_t version = kVersion;
		float         brightness = 0.0f;  // RoomBrightness; 0 until its texels have been on the CPU
	};
	std::unordered_map<std::string, Picture> pictures;

	struct Readback
	{
		std::string      key;
		ID3D11Texture2D* staging = nullptr;
		UINT             width = 0;
		UINT             height = 0;
		DoorwayPlace     takenFrom{};
		Extent           extent{};
		float            lightScale = 1.0f;
		int              frames = 0;
		std::chrono::steady_clock::time_point taken{};  // the picture's Picture::taken
	};
	std::vector<Readback> readbacks;

	// Set by Capture, taken by the portal pass (TakeCaptureRequest); both on the render thread.
	std::optional<Portal::Snapshot::CaptureCamera> captureWanted;
	// Where that camera stood, for the frame drawn with it: placed again from the eye a frame on,
	// a gate's moved with the player and the picture's corners missed what was drawn.
	std::pair<RE::FormID, DoorwayPlace> wantedFrom{};

	// The doorway's frame: corner 0 and its edges, and its normal turned away from a_eye (so the
	// same whichever order the corners come in for the side it is seen from).
	struct Frame
	{
		RE::NiPoint3 origin;
		RE::NiPoint3 across;
		RE::NiPoint3 up;
		RE::NiPoint3 inward;

		Frame(const std::array<RE::NiPoint3, 4>& a_corners, const RE::NiPoint3& a_eye) :
			origin(a_corners[0]), across(a_corners[1] - a_corners[0]), up(a_corners[2] - a_corners[0])
		{
			inward = across.Cross(up);
			inward.Unitize();
			if (inward.Dot((a_corners[0] + a_corners[3]) * 0.5f - a_eye) < 0.0f) {
				inward = inward * -1.0f;
			}
		}

		DoorwayPlace PlaceOf(const RE::NiPoint3& a_point) const
		{
			const auto offset = a_point - origin;
			return { offset.Dot(across) / across.SqrLength(), offset.Dot(up) / up.SqrLength(), offset.Dot(inward) };
		}

		RE::NiPoint3 PointAt(const DoorwayPlace& a_place) const
		{
			return origin + across * a_place[0] + up * a_place[1] + inward * a_place[2];
		}
	};

	// Looking along the doorway's normal, so its plane is parallel to the image: the picture's
	// texels are evenly spread over the door's plane, as the draw reads them.
	Portal::Snapshot::CaptureCamera CameraFor(RE::FormID a_door, const Frame& a_frame, const CameraShape& a_shape, const DoorwayPlace& a_place)
	{
		Portal::Snapshot::CaptureCamera camera;
		camera.door = a_door;
		const float width = a_frame.across.Length();
		const float height = a_frame.up.Length();
		const auto  forward = a_frame.inward;
		const auto  up = a_frame.up / height;
		const auto  right = forward.Cross(up);  // an NiCamera's columns: forward, up, right
		for (int row = 0; row < 3; ++row) {
			camera.transform.rotate.entry[row][0] = forward[row];
			camera.transform.rotate.entry[row][1] = up[row];
			camera.transform.rotate.entry[row][2] = right[row];
		}
		camera.transform.translate = a_frame.PointAt(a_place);
		camera.transform.scale = 1.0f;
		// The doorway's across may run either way on screen.
		const float acrossOnScreen = right.Dot(a_frame.across) / width;
		const float side0 = acrossOnScreen * (a_shape.extent[0] - a_place[0]) * width / a_shape.back;
		const float side1 = acrossOnScreen * (a_shape.extent[1] - a_place[0]) * width / a_shape.back;
		camera.left = std::min(side0, side1);
		camera.right = std::max(side0, side1);
		camera.bottom = (a_shape.extent[2] - a_place[1]) * height / a_shape.back;
		camera.top = (a_shape.extent[3] - a_place[1]) * height / a_shape.back;
		camera.toDoorway = a_shape.back;
		return camera;
	}

	// Where a place on the door's plane is in the capture camera's image, in clip space.
	std::array<float, 4> CameraClip(const Portal::Snapshot::CaptureCamera& a_camera, const Frame& a_frame, float a_across, float a_up)
	{
		const auto  point = a_frame.PointAt({ a_across, a_up, 0.0f });
		const auto  offset = point - a_camera.transform.translate;
		const auto& rotate = a_camera.transform.rotate;
		const float ahead = offset.x * rotate.entry[0][0] + offset.y * rotate.entry[1][0] + offset.z * rotate.entry[2][0];
		const float up = (offset.x * rotate.entry[0][1] + offset.y * rotate.entry[1][1] + offset.z * rotate.entry[2][1]) / ahead;
		const float right = (offset.x * rotate.entry[0][2] + offset.y * rotate.entry[1][2] + offset.z * rotate.entry[2][2]) / ahead;
		return { 2.0f * (right - a_camera.left) / (a_camera.right - a_camera.left) - 1.0f, 2.0f * (up - a_camera.bottom) / (a_camera.top - a_camera.bottom) - 1.0f, 0.0f, 1.0f };
	}

	struct Pipeline
	{
		ID3D11VertexShader*      captureVS = nullptr;
		ID3D11PixelShader*       capturePS = nullptr;
		ID3D11Buffer*            captureConstants = nullptr;
		ID3D11Texture2D*         target = nullptr;  // at the size pictures are taken at now (EnsureTarget)
		ID3D11RenderTargetView*  targetView = nullptr;
		UINT                     targetWidth = 0;
		UINT                     targetHeight = 0;
		ID3D11VertexShader*      drawVS = nullptr;
		ID3D11PixelShader*       drawPS = nullptr;
		ID3D11InputLayout*       layout = nullptr;
		ID3D11Buffer*            vertices = nullptr;
		ID3D11Buffer*            drawConstants = nullptr;
		ID3D11SamplerState*      sampler = nullptr;
		ID3D11DepthStencilState* depthWrite = nullptr;
		ID3D11DepthStencilState* depthTest = nullptr;
		ID3D11DepthStencilState* noDepth = nullptr;
		ID3D11BlendState*        blend = nullptr;
		ID3D11BlendState*        opaque = nullptr;
		ID3D11RasterizerState*   raster = nullptr;
		bool                     failed = false;
	} pipeline;

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

	// The outdoors by the part of the day it is seen in; an interior looks the same at any hour.
	constexpr std::array<const char*, 4> kParts{ "night", "dawn", "day", "dusk" };
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

	ID3D11DeviceContext* Context()
	{
		return reinterpret_cast<ID3D11DeviceContext*>(RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context);
	}

	// The engine holds the device for the whole session; the reference taken here isn't kept.
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

	template <class T>
	void Release(T*& a_object)
	{
		if (a_object) {
			a_object->Release();
			a_object = nullptr;
		}
	}

	// Below the darkest a texel is counted as: black (an unlit corner) would pull the mean to 0.
	constexpr float kDarkest = 1e-3f;

	// RoomBrightness of a picture's texels (RGBA16F, alpha how far behind the doorway).
	float Brightness(const void* a_texels, UINT a_width, UINT a_height, std::size_t a_rowPitch, float a_lightScale)
	{
		using DirectX::PackedVector::XMConvertHalfToFloat;
		double sum = 0.0;
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

	// With mipmaps: where the eye's ray stretches a row of texels across the screen (leaving the
	// picture at its side, or sliding off a near thing's edge onto the room behind it) the draw
	// blurs by how far, from a smaller mip. Sampled sharp there, the row showed as streaks.
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
			Release(a_picture.texture);
			return false;
		}
		if (a_texels) {
			auto* context = Context();
			context->UpdateSubresource(a_picture.texture, 0, nullptr, a_texels, a_width * 8, 0);
			context->GenerateMips(a_picture.view);
		}
		a_picture.width = a_width;
		a_picture.height = a_height;
		return true;
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
			Release(picture.view);
			Release(picture.texture);
			return true;
		});
	}

	// Set by DeleteAll on the menu's thread; the pictures and readbacks are the main thread's.
	std::atomic<bool> forgetAllWanted;

	// After DeleteAll: kept in memory, a door went on showing its old picture, and a readback still
	// in flight wrote its file back.
	void ForgetAllIfDeleted()
	{
		if (!forgetAllWanted.exchange(false)) {
			return;
		}
		for (auto& [key, picture] : pictures) {
			Release(picture.view);
			Release(picture.texture);
		}
		pictures.clear();
		for (auto& readback : readbacks) {
			readback.staging->Release();
		}
		readbacks.clear();
	}

	// The door's picture for this part of the day, loaded from disk the first time it is asked
	// for; another part's when this one was never taken (a door first passed at night).
	Picture* Find(ID3D11Device* a_device, RE::FormID a_door, bool a_outdoors)
	{
		ForgetAllIfDeleted();
		ForgetUnused();
		const int part = PartOfDay();
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
				const auto [widest, highest] = SizeFor(kRoomCamera.extent, kMaxHeight, kWidest);  // the room's is the wider
				if (file.read(reinterpret_cast<char*>(header), sizeof(header)) && header[0] == kMagic && header[1] >= 1 && header[1] <= kVersion && header[2] >= 1 &&
					header[2] <= widest && header[3] >= 1 && header[3] <= highest) {
					float      place[4]{};
					Extent     extent = kDoorwayOnly;
					const bool depth = header[1] >= 2;
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

	constexpr std::size_t kCaptureConstants = 52;
	constexpr std::size_t kDrawConstants = 104;

	bool EnsurePipeline(ID3D11Device* a_device)
	{
		if (pipeline.captureVS || pipeline.failed) {
			return !pipeline.failed;
		}
		// Capture: each texel of the picture is the doorway's rectangle at that place, as the frame
		// shows it, averaged over 4x4 points (at the default size the doorway covers
		// dozens of pixels per texel). Alpha: how far behind the doorway the surface seen there is.
		static const char captureSource[] = R"(
			Texture2D<float4> Scene : register(t0);
			Texture2D<float>  Depth : register(t1);
			SamplerState      Linear : register(s0);
			cbuffer Capture : register(b0)
			{
				float4 Corner0;  // clip space: the picture's bottom
				float4 Corner1;  // bottom, across
				float4 Corner2;  // top
				// xy: the scene's viewport in pixels; zw: a viewport pixel's size in the texture (the
				// main scene's viewport is the top-left part of its texture at a render scale below
				// 1; the portal pass's image always fills its target).
				float4 Sizes;
				float4 Out;      // xy: the picture's size
				row_major float4x4 InverseViewProj;
				float4 WColumn;  // a camera-relative point's distance along the view axis: dot(p, xyz) + w
				float4 Origin;   // the doorway's bottom corner, camera-relative
				float4 Inward;   // the doorway's normal, away from the eye
				// x, y: the portal pass's near and far; z: 1 = Depth is the portal pass's; w: the capture
				// camera's distance to the doorway (looking along its normal), 0 = the player's view
				float4 Portal;
			};
			// The sky and what is too far to tell apart from it; within half precision.
			static const float kFar = 30000;
			float4 VS(uint id : SV_VertexID) : SV_Position
			{
				float2 p = float2((id << 1) & 2, id & 2);
				return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
			}
			float Behind(float2 ndc, float2 pixel)
			{
				float2 size;
				Depth.GetDimensions(size.x, size.y);
				float  depth = Depth.Load(int3(clamp(pixel * Sizes.zw * size, 0, size - 1), 0));
				float3 world;
				if (Portal.z > 0.5) {
					// As the doorway quad reads it (PortalRender): the distance along the view axis, then
					// the point on this pixel's ray.
					if (depth >= 1)
						return kFar;
					float  viewDistance = Portal.x * Portal.y / max(Portal.y - depth * (Portal.y - Portal.x), 1e-3);
					if (Portal.w > 0)
						return clamp(viewDistance - Portal.w, 1, kFar);
					float4 p = mul(float4(ndc, 0.5, 1), InverseViewProj);
					p.xyz /= p.w;
					world = p.xyz * viewDistance / (dot(p.xyz, WColumn.xyz) + WColumn.w);
				} else {
					if (depth >= 0.999999)
						return kFar;
					float4 p = mul(float4(ndc, depth, 1), InverseViewProj);
					world = p.xyz / p.w;
				}
				// In front of the doorway (its frame, a leaf swung towards the eye): at it.
				return clamp(dot(world - Origin.xyz, Inward.xyz), 1, kFar);
			}
			float4 PS(float4 position : SV_Position) : SV_Target
			{
				float3 sum = 0;
				float3 nearSum = 0;
				float  nearness = 0;
				float  count = 0;
				[unroll] for (int i = 0; i < 4; ++i)
				[unroll] for (int j = 0; j < 4; ++j) {
					float2 uv = (floor(position.xy) + (float2(i, j) + 0.5) / 4) / Out.xy;
					float4 clip = Corner0 + uv.x * (Corner1 - Corner0) + (1 - uv.y) * (Corner2 - Corner0);
					float2 ndc = clip.xy / clip.w;
					float2 pixel = float2((ndc.x * 0.5 + 0.5) * Sizes.x, (0.5 - ndc.y * 0.5) * Sizes.y);
					float3 color = Scene.SampleLevel(Linear, pixel * Sizes.zw, 0).rgb;
					float  behind = Behind(ndc, pixel);
					sum += color;
					if (behind < kFar) {
						nearSum += color;
						nearness += 1 / behind;
						++count;
					}
				}
				if (count == 0)
					return float4(max(sum / 16, 0), kFar);
				// The sky left out where anything else is: through a gate the real sky is shown where
				// the picture has none (Draw), and a roof's edge averaged with the portal pass's flat
				// sky colour stood in it as a blue fringe. Averaged as nearness: a lamp in front of a
				// wall far behind it isn't pulled away to the wall.
				return float4(max(nearSum / count, 0), count / nearness);
			}
		)";
		// Draw: the picture blurred (or the plain colour) in the doorway, as the space behind it in
		// relief. The doorway's own depth is kept (written, so what is drawn later is hidden behind
		// it as by a wall there); the motion is the surface's seen through it.
		static const char drawSource[] = R"(
			Texture2D<float4> Picture : register(t0);
			SamplerState      Linear : register(s0);
			cbuffer Draw : register(b0)
			{
				row_major float4x4 ViewProj;          // as drawn (jittered)
				row_major float4x4 ViewProjUnjittered;
				row_major float4x4 PreviousViewProj;  // unjittered, relative to the previous eye
				float4             EyeDelta;          // this frame's eye - the previous frame's
				float4             Fog;               // as the engine's FogParam
				float4             Plain;             // rgb: the colour without a picture; w: 1 = the picture
				float4             Params;            // x: alpha; yz: a texel's size
				float4             FogNear;
				float4             FogFar;
				float4             Origin;            // xyz: the doorway's bottom corner, camera-relative; w: 1 = the picture has depth
				float4             Across;            // xyz: its bottom edge; w: 1 / its length squared
				float4             Up;                // xyz: its side going up; w: 1 / its length squared
				float4             Inward;            // xyz: its normal, away from the eye; w: how far behind it the picture was taken from (negative)
				float4             TakenFrom;         // xyz: where, camera-relative; w: how far behind the doorway the plain colour stands
				float4             Look;              // x: Adaptation::Kind; y: daylight; z: 1 = the outdoors behind; w: the picture's light now against when taken
				float4             Extent;            // the part of the door's plane the picture covers, as Extent
				float4             Gate;              // x: 1 = a city gate (the same sky on both sides)
			};
			static const float kFar = 30000;
			struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; float3 world : TEXCOORD1; float depth : TEXCOORD2; };
			Vertex VS(float3 world : POSITION, float2 uv : TEXCOORD0)
			{
				Vertex v;
				v.position = mul(float4(world, 1), ViewProj);
				v.uv = uv;
				v.world = world;
				v.depth = v.position.w;
				return v;
			}
			// The composite fogs the world by view depth (not distance); the doorway at its own depth
			// matches the wall around it.
			float3 Fogged(float3 color, float depth)
			{
				if (Fog.w <= 0)
					return color;
				float f = min(Fog.w, pow(saturate(depth * Fog.y - Fog.x), Fog.z));
				return lerp(color, lerp(FogNear.rgb, FogFar.rgb, f), f);
			}
			struct Pixel { float4 color : SV_Target0; float2 motion : SV_Target1; };
			// Cubic B-spline filtering from four bilinear reads (GPU Gems 2, ch. 20). Bilinear alone,
			// magnified ten times and more, showed every texel as a patch with creases at its edges;
			// the B-spline is smooth across them.
			// At mip a_level (fractional: the B-spline of the two around it, blended).
			float3 SampleSmoothAt(float2 uv, float a_level)
			{
				float2 texel = Params.yz * exp2(a_level);
				float2 p = uv / texel - 0.5;
				float2 i = floor(p);
				float2 f = p - i;
				float2 w0 = (1 - f) * (1 - f) * (1 - f) / 6;
				float2 w1 = (4 - 6 * f * f + 3 * f * f * f) / 6;
				float2 w3 = f * f * f / 6;
				float2 w2 = 1 - w0 - w1 - w3;
				float2 g0 = w0 + w1;
				float2 g1 = w2 + w3;
				float2 t0 = (i + 0.5 - 1 + w1 / g0) * texel;
				float2 t1 = (i + 0.5 + 1 + w3 / g1) * texel;
				return g0.y * (g0.x * Picture.SampleLevel(Linear, t0, a_level).rgb + g1.x * Picture.SampleLevel(Linear, float2(t1.x, t0.y), a_level).rgb) +
				       g1.y * (g0.x * Picture.SampleLevel(Linear, float2(t0.x, t1.y), a_level).rgb + g1.x * Picture.SampleLevel(Linear, t1, a_level).rgb);
			}
			float3 SampleSmooth(float2 uv, float a_level)
			{
				float below = floor(a_level);
				return lerp(SampleSmoothAt(uv, below), SampleSmoothAt(uv, below + 1), a_level - below);
			}
			float2 PictureUV(float3 p)
			{
				float3 d = p - Origin.xyz;
				float2 inDoorway = float2(dot(d, Across.xyz) * Across.w, dot(d, Up.xyz) * Up.w);
				return float2((inDoorway.x - Extent.x) / (Extent.y - Extent.x), 1 - (inDoorway.y - Extent.z) / (Extent.w - Extent.z));
			}
			float StoredBehind(float2 uv)
			{
				if (Plain.w < 0.5)
					return TakenFrom.w;
				// A picture without depth: everything at the doorway, drawn flat as before.
				if (Origin.w < 0.5)
					return 0;
				// Through a gate a roof or an arch stands against the sky: read between them, the
				// depth made a slope to the sky that rays landed on, in dashes along every edge.
				if (Gate.x > 0.5)
					return Picture.Load(int3(clamp(uv / Params.yz, 0, 1 / Params.yz - 1), 0)).a;
				return Picture.SampleLevel(Linear, uv, 0).a;
			}
			// Along the eye's ray past the doorway, the place its points are seen at from where the
			// picture was taken moves in a straight line across the picture: from where the ray
			// meets the doorway (t = 0) towards where it would at infinite distance (t = 1). t is
			// also how far behind the doorway a point is, as h / (h + the distance the picture was
			// taken from), so a stored distance converts to it directly.
			float TOf(float a_behind) { return a_behind / (a_behind - Inward.w); }
			Pixel PS(Vertex v)
			{
				Pixel  p;
				float3 dir = normalize(v.world);
				// Grazing the doorway, infinitely far is off any picture; it is cut off at the sides anyway.
				float  facing = max(dot(dir, Inward.xyz), 0.05);
				float2 uvNear = PictureUV(v.world);
				float2 uvFar = PictureUV(TakenFrom.xyz + dir * (-Inward.w / facing));
				float2 delta = uvFar - uvNear;
				// Past the picture's sides is what the doorway's sides hid where it was taken from: the
				// ray takes the colour at the side it leaves by.
				float  end = 1;
				[unroll] for (int axis = 0; axis < 2; ++axis) {
					if (delta[axis] > 1e-6)
						end = min(end, (1 - uvNear[axis]) / delta[axis]);
					else if (delta[axis] < -1e-6)
						end = min(end, -uvNear[axis] / delta[axis]);
				}
				end = saturate(end);
				float before = 0;
				float t = end;
				bool  hit = false;
				[loop] for (int i = 1; i <= 24; ++i) {
					float at = end * i / 24;
					float surface = TOf(StoredBehind(uvNear + delta * at));
					// Through a gate the eye looks under the gatehouse's arches from lower down than
					// the picture was taken: the ray comes at an arch from behind, where the picture
					// has the arch over the street it passes on to. As a solid reaching back forever
					// the arch was drawn out over that street in streaks; a near thing is taken as a
					// shell a few steps deep. A room's walls stay solid.
					if (Gate.x > 0.5 && at - surface > 2.0 * end / 24) {
						before = at;
						continue;
					}
					if (at >= surface) {
						hit = true;
						t = at;
						[unroll] for (int j = 0; j < 5; ++j) {
							float middle = 0.5 * (before + t);
							if (middle >= TOf(StoredBehind(uvNear + delta * middle)))
								t = middle;
							else
								before = middle;
						}
						break;
					}
					before = at;
				}
				t = min(t, 0.9995);
				float2 uv = uvNear + delta * t;
				float  behind = min(t * -Inward.w / (1 - t), kFar);
				float3 world = v.world + dir * (behind / facing);
				// As the live gate (PortalRender's doorway quad): where the far side has nothing, the
				// player's own worldspace shows, with the real sky. The portal pass's is a flat colour,
				// and a picture's sky is of another hour and weather. A ray that met nothing in the
				// picture has left it past what was taken: the world there is the nearest guess.
				if (Gate.x > 0.5 && Plain.w > 0.5 && (!hit || behind >= kFar * 0.5))
					discard;

				float3 color;
				if (Plain.w > 0.5) {
					// How stretched the picture is here: across the screen the texels it reads run
					// along one line and stand still along the other where the ray leaves the
					// picture at its side, or slides off a near thing's edge onto the room behind.
					// Sharp, a row of texels drawn out so read as streaks; blurred by how far, from
					// a smaller mip, as a smear. Seen straight or at a slant (stretched twice or so)
					// it stays sharp.
					float2 alongX = ddx(uv) / Params.yz;
					float2 alongY = ddy(uv) / Params.yz;
					float  major = max(length(alongX), length(alongY));
					float  minor = abs(alongX.x * alongY.y - alongX.y * alongY.x) / max(major, 1e-6);
					float  level = major > 1e-6 ? clamp(log2(major / max(minor, major * 1e-3)) - 1.5, 0, 4) : 0;
					float2 step = Params.yz * exp2(level);
					// A little more blur on top: what changed since the picture was taken stays vague.
					float3 sum = 0;
					[unroll] for (int x = -1; x <= 1; ++x)
					[unroll] for (int y = -1; y <= 1; ++y) {
						sum += (2 - abs(x)) * (2 - abs(y)) * SampleSmooth(uv + float2(x, y) * step, level);
					}
					color = sum / 16;
				} else if (Look.z > 0.5) {
					// The outdoors: lighter up towards the sky.
					color = Plain.rgb * lerp(1.2, 0.85, saturate(uv.y));
				} else {
					// A room: its floor lit from the doorway, its ceiling dark.
					color = Plain.rgb * lerp(0.6, 1.2, saturate(uv.y));
				}

				// A picture as bright as the real space is now. Only the plain colour is made as an eye
				// used to the player's space sees the other, eased out as it fades over the real space
				// (which is drawn without it): the doorway brightens or dims into it like an eye
				// adjusting. A picture got that too, and came out brighter or darker than the real
				// space, its light already in it (taken in the same high dynamic range as the wall
				// around it).
				color *= Look.w;
				float  adapt = Plain.w > 0.5 ? 0 : Look.y * Params.x;
				float  luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
				if (Look.x > 0.5 && Look.x < 1.5) {
					// A room from the street by day: dim, dimmer the deeper in, but its lamps, fire
					// and windows still bright.
					float lit = saturate(luminance / 1.5);
					float dark = 0.6 * lerp(1, exp(-behind / 800), 0.5);
					color *= lerp(1, lerp(dark, 1, lit), adapt);
				} else if (Look.x > 1.5) {
					// The street from a room by day: a little washed out, and bright enough for the
					// bloom to glow around the doorway.
					color = lerp(color, luminance.xxx, 0.25 * adapt) * lerp(1, 1.6, adapt);
				}

				// The picture has the fog between where it was taken and what it shows already; the
				// eye's own is up to the doorway.
				p.color = float4(Fogged(color, v.depth), Params.x);
				float4 now = mul(float4(world, 1), ViewProjUnjittered);
				float4 then = mul(float4(world + EyeDelta.xyz, 1), PreviousViewProj);
				p.motion = float2(-0.5, 0.5) * (now.xy / now.w - then.xy / then.w);
				return p;
			}
		)";
		std::vector<ID3DBlob*> blobs;
		const auto compile = [&](const char* a_source, std::size_t a_size, const char* a_entry, const char* a_profile) -> ID3DBlob* {
			ID3DBlob* out = nullptr;
			ID3DBlob* errors = nullptr;
			if (FAILED(::D3DCompile(a_source, a_size, "SeamlessDoorway", nullptr, nullptr, a_entry, a_profile, 0, 0, &out, &errors))) {
				logger::error("doorway snapshot: shader {} failed: {}", a_entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
				pipeline.failed = true;
			}
			if (errors) {
				errors->Release();
			}
			if (out) {
				blobs.push_back(out);
			}
			return out;
		};
		auto* captureVS = compile(captureSource, sizeof(captureSource) - 1, "VS", "vs_5_0");
		auto* capturePS = compile(captureSource, sizeof(captureSource) - 1, "PS", "ps_5_0");
		auto* drawVS = compile(drawSource, sizeof(drawSource) - 1, "VS", "vs_5_0");
		auto* drawPS = compile(drawSource, sizeof(drawSource) - 1, "PS", "ps_5_0");
		if (!pipeline.failed) {
			const D3D11_INPUT_ELEMENT_DESC elements[]{
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};
			D3D11_BUFFER_DESC    vertices{ sizeof(float) * 5 * 4, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
			D3D11_BUFFER_DESC    captureConstants{ sizeof(float) * kCaptureConstants, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
			D3D11_BUFFER_DESC    drawConstants{ sizeof(float) * kDrawConstants, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
			D3D11_SAMPLER_DESC   sampler{};
			sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampler.MaxLOD = D3D11_FLOAT32_MAX;
			D3D11_DEPTH_STENCIL_DESC depthWrite{};
			depthWrite.DepthEnable = TRUE;
			depthWrite.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			depthWrite.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
			D3D11_DEPTH_STENCIL_DESC depthTest = depthWrite;
			depthTest.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			D3D11_DEPTH_STENCIL_DESC noDepth{};
			D3D11_BLEND_DESC         blend{};
			blend.IndependentBlendEnable = TRUE;
			blend.RenderTarget[0].BlendEnable = TRUE;
			blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
			blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
			blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
			blend.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			D3D11_BLEND_DESC opaque{};
			opaque.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			D3D11_RASTERIZER_DESC raster{};
			raster.FillMode = D3D11_FILL_SOLID;
			raster.CullMode = D3D11_CULL_NONE;
			raster.DepthClipEnable = TRUE;
			pipeline.failed =
				FAILED(a_device->CreateVertexShader(captureVS->GetBufferPointer(), captureVS->GetBufferSize(), nullptr, &pipeline.captureVS)) ||
				FAILED(a_device->CreatePixelShader(capturePS->GetBufferPointer(), capturePS->GetBufferSize(), nullptr, &pipeline.capturePS)) ||
				FAILED(a_device->CreateVertexShader(drawVS->GetBufferPointer(), drawVS->GetBufferSize(), nullptr, &pipeline.drawVS)) ||
				FAILED(a_device->CreatePixelShader(drawPS->GetBufferPointer(), drawPS->GetBufferSize(), nullptr, &pipeline.drawPS)) ||
				FAILED(a_device->CreateInputLayout(elements, 2, drawVS->GetBufferPointer(), drawVS->GetBufferSize(), &pipeline.layout)) ||
				FAILED(a_device->CreateBuffer(&vertices, nullptr, &pipeline.vertices)) || FAILED(a_device->CreateBuffer(&captureConstants, nullptr, &pipeline.captureConstants)) ||
				FAILED(a_device->CreateBuffer(&drawConstants, nullptr, &pipeline.drawConstants)) || FAILED(a_device->CreateSamplerState(&sampler, &pipeline.sampler)) ||
				FAILED(a_device->CreateDepthStencilState(&depthWrite, &pipeline.depthWrite)) || FAILED(a_device->CreateDepthStencilState(&depthTest, &pipeline.depthTest)) ||
				FAILED(a_device->CreateDepthStencilState(&noDepth, &pipeline.noDepth)) || FAILED(a_device->CreateBlendState(&blend, &pipeline.blend)) ||
				FAILED(a_device->CreateBlendState(&opaque, &pipeline.opaque)) || FAILED(a_device->CreateRasterizerState(&raster, &pipeline.raster));
			if (pipeline.failed) {
				logger::error("doorway snapshot: could not create the pipeline");
			}
		}
		for (auto* blob : blobs) {
			blob->Release();
		}
		return !pipeline.failed;
	}

	// Made anew when the size was changed.
	bool EnsureTarget(ID3D11Device* a_device, UINT a_width, UINT a_height)
	{
		if (pipeline.targetView && pipeline.targetWidth == a_width && pipeline.targetHeight == a_height) {
			return true;
		}
		Release(pipeline.targetView);
		Release(pipeline.target);
		D3D11_TEXTURE2D_DESC target{ a_width, a_height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
		if (FAILED(a_device->CreateTexture2D(&target, nullptr, &pipeline.target)) || FAILED(a_device->CreateRenderTargetView(pipeline.target, nullptr, &pipeline.targetView))) {
			logger::error("doorway snapshot: could not create a {}x{} target", a_width, a_height);
			Release(pipeline.target);
			return false;
		}
		pipeline.targetWidth = a_width;
		pipeline.targetHeight = a_height;
		return true;
	}

	// The engine tracks what it has bound; everything touched here goes back exactly as it was.
	class SavedState
	{
	public:
		explicit SavedState(ID3D11DeviceContext* a_context) :
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
		~SavedState()
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
		ID3D11ShaderResourceView* textures[2]{};  // the capture's depth in the second
		ID3D11SamplerState*       sampler = nullptr;
		ID3D11Buffer*             psConstants = nullptr;
		ID3D11Buffer*             vsConstants = nullptr;
	};

	void Upload(ID3D11DeviceContext* a_context, ID3D11Buffer* a_buffer, const void* a_data, std::size_t a_size)
	{
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(a_context->Map(a_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, a_data, a_size);
			a_context->Unmap(a_buffer, 0);
		}
	}

	std::array<float, 4> ToClip(const Portal::Snapshot::View& a_view, const RE::NiPoint3& a_point)
	{
		const float          v[3] = { a_point.x - a_view.eye.x, a_point.y - a_view.eye.y, a_point.z - a_view.eye.z };
		const float*         m = a_view.viewProj;
		std::array<float, 4> clip{};
		for (int column = 0; column < 4; ++column) {
			clip[column] = v[0] * m[column] + v[1] * m[4 + column] + v[2] * m[8 + column] + m[12 + column];
		}
		return clip;
	}

	// Pictures taken a few frames ago, read back and written to disk (the copy is done by then;
	// asked for at once, the read would wait for the GPU).
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
				std::ofstream file(path, std::ios::binary | std::ios::trunc);
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

	// MeasureScene: one group of 16x16 threads, each taking 4x4 points of a 64x64 grid over the
	// viewport; read back through a ring of staging copies so the GPU is never waited for.
	struct Meter
	{
		ID3D11ComputeShader*                 shader = nullptr;
		ID3D11Buffer*                        constants = nullptr;
		ID3D11Texture2D*                     result = nullptr;
		ID3D11UnorderedAccessView*           resultView = nullptr;
		std::array<ID3D11Texture2D*, 4>      staging{};
		std::array<bool, 4>                  pending{};
		std::size_t                          next = 0;
		bool                                 failed = false;
		float                                adapted = 0.0f;  // 0: no reading yet
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
		ID3DBlob* blob = nullptr;
		ID3DBlob* errors = nullptr;
		meter.failed = FAILED(::D3DCompile(source, sizeof(source) - 1, "SeamlessMeter", nullptr, nullptr, "CS", "cs_5_0", 0, 0, &blob, &errors));
		if (errors) {
			logger::error("scene brightness: shader failed: {}", static_cast<const char*>(errors->GetBufferPointer()));
			errors->Release();
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

namespace Portal::Snapshot
{
	void MeasureScene(void* a_scene, float a_width, float a_height)
	{
		auto* context = Context();
		auto* device = Device();
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
		Upload(context, meter.constants, size, sizeof(size));
		// The scene may still be bound as a target, and a view of it then reads as nothing.
		const SavedState        saved(context);
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

	std::optional<float> SceneBrightness()
	{
		return meter.adapted > 0.0f ? std::optional(meter.adapted) : std::nullopt;
	}

	std::optional<float> RoomBrightness(RE::FormID a_door)
	{
		auto*       device = Device();
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

	void Flush()
	{
		if (!readbacks.empty()) {
			if (auto* context = Context()) {
				WriteReadbacks(context);
			}
		}
	}

	std::optional<CaptureCamera> TakeCaptureRequest()
	{
		return std::exchange(captureWanted, std::nullopt);
	}

	CaptureCamera CameraFor(RE::FormID a_door, const Corners& a_corners, const RE::NiPoint3& a_eye)
	{
		return ::CameraFor(a_door, Frame(a_corners, a_eye), kRoomCamera, PlaceOf(kRoomCamera, 0.0f));
	}

	void Prewarm()
	{
		if (auto* device = Device()) {
			const auto started = std::chrono::steady_clock::now();
			EnsurePipeline(device);
			logger::info("portal: doorway picture shaders made ahead ({:.1f} ms)", std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count());
		}
	}

	bool HasPicture(RE::FormID a_door, bool a_outdoors)
	{
		auto* device = Device();
		return device && !pipeline.failed && Find(device, a_door, a_outdoors);
	}

	bool CameraPictureWanted(RE::FormID a_door, const Corners& a_corners)
	{
		auto* device = Device();
		if (!device || pipeline.failed) {
			return false;
		}
		const auto* picture = Find(device, a_door, false);
		const auto [width, height] = SizeFor(kRoomCamera.extent, pictureHeight.load(), AspectOf(a_corners));
		return !picture || picture->version < kVersion || picture->extent != kRoomCamera.extent || picture->width != width || picture->height != height;
	}

	void Capture(const View& a_view, const Source& a_source, RE::FormID a_door, bool a_outdoors, bool a_gate, const Corners& a_corners)
	{
		const auto& shape = CameraOf(a_gate);
		auto* context = Context();
		auto* device = Device();
		// Without the depth a picture would be drawn flat again, and kept so on disk.
		if (!context || !device || !a_source.color || !a_source.depth || !EnsurePipeline(device)) {
			return;
		}
		WriteReadbacks(context);
		const Frame frame(a_corners, a_view.eye);
		// Merged into the world, the space is only in the frame the player sees, and the walls
		// around the doorway are the player's space's: the picture is of the doorway, as seen.
		const bool withCamera = a_source.portalFar > 0.0f;
		const auto extent = withCamera ? shape.extent : kDoorwayOnly;
		const auto cameraPlace = a_source.captureCamera && wantedFrom.first == a_door ? wantedFrom.second : PlaceOf(shape, frame.PlaceOf(a_view.eye)[1]);
		std::array<std::array<float, 4>, 4> clip;
		if (withCamera) {
			const auto camera = CameraFor(a_door, frame, shape, cameraPlace);
			for (int i = 0; i < 4; ++i) {
				clip[i] = CameraClip(camera, frame, extent[i & 1], extent[2 + (i >> 1)]);
			}
		} else {
			// Seen well enough: the whole doorway on screen, from in front, big enough to fill the
			// picture.
			auto toDoorway = (a_corners[0] + a_corners[3]) * 0.5f - a_view.eye;
			toDoorway.Unitize();
			if (frame.inward.Dot(toDoorway) < 0.8f) {
				return;
			}
			for (int i = 0; i < 4; ++i) {
				clip[i] = ToClip(a_view, a_corners[i]);
				if (clip[i][3] <= 1.0f || std::abs(clip[i][0] / clip[i][3]) > 0.95f || std::abs(clip[i][1] / clip[i][3]) > 0.95f) {
					return;
				}
			}
			if (std::abs(clip[2][1] / clip[2][3] - clip[0][1] / clip[0][3]) < 0.5f) {
				return;
			}
		}

		const auto key = KeyOf(a_door, a_outdoors, PartOfDay());
		auto&      picture = pictures[key];
		picture.used = std::chrono::steady_clock::now();
		// The player's view sees less than the capture camera did.
		if (!withCamera && picture.view && picture.extent != kDoorwayOnly) {
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		const auto [width, height] = SizeFor(extent, pictureHeight.load(), AspectOf(a_corners));
		// One at another size or extent (the size was changed since, or it was taken with the
		// player's view) is taken again at once. The capture camera's is kept: from its fixed place
		// it comes out the same.
		const bool sized = picture.width == width && picture.height == height && picture.extent == extent && picture.version == kVersion;
		if (picture.view && sized && (withCamera || now - picture.taken < kRetake)) {
			return;
		}
		if (withCamera && !a_source.captureCamera) {
			captureWanted = CameraFor(a_door, frame, shape, cameraPlace);
			wantedFrom = { a_door, cameraPlace };
			return;
		}

		auto*                 sceneView = static_cast<ID3D11ShaderResourceView*>(a_source.color);
		ID3D11Resource*       sceneResource = nullptr;
		D3D11_TEXTURE2D_DESC  sceneDesc{};
		sceneView->GetResource(&sceneResource);
		if (!sceneResource) {
			return;
		}
		static_cast<ID3D11Texture2D*>(sceneResource)->GetDesc(&sceneDesc);
		sceneResource->Release();
		if (!EnsureTarget(device, width, height)) {
			return;
		}
		if (picture.texture && !sized) {
			Release(picture.view);
			Release(picture.texture);
		}
		if (!picture.texture && !MakeView(device, picture, nullptr, width, height)) {
			return;
		}
		{
			const SavedState saved(context);
			float            constants[kCaptureConstants]{};
			for (int i = 0; i < 3; ++i) {
				std::memcpy(constants + i * 4, clip[i].data(), sizeof(float) * 4);
			}
			constants[12] = a_view.width;
			constants[13] = a_view.height;
			const bool fillsTarget = a_source.portalFar > 0.0f;
			constants[14] = 1.0f / (fillsTarget ? a_view.width : sceneDesc.Width);
			constants[15] = 1.0f / (fillsTarget ? a_view.height : sceneDesc.Height);
			constants[16] = static_cast<float>(width);
			constants[17] = static_cast<float>(height);
			std::memcpy(constants + 20, a_view.viewProjInverse, sizeof(float) * 16);
			for (int row = 0; row < 4; ++row) {
				constants[36 + row] = a_view.viewProj[row * 4 + 3];
			}
			const auto origin = frame.origin - a_view.eye;
			constants[40] = origin.x;
			constants[41] = origin.y;
			constants[42] = origin.z;
			constants[44] = frame.inward.x;
			constants[45] = frame.inward.y;
			constants[46] = frame.inward.z;
			constants[48] = a_source.portalNear;
			constants[49] = a_source.portalFar;
			constants[50] = fillsTarget ? 1.0f : 0.0f;
			constants[51] = withCamera ? shape.back : 0.0f;
			Upload(context, pipeline.captureConstants, constants, sizeof(constants));
			// The scene may still be bound as a target.
			ID3D11RenderTargetView* noTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, noTargets, nullptr);
			context->OMSetRenderTargets(1, &pipeline.targetView, nullptr);
			context->OMSetDepthStencilState(pipeline.noDepth, 0);
			context->OMSetBlendState(pipeline.opaque, nullptr, 0xFFFFFFFF);
			context->RSSetState(pipeline.raster);
			const D3D11_VIEWPORT viewport{ 0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1 };
			context->RSSetViewports(1, &viewport);
			context->VSSetShader(pipeline.captureVS, nullptr, 0);
			context->PSSetShader(pipeline.capturePS, nullptr, 0);
			context->GSSetShader(nullptr, nullptr, 0);
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			context->IASetInputLayout(nullptr);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ID3D11ShaderResourceView* sources[2]{ sceneView, static_cast<ID3D11ShaderResourceView*>(a_source.depth) };
			context->PSSetShaderResources(0, 2, sources);
			context->PSSetSamplers(0, 1, &pipeline.sampler);
			context->PSSetConstantBuffers(0, 1, &pipeline.captureConstants);
			context->Draw(3, 0);
			ID3D11ShaderResourceView* none[2]{};
			context->PSSetShaderResources(0, 2, none);
		}
		context->CopySubresourceRegion(picture.texture, 0, 0, 0, 0, pipeline.target, 0, nullptr);
		context->GenerateMips(picture.view);
		picture.taken = now;
		picture.diskTried = true;
		picture.depth = true;
		picture.takenFrom = withCamera ? cameraPlace : frame.PlaceOf(a_view.eye);
		picture.extent = extent;
		picture.lightScale = std::max(a_source.lightScale, 0.01f);
		picture.version = kVersion;
		D3D11_TEXTURE2D_DESC staging{ width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0 };
		ID3D11Texture2D*     copy = nullptr;
		if (SUCCEEDED(device->CreateTexture2D(&staging, nullptr, &copy))) {
			context->CopyResource(copy, pipeline.target);
			readbacks.push_back({ key, copy, width, height, picture.takenFrom, extent, picture.lightScale, 0, picture.taken });
		}
		logger::info("doorway snapshot: {} taken ({}x{}, {})", key, width, height, withCamera ? "capture camera" : "the player's view");
	}

	void Draw(const View& a_view, RE::FormID a_door, bool a_outdoors, bool a_gate, const Corners& a_corners, float a_alpha, const RE::NiColor& a_plain, const Adaptation& a_adaptation)
	{
		auto* context = Context();
		auto* device = Device();
		if (!context || !device || a_alpha <= 0.0f || !EnsurePipeline(device)) {
			return;
		}
		WriteReadbacks(context);
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto& rendererData = renderer->GetRuntimeData();
		auto* sceneTarget = reinterpret_cast<ID3D11RenderTargetView*>(rendererData.renderTargets[RE::RENDER_TARGETS::kMAIN].RTV);
		auto* motionTarget = reinterpret_cast<ID3D11RenderTargetView*>(rendererData.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].RTV);
		auto* depthView = reinterpret_cast<ID3D11DepthStencilView*>(renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].views[0]);
		if (!sceneTarget || !depthView) {
			return;
		}
		const auto* picture = Find(device, a_door, a_outdoors);

		const SavedState saved(context);
		{
			// Corner order bottom, bottom across, top, top across: a strip; the picture's top row is v = 0.
			const float uv[4][2] = { { 0, 1 }, { 1, 1 }, { 0, 0 }, { 1, 0 } };
			float       vertices[4][5];
			for (int i = 0; i < 4; ++i) {
				vertices[i][0] = a_corners[i].x - a_view.eye.x;
				vertices[i][1] = a_corners[i].y - a_view.eye.y;
				vertices[i][2] = a_corners[i].z - a_view.eye.z;
				vertices[i][3] = uv[i][0];
				vertices[i][4] = uv[i][1];
			}
			Upload(context, pipeline.vertices, vertices, sizeof(vertices));
			float constants[kDrawConstants]{};
			std::memcpy(constants, a_view.viewProj, sizeof(float) * 16);
			std::memcpy(constants + 16, a_view.viewProjUnjittered, sizeof(float) * 16);
			std::memcpy(constants + 32, a_view.previousViewProjUnjittered, sizeof(float) * 16);
			constants[48] = a_view.eyeDelta.x;
			constants[49] = a_view.eyeDelta.y;
			constants[50] = a_view.eyeDelta.z;
			std::memcpy(constants + 52, a_view.fog.data(), sizeof(float) * 4);
			std::memcpy(constants + 64, a_view.fog.data() + 4, sizeof(float) * 3);
			std::memcpy(constants + 68, a_view.fog.data() + 8, sizeof(float) * 3);
			constants[56] = a_plain.red;
			constants[57] = a_plain.green;
			constants[58] = a_plain.blue;
			constants[59] = picture ? 1.0f : 0.0f;
			constants[60] = std::clamp(a_alpha, 0.0f, 1.0f);
			constants[61] = picture ? 1.0f / picture->width : 0.0f;
			constants[62] = picture ? 1.0f / picture->height : 0.0f;
			const Frame frame(a_corners, a_view.eye);
			const auto  takenFrom = picture && picture->depth ? picture->takenFrom : kPlainEye;
			const auto  put = [&](int a_at, const RE::NiPoint3& a_value, float a_w) {
				constants[a_at] = a_value.x;
				constants[a_at + 1] = a_value.y;
				constants[a_at + 2] = a_value.z;
				constants[a_at + 3] = a_w;
			};
			put(72, frame.origin - a_view.eye, picture && picture->depth ? 1.0f : 0.0f);
			put(76, frame.across, 1.0f / frame.across.SqrLength());
			put(80, frame.up, 1.0f / frame.up.SqrLength());
			put(84, frame.inward, takenFrom[2]);
			put(88, frame.PointAt(takenFrom) - a_view.eye, a_outdoors ? kPlainBehindOutdoors : kPlainBehindRoom);
			constants[92] = static_cast<float>(std::to_underlying(a_adaptation.kind));
			constants[93] = std::clamp(a_adaptation.daylight, 0.0f, 1.0f);
			constants[94] = a_outdoors ? 1.0f : 0.0f;
			constants[95] = picture ? a_adaptation.lightScale / picture->lightScale : 1.0f;
			const auto& extent = picture ? picture->extent : kDoorwayOnly;
			std::memcpy(constants + 96, extent.data(), sizeof(float) * 4);
			constants[100] = a_gate ? 1.0f : 0.0f;
			Upload(context, pipeline.drawConstants, constants, sizeof(constants));
		}
		// Opaque, it stands in for the doorway like geometry (depth and motion); fading out, it only
		// lies over what the real space drew there.
		const bool              opaque = a_alpha >= 0.999f;
		ID3D11RenderTargetView* noTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, noTargets, nullptr);
		ID3D11RenderTargetView* targets[2] = { sceneTarget, opaque ? motionTarget : nullptr };
		context->OMSetRenderTargets(opaque && motionTarget ? 2 : 1, targets, depthView);
		context->OMSetDepthStencilState(opaque ? pipeline.depthWrite : pipeline.depthTest, 0);
		context->OMSetBlendState(pipeline.blend, nullptr, 0xFFFFFFFF);
		context->RSSetState(pipeline.raster);
		const D3D11_VIEWPORT viewport{ 0, 0, a_view.width, a_view.height, 0, 1 };
		context->RSSetViewports(1, &viewport);
		context->VSSetShader(pipeline.drawVS, nullptr, 0);
		context->PSSetShader(pipeline.drawPS, nullptr, 0);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->IASetInputLayout(pipeline.layout);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		const UINT stride = sizeof(float) * 5, offset = 0;
		context->IASetVertexBuffers(0, 1, &pipeline.vertices, &stride, &offset);
		auto* pictureView = picture ? picture->view : nullptr;
		context->PSSetShaderResources(0, 1, &pictureView);
		context->PSSetSamplers(0, 1, &pipeline.sampler);
		context->PSSetConstantBuffers(0, 1, &pipeline.drawConstants);
		context->VSSetConstantBuffers(0, 1, &pipeline.drawConstants);
		context->Draw(4, 0);
	}

	json Describe()
	{
		std::size_t inMemory = 0;
		for (const auto& [key, picture] : pictures) {
			inMemory += picture.view ? 1 : 0;
		}
		const int height = pictureHeight.load();
		return { { "pictures", inMemory }, { "readbacks", readbacks.size() }, { "failed", pipeline.failed }, { "size", std::format("{}x{}", WidthFor(height), height) } };
	}

	int PictureHeight()
	{
		return pictureHeight.load();
	}

	void SetPictureHeight(int a_height)
	{
		// A multiple of 4, so that the width (three quarters) is whole.
		pictureHeight = std::clamp(a_height, kMinHeight, kMaxHeight) / 4 * 4;
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
}
