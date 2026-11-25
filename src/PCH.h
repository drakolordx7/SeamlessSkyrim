#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <future>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std::literals;
using json = nlohmann::json;

namespace logger = SKSE::log;

// Active in every build: the plugin only ever runs as RelWithDebInfo, where <cassert> is compiled
// out. A broken assumption about the engine is logged and then crashes on the spot, so Crash
// Logger records the stack where it went wrong instead of wherever the damage shows up later.
#define SEAMLESS_ASSERT(a_condition)                                                                   \
	do {                                                                                               \
		if (!(a_condition)) [[unlikely]] {                                                             \
			logger::critical("assertion failed: {} ({}:{})", #a_condition, __FILE__, __LINE__);        \
			spdlog::default_logger()->flush();                                                         \
			__debugbreak();                                                                            \
		}                                                                                              \
	} while (false)
