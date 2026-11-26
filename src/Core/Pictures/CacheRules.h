#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The rules of the doorway picture cache that don't depend on how a picture is stored: which file
// and key a door's picture lives under, which pictures count as valid, how bright a room is read
// off its texels, and which pictures are let go of.
namespace Core::Pictures
{
	// The file layout and what is drawn into it: alpha is how far behind the doorway each texel's
	// surface is, and the header is followed by where the picture was taken from and the part of the
	// door's plane it covers; taken without the room's people or its front wall, the sky left out of
	// a texel's average where anything else is in it, a gate's from further back. Only this version
	// is read; changing the layout or what a picture holds bumps it, and files of any other are
	// taken again.
	//
	// 6: the image tier at the full size without blur, files carry the load order's hash and the
	// picture's brightness, and the depth image tier's layered files (FileFormat.h) exist.
	//
	// 7: block compressed (BC6H colour with its mips, half-float distance apart: Compress.h), and the
	// header says where the picture came from (a visit or taken ahead) and when.
	//
	// 8: pictures taken with the far-door clip left no colour (flat grey) on a room's walls, floor and
	// ceiling near the doorway (PortalPass); they are taken again.
	inline constexpr std::uint32_t kVersion = 8;
	inline constexpr std::uint32_t kMagic = 0x57445353;  // "SSDW"

	// A picture is taken again at most this often (the room changes; the light outdoors does).
	inline constexpr auto kRetake = std::chrono::seconds(20);
	// Pictures of doors not looked at for this long are let go; they are on disk for the next time.
	inline constexpr auto kKeptUnused = std::chrono::seconds(60);
	inline constexpr auto kLookedAtEvery = std::chrono::seconds(5);

	// "Skyrim.esm|01A6FB": the same door whatever the load order. Without a plugin to name, the bare ID.
	[[nodiscard]] std::string DoorKey(std::string_view a_plugin, std::uint32_t a_formId, bool a_lightPlugin);
	[[nodiscard]] std::string DoorKeyWithoutPlugin(std::uint32_t a_formId);

	// The outdoors is told apart by the part of the day it is seen in, an interior looks the same
	// at any hour.
	inline constexpr int kParts = 4;
	[[nodiscard]] int PartOfDay(float a_hour);
	// a_part first, then the one after, the one before, the one across: another part's picture when
	// this one was never taken (a door first passed at night).
	[[nodiscard]] std::array<int, kParts> SearchOrder(int a_part);
	[[nodiscard]] std::string             KeyOf(std::string_view a_doorKey, bool a_outdoors, int a_part);
	// The key as a file name stem.
	[[nodiscard]] std::string FileStem(std::string_view a_key);

	// The load order as one number: the plugins' names, in order (case aside). Pictures are of what
	// the load order made of a space, so another load order's are not shown or kept: a hash of it is in
	// the file name, and in the file. A different order of the same plugins is another load order.
	[[nodiscard]] std::uint64_t LoadOrderHash(const std::vector<std::string>& a_plugins);
	// A file name: the key's stem, the hash's low 32 bits, and the extension (".ssdw" the image tier's,
	// ".ssdl" the depth image's).
	[[nodiscard]] std::string FileName(std::string_view a_key, std::uint64_t a_loadOrder, bool a_layered);

	// The file: four 32-bit words, magic, version, width, height, then where the picture was taken
	// from and what it covers, then the texels. The widest and highest a picture was ever written
	// at bound what a damaged header can ask for.
	[[nodiscard]] bool HeaderValid(const std::uint32_t (&a_header)[4], std::uint32_t a_widest, std::uint32_t a_highest);

	// How bright a room is, from its texels: the geometric mean of their luminance (a texel with
	// nothing there is not part of the room), as the room's own light scaled it.
	inline constexpr float kNothingThere = 20000.0f;  // alpha: how far behind the doorway
	[[nodiscard]] inline float Luminance(float a_red, float a_green, float a_blue)
	{
		return 0.2126f * a_red + 0.7152f * a_green + 0.0722f * a_blue;
	}

	class BrightnessMean
	{
	public:
		// Below the darkest a texel is counted as: black (an unlit corner) would pull the mean to 0.
		static constexpr float kDarkest = 1e-3f;

		void Add(float a_luminance)
		{
			sum += std::log(std::max(a_luminance, kDarkest));
			++count;
		}
		// 0 when nothing was added.
		[[nodiscard]] float Result(float a_lightScale) const { return count ? static_cast<float>(std::exp(sum / count)) / a_lightScale : 0.0f; }

	private:
		double      sum = 0.0;
		std::size_t count = 0;
	};

	// The geometric mean of the rooms' brightness that are known, none when none is.
	[[nodiscard]] float GeometricMean(const std::vector<float>& a_values);

	// A picture is let go of when it hasn't been used for kKeptUnused and isn't still being read
	// back for its file.
	[[nodiscard]] inline bool ShouldForget(std::chrono::steady_clock::duration a_sinceUsed, bool a_beingWritten)
	{
		return a_sinceUsed >= kKeptUnused && !a_beingWritten;
	}
}
