#include "Skyrim/Bridge/Sampler.h"

#include <Windows.h>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

namespace
{
	constexpr std::size_t kMaxFrames = 48;
	constexpr std::size_t kMaxSamples = 10000;

	struct Sample
	{
		float          ms;
		std::uint32_t  count;
		std::uintptr_t frames[kMaxFrames];
	};

	std::atomic<DWORD> mainThread = 0;
	std::atomic_bool   running = false;

	// Walks the suspended thread's stack with the x64 unwind tables. Plain function (no C++
	// objects) so it can use SEH: a stack in an odd state must not take the game down.
	std::uint32_t Unwind(CONTEXT* a_context, std::uintptr_t* a_frames)
	{
		std::uint32_t count = 0;
		__try {
			while (count < kMaxFrames && a_context->Rip) {
				a_frames[count++] = a_context->Rip;
				DWORD64 imageBase = 0;
				auto*   function = ::RtlLookupFunctionEntry(a_context->Rip, &imageBase, nullptr);
				if (!function) {
					// Leaf code or a hook trampoline without unwind data: the return address is on top.
					a_context->Rip = *reinterpret_cast<DWORD64*>(a_context->Rsp);
					a_context->Rsp += 8;
					continue;
				}
				PVOID   handlerData = nullptr;
				DWORD64 establisher = 0;
				::RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, a_context->Rip, function, a_context, &handlerData, &establisher, nullptr);
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
		return count;
	}

	std::string Describe(std::uintptr_t a_address, std::unordered_map<std::uintptr_t, std::string>& a_names)
	{
		HMODULE module = nullptr;
		if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(a_address), &module)) {
			return std::format("?+{:X}", a_address);
		}
		const auto base = reinterpret_cast<std::uintptr_t>(module);
		auto       it = a_names.find(base);
		if (it == a_names.end()) {
			wchar_t path[MAX_PATH]{};
			::GetModuleFileNameW(module, path, MAX_PATH);
			it = a_names.emplace(base, std::filesystem::path(path).stem().string()).first;
		}
		return std::format("{}+{:X}", it->second, a_address - base);
	}
}

namespace Sampler
{
	void SetMainThread()
	{
		mainThread = ::GetCurrentThreadId();
	}

	bool Running()
	{
		return running;
	}

	bool Start(const std::filesystem::path& a_file, double a_seconds)
	{
		if (!mainThread || running.exchange(true)) {
			return false;
		}
		std::thread([a_file, a_seconds, start = std::chrono::steady_clock::now()] {
			// Everything is allocated up front: the main thread may be suspended while holding the
			// heap lock, and allocating then would deadlock.
			auto        samples = std::make_unique<Sample[]>(kMaxSamples);
			std::size_t taken = 0;
			HANDLE      thread = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, mainThread);
			::timeBeginPeriod(1);
			while (thread && taken < kMaxSamples) {
				const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
				if (ms > a_seconds * 1000.0) {
					break;
				}
				if (::SuspendThread(thread) != static_cast<DWORD>(-1)) {
					CONTEXT context{};
					context.ContextFlags = CONTEXT_FULL;
					if (::GetThreadContext(thread, &context)) {
						auto& sample = samples[taken++];
						sample.ms = static_cast<float>(ms);
						sample.count = Unwind(&context, sample.frames);
					}
					::ResumeThread(thread);
				}
				::Sleep(1);
			}
			::timeEndPeriod(1);
			if (thread) {
				::CloseHandle(thread);
			}

			std::unordered_map<std::uintptr_t, std::string> names;
			std::ofstream                                   out(a_file);
			for (std::size_t i = 0; i < taken; ++i) {
				out << std::format("{:.2f}\t", samples[i].ms);
				for (std::uint32_t f = 0; f < samples[i].count; ++f) {
					out << (f ? "," : "") << Describe(samples[i].frames[f], names);
				}
				out << '\n';
			}
			logger::info("sampler: {} samples written to {}", taken, a_file.string());
			running = false;
		}).detach();
		return true;
	}

	// ---- hitch watcher ------------------------------------------------------------------------

	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr std::size_t kMaxHitchSamples = 4000;

		std::atomic<std::int64_t> lastTick = 0;  // Clock ticks of the last finished frame
		std::atomic_bool          watching = false;
		std::mutex                markLock;
		std::vector<std::string>  marks;  // pending, written by the watcher

		std::int64_t Now()
		{
			return Clock::now().time_since_epoch().count();
		}

		std::string WallClock()
		{
			return std::format("{:%H:%M:%S}", std::chrono::current_zone()->to_local(std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now())));
		}
	}

	void WatchTick()
	{
		lastTick.store(Now(), std::memory_order_relaxed);
	}

	void WatchMark(const std::string& a_text)
	{
		std::scoped_lock lock(markLock);
		marks.push_back(std::format("M\t{}\t{}", WallClock(), a_text));
	}

	bool WatchStart(const std::filesystem::path& a_file, double a_thresholdMs)
	{
		if (!mainThread || watching.exchange(true)) {
			return false;
		}
		std::thread([a_file, a_thresholdMs] {
			auto        samples = std::make_unique<Sample[]>(kMaxHitchSamples);
			HANDLE      thread = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, mainThread);
			std::ofstream out(a_file, std::ios::app);
			std::unordered_map<std::uintptr_t, std::string> names;
			::timeBeginPeriod(1);
			const auto ms = [](std::int64_t a_from, std::int64_t a_to) { return std::chrono::duration<double, std::milli>(Clock::duration(a_to - a_from)).count(); };
			for (;;) {
				::Sleep(1);
				{
					std::scoped_lock lock(markLock);
					for (const auto& m : marks) {
						out << m << '\n';
					}
					out.flush();
					marks.clear();
				}
				const auto before = lastTick.load(std::memory_order_relaxed);
				if (!before || ms(before, Now()) < a_thresholdMs) {
					continue;
				}
				// The frame in progress has run past the threshold: sample it until it ends.
				const auto wall = WallClock();
				std::size_t taken = 0;
				while (lastTick.load(std::memory_order_relaxed) == before) {
					if (taken < kMaxHitchSamples && ::SuspendThread(thread) != static_cast<DWORD>(-1)) {
						CONTEXT context{};
						context.ContextFlags = CONTEXT_FULL;
						if (::GetThreadContext(thread, &context)) {
							auto& sample = samples[taken++];
							sample.ms = static_cast<float>(ms(before, Now()));
							sample.count = Unwind(&context, sample.frames);
						}
						::ResumeThread(thread);
					}
					::Sleep(1);
				}
				out << std::format("H\t{}\t{:.1f}\n", wall, ms(before, lastTick.load(std::memory_order_relaxed)));
				for (std::size_t i = 0; i < taken; ++i) {
					out << std::format("S\t{:.1f}\t", samples[i].ms);
					for (std::uint32_t f = 0; f < samples[i].count; ++f) {
						out << (f ? "," : "") << Describe(samples[i].frames[f], names);
					}
					out << '\n';
				}
				out.flush();
			}
		}).detach();
		return true;
	}
}
