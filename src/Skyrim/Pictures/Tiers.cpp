#include "Skyrim/Pictures/Tiers.h"

#include "Core/Pictures/Exposure.h"
#include "Skyrim/Pictures/Cache.h"
#include "Skyrim/Pictures/DoorwayPictures.h"
#include "Skyrim/Pictures/Gpu.h"
#include "Skyrim/Render/D3D.h"
#include "Skyrim/Render/SceneMeter.h"

namespace Pictures
{
	namespace
	{
		struct Asked
		{
			Doorway doorway;
			Tier    tier;
		};

		// A handful at a time: a flat vector.
		std::vector<Asked> requests;

		bool IsLayered(Tier a_tier)
		{
			return a_tier == Tier::kDepthImage;
		}

		// The rows the image tier's picture has at the size set.
		UINT RowsOf(const Doorway& a_doorway)
		{
			const auto& extent = Camera::ShapeOf(a_doorway.gate).extent;
			return static_cast<UINT>(std::lround(PictureHeight() * (extent[3] - extent[2])));
		}

		// The next view of the door's depth image still to take: the ones in are kept, a picture at
		// another size or number of views begins again.
		int NextLayer(const Doorway& a_doorway)
		{
			const auto* entry = Cache::FindLayered(Render::D3D::Device(), a_doorway.door, a_doorway.outdoors);
			const int   count = DepthLayers();
			const auto [width, height] = Layers::SizeOf(PictureHeight());
			if (!entry || entry->layers != count || entry->width != width || entry->height != height) {
				return 0;
			}
			for (int layer = 0; layer < count; ++layer) {
				if (!(entry->captured & (1u << layer))) {
					return layer;
				}
			}
			return count;
		}
	}

	bool Offered(const Doorway& a_doorway, Tier a_tier)
	{
		return !Gpu::Failed() && (a_tier == Tier::kImage || (!a_doorway.gate && DepthLayers() > 0));
	}

	bool Ready(const Doorway& a_doorway, Tier a_tier)
	{
		if (!Offered(a_doorway, a_tier)) {
			return false;
		}
		auto* device = Render::D3D::Device();
		if (IsLayered(a_tier)) {
			const auto* entry = Cache::FindLayered(device, a_doorway.door, a_doorway.outdoors);
			return entry && entry->Complete();
		}
		return Cache::Find(device, a_doorway.door, a_doorway.outdoors) != nullptr;
	}

	bool Wanted(const Doorway& a_doorway, Tier a_tier)
	{
		if (!Offered(a_doorway, a_tier)) {
			return false;
		}
		auto* device = Render::D3D::Device();
		if (IsLayered(a_tier)) {
			const auto* entry = Cache::FindLayered(device, a_doorway.door, a_doorway.outdoors);
			const auto [width, height] = Layers::SizeOf(PictureHeight());
			return !entry || !entry->Complete() || entry->layers != DepthLayers() || entry->width != width || entry->height != height;
		}
		// Taken from the player's view (a doorway's own part of the plane) it is Ready, and a capture
		// camera's, which reaches past the doorway, is better.
		const auto* picture = Cache::FindExact(device, a_doorway.door, a_doorway.outdoors);
		return !picture || picture->extent != Camera::ShapeOf(a_doorway.gate).extent || picture->height != RowsOf(a_doorway);
	}

	void Request(const Doorway& a_doorway, Tier a_tier)
	{
		if (!Wanted(a_doorway, a_tier) || Requested(a_doorway, a_tier)) {
			return;
		}
		requests.push_back({ a_doorway, a_tier });
	}

	void Cancel(RE::FormID a_door)
	{
		std::erase_if(requests, [&](const Asked& a_asked) { return a_asked.doorway.door == a_door; });
	}

	bool Requested(const Doorway& a_doorway, Tier a_tier)
	{
		return std::ranges::any_of(requests, [&](const Asked& a_asked) { return a_asked.doorway.door == a_doorway.door && a_asked.tier == a_tier; });
	}

	std::vector<Pending> PendingRequests()
	{
		std::erase_if(requests, [](const Asked& a_asked) { return !Wanted(a_asked.doorway, a_asked.tier); });
		std::vector<Pending> out;
		for (const auto& asked : requests) {
			out.push_back({ asked.doorway, asked.tier });
		}
		return out;
	}

	void Retake(RE::FormID a_door, bool a_outdoors)
	{
		Cache::Invalidate(a_door, a_outdoors);
	}

	std::optional<CaptureCamera> NextRequestedCamera(RE::FormID a_portalDoor, const RE::NiPoint3& a_viewer)
	{
		// Done ones are dropped (a capture is taken a frame after its camera is asked for).
		std::erase_if(requests, [](const Asked& a_asked) { return !Wanted(a_asked.doorway, a_asked.tier); });
		for (const auto& asked : requests) {
			if (asked.doorway.door != a_portalDoor) {
				continue;
			}
			const auto corners = CaptureCorners(asked.doorway.door, asked.doorway.gate, a_viewer);
			if (asked.tier == Tier::kImage) {
				return CameraFor(asked.doorway.door, corners, a_viewer, asked.doorway.gate);
			}
			const int layer = NextLayer(asked.doorway);
			if (layer < DepthLayers()) {
				return LayerCameraFor(asked.doorway.door, layer, corners, a_viewer);
			}
		}
		return std::nullopt;
	}

	bool Draw(const Doorway& a_doorway, Tier a_tier, const View& a_view, const Corners& a_corners, float a_alpha)
	{
		if (!Ready(a_doorway, a_tier)) {
			return false;
		}
		const auto look = OutdoorLookNow();
		auto*      here = RE::PlayerCharacter::GetSingleton()->GetParentCell();
		const bool hereIndoors = here && here->IsInteriorCell();
		using Kind = Adaptation::Kind;
		Adaptation adaptation{ Kind::kNone, look.daylight };
		if (a_doorway.outdoors && hereIndoors) {
			adaptation.kind = Kind::kOutdoorsFromRoom;
		} else if (!a_doorway.outdoors && !hereIndoors) {
			adaptation.kind = Kind::kRoomFromOutdoors;
		}
		DrawOptions options;
		options.depthImage = IsLayered(a_tier);
		// A room as an eye adapted to the viewer's space sees it: a dim room from a sunny street is
		// dim. The street from a room has its own look (the shader's, by Adaptation::Kind), and a street
		// picture is of another hour and weather than the street now.
		if (!a_doorway.outdoors) {
			if (const auto scene = Render::SceneMeter::Brightness()) {
				options.exposure = Core::Pictures::Exposure::ViewScale(Cache::PictureBrightness(a_doorway.door, a_doorway.outdoors, IsLayered(a_tier)), *scene);
			}
		}
		Pictures::Draw(a_view, a_doorway.door, a_doorway.outdoors, a_doorway.gate, a_corners, a_alpha, a_doorway.outdoors ? look.plain : kPlainInside, adaptation, options);
		return true;
	}
}
