#include "Core/Pictures/Budget.h"

#include <algorithm>
#include <numeric>

namespace Core::Pictures::Budget
{
	std::vector<std::size_t> Evict(const std::vector<File>& a_files, std::uint64_t a_budgetBytes)
	{
		std::uint64_t total = 0;
		for (const auto& file : a_files) {
			total += file.bytes;
		}
		std::vector<std::size_t> order(a_files.size());
		std::iota(order.begin(), order.end(), std::size_t{ 0 });
		std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) { return a_files[a].lastUsed < a_files[b].lastUsed; });
		std::vector<std::size_t> out;
		for (const auto index : order) {
			if (total <= a_budgetBytes) {
				break;
			}
			total -= a_files[index].bytes;
			out.push_back(index);
		}
		return out;
	}
}
