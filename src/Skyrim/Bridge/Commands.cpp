#include "Skyrim/Bridge/Commands.h"

namespace Bridge
{
	const CommandTable& GetCommands()
	{
		static const CommandTable commands = [] {
			CommandTable table;
			AddGameCommands(table);
			AddProbeCommands(table);
			AddPortalCommands(table);
			return table;
		}();
		return commands;
	}
}
