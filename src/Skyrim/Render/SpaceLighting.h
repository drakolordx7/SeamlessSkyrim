#pragma once

#include "Core/Lighting/Ambient.h"
#include "Skyrim/Render/Types.h"

// The light of a space seen from another: its sun, directional ambient, fog and sky, worked out
// from the engine's own data (the cell's lighting, the weather, what the world was lit with last)
// and handed on as typed structs (Render/Wire.h). Used by the portal pass, by the manager for the
// rooms it merges into the world, and by the doorway pictures.
namespace Render::SpaceLighting
{
	// Lighting shaders read one global directional ambient, set each frame for the current space
	// by Sky::SetDirectionalAmbientColors (ID 105643, colors[3][2] + specular tint + fresnel). The
	// portal's space needs its own, so it is set for the portal pass and the world's put back.
	using AmbientState = Core::Lighting::Ambient;

	// Everything ID 105643 writes, as memory: a copy of its inputs (ID 414646, 0x48 bytes) and what
	// it builds from them (from ID 390984: the ambient transform, then tint and fresnel, 17 floats).
	// The world's ambient is put back from this, not by calling the function again with its saved
	// inputs: those are the colours after Community Shaders' hook adjusted them (Effects11), and
	// passed in again they were adjusted twice. The outdoor ambient changed, every frame a portal
	// was drawn from inside and every second a room was merged in.
	struct AmbientSnapshot
	{
		std::array<std::byte, 0x48> inputs;
		std::array<std::byte, 0x44> built;
	};

	[[nodiscard]] AmbientSnapshot SaveAmbient();
	void                          RestoreAmbient(const AmbientSnapshot& a_snapshot);

	// The directional ambient transform the lighting shaders read (what ID 105643 builds), in the engine's own memory.
	[[nodiscard]] RE::NiTransform& EngineAmbientTransform();

	struct SkyColors
	{
		RE::NiColor upper;
		RE::NiColor horizon;
		RE::NiColor lower;
	};

	// Outdoors seen from inside: the engine's sky is in interior mode then, so the exterior is lit
	// from the weather's data for the current hour (sun colour, directional ambient; the sky
	// colours for the background) instead of from Sky's per-frame state.
	struct Outdoors
	{
		RE::NiColor  sun;
		AmbientState ambient;
		SkyColors    sky;
		// Distance fog: near and far distance, power, the most it covers, and its two colours.
		float        fogNear = 0.0f;
		float        fogFar = 0.0f;
		float        fogPower = 1.0f;
		float        fogMax = 1.0f;
		RE::NiColor  fogNearColor;
		RE::NiColor  fogFarColor;
	};

	// Blended the way the sky blends them outdoors (Sky::UpdateColors, ID 26232): between two of the
	// weather's four colour sets by the hour, and from the previous weather during a transition.
	[[nodiscard]] std::optional<Outdoors> WeatherLighting();

	// Render thread, before the world is drawn, while the player is outdoors: the outdoors as the
	// engine itself lit it, kept for the time the player is indoors.
	void CaptureOutdoors();

	// The portal scene's sun and the directional ambient for the remote space; outdoors also the
	// background colour.
	std::optional<SkyColors> ApplyRemoteLighting(RE::ShadowSceneNode* a_scene, RE::TESObjectCELL* a_cell);

	// An interior's or the outdoors' directional light and ambient as a room merged into the world is
	// lit by them, rotated by a_rotation (the space's placement in the world). Main thread.
	[[nodiscard]] std::optional<Render::SpaceLight> MergedLighting(RE::TESObjectCELL* a_cell, const RE::NiMatrix3& a_rotation);

	// The outdoors' distance fog for the street merged into an interior (the interior's fog, a few
	// thousand units deep and dark, turned the distant mountains black). From the weather.
	[[nodiscard]] std::optional<Render::SpaceFog> MergedFog();
	// The same for a room merged into the street: the interior's own fog (its lighting, or its
	// lighting template's where it inherits).
	[[nodiscard]] std::optional<Render::SpaceFog> CellFog(RE::TESObjectCELL* a_cell);

	// Diagnostics: the outdoors' light as the last snapshot holds it and as worked out from the weather.
	json OutdoorLightingProbe();
}

// The direction the sun's light travels in a space.
namespace Render::SunDirections
{
	// The way the light travels in an interior, as the engine lit it while the player was in it
	// (Observe) or else by the formula.
	[[nodiscard]] RE::NiPoint3 OfCell(RE::TESObjectCELL* a_cell);
	// Outdoors.
	[[nodiscard]] RE::NiPoint3 Outdoors();

	// Render thread, before the world is drawn: the formulas against the engine's own light in the
	// space the player is in, once per space, so a mistake in them shows in the log. What it sees in
	// an interior is what OfCell returns for it from then on: the formula was measured 89 degrees
	// off in Arcadia's Cauldron, and the engine's light is the truth. A second after arriving: the
	// engine brings a new space's light in a frame late, and on the frame of arrival the world's sun
	// carries ours (see Frame.cpp).
	void Observe();
}
