#pragma once

// The composition root: what the plugin consists of and the order its parts are set up in.
namespace Plugin
{
	// Once, at kDataLoaded, when the event sources, the data handler and the renderer's swap chain
	// all exist: loads the settings, then installs every module and the engine hooks they need.
	void Install();

	// Portals were on in the ini at startup, or this is an agent session: the modules' engine hooks
	// are in. They can't be put in later when portals are switched on: the physics step's call site
	// runs on job threads, and code can't be patched safely under them.
	[[nodiscard]] bool HooksInstalled();
}
