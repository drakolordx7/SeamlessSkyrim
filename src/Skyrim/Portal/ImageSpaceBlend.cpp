#include "Skyrim/Portal/ImageSpaceBlend.h"

#include "Core/ImageSpace/Blend.h"
#include "Skyrim/Doors/Pair.h"
#include "Skyrim/Engine/CoreConvert.h"
#include "Skyrim/Portal/Crossing.h"

namespace Portal::ImageSpaceBlend
{
	namespace
	{
		RE::ImageSpaceBaseData toneBlend;
		// The outdoors' as the engine last had it, with the weather and the hour it was taken in: it
		// holds for those. Kept for good, a save loaded indoors at night after a noon outdoors blended
		// the doorway with noon's, and lit the street seen from the room by noon's sun.
		RE::ImageSpaceBaseData toneOutdoors;
		const RE::TESWeather*  toneOutdoorsWeather = nullptr;
		float                  toneOutdoorsHour = 0.0f;  // in Calendar's hours passed
		bool                   toneOutdoorsLogged = false;
		RE::ImageSpaceBaseData toneIndoors;            // the interior the player is in, as the engine had it
		RE::TESObjectCELL*     toneIndoorsCell = nullptr;

		// The outdoors' image space before the player has been outdoors in this session (loaded indoors):
		// the current weather's for this part of the day. Without it the first walk outdoors had no blend
		// at all, and the street came in at twice the room's brightness at once.
		const RE::ImageSpaceBaseData* WeatherImageSpace()
		{
			auto* sky = RE::Sky::GetSingleton();
			auto* weather = sky ? (sky->currentWeather ? sky->currentWeather : sky->defaultWeather) : nullptr;
			if (!weather) {
				return nullptr;
			}
			using Time = RE::TESWeather::ColorTime;
			using Core::ImageSpace::WeatherTime;
			const auto time = [&] {
				switch (Core::ImageSpace::WeatherTimeOf(sky->currentGameHour)) {
				case WeatherTime::kNight:
					return Time::kNight;
				case WeatherTime::kSunrise:
					return Time::kSunrise;
				case WeatherTime::kDay:
					return Time::kDay;
				case WeatherTime::kSunset:
					return Time::kSunset;
				}
				std::unreachable();
			}();
			auto*       imageSpace = weather->imageSpaces[time];
			return imageSpace ? &imageSpace->data : nullptr;
		}

		const RE::ImageSpaceBaseData* CellImageSpace(RE::TESObjectCELL* a_cell)
		{
			auto* extra = a_cell ? a_cell->extraList.GetByType<RE::ExtraCellImageSpace>() : nullptr;
			return extra && extra->imageSpace ? &extra->imageSpace->data : nullptr;
		}

		// a_interiorDepth: how far the camera is inside the interior behind the doorway (negative: outside).
		// a_inDoorway: 1 with the camera in front of the doorway, falling to 0 beside, above or below it,
		// where the player's own space's image space applies alone.
		void UpdateTone(RE::TESObjectCELL* a_here, RE::TESObjectCELL* a_behindDoor, float a_interiorDepth, float a_inDoorway)
		{
			auto* manager = RE::ImageSpaceManager::GetSingleton();
			if (!manager || !a_here || !a_behindDoor || a_here->IsInteriorCell() == a_behindDoor->IsInteriorCell()) {
				Clear();
				return;
			}
			auto&      runtime = manager->GetRuntimeData();
			const bool ours = runtime.overrideBaseData == &toneBlend;
			if (runtime.overrideBaseData && !ours) {
				return;  // someone else's override (a script, the console)
			}
			const bool inside = a_here->IsInteriorCell();
			// Each side's own settings, never what the engine shows while our blend is on: that is our
			// blend itself. Taken from it, every frame blended the last frame's blend again, and when
			// the blend switched off the picture jumped back: the whole screen flashed near a doorway
			// (in first person, whose camera stays in the blend's range).
			// The outdoors' own only once the player has been out a while (kOutdoorsSettledFrames).
			static int framesOutdoors = 0;
			framesOutdoors = inside ? 0 : framesOutdoors + 1;
			if (!ours && runtime.currentBaseData && (inside || framesOutdoors > Core::ImageSpace::kOutdoorsSettledFrames)) {
				if (inside) {
					toneIndoors = *runtime.currentBaseData;
					toneIndoorsCell = a_here;
				} else {
					toneOutdoors = *runtime.currentBaseData;
					toneOutdoorsWeather = RE::Sky::GetSingleton()->currentWeather;
					toneOutdoorsHour = RE::Calendar::GetSingleton()->GetHoursPassed();
					if (!std::exchange(toneOutdoorsLogged, true)) {
						const auto* weather = WeatherImageSpace();
						const auto& e = toneOutdoors.hdr;
						logger::info("portal: outdoor image space eye adapt {:.2f}/{:.2f} white {:.2f} sun {:.2f} sky {:.2f} brightness {:.2f}; the weather's {:.2f}/{:.2f} {:.2f} {:.2f} {:.2f} {:.2f}",
							e.eyeAdaptSpeed, e.eyeAdaptStrength, e.white, e.sunlightScale, e.skyScale, toneOutdoors.cinematic.brightness, weather ? weather->hdr.eyeAdaptSpeed : -1.0f,
							weather ? weather->hdr.eyeAdaptStrength : -1.0f, weather ? weather->hdr.white : -1.0f, weather ? weather->hdr.sunlightScale : -1.0f,
							weather ? weather->hdr.skyScale : -1.0f, weather ? weather->cinematic.brightness : -1.0f);
					}
				}
			}
			const auto* indoors = inside ? (toneIndoorsCell == a_here ? &toneIndoors : CellImageSpace(a_here)) : CellImageSpace(a_behindDoor);
			const auto* outdoorsData = Outdoors();
			const float t = Core::ImageSpace::Weight(a_interiorDepth, a_inDoorway, inside);
			const bool  snapAdapt = !inside && framesOutdoors <= Core::ImageSpace::kAdaptSnapFrames;
			if (!indoors || !outdoorsData || Core::ImageSpace::Settled(t, inside)) {
				if (snapAdapt && (outdoorsData || ours || runtime.currentBaseData)) {
					if (outdoorsData) {
						toneBlend = *outdoorsData;
					} else if (!ours) {
						toneBlend = *runtime.currentBaseData;
					}
					toneBlend.hdr.eyeAdaptSpeed = Core::ImageSpace::kSnapSpeed;
					runtime.overrideBaseData = &toneBlend;
					return;
				}
				Clear();
				return;
			}
			toneBlend = inside ? *indoors : *outdoorsData;  // depth of field and the like: the current space's
			// HDR, cinematic and tint are plain floats (0x40 bytes).
			constexpr std::size_t kFloats = offsetof(RE::ImageSpaceBaseData, depthOfField) / sizeof(float);
			constexpr std::size_t keep[] = { offsetof(RE::ImageSpaceBaseData::HDR, eyeAdaptSpeed) / sizeof(float), offsetof(RE::ImageSpaceBaseData::HDR, eyeAdaptStrength) / sizeof(float) };
			Core::ImageSpace::Mix({ reinterpret_cast<float*>(&toneBlend), kFloats }, { reinterpret_cast<const float*>(outdoorsData), kFloats }, { reinterpret_cast<const float*>(indoors), kFloats }, t, keep);
			if (snapAdapt) {
				toneBlend.hdr.eyeAdaptSpeed = Core::ImageSpace::kSnapSpeed;
			}
			runtime.overrideBaseData = &toneBlend;
		}
	}

	const RE::ImageSpaceBaseData* Outdoors()
	{
		const bool indoors = RE::PlayerCharacter::GetSingleton()->GetParentCell()->IsInteriorCell();
		const bool holds = toneOutdoorsWeather && Core::ImageSpace::OutdoorsHolds(toneOutdoorsWeather == RE::Sky::GetSingleton()->currentWeather, indoors,
		                                                                       RE::Calendar::GetSingleton()->GetHoursPassed() - toneOutdoorsHour);
		return holds ? &toneOutdoors : WeatherImageSpace();
	}

	void Update(const std::optional<Active>& a_ready)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* here = player ? player->GetParentCell() : nullptr;
		if (const auto mapping = a_ready && here ? Doors::TryCompute(*a_ready) : std::nullopt) {
			const auto doorway = Doors::DoorwayOf(*mapping);
			const auto camera = ToCore(Crossing::CameraPosition());
			const bool inside = here->IsInteriorCell();
			const float inDoorway = Core::ImageSpace::InDoorway(doorway, camera);
			UpdateTone(here, a_ready->cell, Core::ImageSpace::InteriorDepth(doorway, camera, inside), inDoorway);
		} else {
			Clear();
		}
	}

	void Clear()
	{
		auto* manager = RE::ImageSpaceManager::GetSingleton();
		if (manager && manager->GetRuntimeData().overrideBaseData == &toneBlend) {
			manager->GetRuntimeData().overrideBaseData = nullptr;
		}
	}

	bool On()
	{
		auto* manager = RE::ImageSpaceManager::GetSingleton();
		return manager && manager->GetRuntimeData().overrideBaseData == &toneBlend;
	}
}
