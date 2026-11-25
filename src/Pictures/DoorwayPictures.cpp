#include "Pictures/DoorwayPictures.h"

#include "Doors/DoorPair.h"
#include "Engine/Fog.h"
#include "Pictures/DoorwaySnapshot.h"
#include "Portal/LightOcclusion.h"
#include "Portal/PortalManager.h"
#include "Render/D3D.h"
#include "Render/MainView.h"
#include "Render/PortalPass.h"
#include "Render/SceneMeter.h"
#include "Render/SpaceLighting.h"

namespace Pictures
{
	namespace
	{
	// An interior never seen through its door: a dim grey, a little lighter than a dark room.
	constexpr RE::NiColor kPlainInside{ 0.08f, 0.08f, 0.08f };

	Portal::Snapshot::View SnapshotView()
	{
		auto&                  mainView = Render::CurrentMainView();
		const auto             eye = mainView.posAdjust.getEye();
		const auto             previousEye = mainView.previousPosAdjust.getEye();
		const auto             view = mainView.cameraData.getEye();
		Portal::Snapshot::View out;
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
	}

	void UpdateDoorwayPictures(bool a_realShown)
	{
		Portal::Snapshot::Flush();
		const auto view = SnapshotView();
		// Before any stand-in picture is drawn into it: the eye adapts to the real view.
		Render::SceneMeter::Measure(RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].SRV, view.width, view.height);
		const auto chance = Portal::Manager::SnapshotChance();
		const auto standIns = Portal::Manager::StandIns();
		const auto& captureDrawn = Render::PortalPass::CaptureDrawnThisFrame();
		if (!chance && standIns.empty() && !captureDrawn) {
			return;
		}
		auto*      camera = RE::Main::WorldRootCamera();
		const auto viewer = camera ? camera->world.translate : RE::NiPoint3{};
		// One door at a time: a door that throws (unloaded meanwhile) leaves the others drawn.
		const auto guarded = [](auto&& a_draw) {
			try {
				a_draw();
			} catch (const std::exception& e) {
				static int reported = 0;
				if (reported++ < 10) {
					logger::warn("doorway snapshot: {}", e.what());
				}
			}
		};
		// First person only: in third person the player's own body stands in the doorway.
		auto* playerCamera = RE::PlayerCamera::GetSingleton();
		// Merged, the real space is drawn with the world, into the main scene; otherwise by the
		// portal pass, which draws the picture with a camera of its own and needs no first person
		// (in third person the player's body stands in the doorway of the main view only).
		const bool merged = Portal::Manager::Merged();
		if (captureDrawn && Portal::LightOcclusion::UnmeasuredThisFrame() > 0) {
			// Lux's lights not yet known to stand clear of some of the room were half on there; the
			// manager asks again, and the next capture frame measures further.
		} else if (captureDrawn) {
			// Whether or not the doorway is settled (a room waits to be merged for this).
			guarded([&] {
				// The doorway as the picture was asked for with (the snapshot chance's, below): a
				// gate's with another shape was a picture of another size, never the one asked for,
				// and Whiterun's gate was captured every other frame for good, every other frame
				// showing the picture of the frame before (the view through it stuttered, and lagged
				// a turn by a frame half the time).
				const auto                     corners = Portal::Doors::Quad(Portal::Doors::Door(captureDrawn->door), viewer, !captureDrawn->gate, captureDrawn->gate);
				const Portal::Snapshot::Source source{ Render::PortalPass::DrawingTargets().color.SRV, Render::PortalPass::DrawingTargets().depth.depthSRV, captureDrawn->nearClip, captureDrawn->farClip, true };
				Portal::Snapshot::Capture(view, source, captureDrawn->door, captureDrawn->outdoors, captureDrawn->gate, corners);
			});
		} else if (chance && a_realShown && (!merged || (playerCamera && playerCamera->IsInFirstPerson()))) {
			guarded([&] {
				const auto               corners = Portal::Doors::Quad(Portal::Doors::Door(chance->door), viewer, !chance->gate, chance->gate);
				Portal::Snapshot::Source source;
				auto*                    renderer = RE::BSGraphics::Renderer::GetSingleton();
				if (merged) {
					source.color = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].SRV;
					source.depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
					auto* here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
					if (!chance->outdoors && here && !here->IsInteriorCell()) {
						source.lightScale = Portal::Manager::RoomLightScale(chance->door);
					}
				} else {
					source = { Render::PortalPass::ShownTargets().color.SRV, Render::PortalPass::ShownTargets().depth.depthSRV, Render::PortalPass::NearClip(), Render::PortalPass::FarClip(), false };
				}
				Portal::Snapshot::Capture(view, source, chance->door, chance->outdoors, chance->gate, corners);
			});
		}
		auto        plainOutside = kPlainInside;
		const auto  weather = Render::SpaceLighting::WeatherLighting();
		float       daylight;
		if (weather) {
			// The horizon's colour for the hour, half way to grey: the sky alone read as a blue
			// panel, a street is mostly walls and ground under it.
			const auto& sky = weather->sky.horizon;
			const float grey = 0.2126f * sky.red + 0.7152f * sky.green + 0.0722f * sky.blue;
			plainOutside = { 0.5f * (sky.red + grey), 0.5f * (sky.green + grey), 0.5f * (sky.blue + grey) };
			// The horizon rather than the sun: under heavy cloud the sun's colour is dim by day, and
			// a street is still far brighter than a room.
			daylight = std::clamp((grey - 0.1f) / 0.3f, 0.0f, 1.0f);
		} else {
			auto*       calendar = RE::Calendar::GetSingleton();
			const float hour = calendar ? calendar->GetHour() : 12.0f;
			daylight = std::clamp(std::min(hour - 5.0f, 21.0f - hour) / 3.0f, 0.0f, 1.0f);
		}
		auto*      here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		const bool hereIndoors = here && here->IsInteriorCell();
		for (const auto& standIn : standIns) {
			guarded([&] {
				// Shown: an always-open door of the street merged into a room is drawn moved with it.
				const auto corners = Portal::Doors::ShownQuad(Portal::Doors::Door(standIn.door), viewer, !standIn.gate, standIn.gate);
				using Kind = Portal::Snapshot::Adaptation::Kind;
				Portal::Snapshot::Adaptation adaptation{ Kind::kNone, daylight };
				if (standIn.outdoors && hereIndoors) {
					adaptation.kind = Kind::kOutdoorsFromRoom;
				} else if (!standIn.outdoors && !hereIndoors) {
					adaptation.kind = Kind::kRoomFromOutdoors;
					adaptation.lightScale = Portal::Manager::RoomLightScale(standIn.door);
				}
				Portal::Snapshot::Draw(view, standIn.door, standIn.outdoors, standIn.gate, corners, standIn.alpha, standIn.outdoors ? plainOutside : kPlainInside, adaptation);
			});
		}
	}
}
