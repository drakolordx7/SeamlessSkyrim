#pragma once

// A small sampling profiler for the main thread: another thread suspends it about once per
// millisecond, records its call stack and lets it run on. Written for finding out where a
// stutter goes (no ETW / admin rights needed); the stacks are written as module+offset and
// resolved to Address Library IDs offline (tools/skyrim-mcp).
namespace Sampler
{
	// Main thread, any time before Start (every frame is fine).
	void SetMainThread();

	// Any thread: samples for a_seconds, then writes the file. Sample times are milliseconds since
	// this call. False, and nothing sampled, while a run is in progress or before the main thread
	// is known (the first player update).
	bool Start(const std::filesystem::path& a_file, double a_seconds);

	[[nodiscard]] bool Running();
}
