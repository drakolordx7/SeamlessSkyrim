#pragma once

#include "Core/Graph/SpaceGraph.h"

// The places of the loaded game and the doorways between them (Core::Graph::SpaceGraph), from the
// placed teleport doors the engine has in memory: the same records the door census lists
// (tools/census), gathered at run time so that any load order, DLC and mod is covered without a file.
//
// What is in memory grows. Once, at data load, every reference form in the engine's form map and
// every worldspace's persistent cell gives its doors (at that moment mostly persistent ones: a cell's
// other references are read when the cell is). After that each cell the player's surroundings load
// is looked over once, and the graph is built again with the doors it brought, so the doors around
// the player are always in it and the graph reaches as far as the game has been played.
//
// Indices (SpaceIx, DoorIx) stay the same when the graph is built again: records are only appended.
// Doorway indices don't (a one-way door can turn into half of a pair), so whoever keeps state by
// doorway follows it through SetOnRebuilt.
namespace Graph
{
	// Once, at kDataLoaded: the scan, the graph, one log line of counts.
	void Install();

	// Main thread, every frame: looks over cells that came into memory around a_here, a few times a second.
	void Discover(RE::TESObjectCELL* a_here, float a_delta);

	// The graph is built anew when records were added (called by SpaceOf's callers via Flush, and by
	// Discover); a_old is still alive in the callback. Set once.
	using RebuiltCallback = void (*)(const Core::Graph::SpaceGraph& a_old, const Core::Graph::SpaceGraph& a_new);
	void SetOnRebuilt(RebuiltCallback a_callback);
	// Builds the graph now if records were added since it was built.
	void Flush();

	[[nodiscard]] const Core::Graph::SpaceGraph& Get();

	// The space of a cell: its worldspace outdoors, the cell indoors. Added if unknown (the graph is
	// built again at the next Flush).
	[[nodiscard]] Core::Graph::SpaceIx SpaceOf(RE::TESObjectCELL* a_cell);
	// The same without adding: kNoSpace for a space the graph doesn't hold (yet).
	[[nodiscard]] Core::Graph::SpaceIx FindSpaceOf(RE::TESObjectCELL* a_cell);
	[[nodiscard]] Core::Graph::DoorIx  DoorOf(RE::FormID a_door);
	// The engine form of a space (its cell, or its worldspace) and of a door.
	[[nodiscard]] RE::FormID SpaceForm(Core::Graph::SpaceIx a_space);
	[[nodiscard]] RE::FormID DoorForm(Core::Graph::DoorIx a_door);
	[[nodiscard]] bool       IsWorld(Core::Graph::SpaceIx a_space);

	// For the bridge: the counts of what the graph holds and how it was found.
	[[nodiscard]] json Summary();
}
