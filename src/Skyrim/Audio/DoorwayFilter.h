#pragma once

// A low-pass on the sounds heard through a doorway: the same XAudio2 source voice filter the sound
// engine's own voices would have, set from the audio thread.
//
// What the engine gives: nothing. Skyrim's voices are created without XAUDIO2_VOICE_USEFILTER, and
// XAudio2 ignores SetFilterParameters on a voice made without it, so a hook on
// IXAudio2::CreateSourceVoice adds the flag (and sets every such voice to a high-pass at 0 Hz, which
// passes the sound through untouched: XAudio2's default for the flag is a low-pass at a sixth of the
// sample rate, which would dull everything the game plays). A hook on BSXAudio2GameSound::Update,
// which the audio thread calls for each sound, then sets the cutoff of the sounds registered here.
// The voice and the sound-id table are only ever touched on the audio thread, which owns both, so
// nothing races. A cutoff above what the filter reaches (a sixth of the sound's own sample rate, which
// for a 22 kHz loop is 3.7 kHz) leaves the sound as it is. The cutoff itself comes from Core/Audio/Doorway.h (LowPassCutoff).
namespace Audio::Filter
{
	// At kDataLoaded, when the sound engine is up. False (and nothing hooked) without an audio device.
	bool Install();

	// Main thread. The sound with this handle id is heard through a low-pass at this cutoff (Hz).
	void Set(std::uint32_t a_soundId, float a_cutoffHz);
	// Main thread. The sound is no longer ours: its filter goes back to passing everything.
	void Clear(std::uint32_t a_soundId);

	// What the audio thread last set on a sound's voice and the voice then reads back: the filter type
	// (XAudio2's enum: 0 low-pass, 2 high-pass, which at frequency 0 passes everything), its frequency
	// parameter (2 sin(pi f / rate), 1 at most) and the voice's input sample rate. Empty until it did.
	struct State
	{
		int           type;
		float         frequency;
		std::uint32_t rate;
	};
	[[nodiscard]] std::optional<State> StateOf(std::uint32_t a_soundId);

	// What the audio thread did, for the bridge's portal_status.audio.filter.
	[[nodiscard]] json Status();
}
