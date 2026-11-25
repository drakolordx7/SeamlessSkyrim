#pragma once

// Raw access for agent-driven reverse engineering through the bridge: the Address Library's
// id <-> offset table and reads of the game's own memory. Disassembly happens in the MCP server.
namespace ReverseEngineering
{
	json ExportIDs(const json& a_args);
	json ReadMemory(const json& a_args);
	// Calls an engine function by AE Address Library id with up to 4 integer/pointer arguments.
	// Experiments only: a wrong signature crashes the game.
	json CallFunction(const json& a_args);
}
