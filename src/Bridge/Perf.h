#pragma once

// Where a frame's time goes (bridge `perf`, docs/BUILD.md): main-thread CPU scopes, and GPU sections
// timed with timestamp queries and counted with pipeline statistics (pixel shader invocations),
// read back a few frames later. Off until the bridge starts it: the queries cost a little every
// frame and only an agent reads them.
namespace Perf
{
	[[nodiscard]] bool On();

	void AddCpu(const char* a_name, double a_ms);
	void AddCount(const char* a_name, double a_value);

	// What Scope does, for the telemetry sink (Diag::Scope): a start stamp, 0 while off, and the end.
	[[nodiscard]] std::int64_t ScopeBegin();
	void                       ScopeEnd(const char* a_name, std::int64_t a_start);

	class Scope
	{
	public:
		explicit Scope(const char* a_name);
		~Scope();
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		const char*  name;
		std::int64_t start;
	};

	// Main thread, on the engine's immediate context. A name is timed once per frame.
	void GpuBegin(const char* a_name);
	void GpuEnd(const char* a_name);

	// At Present: closes the frame.
	void FrameEnd();

	// Bridge thread: a_on starts / stops; the report covers the frames since the last reset.
	json Command(const json& a_args);
}
