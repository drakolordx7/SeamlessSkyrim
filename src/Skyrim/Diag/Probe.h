#pragma once

// Log helpers: a cell as one line, and which cells the engine keeps loaded and simulated.
namespace Probe
{
	// The loaded interiors and the cells with high-process actors, after a load and for the bridge.
	void DumpWorldState(std::string_view a_reason);
	std::string DescribeCell(RE::TESObjectCELL* a_cell);
}
