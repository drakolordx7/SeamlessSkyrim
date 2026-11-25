#pragma once

// What the portal code reports to the test tooling (perf scopes, counters, the sampling profiler,
// frame capture), without depending on it. Every call is a no-op until the bridge registers its
// implementations (Bridge::RegisterTelemetry, at plugin load).
namespace Diag
{
	struct Sink
	{
		// Diag::Scope's two ends: a start stamp (0: not timing), and the scope's end.
		std::int64_t (*scopeBegin)() = [] { return std::int64_t{ 0 }; };
		void (*scopeEnd)(const char* a_name, std::int64_t a_start) = [](const char*, std::int64_t) {};
		void (*addCount)(const char* a_name, double a_value) = [](const char*, double) {};
		void (*gpuBegin)(const char* a_name) = [](const char*) {};
		void (*gpuEnd)(const char* a_name) = [](const char*) {};
		void (*frameEnd)() = [] {};

		bool (*samplerStart)(const std::filesystem::path& a_file, double a_seconds) = [](const std::filesystem::path&, double) { return false; };
		bool (*samplerRunning)() = [] { return false; };
		void (*samplerSetMainThread)() = [] {};

		void (*captureEventFrames)(std::wstring_view a_event, int a_frames) = [](std::wstring_view, int) {};
		void (*stopWalk)(std::string_view a_reason) = [](std::string_view) {};
		void (*markArrivalFrame)() = [] {};
	};

	inline Sink  sink;
	// Also called at Present, whether or not the bridge is there: the portal scheduler's frame timing.
	inline void (*frameEndHook)() = nullptr;
	// Set by the bridge at load: the game was launched by the MCP server (AgentMode::Enabled).
	inline bool agentMode = false;

	[[nodiscard]] inline bool AgentMode()
	{
		return agentMode;
	}

	// RAII: times the scope in the perf report.
	class Scope
	{
	public:
		explicit Scope(const char* a_name) :
			name(a_name), start(sink.scopeBegin())
		{}
		~Scope()
		{
			sink.scopeEnd(name, start);
		}
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		const char*  name;
		std::int64_t start;
	};

	inline void AddCount(const char* a_name, double a_value) { sink.addCount(a_name, a_value); }
	inline void GpuBegin(const char* a_name) { sink.gpuBegin(a_name); }
	inline void GpuEnd(const char* a_name) { sink.gpuEnd(a_name); }
	// At Present: closes the frame.
	inline void FrameEnd()
	{
		sink.frameEnd();
		if (frameEndHook) {
			frameEndHook();
		}
	}

	inline bool SamplerStart(const std::filesystem::path& a_file, double a_seconds) { return sink.samplerStart(a_file, a_seconds); }
	[[nodiscard]] inline bool SamplerRunning() { return sink.samplerRunning(); }
	inline void SamplerSetMainThread() { sink.samplerSetMainThread(); }

	// Saves the next a_frames frames when an event capture directory is set.
	inline void CaptureEventFrames(std::wstring_view a_event, int a_frames) { sink.captureEventFrames(a_event, a_frames); }
	inline void StopWalk(std::string_view a_reason) { sink.stopWalk(a_reason); }
	// The next frame presented is the first in a new space (marked in the brightness log).
	inline void MarkArrivalFrame() { sink.markArrivalFrame(); }
}
