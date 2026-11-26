#include "Skyrim/Pictures/DoorwayPictures.h"

#include "Skyrim/Doors/Geometry.h"
#include "Skyrim/Doors/Pair.h"
#include "Skyrim/Engine/Fog.h"
#include "Skyrim/Engine/SameSpace.h"
#include "Skyrim/Pictures/DoorwaySnapshot.h"
#include "Skyrim/Pictures/Tiers.h"
#include "Skyrim/Portal/LightOcclusion.h"
#include "Skyrim/Portal/MergedView.h"
#include "Skyrim/Portal/PortalManager.h"
#include "Skyrim/Portal/Schedule.h"
#include "Skyrim/Portal/StandIns.h"
#include "Skyrim/Render/D3D.h"
#include "Skyrim/Render/MainView.h"
#include "Skyrim/Render/PortalPass.h"
#include "Skyrim/Render/SceneMeter.h"
#include "Skyrim/Render/SpaceLighting.h"

namespace Pictures
{
	Pictures::View SnapshotView()
	{
		auto&                  mainView = Render::CurrentMainView();
		const auto             eye = mainView.posAdjust.getEye();
		const auto             previousEye = mainView.previousPosAdjust.getEye();
		const auto             view = mainView.cameraData.getEye();
		Pictures::View         out;
		std::memcpy(out.viewProj, &view.viewProjMat, sizeof(out.viewProj));
		double matrix[16], inverse[16];
		for (int i = 0; i < 16; ++i) {
			matrix[i] = out.viewProj[i];
		}
		if (Render::D3D::Invert(matrix, inverse)) {
			for (int i = 0; i < 16; ++i) {
				out.viewProjInverse[i] = static_cast<float>(inverse[i]);
			}
		}
		std::memcpy(out.viewProjUnjittered, &view.viewProjMatrixUnjittered, sizeof(out.viewProjUnjittered));
		std::memcpy(out.previousViewProjUnjittered, &view.previousViewProjMatrixUnjittered, sizeof(out.previousViewProjUnjittered));
		out.eye = { eye.x, eye.y, eye.z };
		out.eyeDelta = { eye.x - previousEye.x, eye.y - previousEye.y, eye.z - previousEye.z };
		auto&                scene = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		D3D11_TEXTURE2D_DESC desc{};
		if (scene.texture) {
			reinterpret_cast<ID3D11Texture2D*>(scene.texture)->GetDesc(&desc);
		}
		const auto scale = Render::D3D::RenderScale();
		out.width = desc.Width * scale[0];
		out.height = desc.Height * scale[1];
		out.fog = Engine::Fog::World();
		return out;
	}

	Pictures::Corners CaptureCorners(RE::FormID a_door, bool a_gate, const RE::NiPoint3& a_viewer)
	{
		return Portal::Doors::Quad(Portal::Doors::Door(a_door), a_viewer, { .behindPanel = !a_gate, .wholeMesh = a_gate });
	}

	namespace
	{
		// One door at a time: a door that throws (unloaded meanwhile) leaves the others drawn.
		template <class F>
		void Guarded(F&& a_draw)
		{
			try {
				a_draw();
			} catch (const std::exception& e) {
				static int reported = 0;
				if (reported++ < 10) {
					logger::warn("doorway snapshot: {}", e.what());
				}
			}
		}

		// The picture the portal pass drew this frame with the capture camera.
		void TakeCameraPicture(const Pictures::View& a_view, const Render::PortalPass::CaptureDrawn& a_drawn, const RE::NiPoint3& a_viewer)
		{
			// Not while Lux's lights not yet known to stand clear of some of the room were half on there;
			// the manager asks again, and the next capture frame measures further.
			if (Portal::LightOcclusion::UnmeasuredThisFrame() > 0) {
				return;
			}
			// Whether or not the doorway is settled otherwise (a room waits to be merged for this).
			Guarded([&] {
				Pictures::Source source{ Render::PortalPass::DrawingTargets().color.SRV, Render::PortalPass::DrawingTargets().depth.depthSRV, a_drawn.nearClip, a_drawn.farClip, true };
				source.ahead = a_drawn.ahead;
				const auto             corners = CaptureCorners(a_drawn.door, a_drawn.gate, a_viewer);
				if (a_drawn.layer >= 0) {
					Pictures::CaptureLayer(a_view, source, a_drawn.door, a_drawn.outdoors, a_drawn.layer, corners);
				} else {
					Pictures::Capture(a_view, source, a_drawn.door, a_drawn.outdoors, a_drawn.gate, corners);
				}
			});
		}

		// The doorway shown open on the real space, photographed: from the world's own frame when the
		// space is merged, else from the portal pass's.
		void TakeViewPicture(const Pictures::View& a_view, const Portal::StandIns::DoorwayFill& a_chance, bool a_merged, const RE::NiPoint3& a_viewer)
		{
			Guarded([&] {
				Pictures::Source source;
				auto*            renderer = RE::BSGraphics::Renderer::GetSingleton();
				if (a_merged) {
					source.color = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].SRV;
					source.depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
					auto* here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
					if (!a_chance.outdoors && here && !here->IsInteriorCell()) {
						source.lightScale = Portal::MergedView::RoomLightScale(a_chance.door);
					}
				} else {
					source = { Render::PortalPass::ShownTargets().color.SRV, Render::PortalPass::ShownTargets().depth.depthSRV, Render::PortalPass::NearClip(), Render::PortalPass::FarClip(), false };
				}
				Pictures::Capture(a_view, source, a_chance.door, a_chance.outdoors, a_chance.gate, CaptureCorners(a_chance.door, a_chance.gate, a_viewer));
			});
		}
	}

	OutdoorLook OutdoorLookNow()
	{
		if (const auto weather = Render::SpaceLighting::WeatherLighting()) {
			// The horizon's colour for the hour, half way to grey: the sky alone read as a blue
			// panel, a street is mostly walls and ground under it.
			const auto& sky = weather->sky.horizon;
			const float grey = 0.2126f * sky.red + 0.7152f * sky.green + 0.0722f * sky.blue;
			// The horizon rather than the sun: under heavy cloud the sun's colour is dim by day, and
			// a street is still far brighter than a room.
			return { { 0.5f * (sky.red + grey), 0.5f * (sky.green + grey), 0.5f * (sky.blue + grey) }, std::clamp((grey - 0.1f) / 0.3f, 0.0f, 1.0f) };
		}
		auto*       calendar = RE::Calendar::GetSingleton();
		const float hour = calendar ? calendar->GetHour() : 12.0f;
		return { kPlainInside, std::clamp(std::min(hour - 5.0f, 21.0f - hour) / 3.0f, 0.0f, 1.0f) };
	}

	std::optional<Pictures::CaptureCamera> CaptureCameraFor(const Portal::Active& a_portal, bool a_forFile)
	{
		// Taken here, so a pass drawn again is the capture camera's too.
		auto capture = Pictures::TakeCaptureRequest();
		// Every other frame at most: a room takes several to measure its lights for (UpdateDoorwayPictures),
		// and the doorway on screen stays still while one is drawn.
		static bool capturedLastFrame = false;
		auto*       camera = RE::Main::WorldRootCamera();
		if (const auto door = Portal::StandIns::PictureWanted(); !capture && door == a_portal.sourceDoor && !capturedLastFrame) {
			capture = Pictures::CameraFor(door, Portal::Doors::Quad(Portal::Doors::Door(door), camera->world.translate, { .behindPanel = true }), camera->world.translate);
		}
		// What a scheduler asked for (Tiers.h): the image tier's picture, or the depth image's next view.
		if (!capture && !capturedLastFrame && !a_forFile) {
			capture = Pictures::NextRequestedCamera(a_portal.sourceDoor, camera->world.translate);
		}
		// A diagnostic file wants the player's view; the request is asked for again.
		if (capture && (a_forFile || capture->door != a_portal.sourceDoor)) {
			capture.reset();
		}
		capturedLastFrame = capture.has_value();
		return capture;
	}

	void UpdateDoorwayPictures(bool a_realShown)
	{
		Pictures::Flush();
		const auto view = SnapshotView();
		// Before any stand-in picture is drawn into it: the eye adapts to the real view.
		Render::SceneMeter::Measure(RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].SRV, view.width, view.height);
		const auto  chance = Portal::StandIns::SnapshotChance();
		const auto  standIns = Portal::StandIns::Fills();
		const auto& captureDrawn = Render::PortalPass::CaptureDrawnThisFrame();
		if (!chance && standIns.empty() && !captureDrawn) {
			return;
		}
		auto*      camera = RE::Main::WorldRootCamera();
		const auto viewer = camera ? camera->world.translate : RE::NiPoint3{};
		// Merged, the real space is drawn with the world, into the main scene; otherwise by the
		// portal pass, which draws the picture with a camera of its own and needs no first person
		// (in third person the player's body stands in the doorway of the main view only).
		const bool merged = Portal::Manager::Merged();
		auto*      playerCamera = RE::PlayerCamera::GetSingleton();
		if (captureDrawn) {
			TakeCameraPicture(view, *captureDrawn, viewer);
		} else if (chance && a_realShown && (!merged || (playerCamera && playerCamera->IsInFirstPerson()))) {
			TakeViewPicture(view, *chance, merged, viewer);
		}
		const auto look = OutdoorLookNow();
		auto*      here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		const bool hereIndoors = here && here->IsInteriorCell();
		for (const auto& standIn : standIns) {
			Guarded([&] {
				// Shown: an always-open door of the street merged into a room is drawn moved with it.
				const auto corners = Portal::Doors::ShownQuad(Portal::Doors::Door(standIn.door), viewer, { .behindPanel = !standIn.gate, .wholeMesh = standIn.gate });
				// The best picture that is ready, no better than the scheduler holds the space behind the door at
				// (Schedule::DoorTier): the depth image from T2, else the image from T1.
				const Pictures::Doorway doorway{ standIn.door, standIn.outdoors, standIn.gate };
				const auto              tier = Portal::Schedule::DoorTier(standIn.door);
				// A door of the space merged into the player's, seen through the live doorway (a side room: the
				// inn across the street from inside a shop) has no tier of its own here, but its depth image, if
				// one was taken from the street, is what shows it in parallax at an angle.
				auto*      doorRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(standIn.door);
				const bool seenThrough = doorRef && !Engine::SameSpace(doorRef->GetParentCell(), here);
				if (((tier >= Core::Graph::Tier::DepthImage || seenThrough) && Pictures::Draw(doorway, Pictures::Tier::kDepthImage, view, corners, standIn.alpha)) ||
					(tier >= Core::Graph::Tier::Image && Pictures::Draw(doorway, Pictures::Tier::kImage, view, corners, standIn.alpha))) {
					return;
				}
				// A door never seen open: the plain colour, so the doorway isn't the void behind the door.
				using Kind = Pictures::Adaptation::Kind;
				Pictures::Adaptation adaptation{ Kind::kNone, look.daylight };
				if (standIn.outdoors && hereIndoors) {
					adaptation.kind = Kind::kOutdoorsFromRoom;
				} else if (!standIn.outdoors && !hereIndoors) {
					adaptation.kind = Kind::kRoomFromOutdoors;
					adaptation.lightScale = Portal::MergedView::RoomLightScale(standIn.door);
				}
				Pictures::Draw(view, standIn.door, standIn.outdoors, standIn.gate, corners, standIn.alpha, standIn.outdoors ? look.plain : kPlainInside, adaptation);
			});
		}
	}
}
