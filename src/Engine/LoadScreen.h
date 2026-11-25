#pragma once

namespace Engine
{
	// ID 13363 (TES: show/hide the loading screen) returns without showing it while this is set.
	// The engine sets it itself for short teleports inside one worldspace.
	inline bool& SkipLoadScreen()
	{
		static REL::Relocation<bool*> skip{ REL::RelocationID(514187, 400337) };
		return *skip;
	}

	// Set while the loading screen is up (written by ID 13363).
	inline bool LoadScreenUp()
	{
		static REL::Relocation<bool*> up{ REL::RelocationID(514186, 400336) };
		return *up;
	}
}
