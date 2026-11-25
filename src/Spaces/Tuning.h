#pragma once

#include "Plugin/Settings.h"

namespace Spaces
{
	// What the bridge (and the settings at startup) can change about how a remote space is kept.
	struct Tuning
	{
		// false: a room's world is stepped on the main thread at the end of the frame instead of
		// in the engine's step job (RemoteCell::StepAtEndOfFrame), for setups where a second world
		// in the step job deadlocks. A street's is stepped there either way.
		bool stepInJob = true;
		// The street seen from indoors: its people raised to the high process and its world stepped
		// (end of frame only). Unstepped, they hung in a falling pose and moved in a jump every few
		// seconds. ([Portals] bStepExteriorFromInside; the long frames after walking out, 100-200 ms
		// about every other time, happen with and without it: measured alternating, 8 round trips.)
		bool stepExterior = true;
		// [Portals] bHandOverStreet (see RemoteCell::Release).
		bool handOverStreet = true;
		// Wait before a just-left exterior is drawn behind the door.
		float quietSecondsLeftCell = 0.3f;
	};

	inline Tuning tuning;

	// Startup, once Settings are loaded and before the hooks that read it are installed.
	// InstallGridHook may switch handOverStreet off again afterwards.
	inline void LoadTuning()
	{
		using Plugin::Settings::Id;
		tuning.stepInJob = Plugin::Settings::Int(Id::kStepRemoteHavok) == 1;
		tuning.stepExterior = Plugin::Settings::Bool(Id::kStepExteriorFromInside);
		tuning.handOverStreet = Plugin::Settings::Bool(Id::kHandOverStreet);
	}
}
