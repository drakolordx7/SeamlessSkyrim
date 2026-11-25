#include "Skyrim/Pictures/DoorwaySnapshot.h"

#include "Skyrim/Engine/CoreConvert.h"
#include "Skyrim/Pictures/Cache.h"
#include "Skyrim/Pictures/Gpu.h"
#include "Skyrim/Render/D3D.h"

namespace Pictures
{
	namespace
	{
		using Camera::DoorwayPlace;

		// The plain colour stands this far behind the doorway, seen from in front of its middle: a
		// back wall at a room's depth, the outdoors further off. Something for the eye to look past.
		constexpr float kPlainBehindRoom = 400.0f;
		constexpr float kPlainBehindOutdoors = 3000.0f;

		// Set by Capture, taken by the portal pass (TakeCaptureRequest); both on the render thread.
		std::optional<CaptureCamera> captureWanted;
		// Where that camera stood, for the frame drawn with it: placed again from the eye a frame on,
		// a gate's moved with the player and the picture's corners missed what was drawn.
		std::pair<RE::FormID, DoorwayPlace> wantedFrom{};

		void PutCorner(float (&a_out)[4], const Core::Vec3& a_value, float a_w)
		{
			a_out[0] = a_value.x;
			a_out[1] = a_value.y;
			a_out[2] = a_value.z;
			a_out[3] = a_w;
		}
	}

	std::optional<CaptureCamera> TakeCaptureRequest()
	{
		return std::exchange(captureWanted, std::nullopt);
	}

	CaptureCamera CameraFor(RE::FormID a_door, const Corners& a_corners, const RE::NiPoint3& a_eye)
	{
		return { a_door, Camera::For(Camera::Frame(ToCore(a_corners), ToCore(a_eye)), Camera::kRoomCamera, Camera::PlaceOf(Camera::kRoomCamera, 0.0f)) };
	}

	void Prewarm()
	{
		const auto started = std::chrono::steady_clock::now();
		Gpu::EnsurePipeline(Render::D3D::Device());
		logger::info("portal: doorway picture shaders made ahead ({:.1f} ms)", std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count());
	}

	bool HasPicture(RE::FormID a_door, bool a_outdoors)
	{
		return !Gpu::Failed() && Cache::Find(Render::D3D::Device(), a_door, a_outdoors);
	}

	bool CameraPictureWanted(RE::FormID a_door, const Corners& a_corners)
	{
		if (Gpu::Failed()) {
			return false;
		}
		const auto* picture = Cache::Find(Render::D3D::Device(), a_door, false);
		const auto [width, height] = Camera::SizeFor(Camera::kRoomCamera.extent, PictureHeight(), Camera::AspectOf(ToCore(a_corners)));
		return !picture || picture->version < Cache::kVersion || picture->extent != Camera::kRoomCamera.extent || picture->width != width || picture->height != height;
	}

	void Capture(const View& a_view, const Source& a_source, RE::FormID a_door, bool a_outdoors, bool a_gate, const Corners& a_corners)
	{
		const auto& shape = Camera::ShapeOf(a_gate);
		auto*       context = Render::D3D::Context();
		auto*       device = Render::D3D::Device();
		// Without the depth a picture would be drawn flat again, and kept so on disk.
		if (!a_source.color || !a_source.depth || !Gpu::EnsurePipeline(device)) {
			return;
		}
		Cache::WriteReadbacks(context);
		const auto          corners = ToCore(a_corners);
		const auto          eye = ToCore(a_view.eye);
		const Camera::Frame frame(corners, eye);
		// Merged into the world, the space is only in the frame the player sees, and the walls
		// around the doorway are the player's space's: the picture is of the doorway, as seen.
		const bool withCamera = a_source.portalFar > 0.0f;
		const auto extent = withCamera ? shape.extent : Camera::kDoorwayOnly;
		const auto cameraPlace = a_source.captureCamera && wantedFrom.first == a_door ? wantedFrom.second : Camera::PlaceOf(shape, frame.PlaceOf(eye)[1]);
		std::array<std::array<float, 4>, 4> clip;
		if (withCamera) {
			const auto camera = Camera::For(frame, shape, cameraPlace);
			for (int i = 0; i < 4; ++i) {
				clip[i] = Camera::Clip(camera, frame, extent[i & 1], extent[2 + (i >> 1)]);
			}
		} else {
			// Seen well enough: the whole doorway on screen, from in front, big enough to fill the
			// picture.
			auto toDoorway = (corners[0] + corners[3]) * 0.5f - eye;
			toDoorway.Unitize();
			if (frame.inward.Dot(toDoorway) < 0.8f) {
				return;
			}
			for (int i = 0; i < 4; ++i) {
				clip[i] = Render::D3D::ClipOf(a_view.viewProj, a_view.eye, a_corners[i]);
				if (clip[i][3] <= 1.0f || std::abs(clip[i][0] / clip[i][3]) > 0.95f || std::abs(clip[i][1] / clip[i][3]) > 0.95f) {
					return;
				}
			}
			if (std::abs(clip[2][1] / clip[2][3] - clip[0][1] / clip[0][3]) < 0.5f) {
				return;
			}
		}

		const auto key = Cache::KeyOf(a_door, a_outdoors, Cache::PartOfDay());
		auto&      picture = Cache::Entry(key);
		picture.used = std::chrono::steady_clock::now();
		// The player's view sees less than the capture camera did.
		if (!withCamera && picture.view && picture.extent != Camera::kDoorwayOnly) {
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		const auto [width, height] = Camera::SizeFor(extent, PictureHeight(), Camera::AspectOf(corners));
		// One at another size or extent (the size was changed since, or it was taken with the
		// player's view) is taken again at once. The capture camera's is kept: from its fixed place
		// it comes out the same.
		const bool sized = picture.width == width && picture.height == height && picture.extent == extent && picture.version == Cache::kVersion;
		if (picture.view && sized && (withCamera || now - picture.taken < Cache::kRetake)) {
			return;
		}
		if (withCamera && !a_source.captureCamera) {
			captureWanted = CaptureCamera{ a_door, Camera::For(frame, shape, cameraPlace) };
			wantedFrom = { a_door, cameraPlace };
			return;
		}

		auto*                sceneView = static_cast<ID3D11ShaderResourceView*>(a_source.color);
		ID3D11Resource*      sceneResource = nullptr;
		D3D11_TEXTURE2D_DESC sceneDesc{};
		sceneView->GetResource(&sceneResource);
		static_cast<ID3D11Texture2D*>(sceneResource)->GetDesc(&sceneDesc);
		sceneResource->Release();
		if (!Gpu::EnsureTarget(device, width, height)) {
			return;
		}
		if (picture.texture && !sized) {
			Cache::ReleaseTexture(picture);
		}
		if (!picture.texture && !Cache::MakeView(device, picture, nullptr, width, height)) {
			return;
		}
		{
			Gpu::CaptureConstants constants{};
			for (int i = 0; i < 3; ++i) {
				std::ranges::copy(clip[i], constants.corner[i]);
			}
			constants.sizes[0] = a_view.width;
			constants.sizes[1] = a_view.height;
			const bool fillsTarget = a_source.portalFar > 0.0f;
			constants.sizes[2] = 1.0f / (fillsTarget ? a_view.width : sceneDesc.Width);
			constants.sizes[3] = 1.0f / (fillsTarget ? a_view.height : sceneDesc.Height);
			constants.out[0] = static_cast<float>(width);
			constants.out[1] = static_cast<float>(height);
			std::copy_n(a_view.viewProjInverse, 16, constants.inverseViewProj);
			for (int row = 0; row < 4; ++row) {
				constants.wColumn[row] = a_view.viewProj[row * 4 + 3];
			}
			PutCorner(constants.origin, frame.origin - eye, 0.0f);
			PutCorner(constants.inward, frame.inward, 0.0f);
			constants.portal[0] = a_source.portalNear;
			constants.portal[1] = a_source.portalFar;
			constants.portal[2] = fillsTarget ? 1.0f : 0.0f;
			constants.portal[3] = withCamera ? shape.back : 0.0f;
			Gpu::Capture(context, constants, sceneView, static_cast<ID3D11ShaderResourceView*>(a_source.depth), width, height);
		}
		context->CopySubresourceRegion(picture.texture, 0, 0, 0, 0, Gpu::Target(), 0, nullptr);
		context->GenerateMips(picture.view);
		picture.taken = now;
		picture.diskTried = true;
		picture.depth = true;
		picture.takenFrom = withCamera ? cameraPlace : frame.PlaceOf(eye);
		picture.extent = extent;
		picture.lightScale = std::max(a_source.lightScale, 0.01f);
		picture.version = Cache::kVersion;
		D3D11_TEXTURE2D_DESC staging{ width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0 };
		ID3D11Texture2D*     copy = nullptr;
		if (SUCCEEDED(device->CreateTexture2D(&staging, nullptr, &copy))) {
			context->CopyResource(copy, Gpu::Target());
			Cache::Queue({ key, copy, width, height, picture.takenFrom, extent, picture.lightScale, 0, picture.taken });
		}
		logger::info("doorway snapshot: {} taken ({}x{}, {})", key, width, height, withCamera ? "capture camera" : "the player's view");
	}

	void Draw(const View& a_view, RE::FormID a_door, bool a_outdoors, bool a_gate, const Corners& a_corners, float a_alpha, const RE::NiColor& a_plain, const Adaptation& a_adaptation)
	{
		auto* context = Render::D3D::Context();
		auto* device = Render::D3D::Device();
		if (a_alpha <= 0.0f || !Gpu::EnsurePipeline(device)) {
			return;
		}
		Cache::WriteReadbacks(context);
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto& rendererData = renderer->GetRuntimeData();
		auto* sceneTarget = reinterpret_cast<ID3D11RenderTargetView*>(rendererData.renderTargets[RE::RENDER_TARGETS::kMAIN].RTV);
		auto* motionTarget = reinterpret_cast<ID3D11RenderTargetView*>(rendererData.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].RTV);
		auto* depthView = reinterpret_cast<ID3D11DepthStencilView*>(renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].views[0]);
		if (!sceneTarget || !depthView) {
			return;
		}
		const auto* picture = Cache::Find(device, a_door, a_outdoors);

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
		Gpu::DrawConstants constants{};
		std::copy_n(a_view.viewProj, 16, constants.viewProj);
		std::copy_n(a_view.viewProjUnjittered, 16, constants.viewProjUnjittered);
		std::copy_n(a_view.previousViewProjUnjittered, 16, constants.previousViewProj);
		PutCorner(constants.eyeDelta, ToCore(a_view.eyeDelta), 0.0f);
		constants.fog = Pictures::Hlsl::FogConstants::From(a_view.fog);
		constants.plain[0] = a_plain.red;
		constants.plain[1] = a_plain.green;
		constants.plain[2] = a_plain.blue;
		constants.plain[3] = picture ? 1.0f : 0.0f;
		constants.params[0] = std::clamp(a_alpha, 0.0f, 1.0f);
		constants.params[1] = picture ? 1.0f / picture->width : 0.0f;
		constants.params[2] = picture ? 1.0f / picture->height : 0.0f;
		const auto          eye = ToCore(a_view.eye);
		const Camera::Frame frame(ToCore(a_corners), eye);
		const auto          takenFrom = picture && picture->depth ? picture->takenFrom : Camera::kPlainEye;
		PutCorner(constants.origin, frame.origin - eye, picture && picture->depth ? 1.0f : 0.0f);
		PutCorner(constants.across, frame.across, 1.0f / frame.across.SqrLength());
		PutCorner(constants.up, frame.up, 1.0f / frame.up.SqrLength());
		PutCorner(constants.inward, frame.inward, takenFrom[2]);
		PutCorner(constants.takenFrom, frame.PointAt(takenFrom) - eye, a_outdoors ? kPlainBehindOutdoors : kPlainBehindRoom);
		constants.look[0] = static_cast<float>(std::to_underlying(a_adaptation.kind));
		constants.look[1] = std::clamp(a_adaptation.daylight, 0.0f, 1.0f);
		constants.look[2] = a_outdoors ? 1.0f : 0.0f;
		constants.look[3] = picture ? a_adaptation.lightScale / picture->lightScale : 1.0f;
		const auto& extent = picture ? picture->extent : Camera::kDoorwayOnly;
		std::ranges::copy(extent, constants.extent);
		constants.gate[0] = a_gate ? 1.0f : 0.0f;
		Gpu::Draw(context, constants, vertices, { sceneTarget, motionTarget, depthView, a_view.width, a_view.height }, picture ? picture->view : nullptr, a_alpha >= 0.999f);
	}

	void Flush()
	{
		if (Cache::HasReadbacks()) {
			Cache::WriteReadbacks(Render::D3D::Context());
		}
	}

	json Describe()
	{
		const auto counts = Cache::Count();
		const int  height = PictureHeight();
		return { { "pictures", counts.inMemory }, { "readbacks", counts.readbacks }, { "failed", Gpu::Failed() }, { "size", std::format("{}x{}", Camera::WidthFor(height), height) } };
	}
}
