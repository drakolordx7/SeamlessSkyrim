#pragma once

namespace Render::Enb
{
	// Looks for ENB's SDK exports and logs what it finds. Once, at kDataLoaded.
	void Detect();

	// For the bridge's render status.
	[[nodiscard]] json Describe();
}
