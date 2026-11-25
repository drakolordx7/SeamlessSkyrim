#pragma once

#include <cstdint>

// What names a thing in the game, without a game's types. A FormID's top byte is the load-order
// slot of the plugin that made it, so the same number means different things in different load
// orders; anything stored across sessions keys by plugin name plus the low bits instead.
namespace Core
{
	enum class Game : std::uint8_t
	{
		kSkyrim,
		kFallout4,
		kStarfield,
	};

	struct FormRef
	{
		std::uint32_t formId = 0;

		[[nodiscard]] bool operator==(const FormRef&) const = default;
		[[nodiscard]] explicit operator bool() const { return formId != 0; }
	};

	// A cell or worldspace: a place that is loaded as a whole.
	struct SpaceRef
	{
		Game    game = Game::kSkyrim;
		FormRef form;

		[[nodiscard]] bool operator==(const SpaceRef&) const = default;
	};

	// One load door.
	struct DoorRef
	{
		Game    game = Game::kSkyrim;
		FormRef form;

		[[nodiscard]] bool operator==(const DoorRef&) const = default;
	};

	// Two ends of a teleport: `source` is the door in the player's space.
	struct DoorPairRef
	{
		DoorRef source;
		DoorRef linked;

		[[nodiscard]] bool operator==(const DoorPairRef&) const = default;
	};
}
