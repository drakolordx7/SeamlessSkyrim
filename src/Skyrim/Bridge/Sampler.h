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

	// Hitch watcher: a thread that, when a frame runs longer than a_thresholdMs, samples the main
	// thread until it ends and appends "H <time> <frame ms>" with its "S <ms> <stack>" lines to
	// a_file; "M <time> <text>" lines are the marks of WatchMark. Costs a wake-up per millisecond.
	// False when already watching or before the main thread is known.
	bool WatchStart(const std::filesystem::path& a_file, double a_thresholdMs);
	// Main thread, once per frame (at Present).
	void WatchTick();
	// Any thread: a line in the watcher's file saying where the test is (a crawl's stage).
	void WatchMark(const std::string& a_text);
}
