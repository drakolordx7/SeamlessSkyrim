#pragma once

#include "Skyrim/Render/Wire.h"

// The engine's fog property (what BSShaderManager's light scenes hold, ID 105637 by scene index),
// as the lighting shader's technique setup (ID 107320) and the SSAO composite read it.
namespace Engine::Fog
{
	inline constexpr std::size_t kNearColor = 0x38;  // three floats
	inline constexpr std::size_t kFarColor = 0x44;   // three floats
	inline constexpr std::size_t kNear = 0x50;
	inline constexpr std::size_t kFar = 0x54;
	inline constexpr std::size_t kPower = 0x84;
	inline constexpr std::size_t kMost = 0x88;  // the most it covers
	// Everything this plugin reads or writes, as one block.
	inline constexpr std::size_t kBegin = kNearColor;
	inline constexpr std::size_t kEnd = kMost + sizeof(float);

	// The fog property of a light scene (0: the world's); the active one is the one the scene
	// index (ID 390982) names.
	inline std::byte* Property(int a_scene)
	{
		static REL::Relocation<std::byte* (*)(int)> fogOf{ REL::RelocationID(98984, 105637) };
		return fogOf(a_scene);
	}

	inline std::byte* ActiveProperty()
	{
		static REL::Relocation<std::uint8_t*> sceneIndex{ REL::RelocationID(513241, 390982) };
		return Property(*sceneIndex.get());
	}

	inline float Read(const std::byte* a_property, std::size_t a_offset)
	{
		float value;
		std::memcpy(&value, a_property + a_offset, sizeof(value));
		return value;
	}

	inline void Write(std::byte* a_property, std::size_t a_offset, float a_value)
	{
		std::memcpy(a_property + a_offset, &a_value, sizeof(a_value));
	}

	// a_fog's values into a_property, as the engine would hold them.
	inline void Write(std::byte* a_property, const Render::SpaceFog& a_fog)
	{
		Write(a_property, kNearColor, a_fog.nearColor.red);
		Write(a_property, kNearColor + 4, a_fog.nearColor.green);
		Write(a_property, kNearColor + 8, a_fog.nearColor.blue);
		Write(a_property, kFarColor, a_fog.farColor.red);
		Write(a_property, kFarColor + 4, a_fog.farColor.green);
		Write(a_property, kFarColor + 8, a_fog.farColor.blue);
		Write(a_property, kNear, a_fog.nearDistance);
		Write(a_property, kFar, a_fog.farDistance);
		Write(a_property, kPower, a_fog.power);
		Write(a_property, kMost, a_fog.most);
	}

	// The world's fog laid out as the engine uploads it (FogParam: near / (far - near), 1 / (far -
	// near), power, most it covers; near colour at [4..6], far colour at [8..10]). The world gets it
	// in the SSAO composite, which has run by the time a doorway's picture goes in, so a picture
	// has to be fogged by itself or it stands out of the haze around it, sharp and dark.
	inline std::array<float, 12> World()
	{
		std::array<float, 12> out{};
		const auto*           property = Property(0);
		const float fogNear = Read(property, kNear), fogFar = Read(property, kFar);
		// The engine uploads a constant for "no fog" when both are 0; a most-it-covers of 0 is that here.
		if (fogFar - fogNear > 1e-3f) {
			out[1] = 1.0f / (fogFar - fogNear);
			out[0] = fogNear * out[1];
			out[2] = Read(property, kPower);
			out[3] = Read(property, kMost);
		}
		for (std::size_t i = 0; i < 3; ++i) {
			out[4 + i] = Read(property, kNearColor + 4 * i);
			out[8 + i] = Read(property, kFarColor + 4 * i);
		}
		return out;
	}
}
