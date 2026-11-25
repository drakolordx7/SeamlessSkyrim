#include "Portal/ImageSpaceBlend.h"

#include "Doors/Pair.h"
#include "Portal/Crossing.h"

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
			const float hour = sky->currentGameHour;
			const auto  time = hour < 5.0f || hour >= 21.0f ? Time::kNight : hour < 8.0f ? Time::kSunrise : hour < 17.0f ? Time::kDay : Time::kSunset;
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
			// The outdoors' own only once the player has been out a while: taken in the first frames of the
			// first walk outdoors in a session, they were the engine's in-between values, and the blend
			// made of them darkened the street to near black for a second after it.
			static int framesOutdoors = 0;
			framesOutdoors = inside ? 0 : framesOutdoors + 1;
			if (!ours && runtime.currentBaseData && (inside || framesOutdoors > 30)) {
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
			constexpr float kFrom = -150.0f, kTo = 120.0f;
			float           t = std::clamp((a_interiorDepth - kFrom) / (kTo - kFrom), 0.0f, 1.0f);
			t = t * t * (3.0f - 2.0f * t);
			t = inside ? 1.0f - (1.0f - t) * a_inDoorway : t * a_inDoorway;
			// The eye adaptation walked out with was the room's: the street came in far too bright and
			// took seconds to settle. A load door starts it afresh behind its loading screen; here it is
			// let catch up with the street at once in the first frames outdoors.
			constexpr int kAdaptSnapFrames = 15;
			const bool    snapAdapt = !inside && framesOutdoors <= kAdaptSnapFrames;
			constexpr float kSnapSpeed = 1000.0f;  // times the frame time: past 1, the shader's step is the whole way
			if (!indoors || !outdoorsData || (inside ? t >= 1.0f : t <= 0.0f)) {
				if (snapAdapt && (outdoorsData || ours || runtime.currentBaseData)) {
					if (outdoorsData) {
						toneBlend = *outdoorsData;
					} else if (!ours) {
						toneBlend = *runtime.currentBaseData;
					}
					toneBlend.hdr.eyeAdaptSpeed = kSnapSpeed;
					runtime.overrideBaseData = &toneBlend;
					return;
				}
				Clear();
				return;
			}
			const auto mix = [t](float a_out, float a_in) { return a_out + (a_in - a_out) * t; };
			toneBlend = inside ? *indoors : *outdoorsData;  // depth of field and the like: the current space's
			const float* outdoors = reinterpret_cast<const float*>(outdoorsData);
			const float* in = reinterpret_cast<const float*>(indoors);
			float*       blend = reinterpret_cast<float*>(&toneBlend);
			// HDR, cinematic and tint are plain floats (0x40 bytes).
			for (std::size_t i = 0; i < offsetof(RE::ImageSpaceBaseData, depthOfField) / sizeof(float); ++i) {
				// Eye adaptation's speed and strength stay the current space's own. On the first walk outdoors
				// of a session the engine starts its adaptation afresh (the white frame FrameHooks' flash
				// guard replaces), and with these blended it then settled near black for a second.
				const bool adapt = i == offsetof(RE::ImageSpaceBaseData::HDR, eyeAdaptSpeed) / sizeof(float) || i == offsetof(RE::ImageSpaceBaseData::HDR, eyeAdaptStrength) / sizeof(float);
				if (adapt) {
					continue;
				}
				blend[i] = mix(outdoors[i], in[i]);
			}
			if (snapAdapt) {
				toneBlend.hdr.eyeAdaptSpeed = kSnapSpeed;
			}
			runtime.overrideBaseData = &toneBlend;
		}
	}

	const RE::ImageSpaceBaseData* Outdoors()
	{
		// The hour only counts indoors: outdoors it is taken every frame, except while our own blend
		// is on (UpdateTone), and standing at a doorway for half an hour mustn't swap it for the
		// weather's.
		constexpr float kHoldsForHours = 0.5f;
		const bool      indoors = RE::PlayerCharacter::GetSingleton()->GetParentCell()->IsInteriorCell();
		const bool      holds = toneOutdoorsWeather && toneOutdoorsWeather == RE::Sky::GetSingleton()->currentWeather &&
		                        (!indoors || std::abs(RE::Calendar::GetSingleton()->GetHoursPassed() - toneOutdoorsHour) < kHoldsForHours);
		return holds ? &toneOutdoors : WeatherImageSpace();
	}

	void Update(const std::optional<Active>& a_ready)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* here = player ? player->GetParentCell() : nullptr;
		bool  toned = false;
		if (const auto mapping = a_ready && here ? Doors::TryCompute(*a_ready) : std::nullopt) {
			const auto offset = Crossing::CameraPosition() - mapping->sourceCenter;
			const float depth = offset.x * mapping->inward.x + offset.y * mapping->inward.y;
			// Only in front of the doorway: by depth alone, anyone level with the door's plane
			// (walking along the street past an open door, or teleported along it) had the whole
			// outdoors drawn with the interior's image space: a white flash.
			const float side = std::abs(offset.x * mapping->inward.y - offset.y * mapping->inward.x);
			const float above = std::abs(offset.z) - mapping->halfHeight;
			const auto  fade = [](float a_beyond) { return 1.0f - std::clamp(a_beyond / 100.0f, 0.0f, 1.0f); };
			const float inDoorway = fade(side - mapping->halfWidth) * fade(above);
			// The source door's "inward" points away from the player's space: into the interior
			// from outside, out of it from inside.
			UpdateTone(here, a_ready->cell, here->IsInteriorCell() ? -depth : depth, inDoorway);
			toned = true;
		}
		if (!toned) {
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
