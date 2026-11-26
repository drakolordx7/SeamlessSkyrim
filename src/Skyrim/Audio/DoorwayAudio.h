#pragma once

#include "Skyrim/Portal/Active.h"

// The live room heard through its open doorway: standing outside an open tavern door, its ambience,
// fires and other sound sources come from the doorway, quieter and duller with distance and angle;
// from inside, the street's weather and ambience come through the open door the same way. Only the
// room the scheduler holds live, only while its door is open; a door that shuts, or a room let go,
// fades out (never cut). Nothing is played for a space that is shut, far or merely kept loaded.
//
// How: the room's sound sources (its acoustic space's loop, the sound markers, activators and lights
// with a sound that stand in it, an exterior's weather) are collected once when the room becomes
// live and each is played as an ordinary 3D sound from the doorway's centre, as loud as the doorway
// lets it through (Core/Audio/Doorway.h) and the source's own distance from the doorway inside the
// room. The engine's own ambience is the player's space's and is not touched. See
// .weaver/scratch/sound-notes.md for what is assumed and what to check in game.
namespace Audio::Doorway
{
	// At kDataLoaded, after the settings.
	void Install();

	// Main thread, every frame at the end of the manager's update. a_live: the doorway of the space
	// the scheduler holds live and the manager has ready (none otherwise); a_open how far its door
	// stands open, 0 shut .. 1.
	void Update(float a_delta, const std::optional<Portal::Active>& a_live, float a_open);
	// The sounds are let go (the space was released, portals off, a load): a short fade, then none.
	void Stop();

	[[nodiscard]] bool Enabled();
	void               SetEnabled(bool a_enabled);
	// Percent, 0..100.
	[[nodiscard]] int  Volume();
	void               SetVolume(int a_percent);

	// The bridge's portal_status "audio": what plays through which doorway.
	[[nodiscard]] json Status();
}
