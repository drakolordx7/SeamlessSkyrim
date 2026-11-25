#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// The pictures on disk are kept within a size budget (Settings): when the folder is over it, the
// ones used longest ago go first.
namespace Core::Pictures::Budget
{
	struct File
	{
		std::uint64_t bytes = 0;
		std::int64_t  lastUsed = 0;  // any clock, larger is later
	};

	// Indices into a_files of the ones to delete so that the rest fit a_budgetBytes: oldest first,
	// stopping once they do. None when they already fit.
	[[nodiscard]] std::vector<std::size_t> Evict(const std::vector<File>& a_files, std::uint64_t a_budgetBytes);
}
