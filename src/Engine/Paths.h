#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace Paths
{
	// The folder this DLL was loaded from (Data/SKSE/Plugins), where its ini files are installed.
	// The working directory is usually the game's, but that is up to whatever launched the game
	// (a shortcut's "Start in", a mod manager's setting).
	inline const std::filesystem::path& PluginDirectory()
	{
		static const std::filesystem::path directory = [] {
			HMODULE module = nullptr;
			::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&PluginDirectory), &module);
			std::wstring path(MAX_PATH, L'\0');
			for (;;) {
				const auto length = ::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
				SEAMLESS_ASSERT(length != 0);
				if (length < path.size()) {
					path.resize(length);
					break;
				}
				path.resize(path.size() * 2);  // truncated: a long install path
			}
			return std::filesystem::path(path).parent_path();
		}();
		return directory;
	}

	// Plugin file names are ANSI-code-page strings in the engine (TESFile); the ini functions are
	// wide. Converted through the code page, not char by char: that only held for ASCII names.
	inline std::wstring Widen(std::string_view a_text)
	{
		if (a_text.empty()) {
			return {};
		}
		std::wstring wide(::MultiByteToWideChar(CP_ACP, 0, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0), L'\0');
		::MultiByteToWideChar(CP_ACP, 0, a_text.data(), static_cast<int>(a_text.size()), wide.data(), static_cast<int>(wide.size()));
		return wide;
	}

	inline std::string Narrow(std::wstring_view a_text)
	{
		if (a_text.empty()) {
			return {};
		}
		std::string narrow(::WideCharToMultiByte(CP_ACP, 0, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr), '\0');
		::WideCharToMultiByte(CP_ACP, 0, a_text.data(), static_cast<int>(a_text.size()), narrow.data(), static_cast<int>(narrow.size()), nullptr, nullptr);
		return narrow;
	}
}
