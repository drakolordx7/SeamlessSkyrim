#include "Core/Pictures/CacheRules.h"

#include <format>

namespace Core::Pictures
{
	namespace
	{
		constexpr std::array<const char*, kParts> kPartNames{ "night", "dawn", "day", "dusk" };
	}

	std::string DoorKey(std::string_view a_plugin, std::uint32_t a_formId, bool a_lightPlugin)
	{
		return std::format("{}|{:06X}", a_plugin, a_lightPlugin ? (a_formId & 0xFFF) : (a_formId & 0xFFFFFF));
	}

	std::string DoorKeyWithoutPlugin(std::uint32_t a_formId)
	{
		return std::format("{:08X}", a_formId);
	}

	int PartOfDay(float a_hour)
	{
		return a_hour < 5.0f || a_hour >= 21.0f ? 0 : a_hour < 8.0f ? 1 : a_hour < 18.0f ? 2 : 3;
	}

	std::array<int, kParts> SearchOrder(int a_part)
	{
		return { a_part, (a_part + 1) % kParts, (a_part + 3) % kParts, (a_part + 2) % kParts };
	}

	std::string KeyOf(std::string_view a_doorKey, bool a_outdoors, int a_part)
	{
		return std::string(a_doorKey) + "#" + (a_outdoors ? kPartNames[a_part] : "inside");
	}

	std::string FileStem(std::string_view a_key)
	{
		std::string name(a_key);
		for (auto& c : name) {
			if (c == '|' || c == '#' || c == '\\' || c == '/' || c == ':') {
				c = '_';
			}
		}
		return name;
	}

	bool HeaderValid(const std::uint32_t (&a_header)[4], std::uint32_t a_widest, std::uint32_t a_highest)
	{
		return a_header[0] == kMagic && a_header[1] >= 1 && a_header[1] <= kVersion && a_header[2] >= 1 && a_header[2] <= a_widest && a_header[3] >= 1 &&
		       a_header[3] <= a_highest;
	}

	float GeometricMean(const std::vector<float>& a_values)
	{
		if (a_values.empty()) {
			return 0.0f;
		}
		double sum = 0.0;
		for (const float value : a_values) {
			sum += std::log(value);
		}
		return static_cast<float>(std::exp(sum / static_cast<double>(a_values.size())));
	}
}
