#include "Skyrim/Audio/DoorwayFilter.h"

#include "Core/Audio/Doorway.h"

#include <atomic>

namespace Audio::Filter
{
	namespace
	{
		constexpr std::uint32_t kNone = 0xFFFFFFFF;
		// The most sounds at once: the doorway's voices (DoorwayAudio's kMaxVoices plus the weather).
		constexpr std::size_t kSlots = 12;
		// A cutoff that moved less than this fraction is not sent to XAudio2 again.
		constexpr float kResend = 0.02f;

		// Written by the main thread, read by the audio thread. A slot is claimed by its id; the cutoff is
		// 0 for "pass everything".
		struct Slot
		{
			std::atomic<std::uint32_t> id{ kNone };
			std::atomic<float>         cutoff{ 0.0f };
			// What the audio thread last set on the voice's filter, and on which sound object.
			std::atomic<float>         applied{ -1.0f };
			std::atomic<void*>         appliedTo{ nullptr };
			// What the voice's filter reads back after that, for the bridge.
			std::atomic<int>           type{ -1 };
			std::atomic<float>         frequency{ 0.0f };
			std::atomic<std::uint32_t> rate{ 0 };
		};

		std::array<Slot, kSlots> slots;

		// Counters, written by the audio thread.
		std::atomic<std::uint32_t> updateCalls{ 0 };
		std::atomic<std::uint32_t> voicesCreated{ 0 };
		std::atomic<std::uint32_t> voicesMadeFilterable{ 0 };
		std::atomic<std::uint32_t> filterSets{ 0 };
		std::atomic<std::uint32_t> unfilterable{ 0 };
		std::atomic<std::uint32_t> filterResets{ 0 };
		std::atomic<std::uint32_t> updateThread{ 0 };
		std::atomic<std::uint32_t> lastCreationFlags{ 0 };
		std::atomic<float>         lastFrequency{ -1.0f };
		bool                       installed = false;
		std::uint32_t              mainThread = 0;

		void SetFilter(RE::IXAudio2Voice* a_voice, RE::XAUDIO2_FILTER_TYPE a_type, float a_frequency)
		{
			const RE::XAUDIO2_FILTER_PARAMETERS parameters{ a_type, a_frequency, 1.0f };
			a_voice->SetFilterParameters(&parameters);
		}

		// Every source voice the game makes can be filtered, and is made to sound as it did.
		struct CreateSourceVoiceHook
		{
			static std::int32_t Thunk(RE::IXAudio2* a_this, RE::IXAudio2SourceVoice** a_voice, const RE::WAVEFORMATEX* a_format, std::uint32_t a_flags, float a_maxRatio,
				RE::IXAudio2VoiceCallback* a_callback, const RE::XAUDIO2_VOICE_SENDS* a_sends, const RE::XAUDIO2_EFFECT_CHAIN* a_chain)
			{
				const bool added = !(a_flags & RE::XAUDIO2_VOICE_USEFILTER);
				const auto result = func(a_this, a_voice, a_format, a_flags | RE::XAUDIO2_VOICE_USEFILTER, a_maxRatio, a_callback, a_sends, a_chain);
				if (result >= 0 && *a_voice) {
					++voicesCreated;
					lastCreationFlags = a_flags;
					if (added) {
						// The flag's default filter is a low-pass at a sixth of the sample rate; a high-pass at 0 passes all.
						SetFilter(*a_voice, RE::XAUDIO2_FILTER_TYPES::HighPassFilter, 0.0f);
						++voicesMadeFilterable;
					}
				}
				return result;
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};

		// Sound objects whose voice has our low-pass set; touched on the audio thread only.
		std::vector<RE::BSXAudio2GameSound*> filtered;

		// Whether a voice was made filterable (by us, or by the game).
		bool Filterable(RE::IXAudio2SourceVoice* a_voice, std::uint32_t& a_rate)
		{
			RE::XAUDIO2_VOICE_DETAILS details{};
			a_voice->GetVoiceDetails(&details);
			a_rate = details.InputSampleRate;
			return (details.CreationFlags & RE::XAUDIO2_VOICE_USEFILTER) != 0;
		}

		// The audio thread, once per sound per tick.
		struct UpdateHook
		{
			static bool Thunk(RE::BSXAudio2GameSound* a_this)
			{
				const bool result = func(a_this);
				++updateCalls;
				updateThread = ::GetCurrentThreadId();
				auto* voice = a_this->sourceVoice;
				if (!voice) {
					return result;
				}
				bool ours = false;
				for (auto& slot : slots) {
					const auto id = slot.id.load();
					if (id == kNone) {
						continue;
					}
					auto&      active = RE::BSAudioManager::GetSingleton()->activeSounds;
					const auto it = active.find(id);
					if (it == active.end() || it->second != a_this) {
						continue;
					}
					ours = true;
					const float cutoff = slot.cutoff.load();
					std::uint32_t rate = 0;
					if (!Filterable(voice, rate)) {
						// A voice made before the hook was in place.
						++unfilterable;
						break;
					}
					if (slot.appliedTo.load() != a_this || std::abs(slot.applied.load() - cutoff) > kResend * std::max(cutoff, 1.0f)) {
						slot.appliedTo = a_this;
						slot.applied = cutoff;
						// XAudio2's filter, in the sound's own sample rate (the voice's input rate), reaches up to a sixth
						// of it and no further: a cutoff above that is the sound as it is, not a filter at its limit.
						const float frequency = Core::Audio::FilterFrequency(cutoff, static_cast<float>(rate));
						if (frequency < 1.0f) {
							SetFilter(voice, RE::XAUDIO2_FILTER_TYPES::LowPassFilter, frequency);
						} else {
							SetFilter(voice, RE::XAUDIO2_FILTER_TYPES::HighPassFilter, 0.0f);
						}
						lastFrequency = frequency;
						++filterSets;
						if (std::ranges::find(filtered, a_this) == filtered.end()) {
							filtered.push_back(a_this);
						}
					}
					// What the voice reads back; XAudio2 applies a change at its next pass, so it shows a tick later.
					RE::XAUDIO2_FILTER_PARAMETERS now{};
					voice->GetFilterParameters(&now);
					slot.type = static_cast<int>(now.Type);
					slot.frequency = now.Frequency;
					slot.rate = rate;
					break;
				}
				if (!ours) {
					// A sound that was ours and isn't any more (stopped, or its object reused): as it was made.
					if (const auto it = std::ranges::find(filtered, a_this); it != filtered.end()) {
						filtered.erase(it);
						SetFilter(voice, RE::XAUDIO2_FILTER_TYPES::HighPassFilter, 0.0f);
						++filterResets;
					}
				}
				return result;
			}
			static inline REL::Relocation<decltype(Thunk)> func;
		};
	}

	bool Install()
	{
		auto* audio = RE::BSXAudio2GameSound::GetAudioImplementation();
		// No audio device: the game runs without sound and has no XAudio2.
		if (!audio || !audio->XAudio) {
			logger::info("doorway sound: no audio device, no low-pass");
			return false;
		}
		REL::Relocation<std::uintptr_t> xaudio{ *reinterpret_cast<std::uintptr_t*>(audio->XAudio) };
		CreateSourceVoiceHook::func = xaudio.write_vfunc(0x8, CreateSourceVoiceHook::Thunk);
		REL::Relocation<std::uintptr_t> sound{ RE::VTABLE_BSXAudio2GameSound[0] };
		UpdateHook::func = sound.write_vfunc(0xE, UpdateHook::Thunk);
		installed = true;
		mainThread = ::GetCurrentThreadId();
		logger::info("doorway sound: low-pass hooked (XAudio2 CreateSourceVoice, BSXAudio2GameSound::Update)");
		return true;
	}

	void Set(std::uint32_t a_soundId, float a_cutoffHz)
	{
		if (!installed) {
			return;
		}
		Slot* free = nullptr;
		for (auto& slot : slots) {
			const auto id = slot.id.load();
			if (id == a_soundId) {
				slot.cutoff = a_cutoffHz;
				return;
			}
			if (id == kNone && !free) {
				free = &slot;
			}
		}
		SEAMLESS_ASSERT(free);
		free->cutoff = a_cutoffHz;
		free->applied = -1.0f;
		free->appliedTo = nullptr;
		free->type = -1;
		free->id = a_soundId;
	}

	std::optional<State> StateOf(std::uint32_t a_soundId)
	{
		for (auto& slot : slots) {
			if (slot.id.load() == a_soundId && slot.type.load() >= 0) {
				return State{ slot.type.load(), slot.frequency.load(), slot.rate.load() };
			}
		}
		return std::nullopt;
	}

	void Clear(std::uint32_t a_soundId)
	{
		for (auto& slot : slots) {
			if (slot.id.load() == a_soundId) {
				slot.id = kNone;
			}
		}
	}

	json Status()
	{
		return { { "installed", installed }, { "updateCalls", updateCalls.load() }, { "voicesCreated", voicesCreated.load() }, { "voicesMadeFilterable", voicesMadeFilterable.load() },
			{ "lastCreationFlagsOfTheGame", lastCreationFlags.load() }, { "filterSets", filterSets.load() }, { "filterResets", filterResets.load() }, { "updateThread", updateThread.load() }, { "mainThread", mainThread }, { "unfilterableVoices", unfilterable.load() }, { "lastFrequency", lastFrequency.load() } };
	}
}
