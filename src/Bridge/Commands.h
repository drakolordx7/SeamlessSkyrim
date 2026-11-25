#pragma once

namespace Bridge
{
	struct Command
	{
		json (*handler)(const json& a_args);
		// Almost everything touches engine state that is only safe on the main thread. Commands
		// that wait for something else (e.g. a frame to be presented) opt out so they don't block it.
		bool mainThread = true;
	};

	using CommandTable = std::unordered_map<std::string, Command>;

	// The commands by theme: driving the game, probing engine state, and the portal's tooling.
	void AddGameCommands(CommandTable& a_commands);
	void AddProbeCommands(CommandTable& a_commands);
	void AddPortalCommands(CommandTable& a_commands);

	const CommandTable& GetCommands();
}
