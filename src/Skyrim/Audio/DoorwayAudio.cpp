#include "Skyrim/Audio/DoorwayAudio.h"

#include "Core/Audio/Doorway.h"
#include "Skyrim/Doors/Pair.h"
#include "Skyrim/Engine/CoreConvert.h"
#include "Skyrim/Plugin/Settings.h"
#include "Skyrim/Portal/PortalManager.h"

namespace Audio::Doorway
{
	namespace
	{
		namespace CA = Core::Audio;

		enum class Kind
		{
			kAcoustic,  // the room's acoustic space loop (an interior's ambience)
			kMarker,    // a sound marker, an activator's loop or a light's sound standing in the room
			kWeather,   // an exterior's weather loops, heard from indoors
		};

		constexpr const char* KindName(Kind a_kind)
		{
			constexpr const char* names[] = { "acoustic", "marker", "weather" };
			return names[static_cast<int>(a_kind)];
		}

		// The most sources played for one room: each is a voice of the sound engine.
		constexpr std::size_t kMaxVoices = 6;
		// Sources further than this from the doorway, inside the room, are left out.
		constexpr float kMaxSourceRange = 3000.0f;
		// A weather loop is background: how much of it the doorway carries against a source at it.
		constexpr float kWeatherShare = 0.6f;
		// A voice starts above this and stops below the lower one, so one near the edge doesn't flap.
		constexpr float kStartAbove = 0.03f;
		constexpr float kStopBelow = 0.012f;
		constexpr float kVolumeStep = 0.01f;
		constexpr float kMoveStep = 3.0f;
		constexpr std::uint16_t kStartFadeMs = 300;
		constexpr std::uint16_t kStopFadeMs = 250;
		constexpr std::uint16_t kQuickFadeMs = 150;
		// A sound that isn't a loop, ended: played again after this long.
		constexpr float kRestartSeconds = 3.0f;
		// A sound just started may not read as playing yet (the sound engine takes messages).
		constexpr float kStartSettleSeconds = 0.6f;

		struct Voice
		{
			Kind              kind;
			RE::FormID        sound;     // the descriptor
			float             roomGain;  // the share of the doorway's sound this source has, by its distance inside the room
			float             roomDistance;
			RE::BSSoundHandle handle;
			float             volume = 0.0f;
			float             age = 0.0f;
			float             restart = 0.0f;
			RE::NiPoint3      position;
			int               failedStarts = 0;  // the sound engine gave no handle
		};

		// The sounds of the live room being heard: the room, its sources and how loud it is now.
		struct Group
		{
			RE::FormID            cell = 0;
			std::uint32_t         generation = 0;
			RE::FormID            door = 0;
			std::vector<Voice>    voices;
			CA::Fade              fade;
			CA::Doorway           doorway;
			CA::Heard             heard;
			float                 open = 0.0f;
			float                 target = 0.0f;
		};

		bool                  enabled = true;
		int                   volume = 70;
		std::optional<Group>  group;

		const CA::Tuning tuning;

		RE::NiPoint3 ListenerPosition()
		{
			return RE::PlayerCamera::GetSingleton()->cameraRoot->world.translate;
		}

		void StopVoice(Voice& a_voice, std::uint16_t a_fadeMs)
		{
			if (a_voice.handle.IsValid()) {
				a_voice.handle.FadeOutAndRelease(a_fadeMs);
			}
			a_voice.handle = RE::BSSoundHandle();
			a_voice.volume = 0.0f;
		}

		void StopGroup(std::uint16_t a_fadeMs)
		{
			if (group) {
				for (auto& voice : group->voices) {
					StopVoice(voice, a_fadeMs);
				}
				group.reset();
			}
		}

		bool StartVoice(Voice& a_voice, float a_volume, const RE::NiPoint3& a_position)
		{
			auto* descriptor = RE::TESForm::LookupByID<RE::BGSSoundDescriptorForm>(a_voice.sound);
			RE::BSSoundHandle handle;
			if (!descriptor || !RE::BSAudioManager::GetSingleton()->GetSoundHandle(handle, descriptor)) {
				++a_voice.failedStarts;
				return false;
			}
			handle.SetPosition(a_position);
			handle.SetVolume(a_volume);
			handle.FadeInPlay(kStartFadeMs);
			a_voice.handle = handle;
			a_voice.volume = a_volume;
			a_voice.position = a_position;
			a_voice.age = 0.0f;
			return true;
		}

		bool Has(const std::vector<Voice>& a_voices, RE::FormID a_sound)
		{
			return std::ranges::any_of(a_voices, [&](const Voice& a_voice) { return a_voice.sound == a_sound; });
		}

		// The sound a placed reference makes by itself while it stands in a loaded cell: a sound marker's
		// descriptor, an activator's looping sound (a forge, a waterfall), a light's sound (a fire).
		RE::BGSSoundDescriptorForm* SoundOf(RE::TESObjectREFR* a_ref)
		{
			auto* base = a_ref->GetBaseObject();
			if (!base) {
				return nullptr;
			}
			switch (base->GetFormType()) {
			case RE::FormType::Sound:
				return base->As<RE::TESSound>()->descriptor;
			case RE::FormType::Activator:
				return base->As<RE::TESObjectACTI>()->soundLoop;
			case RE::FormType::Light:
				return base->As<RE::TESObjectLIGH>()->sound;
			default:
				return nullptr;
			}
		}

		// The room's sources, once, as it becomes live: its acoustic space's loop, the nearest few distinct
		// sounds placed in its cells, and an exterior's weather.
		std::vector<Voice> Sources(const Portal::Active& a_live)
		{
			std::vector<Voice> voices;
			auto*              cell = a_live.cell;
			auto*              linked = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_live.linkedDoor);
			const auto         doorPlace = linked->GetPosition();

			if (cell->IsInteriorCell()) {
				if (auto* extra = cell->extraList.GetByType<RE::ExtraCellAcousticSpace>(); extra && extra->space && extra->space->loopingSound) {
					voices.push_back({ Kind::kAcoustic, extra->space->loopingSound->GetFormID(), 1.0f, 0.0f });
				}
			}

			struct Placed
			{
				RE::FormID sound;
				float      distance;
			};
			std::vector<Placed> placed;
			for (auto* roomCell : Portal::Manager::RemoteCells()) {
				roomCell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
					if (a_ref->IsDisabled() || a_ref->IsDeleted()) {
						return RE::BSContainer::ForEachResult::kContinue;
					}
					if (auto* sound = SoundOf(a_ref)) {
						const float distance = a_ref->GetPosition().GetDistance(doorPlace);
						if (distance <= kMaxSourceRange) {
							placed.push_back({ sound->GetFormID(), distance });
						}
					}
					return RE::BSContainer::ForEachResult::kContinue;
				});
			}
			std::ranges::sort(placed, {}, &Placed::distance);
			for (const auto& entry : placed) {
				if (voices.size() >= kMaxVoices) {
					break;
				}
				if (!Has(voices, entry.sound)) {
					voices.push_back({ Kind::kMarker, entry.sound, CA::RoomFalloff(entry.distance, tuning), entry.distance });
				}
			}

			if (!cell->IsInteriorCell()) {
				auto* weather = RE::Sky::GetSingleton()->currentWeather;
				if (weather) {
					for (auto* weatherSound : weather->sounds) {
						if (voices.size() < kMaxVoices + 2 && weatherSound->type != RE::TESWeather::SoundType::kThunder && !Has(voices, weatherSound->soundFormID)) {
							voices.push_back({ Kind::kWeather, weatherSound->soundFormID, kWeatherShare, 0.0f });
						}
					}
				}
			}
			return voices;
		}

		// Every frame the group has: each source's volume follows the room's level and its own share;
		// one too quiet to hear stops (faded), one that became audible starts (faded in).
		void Drive(Group& a_group, float a_delta)
		{
			const auto position = ToNi(a_group.doorway.center);
			for (auto& voice : a_group.voices) {
				const float want = a_group.fade.level * voice.roomGain;
				voice.age += a_delta;
				if (voice.handle.IsValid()) {
					if (want < kStopBelow) {
						StopVoice(voice, kStopFadeMs);
					} else if (voice.age > kStartSettleSeconds && !voice.handle.IsPlaying()) {
						voice.handle = RE::BSSoundHandle();
						voice.restart = kRestartSeconds;
					} else {
						if (std::abs(want - voice.volume) > kVolumeStep) {
							voice.handle.SetVolume(want);
							voice.volume = want;
						}
						if (position.GetDistance(voice.position) > kMoveStep) {
							voice.handle.SetPosition(position);
							voice.position = position;
						}
					}
				} else if (want > kStartAbove) {
					voice.restart -= a_delta;
					if (voice.restart <= 0.0f && !StartVoice(voice, want, position)) {
						voice.restart = kRestartSeconds;
					}
				}
			}
		}
	}

	void Install()
	{
		using Plugin::Settings::Id;
		enabled = Plugin::Settings::Bool(Id::kDoorwaySound);
		volume = Plugin::Settings::Int(Id::kDoorwaySoundVolume);
	}

	void Update(float a_delta, const std::optional<Portal::Active>& a_live, float a_open)
	{
		if (!enabled) {
			Stop();
			return;
		}
		float target = 0.0f;
		if (a_live && a_open > tuning.minOpen) {
			if (const auto mapping = Portal::Doors::TryCompute(*a_live)) {
				const CA::Doorway doorway{ ToCore(mapping->sourceCenter), ToCore(mapping->inward), mapping->halfWidth, mapping->halfHeight };
				const auto        heard = CA::ThroughDoorway(ToCore(ListenerPosition()), doorway, a_open, tuning);
				if (group && (group->cell != a_live->cell->GetFormID() || group->generation != a_live->generation)) {
					StopGroup(kStopFadeMs);
				}
				// The room's sources are collected only once something of it can be heard.
				if (!group && heard.gain > 0.0f) {
					group.emplace();
					group->cell = a_live->cell->GetFormID();
					group->generation = a_live->generation;
					group->door = a_live->sourceDoor;
					group->voices = Sources(*a_live);
				}
				if (group) {
					group->doorway = doorway;
					group->open = a_open;
					group->heard = heard;
				}
				target = heard.gain * static_cast<float>(volume) * 0.01f;
			}
		}
		if (!group) {
			return;
		}
		group->target = target;
		group->fade.Step(target, a_delta, tuning);
		Drive(*group, a_delta);
		if (group->fade.level <= 0.0f && target <= 0.0f) {
			StopGroup(kQuickFadeMs);
		}
	}

	void Stop()
	{
		StopGroup(kStopFadeMs);
	}

	bool Enabled()
	{
		return enabled;
	}

	void SetEnabled(bool a_enabled)
	{
		enabled = a_enabled;
		if (!enabled) {
			Stop();
		}
	}

	int Volume()
	{
		return volume;
	}

	void SetVolume(int a_percent)
	{
		volume = std::clamp(a_percent, 0, 100);
	}

	json Status()
	{
		json out = { { "enabled", enabled }, { "volume", volume }, { "playing", nullptr } };
		if (!group) {
			return out;
		}
		json voices = json::array();
		for (const auto& voice : group->voices) {
			auto* form = RE::TESForm::LookupByID(voice.sound);
			voices.push_back({ { "kind", KindName(voice.kind) }, { "sound", std::format("{:08X}", voice.sound) }, { "name", form && form->GetFormEditorID() ? form->GetFormEditorID() : "" },
				{ "roomDistance", voice.roomDistance }, { "roomGain", voice.roomGain }, { "volume", voice.volume }, { "failedStarts", voice.failedStarts }, { "soundId", voice.handle.IsValid() ? static_cast<std::int64_t>(voice.handle.soundID) : -1 },
				{ "playing", voice.handle.IsValid() && voice.handle.IsPlaying() } });
		}
		out["playing"] = {
			{ "door", std::format("{:08X}", group->door) },
			{ "cell", std::format("{:08X}", group->cell) },
			{ "doorway", { group->doorway.center.x, group->doorway.center.y, group->doorway.center.z } },
			{ "open", group->open },
			{ "distance", group->heard.distance },
			{ "cosine", group->heard.cosine },
			{ "muffle", group->heard.muffle },
			{ "gain", group->heard.gain },
			{ "target", group->target },
			{ "level", group->fade.level },
			{ "voices", voices },
		};
		return out;
	}
}
